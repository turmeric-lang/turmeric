# A constructor refuses a closure parameter whose argument types are scalars

**Severity: medium (compile failure on a valid program).** A by-value struct's
fn field, filled from a function parameter of the field's own type, is a type
error when that type's arguments are primitives.

Found 2026-11-28 while fixing
[fnfield-struct-binder-frees-borrowed-closure](../archive/fnfield-struct-binder-frees-borrowed-closure.md).
It reproduces on `main` at `c878a702`, before that change.

## Repro

```turmeric
(defstruct H [raw : int handler : (fn [int] int)])
(defn wrap [f : (fn [int] int)] : H (H 2 f))
(defn main [] : int 0)
```

```
error [TUR-E0001]: function 'H' arg 2: expected (fn [] : ?), got ptr<void>
```

The same error appears for `(fn [cstr] int)` and `(fn [int int] int)`, and when
the constructor is inside a `let`. With a `defdata` argument type
(`(fn [Ev] int)`, `(fn [Ev] bool)`) it compiles, and so does a lambda written
at the constructor call.

## Root cause

Not investigated. The expected type `(fn [] : ?)` has lost the field's arity
and result, and the parameter reads as `ptr<void>`, its fat-closure carrier.
So the check seems to compare the parameter's erased carrier against a field
type that was never resolved.

## Fix directions

1. Find where the constructor's argument check builds the expected field type
   `(fn [] : ?)` for this case. It should be the declared
   `(fn [int] int)`.
2. Compare the parameter by its binding's declared fn type, not the carrier,
   as the `^fat` forwarding path does (`EX_FN_TO_FAT`, `word_params`,
   `src/compiler/emit_expr.c`).

## Workaround

Give the closure a non-scalar argument type, or pass a lambda that calls the
parameter: `(H 2 (fn [x : int] : int (f x)))`.
