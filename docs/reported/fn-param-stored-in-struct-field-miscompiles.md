# A `(fn ...)` parameter stored into a struct field does not compile

**Severity: low-medium (cc error on a natural program; `^fat` works around
it).** Found 2026-10-21 migrating tur-plot's curve callbacks to typed fn
fields (plot-renderer-typeclass-plan P4).

A function-typed parameter not marked `^fat` cannot be stored into a struct
field of the same function type. The program type-checks and the C compiler
rejects it:

```turmeric
(defstruct F :copy [f : (fn [float] float)])
(defn sq [x : float] : float (* x x))
(defn mk [f : (fn [float] float)] : F (make-struct F f))
(defn main [] : int
  (do (println ((.f (mk sq)) 3.0))
      0))
```

```
error: aggregate value used where an integer was expected
    tur_adt_F __ps_202 = (ctor_F_F((int64_t)(intptr_t)(f)));
```

Writing `[^fat f : (fn [float] float)]` makes the same program print `9`.

## Root cause

A non-`^fat` fn parameter is the two-word poly carrier `tur_poly_fn_t
{env, fn, fn_cps}`; a struct's fn field is the one-word fat closure handle
(`int64_t`, dropped with `TUR_CLOSURE_DROP`). The constructor argument is
emitted with the scalar cast `(int64_t)(intptr_t)(f)`, which C refuses on a
struct.

The conversion the field needs already exists as `EX_POLY_TO_FAT`: a carrier
param flowing into a type-variable slot is boxed onto the fat handle
(`src/compiler/elab_call.c`, the `fn-param-at-type-variable-slot` block just
before `arg_ok`), and so is one returned from a defn whose result is a fn type
(`elab_normalize_fn_tail_leaves`, `src/compiler/elab_fns.c`). The
`make-struct` / constructor field sink is not on that list.

## Fix directions

1. Wrap a carrier fn param flowing into a fn-typed constructor field in
   `EX_POLY_TO_FAT` (sink type = the field's fn type), as the type-variable
   slot does.
2. A fixture: the program above, plus a capturing `fn` literal passed through
   the same parameter.
