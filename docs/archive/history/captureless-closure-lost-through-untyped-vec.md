---
title: Captureless closure stored in an untyped Vec loses its fn-pointer identity -- fat-dispatch at retrieval segfaults
category: Resolved
severity: high
status: fixed in #307 (box captureless closures at TY_TYVAR-parameter boundary in elab_call.c)
description: When a captureless closure (bare fn ptr) is stored in a Vec[A] (e.g. via `vec-of`) and then retrieved with `vec-get`, the elaborator sees the result as TY_INT with no fn-pointer provenance. A subsequent `^fat` binding or `(:: ... :ptr<void>)` cast then treats the code address as a fat-box pointer and dispatches garbage -- segfault. The fix in elab_call.c that strips `(:: captureless :ptr<void>)` ascriptions at ^fat call sites (PR #302 followup, captureless-closure-not-boxed-at-fat-ptr-void-boundary.md) only covers the DIRECT ascription path; once the value passes through vec-push!/vec-get the provenance is irreversibly gone.
---

# Captureless closure lost through an untyped `Vec`

## Resolution (2026-06-07)

**Fixed** via proposed Direction 1 (box at the escape point), applied at the
polymorphic-carrier boundary rather than only the `TY_INT` boundary. In
`src/compiler/elab_call.c`, the `TY_TYVAR`-parameter acceptance hatch now boxes a
captureless closure argument (`TY_FN && !boxed`, arity 1-5) into a uniform
`{ shim, fn }` fat box via `EX_FN_TO_FAT` -- the same box the `^fat` auto-shim
produces. This fires for `vec-push!`'s `val :A` parameter (and any generic
`:A`-typed sink), so a captureless closure stored in a `Vec` is a valid fat box
regardless of capture; `vec-get` + `(:: v :ptr<void>)` + `^fat` dispatch then
reads a real slot-0 thunk instead of a code address.

A *capturing* closure value is `TY_PTR_VOID` (already a fat box) and an
already-boxed `TY_FN` is left untouched, so only bare captureless closures are
shimmed -- no double-boxing. The TY5 HKT-method concern does not arise: generic
`TY_TYVAR` parameters are never directly fat-called as a known-arity fn inside
the generic body, so the uniform fat-box representation is transparent to them.

Regression fixture: `tests/fixtures/vec-captureless-fat-closure-readback/`
(gain-then-invert chain over a `vec-of` mixing a capturing and a captureless SF,
asserts `-1`). The direct-path fixture `fat-captureless-closure-ptr-void` stays
green and `bash tests/run.sh` is green (1530 passed, 0 failed).

## Summary

**Severity: High.** A captureless closure (bare C function pointer) stored in a
`Vec[A]` via `vec-of` or `vec-push!` is cast to `int64_t` at push time. When
retrieved with `vec-get` the elaborator sees a plain `TY_INT` value -- no fn-type
provenance. A downstream `^fat` annotation or `(:: v :ptr<void>)` cast then
fat-dispatches the code address as if it were slot 0 of a `{ thunk, env }` fat
box -> **segfault**.

This is the *vec* sibling of
[captureless-closure-not-boxed-at-fat-ptr-void-boundary.md](captureless-closure-not-boxed-at-fat-ptr-void-boundary.md)
(resolved for the direct-ascription path by the ascription-stripping fix in
`elab_call.c`). The vec path survives that fix because provenance is lost at the
`vec-push!` call site.

## Minimal repro

```turmeric
(load "stdlib/arrow.tur")

(defn invert []
  (fn [^fat sig : (fn [float] float)]
    (fn [t : float] : float (- 0.0 (sig t)))))

(defn gain [g : float]
  (let [gv g]
    (fn [^fat sig : (fn [float] float)]
      (fn [t : float] : float (* gv (sig t))))))

(defn konst [k : float] (let [kv k] (fn [t : float] : float kv)))

(defn main [] : int
  ;; (invert) is captureless -> bare fn ptr stored as int64_t in the vec
  (let [effects                                     (vec-of (gain 2.0) (invert))
        ^fat sf0 : (fn [ptr<void>] #{} ptr<void>)   (:: (vec-get effects 0) :ptr<void>)
        ^fat sf1 : (fn [ptr<void>] #{} ptr<void>)   (:: (vec-get effects 1) :ptr<void>)
        ^fat chain : (fn [ptr<void>] #{} ptr<void>) (>>> sf0 sf1)
        ^fat out : (fn [float] #{} float)           (chain (:: (konst 0.5) :ptr<void>))]
    (println (out 0.0)))  ;; expected: -1.0 (0.5*2=1.0, inverted), actual: Segfault
  0)
```

Control (capturing wrapper works):

```turmeric
(defn invert-fat []
  (let [z 0.0]
    (fn [^fat sig : (fn [float] float)]
      (fn [t : float] : float (- z (sig t))))))

;; replacing (invert) with (invert-fat) above prints -1.0 correctly.
```

Direct `>>>` without vec-of ALSO works (the direct ascription fix covers this):

```turmeric
(let [^fat chain : (fn [ptr<void>] #{} ptr<void>) (>>> (gain 2.0) (invert))
      ^fat out   : (fn [float] #{} float)         (chain (konst 0.5))]
  (println (out 0.0)))  ;; prints -1.0 -- works fine
```

## Observed vs expected

- **Observed**: `Segmentation fault` (exit 139) when `(invert)` is stored in a
  `vec-of` and retrieved via `vec-get` then fat-dispatched.
- **Expected**: `-1.0` (0.5 * 2.0 = 1.0, negated = -1.0).

## Practical impact

`signal/compose.tur`'s `effects-chain` takes a `Vec` of SFs and folds them with
`>>>`. Any captureless Tier-1 shaper (`invert`, `abs-sf`, ...) passed directly
into `effects-chain` segfaults. Capturing SFs (`gain`, `offset`, `low-pass`,
`hard-clip`, ...) work. This blocks mixed capturing/captureless effect chains.

## Root cause

### `vec-push!` stores bare fn ptr as int64_t without boxing

`stdlib/vec.tur`'s `vec-push! [A] [v :int val :A]` stores `val` as `int64_t`
via inline-C (`vec->data[vec->len++] = val;`). The elaborator's argument type
check in `elab_call.c` around line 2396-2403 accepts a `TY_FN` argument where
`TY_INT` is expected (Phase TY5 rule: "allow passing a function reference
(TY_FN) where int64_t is expected") and emits a plain `(int64_t)(intptr_t)` cast
-- no boxing.

For a **capturing** closure the `val` expression has type `TY_PTR_VOID` (already
a fat box pointer), so the stored int64_t is a valid fat-box address.

For a **captureless** closure the `val` expression has type `TY_FN && !boxed`
(bare fn pointer), so the stored int64_t is a **code address**, not a fat-box
pointer.

### `vec-get` returns opaque `TY_INT` -- provenance gone

`vec-get` is typed as returning `:int` (the opaque int64_t carrier for Vec[A]).
The elaborator has no way to know whether the stored value is a fat-box pointer
or a code address. A subsequent `(:: v :ptr<void>)` cast + `^fat` binding
therefore treats the code address as a fat box, reads its first 8 bytes as the
thunk pointer, and calls garbage -> segfault.

### Why the direct-ascription fix doesn't help here

The fix in `elab_call.c` (lines 2836-2855, captureless-closure-not-boxed-at-fat-ptr-void-boundary.md) detects a specific AST pattern:

```
EX_ASCRIBE { inner: TY_FN && !boxed, outer: TY_PTR_VOID }
```

and strips it so the inner `TY_FN` reaches the `EX_FN_TO_FAT` shim. But after
`vec-get`, the expression is a plain `EX_CALL` with type `TY_INT` -- the
`EX_ASCRIBE` wrapper is gone and `!boxed` is lost. The fix never fires.

## Proposed fix directions

1. **Box at `vec-push!` time** (narrowest safe fix): In `elab_call.c`, when a
   `TY_FN && !boxed` argument is passed to a `TY_INT` parameter, insert an
   `EX_FN_TO_FAT` shim (same as the `^fat` path). This requires gating on the
   argument being a captureless fn (not a boxed one), to avoid double-boxing.
   Risk: the Phase TY5 rule explicitly allows this for HKT typeclass method
   signatures that thread fn-typed values as int; those callers rely on the bare
   fn pointer being passed through unchanged. Need to audit whether those sites
   can tolerate fat-boxing, or gate on a narrower condition.

2. **Box captureless closures when they escape as values** (Direction 1 from the
   original bug report, the principled fix): Emit a static or heap fat box
   `{ __fn, /*no env*/ }` for every captureless closure at codegen time, so all
   closure values are uniformly fat boxes regardless of whether they capture. The
   bare-fn shortcut would only apply for directly-called closures (not stored in
   vecs or passed as fn-typed values). Wide blast radius; touches closure codegen.

3. **Typed Vec**: Give `Vec` a typed wrapper (`Vec[SF]`) that knows its element
   type is a fat-dispatch-compatible pointer, and box captureless closures at
   push time in the typed wrapper. Requires HKT or a typed alias.

4. **Workaround (user-side, non-fix)**: Users who need to store captureless SFs
   in a Vec can wrap them in a capturing closure with a dummy capture (e.g.
   `(let [z 0.0] (fn ...))`) before pushing. This forces the fat-box
   representation. The resulting behavior is correct but the idiom is non-obvious.

Fix 1 is the most targeted change; fix 2 is the principled solution (widest
blast radius). Either requires careful auditing of the TY5 rule to avoid
regressing HKT callers.

## How to validate

- Minimal repro above prints `-1.0` and exits 0.
- `signal/compose.tur`'s `effects-chain` with `(vec-of (gain 2.0) (invert))`:
  applies a gain-then-invert chain to a `constant 0.5` signal and returns `-1.0`.
- `tests/fixtures/fat-captureless-closure-ptr-void` stays green (the direct-path
  fix must not be regressed).
- `bash tests/run.sh` stays green.

## Related

- [captureless-closure-not-boxed-at-fat-ptr-void-boundary.md](captureless-closure-not-boxed-at-fat-ptr-void-boundary.md) -- the parent bug; resolved for direct ascription
- Phase 5 of `docs/upcoming/tur-signal-rebuild-plan.md`: captureless SFs in `effects-chain` remain blocked pending this fix
