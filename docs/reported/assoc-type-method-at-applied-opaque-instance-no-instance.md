# A method returning an associated type fails inside a generic when the instances are keyed on an applied opaque

**Severity: medium-low (blocks a generic numeric-array API; a workaround exists).**

Found 2026-10-24 while prototyping the generic option of
[simd-and-vectorization-plan](../upcoming/simd-and-vectorization-plan.md)
(open question 1).

A class method whose result is the class's associated type, called on the
constraint's type variable inside a constrained generic, hard-errors with
`no instance '<Class> tyvar'` when every instance head is an *applied*
opaque such as `(NumVec float)`. The same program with plain opaques
(`F64V`, `F32V`) compiles and prints the right answer, and so does the
applied-opaque version when the method's result is a fixed type instead of
the associated one.

## Repro

```turmeric
(defopaque NumVec [t] :int)
(defclass NumSum [v]
  (type Elem : Type)
  (nv-sum [v] : Elem))
(definstance NumSum [(NumVec float)] (type Elem = float) (nv-sum [v] 7.1))
(definstance NumSum [(NumVec float32)] (type Elem = float32) (nv-sum [v] (:: 3.25 float32)))
(defn total [V] [(NumSum V)] [v : V] : (Elem V)
  (nv-sum v))
(defn main [] : int
  (let [a : (NumVec float) (:: 0 (NumVec float))
        b : (NumVec float32) (:: 0 (NumVec float32))]
    (println (total a))
    (println (total b))
    0))
```

```
error: no instance 'NumSum tyvar'      ; at (nv-sum v) in total
```

Expected `7.1` then `3.25`. Replace `(NumVec float)` / `(NumVec float32)`
with two plain opaques `(defopaque F64V :int)` / `(defopaque F32V :int)` and
it prints exactly that.

## Root cause (likely)

`nv-sum`'s result is the associated type, so dispatch goes down the
return-directed path in `src/compiler/elab_typeclasses.c`. At an abstract
type variable it searches for a representative instance (around the
`carrier_opaque` loop, ~line 6380): it takes a `TY_INT` head, or a `TY_ADT`
head whose `AdtDef` is a non-pointer opaque. An applied opaque head is a type
application, not a bare `TY_ADT`, so neither branch matches, no
representative is found, and the `no instance '%s %s'` error at ~line 6453
fires. Not confirmed with a debugger.

## Fix directions

- Accept an applied head whose constructor is a non-pointer opaque as a
  carrier representative, with the same `tyvar_reaches_param` gate. Emit-side
  re-resolution already specializes per instance (a fixed-result method on the
  same instances gets per-dictionary specs `total__spec__double_int64_t` and
  `..._int64_t__h1`).
- Pin it with a fixture beside `assoc-type-projection-at-tyvar`, covering both
  an applied-opaque head and an applied-struct head.

## Workaround

Give each instance a plain (non-applied) opaque head, or return a fixed type
from the method.
