# Multiple Monoids for One Type: How Other Languages Handle It

## Status

**Background note, not a plan.** `int` is a monoid four different ways
(sum, product, min, max), so any language with a `Monoid` abstraction has
to answer "which one?". Turmeric's answer is the six selection newtypes in
[stdlib/typeclass-lattice.tur](../../stdlib/typeclass-lattice.tur), documented
in [the lattice guide](../guides/lattice-guide.md#the-selection-newtypes).
This document records what everyone else does, so the next time the question
comes up nobody has to re-derive the landscape.

One concrete gap falls out of the comparison (the missing `fold-map`, section 5).
Everything else here is context.

## 1. Turmeric's answer, for reference

No instance for the bare primitive -- `Semigroup [int]` is deliberately
undefined, because shipping one would pick a winner arbitrarily and a
colliding `definstance` from outside `stdlib/` is a hard **TUR-E0025**
(reject, not replace). Instead six `defopaque` newtypes each carry exactly
one algebra, and values are made by ascription:

| Newtype | Carrier | `combine` | `mempty` |
|---|---|---|---|
| `Sum` | `:int` | addition | `0` |
| `Product` | `:int` | multiplication | `1` |
| `MinI` | `:int` | minimum | largest `int64` |
| `MaxI` | `:int` | maximum | smallest `int64` |
| `Any` | `:bool` | disjunction | `false` |
| `All` | `:bool` | conjunction | `true` |

```turmeric
(combine (:: 3 MinI) (:: 7 MinI))   ; => MinI 3
```

The rest of this document places that choice among the alternatives.

## 2. The three design families

There are only three structurally distinct answers in the wild, plus one
hybrid.

### Family A -- wrap the data; the choice lives in the value's type

**Haskell** is the origin of Turmeric's design, down to the names.
`Data.Monoid` ships `Sum`, `Product`, `All`, `Any`, `First`, `Last`, `Dual`,
`Endo`; `Data.Semigroup` adds `Min`, `Max`, and `Arg`/`ArgMin`/`ArgMax`.
Critically, Haskell also ships **no `Semigroup Int` instance** -- the same
refusal, for the same reason.

Two pieces of machinery make the wrapping cheap there, and both are the
interesting part of the comparison:

- `coerce` / `Coercible` make wrap and unwrap free and fully erased, so
  `getSum . foldMap Sum` compiles to the bare loop.
- `foldMap` lets a caller wrap **at the fold** rather than per element:
  `getSum (foldMap Sum xs)` folds a plain `[Int]` under addition without
  ever building a list of `Sum`.

`DerivingVia` (GHC 8.6+) then lifts a chosen algebra onto a user's own type:
`deriving (Semigroup, Monoid) via (Sum Int)`.

**PureScript** takes the same approach with clearer names: `Additive`,
`Multiplicative`, `Conj`, `Disj`, `Min`, `Max`.

**Rust** has no `Monoid`, but the orphan and coherence rules make
one impl per (trait, type) a hard law, so the newtype pattern is in the book
specifically as the escape hatch -- and the standard library uses it for
exactly this purpose. `std::cmp::Reverse<T>` is a newtype whose only job is
to carry the other `Ord`; `Wrapping<T>` and `Saturating<T>` carry alternate
arithmetic.

**Lean 4 / mathlib** uses `Additive a` and `Multiplicative a`: type
synonyms-as-newtypes whose sole purpose is to transport a monoid structure
between the additive and multiplicative spellings, with `@[to_additive]`
generating the mirrored theorems.

**Trade-off:** inference works (the value says which algebra it belongs to),
the type checker prevents mixing two algebras in one fold, and the cost is
zero at runtime -- but you must choose before folding, and every element
pays wrapping syntax unless the language gives you a `foldMap`.

### Family B -- pass the algebra as a value; the choice lives at the use site

**OCaml and SML functors.** A monoid is a *module*:

```ocaml
module type MONOID = sig
  type t
  val empty : t
  val combine : t -> t -> t
end

module IntAdd : MONOID = struct ... end
module IntMul : MONOID = struct ... end

module F = Fold (IntMul)
```

Nothing wraps the data at all. This is the genuinely different answer, and
the trade is sharp: there is no inference (you name the module at every use),
and the type checker will **not** stop you combining values under two
different algebras in the same fold, because the values carry no evidence of
which algebra they belong to.

**Isabelle/HOL locales** are the same shape at the proof level: one
`locale monoid`, many `interpretation`s.

**C++** does it with function objects: `std::reduce(first, last, init, op)`
with `std::plus<>` and `std::multiplies<>` as the named algebras. Those are
OCaml modules with worse syntax.

**Java** is the clearest statement of the whole axis, because it has both
halves in one library: `Comparable` is the coherent one-per-type version,
`Comparator` is the many-passed-explicitly version. `Stream.reduce(identity,
op)` and `Collector` are family B.

**Clojure, Elm, Go** -- `(reduce + 0 xs)`. Family B with no types.

### Family C -- split the class instead of the type

**Rust** again: `std::iter::Sum` and `std::iter::Product` are two *separate
traits*, each with canonical impls for the primitives. No wrapper needed at
the call site, which is the appeal. The cost is that there is no generic
`mconcat`: every algebra needs its own trait and its own fold, and a user
cannot add a seventh one that existing folds will accept.

**Swift** makes the same move with `AdditiveArithmetic`.

### The hybrid -- ambient default plus a local override

**Scala** and **Agda** can do something neither Haskell nor Turmeric can:
pass the instance **explicitly at the call site**. Cats ships `Monoid[Int]` =
addition as the ambient given, and multiplication is reachable with no
newtype at all:

```scala
xs.combineAll(using Monoid.instance(1)(_ * _))
```

Scala 3 also supports **import-scoped instance sets**: put the competing
`given`s in separate objects, and the choice becomes
`import MultiplicativeInstances.given` for a region of code. Lean has the
equivalent via `scoped instance`, `attribute [local instance]`, and `letI`.
Agda's instance arguments can be supplied positionally: `{{IntMul}}`.

**Scalaz** tried a fourth thing worth recording: **phantom tags** --
`Int @@ Tags.Multiplication`, a zero-cost type-level tag rather than a real
wrapper. Cats dropped it; the ergonomics never paid off.

## 3. Summary table

| Language | Mechanism | Family |
|---|---|---|
| Haskell | `Sum`/`Product`/`Min`/`Max`/`Any`/`All` newtypes; no `Semigroup Int` | A |
| PureScript | `Additive`/`Multiplicative`/`Conj`/`Disj` newtypes | A |
| Rust | newtype (`Reverse`, `Wrapping`) **and** split traits (`Sum`, `Product`) | A + C |
| Lean 4 | `Additive`/`Multiplicative` synonyms; also scoped instances | A + hybrid |
| Swift | `AdditiveArithmetic` and friends | C |
| OCaml / SML | functors over a `MONOID` signature | B |
| Isabelle/HOL | locales with multiple `interpretation`s | B |
| C++ | function objects passed to `std::reduce` | B |
| Java | `Comparator` (vs. `Comparable`), `Stream.reduce`, `Collector` | B |
| Clojure / Elm / Go | pass the function and the identity | B |
| Scala (cats) | ambient `given` + explicit `using` + import-scoped sets | hybrid |
| Scalaz | phantom tags (`Int @@ Tags.Multiplication`), abandoned | hybrid |
| Agda | instance arguments, suppliable positionally | hybrid |
| **Turmeric** | **six selection newtypes; no instance for bare `int`** | **A** |

## 4. Why family A is the right fit here

Family B is unavailable: Turmeric resolves instances globally and coherently,
and there is no way to pass a dictionary by hand. The hybrid is unavailable
for the same reason -- TUR-E0025 rejects a competing instance rather than
scoping it. Family C would mean a `Sum`/`Product`/`Min`/`Max` class each,
four `mconcat`s, and no way for a user to add a fifth algebra that the
existing folds accept.

So the newtype approach is not merely a Haskell transplant; it is the only
family the instance-resolution model leaves open. The guide's note that
`Semigroup [int]` stays free for a program with one canonical integer algebra
is the pressure valve.

## 5. The one real gap: no `fold-map`

The comparison surfaces exactly one thing Haskell has that Turmeric does not,
and it is not the design -- it is the ergonomics of wrapping.

`mconcat` takes a `(Vec A)` where `A` is the monoid, so every element must be
pre-ascribed and the vector must be built in the wrapped type:

```turmeric
(mconcat (vec-of (:: 1 Sum) (:: 2 Sum) (:: 3 Sum)))   ; => Sum 6
```

Haskell writes `foldMap Sum xs` over a plain list and wraps once, at the fold.
A caller who has a `(Vec int)` in hand and wants it summed must first build a
parallel `(Vec Sum)`; `fold-map` would let the wrapping happen inside the fold.

### Measured 2026-10-02 (v0.59.0, Debug, macOS arm64)

The `Semigroup` half **works today.** Written as a user-level definition with
an explicit accumulator, this compiles and runs:

```turmeric
(defn fold-map-from [^Semigroup B A]
                    [v : (Vec A) ^fat f : (fn [A] B) i : int n : int acc : B] : B
  (if (>= i n)
    acc
    (fold-map-from v f (+ i 1) n (combine acc (f (vec-get v i))))))
```

Folding `(vec-of 1 2 3 4)` through it answers `10` under `Sum`, `24` under
`Product` and `4` under `MaxI` -- three algebras over one unwrapped
`(Vec int)`, each selecting its own instance.

The `Monoid` half **does not.** Adding the `(mempty)` seed fails at the
definition site:

```turmeric
(defn fold-map [^Monoid B A] [v : (Vec A) ^fat f : (fn [A] B)] : B
  (let [unit : B
        (mempty)]
    (fold-map-from v f 0 (vec-len v) unit)))
```

```
error: no instance 'Monoid tyvar'
```

### Why -- the tyvar-reaches-a-parameter gate does not look through `(fn [A] B)`

This is the same diagnostic, and the same machinery, as
[nullary-class-method-unresolvable-over-newtype-tyvar](../archive/nullary-class-method-unresolvable-over-newtype-tyvar.md)
(resolved 2026-09-11). That fix admits a carrier-compatible opaque newtype as
the return-directed representative **only when** the constraint's tyvar
reaches some parameter of the enclosing generic -- because specializations
split on `type_eq` over the argument vector, so a tyvar that reaches no
parameter gets one spec for every instantiation and `Sum` / `Product` would
silently share an instance. The refused side is pinned by
`tests/fixtures/errors/typeclass-nullary-return-only-newtype`.

The gate is `Elab.cur_fn_constraint_param_mask`, computed in
`src/compiler/elab_fns.c:9906` with `type_mentions_named_tyvar`
(`src/compiler/elab_fns.c:6499`). That predicate handles two type kinds:

```c
case TY_TYVAR: return ... strcmp(t->as.tyvar_.name, name) == 0;
case TY_APP:   return mentions(fn) || mentions(arg);
default:       return false;
```

`TY_FN` falls into `default`. So in `fold-map`, `B` *is* in a parameter --
buried inside `(fn [A] B)` -- but the mask bit never gets set, the
representative search refuses, and `mempty` reports no instance. Isolated
against a minimal pair: `[^Monoid B] [x : B]` resolves, `[^Monoid B] [^fat f :
(fn [int] B)]` does not.

### Fix direction

Recurse `type_mentions_named_tyvar` into `TY_FN`, over
`as.fn.arg_full_types[i]` and `as.fn.result_full_type` (both non-NULL exactly
when the slot is polymorphic, `src/compiler/types.h:691`). The soundness
argument the shipped gate rests on carries over unchanged: `(fn [int] Sum)`
and `(fn [int] Product)` are distinct `Type`s, so the arg vector still splits
the spec, and Gap H's `__h<n>` discriminator separates the two identically
rendered `int64_t` signatures. `rt_type_mentions_tyvar`
(`src/compiler/elab_typeclasses.c:5696`) is a near-duplicate with the same
hole and should be checked for the same widening.

Unverified -- the recursion has not been built and measured. What is measured
is everything above it: the `Semigroup` fold, the `Monoid` failure, and the
minimal pair that localizes the cause to the function-type hole.

### Two shapes that work today, if you want this before the fix

- `fold-map-from` with an explicit seed, exactly as written above. The caller
  passes the identity instead of the compiler deriving it -- which is the
  family B answer from section 2, reached by accident.
- Give `fold-map` a redundant `B`-typed parameter so the mask bit gets set
  (measured working, answers `10` / `24`). Not worth shipping: the witness
  argument is the identity, so it is `fold-map-from` with worse ergonomics.

### Not the gap: no `coerce`

Turmeric has no `coerce`/`Coercible` equivalent, but it does not need one --
the newtypes are `defopaque` over the carrier, so ascription is already
erased and the wrapping cost is syntactic, not runtime. The missing piece
really is just `fold-map`, and within `fold-map` just the `mempty` seed.
