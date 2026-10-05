---
title: Approaches to Polymorphism
category: Type System
description: A tour of the polymorphism mechanisms Turmeric provides -- parametric, ad-hoc, structural, row, kind, rank, and substructural -- with guidance on which one to reach for
---

# Approaches to Polymorphism

"Polymorphism" means "one piece of code, many shapes of input". Turmeric gives
you more than one knob for this because no single mechanism handles every case
well: writing `identity` is not the same problem as writing `print`, which is
not the same as writing `fmap`, which is not the same as writing a function
that accepts "any iterable of any element".

This guide is a map of the territory. Each section is short on purpose: it
shows the syntax, gives one representative example, says when to reach for it,
and links to a deeper guide for the rules and edge cases.

---

## At a glance

| You want to ... | Reach for | Where to read more |
|---|---|---|
| Write code that works the same for every type | Parametric polymorphism (type variables) | [type-annotations-guide.md](type-annotations-guide.md) |
| Pick a different implementation per type | Typeclasses (`defclass` / `definstance`) | this guide; see also [hkt-guide.md](hkt-guide.md) |
| Abstract over a container, not just an element | Higher-kinded types (`^f`) | [hkt-guide.md](hkt-guide.md) |
| Accept a callback that must itself stay polymorphic | Higher-ranked types (`forall` in argument position) | [hrt-guide.md](hrt-guide.md) |
| Refine a type through a pattern match | GADTs (`defgadt`) | [gadts-guide.md](gadts-guide.md), [gadts-cookbook.md](gadts-cookbook.md) |
| Accept "one of these concrete types" | Union types (`A \| B`) | [union-intersection-types-guide.md](union-intersection-types-guide.md) |
| Demand "all of these capabilities at once" | Intersection types (`A & B`) | [union-intersection-types-guide.md](union-intersection-types-guide.md) |
| Hide a concrete type behind capabilities | Existential types (`exists` / `pack` / `open`) | [existential-types-guide.md](existential-types-guide.md) |
| Be effect-polymorphic | Effect rows (`{Io \| e}`) | [effects-system-guide.md](effects-system-guide.md) |
| Branch on a tagged value | ADTs (`defdata`) | [gadts-guide.md](gadts-guide.md) |
| Track usage discipline (linear / affine / unique) | Substructural annotations | [substructural-types-guide.md](substructural-types-guide.md), [uniqueness-types-guide.md](uniqueness-types-guide.md) |
| Accept any number of same-typed values | `& rest :T` variadics | [function-arity-guide.md](function-arity-guide.md) |
| Drop type-checking at the boundary | Inline-C with `#fx{Unsafe}` | [c-integration-guide.md](c-integration-guide.md) |

The rest of this guide walks each row.

---

## Parametric polymorphism

The simplest form: write a function over a *type variable* that the compiler
fills in at each call site. The body cannot inspect the value -- it must treat
every `a` identically -- which is exactly what makes the function reusable.

```turmeric
(defn identity [x : a] : a x)

(defn map-vec [v : (vec a)  f : (-> a b)] : (vec b) ...)
```
```sweet-exp
defn identity [x : a] : a
  x

defn map-vec [v : (vec a)  f : (-> a b)] : (vec b)
  ...
```

For type constructors that themselves take parameters (`Pair`, `Option`,
`Result`), introduce the type parameters in brackets *before* the value
parameters:

```turmeric
(defstruct Pair [A B] (fst A) (snd B))

(defn pair     [A B] [a : A  b : B] : (Pair A B) (make-struct Pair a b))
(defn pair-fst [A B] [p : (Pair A B)] : A (.fst p))
```
```sweet-exp
defstruct Pair [A B] (fst A) (snd B)

defn pair     [A B] [a : A  b : B] : (Pair A B)
  make-struct(Pair a b)

defn pair-fst [A B] [p : (Pair A B)] : A
  .fst(p)
```

**Reach for it when** you can write the function once for every type with no
per-type behaviour. If you find yourself wanting to compare, print, or
serialize the value, you have crossed into typeclass territory.

---

## Typeclasses (ad-hoc polymorphism)

Typeclasses let one name (`eq?`, `fmap`, `show`) resolve to a *different*
implementation depending on the type of the argument. The class declares the
interface; each `definstance` supplies a concrete implementation. Resolution
happens at compile time -- neither virtual dispatch nor runtime tag lookup is involved.

```turmeric
(defclass Eq [a] (eq? [x y] : bool))

(definstance Eq [int]  (eq? [x y] (= x y)))
(definstance Eq [bool] (eq? [x y] (= x y)))
(definstance Eq [cstr] (eq? [x y] : bool
  ```c if (x == NULL && y == NULL) return true;
       if (x == NULL || y == NULL) return false;
       return strcmp((const char *)x, (const char *)y) == 0;
  ```))
```
```sweet-exp
defclass Eq [a]
  eq? [x y] :bool

definstance Eq [int]
  eq? [x y] {x = y}
definstance Eq [bool]
  eq? [x y] {x = y}
definstance Eq [cstr]
  eq? [x y] :bool
    ```c if (x == NULL && y == NULL) return true;
       if (x == NULL || y == NULL) return false;
       return strcmp((const char *)x, (const char *)y) == 0;
  ```
```

Instances can themselves be parametric -- "two pairs are equal when both
halves are equal":

```turmeric
(definstance Eq [Pair] [(Eq A) (Eq B)]
  (eq? [p1 p2]
    (and (eq? (.fst p1) (.fst p2))
         (eq? (.snd p1) (.snd p2)))))
```
```sweet-exp
definstance Eq [Pair] [(Eq A) (Eq B)]
  eq? [p1 p2]
    {eq?(.fst(p1) .fst(p2)) and eq?(.snd(p1) .snd(p2))}
```

**Reach for it when** the same operation has many implementations and which
one to use is determined by the argument's type. The stdlib uses this for
`Eq`, `Ord`, `Show`, `Num`, `Hash`, `Clone`, `Functor`, `Applicative`,
`Monad`, `Alternative`, and the `Category` / `Arrow` / `Kleisli` arrow
hierarchy (`stdlib/arrow.tur`, `stdlib/kleisli.tur`; see
[arrows-guide.md](arrows-guide.md)) -- see `stdlib/typeclass*.tur`.

`definstance` is **idempotent**: re-running a `definstance C [T]` (e.g. via
REPL `(reload)` or re-importing a module) replaces the singleton entry rather
than appending a duplicate. The most recent elaboration wins.

> **TUR-W0039** -- when a typeclass method shares a name with an ordinary
> `defn` in the same scope, the compiler warns at the shadowing site. Both
> bindings coexist (method dispatch goes through the dictionary; the free
> `defn` is still callable), but rename one when the clash is unintentional.
> See [typeclass-internals-guide.md](typeclass-internals-guide.md) and
> [arrows-guide.md](arrows-guide.md).

---

## Higher-kinded types

Typeclasses get more powerful once they can abstract not just over *types* but
over *type constructors*. `Functor` is the canonical case: it makes sense for
every container, not for any single element type.

The `^f` annotation marks a parameter of kind `* -> *` (a unary type
constructor). `^^f` is `* -> * -> *`.

```turmeric
(defclass Functor [^f]
  (fmap [container [fn :fn]] : int))

(defclass Monad [^m]
  (bind [ma fn] : int))
```
```sweet-exp
defclass Functor [^f]
  fmap [container [fn :fn]] :int

defclass Monad [^m]
  bind [ma fn] :int
```

**Reach for it when** the abstraction is about the *shape* of a container,
not its contents (mapping, folding, sequencing, lifting). Full rules,
performance model, and dispatch in [hkt-guide.md](hkt-guide.md).

---

## Higher-ranked types

Normally a polymorphic function is instantiated by its caller. Higher-ranked
types flip that: the *callee* receives a still-polymorphic function and
chooses how to use it. The marker is a `forall` *inside* an argument type.

```turmeric
(defn apply-poly [f : (forall [a] (-> a a))  x : int] : int
  (f x))
```
```sweet-exp
defn apply-poly [f : (forall [a] (-> a a))  x : int] : int
  f(x)
```

**Reach for it when** the function you take must work at more than one type
*within your body* -- not just at whatever type the caller picks. See
[hrt-guide.md](hrt-guide.md).

---

## ADTs and GADTs

`defdata` builds an ordinary tagged sum (`Option`, `Result`, `Tree`). Pattern
matches are exhaustiveness-checked.

```turmeric
(defdata Fix [^f] (Roll :int))
```
```sweet-exp
defdata Fix [^f]
  Roll :int
```

`defgadt` ("generalized algebraic data type") goes further: each constructor
can *constrain* the type parameter, so a match arm sees a more specific type
than the whole. This eliminates whole classes of "shouldn't happen" branches
because they truly cannot.

```turmeric
(defgadt Nat []
  (Zero          : (Nat))
  (Succ  (Nat)   : (Nat)))

(defgadt GVec [a]
  (GVNil                  : (GVec int))
  (GVCons int (GVec int)  : (GVec int)))
```
```sweet-exp
defgadt Nat []
  Zero          : (Nat)
  Succ  (Nat)   : (Nat)

defgadt GVec [a]
  GVNil                  : (GVec int)
  GVCons int (GVec int)  : (GVec int)
```

**Reach for ADTs** for everyday sum types. **Reach for GADTs** when a
constructor needs to *prove* something about its type parameter (a tagless
interpreter, a length-indexed vector, a type-safe AST). Full treatment in
[gadts-guide.md](gadts-guide.md) and the [gadts-cookbook.md](gadts-cookbook.md).

---

## Union and intersection types

Sometimes you want "any of these concrete types" without inventing a sum
constructor for each one. Union types (`A | B`) say "this is exactly one of
these". Intersection types (`A & B`) say "this satisfies all of these
constraints simultaneously".

```turmeric
(defn describe [x : (int | cstr | bool)] : cstr
  (match x
    (i : int)  "an integer"
    (s : cstr) "a string"
    (b : bool) "a boolean"))
```
```sweet-exp
defn describe [x : (int | cstr | bool)] : cstr
  match x
    (i : int)
    "an integer"
    (s : cstr)
    "a string"
    (b : bool)
    "a boolean"
```

**Reach for unions** at FFI/JSON boundaries and for gradual typing. **Reach
for intersections** when you want a value that simultaneously satisfies
several typeclass constraints. See
[union-intersection-types-guide.md](union-intersection-types-guide.md).

---

## Existential types

The dual of `forall`: instead of "the caller picks the type", the *callee*
picks the type and hides it, exposing only the capabilities the caller is
allowed to use. A pack-and-open pair gives you closed-over data plus its
operations as a single opaque value.

```turmeric
(defn pack-counter [] : (exists [a] (pair a (-> a int)))
  ...)
```
```sweet-exp
defn pack-counter [] : (exists [a] (pair a (-> a int)))
  ...
```

**Reach for it when** you want a list of "things that all support `Show`" or
"plugins that expose `run`" without committing every consumer to the same
concrete plugin type. See [existential-types-guide.md](existential-types-guide.md).

---

## Effect-row polymorphism

Effects in Turmeric are tracked as a *row* on the function type. You can be
polymorphic over the "tail" of that row -- "I add `Io`, I leave the rest of
the effects alone":

```turmeric
(defn read-file [path : cstr] : cstr @ {Io} ...)

(defn with-prefix [pfx : cstr  k : (-> cstr cstr)] : cstr @ {Ask | e}
  (str-concat pfx (k (perform (Ask)))))
```
```sweet-exp
defn read-file [path : cstr] : cstr @ {Io}
  ...

defn with-prefix [pfx : cstr  k : (-> cstr cstr)] : cstr @ {Ask | e}
  str-concat(pfx k(perform(Ask())))
```

**Reach for it when** writing higher-order combinators that must compose with
arbitrary handlers (every monad-transformer-shaped problem in Haskell becomes
this). See [effects-system-guide.md](effects-system-guide.md) and the design
rationale in [effects-vs-monads.md](effects-vs-monads.md).

---

## Substructural polymorphism (resource discipline)

Annotations on a parameter declare *how often* it may be used. The same
function body can be reused at different disciplines, and the compiler will
reject misuse.

```turmeric
(defn consume  [^linear  fh  : FileHandle] : unit ...)
(defn maybe    [^affine  buf : (vec int)]  : unit ...)
(defn log-all  [^relevant msg : str]       : unit ...)
(defn sort!    [^unique  v   : (vec int)]  : unit ...)
```
```sweet-exp
defn consume  [^linear  fh  : FileHandle] : unit ...
defn maybe    [^affine  buf : (vec int)]  : unit ...
defn log-all  [^relevant msg : str]       : unit ...
defn sort!    [^unique  v   : (vec int)]  : unit ...
```

**Reach for it when** correctness depends on *count of uses*, not just type:
file handles, sockets, single-owner buffers, session-typed channels. See
[substructural-types-guide.md](substructural-types-guide.md) and
[uniqueness-types-guide.md](uniqueness-types-guide.md).

---

## Variadic functions (`& rest :T`)

Real ad-hoc polymorphism over *arity* is rare in Turmeric: most "many
arguments" cases are better served by a struct. But for genuine
"unknown-number of same-typed values" (`println`, `format`, "make a vec of
these"), a typed rest parameter exists:

```turmeric
(defn println-all [first : cstr  & rest : cstr] : void ...)
```
```sweet-exp
defn println-all [first : cstr  & rest : cstr] : void
  ...
```

The rest type is type-checked (including opaques and user-defined types), and
calling with zero rest args passes `nil`. The full rules are in
[function-arity-guide.md](function-arity-guide.md).

**Reach for it when** the arguments are genuinely a homogeneous sequence.
For "lots of named, independent inputs", a `defstruct` options value is
almost always clearer.

---

## First-class functions

Functions are values, so any function that takes a function-typed parameter
is, in a small sense, polymorphic over behaviour. This is the original
"strategy pattern", and it composes with every other mechanism on this list.

```turmeric
(defn pair-eq?
  [A B]
  [p1 : (Pair A B)
   p2 : (Pair A B)
   fst-cmp : (-> A A bool)
   snd-cmp : (-> B B bool)]
  : bool
  (and (fst-cmp (.fst p1) (.fst p2))
       (snd-cmp (.snd p1) (.snd p2))))
```
```sweet-exp
defn pair-eq? [A B] [p1 : (Pair A B) p2 : (Pair A B) fst-cmp : (-> A A bool) snd-cmp : (-> B B bool)] : bool
  {fst-cmp(.fst(p1) .fst(p2)) and snd-cmp(.snd(p1) .snd(p2))}
```

**Reach for it when** the per-type behaviour is supplied at the *call site*,
not derived from the *type*. (When the behaviour really does follow from the
type, prefer a typeclass -- you stop threading the comparator through every
call.)

---

## The unsafe escape hatch

When no static encoding works -- because the actual operation lives on the C
side, or the cost of an indirect dispatch is unacceptable -- drop into an
inline-C block and tag the surrounding `defn` with `#fx{Unsafe}`. The compiler
stops type-checking inside the block; you become responsible for the types.

```turmeric
(defn list-sum [lst : int] #{Unsafe} : int
  ```c
  typedef struct { int64_t head; int64_t tail; } __tur_cons_cell;
  int64_t acc = 0;
  __tur_cons_cell *p = (__tur_cons_cell *)(intptr_t)lst;
  while (p) { acc += p->head; p = (__tur_cons_cell *)(intptr_t)p->tail; }
  return acc;
  ```)
```

**Reach for it sparingly**, and only after the safer mechanisms have been
ruled out for a concrete reason. See [c-integration-guide.md](c-integration-guide.md).

---

## How the mechanisms combine

The interesting designs use several at once:

- A **typeclass over an HKT** (`Functor [^f]`) is parametric polymorphism
  over the element type plus ad-hoc polymorphism over the container.
- An **existential plus typeclass constraint** (`exists [a] [(Show a)] a`)
  gives you a heterogeneous list whose elements you can still `show`.
- A **GADT-indexed function with a `forall` argument** lets a tagless
  interpreter walk a typed AST without per-arm casts.
- A **linear parameter holding a typeclass dictionary** gives you a typed
  effect handle that must be threaded exactly once.
- **Effect rows plus higher-ranked types** are what makes "polymorphic over
  the rest of the effects" work cleanly.

There is no single "right" axis. Pick the smallest one that captures the
variation you actually have, and reach for a stronger one only when the
weaker one starts requiring boilerplate at every call site.

---

## See also

- Design rationale: [advanced-type-system-rationale.md](advanced-type-system-rationale.md)
  -- why these specific mechanisms, and why dependent types were deferred
  (refinement types have since shipped; see
  [refinement-types-guide.md](refinement-types-guide.md))
- [type-annotations-guide.md](type-annotations-guide.md) -- syntax for
  writing the types this guide refers to
- [hkt-guide.md](hkt-guide.md), [hrt-guide.md](hrt-guide.md),
  [gadts-guide.md](gadts-guide.md), [existential-types-guide.md](existential-types-guide.md),
  [union-intersection-types-guide.md](union-intersection-types-guide.md) --
  per-feature deep dives
- [effects-system-guide.md](effects-system-guide.md),
  [effects-vs-monads.md](effects-vs-monads.md) -- effect rows
- [substructural-types-guide.md](substructural-types-guide.md),
  [uniqueness-types-guide.md](uniqueness-types-guide.md) -- linear / affine /
  unique disciplines
- [function-arity-guide.md](function-arity-guide.md) -- variadics,
  struct-of-options, arity limits
