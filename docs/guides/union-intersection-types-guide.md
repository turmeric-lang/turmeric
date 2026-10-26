---
title: Union and Intersection Types Guide
category: Type System
description: Union (`A | B`) and intersection (`A & B`) types, `any`, gradual typing
tags:
  - union-types
  - intersection-types
  - gradual-typing
  - subtyping
  - any
---

# Union and Intersection Types Guide

> **Status:** `any` boxing codegen, the checked `cast`, and `type-of` ship for
> every payload kind (int/bool/float/nil/cstr/ptr, ADTs, and heap-boxed
> structs), at per-TYPE granularity -- `type-of` names the struct or ADT and
> `cast` rejects a different one. The one deferred item is general
> `struct { int tag; union { ... } }` tagged-union C emission. See
> [Deferred](#deferred) below.

Union types (`A | B`) and intersection types (`A & B`) extend the Turmeric type system with
structural type combinations. Together they enable gradual typing, flexible APIs, and
type-safe duck typing without wrapper ADTs.

Both features, along with the `any` type, are enabled by default; no flag is required.

---

## Union Types

A union type `(A | B)` represents a value that is **either** `A` or **`B`**. At runtime
a union-typed value is carried as a `tur_tagged_t` (`{ int64_t tag; int64_t val; }`) --
a tag word plus one 64-bit payload slot.

### Syntax

```turmeric
;; Named union type
(defalias IntOrString (int | cstr))

;; Inline in a function signature
(defn print-value [x : (int | cstr | bool)] : nil
  (match x
    (i : int)  (println i)
    (s : cstr) (println s)
    (b : bool) (println (if b "true" "false"))))
```

```sweet-exp
;; Named union type
defalias IntOrString (int | cstr)

;; Inline in a function signature
defn print-value [x : (int | cstr | bool)] : nil
  match x
    (i : int)
    println(i)
    (s : cstr)
    println(s)
    (b : bool)
    println((if b "true" "false"))
```

Name a union with `defalias`, not `deftype`: `deftype` is the *recursive* type
binder, so the name it binds is a distinct nominal type that use sites fail to
unify with the union body (`expected <rec>, got int`). The
[syntax guide](syntax-guide.md#naming-a-type----defalias-vs-deftype) has the
full rule.

Nested unions are flattened: `(int | (cstr | bool))` becomes `(int | cstr | bool)`.

### Pattern Matching on Union Types

Use `match` to narrow a union-typed value. The elaborator checks exhaustiveness across
all union members:

```turmeric
(defn describe [x : (int | cstr)] : cstr
  (match x
    (n : int)  "a number"
    (s : cstr) s))
```

```sweet-exp
defn describe [x : (int | cstr)] : cstr
  match x
    (n : int)
    "a number"
    (s : cstr)
    s
```

Omitting any member is a compile-time error (`TUR-E0301`).

### Subtyping

A value of type `A` can be passed anywhere `(A | B)` is expected (widening). This is
handled implicitly at call sites and return positions:

```turmeric
(defn accepts-union [x : (int | cstr)] : nil ...)

(accepts-union 42)       ;; int widens to (int | cstr)
(accepts-union "hello")  ;; cstr widens to (int | cstr)
```

```sweet-exp
defn accepts-union [x : (int | cstr)] : nil ...

accepts-union(42)       ;; int widens to (int | cstr)
accepts-union("hello")  ;; cstr widens to (int | cstr)
```

### Typeclass Methods on Unions

When `x : (int | cstr)`, typeclass methods implemented by **all** union members may be
called directly without a `match`. Methods not in the intersection require an explicit
`match` to narrow first:

```turmeric
;; Show is implemented by both int and cstr
(show x)   ;; ok -- in the instance intersection

;; Arithmetic is only on int; requires narrowing
(match x
  (n : int)  (+ n 1)
  (s : cstr) ...)
```

```sweet-exp
;; Show is implemented by both int and cstr
show(x)   ;; ok -- in the instance intersection

;; Arithmetic is only on int; requires narrowing
match x
  (n : int)
  {n + 1}
  (s : cstr)
  ...
```

---

## Intersection Types

An intersection type `(A & B)` represents a value that satisfies **both** `A` and `B`.
The primary use is combining a concrete type with typeclass constraints.

### Syntax

```turmeric
;; Named intersection type
(defalias ReadWrite (Readable & Writable))

;; Inline in a function signature
(defn save [x : (int & Serializable)] : nil
  (file/write (serialize x) "output.bin"))
```

```sweet-exp
;; Named intersection type
defalias ReadWrite (Readable & Writable)

;; Inline in a function signature
defn save [x : (int & Serializable)] : nil
  file/write(serialize(x) "output.bin")
```

`defalias`, not `deftype`, for the same reason as a named union above.

### Subtyping

From a value of intersection type you can project either member:

- `(A & B) <: A` and `(A & B) <: B`
- A function expecting `A` accepts a value of type `(A & B)`

### Typeclass Intersection

Intersection is most useful when one side is a typeclass:

```turmeric
(defclass Serializable [a]
  (serialize [x : a] : cstr))

(definstance Serializable [int]
  (serialize [x : int] : cstr "an int"))

(defn serialize-int [x : (int & Serializable)] : cstr
  (serialize x))
```

```sweet-exp
defclass Serializable [a]
  serialize [x : a] : cstr

definstance Serializable [int]
  serialize [x : int] : cstr
    "an int"

defn serialize-int [x : (int & Serializable)] : cstr
  serialize(x)
```

The `definstance` is not optional decoration: the intersection resolves the
method against a real instance, so a class with none in scope fails with
`no typeclass method found for 'serialize'`.

The value is an `int` with a `Serializable` dictionary attached. The elaborator
resolves the instance at the intersection type site.

### Unsatisfiable Intersections

Intersections of known-disjoint concrete types are rejected statically (`TUR-E0350`):

```turmeric
;; Compile error: int and cstr are disjoint
(defn bad [x : (int & cstr)] : nil ...)
```

```sweet-exp
;; Compile error: int and cstr are disjoint
defn bad [x : (int & cstr)] : nil ...
```

Intersections involving typeclasses or type variables that cannot be determined disjoint
at compile time are permitted and fail during instance resolution.

---

## The `any` Type

`any` is the **top type**: every type is a subtype of `any`. Like unions and
intersections, it is enabled by default.

```turmeric
(defn debug-print [x : any] : nil
  (println (type-of x)))

(debug-print 42)      ;; prints int
(debug-print "hello") ;; prints cstr
(debug-print true)    ;; prints bool
```

```sweet-exp
defn debug-print [x : any] : nil
  println $ type-of x

debug-print(42)      ;; prints int
debug-print("hello") ;; prints cstr
debug-print(true)    ;; prints bool
```

Note what the body does **not** do: `(println x)` on an `any` does not compile.
The builtin operator table is keyed on concrete argument kinds and has no `any`
row, so `println`, `+`, `-`, `=` and `<` all reject an `any` argument with
`TUR-E0006`. `any` is a storage and reflection type -- it holds a value and
tells you what it is; to *operate* on the payload, narrow it first with an
`is?` guard or a `cast` (both below).

`any`-typed values are represented at codegen as a `tur_tagged_t`
(`{ int64_t tag; int64_t val; }`): the `tag` is the payload's `TypeKind` and
the `val` carries the payload. Immediate values (int/bool/nil) ride the
carrier directly, floats are stored as their IEEE-754 bit pattern, pointer
payloads (`cstr`, `ptr`, ADT handles) store the pointer, and by-value structs
are heap-boxed (a `malloc`'d copy whose pointer rides the carrier). A value is
boxed automatically wherever it is widened to `any` -- at a call argument, a
function's `: any` return position, or an `if` branch facing an `any` sibling.

Union simplification: `(int | cstr | any)` simplifies to `any`.

---

## Gradual Typing

Union types and `any` enable a gradual typing path:

```turmeric
;; Start untyped -- accept anything, and report what actually arrived
(defn debug-print [x : any] : nil
  (println (type-of x)))

;; Narrow gradually as the shape becomes known: a union you can match on
(defn typed-print [x : (int | cstr)] : nil
  (match x
    (n : int)  (println n)
    (s : cstr) (println s)))

;; A union-typed value still flows into the `any`-typed function, so the two
;; coexist while you migrate -- the `any` reports the MEMBER's type, not "union"
(defn describe-either [x : (int | cstr)] : nil
  (debug-print x))
```

```sweet-exp
;; Start untyped -- accept anything, and report what actually arrived
defn debug-print [x : any] : nil
  println $ type-of x

;; Narrow gradually as the shape becomes known: a union you can match on
defn typed-print [x : (int | cstr)] : nil
  match x
    (n : int)
    println(n)
    (s : cstr)
    println(s)

;; A union-typed value still flows into the `any`-typed function
defn describe-either [x : (int | cstr)] : nil
  debug-print(x)
```

Widening a union to `any` re-boxes it through the member it actually holds:
`(describe-either 42)` prints `int` and `(describe-either "hi")` prints `cstr`,
never `union`. The two representations look alike -- both are a tag word plus a
payload -- but a union's tag is a *member index* while an `any` box's is a type
id, so the widen switches on the index rather than reinterpreting the value.

---

## Boxing, `cast`, and `type-of`

A value widened to `any` is boxed into a `tur_tagged_t` that records the
payload's runtime type. Two forms read that box back:

- **`(type-of x)`** returns the payload's type name as a `cstr`. A primitive
  reports its kind -- `"int"`, `"bool"`, `"float"`, `"cstr"`, `"ptr"` -- and a
  struct or ADT reports **its own name**: `"Point"`, `"Shape"`. (It used to
  answer `"struct"` for every struct and `"adt"` for every ADT.)
- **`(cast x : T)`** is a *checked* downcast. It verifies the box tag matches
  `T` and returns the unboxed value; on a mismatch it **panics** (aborts with a
  message like `cast: any holds cstr, not int`). `T` may be a primitive type
  name or a struct/ADT name.

```turmeric
(defn box-it [] : any "hello")

(defn main [] : int
  (let [a (box-it)]
    (println (type-of a))    ;; => cstr
    (println (cast a cstr))  ;; => hello
    ;; (cast a int)          ;; would panic: any holds cstr, not int
    0))
```

The check is by TYPE, not by kind, so two struct types are distinguishable:

```turmeric
(defstruct Point [x : int y : int])
(defstruct Other [a : int b : int])

(let [a (:: (make-struct Point 3 4) any)]
  (println (type-of a))     ;; => Point
  (println (is? a Point))   ;; => true
  (println (is? a Other))   ;; => false
  ;; (cast a Other)         ;; panics: cast: any holds Point, not Other
  (.x (cast a Point)))      ;; => 3
```

Under the hood the box tag is a per-monomorph id for a struct/ADT payload (a
primitive keeps its `TypeKind`), and the program carries a table naming the ids
it allocated. `(Box int)` and `(Box float)` get distinct ids; `type-of` reports
the head name (`"Box"`) for both.

By-value structs are heap-boxed on widening (a `malloc`'d copy) and unboxed by
dereference on `cast`; ADTs and `cstr` are pointer-carried and ride the carrier
directly; floats are stored by their bit pattern so no precision is lost.

> **Note:** widening a struct to `any` does not leak. The heap box a by-value
> payload needs is owned in every position it can occupy:
>
> - **As a call argument**, when the callee neither retains the value nor can
>   suspend, there is no allocation at all -- the copy lives in the caller's
>   frame.
> - **Bound to a local** that does not escape, the box is released when the
>   local dies: at scope exit, at its sole consuming use when the scope's end is
>   unreachable, and at a `return` or a tail-call back-edge otherwise. A local
>   that `is?` narrows is covered too, even though the narrowing rebinds the
>   name.
> - **As a temporary** -- never named -- produced by a function whose body ends
>   in a widen, or forwarded through a pure passthrough, and consumed by a
>   non-retaining, effect-free call.
>
> A callee that retains the value, has an inline-C body, may suspend, or is
> called indirectly keeps the box by design: the caller cannot know when it
> dies. That is the one place a struct payload still costs an allocation, and
> the guidance there is unchanged -- prefer a pointer/ADT payload, which is
> carrier-resident and allocation-free.
>
> Pinned leak-clean under LeakSanitizer by `tests/fixtures/any-widen-frame-box`,
> `any-widen-local-drop`, `any-widen-temp-drop`,
> `any-widen-drop-past-early-exit`, and `any-widen-drop-narrowed`; the shapes
> that must NOT be dropped by `any-widen-retaining-callee`. See
> `docs/archive/any-struct-box-leak-per-widen.md`.

---

## ADTs and Unions (interop via `any`, not a desugar)

An early plan proposed desugaring `defdata` into a union type internally so the
two share one code path. That internal unification is **not pursued**: the union
machinery is monomorphic, closed, and non-recursive, while every `defdata` in
the tree is parametric (`Either [L R]`), higher-kinded (`Fix [^f]`,
`Free [^f a]`), recursive (`Nat`), or a GADT (`Equal`). Lowering those onto
today's union representation would mean rebuilding parametric/HKT/recursive/GADT
sum-type support on top of unions -- a far larger change with no user-visible
payoff.

The user-facing goal that desugar was meant to deliver -- ADT values
participating in union-style dispatch -- **already ships** through the `any` top
type. An ADT value widens to `any` (boxing codegen), reports **its own ADT
name** via `type-of` -- a `(Circle 5)` answers `"Shape"`, not `"adt"`, per the
per-type granularity described above -- and lands back in `match` through a
checked `cast`. See the
`defdata-as-union` and `any-box-adt` fixtures.

---

## Error Codes

| Code | Message |
|---|---|
| `TUR-E0300` | Union type mismatch: expected `{expected}`, got `{actual}` |
| `TUR-E0301` | Non-exhaustive pattern match on union type `{type}` -- missing arm for `{variant}` |
| `TUR-E0350` | Intersection type unsatisfiable: no value can be both `{A}` and `{B}` |
| `TUR-E0351` | Value of type `{actual}` does not satisfy intersection member `{missing}` |

---

## Known Limitations

### Tagged Union Overhead

TypeScript's union types are zero-cost (erased). Turmeric carries every
union-typed value as a `tur_tagged_t` (`{ int64_t tag; int64_t val; }`): one
extra 64-bit tag word per value, with the payload riding a single 64-bit slot
(by-value structs are heap-boxed into it). This matters for arrays, struct
fields, and cache pressure.

Widening (passing `42` where `(int | cstr)` is expected) requires constructing
the tagged union at the call site -- it is not a free annotation.

> **Where the by-value heap box does and does not happen.** In ARGUMENT
> position, widening a by-value member for a call that provably neither retains
> the payload nor suspends puts the copy in the caller's frame instead, so there
> is no allocation at all -- the same rule, and the same inference, that makes an
> `any` widen allocation-free. Every other position (a union bound to a local,
> returned, or held as a temporary) still boxes, and that box has no owner: one
> leak per widen. Pinned by `tests/fixtures/union-widen-frame-box` under
> LeakSanitizer; see `docs/archive/union-tagged-union-c-emission.md` for what
> the remaining positions need.

Two member kinds do not ride the `val` slot as an integer, and both the inject
and the `match` binder account for it: a by-value aggregate is heap-boxed and
the slot holds a pointer, and a `float` rides as its IEEE-754 bit pattern rather
than a numeric conversion.

### `if`-Guard Narrowing (`any`)

Flow-sensitive narrowing works in `if` guards on an `any`-typed variable. A
type-test guard in the condition refines the variable to the tested type inside
the **then**-branch, so it can be used at that type with no explicit cast. Two
guard shapes are recognized:

```turmeric
;; (is? x T) -- the dedicated type-test predicate
(defn bump [x : any] : int
  (if (is? x int)
    (+ x 1)     ;; x is narrowed to int here -- no cast needed
    0))

;; (= (type-of x) "T") -- type-of compared against a string literal
(if (= (type-of x) "int")
  (+ x 1)
  0)
```

Chaining handles multi-type dispatch:

```turmeric
(defn describe [v : any] : cstr
  (if (is? v int)   "int"
    (if (is? v bool) "bool"
      (if (is? v Point) "point" "other"))))
```

`(is? x T)` is also a plain boolean predicate (it requires an `any`-typed
argument, like `type-of` and `cast`). The runtime check compares the value's
box tag to `T`; `T` may be a primitive, struct, or ADT name.

**Supported guard shapes (narrow):** a direct `(is? x T)` or
`(= (type-of x) "T")` test on a single `any` variable, used as the whole `if`
condition. **Unsupported (do not narrow):** negation (`(not (is? x T))`),
conjunction/disjunction of tests, the else-branch complement, and tests on
union (`A | B`) variables. For unions, use `match`, which narrows exhaustively:

```turmeric
(match x
  (n : int)  (+ n 1)
  (s : cstr) ...)
```

### Intersection is Constraint-Only

TypeScript and Scala 3 merge struct fields across intersections
(`{ x: int } & { y: bool }` gives `{ x: int; y: bool }`). Turmeric statically
rejects intersections of two known-disjoint concrete types. Intersection is only
useful when at least one side is a typeclass.

### Closed Unions

Union types are closed -- the member set is fixed at definition time. A library
returning `(int | ParseError)` cannot be transparently composed with one returning
`(bool | ParseError)` without an explicit adapter.

### Variance with Generics

Variance for type constructors containing union or intersection types is not yet
specified. Passing `(vec (int | cstr))` where `(vec int)` is expected may produce
unexpected behaviour.

---

## Deferred

The following items are not yet implemented:

| Item | Notes |
|---|---|
| Tagged union C codegen | General `struct { int tag; union { A a; B b; } data; }` emission for `(A \| B)` unions (the `any` top type ships via `tur_tagged_t`) |
| ADT-as-union sugar | Not pursued -- infeasible against monomorphic unions (defdata is parametric/HKT/recursive/GADT). ADTs already interoperate with unions via `any` boxing; see [ADTs and Unions](#adts-and-unions-interop-via-any-not-a-desugar). |
| Instance intersection on unions | Deferred failure during instance resolution may be hard to diagnose |

---

## See Also

- [Type Annotations Guide](type-annotations-guide.md) -- compound type syntax: `(-> a b)`, `(vec T)`, `forall`
- [GADTs Guide](gadts-guide.md) -- refined constructors (`defdata` with `: Result`), type refinement, union types interaction
- [Error Handling Guide](error-handling-guide.md) -- `Result` and `Option` types
- [Contract Types Guide](contract-types-guide.md) -- runtime-checked predicates on types
