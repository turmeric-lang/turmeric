---
title: Typeclass Guide
category: Type System
description: Comprehensive guide to Turmeric's typeclass system -- defining typeclasses with defclass, implementing instances with definstance, constraints, associated types, functional dependencies, and default implementations.
---

# Typeclasses in Turmeric

Typeclasses are Turmeric's mechanism for **ad-hoc polymorphism** -- picking a different implementation of a function based on the type of its arguments. While parametric polymorphism (generics) treats all types identically, typeclasses let you define a single interface (`eq?`, `show`, `fmap`) and provide specialized implementations for each type.

Turmeric's typeclasses are resolved entirely at **compile time** using static dictionary passing. The compiler lowers each instance to a C dictionary struct and a global singleton pointing to the concrete implementations, resulting in zero virtual-dispatch (vtable) or runtime tag-lookup overhead.

---

## At a Glance

- **`defclass`** -- Declares a typeclass name, its type parameters, and its method signatures.
- **`definstance`** -- Implements a typeclass for a specific type or type constructor, optionally requiring constraints.
- **Superclasses** -- A `defclass` may list the classes a constraint on it entails, `(defclass Monoid [a] [(Semigroup a)] ...)`; every instance of it must then have the superclass instance too. See [Superclasses](#superclasses).
- **Idempotency** -- Re-running or reloading a `definstance` replaces the existing entry in the dispatch table, making it safe for interactive REPL-based development.
- **Stdlib classes** -- `Eq`, `Ord`, `Show`, `Hash`, `Functor`, `Monad` and friends each ship in their own `stdlib/typeclass-*.tur` file and are **auto-loaded**. The algebraic combining classes (`Semigroup`, `Monoid`, and the join/meet lattice family) are the exception: they live in `stdlib/typeclass-lattice.tur`, which you `load` explicitly -- see the [Lattice Guide](lattice-guide.md).
- **Shadowing Warnings (TUR-W0039)** -- If a typeclass method shares a name with an ordinary function (`defn`) in the same scope, the compiler emits a warning. Both coexist, but rename one if the clash is accidental.

---

## Defining Typeclasses with `defclass`

A typeclass declares an interface consisting of one or more method signatures.

```turmeric
(defclass Eq [a]
  (eq? [x y] : bool))
```
```sweet-exp
defclass Eq [a]
  eq? [x y] :bool
```

For classes with multiple methods, declare them sequentially within the `defclass` body:

```turmeric
(defclass Ord [a]
  (lt? [x y] : bool)
  (gt? [x y] : bool))
```
```sweet-exp
defclass Ord [a]
  lt? [x y] :bool
  gt? [x y] :bool
```

Typeclasses can also abstract over **type constructors** (such as `Vec`, `Option`, or user-defined containers) instead of plain types. These are called Higher-Kinded Typeclasses. The compiler uses specific annotations to distinguish their kinds:
- `[^f]` -- a unary type constructor of kind `* -> *` (e.g., `Functor`, `Monad`).
- `[^^f]` -- a binary type constructor of kind `* -> * -> *` (e.g., `Bifunctor`).

```turmeric
(defclass Functor [^f]
  (fmap [container g] : int))
```
```sweet-exp
defclass Functor [^f]
  fmap [container g] :int
```

---

## Implementing Instances with `definstance`

A `definstance` provides concrete implementations for the declared methods of a typeclass.

### Monomorphic Instances

For basic types, declare the instance directly:

```turmeric
(definstance Eq [int]
  (eq? [x y] (= x y)))
```
```sweet-exp
definstance Eq [int]
  eq? [x y] {x = y}
```

You can also use inline C blocks within instance methods to write high-performance native implementations:

```turmeric
(definstance Eq [cstr]
  (eq? [x y]
    : bool
    ```c
    if (x == NULL && y == NULL) return true;
    if (x == NULL || y == NULL) return false;
    return strcmp((const char *)x, (const char *)y) == 0;
    ```))
```
```sweet-exp
definstance Eq [cstr]
  eq? [x y] :bool
    ```c
    if (x == NULL && y == NULL) return true;
    if (x == NULL || y == NULL) return false;
    return strcmp((const char *)x, (const char *)y) == 0;
    ```
```

---

## Parametric Instances and Constraints

Often, a container type can implement a typeclass only if its elements also implement that typeclass. You can specify these dependencies using **constraints** placed in a second list inside `definstance`.

For example, two `Option` values are equal if their wrapped values are equal:

```turmeric
(definstance Eq [Option]
  [(Eq A)]
  (eq? [x y]
    (if {.is-some(x) = .is-some(y)}
      (if (.is-some x) (eq? (.value x) (.value y)) true)
      false)))
```
```sweet-exp
definstance Eq [Option]
  [(Eq A)]
  eq? [x y]
    if {(.is-some x) = (.is-some y)}
      if (.is-some x) (eq? (.value x) (.value y)) true
      false
```

Multiple constraints can be specified within the brackets:

```turmeric no-check
(definstance Eq [Result]
  [(Eq A) (Eq B)]
  (eq? [x y]
    (if {.is-ok(x) = .is-ok(y)}
      (if (.is-ok x)
        (eq? (.ok-val x) (.ok-val y))
        (eq? (.err-val x) (.err-val y)))
      false)))
```
```sweet-exp
definstance Eq [Result]
  [(Eq A) (Eq B)]
  eq? [x y]
    if {(.is-ok x) = (.is-ok y)}
      if (.is-ok x)
        eq?(.ok-val x) (.ok-val y)
        eq?(.err-val x) (.err-val y)
      false
```

### Trailing-Parameter Instance Heads (Holes)

Some container types take multiple parameters, but you may want to implement a unary typeclass like `Functor` over only one of them. Turmeric supports **holes (`_`)** to fix some parameters while leaving others free.

For instance, `Result` is a binary constructor with two parameters: `[A B]` representing the OK type and Error type. To map a function only over the OK payload (the right-biased convention), we can fix the Error type `B` and leave the OK type free:

```turmeric no-check
(definstance Functor [(Result _ B)]
  (fmap [container g]
    (if (.is-ok container)
      (ok (g (.ok-val container)))
      (err (.err-val container)))))
```
```sweet-exp
definstance Functor [(Result _ B)]
  fmap [container g]
    if (.is-ok container)
      ok(g(.ok-val container))
      err(.err-val container)
```

A method dispatched through a partially-applied head is compiled one of two
ways, and the difference is representation only, never the answer. When the
receiver's type arguments are all the same type and the body constructs its
result in-body, the call takes the by-value route (a monomorphized instance
method). Otherwise -- a heterogeneous receiver such as `(Result int cstr)`,
or a body that delegates to a helper as `Functor [(Either E)]`'s `fmap` does
-- the call stays on the uniform carrier. Either way the call's static type
is the precise applied result (`(Result int cstr)` for the example above),
so a `match` on it sees the real payload types and a `cstr` in the `Err` arm
prints as the string, not as an address.

---

## Constrained Functions

A `defn` asks for an instance with a caret constraint in its parameter
vector: `^Class` names the class, the next symbol declares the type
variable it constrains, and the parameters that follow use that variable.

```turmeric
(defn display [^Show a x : a] : void
  (println (show x)))

(defn same? [^Eq a x : a y : a] : bool
  (eq? x y))
```
```sweet-exp
defn display [^Show a x : a] : void
  println(show(x))

defn same? [^Eq a x : a y : a] : bool
  eq?(x y)
```

Bare parameters that directly follow the binder take its type, so the
annotation may be left off: `[^Show a x]` reads as `[^Show a x : a]`, and
`[^Eq a x y]` as `[^Eq a x : a y : a]`. The run ends at the first parameter
that carries its own annotation, which always wins -- `[^Show a x n : int]`
is `x : a` and `n : int`. Several constraints on one variable are written in
sequence (`[^Eq ^Show a x]`); a constructor variable (`[^f] [^Functor f
xs : (f int)]`) is never a value parameter's type and does not start a run.

Each call instantiates the variable at the argument's type and dispatches
the class methods on that type: `(display (Red))` reaches `Show [Color]`
and `(display 3)` reaches `Show [int]`, with a by-value `defdata` or
`defstruct` argument passed as the aggregate it is.

---

### Two rules the compiler enforces

**The constraint is what makes the call legal.** A body that calls a method on
its own type variable must declare the constraint. Without it there is no
instance for the call to dispatch to:

```turmeric
(defn use-foo [W] [^borrow w : W] : int
  (foo-of w))          ;; TUR-E0015: 'use-foo' does not constrain 'W' to 'Foo'
```
```sweet-exp
defn use-foo [W] [^borrow w : W] : int
  foo-of(w)          ;; TUR-E0015: 'use-foo' does not constrain 'W' to 'Foo'
```

Add `[(Foo W)]` and it resolves. This is checked at `tur check` time; an
unconstrained call used to pass the type checker and fail later in the C
compiler, naming a mangled internal symbol.

**Instance order does not matter.** A `definstance` may appear above or below
the code that dispatches on it, at file scope or inside a `defmodule`:

```turmeric
(defn use-foo [W] [(Foo W)] [^borrow w : W] : int
  (foo-of w))          ;; fine -- the instance below is found

(definstance Foo [Bar]
  (foo-of [w] (.v w)))
```
```sweet-exp
defn use-foo [W] [(Foo W)] [^borrow w : W] : int
  foo-of(w)          ;; fine -- the instance below is found

definstance Foo [Bar]
  (foo-of [w] .v(w))
```

A `defn` whose body cannot resolve a class method is elaborated speculatively,
rolled back, and retried once every form in its unit has been processed -- so
by the time it is typed for real, every instance in the file is registered.
The retry carries no capture frame, so a body that fails for some other reason
still reports its own diagnostic.

If you see `no 'Foo' instance is visible here`, the program declares no `Foo`
instance **at all** -- moving something will not help.

The `defclass` itself must still precede its instances and uses, as any
declaration must.

**One instance per class and type (TUR-E0025).** A `definstance` for a
`(class, type)` pair that already has an instance is an error, and the
autoloaded stdlib already supplies instances for the primitives (`Eq [int]`,
`Show [cstr]`, ...). This is the conventional overlapping-instance rule: there
is exactly one dictionary to dispatch to, so a second definition cannot
coexist with the first, and it is rejected rather than silently ignored (which
is what used to happen, with the stdlib's definition winning). To give a
primitive different behaviour under a class, wrap it in a newtype:

```turmeric
(definstance Eq [int]                ;; TUR-E0025: stdlib already defines Eq [int]
  (eq? [a b] : bool false))

(defopaque Loose :int)
(definstance Eq [Loose]              ;; fine: a different type
  (eq? [a b] : bool false))
(.eq? (:: 3 Loose) (:: 3 Loose))    ;; false; (.eq? 3 3) is still true
```
```sweet-exp
definstance Eq [int]                ;; TUR-E0025: stdlib already defines Eq [int]
  (eq? [a b] : bool false)

defopaque Loose :int
definstance Eq [Loose]              ;; fine: a different type
  (eq? [a b] : bool false)
.eq?((:: 3 Loose) (:: 3 Loose))    ;; false; (.eq? 3 3) is still true
```

A stdlib file loaded twice (an explicit `(load "stdlib/...")` beside the
autoload) is not a duplicate in this sense: the same definition arriving through
two load paths stays a silent no-op.

## Superclasses

A `defclass` may declare that a constraint on it **entails** other classes.
The declaration is a constraint vector right after the type-parameter vector
-- character for character the `[(Class var)]` form `definstance` and `defn`
already use, in the position `defn` puts it:

```turmeric
(defclass Semigroup [a]
  (combine [x : a y : a] : a))

(defclass Monoid [a]
  [(Semigroup a)]
  (mempty [] : a))
```
```sweet-exp
defclass Semigroup [a]
  combine [x : a y : a] :a

defclass Monoid [a]
  [(Semigroup a)]
  mempty [] :a
```

Two things follow from that one line, and they land together.

**Entailment.** A body constrained by the subclass may call the superclass's
methods. `[^Monoid A]` licenses `combine`; there is no need to also write
`^Semigroup A`. It is transitive (`C` listing `B` listing `A` lets a `C`
constraint reach `A`'s methods) and works for return-directed methods
(`mempty`-shaped, resolved from the expected type) as well as receiver
methods:

```turmeric
(defn double-up [^Monoid A] [x : A] : A
  (combine x x))

(defn fold3 [^Monoid A] [x : A y : A z : A] : A
  (let [e : A (mempty)]
    (combine (combine (combine e x) y) z)))
```
```sweet-exp
defn double-up [^Monoid A] [x : A] :A
  combine(x x)

defn fold3 [^Monoid A] [x : A y : A z : A] :A
  let [e : A mempty()]
    combine(combine(combine(e x) y) z)
```

**The instance obligation.** `(definstance Monoid [int] ...)` requires a
`Semigroup [int]` instance to exist somewhere in the program, and is
`TUR-E0393` otherwise. This is what makes the entailment sound: without it a
`[^Monoid A]` body could call `combine` at a type that has no instance to
dispatch to. The superclass instance is *required*, never inherited -- the
`Monoid` instance does not supply `combine`, the `Semigroup` one does. A
parametric instance discharges the obligation against its own constraints:
`Monoid [(Option A)] [(Monoid A)]` needs a `Semigroup [(Option A)]`, which
may itself be parametric on `(Semigroup A)`.

Both checks run after every form in the unit is registered, so declaration
order between the two classes does not matter and the superclass instance may
sit above or below the subclass instance.

Several superclasses go in the same vector, and a multi-parameter superclass
names several of the class's variables:

```turmeric
(defclass BoundedJoinSemilattice [a]
  [(JoinSemilattice a) (Eq a)]
  (bottom [] : a))

(defclass Sized [s e]
  [(Get2 s e)]
  (sz-len [^borrow self : s v : e] : int))
```
```sweet-exp
defclass BoundedJoinSemilattice [a]
  [(JoinSemilattice a) (Eq a)]
  bottom [] :a

defclass Sized [s e]
  [(Get2 s e)]
  sz-len [^borrow self : s v : e] :int
```

The fundep clause follows the vector (fundeps are written in s-expression
syntax, as in [Functional Dependencies](#functional-dependencies)):

```turmeric no-check
(defclass Coll [c e]
  [(Semigroup c)]
  | (c -> e)
  (cget [^borrow x : c i : int] : e))
```
```sweet-exp
defclass Coll [c e]
  [(Semigroup c)]
  | (c -> e)
  (cget [^borrow x : c i : int] : e)
```

The rules the compiler enforces, each with its own code:

- Every variable in an element must be one of the class's own type
  parameters, and every element must be `(Class var...)` -- `TUR-E0390`.
  An empty vector is `TUR-E0390` too: list at least one class, or drop the
  vector.
- The vector goes *before* the `|` clause; the other order is `TUR-E0390`
  naming the canonical order, not a silent accept.
- Each superclass must be a defined class whose parameter count and kinds fit
  the variables applied to it (`[(Functor a)]` over a kind-`*` `a` is
  refused) -- `TUR-E0391`.
- The graph must be acyclic; a class may not list itself, directly or
  through others -- `TUR-E0392`, naming the cycle.

A constraint on a subclass behaves exactly as if its superclasses were written
out too: `[^Alternative F]` is read as `[^Alternative F ^Applicative F]`. A
statically resolved call is unchanged, since dispatch still finds the instance
from the concrete type. A generic compiled by dictionary passing, or run by the
interpreter, receives a dictionary for each implied superclass as well, which
is what lets a return-directed method such as `pure` resolve under
`[^Alternative F]`. The same holds for a constrained rank-2 `forall`: its
`[(Applicative m)]` implies `(Functor m)` too, so a function passed to it lines
up dictionary for dictionary. The instance obligation guarantees each of those
dictionaries exists.

The stdlib uses the preamble where the relation is real:

| Class | Superclass | File |
|---|---|---|
| `Ord` | `Eq` | `typeclass-ord.tur` (auto-loaded) |
| `Applicative` | `Functor` | `typeclass-applicative.tur` (auto-loaded) |
| `Monad` | `Applicative` | `typeclass-monad.tur` (auto-loaded) |
| `Alternative` | `Applicative` | `typeclass-alternative.tur` (auto-loaded) |
| `MonadError` | `Monad` | `typeclass-monaderror.tur` (auto-loaded) |
| `Traversable` | `Functor`, `Foldable` | `typeclass.tur` |
| `Arrow`, `ArrowZero` | `Category` | `arrow.tur` |
| `ArrowChoice`, `ArrowLoop`, `ArrowApply` | `Arrow` | `arrow.tur` |
| `ArrowPlus` | `ArrowZero` | `arrow.tur` |
| `Monoid` | `Semigroup` | `typeclass-lattice.tur` |
| `BoundedJoin` | `JoinSemilattice` | `typeclass-lattice.tur` |
| `BoundedMeet` | `MeetSemilattice` | `typeclass-lattice.tur` |

So a `[^Ord A]` function may call `eq?`, a `[^Monad M]` function may call
`pure` and `fmap`, and `mconcat` carries one constraint rather than two (see
the [lattice guide](lattice-guide.md)). A class with no natural superclass --
`Eq`, `Functor`, `Hash`, `Show`, `Clone`, `Drop`, `Bifunctor`, `Num` -- stays
flat. Adding a preamble to an existing class obliges every existing instance of
it, in every downstream spice, to carry the superclass instance, which is why
each of these was retrofitted and audited separately.

## Associated Types

An **Associated Type** allows a typeclass to declare a placeholder type member using `(type Name : Type)`. Each concrete instance binds this member to a specific type with `(type Name = <type>)`.

This is incredibly useful for container/collection typeclasses where the container itself determines the element type.

### Declaring and Binds

In a `defclass` body, declare the associated type with `: Type`:

```turmeric no-check
(defclass Container [t]
  (type Elem : Type)
  (make-empty [self : t] : int))
```
```sweet-exp
defclass Container [t]
  type Elem :Type
  make-empty [self : t] :int
```

In a `definstance` body, bind it to a concrete type:

```turmeric no-check
(definstance Container [(Vec int)]
  (type Elem = int)
  (make-empty [self] 0))

(definstance Container [(Vec cstr)]
  (type Elem = cstr)
  (make-empty [self] 0))
```
```sweet-exp
definstance Container [(Vec int)]
  type Elem = int
  make-empty [self] 0

definstance Container [(Vec cstr)]
  type Elem = cstr
  make-empty [self] 0
```

### Type-Level Projections

You can project associated types in type annotations using the syntax `(AssociatedTypeName TypeArgument)`. At compile time, the projection resolves to the precise type bound by the matching instance:

```turmeric no-check
;; (Elem (Vec int)) reduces to int; (Elem (Vec cstr)) reduces to cstr.
(defn take-elem-int [x : (Elem (Vec int))] : int
  {x + 1})

(defn take-elem-cstr [x : (Elem (Vec cstr))] : cstr
  x)
```
```sweet-exp
;; (Elem (Vec int)) reduces to int; (Elem (Vec cstr)) reduces to cstr.
defn take-elem-int [x : (Elem (Vec int))] : int
  {x + 1}

defn take-elem-cstr [x : (Elem (Vec cstr))] : cstr
  x
```

If a class takes multiple type parameters, project across all of them:

```turmeric no-check
(defn bump-acc-int [x : (Acc (Vec int) int)] : int
  (+ x 1))
```
```sweet-exp
defn bump-acc-int [x : (Acc (Vec int) int)] : int
  {x + 1}
```

### Method Signature Propagation

When a typeclass method uses an associated type in its signature (either as a parameter or return type), the concrete instance methods **inherit those types under the instance substitution**. This means you do not have to repeat the annotations in the `definstance` methods:

```turmeric no-check
(defclass StorageOps [S]
  (type Elem : Type)
  (sop-get [^borrow s : S idx : int] : Elem))

;; The compiler automatically knows that `sop-get` returns `Pos`
(definstance StorageOps [(Dense Pos)]
  (type Elem = Pos)
  (sop-get [s idx] (dense-get s idx)))
```
```sweet-exp
defclass StorageOps [S]
  type Elem :Type
  sop-get [^borrow s : S idx : int] :Elem

;; The compiler automatically knows that `sop-get` returns `Pos`
definstance StorageOps [(Dense Pos)]
  type Elem = Pos
  sop-get [s idx] (dense-get s idx)
```

---

## Functional Dependencies

For multi-parameter typeclasses, you can declare **Functional Dependencies** (fundeps) using the `| (a -> b)` syntax. This declares that knowing the type parameter `a` uniquely determines the type parameter `b`.

This helps the compiler infer type variables during method resolution and enables **Return-Only Dispatch** -- resolving a dispatch type parameter even if it only appears in the return type.

```turmeric no-check
;; s (storage backend) uniquely determines e (element type)
(defclass StorageOps [s e] | (s -> e)
  (sop-get [^borrow self : s idx : int] : e))
```
```sweet-exp
;; s (storage backend) uniquely determines e (element type)
defclass StorageOps [s e] | (s -> e)
  sop-get [^borrow self : s idx : int] :e
```

When implementing instances of a class with functional dependencies, the compiler enforces **coherence and uniqueness**:

```turmeric no-check
(defopaque Box [A] : int)

(defclass Collect [c e] | (c -> e)
  (cinsert [self : c x : e] : c))

;; This is valid
(definstance Collect [(Box int) int]
  (cinsert [self x] self))

;; ERROR: This violates the functional dependency!
;; (Box int) has already been pinned to determine `int`, not `cstr`.
(definstance Collect [(Box int) cstr]
  (cinsert [self x] self))
```
```sweet-exp
defopaque Box [A] : int

defclass Collect [c e] | (c -> e)
  cinsert [self : c x : e] : c

;; This is valid
definstance Collect [(Box int) int]
  cinsert [self x] self

;; ERROR: This violates the functional dependency!
;; (Box int) has already been pinned to determine `int`, not `cstr`.
definstance Collect [(Box int) cstr]
  cinsert [self x] self
```

---

## Default Method Implementations

Typeclasses can define default implementations for their methods. If a `definstance` does not provide an explicit implementation for a method, it falls back to the default body declared in the `defclass`.

To declare a default body, simply provide forms after the return type in the `defclass` method signature:

```turmeric
(defclass Ord [a]
  (lt? [x y] : bool)
  (lte? [x y] : bool (or (.lt? x y) (= x y))))
```
```sweet-exp
defclass Ord [a]
  lt? [x y] :bool
  lte? [x y] :bool
    {.lt?(x y) or {x = y}}
```

When implementing the instance, we can choose to implement `lt?` and completely omit `lte?`, inheriting the default logic:

```turmeric
(definstance Ord [int]
  (lt? [x y] (< x y)))
```
```sweet-exp
definstance Ord [int]
  lt? [x y] {x < y}
```

---

## Under the Hood: Low-Level Dictionary Passing

For those curious about how this lowers, every `definstance Class [TypeArgs]` undergoes several compilation phases (managed in `src/compiler/elab_typeclasses.c`):

1. **Dictionary Struct lowering**: The compiler generates a C structure type `dict_<Class>_<TypeArgs>` representing the class dictionary. The struct fields are function pointers mirroring each method's return type and parameter C types.
2. **Global Singleton emission**: A global singleton `dict_<Class>_<TypeArgs>_singleton` is initialized. Its fields point to the local instance-level implementations (`__inst_<Class>_<Method>_<TypeArgs>`).
3. **Dispatch Resolution**: When calling a typeclass method, the compiler locates the appropriate dictionary singleton at compile time and emits a standard direct/indirect function pointer call (`EX_DICT`).

For a deep dive into return overriding, the closure-handle convention, and C types resolution, please see [Typeclass Dictionary Internals](typeclass-internals-guide.md).

---

## Related Guides

- [Semigroups, Monoids, and Lattices](lattice-guide.md) -- the algebraic
  combining vocabulary in `stdlib/typeclass-lattice.tur`, the selection
  newtypes that carry each algebra, and the runnable law predicates. Also
  covers `max` / `min`, which are now constrained generics over `Ord` rather
  than macros.
- [Typeclass Dictionary Internals](typeclass-internals-guide.md) -- how an
  instance lowers to a C dictionary struct and singleton.
- [Approaches to Polymorphism](polymorphism-guide.md) -- where ad-hoc
  polymorphism sits among the other mechanisms.
