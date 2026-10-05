---
title: Semigroups, Monoids, and Lattices
category: Functional Patterns
description: The algebraic combining vocabulary in stdlib/typeclass-lattice.tur -- Semigroup, Monoid, the join/meet lattice classes, the selection newtypes that carry them, mconcat, and the law predicates.
---

# Semigroups, Monoids, and Lattices

`stdlib/typeclass-lattice.tur` gives a name to the most common shape in
programming: **these two values combine, associatively**. Summing a column,
folding a list of updates, merging two replicas of a distributed counter, and
chaining signal-processing stages are all the same operation wearing different
clothes.

The module ships six classes, six newtypes that carry the algebras, a fold
(`mconcat`), an induced order (`lattice-leq?`), and a set of law predicates you
can run against your own instances.

## At a Glance

| Class | Superclass | Method | Means |
|---|---|---|---|
| `Semigroup` | -- | `combine [x y] : a` | associative binary operation |
| `Monoid` | `Semigroup` | `mempty [] : a` | identity element for `combine` |
| `JoinSemilattice` | -- | `join [x y] : a` | least upper bound -- associative, commutative, **idempotent** |
| `MeetSemilattice` | -- | `meet [x y] : a` | greatest lower bound (the dual) |
| `BoundedJoin` | `JoinSemilattice` | `bottom [] : a` | least element; identity for `join` |
| `BoundedMeet` | `MeetSemilattice` | `top [] : a` | greatest element; identity for `meet` |

A constraint on a class entails the class in its Superclass column, so
`[^Monoid A]` alone licenses `combine` and `[^BoundedJoin A]` alone licenses
`join`. See [Superclasses](typeclass-guide.md#superclasses) for the rule.

`JoinSemilattice` declares exactly the shape `Semigroup` does -- both are
`a -> a -> a`. What separates them is **commutativity and idempotence**, which
live in the laws, not the signature. That is why the law predicates below are
the content of this module rather than an accessory to it.

## Loading

The module is **not auto-loaded**. The auto-load list is paid by every
translation unit in every build, and nothing else in stdlib depends on these
classes, so you opt in:

```turmeric
(load "stdlib/typeclass-lattice.tur")
```
```sweet-exp
load "stdlib/typeclass-lattice.tur"
```

That one load brings in the classes, the newtypes, their instances, and the
laws. `Eq` and `Vec` come from the auto-loaded stdlib, so nothing else is
needed.

## Semigroup

```turmeric
(defclass Semigroup [a] (combine [x : a y : a] : a))
```
```sweet-exp
defclass Semigroup [a]
  combine [x : a y : a] : a
```

One law, and it is not checked by the compiler -- it is on you:

> **associativity** -- `(combine (combine x y) z)` = `(combine x (combine y z))`

```turmeric
(println (:: (combine (:: 3 Sum) (:: 7 Sum)) int))         ; => 10
(println (:: (combine (:: 3 Product) (:: 7 Product)) int)) ; => 21
```
```sweet-exp
println $ (:: combine((:: 3 Sum) (:: 7 Sum)) int)          ; => 10
println $ (:: combine((:: 3 Product) (:: 7 Product)) int)  ; => 21
```

## Monoid

A `Monoid` is a `Semigroup` with an identity, and it is declared that way.
A function constrained by `Monoid` may call `combine` without also naming
`Semigroup`:

```turmeric
(defclass Monoid [a]
  [(Semigroup a)]
  (mempty [] : a))

(defn double-up [^Monoid A] [x : A] : A
  (combine x x))
```
```sweet-exp
defclass Monoid [a]
  [(Semigroup a)]
  mempty [] : a

defn double-up [^Monoid A] [x : A] : A
  combine(x x)
```

The other side of that is an obligation on instances. Every
`(definstance Monoid [T] ...)` needs a `(definstance Semigroup [T] ...)` beside
it, anywhere in the program, or the build stops with `TUR-E0393`. The
`Monoid` instance does not supply `combine`; the `Semigroup` one does.

> **identity** -- `(combine (mempty) x)` = `x` = `(combine x (mempty))`

### `mempty` cannot go in first argument position

This is the one ergonomic wrinkle worth knowing up front. Argument inference
runs **left to right**, so in first position `mempty` has nothing to fix its
type yet:

```turmeric
(combine (mempty) x)   ; ERROR: cannot infer type for return-directed method 'mempty'
```

Bind it to a local with a declared type instead. In second position it is
already fine, because `x` has fixed the class variable:

```turmeric
(let [unit : Sum (mempty)]
  (combine unit (:: 3 Sum)))        ; => Sum 3
```
```sweet-exp
let [unit : Sum (mempty)]
  combine(unit (:: 3 Sum))
```

The same rule applies to `bottom` and `top`, which are nullary for the same
reason.

There is a deeper version of this constraint: **a constrained generic's type
variable must reach a parameter** for a nullary method to resolve inside it. A
generic whose tyvar appears only in the return type interns one specialization
for every instantiation, so the representative gets baked in for all of them.
`mconcat` below works precisely because its `(Vec A)` parameter carries the
class variable.

## The selection newtypes

`int` is a monoid four different ways -- sum, product, min, and max -- so the
module ships **no instance for a bare primitive**. Shipping one would pick a
winner arbitrarily, and a user who wanted a different one could not have it: a
colliding `definstance` from outside `stdlib/` is a hard **TUR-E0025**
(reject, not replace).

Instead, six newtypes each carry one algebra. Values are made by ascription,
and the carrier is read back the same way:

| Newtype | Carrier | `combine` | `mempty` |
|---|---|---|---|
| `Sum` | `:int` | addition | `0` |
| `Product` | `:int` | multiplication | `1` |
| `MinI` | `:int` | minimum | largest `int64` |
| `MaxI` | `:int` | maximum | smallest `int64` |
| `Any` | `:bool` | disjunction (`or`) | `false` |
| `All` | `:bool` | conjunction (`and`) | `true` |

```turmeric
(combine (:: 3 MinI) (:: 7 MinI))        ; => MinI 3
(combine (:: true Any) (:: false Any))   ; => Any true
(combine (:: true All) (:: false All))   ; => All false
```
```sweet-exp
combine((:: 3 MinI) (:: 7 MinI))
combine((:: true Any) (:: false Any))
combine((:: true All) (:: false All))
```

> **Naming caution:** the newtype `Any` is `bool` under disjunction. It is
> unrelated to the `any` dynamic type. The capitalization is the only thing
> distinguishing them.

Because `Semigroup [int]` is deliberately left undefined, it stays free for you
to claim unopposed if your program really does have one canonical integer
algebra.

## Folding with `mconcat`

```turmeric
(mconcat (vec-of (:: 1 Sum) (:: 2 Sum) (:: 3 Sum)))          ; => Sum 6
(mconcat (vec-of (:: 2 Product) (:: 3 Product) (:: 4 Product))) ; => Product 24
(mconcat (:: (vec-of) (Vec Sum)))                            ; => Sum 0
```
```sweet-exp
mconcat $ vec-of((:: 1 Sum) (:: 2 Sum) (:: 3 Sum))
mconcat $ vec-of((:: 2 Product) (:: 3 Product) (:: 4 Product))
mconcat $ (:: vec-of() (Vec Sum))
```

The empty case is the one that needs the identity, and it is where a `Monoid`
earns its keep over a bare `Semigroup`. `mconcat-from` is the same fold with an
explicit accumulator and index range, if you want to fold a slice.

## The lattice four

A **join** is a `combine` that is additionally commutative and idempotent.
Idempotence is the law a CRDT actually leans on: re-delivering the same state
must not change the result.

```turmeric
(join (:: 3 MaxI) (:: 7 MaxI))       ; => MaxI 7
(meet (:: 3 MinI) (:: 7 MinI))       ; => MinI 3
(join (:: false Any) (:: true Any))  ; => Any true
(meet (:: false All) (:: true All))  ; => All false

(let [z : MaxI (bottom)] (join z (:: 3 MaxI)))   ; => MaxI 3
(let [t : MinI (top)]    (meet t (:: 3 MinI)))   ; => MinI 3
```
```sweet-exp
join((:: 3 MaxI) (:: 7 MaxI))
meet((:: 3 MinI) (:: 7 MinI))
join((:: false Any) (:: true Any))
meet((:: false All) (:: true All))

let [z : MaxI (bottom)]
  join(z (:: 3 MaxI))
let [t : MinI (top)]
  meet(t (:: 3 MinI))
```

### Each newtype carries exactly one lattice

| Newtype | `join` | `bottom` | `meet` | `top` |
|---|---|---|---|---|
| `MaxI` | max | smallest `int64` | -- | -- |
| `MinI` | -- | -- | min | largest `int64` |
| `Any` | `or` | `false` | -- | -- |
| `All` | -- | -- | `and` | `true` |

`MaxI` is deliberately **not** given a `meet`, nor `MinI` a `join`. A type whose
name says "maximum" has one lattice; giving it both would re-introduce exactly
the arbitrary choice the newtypes exist to avoid.

### The order comes free

Once you have a join, the partial order is not a separate thing to implement --
it falls out of the operation:

```turmeric
(lattice-leq? (:: 3 MaxI) (:: 7 MaxI))   ; => true
(lattice-leq? (:: 7 MaxI) (:: 3 MaxI))   ; => false
```
```sweet-exp
lattice-leq?((:: 3 MaxI) (:: 7 MaxI))
lattice-leq?((:: 7 MaxI) (:: 3 MaxI))
```

`(lattice-leq? x y)` is `(eq? (join x y) y)`. This is the reason a
join-semilattice is worth naming at all: a CRDT's "has this replica seen at
least that much" becomes one call rather than a hand-written comparison per
state type.

## Laws are the content

Because `Semigroup` and `JoinSemilattice` have identical signatures, the laws
are what distinguish them -- so the module ships them as runnable predicates:

| Predicate | Checks |
|---|---|
| `law-associative?` | `(combine (combine x y) z)` = `(combine x (combine y z))` |
| `law-identity?` | `mempty` is a two-sided identity |
| `law-join-associative?` | `join` associates |
| `law-join-commutative?` | `join` commutes |
| `law-join-idempotent?` | `(join x x)` = `x` |
| `law-bottom-identity?` | `(join (bottom) x)` = `x` |
| `law-meet-idempotent?` | `(meet x x)` = `x` |
| `law-top-identity?` | `(meet (top) x)` = `x` |

Each takes `^Eq A` alongside its algebraic constraint, since checking a law
means comparing results.

These **discriminate** -- a law suite that answers `true` for everything is
indistinguishable from one that does not work. An instance that combines by
subtraction fails associativity; one that is associative and commutative but
adds rather than selects fails exactly idempotence:

```turmeric
(law-associative? (:: 1 Sum) (:: 2 Sum) (:: 3 Sum))   ; => true
(law-identity? (:: 5 Product))                        ; => true
(law-join-idempotent? (:: 5 MaxI))                    ; => true
```
```sweet-exp
law-associative?((:: 1 Sum) (:: 2 Sum) (:: 3 Sum))
law-identity?((:: 5 Product))
law-join-idempotent?((:: 5 MaxI))
```

## Writing your own instance

A `defopaque` inherits nothing, so each newtype needs its own `Eq` before the
law predicates can be used on it:

```turmeric
(defopaque Longest :cstr)

(definstance Eq [Longest]
  (eq? [x y] (cstr-eq? (:: x cstr) (:: y cstr))))

(definstance Semigroup [Longest]
  (combine [x y]
    (if (< (cstr-len (:: x cstr)) (cstr-len (:: y cstr))) y x)))
```
```sweet-exp
defopaque Longest :cstr

definstance Eq [Longest]
  eq? [x y] cstr-eq?((:: x cstr) (:: y cstr))

definstance Semigroup [Longest]
  combine [x y]
    if {cstr-len((:: x cstr)) < cstr-len((:: y cstr))}
      y
      x
```

Then check yourself against the laws rather than assuming:

```turmeric
(law-associative? (:: "aa" Longest) (:: "b" Longest) (:: "cccc" Longest))
```

## `max` and `min` on `Ord`

Related, and reworked alongside this module: `max` and `min` are
**constrained generic functions over `Ord`**, defined in
`stdlib/typeclass-ord.tur`. That file is auto-loaded, so they need no `load` of
their own:

```turmeric
(defn generic-max [^Ord A] [x : A y : A] : A (max x y))

(println (max 2.5 7.1))          ; => 7.1
(println (max "alpha" "beta"))   ; => beta
(println (generic-max 2.5 7.1))  ; => 7.1
```
```sweet-exp
defn generic-max [^Ord A] [x : A y : A] : A
  max(x y)

println $ max(2.5 7.1)
println $ max("alpha" "beta")
println $ generic-max(2.5 7.1)
```

They used to be macros in `stdlib/macros.tur` that expanded to the builtin
`>=` / `<=`, which meant they worked for concrete `int` and `float` and nowhere
else -- `(max "a" "b")` and any use inside a constrained generic both failed
with an operator-lookup error. The two spellings cannot coexist, because macro
expansion precedes typeclass dispatch: a macro named `max` makes the method
unreachable everywhere, so the macros were removed.

The generic case is the one that matters: a pointwise maximum over an abstract
element type is exactly what a CRDT counter join needs. `Ord [float]` and
`Ord [cstr]` are new too -- `float32` had an instance and `float` did not, and
`String`/`StringSlice` had one while bare `cstr` did not.

## See also

- [Typeclass Guide](typeclass-guide.md) -- `defclass`, `definstance`,
  constraints, and how dispatch resolves.
- [Typeclass Dictionary Internals](typeclass-internals-guide.md) -- how an
  instance lowers to a C dictionary.
- [Arrows and Signal Processing](arrows-guide.md) -- `Category`'s `comp` is one
  of the hand-rolled combining operations this vocabulary generalizes.
- The design rationale, including why no bare-primitive instances ship, is in
  [lattice-vocabulary-plan.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/lattice-vocabulary-plan.md).
