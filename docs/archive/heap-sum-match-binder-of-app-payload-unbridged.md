# A `(Cons T)` payload bound by a generic `match` on a `:heap` sum is declared `int64_t`, and rebuilding passes it to a typed-pointer constructor uncast

**RESOLVED 2026-10-14** as a side effect of fixing
[heap-sum-generic-never-specialized](heap-sum-generic-never-specialized.md).
Pinned by `tests/fixtures/heap-sum-match-binder-cons-payload`: this repro, the
same rebuild with the sum as the parameter itself (which needed the
never-specialized fix to get a clone at all), and a `float` element.

## Resolution

Half 1 of the root cause below is gone, at its source rather than by widening
the SR2b leg (fix direction 1). The ADT match emitter upgrades a scrutinee to
its monomorph when the spec resolves it concretely. That upgrade now accepts a
`:heap` parametric sum app as well as a by-value product
(`adt_app_is_heap_sum`, `src/compiler/emit_expr.c:20913` and `:20939`). Inside
`bump__spec__...` the scrutinee is therefore `tur_adt_Fr__R *`, and its typed
`(Cons R)` field declares the binder as `tur_adt_Cons__R *`, so
`ctor_Fr_Frame__R` receives the type it takes. Verified on unmodified `main`
first: `CC=clang tur run` fails there with the `-Wint-conversion` error
quoted below, and builds and prints `8` with the fix.

Half 2 (the constructor-argument bridge trusts the argument's type over the
local's declared C type) is unchanged. As the report says, it is harmless
alone; fix direction 2 remains a hardening option, not an open defect.

## A parallel fix, on the same day

This branch fixed the same repro one level down, before the source-level fix
above reached it. That fix added a `TY_APP` twin of the SR2b base-carrier leg
(`match_binder_base_app_ptr_ctype`, `src/compiler/emit_expr.c`). When a
scrutinee is still read through the erased base layout and the binder's type
is a parametric application that the active spec grounds to a typed pointer,
the binder relabels the base slot's int64 word as that pointer. Both the
switch path and the guarded if-chain path take it. With the scrutinee
upgrade above, this repro no longer reaches that leg. It stays as the
binder-level answer for a scrutinee the upgrade does not cover. It is
pinned, alongside `heap-sum-match-binder-cons-payload`, by
`tests/fixtures/heap-sum-match-binder-app-payload`: a guarded rebuild, plus
`:heap`-sum, int, 7.1 and cstr instantiations.

Merging the two fixes exposed a third gap. The upgrade makes the guarded
rebuild's scrutinee a monomorph, so it takes the if-chain path's monomorph
binder leg. That leg substituted only a field declared as a bare type
variable, so `l : (Cons T)` stayed `int64_t` and clang rejected the
rebuild again. The if-chain path now carries the switch path's SR2b leg as
well: when the binder names the int64 carrier, it substitutes the field's
declared type through the scrutinee's arguments. The guarded fixture pins
it.

**A correction to half 2 below.** It overstates the bridge.
`emit_var_spec_arg_type` answers only for a *parameter* of the active spec,
so for the binder `l` it returned nothing. The binder was not recorded
either, so the bridge had no C type for the argument and inserted no cast.

**The CPS backend's arm-binder mirror** (`emit_cps_ir.c`, the `CT_MATCH`
field binds) has none of the spec-substitution legs. An effectful version of
this rebuild ICEd earlier, on the merge temp, and was filed as
[heap-sum-rebuild-in-effectful-generic-merge-temp-ice](heap-sum-rebuild-in-effectful-generic-merge-temp-ice.md),
resolved the same day. The rebuilt clones there print the right values, so
that mirror is not on that path.


**Severity: medium.** This is a build failure under clang 15+ and gcc 14:
`-Wint-conversion` is an error there. gcc 13 only warns, but `tests/run.sh`
fails a fixture on that warning. The program is correct, and with gcc 13 it
runs and prints the right answer.

**Discovered:** 2026-10-13, prototyping
[generic-zipper-plan](../upcoming/generic-zipper-plan.md) (its D1), on
`main` at v0.64.4 (`704cc531`).

## Repro

```turmeric
(defdata R :heap :copy (Lf int) (Br (Cons R)))
(defdata Fr :heap :copy [T] (Nil) (Frame (Cons T) T))
(defstruct Box :heap :copy [T] (fr (Fr T)))
(defn val [r : R] : int (match r (Lf v) v (Br _) 0))
(defn bump [T] [b : (Box T)] : (Box T)
  (make-struct Box :fr (match (.fr b)
                         (Nil) (.fr b)
                         (Frame l x) (Frame l x))))
(defn main [] : int
  (let [b (make-struct Box :fr (Frame (tcons-of (Lf 1) (tnil)) (Lf 7)))]
    (match (.fr (bump b))
      (Nil) (println 0)
      (Frame l x) (println (+ (val x) (val (thead l))))))
  0)
```

`CC=clang tur run`:

```
error: incompatible integer to pointer conversion passing 'int64_t' (aka 'long')
to parameter of type 'tur_adt_Cons__R *' [-Wint-conversion]
```

Inside the clone `bump__spec__tur_adt_Box__R___tur_adt_Box__R__`, the arm
emits:

```c
int64_t l_N = (int64_t)__scrut->as.Frame._0;
int64_t x_N = (int64_t)__scrut->as.Frame._1;
... ctor_Fr_Frame__R(l_N, x_N) ...   /* takes (tur_adt_Cons__R *, int64_t) */
```

The `Box` wrapper is there so that `bump` gets a clone at all. Without it,
[heap-sum-generic-never-specialized](../reported/heap-sum-generic-never-specialized.md)
hits first.

## Root cause

Two halves, each harmless alone.

1. **The binder takes the base layout's slot type.** The scrutinee `(Fr R)`
   is not recognized as a monomorph (`scrut_is_app_monomorph` is false,
   `src/compiler/emit_expr.c:20874`). The reason is that `type_c_name`
   spells a `:heap` parametric *sum* application as `int64_t`
   (`src/compiler/types.c:4768` names only single-constructor products). The
   binder is therefore declared with the base union slot's `int64_t`. The
   SR2b "BASE-carrier leg" that substitutes the active spec's type for a
   binder (`src/compiler/emit_expr.c:21682`) is gated on
   `fb->type.kind == TY_TYVAR || TY_FORALL`. A binder typed `(Cons T)` is a
   `TY_APP`, so it keeps `int64_t`.
2. **The constructor-argument bridge reads the argument's type, not its
   declaration.** At `src/compiler/emit_expr.c:13107`, an `EX_VAR` argument's
   C type comes from `emit_var_spec_arg_type`: the spec-resolved `(Cons R)`,
   that is `tur_adt_Cons__R *`. That matches the slot, so no cast is
   inserted. The local's *declared* C type, which
   `emit_localvar_lookup_ctype` would return (`:13125`), is consulted only
   when the type-derived lookup fails.

## Fix directions

- **Widen the base-carrier leg.** Let it also substitute when the binder's
  type is a `TY_APP` whose resolved C name is a typed pointer. The
  declaration then matches what every later use assumes. This is the more
  local fix.
- **Prefer the declaration in the bridge.** When a bare-identifier argument
  has a recorded C type, the bridge should use it before the type-derived
  one. That catches any binder or temp whose declaration and type disagree,
  not just this one.
- **Pin it.** Make the repro a fixture; it fails `run.sh`'s emitted-C
  `-Wint-conversion` check today.

## Related

- [heap-sum-generic-never-specialized](../reported/heap-sum-generic-never-specialized.md)
  has the same `types.c` spelling as its root.
