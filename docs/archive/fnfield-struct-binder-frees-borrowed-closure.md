# A let binder frees the closure of a struct it does not own

**Status: RESOLVED 2026-11-28** -- see [Resolution](#resolution). The text
down to it is the report as filed.

**Severity: high (double free / use-after-free on the default build).** A
`let` local whose type is a by-value struct with a closure field frees that
closure at scope end (local-struct-drop). The rule assumes the local owns the
closure. Three ordinary shapes break that assumption, and each frees a closure
that its real owner frees again or still uses.

Found 2026-11-24 while executing
[cps-byvalue-struct-with-closure-field](cps-byvalue-struct-with-closure-field.md).
All three reproduce on `main` at `1227412b`, with no effects involved: the
first two in direct-emitted code, the third in a function the CPS backend
emits anyway (`mk__cps`), where the entry boundary's reap does the freeing.
The plain build usually prints the right answer; build as
`tests/run-leak-check.sh` does (ASan, `TUR_RUNTIME=source`) to see them.

## Repros

**1. A rebinding of a `^copy` struct.** Both `h` and `g` are flagged.

```turmeric
(defstruct ^copy H [raw : int handler : (fn [int] int)])
(defn main [] : int
  (let [n 3
        h (H 100 (fn [x : int] : int (* x n)))]
    (let [g h] (println ((.handler g) 2)))
    (println ((.handler h) 2)))
  0)
```

```
ERROR: AddressSanitizer: heap-use-after-free ... in main
```

**2. A function that returns the `^copy` struct it was given.** `r` is flagged
as if `pick` had built a fresh one.

```turmeric
(defstruct ^copy H [raw : int handler : (fn [int] int)])
(defn pick [h : H n : int] : H
  h)
(defn main [] : int
  (let [h (H 100 (fn [x : int] : int (* x 10)))]
    (let [r (pick h 3)]
      (println ((.handler r) 4))))
  0)
```

```
ERROR: AddressSanitizer: heap-use-after-free ... in tur_closure_drop
    #1 drop_fnfields_tur_adt_H
    #2 main
```

Without `^copy` this one is fine: passing `h` moves it, so only `r` is
flagged.

**3. A struct built from a closure parameter.** `h` is flagged, and frees the
caller's closure. This one does not need `^copy`.

```turmeric
(defdata Ev (Start int) (End))
(defstruct H [raw : int handler : (fn [Ev] int)])
(defn mk [f : (fn [Ev] int)] : int
  (let [h (H 1 f)]
    ((.handler h) (End))))
(defn main [] : int
  (let [n 3
        g (fn [e : Ev] : int n)]
    (println (mk g))
    (println (mk g)))
  0)
```

```
ERROR: AddressSanitizer: heap-use-after-free ... in mk__cps
```

## Root cause

`Binding.drops_fn_fields` is set per `let` binding from the binding's TYPE and
a few guards on the BODY (`src/compiler/elab_forms.c`, the "local-struct-drop
(fn-field)" loop: `is_moved`, `is_binding_consumed`, `is_field_consumed`, and
since 2026-11-27 `fnfield_local_leaves_scope`). Nothing looks at the INIT, so
the rule cannot tell a freshly built struct from a copy of one, or a closure
the constructor was handed from one it made.

`elab_structs.c` states the precondition the free relies on: a boxed fn field
makes the struct move-only, "the precondition that keeps a single owner and
avoids a copy double-freeing the shared handle". Two things void it:

- `^copy` on the struct (repros 1 and 2). The recursive-spine free beside this
  one excludes `:copy` for exactly this reason (`!def->is_copy`, "`:copy` IS
  THE SOUNDNESS LINE"); the fn-field free does not.
- A constructor argument that is borrowed (repro 3). A fn parameter is the
  caller's, and storing it in a field does not transfer it.

The CPS path mirrors the direct one (`emit_fnfield_reap`,
`src/compiler/emit_cps_ir.c`). Its `__dk_reap_closure_once` hides shapes 1 and
2 when both binders are in one CPS entry, and nothing more.

## Fix directions

1. **Ask the init.** Flag only a binding whose init is fresh: a constructor
   call whose fn-field arguments are closure literals or top-level function
   names, or a call to a function proven to return a fresh one. The RM1 sum
   work already has that shape of analysis (`returns_fresh_sum_box`,
   `call_returns_fresh_sum_box`, `src/compiler/elab_fns.c`). Everything else
   keeps the leak.
2. **Apply the `:copy` line.** A `^copy` struct with a closure field is not
   freed at all unless the init is a constructor written in place. This is the
   smaller change, and it turns repros 1 and 2 into leaks. The SAX parsers in
   turmeric-spices are `^copy`.
3. Either way, repro 3 needs the constructor to know whether it took
   ownership of each fn argument.

## Workaround

Do not `let`-bind a copy of a closure-field struct or the result of a function
that hands one back; use the original binding. Build such a struct from a
closure written at the constructor call.

## Resolution

**Fixed 2026-11-28**, by fix direction 1: the binder asks its init.

**The rule.** A let local frees its struct's closures only when it is the
**owner** -- `Binding.fnfield_owner`, decided from the init before the body is
elaborated (`elab_fnfield_init_is_fresh`, `src/compiler/elab_forms.c`). The
old guards (moved, consumed, leaves the scope) still apply on top. An init is
fresh when it is:

- a constructor whose every boxed fn field is given a closure minted there: a
  lambda literal, a function name boxed into a fat handle (`EX_FN_TO_FAT`), an
  unboxed fn parameter re-packed into one (`EX_POLY_TO_FAT`: the box is new
  and owns nothing), or a call to a `returns_fresh_closure` function;
- a call to a function stamped `returns_fresh_fnfield`, or one fresh through a
  parameter (`fresh_fnfield_via_param_mask`) whose argument is fresh;
- a move-only local that was the owner (the move hands it over);
- a `let` / `do` / `if` / `match` whose every value path is one of those.

A `^copy` local is never the owner by copying one. As a function's RESULT it
is: the local's own free was withheld because the value leaves the scope
(`fnfield_local_leaves_scope`), so the caller's binder takes it over. That is
the `(let [h (H n (fn ...))] h)` constructor shape.

**The per-function stamp.** `elab_stamp_fnfield_freshness` runs beside
`elab_stamp_sum_freshness` once a `defn` body is elaborated, with the same
greatest-fixed-point treatment of a self-call. A path that returns a move-only
by-value parameter, or that stores a closure parameter in a fn field, sets that
parameter's mask bit. The closure parameter counts only when the body names it
exactly once (a variable or a capture), so a function that also stores it
elsewhere, or puts it in two fields, is not fresh.

**The unbound receiver.** `(.f (S ...) x)` is hoisted into a `let` so the
constructor's closures have an owner (`elab_typeclasses.c`). That `let` frees
them now only when the receiver is fresh.

**What it costs.** Anything the walk cannot prove is a leak, never a free.
Measured against `main` by emitting every fixture and comparing the closure
frees:

- `cps-byvalue-struct-closure-field-value` lost one: the `r` of
  `(pick h g 3)`, which was this report's repro 2 hidden by
  `__dk_reap_closure_once`.
- `closure-slot-ptr-void-param-sinks` lost one, and leaks 24 B more: a
  let-bound closure `cap` that is also pushed into a vec and passed to four
  calls was freed by the one struct that held it. Nothing proved the struct
  was its last user.
- `fn-value-at-tyvar-slot-static-box` lost one, with no effect: the field
  holds `(ident plus1)`, whose box is the shared static one.
- `closure-field-boxed-all-shapes`, `fn-param-stored-in-struct-field` and
  `refine-fn-variance-ok` gained frees and leak less (96 -> 48 B, 88 -> 24 B,
  48 -> 24 B): the call-head receiver below.

The second and third are in
[fnfield-ctor-param-closure-leaks-in-caller](../reported/fnfield-ctor-param-closure-leaks-in-caller.md).

**Forward references.** The answers are read as each `let` and each `defn` is
elaborated, so a caller written above its callee saw "not fresh" and leaked.
Every such `let` and function is noted (`elab_fnfield_note_let`, the stamp)
and decided again once the program is in (`elab_fnfield_resolve_pending`,
called from `elaborate_program_session`), as one greatest fixed point: start
every noted function at "fresh" and every noted `let` at "owner", and lower
whatever a walk does not confirm until nothing moves. That also settles
mutual recursion, and a self-recursive function that threads a move-only
parameter through (`(if done m (spin m (- n 1)))`). Only flags the emitters
read change.

**The call-head receiver.** `((.run (mk n)) x)` reads a closure out of a
struct no binding holds. The method spelling `(.run (mk n) x)` already bound
such a receiver so a `let` owns it; the call-head spelling did not, and leaked
on `main` too. `elab_call_head_expr` (`src/compiler/elab_call.c`) binds it
now, and frees only when the call's result is a plain scalar.

**Repro 3's other half.** The struct built from a closure parameter no longer
frees it, and the caller's own closure is not freed by the caller either. The
callee's parameter counts as retained because it is stored in a constructor.
That leak (24 B per closure) is
[fnfield-ctor-param-closure-leaks-in-caller](../reported/fnfield-ctor-param-closure-leaks-in-caller.md).

**Regression checks.**

- `tests/run.sh`: 4363 passed, 0 failed.
- `tests/run-leak-check.sh`: no failures. The new
  `fnfield-struct-binder-borrowed-closure` is known-open for the leak above.
- A module imported by another (single-file build and `tur build <dir>`):
  the importer's binder frees the struct a module function built.
- The turmeric-spices SAX parsers (`SaxParser`, `HtmlSaxParser`) are bound
  by `match` on a `(Result P E)`, never by a `let`, so this rule never freed
  their closures and does not change them. The spices' suites were not run.

Pinned by `tests/fixtures/fnfield-struct-binder-owns-only-fresh` (leak-checked:
all three repros' owners, every fresh shape, and a constructor function storing
its closure parameter) and `tests/fixtures/fnfield-struct-binder-borrowed-closure`
(repro 3 and its relatives; a use-after-free still fails the gate), and
`tests/fixtures/fnfield-struct-binder-forward-callee` (leak-checked: callers
above their callees, both receiver spellings, mutual recursion; on `main` it
is a heap-use-after-free).

## Found on the way

- [ctor-closure-param-with-scalar-args-refused](../reported/ctor-closure-param-with-scalar-args-refused.md)
  -- open. A constructor given a closure parameter typed `(fn [int] int)` is
  a type error. The same constructor with `(fn [Ev] int)`, Ev a `defdata`,
  compiles.
