---
title: Higher-Kinded Types
category: Type System
description: Higher-kinded types (functor, monad, applicative abstractions, performance/dispatch model)
---

# Higher-Kinded Types in Turmeric

Turmeric supports higher-kinded types (HKT), allowing you to write generic code
over type constructors like `Option`, `Vec`, or any user-defined container.

## Overview

A **type constructor** takes one or more types and produces a new type.
For example, `Option` is a type constructor: given `int`, it produces `Option<int>`.

In Turmeric's kind system:
- `*` -- a plain type (e.g. `int`, `bool`)
- `* -> *` -- a unary type constructor (e.g. `Option`, `Vec`)
- `* -> * -> *` -- a binary type constructor (e.g. `Either`, `Pair`)

Typeclasses can be parameterised over type constructors using the `^f` or `^^f` syntax.

## Defining HKT Typeclasses

### Unary type constructor (`^f` -- kind `* -> *`)

```turmeric
(defclass Functor [^f]
  (fmap [container fn] : int))

(defclass Monad [^m]
  (bind [ma fn] : int))

(defclass Foldable [^t]
  (fold-left  [container acc fn] : int)
  (fold-right [container acc fn] : int))
```

```sweet-exp
defclass Functor [^f]
  fmap [container fn] :int

defclass Monad [^m]
  bind [ma fn] :int

defclass Foldable [^t]
  fold-left  [container acc fn] :int
  fold-right [container acc fn] :int
```

The `^f` parameter means "f has kind `* -> *`".
Using a primitive type (like `int`) in the type argument position is a type error:

```turmeric
;; ERROR: int has kind *, not * -> *
(definstance Functor [int]
  (fmap [c f] c))
```

```sweet-exp
;; ERROR: int has kind *, not * -> *
definstance Functor [int]
  fmap [c f] c
```

### Binary type constructor (`^^f` -- kind `* -> * -> *`)

```turmeric
(defclass Bifunctor [^^f]
  (bimap [container fn-left fn-right] : int))
```

```sweet-exp
defclass Bifunctor [^^f]
  bimap [container fn-left fn-right] :int
```

The `^^f` parameter means "f has kind `* -> * -> *`".

### Kind aliases with `defkind`

`defkind` lets you give a name to a kind for documentation purposes.
It is currently informational only (parsed and ignored):

```turmeric
(defkind Unary  (* -> *))
(defkind Binary (* -> * -> *))
```

```sweet-exp
defkind Unary  (* -> *)
defkind Binary (* -> * -> *)
```

## Defining Instances

Provide a concrete type constructor when implementing an HKT typeclass:

```turmeric
;; Functor for Option
(definstance Functor [option]
  (fmap [container fn] (__fmap_option container fn)))

;; Monad for Option
(definstance Monad [option]
  (bind [ma fn] (__bind_option ma fn)))

;; Bifunctor for Pair
(definstance Bifunctor [pair]
  (bimap [container fn-left fn-right] (__bimap_pair container fn-left fn-right)))
```

```sweet-exp
;; Functor for Option
definstance Functor [option]
  fmap [container fn] __fmap_option(container fn)

;; Monad for Option
definstance Monad [option]
  bind [ma fn] __bind_option(ma fn)

;; Bifunctor for Pair
definstance Bifunctor [pair]
  bimap [container fn-left fn-right] __bimap_pair(container fn-left fn-right)
```

## Using HKT Typeclasses

### Method dispatch (`.method`)

Call typeclass methods using the dot-dispatch syntax:

```turmeric
;; fmap over an Option
(let [opt (__opt_some 5)]
  (.fmap opt (fn [x] (* x 2))))    ;; dispatches Functor.fmap
```

```sweet-exp
;; fmap over an Option
let [opt __opt_some(5)]
  .fmap(opt (fn [x] {x * 2}))    ;; dispatches Functor.fmap
```

> **Note**: Method dispatch on HKT containers uses the first-found instance as a
> fallback when the container type is stored as `int64_t` (opaque handle). This
> works reliably when only one instance of the typeclass is in scope.

### Direct function calls

For reliability with multiple instances, call the implementation function directly:

```turmeric
(__fmap_option opt (fn [x] (* x 2)))
(__bind_option opt (fn [x] (__opt_some (* x 2))))
```

```sweet-exp
__fmap_option(opt (fn [x] {x * 2}))
__bind_option(opt (fn [x] __opt_some({x * 2})))
```

## Container Values at Runtime

A hand-rolled HKT container (like the `__opt_*` Option used throughout this
guide) can be represented as an opaque `int64_t` handle. Build and read the
handles through the codegen preamble's Option helpers (`tur_box_some` /
`tur_is_some` / `tur_opt_value` / `TUR_NONE`), which construct the canonical
tagged layout -- SR2b retired the old hand-rolled
`{ bool is_some; int64_t value; }` struct, and spelling any layout by hand is
exactly the drift the helpers exist to prevent. (Built-in containers such as
`Option`/`Result` use the by-value monomorphized ABI instead -- see the
[monomorphization ABI guide](monomorphization-abi-guide.md)):

```turmeric
(defn __opt_some [x] : int
  ```c return tur_box_some(x); ```)

(defn __opt_none [] : int
  ```c return TUR_NONE; ```)
```

```sweet-exp
defn __opt_some [x] :int
  ```c return tur_box_some(x); ```

defn __opt_none [] :int
  ```c return TUR_NONE; ```
```

Implement `fmap` and `bind` using inline C to call the closure function pointer:

```turmeric
(defn __fmap_option [container fn] : int
  ```c
  if (!tur_is_some(container)) return TUR_NONE;
  return tur_box_some(
      ((int64_t(*)(int64_t))(intptr_t)fn)(tur_opt_value(container)));
  ```)

(defn __bind_option [ma fn] : int
  ```c
  if (!tur_is_some(ma)) return TUR_NONE;
  return ((int64_t(*)(int64_t))(intptr_t)fn)(tur_opt_value(ma));
  ```)
```

```sweet-exp
defn __fmap_option [container fn] :int
  ```c
  if (!tur_is_some(container)) return TUR_NONE;
  return tur_box_some(
      ((int64_t(*)(int64_t))(intptr_t)fn)(tur_opt_value(container)));
  ```

defn __bind_option [ma fn] :int
  ```c
  if (!tur_is_some(ma)) return TUR_NONE;
  return ((int64_t(*)(int64_t))(intptr_t)fn)(tur_opt_value(ma));
  ```
```

## Standard HKT Typeclasses

The standard library (`stdlib/typeclass.tur`) provides:

| Typeclass    | Kind       | Methods                           |
|-------------|-----------|-----------------------------------|
| `Functor`   | `* -> *`   | `fmap [container fn]`             |
| `Applicative` | `* -> *` | `pure [x]`, `ap [fn-container container]` |
| `Monad`     | `* -> *`   | `bind [ma fn]`                    |
| `Foldable`  | `* -> *`   | `fold-left`, `fold-right`         |
| `Traversable` | `* -> *` | `traverse [container fn]`         |
| `Bifunctor` | `* -> * -> *` | `bimap [container fn-left fn-right]` |

## Functor Laws Example

```turmeric
(defclass Functor [^f]
  (fmap [container fn] : int))

(defn id [x] : int x)
(defn times2 [x] : int (* x 2))
(defn inc [x] : int (+ x 1))

(definstance Functor [option]
  (fmap [container fn] (__fmap_option container fn)))

(defn main [] : int
  (do
    ;; Identity law: fmap id x = x
    (let [opt    (__opt_some 42)
          result (__fmap_option opt id)]
      (println (__opt_unwrap result)))  ;; 42

    ;; Composition law: fmap (f . g) x = fmap f (fmap g x)
    (let [opt (__opt_some 5)
          lhs (__fmap_option opt (fn [x] (times2 (inc x))))
          rhs (__fmap_option (__fmap_option opt inc) times2)]
      (println (= (__opt_unwrap lhs) (__opt_unwrap rhs))))  ;; true
    0))
```

```sweet-exp
defclass Functor [^f]
  fmap [container fn] :int

defn id [x] :int x
defn times2 [x] :int {x * 2}
defn inc [x] :int {x + 1}

definstance Functor [option]
  fmap [container fn] __fmap_option(container fn)

defn main [] :int
  do
    ;; Identity law: fmap id x = x
    let [opt    __opt_some(42)
         result __fmap_option(opt id)]
      println(__opt_unwrap(result))  ;; 42

    ;; Composition law: fmap (f . g) x = fmap f (fmap g x)
    let [opt __opt_some(5)
         lhs __fmap_option(opt (fn [x] times2(inc(x))))
         rhs __fmap_option(__fmap_option(opt inc) times2)]
      println({__opt_unwrap(lhs) = __opt_unwrap(rhs)})  ;; true
    0
```

## Monad Chaining

Use `bind` directly to chain monadic operations:

```turmeric
;; Sequence two Option computations
(let [step1  (__bind_option (__opt_some 3) (fn [x] (__opt_some (* x 2))))  ;; step1 = some 6
      result (__bind_option step1 (fn [y] (__opt_some (+ y 1))))]           ;; result = some 7
  (println (__opt_unwrap result)))  ;; 7
```

```sweet-exp
;; Sequence two Option computations
let [step1  __bind_option(__opt_some(3) (fn [x] __opt_some({x * 2})))  ;; step1 = some 6
     result __bind_option(step1 (fn [y] __opt_some({y + 1})))]         ;; result = some 7
  println(__opt_unwrap(result))  ;; 7
```

## do-m Notation

The `do-m` macro provides monadic do-notation. It desugars to nested `.bind` calls:

```turmeric
;; (do-m x ma1 y ma2 body) desugars to:
;; (.bind ma1 (fn [x] (.bind ma2 (fn [y] body))))
```

```sweet-exp
;; do-m(x ma1 y ma2 body) desugars to:
;; .bind(ma1 (fn [x] .bind(ma2 (fn [y] body))))
```

Simple usage (single binding, no variable capture in body):

```turmeric
(definstance Monad [option]
  (bind [ma fn] (__bind_option ma fn)))

;; Single expression: returned as-is
(let [r (do-m (__opt_some 42))]
  (println (__opt_unwrap r)))  ;; 42

;; One binding: desugars to .bind call
(let [r (do-m x (__opt_some 5) (__opt_some (* x 3)))]
  (println (__opt_unwrap r)))  ;; 15

;; None propagates automatically
(let [r (do-m x (__opt_none) (__opt_some (* x 3)))]
  (println (__opt_some? r)))  ;; false
```

```sweet-exp
definstance Monad [option]
  bind [ma fn] __bind_option(ma fn)

;; Single expression: returned as-is
let [r do-m(__opt_some(42))]
  println(__opt_unwrap(r))  ;; 42

;; One binding: desugars to .bind call
let [r do-m(x __opt_some(5) __opt_some({x * 3}))]
  println(__opt_unwrap(r))  ;; 15

;; None propagates automatically
let [r do-m(x __opt_none() __opt_some({x * 3}))]
  println(__opt_some?(r))  ;; false
```

## Closures with fmap

Both non-capturing and capturing closures work with `.fmap` and `.bind`
typeclass dispatch.

```turmeric
;; Non-capturing closure
(let [opt    (__opt_some 10)
      result (.fmap opt (fn [x] (* x x)))]
  (println (__opt_unwrap result)))  ;; 100

;; Capturing closure -- delta is captured from the enclosing scope
(let [delta  5
      opt    (__opt_some 10)
      result (.fmap opt (fn [x] (+ x delta)))]
  (println (__opt_unwrap result)))  ;; 15

;; Multi-capture
(let [a      2
      b      3
      opt    (__opt_some 10)
      result (.fmap opt (fn [x] (+ (* x a) b)))]
  (println (__opt_unwrap result)))  ;; 23

;; Capturing closure through .bind
(let [scale 3
      r     (.bind (__opt_some 4) (fn [x] (__opt_some (* x scale))))]
  (println (__opt_unwrap r)))  ;; 12
```

```sweet-exp
;; Non-capturing closure
let [opt    __opt_some(10)
     result .fmap(opt (fn [x] {x * x}))]
  println(__opt_unwrap(result))  ;; 100

;; Capturing closure -- delta is captured from the enclosing scope
let [delta  5
     opt    __opt_some(10)
     result .fmap(opt (fn [x] {x + delta}))]
  println(__opt_unwrap(result))  ;; 15

;; Multi-capture
let [a      2
     b      3
     opt    __opt_some(10)
     result .fmap(opt (fn [x] {{x * a} + b}))]
  println(__opt_unwrap(result))  ;; 23

;; Capturing closure through .bind
let [scale 3
     r     .bind(__opt_some(4) (fn [x] __opt_some({x * scale})))]
  println(__opt_unwrap(r))  ;; 12
```

Named functions (non-closures) also work unchanged:

```turmeric
(defn double [x] : int (* x 2))

(let [opt    (__opt_some 7)
      result (.fmap opt double)]
  (println (__opt_unwrap result)))  ;; 14
```

```sweet-exp
defn double [x] :int {x * 2}

let [opt    __opt_some(7)
     result .fmap(opt double)]
  println(__opt_unwrap(result))  ;; 14
```

`do-m` with captured variables works too:

```turmeric
(let [factor 3
      r      (do-m x (__opt_some 5) (__opt_some (* x factor)))]
  (println (__opt_unwrap r)))  ;; 15
```

```sweet-exp
let [factor 3
     r      do-m(x __opt_some(5) __opt_some({x * factor}))]
  println(__opt_unwrap(r))  ;; 15
```

## Binary Type Constructors (Bifunctor)

```turmeric
(defclass Bifunctor [^^f]
  (bimap [container fn-left fn-right] : int))

(definstance Bifunctor [pair]
  (bimap [container fn-left fn-right]
    (__bimap_pair container fn-left fn-right)))
```

```sweet-exp
defclass Bifunctor [^^f]
  bimap [container fn-left fn-right] :int

definstance Bifunctor [pair]
  bimap [container fn-left fn-right]
    __bimap_pair(container fn-left fn-right)
```

## Type-Level Rows (`#row{...}`)

A **row** is a compile-time list of types, written `#row{A B C}`. Rows are the
variadic member of the kind family: where `^f` marks a type parameter of kind
`* -> *` and `^^f` marks `* -> * -> *`, `^&r` marks a parameter of kind `[*]`
-- "list of types".

Rows exist to let a type record *which* types a value is indexed by, when the
count is not fixed in advance: an ECS query parameterised by its component set,
a data frame parameterised by its column schema, a SQL row type. They were
built for the ECS query layer and are used today by the `frame`, `sqlite`,
`httpd`, and `postgres` spices.

### Rows are type-level only

This is the single most important fact about rows, and the source of most
confusion:

> A row is a **phantom type argument**. It exists during type checking and is
> **completely erased at codegen**. A row is never the type of a runtime value.

`(Query #row{Position Velocity})` and `(Query #row{Health})` are distinct
*types*, but the compiled C for both carries only the struct's real fields --
the row never appears in a field, a signature, or a runtime tag. Rows cost
nothing at runtime because at runtime they do not exist.

### Declaring a row parameter

Mark the parameter with `^&` on `defstruct`, `deftype`, `defdata`, `defgadt`,
or `defn`:

```turmeric
(defopaque Position :int)
(defopaque Velocity :int)

(defstruct Query [^&components] (world :int))

(defn run-system [q : (Query #row{Position Velocity})] : int
  (.world q))
```

```sweet-exp
defopaque Position :int
defopaque Velocity :int

defstruct Query [^&components]
  world :int

defn run-system [q : (Query #row{Position Velocity})] : int
  .world(q)
```

On `defn`, `^&r` introduces a **row-polymorphic** function. The `^&` is
stripped, so the annotations reference the bare name:

```turmeric
(defstruct Frame [^&cols] (n :int))

(defn frame-id [^&r] [f : (Frame r)] : (Frame r) f)
```

### Two literal forms

**Bare positional** -- slots are element types:

```turmeric
#row{Position Velocity Health}
```

**Typed-field** -- slots carry a field name *and* an element type. Field names
participate in type equality, so two rows with identical element types but
different names are distinct:

```turmeric
(defstruct Tbl [^&cols] (rows :int))

(defn make-users [n : int] : (Tbl #row{id : int  name : cstr})
  (:: (make-struct Tbl n) (Tbl #row{id : int  name : cstr})))
```

The two forms cannot be mixed inside one literal (`TUR-E0290`), and duplicate
field names are rejected (`TUR-E0291`). An empty `#row{}` is the unit row.
Rows are order-significant, keep duplicates, do not nest-flatten, and cap at
255 elements.

### Row algebra

Four operators compute new rows from old ones, entirely at compile time:

| Operator | Meaning |
|---|---|
| `(row-concat A B ...)` | `A ++ B`, order-preserving, duplicates kept |
| `(row-union A B ...)` | set-union (deduplicated join) |
| `(row-intersect A B ...)` | elements common to all operands |
| `(row-canon R)` | canonical copy, sorted by type name |

A computed row is just a type, so it can appear anywhere a literal can. Two
queries compose by `row-union`:

```turmeric
(defn query [w : int] : (Query (row-union #row{Position Velocity}
                                          #row{Velocity Health}))
  (:: (make-struct Query w) (Query #row{Position Velocity Health})))
```

Ordinary row equality is **order-sensitive**. `row-canon` is the opt-in escape
hatch when column order should not matter: `(row-canon #row{int bool})` and
`(row-canon #row{bool int})` reduce to the same row, so ordinary type equality
then agrees. Use it on both sides of the boundary you want to be
permutation-insensitive.

### Where a row may not appear

Rows are rejected in three positions, each deliberately:

1. **As a value expression.** `#row{...}` in expression position is an error:
   *"is a type-level row and can only appear in a type annotation."*
2. **As the type of a value** -- `TUR-E0012`: *"a value cannot have row type. A
   row may only appear as a type argument to a row-kinded (`^&`) constructor
   parameter."* This fires in every value-type sub-position: parameters,
   returns, struct fields, let bindings, arrow arguments.
3. **Applied to something.** A row has kind `[*]`, not an arrow kind, so it
   cannot be applied, and passing a row where kind `*` is expected (or a
   non-row where `^&` is expected) is a kind error.

These are guards, not gaps. Guard 2 exists because a row in value-type
position was once silently accepted and *lost its elements*; the check now
sits in the shared wrapper so it cannot be bypassed. There is no `Row r`
value-wrapper type, and adding one is not a small change: because rows erase,
distinguishing two rows at a value boundary would require passing runtime
**witnesses**, which is exactly the indirection rows were designed to avoid.

If you want to operate on a row's contents at runtime, you do not want a row
-- you want a real value (a vector, a struct, a tuple). Reach for the row when
you want the *type checker* to track a set of types with zero runtime cost.

### Current limits

- **No term-level row operations.** There is no `(k in r)` membership
  predicate and no generic `column-of` accessor; per-column accessors are
  hand-written and hand-typed.
- **No extensible records.** Rows are not the `{ x : int | r }` open-row
  system from the ML/PureScript tradition -- there is no record extension,
  field insert/delete, or row restriction. "Row-polymorphic" here means a type
  parameter of kind `[*]` threaded phantom-ly.
- **Permutation equality is opt-in only.** The checker never applies it
  implicitly; `row-canon` is the only route.
- **Element resolution is lenient** in some positions -- an unregistered
  element name can produce an opaque placeholder rather than an error.
- **`deftype` carries no per-parameter kind array**, so its application sites
  fall back to arity-only checking.

## Performance

### Dispatch model: dictionary passing

By default Turmeric implements typeclass method calls via **dictionary passing**.
For each typeclass instance, the compiler generates a struct ("dictionary") that
holds one function pointer per method:

```c
/* generated for (definstance Functor [option] ...) */
/* fn params annotated with :fn receive tur_poly_fn_t so fat closures work */
typedef struct { int64_t (*fmap)(int64_t container, tur_poly_fn_t fn); } dict_Functor_option;
static dict_Functor_option __dict_Functor_option = { .__fmap_option };
```

At a call site such as `.fmap opt f`, the compiler locates the relevant dictionary
at compile time and emits a direct function-pointer call through it.  The overhead
is one pointer dereference, comparable to a C virtual dispatch.

### Controlling the C back-end optimisation level

`tur build` and `tur run` shell out to a C compiler.  Two environment variables
let you control that step:

| Variable | Default | Effect |
|---|---|---|
| `CC` | `cc` | C compiler executable |
| `TUR_CC_FLAGS` | `-O2 -std=c99 -Wall -fno-strict-aliasing` | Flags passed to every `cc` invocation |

Examples:

```sh
# Ship a fast binary (aggressive optimisation, link-time optimisation)
CC=clang TUR_CC_FLAGS="-O3 -flto -std=c99" tur build app.tur -o app

# Quick iteration build (fast compile, no optimisation)
TUR_CC_FLAGS="-O0 -std=c99" tur build app.tur -o app

# Debug symbols + sanitizer
CC=clang TUR_CC_FLAGS="-O1 -g -fsanitize=address -std=c99" tur build app.tur -o app
```

Both variables are inherited by `tur run` and by the test runner (`TUR_CC_FLAGS`
is explicitly documented in `tests/run.sh`).

### Benchmarking HKT dispatch overhead

The benchmark suite in `tests/benchmarks/` measures typeclass dispatch overhead.
The primary benchmark is `hkt-dict-pass.tur`, which exercises `Functor.fmap` over
`option` in a tight loop:

```sh
./tests/run-bench.sh                  # run all benchmarks
./tests/run-bench.sh hkt-dict-pass    # run only the HKT dispatch benchmark
BENCHMINIT=10000 ./tests/run-bench.sh # increase minimum iterations
```

Results are written to `tests/benchmarks/output/`.

### Monomorphization

There is no separate `-O` flag for this: the compiler monomorphizes by
default. The elaborator specialises polymorphic defns and by-value HKT
instance methods per concrete type at each call site, emitting
`__spec__`-suffixed symbols (e.g. `Functor_fmap__spec__Option__int`)
with no dictionary indirection on the specialised path. Dictionary
passing remains the mechanism for the residual cases that have no
by-value rewrite (see the
[monomorphization ABI guide](monomorphization-abi-guide.md) for the
full model, the spec-symbol naming rules, and the remaining carrier
bridge).

## Recursive Types

Turmeric supports self-referential and mutually-recursive `defdata`/`defstruct`
definitions. Recursive `defdata` payloads are heap-boxed and referenced as
opaque `int64_t` pointers, so recursive field types require no special
C-level treatment -- the elaborator recognises the type name and records the
field as the pointer-width carrier.

### Self-referential ADTs

```turmeric
(defdata IntList
  (Cons :int :IntList)
  (Nil))

(defn sum [lst : int] : int
  (match lst
    (Cons h t) (+ h (sum t))
    (Nil)      0))
```

```sweet-exp
defdata IntList
  (Cons :int :IntList)
  (Nil)

defn sum [lst :int] :int
  match lst
    (Cons h t)
    {h + sum(t)}
    (Nil)
    0
```

### Mutually-recursive ADTs

```turmeric
(defdata Expr
  (Lit :int)
  (Add :Expr :Expr))

(defn eval-expr [e : int] : int
  (match e
    (Lit n)   n
    (Add l r) (+ (eval-expr l) (eval-expr r))))
```

```sweet-exp
defdata Expr
  (Lit :int)
  (Add :Expr :Expr)

defn eval-expr [e :int] :int
  match e
    (Lit n)
    n
    (Add l r)
    {eval-expr(l) + eval-expr(r)}
```

### Fix -- fixed-point of a functor

`stdlib/fix.tur` provides the `Fix` type and the catamorphism/anamorphism pair:

```turmeric
(load "stdlib/fix.tur")

(defdata NatF [^f]
  (ZeroF)
  (SuccF :int))

(defn to-nat [n : int] : int
  (if (= n 0)
    (roll (ZeroF))
    (roll (SuccF (to-nat (- n 1))))))
```

```sweet-exp
load("stdlib/fix.tur")

defdata NatF [^f]
  (ZeroF)
  (SuccF :int)

defn to-nat [n :int] :int
  if {n = 0}
    roll(ZeroF())
    roll(SuccF(to-nat({n - 1})))
```

See `stdlib/fix.tur` for the full API.

### Free monad

`stdlib/free.tur` implements the Free monad, enabling pure DSL interpreters
without committing to a concrete effect type:

```turmeric
(load "stdlib/free.tur")

;; Define a small effect algebra
(defdata CalcOp
  (CalcAdd :int :int)
  (CalcMul :int :int))

;; Lift operations into Free
(defn calc-add [a : int b : int] : int (free-lift (CalcAdd a b)))

;; Interpret with free-run
(defn run-op [op : int] : int
  (match op
    (CalcAdd a b) (+ a b)
    (CalcMul a b) (* a b)))

(defn main [] : int
  (let [prog (calc-add 3 4)]
    (println (free-run (fn [op] (let [_ prog] (run-op op))) prog))
    0))
```

```sweet-exp
load("stdlib/free.tur")

;; Define a small effect algebra
defdata CalcOp
  (CalcAdd :int :int)
  (CalcMul :int :int)

;; Lift operations into Free
defn calc-add [a :int b :int] :int free-lift(CalcAdd(a b))

;; Interpret with free-run
defn run-op [op :int] :int
  match op
    (CalcAdd a b)
    {a + b}
    (CalcMul a b)
    {a * b}

defn main [] :int
  let [prog calc-add(3 4)]
    println(free-run((fn [op] let([_ prog] run-op(op))) prog))
    0
```

See `stdlib/free.tur` for `free-pure`, `free-lift`, `free-bind`, `free-fmap`,
and `free-run`.

## Known Limitations

1. **Method dispatch on containers**: Since HKT container values are stored as
   `int64_t`, type-based dispatch may fall back to the first matching instance.
   When exactly one typeclass instance is in scope for the given method, the
   fallback is accepted silently and the program behaves correctly. When two or
   more instances are in scope and the receiver type has been erased to
   `int64_t`, the compiler emits `TUR_E0020_AMBIGUOUS_DISPATCH` (Phase D0).
   Use the `@TypeName` witness syntax (Phase D1) to resolve the ambiguity at
   zero runtime cost:

   ```turmeric
   (.fmap @option opt f)   ; disambiguate: use the option Functor instance
   ```

   ```sweet-exp
   .fmap(@option opt f)   ; disambiguate: use the option Functor instance
   ```

   See `docs/archive/hkt-opaque-dispatch-plan.md` for background.

2. **`defkind`**: Currently parsed and ignored. Future versions may use it for documentation generation and kind inference.

3. **Rows are type-level only**: `#row{...}` cannot be a value or the type of a
   value. See [Type-Level Rows](#type-level-rows-row) above for the full rules
   and the reasoning behind them.

## See also

- [gadts-guide.md](gadts-guide.md) -- GADTs, `defgadt`, and equality witnesses
