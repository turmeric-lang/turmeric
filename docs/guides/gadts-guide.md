---
title: GADTs -- A Practical Guide
category: Advanced Types
description: GADTs: refined constructors (`defdata` with `: Result`), type refinement, equality witnesses, union types, gradual typing
tags:
  - data-types
  - polymorphism
---

# GADTs in Turmeric -- A Practical Guide

Generalized Algebraic Data Types (GADTs) extend ordinary sum types by letting
each constructor specialize the type parameters it returns. The type-checker
learns those specializations when pattern-matching, so programs that previously
needed runtime tags or unsafe casts become statically verified. In Turmeric a
GADT is an ordinary `defdata` whose constructors declare their own result
type; there is no separate form for it.

This guide walks from plain sum types up to GADTs and equality witnesses. All
examples are valid Turmeric source. GADT support is **enabled by default** --
no flag is required. (The historical `-Xgadt` flag is still accepted as a
deprecated no-op for source compatibility; see
[compiler-flags-guide.md](compiler-flags-guide.md#-xgadt----generalized-algebraic-data-types).)

---

## Plain ADTs First (`defdata` + `match`)

Before reaching for GADTs, get comfortable with plain sum types. Turmeric's
`defdata` form declares a sum type; `match` dispatches on its constructors.

```turmeric
(defdata ^copy Color (Red) (Green) (Blue))

(defn color-to-int [c] : int
  (match c
    (Red)   1
    (Green) 2
    (Blue)  3))
```

```sweet-exp
defdata ^copy Color (Red) (Green) (Blue)

defn color-to-int [c] : int
  match c
    (Red)
    1
    (Green)
    2
    (Blue)
    3
```

Parameterized ADTs work the same way:

```turmeric
(defdata Option [a]
  (None)
  (Some a))

(defn option-or [opt default] : int
  (match opt
    (None)    default
    (Some x)  x))
```

```sweet-exp
defdata Option [a]
  (None)
  (Some a)

defn option-or [opt default] : int
  match opt
    (None)
    default
    (Some x)
    x
```

The `^copy` modifier makes the type copyable (like `defstruct ^copy`). Without
it the value follows move semantics.

Constructors can carry typed fields:

```turmeric
(defdata Shape
  (Circle int)
  (Rect   int int))

(defn area [s] : int
  (match s
    (Circle r)   (* r r)
    (Rect w h)   (* w h)))
```

```sweet-exp
defdata Shape
  (Circle int)
  (Rect   int int)

defn area [s] : int
  match s
    (Circle r)
    {r * r}
    (Rect w h)
    {w * h}
```

Pattern matching is exhaustiveness-checked. If you omit a constructor the
compiler reports the missing arm.

---

## Refined Constructors (`defdata` with `: Result`)

A `defdata` constructor may end with `: (Name ...)`, the type it returns. An
unannotated constructor returns the generic instantiation `(Name a ...)`, as
in a plain sum (see "Mixing annotated and unannotated constructors" below):

```turmeric
(defdata Expr [a]
  (Lit int                   : (Expr int))
  (Add (Expr int) (Expr int) : (Expr int)))
```

```sweet-exp
defdata Expr [a]
  (Lit int                   : (Expr int))
  (Add (Expr int) (Expr int) : (Expr int))
```

Each constructor line reads as: "field types ... `: return type`". Here:

- `(Lit int : (Expr int))` -- `Lit` takes one `int` field and returns `(Expr int)`.
- `(Add (Expr int) (Expr int) : (Expr int))` -- `Add` takes two `(Expr int)`
  fields and also returns `(Expr int)`.

Evaluation is written the same as for a plain ADT:

```turmeric
(defn eval-expr [e] : int
  (match e
    (Lit n)   n
    (Add l r) (+ (eval-expr l) (eval-expr r))))
```

```sweet-exp
defn eval-expr [e] : int
  match e
    (Lit n)
    n
    (Add l r)
    {eval-expr(l) + eval-expr(r)}
```

The annotation must apply the type being defined to exactly as many
arguments as it has parameters: `(Lit int : (Option int))` and, for a
two-parameter type, `(IntPair int int : (Pair2 int))` are errors. A record
constructor takes it after the field vector:
`(Rect [w : int h : int] : (Shape int))`.

Older code spells this `defgadt`. That keyword is **deprecated**: it is
`defdata` with every constructor required to carry an annotation, it still
compiles, and it emits `TUR-D0005` (an error under `--Werror=deprecated`;
`tur explain TUR-D0005` shows the rewrite). It will be removed in a future
major release. The rewrite is the keyword alone -- `(defgadt Expr [a] ...)`
becomes `(defdata Expr [a] ...)` with the constructors unchanged.

GADT support is on by default, so no flag is needed. (Passing the legacy
`-Xgadt` flag still works but prints a deprecation notice.)

### Refined or not is read from the annotations

Annotating a constructor does not by itself make a type a GADT. Both
constructors of the `Expr` above return `(Expr int)`, so it refines
nothing: it is an ordinary sum whose parameter is pinned to `int`. A type is
**not refined** when every constructor returns one shared instantiation
whose arguments are each the parameter in its own position or a closed type
-- `(Expr int)` from every constructor, or a generic `(Box a)`. It is
**refined** when the constructors say something different about the
parameters: `Refl : (Equal a a)` (its second argument is not `b`), or
`Lit : (Expr int)` beside `IsZero : (Expr bool)`.

The difference is representational. A refined type stays on the uniform
representation -- a boxed value carried as an int64 word -- because a field
of type `a` has a different concrete type per arm. An unrefined one is an
ordinary sum and can go by value like any `defdata`. What a refined type
gets from the checker -- per-arm type refinement, the skolem-escape check,
exhaustiveness by index, and the index checks at calls, ascriptions and
returns -- is what a `defgadt` always got.

### Head keywords

The head takes the same attributes as any `defdata` -- `^copy`, `^move`,
`^linear`, `^affine`, `^heap` and `^no-auto-ctor` -- written before the name,
ahead of the type-parameter vector: `(defdata ^copy Tag [a] ...)`. (`^linear`
with `^affine` is an error, and so is `^copy` with `^move`. The older trailing
`:copy` keyword, before or after the vector, is still read.) Without `^copy`, values follow move semantics (using one twice is a
`TUR-E0005` use-after-move); with it, they are plain, freely-readable values
-- use it when the constructors carry only copyable payloads:

```turmeric
(defdata ^copy Tag [a]
  (IntTag  : (Tag int))
  (BoolTag : (Tag bool)))
```

`stdlib/range.tur`'s `Bound` used to be the example here, spelled
`(defgadt ^copy Bound [A] ...)` with every constructor returning
`(Bound int)`. Those annotations refined nothing -- the parameter was pinned
to `int` and never used -- so `Bound` is now a plain `^copy` sum with no
parameter, and goes by value:

```turmeric
(defdata ^copy Bound
  (Inclusive int)
  (Exclusive int)
  (Unbounded))
```

`(Bound int)` no longer names a type; write `Bound`.

### Mixing annotated and unannotated constructors

`defdata` takes the annotation per constructor, so a type can refine only
where it needs to:

```turmeric
(defdata Expr [a]
  (Lit int            : (Expr int))
  (BLit bool          : (Expr bool))
  (IsZero (Expr int)  : (Expr bool))
  (If (Expr bool) (Expr a) (Expr a)))      ; (Expr a)
```

This `Expr` is refined: `Lit` and `IsZero` return different instantiations.
`(If (IsZero (Lit 0)) (Lit 7) (Lit 9))` is an `(Expr int)`: the unannotated
`If` takes its index from its branches. Record constructors, `^heap`,
`^linear` and `^affine` work on such a type exactly as on any `defdata` --
`(Rect [w : int h : int] : (Shape int))`.

---

## Type Refinement in Match Arms

The power of GADTs shows up when constructors specialize different type
parameters. Consider a typed tag:

```turmeric
(defdata Tag [a]
  (IntTag  : (Tag int))
  (BoolTag : (Tag bool)))
```

```sweet-exp
defdata Tag [a]
  (IntTag  : (Tag int))
  (BoolTag : (Tag bool))
```

A function dispatching on the tag can return `a` without a cast:

```turmeric
(defn default-value [t] : int
  (match t
    (IntTag)  0
    (BoolTag) 0))
```

```sweet-exp
defn default-value [t] : int
  match t
    (IntTag)
    0
    (BoolTag)
    0
```

In the `IntTag` arm the type-checker knows `a = int`; in `BoolTag` it knows
`a = bool`. These *skolem equalities* are introduced automatically per arm and
are invisible to the programmer. No casts, no `option` wrapper, and no runtime tags.

A larger example combining a plain sum and an annotated one in one program:

```turmeric
(defdata ^copy Color (Red) (Green) (Blue))

(defdata Expr [a]
  (Lit int         : (Expr int))
  (Add (Expr int) (Expr int) : (Expr int)))

(defn color-to-int [c] : int
  (match c
    (Red)   1
    (Green) 2
    (Blue)  3))

(defn eval-expr [e] : int
  (match e
    (Lit n)   n
    (Add l r) (+ (eval-expr l) (eval-expr r))))

(defn main [] : int
  (println (color-to-int (Red)))            ; 1
  (println (eval-expr (Add (Lit 10) (Lit 20))))  ; 30
  (println (color-to-int (Green)))          ; 2
  0)
```

```sweet-exp
defdata ^copy Color (Red) (Green) (Blue)

defdata Expr [a]
  (Lit int         : (Expr int))
  (Add (Expr int) (Expr int) : (Expr int))

defn color-to-int [c] : int
  match c
    (Red)
    1
    (Green)
    2
    (Blue)
    3

defn eval-expr [e] : int
  match e
    (Lit n)
    n
    (Add l r)
    {eval-expr(l) + eval-expr(r)}

defn main [] : int
  println(color-to-int((Red)))
  println(eval-expr(Add((Lit 10) (Lit 20))))
  println(color-to-int((Green)))
  0
```

A type can carry any number of annotated constructors. (All three below
return `(Expr int)`, so, like the two-constructor `Expr` earlier, this one is
a sum with its parameter pinned to `int` rather than a refined type.)

```turmeric
(defdata Expr [a]
  (Lit int                         : (Expr int))
  (Add (Expr int) (Expr int)       : (Expr int))
  (Mul (Expr int) (Expr int)       : (Expr int)))

(defn eval-expr [e] : int
  (match e
    (Lit n)   n
    (Add l r) (+ (eval-expr l) (eval-expr r))
    (Mul l r) (* (eval-expr l) (eval-expr r))))

(defn main [] : int
  ; (2 + (3 * 4)) = 14
  (let [e (Add (Lit 2) (Mul (Lit 3) (Lit 4)))]
    (println (eval-expr e))
    0))
```

```sweet-exp
defdata Expr [a]
  (Lit int                         : (Expr int))
  (Add (Expr int) (Expr int)       : (Expr int))
  (Mul (Expr int) (Expr int)       : (Expr int))

defn eval-expr [e] : int
  match e
    (Lit n)
    n
    (Add l r)
    {eval-expr(l) + eval-expr(r)}
    (Mul l r)
    {eval-expr(l) * eval-expr(r)}

defn main [] : int
  ; (2 + (3 * 4)) = 14
  let [e Add((Lit 2) Mul((Lit 3) (Lit 4)))]
    println(eval-expr(e))
    0
```

### Exhaustiveness by index

A `match` lists every constructor, with one exception: a constructor whose
declared result index provably differs from the **scrutinee's annotated
type** cannot be the value, so it needs no arm. Over the refined `Expr` of
"Mixing annotated and unannotated constructors" above:

```turmeric
(defn eval-int [e : (Expr int)] : int
  (match e
    (Lit n)    n
    (If c t f) (if (eval-bool c) (eval-int t) (eval-int f))))

(defn eval-bool [e : (Expr bool)] : bool
  (match e
    (BLit b)   b
    (IsZero x) (= (eval-int x) 0)
    (If c t f) (if (eval-bool c) (eval-bool t) (eval-bool f))))
```

```sweet-exp
defn eval-int [e : (Expr int)] : int
  match e
    (Lit n)
    n
    (If c t f)
    if(eval-bool(c) eval-int(t) eval-int(f))

defn eval-bool [e : (Expr bool)] : bool
  match e
    (BLit b)
    b
    (IsZero x)
    {eval-int(x) = 0}
    (If c t f)
    if(eval-bool(c) eval-bool(t) eval-bool(f))
```

`eval-int` leaves out `BLit` and `IsZero` (both `(Expr bool)`), and
`eval-bool` leaves out `Lit`; neither may leave out `If`, whose result is
the generic `(Expr a)`. The decision reads only the scrutinee's
annotation and each constructor's declared result. With an unannotated
scrutinee (`[e]`) every constructor is required -- the compiler does not
guess an instantiation from the other arms. (An older heuristic did, and
warned that a constructor was "unreachable for this GADT instantiation"; it
is gone.)

---

## The `Equal` GADT and `coerce`

The standard library's equality witness lives in `stdlib/equal.tur`, which is
not auto-loaded -- `(load "stdlib/equal.tur")`, or declare it yourself:

```turmeric
(defdata Equal [a b]
  (Refl : (Equal a a)))
```

```sweet-exp
defdata Equal [a b]
  (Refl : (Equal a a))
```

`(Refl)` is the only constructor. Because its return type is `(Equal a a)`,
a `Refl` can only be built at an equal pair: `(:: (Refl) (Equal int int))` is
a proof, and `(:: (Refl) (Equal int bool))` is TUR-E0001 -- as is a declared
return or an annotated `let` making that claim.

`coerce` takes a proof and a value, checks that the value has the proof's left
type, and hands the value back at the proof's right type -- no conversion, no
code:

```turmeric
(load "stdlib/equal.tur")

(defn main [] : int
  (let [proof (:: (Refl) (Equal int int))]
    (println (coerce proof 42))   ; 42
    0))
```

```sweet-exp
load "stdlib/equal.tur"

defn main [] : int
  let [proof (:: (Refl) (Equal int int))]
    println(coerce(proof 42))
    0
```

With that proof, `(coerce proof 7.1)` is TUR-E0001. With `eq : (Equal a b)`
the result is a `b`, so `(defn conv [a b] [eq : (Equal a b) x : a] : b (coerce
eq x))` type-checks and `(conv (Refl) 7.1)` is the float 7.1 -- the call reads
`b = a` from `Refl`'s `(Equal a a)`. A proof relating two different concrete
types, `(Equal int float)`, is an error: `Refl` cannot be built there.

Symmetry -- turning `(Equal a b)` into `(Equal b a)` -- follows from matching
on `Refl` and returning `Refl`: in the arm `a = b`, so `Refl` is also an
`(Equal b a)`. The checker accepts the declared result; it does not derive it
from the arm, so check such a signature by hand:

```turmeric
(defn sym [a b] [eq : (Equal a b)] : (Equal b a)
  (match eq
    (Refl) (Refl)))
```

```sweet-exp
defn sym [a b] [eq : (Equal a b)] : (Equal b a)
  match eq
    (Refl)
    (Refl)
```

The [cookbook](gadts-cookbook.md#equality-witnesses) has transitivity and a
typed box.

---

## Guard Clauses (`when`) in Match

Guard clauses let you refine which arm fires based on a runtime predicate.
They work in both plain ADT and GADT matches:

```turmeric
(defdata Sign (Pos int) (Neg int) (Zero))

(defn classify [s] : int
  (match s
    (Pos n) when (> n 100) (do (println "big") 0)
    (Pos n)                (do (println "pos") 0)
    (Neg n)                (do (println "neg") 0)
    (Zero)                 (do (println "zero") 0)))
```

```sweet-exp
defdata Sign (Pos int) (Neg int) (Zero)

defn classify [s] : int
  match s
    (Pos n)
    when
    {n > 100}
    do
      println("big")
      0
    (Pos n)
    do
      println("pos")
      0
    (Neg n)
    do
      println("neg")
      0
    (Zero)
    do
      println("zero")
      0
```

Arms are tried top to bottom. The first arm whose pattern matches *and* whose
guard is true fires. If no guard succeeds for a matched pattern the next arm
is tried.

---

## Common Errors and How to Fix Them

| Mistake | Error message (excerpt) | Fix |
|---|---|---|
| Annotating a constructor with another type's name | `defdata: constructor 'Lit' return type must be an application of 'Expr'` | Write `: (Expr ...)` -- the type being defined |
| Annotating with the wrong number of arguments | `defdata: constructor 'IntPair' return type has 1 type argument(s) but 'Pair2' has 2 type parameter(s)` | One argument per type parameter |
| Omitting the `: return-type` on a constructor meant to refine | none at the definition -- it returns the generic `(MyGadt a)` and refines nothing | Add `: (MyGadt ...)` after the field list |
| Missing a constructor in a match | `match: non-exhaustive patterns -- constructor 'Foo' of 'MyGadt' not covered` | Add the missing arm or a wildcard `_`; a constructor is left out only when the scrutinee's annotated type excludes it (see "Exhaustiveness by index") |
| Type mismatch from a skolem | `cannot unify 'int' with 'bool' (skolem from 'BoolTag')` | Check that the correct GADT arm is used for the operation |
| Skolem type escaping its scope | `skolem type variable 'a' escapes its match scope` | Use a universally polymorphic return type instead of the refined one |
| Unannotated GADT in a container | `ambiguous GADT type parameter -- annotation required` | Add an explicit type annotation, e.g. `: (vec (MyGadt int))` |

### No flag required

GADT support is on by default. Files that declare refined constructors or
match on a GADT compile with no special flag:

```sh
tur run build          # builds everything, GADTs included
./build/tur run my-file.tur
./build/tur build my-file.tur
```

The legacy `-Xgadt` flag is still accepted but is a deprecated no-op (it prints
`warning: -Xgadt is deprecated and has no effect; GADTs are enabled by
default`).

---

## Union Types and Gradual Typing

Turmeric also supports structural union types as a complement to GADTs.
Union types and GADTs are independent features that can be used together.

### Declaring a union parameter

```turmeric
(defn describe [x : (int | bool)] : int
  (match x
    (n : int)  (do (println "int")  0)
    (b : bool) (do (println "bool") 0)))
```

```sweet-exp
defn describe [x : (int | bool)] : int
  match x
    (n : int)
    do
      println("int")
      0
    (b : bool)
    do
      println("bool")
      0
```

The `(match x (n : int) body1 (b : bool) body2)` form dispatches on the
runtime tag. Pattern arms must be exhaustive across all union members.

### Typeclass dispatch on union values

When every member of a union has an instance for a typeclass method, you can
call `.method` directly without an explicit `match`. The compiler generates
the tag-dispatched call automatically:

```turmeric
(defclass Show [a]
  (show [x] : cstr))

(definstance Show [int]
  (show [x] "an-int"))

(definstance Show [bool]
  (show [x] "a-bool"))

(defn print-any [x : (int | bool)] : int
  (println (.show x))
  0)
```

```sweet-exp
defclass Show [a]
  show [x] : cstr

definstance Show [int]
  show [x] "an-int"

definstance Show [bool]
  show [x] "a-bool"

defn print-any [x : (int | bool)] : int
  println(.show(x))
  0
```

If any union member lacks an instance the compiler emits an error at the
call site naming the missing member.

### The `any` top type and gradual typing

`any` is a top type -- every concrete type is a subtype of `any`. Values
boxed into `any` carry a runtime tag so their type can be recovered:

```turmeric
(defn consume [x : any] : int
  (println (type-of x))   ; prints "int", "bool", "cstr", etc.
  0)

(defn main [] : int
  (consume 42)      ; prints "int"
  (consume true)    ; prints "bool"
  (consume "hello") ; prints "cstr"
  0)
```

```sweet-exp
defn consume [x : any] : int
  println(type-of(x))   ; prints "int", "bool", "cstr", etc.
  0

defn main [] : int
  consume(42)      ; prints "int"
  consume(true)    ; prints "bool"
  consume("hello") ; prints "cstr"
  0
```

Use `(type-of x)` to retrieve the type name as a `cstr`, and `(is? x T)` as
a boolean predicate on the runtime tag. `(cast x T)` unboxes an `any` value
as type `T`; the runtime tag is **checked**, and a mismatch panics:

```turmeric
(defn print-as-int [x : any] : int
  (println (cast x int))
  0)
```

```sweet-exp
defn print-as-int [x : any] : int
  println(cast(x int))
  0
```

---

## Nested patterns

A constructor pattern may appear in a field position, to any depth, and scalar
literals may appear there too. Arms are tried in order, so a specific arm can
precede a general one for the same constructor:

```turmeric
(defdata Expr [a]
  (Lit int                   : (Expr int))
  (Add (Expr int) (Expr int) : (Expr int)))

(defn simplify [e : (Expr int)] : int
  (match e
    (Lit v)         v
    (Add (Lit 0) r) (eval-expr r)      ; nested literal wins for a zero left
    (Add l r)       (+ (eval-expr l) (eval-expr r))))
```
```sweet-exp
defdata Expr [a]
  (Lit int                   : (Expr int))
  (Add (Expr int) (Expr int) : (Expr int))

defn simplify [e : (Expr int)] : int
  match e
    (Lit v)
    v
    (Add (Lit 0) r)
    eval-expr(r)      ; nested literal wins for a zero left
    (Add l r)
    {eval-expr(l) + eval-expr(r)}
```

The nested arms for one constructor must cover it: either a later arm for the
same constructor binds plain names (as `(Add l r)` does above), the nested
sub-patterns provably cover the sub-type's constructors, or a `_` arm follows.
Otherwise the compiler reports

```
match: the nested patterns for constructor 'Add' are not exhaustive --
add a `(Add ...)` arm binding plain names, or a `_` arm after it
```

rather than turning the gap into a runtime failure. A `when` guard on a nested
arm falls through to the next arm of the same group when it fails.

## Current Limitations

**Unsupported:**

- **No dependent types.** Type parameters must be types, not values. You cannot
  index a GADT by a runtime integer directly; use type-level naturals instead
  (phantom-parameter opaques: `(defopaque Zero :int)`, `(defopaque Succ [n]
  :int)`). A `match` on a scrutinee annotated `(Vec (Succ n))` then omits the
  `VNil` arm -- the exhaustiveness check drops a constructor whose declared
  index provably differs from the scrutinee's annotated type; an unannotated
  scrutinee needs every arm -- and `(vec-head (VNil))` is
  rejected at the call: a constructor application knows its index
  (`(VNil)` is `(Vec Zero)`), and so do a `let` bound to one, an ascription
  and a declared return. The value's type stays the bare `Vec`, so a
  bare-annotated function takes every vector, and an index the checker
  cannot know is not checked rather than rejected. See the cookbook's
  "Length-Indexed Vectors".

- **No mutual recursion across files.** Mutually recursive GADTs must be
  defined in the same file.

- **Polymorphic recursion is not fully inferred.** If a GADT function is
  polymorphically recursive, add an explicit type annotation on the `defn`.

- **`cast` panics on mismatch.** `(cast x T)` checks the runtime tag and
  aborts when it does not match `T`. Use `(is? x T)` first if you need a
  non-aborting downcast.

**Supported:**

- **The `(~ a b)` constraint syntax** binds a type variable to a concrete type
  in a `defn` parameter list: `(defn f [(~ a int) x :a] :int ...)`.

- **`equal-cong`** is implemented in `stdlib/equal.tur`. The congruence lemma
  `(defn equal-cong [^f eq : (Equal a b)] : (Equal (f a) (f b)) ...)` uses
  kind-`* -> *` type variables.

- **Implicit union widening** is supported. A value of type `A` can be passed
  where `(A | B)` is expected; the compiler inserts tag injection
  automatically.

---

## Quick Reference

```turmeric
; Declare a plain ADT
(defdata ^copy Color (Red) (Green) (Blue))

; Constructors with their own result type (defgadt is the deprecated spelling)
(defdata Expr [a]
  (Lit int                         : (Expr int))
  (Add (Expr int) (Expr int)       : (Expr int))
  (Mul (Expr int) (Expr int)       : (Expr int)))

; Pattern match -- exhaustiveness checked, guards optional
(defn eval-expr [e] : int
  (match e
    (Lit n)   n
    (Add l r) (+ (eval-expr l) (eval-expr r))
    (Mul l r) (* (eval-expr l) (eval-expr r))))

; Equality witness -- refined: Refl's result equates a and b
(defdata Equal [a b]
  (Refl : (Equal a a)))

; coerce a value using an equality proof
(coerce (Refl) some-value)

; Union type dispatch
(defn describe [x : (int | bool)] : int
  (match x
    (n : int)  0
    (b : bool) 1))

; Gradual typing
(defn show-type [x : any] : int
  (println (type-of x))
  0)
```

```sweet-exp
; Declare a plain ADT
defdata ^copy Color (Red) (Green) (Blue)

; Constructors with their own result type (defgadt is the deprecated spelling)
defdata Expr [a]
  (Lit int                         : (Expr int))
  (Add (Expr int) (Expr int)       : (Expr int))
  (Mul (Expr int) (Expr int)       : (Expr int))

; Pattern match -- exhaustiveness checked, guards optional
defn eval-expr [e] : int
  match e
    (Lit n)
    n
    (Add l r)
    {eval-expr(l) + eval-expr(r)}
    (Mul l r)
    {eval-expr(l) * eval-expr(r)}

; Equality witness -- refined: Refl's result equates a and b
defdata Equal [a b]
  (Refl : (Equal a a))

; coerce a value using an equality proof
coerce((Refl) some-value)

; Union type dispatch
defn describe [x : (int | bool)] : int
  match x
    (n : int)
    0
    (b : bool)
    1

; Gradual typing
defn show-type [x : any] : int
  println(type-of(x))
  0
```

## See also

- [hrt-guide.md](hrt-guide.md) -- Higher-ranked types; bidirectional checking
  that enables GADT skolem propagation
- [hkt-guide.md](hkt-guide.md) -- Higher-kinded types; used by `equal-cong`
  and polymorphic GADT indices
- [`tests/fixtures/gadt-*/`](https://github.com/turmeric-lang/turmeric/tree/main/tests/fixtures/) -- Working GADT examples
- [`tests/fixtures/union-types-*/`](https://github.com/turmeric-lang/turmeric/tree/main/tests/fixtures/) -- Union type and gradual typing examples
