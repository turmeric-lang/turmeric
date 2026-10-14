# Rebuilding a parametric `:heap` sum in an effectful generic ICEs on the merge temp

**Resolved 2026-10-14: the report is archived.** The repro builds under gcc
and clang and prints `8`. The ICE was the Debug-only face of three separate
defects. Only the first had anything to do with `:heap` sums. The other two
broke generic functions that perform, whatever their types, and they are
what a Release build reported.

1. **A `let` or merge temp of a parametric `:heap` sum was declared
   `int64_t`.** `emit_repr_concrete_heap_ptr_c_name` (`emit_expr.c`) is the
   shared let-bind and merge-temp chokepoint. It took its typed-pointer
   branch, because `repr_of` says heap-ptr, but then asked `type_c_name` for
   the spelling, and that names only products. The non-parametric `R` was
   already rescued by `adt_heap_ptr_c_name`; the application form had no
   such fallback. `adt_app_heap_ptr_c_name` (`types.c`) is that fallback
   (`(Fr int)` → `tur_adt_Fr__int *`). This shape needs no effect at all:
   `(let [f (Frame 3 4)] ...)` alone ICE'd in Debug.

   With the binding now a typed pointer, one consumer was not bridged: a
   closure capturing it stored the pointer into its `int64_t` env field
   (`__t->g = g_25`, -Wint-conversion). That was already true for the
   non-parametric `R` on the baseline. The env fill now relabels a let-bound
   capture the chokepoint spells as a pointer.
2. **`mono_sig_ok` refused a concrete heap handle in a monomorph clone's
   signature.** A generic function that performs has a base that always
   sig-rejects, so its clone is the `perform`'s only lowering.
   `(defn total [T] [v : (Vec T)] : int (+ (perform (Tick)) (vec-len v)))`
   was a hard "no lowering here" error, and so was any `:heap` struct
   parameter. Here that was `bump`'s `(Box T)`, which is why the effectful
   repro never got as far as the binder. The gate admits a concrete handle
   (`mono_heap_handle_ok`, `emit_cps_ir.c`), but only for a clone whose
   body reaches a `perform`. Those are exactly the clones that had no other
   lowering. A first cut admitted the handle for every colored generic and
   moved `map-eq-driver` onto DK continuations for nothing. It is colored
   only by its `^fat` comparator call, and the `map-typed-consumer`
   snapshot caught it. The exclusion's comment cited a show-collections
   miscompile. That was the typeclass-instance double emission, and
   `is_inst` fences it independently of type.
3. **`cps_expr_contains_effect_op` had no arm for `match`.** A generic
   function whose only `perform` sits in a match arm (or under a
   constructor argument, or a field read) was never asked for a clone, so
   it had no lowering either. That happened even with a tyvar-only
   signature: `(defn pick [T] [x : T] : T (match 1 1 (do (perform (Tick)) x)
   _ x))`. Its `default:` now falls back to `cps_visit_children`, as the
   coloring and call-edge walks in the same file already do. That was the
   fix after this hole had been filed three times against them.

Pinned by two fixtures, both running under `run.sh` and `run-turi.sh`, clean
with gcc 13 and clang 18. No `expected.c` moved; full `run.sh` was 3798
passed, 0 failed (the two new fixtures included).

- `tests/fixtures/generic-performer-heap-handle-param` covers halves 2
  and 3. It has a generic performer over `(Vec T)`, over a `:heap`
  `(Box T)` at int, cstr and 7.1, and with its `perform` only in a match
  arm, plus this report's rebuild at `R` and at a fractional float.
- `tests/fixtures/heap-sum-let-binding-typed-pointer` covers half 1. A
  let-bound `(Fr int)` and `R` flow into concrete and generic callees, a
  tail `let`, a struct field, a `Vec`, two closure captures, an `if`
  merge, `set!`, a tail-recursive loop, a 7.1 element and a nested
  application.

Neither carries `requires.leak-check`. Under LSan, their only leaked blocks
are `:heap` nodes and an unfreed `Vec`, which are process-lifetime by
design. No leaked block comes from a DK frame or a closure env.

**Found on the way and filed open:**
[handle-under-match-in-main-has-no-lowering](../reported/handle-under-match-in-main-has-no-lowering.md).
`(match (handle (f) ...) ...)` in `main` is the same hard error. This
fixture's first draft hit it, and it reproduces on the baseline with no
generics at all.

The original filing follows. Its "Root cause" guessed at the ICE and missed
that it was the shadow's view of half 1. It also missed halves 2 and 3, which
the Release-visible error comes from. Its third bullet predicted a second
failure in the CPS `CT_MATCH` binder loop once the ICE was gone. That did not
happen: the rebuilt clones print the right values at `R` and at float, so
that loop is not on this path.

**Severity: medium.** This is an internal compiler error on a correct
program, and it is loud: `tur` refuses to build. It does not produce a wrong
answer.

**Discovered:** 2026-10-14, fixing
[heap-sum-match-binder-of-app-payload-unbridged](../archive/heap-sum-match-binder-of-app-payload-unbridged.md),
on `main` at `90416910`. That fix does not cause it: the unmodified compiler
fails the same way.

## Repro

```turmeric
(defeffect Tick [] int)
(defdata R :heap :copy (Lf int) (Br (Cons R)))
(defdata Fr :heap :copy [T] (Nil) (Frame (Cons T) T))
(defstruct Box :heap :copy [T] (fr (Fr T)))
(defn val [r : R] : int (match r (Lf v) v (Br _) 0))
(defn bump [T] [b : (Box T)] : (Box T)
  (make-struct Box :fr (match (.fr b)
                         (Nil) (.fr b)
                         (Frame l x) (do (perform (Tick)) (Frame l x)))))
(defn main [] : int
  (let [b  (make-struct Box :fr (Frame (tcons-of (Lf 1) (tnil)) (Lf 7)))
        b2 (handle (bump b) (Tick [] k) (resume k 0))]
    (match (.fr b2)
      (Nil) (println 0)
      (Frame l x) (println (+ (val x) (val (thead l))))))
  0)
```

`tur run` (gcc or clang):

```
tur: internal error (ICE): a representation decision disagrees with repr_of at merge-temp.
  repr-shadow merge-temp result type=(Fr R) want=heap-ptr got=carrier-i64 cty=int64_t own=int64_t
```

Without the `perform`, the same program builds and prints `8`. That is the
fixture `heap-sum-match-binder-app-payload`.

## Root cause

Not located. What is known:

- `repr_of` calls the `(Fr R)` merge temp a heap pointer. The emitter declares
  it `int64_t`, because `type_c_name` spells a `:heap` parametric *sum*
  application as the carrier (`src/compiler/types.c`, the `TY_APP` arm names
  only single-constructor products).
- In the version without the effect, the same match's merge temp is also
  declared `int64_t` (`int64_t __tN = 0;` in `bump__spec__tur_adt_Box__R_...`)
  and does not trip the shadow. Which question the two paths ask differently
  has not been traced.
- `heap-sum-generic-never-specialized` has the same `types.c` spelling as its
  root.
- Past the ICE there is a second gap. The CPS backend's arm-binder mirror
  (`emit_cps_ir.c`, the `CT_MATCH` field binds) has none of the direct
  emitter's spec-substitution legs: neither the SR2b tyvar leg nor the
  `TY_APP` typed-pointer leg (`match_binder_base_app_ptr_ctype`). So once the
  ICE is fixed, the `(Cons T)` binder would probably be declared `int64_t`
  again on this path.

## Fix directions

- Find which merge-temp site the effectful body reaches, and make it agree
  with the direct path's treatment of a `:heap` parametric sum.
- Give the CPS `CT_MATCH` binder loop the same substitution the direct
  emitter does, through the shared helper.
- Pin it: this repro becomes a fixture next to
  `heap-sum-match-binder-app-payload`.
