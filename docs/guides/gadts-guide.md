---
title: GADTs -- A Practical Guide
category: Advanced Types
description: GADTs: `defgadt`, type refinement, equality witnesses, union types, gradual typing
---

# GADTs in Turmeric -- A Practical Guide

Generalized Algebraic Data Types (GADTs) extend ordinary sum types by letting
each constructor specialize the type parameters it returns. The type-checker
learns those specializations when pattern-matching, so programs that previously
needed runtime tags or unsafe casts become statically verified.

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
(defdata Color :copy (Red) (Green) (Blue))

(defn color-to-int [c] : int
  (match c
    (Red)   1
    (Green) 2
    (Blue)  3))
```

```sweet-exp
defdata Color :copy (Red) (Green) (Blue)

defn color-to-int [c] :int
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

defn option-or [opt default] :int
  match opt
    (None)
    default
    (Some x)
    x
```

The `:copy` modifier makes the type copyable (like `defstruct :copy`). Without
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

defn area [s] :int
  match s
    (Circle r)
    {r * r}
    (Rect w h)
    {w * h}
```

Pattern matching is exhaustiveness-checked. If you omit a constructor the
compiler reports the missing arm.

---

## Your First GADT (`defgadt`)

A GADT is declared with `defgadt`. The key difference from `defdata` is that
each constructor carries an explicit `: return-type` annotation:

```turmeric
(defgadt Expr [a]
  (Lit int         : (Expr int))
  (Add (Expr int) (Expr int) : (Expr int)))
```

```sweet-exp
defgadt Expr [a]
  (Lit int         : (Expr int))
  (Add (Expr int) (Expr int) : (Expr int))
```

Each constructor line reads as: "field types ... `: return type`". Here:

- `(Lit int : (Expr int))` -- `Lit` takes one `int` field and returns `(Expr int)`.
- `(Add (Expr int) (Expr int) : (Expr int))` -- `Add` takes two `(Expr int)`
  fields and also returns `(Expr int)`.

Like `defdata`, a `defgadt` accepts an optional `:copy` modifier -- placed
after the type-parameter vector -- to opt the type out of affine move
tracking. Without it, GADT values follow move semantics (using one twice is a
`TUR-E0005` use-after-move); with it, the values are plain, freely-readable
value types (use this when the constructors carry only copyable payloads, e.g.
shared range bounds):

```turmeric
(defgadt Bound [A]
  :copy
  (Inclusive int : (Bound int))
  (Exclusive int : (Bound int))
  (Unbounded     : (Bound int)))
```

This is exactly the `Bound` GADT that backs `stdlib/range.tur`'s internal
endpoint representation -- `Inclusive` / `Exclusive` / `Unbounded` range
bounds (see [`range-gadt-typeclass-migration-plan`](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/range-gadt-typeclass-migration-plan.md)).

GADT support is on by default, so no flag is needed. (Passing the legacy
`-Xgadt` flag still works but prints a deprecation notice.)

Evaluation is written the same as for a plain ADT:

```turmeric
(defn eval-expr [e] : int
  (match e
    (Lit n)   n
    (Add l r) (+ (eval-expr l) (eval-expr r))))
```

```sweet-exp
defn eval-expr [e] :int
  match e
    (Lit n)
    n
    (Add l r)
    {eval-expr(l) + eval-expr(r)}
```

---

## Type Refinement in Match Arms

The power of GADTs shows up when constructors specialize different type
parameters. Consider a typed tag:

```turmeric
(defgadt Tag [a]
  (IntTag  : (Tag int))
  (BoolTag : (Tag bool)))
```

```sweet-exp
defgadt Tag [a]
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
defn default-value [t] :int
  match t
    (IntTag)
    0
    (BoolTag)
    0
```

In the `IntTag` arm the type-checker knows `a = int`; in `BoolTag` it knows
`a = bool`. These *skolem equalities* are introduced automatically per arm and
are invisible to the programmer. No casts, no `option` wrapper, and no runtime tags.

A larger example combining plain ADTs and GADTs in one program:

```turmeric
(defdata Color :copy (Red) (Green) (Blue))

(defgadt Expr [a]
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
defdata Color :copy (Red) (Green) (Blue)

defgadt Expr [a]
  (Lit int         : (Expr int))
  (Add (Expr int) (Expr int) : (Expr int))

defn color-to-int [c] :int
  match c
    (Red)
    1
    (Green)
    2
    (Blue)
    3

defn eval-expr [e] :int
  match e
    (Lit n)
    n
    (Add l r)
    {eval-expr(l) + eval-expr(r)}

defn main [] :int
  println(color-to-int((Red)))
  println(eval-expr(Add((Lit 10) (Lit 20))))
  println(color-to-int((Green)))
  0
```

You can also use multiple constructors that refine the same type variable in
different ways:

```turmeric
(defgadt Expr [a]
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
defgadt Expr [a]
  (Lit int                         : (Expr int))
  (Add (Expr int) (Expr int)       : (Expr int))
  (Mul (Expr int) (Expr int)       : (Expr int))

defn eval-expr [e] :int
  match e
    (Lit n)
    n
    (Add l r)
    {eval-expr(l) + eval-expr(r)}
    (Mul l r)
    {eval-expr(l) * eval-expr(r)}

defn main [] :int
  ; (2 + (3 * 4)) = 14
  let [e Add((Lit 2) Mul((Lit 3) (Lit 4)))]
    println(eval-expr(e))
    0
```

---

## The `Equal` GADT and `coerce`

The standard library provides a built-in equality witness GADT:

```turmeric
; Built into the runtime -- you do not need to declare this yourself.
; (defgadt Equal [a b]
;   (Refl : (Equal a a)))
```

```sweet-exp
; Built into the runtime -- you do not need to declare this yourself.
; defgadt Equal [a b]
;   (Refl : (Equal a a))
```

`(Refl)` is the only constructor. Because its return type is `(Equal a a)`,
constructing `(Refl)` proves that the two type parameters are the same type.

Use `coerce` to convert a value across a proven equality:

```turmeric
(defgadt Equal [a b]
  (Refl : (Equal a a)))

(defn main [] : int
  (match (Refl)
    (Refl)
      (do
        (println (coerce (Refl) 42))
        0)))
```

```sweet-exp
defgadt Equal [a b]
  (Refl : (Equal a a))

defn main [] :int
  match (Refl)
    (Refl)
    do
      println(coerce((Refl) 42))
      0
```

In the `(Refl)` arm the type-checker knows `a = b`, so `coerce` can safely
reinterpret the value without any runtime overhead.

Symmetry -- turning `(Equal a b)` into `(Equal b a)` -- follows from matching
on `Refl` and returning `Refl`:

```turmeric
(defn sym [eq] :(Equal b a)
  (match eq
    (Refl) (Refl)))
```

```sweet-exp
defn sym [eq] :(Equal b a)
  match eq
    (Refl)
    (Refl)
```

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

defn classify [s] :int
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
| Omitting the `: return-type` on a refining constructor | `constructor refines type variable -- explicit return type required` | Add `: (MyGadt ...)` after the field list |
| Missing a constructor in a match | `non-exhaustive match: missing constructor 'Foo'` | Add the missing arm or a wildcard `_` |
| Type mismatch from a skolem | `cannot unify 'int' with 'bool' (skolem from 'BoolTag')` | Check that the correct GADT arm is used for the operation |
| Skolem type escaping its scope | `skolem type variable 'a' escapes its match scope` | Use a universally polymorphic return type instead of the refined one |
| Unannotated GADT in a container | `ambiguous GADT type parameter -- annotation required` | Add an explicit type annotation, e.g. `: (vec (MyGadt int))` |

### No flag required

GADT support is on by default. Files that use `defgadt` or match on a GADT
compile with no special flag:

```sh
just build          # builds everything, GADTs included
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
defn describe [x : (int | bool)] :int
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
  show [x] :cstr

definstance Show [int]
  show [x] "an-int"

definstance Show [bool]
  show [x] "a-bool"

defn print-any [x : (int | bool)] :int
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
defn consume [x : any] :int
  println(type-of(x))   ; prints "int", "bool", "cstr", etc.
  0

defn main [] :int
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
defn print-as-int [x : any] :int
  println(cast(x int))
  0
```

---

## Nested patterns

A constructor pattern may appear in a field position, to any depth, and scalar
literals may appear there too. Arms are tried in order, so a specific arm can
precede a general one for the same constructor:

```turmeric
(defgadt Expr [a]
  (Lit int                   : (Expr int))
  (Add (Expr int) (Expr int) : (Expr int)))

(defn simplify [e : (Expr int)] : int
  (match e
    (Lit v)         v
    (Add (Lit 0) r) (eval-expr r)      ; nested literal wins for a zero left
    (Add l r)       (+ (eval-expr l) (eval-expr r))))
```
```sweet-exp
defgadt Expr [a]
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
  index provably differs from the scrutinee's -- and `(vec-head (VNil))` is
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
(defdata Color :copy (Red) (Green) (Blue))

; Declare a GADT  (GADTs are on by default)
(defgadt Expr [a]
  (Lit int                         : (Expr int))
  (Add (Expr int) (Expr int)       : (Expr int))
  (Mul (Expr int) (Expr int)       : (Expr int)))

; Pattern match -- exhaustiveness checked, guards optional
(defn eval-expr [e] : int
  (match e
    (Lit n)   n
    (Add l r) (+ (eval-expr l) (eval-expr r))
    (Mul l r) (* (eval-expr l) (eval-expr r))))

; Equality witness
(defgadt Equal [a b]
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
defdata Color :copy (Red) (Green) (Blue)

; Declare a GADT  (GADTs are on by default)
defgadt Expr [a]
  (Lit int                         : (Expr int))
  (Add (Expr int) (Expr int)       : (Expr int))
  (Mul (Expr int) (Expr int)       : (Expr int))

; Pattern match -- exhaustiveness checked, guards optional
defn eval-expr [e] :int
  match e
    (Lit n)
    n
    (Add l r)
    {eval-expr(l) + eval-expr(r)}
    (Mul l r)
    {eval-expr(l) * eval-expr(r)}

; Equality witness
defgadt Equal [a b]
  (Refl : (Equal a a))

; coerce a value using an equality proof
coerce((Refl) some-value)

; Union type dispatch
defn describe [x : (int | bool)] :int
  match x
    (n : int)
    0
    (b : bool)
    1

; Gradual typing
defn show-type [x : any] :int
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
