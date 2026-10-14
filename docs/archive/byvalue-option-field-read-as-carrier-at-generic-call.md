# A by-value `(Option ...)` field of a `:heap` struct, passed to a generic function, is read as a carrier word

**Resolved 2026-10-14: the report is archived.** The repro prints `true`.
`probe`'s clone now passes the field straight through:
`peek__spec__...(((tur_adt_Holder__int *)(intptr_t)(h))->p)`.

**The judgement, located.** The call-argument bridge (`emit_expr.c`, the
carrier->concrete `emit_carrier_bridge` call in the spec-argument loop)
already had a guard for exactly this case: G9's
`field_read_emits_byvalue_aggregate`. Its comment says "a by-value
aggregate field read is ALREADY the concrete aggregate". That guard
substituted the receiver's type arguments into the field's declared type
only when that type was a **bare** parameter (`(x T)`). Here the field is
`(Option (Fr T))`, an *application* over `T`, so it stayed generic. It
failed the guard's concreteness test, the guard said "not by-value", and
the bridge ran. The report's guess, that the field was judged from its
generic declared type, was right. The guard now substitutes any field type
that mentions a type variable.

**Which shapes shared the path.** These were measured on the previous
commit. Every **argument-position** read of a by-value field whose type is
an application over `T` failed:

- `(Option (Fr T))`: four clones (int, 7.1 and cstr instantiations)
- `(Option T)`: two clones
- a non-`Option` by-value sum `(Sm T)`: two clones
- a nested receiver `(.p (.h o))`: one clone

That was nine compile errors in one probe. A let-bound read, a returned
read and a read matched on directly take other paths and were already
right.

Pinned by `tests/fixtures/byvalue-field-of-param-app-as-generic-arg`. It
covers all of the shapes above plus `None`/`Miss` values and the three
controls, under `run.sh` (gcc 13, clang 18) and `run-turi.sh`. No
`expected.c` moved; full `run.sh` was 3801 passed, 0 failed. The fixture
does not carry `requires.leak-check`: its only leaked blocks are `:heap`
nodes, which are process-lifetime by design.

The original filing follows.

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
