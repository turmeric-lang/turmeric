# Rebuilding a parametric `:heap` sum in an effectful generic ICEs on the merge temp

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
