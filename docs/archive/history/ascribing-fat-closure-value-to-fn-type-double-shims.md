---
title: Ascribing an already-fat closure value to a `(fn ...)` type double-shims it (BUS/SEGV)
category: Bug Report
description: There is no pure-Turmeric way to invoke a fat closure that is already living in an `:int` carrier slot (e.g. a `list-head` result, or any closure passed around as `:ptr<void>`/`:int`). The natural spelling -- ascribe it to a `(fn ...)` type and pass it into a `^fat` parameter -- mis-compiles: codegen treats the already-fat record pointer as a *bare* function pointer and wraps it in `__tur_fatshim_*`, so the dispatch later calls the closure record's first 8 bytes as code and BUS/SEGV-faults. Surfaced while de-inline-C-ing `httpd-mw-fold` for stdlib-inline-c-deworkaround-plan Phase 2.
---

# Ascribing an already-fat closure value to a `(fn ...)` type double-shims it

## Summary

Severity: **silent miscompile -> runtime crash** (no diagnostic; emits a
SEGV-bound program), plus an **expressiveness hole** (there is no
non-inline-C way to do the safe thing).

When a fat closure is already materialised as a one-word value (a raw
`:int`/`:ptr<void>` carrying a `{ thunk, env }` record pointer -- e.g. an
element pulled out of a cons list with `list-head`, or a handler threaded
around as `:int`), there is **no pure-Turmeric way to invoke it**. The
obvious spelling -- ascribe the value to a function type and let a `^fat`
parameter dispatch it -- compiles without complaint but produces a program
that dereferences the closure-record pointer as a code pointer and crashes.

The only working invocation paths today are hand-written inline-C fat
dispatches. The codebase already carries **three independent copies** of
the same five-line dispatch:

- `stdlib/logic.tur:333` -- `apply-fat`
- `stdlib/parsec.tur:311` -- `apply-fat`
- `stdlib/httpd.tur` -- `httpd-call` (void variant) and now `httpd-mw-apply`
  (int variant), added in Phase 2 precisely because no Turmeric-level
  primitive exists.

## Minimal repro

```turmeric
;; Each list cell holds a *fat closure value* (already-allocated { thunk,
;; env } record), carried as :int. We want to apply it to one int arg.

(defn apply-mw [^fat mw : (fn [int] : ptr<void>) cur : int] : int
  (mw cur))

(defn fold [base : int mws : int] : int
  (if (tnil? mws)
    base
    ;; list-head is :int; ascribe it to the closure's fn type so apply-mw's
    ;; ^fat parameter will dispatch it.
    (apply-mw (:: (list-head mws) (fn [int] : ptr<void>))
              (fold base (list-tail mws)))))
```

`tur emit-c` accepts this with no error. The emitted C for the recursive
arm is (abridged):

```c
int64_t *__t26 = malloc(2 * sizeof(int64_t));
__t26[0] = (int64_t)(intptr_t)__tur_fatshim_void___int64_t;  /* shim thunk  */
__t26[1] = (int64_t)(intptr_t)list_head(mws);                /* "bare" fn   */
void *__t27 = __t26;
__t25 = apply_mw((int64_t)(intptr_t)(__t27), fold(base, list_tail(mws)));
```

## Observed vs. expected

**Observed.** Codegen treats `list-head`'s already-fat value as a *bare*
top-level function pointer and synthesises a `__tur_fatshim_*` adapter
record around it: `{ shim_thunk, original_value }`. At dispatch,
`apply-mw`/`httpd-call` reads `fat[0]` (= `shim_thunk`) and calls it; the
shim then invokes `fat[1]` (the original value) *as a function pointer*.
But `fat[1]` is itself a `{ thunk, env }` **record pointer**, not code, so
the call jumps into heap data -> `SIGBUS`/`SIGSEGV`. This is the exact
hazard the `compose-middleware-of` docstring already warns about
(`stdlib/httpd.tur:3204-3208`), now reproduced silently through an
*ascription* rather than a bare-defn argument.

A 70-deep version of this crashes with a recursive
`httpd_mw_fold`/`apply-mw` stack ending in
`SUMMARY: AddressSanitizer: SEGV ... in httpd_mw_apply` (seen while
building `tests/fixtures/httpd-mw-fold-many` before switching to the
inline-C dispatch).

**Expected.** Either:

1. The ascription `(:: v (fn ...))` on a value already in carrier form
   should be understood as "this is already a fat closure; dispatch it
   directly" (no shim) -- mirroring what `^fat T:int` parameters do today
   when handed an already-fat `:int`; or
2. A first-class primitive (e.g. `apply-fat` promoted out of
   `logic.tur`/`parsec.tur` into a core module, or a `(fat-call f args...)`
   form) that performs the no-shim dispatch, so stdlib does not need a
   fourth hand-rolled inline-C copy.

The current behaviour -- silently auto-shimming and crashing -- is the
worst of the three options.

## Root cause (analysis)

The `^fat` auto-shim is keyed off the *static* type at the call site, not
the *provenance* of the value. When the argument's type is a bare function
type `(fn [...] ...)`, codegen assumes a thin (non-capturing) function
pointer and emits `__tur_fatshim_*` to lift it into the fat ABI. That
assumption is correct for a literal/bare top-level `defn` name, but wrong
for a value that is *already* a fat-closure record reinterpreted into a
function type via `(:: ...)`. There is no carried bit distinguishing
"thin fn pointer needing a shim" from "already-fat record."

Contrast the working path: a `^fat handler : int` parameter (e.g.
`httpd-new`, `stdlib/httpd.tur:729`) takes the value as plain `:int` and
*does not* shim it -- the value flows untouched to an inline-C dispatch.
That is why every real invocation site is forced down to inline-C: the
only non-shimming spelling is "type it `:int` and dispatch by hand."

Relevant pointers:

- Shim synthesis: search `__tur_fatshim` emission in `src/compiler/`
  (the `^fat` argument adaptation path; see `emit_call`/`elab_call`).
- The three inline-C dispatch copies: `stdlib/logic.tur:333`,
  `stdlib/parsec.tur:311`, `stdlib/httpd.tur` (`httpd-call`,
  `httpd-mw-apply`).
- Existing warning of the same crash via a different trigger:
  `stdlib/httpd.tur:3204-3208`.

## Proposed fix directions

1. **Provenance-aware ascription.** Make `(:: v (fn ...))` where `v` is
   already a one-word carrier value lower to a *direct* fat dispatch (no
   `__tur_fatshim`). Equivalently: a `^fat` parameter handed a value whose
   source type is `:int`/`:ptr<void>` (not a literal fn) should dispatch,
   not shim. This is the smallest surface change and removes the silent
   miscompile.
2. **Promote `apply-fat` to a core primitive.** Lift the
   `logic.tur`/`parsec.tur` `apply-fat` (and a void/int-returning family)
   into a core module so stdlib has one blessed, reviewed fat-dispatch
   helper instead of N inline-C reimplementations. Pairs well with design
   principle 2 of
   [stdlib-inline-c-deworkaround-plan](../upcoming/stdlib-inline-c-deworkaround-plan.md)
   ("no bespoke fat-closure casts in stdlib").
3. **Diagnose, at minimum.** If neither is feasible short-term, the
   ascription-into-`^fat` case should be a hard error ("cannot shim an
   already-materialised closure value; use apply-fat") rather than a
   silent crash-bound emit.

## How to validate a fix

- The minimal repro above should either dispatch correctly (option 1) or
  fail to compile with a clear diagnostic (option 3) -- never emit a
  `__tur_fatshim` wrapper around a non-literal value.
- `tests/fixtures/httpd-mw-fold-many` already exercises a 70-element fold
  of fat-closure middlewares through inline-C dispatch; once a Turmeric
  primitive exists, `httpd-mw-apply`/`httpd-call`/`apply-fat` can be
  rewritten on top of it and that fixture must stay green (prints `71`).
- Grep `stdlib/` afterward: a successful fix should let the four inline-C
  dispatch copies collapse to one shared primitive.

## Status

**Fixed** via proposed fix direction 1 (provenance-aware ascription).
`elab_call.c`'s `^fat` argument-adaptation path now strips an erased
`(:: v T)` ascription whose underlying value is a one-word `:int`/
`:ptr<void>` carrier before the bare-fn auto-shim test. The carrier value
flows through the already-fat pass-through branch (the same plumbing that
threads a computed `:int` handle untouched) instead of being wrapped in a
`__tur_fatshim` adapter. The minimal repro now dispatches correctly and
emits no shim; the regression is locked in by
`tests/fixtures/fat-closure-ascription` (folds five fat-closure adders
through an ascribed `^fat` dispatch, prints `115`).

Earlier worked around in
[stdlib-inline-c-deworkaround-plan](../upcoming/stdlib-inline-c-deworkaround-plan.md)
Phase 2 by keeping a single inline-C `httpd-mw-apply` dispatch (the same
ABI glue as the pre-existing `httpd-call`) and doing the *cons walk* in
pure Turmeric. With this hole closed, the one-instruction fat dispatch can
now be expressed by ascribing the `:int` handle to its `(fn ...)` type and
handing it to a `^fat` parameter (the inline-C ABI glue is still needed to
read `{ thunk, env }` and forward args, but the mis-shim hazard is gone).
