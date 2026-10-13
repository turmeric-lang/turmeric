# A by-value `(Option ...)` field of a `:heap` struct, passed to a generic function, is read as a carrier word

**Severity: medium.** The emitted C is rejected by both gcc and clang. The
program is correct.

**Discovered:** 2026-10-13, prototyping
[generic-zipper-plan](../upcoming/generic-zipper-plan.md) (its D3), on
`main` at v0.64.4 (`704cc531`).

## Repro

```turmeric
(defstruct Fr :heap :copy [T] (x T))
(defstruct Holder :heap :copy [T] (p (Option (Fr T))))
(defn peek [T] [o : (Option (Fr T))] : bool (match o (None) false (Some _) true))
(defn probe [T] [h : (Holder T)] : bool (peek (.p h)))
(defn main [] : int
  (println (probe (make-struct Holder :p (Some (make-struct Fr :x 7)))))
  0)
```

`tur run`:

```
error: aggregate value used where an integer was expected
```

## What is emitted

The struct holds the field by value:

```c
struct tur_adt_Holder__int {
    tur_adt_Option__Fr__int p;
};
```

The clone `peek__spec__bool_tur_adt_Option__Fr__int` takes it by value
too. But `probe`'s clone reads the field as a carrier word, then unboxes
that word:

```c
int64_t __t = (int64_t)(intptr_t)(((tur_adt_Holder__int *)(intptr_t)(h))->p);
... peek__spec__...((__t ? (*(tur_adt_Option__Fr__int *)(intptr_t)(__t))
                         : (tur_adt_Option__Fr__int){0})) ...
```

The `(int64_t)(intptr_t)` cast of a struct value is the error.

## Root cause (not fully located)

**The emitter of the bad read is known.** The readback is
`emit_carrier_bridge` (`src/compiler/emit_core.c:6219`), in its SR2b
null-guarded deref of a carrier box (around `:6744`).

**The wrong judgement is upstream.** `emit_carrier_bridge` only runs when
the call-argument path has decided that the argument's *source*
representation is the carrier. For an `EX_FIELD` read of a by-value
`Option` field, that judgement is wrong: the field is already the
aggregate the parameter wants, so no bridge is needed. The likely cause is
that the representation is judged from the field's *generic* declared type
`(Option (Fr T))`, which spells as the carrier, rather than from the
struct monomorph's actual field type. This has not been confirmed.

## Fix directions

- **Find the judgement.** Locate where the call-argument path classifies an
  `EX_FIELD` argument as carrier-represented, and judge it from the
  monomorph's field C type (`tur_adt_Option__Fr__int`) instead.
- **Pin it.** Make the repro a fixture. Also cover a non-`Option` by-value
  sum field and an `Option` of a scalar, to show which shapes share the
  path.

## Workaround

Store the link as a nullable typed pointer, as `(Cons A)` does for nil,
instead of an `Option` field. The zipper plan does this for its path
frames.
