# Generic `show-line` / `print-show` wrappers: two rough edges (void return + unresolved element)

**Severity:** low (both are narrow; each has an easy call-site workaround, used in
the stage-4 fixtures)

**Status: ARCHIVED (2026-07-21) -- superseded by a plan; both edges still OPEN.**
The work is now tracked in
`docs/upcoming/generic-show-wrapper-cps-monomorphization-plan.md`, which sequences
the coupled Edge 2 fix (void-temp compile fix + wrapper monomorph/dict dispatch,
which must land together) and Edge 1 (unresolved-element ICE). This file is
retained as the original defect record + repros; the plan is the live tracking
artifact. The Edge 2a void-temp fix was landed then reverted (commit `71b5cef`)
to avoid shipping the 2a-without-2b silent miscompile.

`show-line` and `print-show` (stdlib/typeclass-show.tur) are generic `^Show a`
wrappers that `(show x)` an owned String, print it, release it, and return
`:void`.  Both are the idiomatic replacement for the pre-stage-4
`(println (show x))`.  Two shapes miscompile.

## Edge 1 -- untyped empty collection through the wrapper ICEs

```turmeric
(load "stdlib/typeclass.tur")
(defn main [] : int (do (show-line (vec-new)) 0))
```

```
tur: internal error (ICE): carrier<->concrete crossing reached code emission
with an unresolved parametric param at emit_reresolve_disp_type.
```

`(vec-new)` is a `Vec` with an unresolved element type (no elements to infer `A`
from).  Passing it through the generic `^Show a` wrapper reaches emit with the
element tyvar still unresolved.  `(show (vec-new))` **directly** (not through the
wrapper) is fine -- it renders `[]`.  Workaround: ascribe the element type,
`(show-line (:: (vec-new) (Vec int)))`.

## Edge 2 -- void wrapper result captured under CPS lowering

```turmeric
(load "stdlib/typeclass.tur")
(defn yn [b : bool] : cstr (if b "T" "F"))
(defn demo [] : int
  (let [a (string/from-cstr "Hello")
        b (string/from-cstr "World")
        c (string/concat a b)]
    (do
      (println (string/to-cstr c))
      (println (string/len c))
      (show-line c)                                    ;; void, mid-do
      (println (yn (eq? a (string/from-cstr "Hello")))) ;; typeclass dispatch after
      (println (yn (string/eq? a b)))
      0)))
(defn main [] : int (demo))
```

```
error: variable or field '__t238' declared void
error: void value not ignored as it ought to be
```

The emitted C is `__auto_type __t238 = show_hyline(...); __t62 = __t238;` -- the
`:void` result of the generic `show-line` is captured into a temp and assigned,
which is only reachable through the CPS->direct lowering when a later `do`
statement introduces a CPS boundary (here the `eq?` typeclass dispatch).  A
`show-line` mid-`do` with only plain statements after it compiles fine; it is
the interaction with the following dispatch that trips it.  Workaround: use the
explicit form `(let [s (show c)] (do (println (string/to-cstr s)) (string/release s)))`.

### Edge 2 -- OPEN; an attempted void-temp fix was REVERTED because it unmasks a coupled silent miscompile (2026-07-21)

**Status: OPEN.** A void-temp compile-error fix was written and briefly landed,
then **reverted** (commit follows the revert note below) because it converts the
compile error into a SILENT MISCOMPILE for a common shape -- see "coupled defect".
Edge 2 must be fixed together with that coupled defect, not alone.

The attempted fix (for the record, so it is not re-tried in isolation): in the CPS
emitter (`src/compiler/emit_cps_ir.c`, the `CT_TAILCALL` `cps->direct` arm) a void
call reaches the arm as a tailcall delivering to a continuation and the arm
unconditionally bound the result into an `__auto_type` temp (the
`variable declared void` error). Making the arm detect a `TY_NIL`/`TY_NEVER`
callee (via `fd_for_binding` + `fn_ret_type`) and emit a bare call + deliver the
unit placeholder `0` -- mirroring the `CT_LETCALL` nil arm -- fixes the compile
error. **But that is not enough to ship**: it merely lets `(show-line x)` compile
inside a CPS-lowered helper, where the SAME arm then resolves the callee to the
wrong (carrier-rep) clone. See below. So the compile-error fix and the
callee-resolution fix are coupled and must land together.

#### Coupled defect: generic `^Show a` wrapper misdispatches through the int carrier representative in a CPS-lowered helper

CORRECTION of an earlier note here: `Show[String]` DOES exist
(`stdlib/typeclass-show.tur:126`, `(definstance Show [String] (show [x] : String
(string/retain x)))`), so the String misrender is NOT a missing-instance carrier
fallback and is NOT
`docs/reported/method-dispatch-missing-instance-falls-back-to-carrier-representative.md`.
The real cause: once the void-temp fix lets `(show-line x)` compile inside a
CPS-lowered helper, the `CT_TAILCALL` `cps->direct` arm resolves the callee to the
GENERIC base clone `show_hyline` -- whose body bakes the int carrier
representative `__inst_Show_show_int(x)` -- instead of the emitted monomorph clone
(`show_line__spec__void_int64_t`, which correctly calls `__inst_Show_show_String`
/ `__inst_Show_show_Vec` / ...). The monomorph clone exists but is dead code; the
generic clone runs, treating the `String`/`Vec`/... pointer as an int and printing
the carrier word. Result: a SILENT garbage render for every non-int element type.

Repro (current reverted state: A fails to compile; WITH the void-temp fix applied
alone, A compiles and misrenders, while B renders correctly -- A and B differ only
by helper vs `main`):

```turmeric
(load "stdlib/typeclass.tur")
;; A -- show-line in a helper. Reverted state: `__t declared void` compile error.
;;      With the void-temp fix alone: prints a garbage pointer (e.g. 94330540635104).
(defn demo [] : int
  (let [c (string/concat (string/from-cstr "Hello") (string/from-cstr "World"))]
    (do (show-line c) 0)))
(defn main [] : int (demo))
;; B -- same let directly in main: prints "HelloWorld" (monomorph resolves).
```

General, not String-specific: `(show-line (vec-of 1 2 3))` in a helper prints a
pointer instead of `[1 2 3]`. NON-void `show` in a helper is fine
(`(let [s (show v)] (println (string/to-cstr s)))` renders `[1 2 3]`) -- the gap
is specific to the void-wrapper `CT_TAILCALL` `cps->direct` callee resolution.

This is NOT independent of the void-temp fix: pre-fix, A/C fail to compile (the
`__t declared void` error), so the misdispatch was latent. The void-temp fix
UNMASKS it -- exactly the "do not ship a fix that converts a build error into a
runtime miscompile" hazard (cf.
`docs/reported/effectful-fnvalue-param-miscompile.md`). A complete Edge 2 fix must
also make the `CT_TAILCALL` `cps->direct` arm resolve the wrapper's monomorph
clone (or thread the Show dict) rather than fall back to the carrier-rep base
clone; the two must land together.

## Root cause / fix directions

Both are generic-wrapper monomorphization/lowering gaps, not `Show`-specific:

- Edge 1 (OPEN): `emit_reresolve_disp_type` should resolve (or cleanly diagnose)
  an unresolved element tyvar reached via a `^Class a` wrapper whose argument is
  an element-less parametric container -- rather than ICE.  The default element
  carrier (int) that `(show (vec-new))` uses directly should also apply when the
  container flows through one generic hop. This lives in the ABI carrier-crossing
  recovery machinery (the deep-side `emit_abi_assert_routed_concrete` invariant at
  `emit_core.c:1820`), which is why it is left open rather than point-fixed.
- Edge 2 (RESOLVED): the CPS->direct converter must not bind a `:void`-returning
  call's result to an `__auto_type` temp. A void call in statement position now
  emits as a bare statement even when a later `do` element forces CPS lowering.
  See "Edge 2 resolution" above.

## Notes

Filed while landing stage 4 of docs/archive/show-owned-result-plan.md.  The
stage-4 fixtures use the two workarounds above (type ascription for the empty
collection in show-collections; the explicit release form in string-basic /
string-slice), so the wrappers are otherwise in wide use and work for every
element-typed value and every non-CPS-boundary `do` position.
