# Typeclasses vs. Modular Implicits

## Status

**Background note, not a plan.** OCaml is building a typeclass-shaped
mechanism out of its module system: *modular explicits* (merged; shipping in
5.5.0) and *modular implicits* (the sequel, years out). The question "should
Turmeric have done that instead?" is reasonable enough to deserve a written
answer, so this records the comparison while the OCaml design is still being
argued in public.

Written 2026-10-02 against
[ocaml/ocaml discussion #13274](https://github.com/ocaml/ocaml/discussions/13274)
and Turmeric v0.59.0. One actionable item falls out (section 8); everything
else is context.

This is the resolution-mechanism half of a question whose other half --
"what do you do when one type has four valid instances?" -- is already
answered in [multiple-monoids-across-languages.md](multiple-monoids-across-languages.md).
That document's section 4 concludes that Turmeric's model forecloses both
family B (pass the algebra as a value) and the hybrid (ambient default plus
local override). Modular implicits is precisely OCaml moving from family B
into the hybrid, so the two documents are the same argument from opposite
ends.

## 1. The surface shapes are nearly identical

This is the first surprise, and it is worth getting out of the way before
the differences. Turmeric already writes the constraint **in the parameter
vector**, not as a Haskell-style `=>` prefix:

```turmeric
(defn display [^Show a x : a] : void
  (println (show x)))
```

Proposal 3 in the discussion, which proposal 4 (the favored hybrid) folds in:

```ocaml
let display (implicit S : Show) x = S.show x
```

Same slot, same position, inferred the same way at the call site. The whole
difference is **what is allowed to fill that slot, and who may create
fillers.**

## 2. Inference over an existing language vs. a bespoke one

| | Turmeric typeclasses | Modular implicits |
|---|---|---|
| What gets inferred | A compiler-synthesized dictionary -- `dict_Show_int_singleton`, a C struct of function pointers. Not a value you can write, name, or pass. | An ordinary OCaml module. It already exists in the language; the elaborator finds it. |
| Can you pass it by hand? | No. There is no `(display @MyShowInstance x)`. | Yes. That is the point, and it is why modular *explicits* shipped first. |
| Abstraction language | `defclass`, plus associated types, plus fundeps, plus `^f` kind annotations -- four mechanisms. | One signature language you already had: `sig type t ... end`, `with type`, sharing constraints, nesting. |
| Where candidates come from | Nowhere else. Classes exist only to be inferred. | Functors, `include`, first-class module values -- the whole module calculus feeds it. |

The deeper point: Turmeric has **no module language** to make implicit.
`defmodule` is namespacing plus C symbol mangling (see
[module-system-guide.md](../guides/module-system-guide.md)) -- no signatures,
no functors, no first-class modules. So the two designs are not competing
implementations of one idea. Modular implicits is parasitic on ML modules;
typeclasses are what you build when you do not have them.

Anyone proposing modular implicits for Turmeric is really proposing an ML
module system first, and the typeclass layer second.

## 3. Coherence is the real axis

This is where the two diverge irreconcilably.

**Turmeric is coherent by construction.** `TUR-E0025`: one instance per
`(class, type)` pair, program-wide, reject rather than replace. Instances are
globally registered and order-independent -- a `defn` whose body cannot
resolve a method is speculatively elaborated, rolled back, and retried after
every form in the unit is registered, so an instance may sit above or below
its use. Orphans are restricted: the instance must live in the module owning
either the class or one of the type arguments. The escape hatch for a second
algebra is a newtype:

```turmeric
(defopaque Loose :int)
(definstance Eq [Loose] (eq? [a b] : bool false))
```

**Modular implicits is deliberately not coherent.** Candidates are the
implicit modules in lexical scope, so `open` changes what resolves. gasche's
position in the thread is explicit about the consequence:

> you want to be able to explicitly disambiguate by forcing a specific solution

which is why so much of the syntax debate is about **labeling** implicit
parameters. Three of the four proposals exist mainly to make disambiguation
writable. Non-coherence is not an oversight in that design; it is the feature,
and the labels are the tax.

The trade is concrete and not universally in anyone's favor:

- Turmeric gets the property that makes abstract containers sound. The
  dictionary is a *function of the type*, so a `Set a` built under one `Ord`
  can never be merged with a `Set a` built under another.
  [ecs-vs-haskell-ecs.md](../guides/ecs-vs-haskell-ecs.md) credits exactly
  this for the ECS `Storage T` design: there is no orphan-instance surface
  because the elaborator rejects orphan `Component T` instances outright.
- Modular implicits gets two orderings on `int` with no newtype tax, local
  instance choice, and -- the part no newtype can replicate -- instances for
  types you do not own, assembled by a functor at the use site.

### The honest wrinkle on our side

Coherence is the design intent, and the kind-erased lookup path carries a
pragmatic tiebreak rather than an error: when a method call is ambiguous
between a user instance and a stdlib one, the user instance wins
(`src/compiler/elab_typeclasses.c:7981`, the `user_fallback_count` path).
Associated-type projection avoids the issue entirely by using exact N-ary
matching (`typeclass_env_lookup_instance_exact`) instead of the kind-erased
`typeclass_env_lookup_instance`.

Worth knowing before claiming the model is coherent without qualification.

## 4. Search power runs the other way from what you would expect

Turmeric already does real constraint solving:

- recursive resolution through parametric instance constraints
  (`Eq [Option]` requiring `[(Eq A)]`),
- superclass entailment (`[^Monad M]` licenses `fmap` and `pure`), with the
  instance obligation checked as `TUR-E0393` and the class graph checked
  acyclic as `TUR-E0392`,
- functional dependencies for return-directed dispatch (`| (s -> e)`),
- partially-applied instance heads with holes (`(Result _ B)`),
- associated-type projection.

The discussion proposes modular implicits v1 be **weaker than that on
purpose**. lpw25:

> It enables doing an initial version without needing to define unification
> on module expressions, where searches can only be keyed on types.

And notably the thread contains **no discussion of backtracking, recursive
resolution, or termination** -- those are unresolved. That is not an
oversight either: a Haskell-style solver over module expressions is a harder
search problem than over type constructors, precisely because the candidate
space is richer.

So Turmeric's search is more capable today; modular implicits' *instances*
are more capable in principle, and the search is the part nobody has designed
yet.

## 5. Where ML modules are plainly better: higher kinds

Turmeric needs explicit kind annotations (`[^f]` for `* -> *`, `[^^f]` for
`* -> * -> *`) and hole syntax to partially apply a binary constructor:

```turmeric
(definstance Functor [(Result _ B)]
  (fmap [container g] ...))
```

A module signature just writes `type 'a t`, and `type 'a t = ('a, b) result`
instantiates it. No kind annotation, no holes, no partial-application
machinery, because `type 'a t` is already a type function.

This is the one axis where the modules approach is structurally simpler
rather than merely different, and it is worth conceding plainly. See
[hkt-guide.md](../guides/hkt-guide.md) for what the `^f` machinery buys in
exchange.

## 6. Cost model

Turmeric resolves everything at compile time to a static C global; dispatch
is a direct or indirect call through a function-pointer field, and the
by-value HKT path often monomorphizes outright. No runtime search, no
allocation. See
[typeclass-internals-guide.md](../guides/typeclass-internals-guide.md).

OCaml first-class modules are runtime blocks, so an inferred module argument
is in general a real value passed at runtime; flambda may specialize it away,
but that is an optimization rather than the baseline. Not a disaster, and not
the same as a `static const` dictionary either.

Turmeric's *dynamic* dispatch story is a separate mechanism entirely:
`pack` / `open` existentials bundle a value with a witness vtable, and
dispatch inside `open` goes through the packed witness rather than static
instance search (see
[existential-types-guide.md](../guides/existential-types-guide.md#heterogeneous-dispatch-through-the-packed-witness)).
That is the closest Turmeric gets to "a dictionary as a value", and the
difference matters: it is a witness packaged *with its data*, not an instance
you can select.

## 7. Status of the OCaml work

As of 2026-10-02:

- **Modular explicits: merged.** Documentation landed February 2026; OCaml
  5.5.0 reached beta in April 2026 carrying it. A function can take a module
  as an explicit argument and let later parameter and return types depend on
  it, without full functor syntax.
- **Modular implicits: proposed, not designed.** Explicitly staged behind
  explicits, with the resolution semantics (search, ambiguity, recursion,
  termination) still open. Discussion #13274 is a *syntax* thread; it settles
  how to spell the feature, not how it resolves.

Treat any comparison to shipped modular implicits as a comparison to a
proposal.

## 8. What is worth stealing, and what is not

**Worth stealing, narrowly: named disambiguation at a call site.** The ability
to say "this instance, here" without abandoning global uniqueness as the
default. That would relieve the newtype tax that
[multiple-monoids-across-languages.md](multiple-monoids-across-languages.md)
section 4 concludes we are stuck with, without touching the coherence
guarantee. Scala's
`using Monoid.instance(...)`, Agda's `{{IntMul}}`, and Lean's `letI` are all
this shape, and all three keep an ambient default. Nothing about TUR-E0025
requires that an explicitly named override be illegal -- the rule exists to
stop two *ambient* instances from competing.

**Not worth adopting: scoped, incoherent instance sets.** Import-scoped
candidate sets would break the `Set a` soundness argument in section 3 and
the ECS `Storage T` design that depends on it. The coherence guarantee is
load-bearing in shipped code, not a stylistic preference.

**Not actionable at all: modular implicits as such.** It requires an ML module
system Turmeric does not have and is not going to grow on the way to v1.

## Summary

Modular implicits is **inference retrofitted onto an abstraction mechanism
that already exists**; Turmeric typeclasses are **an abstraction mechanism
that exists only to be inferred**. Everything else follows. Modular implicits
gets first-classness, local selection, functor-built instances, and natural
higher kinds, and pays with incoherence plus an unsolved search design.
Turmeric gets global uniqueness, a static zero-cost dictionary, and a working
solver, and pays the newtype tax plus a bespoke mechanism per feature.

## See also

- [typeclass-guide.md](../guides/typeclass-guide.md) -- `defclass`,
  `definstance`, constraints, superclasses, fundeps, associated types.
- [typeclass-internals-guide.md](../guides/typeclass-internals-guide.md) --
  how an instance lowers to a C dictionary struct and singleton.
- [polymorphism-guide.md](../guides/polymorphism-guide.md) -- where ad-hoc
  polymorphism sits among the other mechanisms.
- [multiple-monoids-across-languages.md](multiple-monoids-across-languages.md)
  -- the one-type-many-instances half of the same question.

## Sources

- [ocaml/ocaml discussion #13274](https://github.com/ocaml/ocaml/discussions/13274)
  -- the four syntax proposals, and the resolution remarks quoted above.
- [manual: document modular explicits (PR #14048)](https://github.com/ocaml/ocaml/pull/14048)
- [OCaml Platform Newsletter, February to May 2026](https://ocaml.org/news/platform-2026-05)
  -- 5.5.0 beta contents.
- [Modular Implicits, White, Bour and Yallop (ML Workshop 2014)](https://www.cl.cam.ac.uk/~jdy22/papers/modular-implicits.pdf)
  -- the original design the discussion is revising.
- [On the design and implementation of Modular Explicits, Vivien et al.](https://gallium.inria.fr/~remy/ocamod/modular-explicits.pdf)
