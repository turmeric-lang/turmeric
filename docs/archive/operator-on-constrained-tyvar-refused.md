# Operators on a constrained type variable were refused

**Resolved 2026-10-29.** Severity was low-medium (an expressiveness hole: the
working spelling existed, but generic numeric code could not use `+`).

Found while checking what generic code over a numeric-array class can do
(simd-and-vectorization-plan section 3.1).

## What was refused

```turmeric
(defn plus [A] [(Num A)] [x : A y : A] : A {x + y})
(defn sum2 [V] [(NA V) (Num (Elem V))] [^borrow a : V] : (Elem V)
  {(na-ref a 0) + (na-ref a 1)})
```

Both were `TUR-E0006: operator lookup failed for '+' ... first arg type
tyvar`, and so was `(< x y)` under `(Ord A)` -- while `(add x y)` and
`(lt? x y)` were accepted.

## Causes and fixes

- **The operator fallback never considered a type variable.**
  `elab_try_num_operator_dispatch` (`src/compiler/elab_call.c`) already reads
  a builtin-operator miss as the `Num` method, for user numeric types, but
  only when an instance matches the operand's exact type. It now also
  dispatches when the operand is abstract and the enclosing generic is
  constrained by the class (`elab_tyvar_constrained_by`), and maps `< <= >
  >=` onto `Ord`'s `lt? lte? gt? gte?` the same way.
- **A constraint on a projection was recorded with no variable.** The
  middle-vector parser (`src/compiler/elab_fns.c`) stored `(Num (Elem V))`
  with `tyvar = NULL`, so nothing could look it up by the operand's type. It
  is recorded under the projection's own name, `(Elem V)`, now.
- An operator on an UNconstrained type variable is still TUR-E0006; a note
  names the constraint that would make it dispatch.

`=` is not covered (it has its own rows); a literal operand is refused, as
any literal at a generic's own type now is
([literal-at-associated-type-unchecked](literal-at-associated-type-unchecked.md)).

## Fixtures

`operator-on-constrained-tyvar` (arithmetic, a variadic `+`, unary `-`, a
running total, comparisons; at a projection and at a plain variable; float
and int32 instances; cc, turi and the JIT agree) and
`errors/operator-on-unconstrained-tyvar-refused`.
