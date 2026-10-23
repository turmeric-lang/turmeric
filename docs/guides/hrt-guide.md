---
title: Higher-Ranked Types
category: Type System
description: Higher-ranked types: rank-2/3 polymorphic function parameters
---

# Higher-Ranked Types in Turmeric

This guide explains how to use higher-ranked types (HRTs) in Turmeric: what they are, when to use them, and common patterns.

## What are higher-ranked types?

A **rank-1 type** is a normal monomorphic function type: `(-> int int)`.

A **rank-2 type** introduces a `forall` *inside* a function argument. The classic example is `(forall [a] (-> a a))` -- a function that works for any type `a`. When you pass such a function as an argument, the *callee* gets to choose `a` for each use, not the caller.

```turmeric
; apply-poly works for any type -- the callee instantiates `a`
(defn apply-poly [f (forall [a] (-> a a)) x : int] : int
  (f x))
```

```sweet-exp
; apply-poly works for any type -- the callee instantiates `a`
defn apply-poly [f (forall [a] (-> a a)) x :int] :int
  f(x)
```

Without rank-2 types, you'd need to pick a concrete type for `f` at the call site, losing the generality.

Higher-ranked types are always available -- no flag or `--enable` is
needed.

## Syntax

### Rank-2 parameter annotation

Annotate a parameter with a `forall` type by placing the type form immediately after the parameter name in the `[]` vector:

```turmeric
(defn my-fn [f (forall [a] (-> a a)) x : int] : int
  (f x))
```

```sweet-exp
defn my-fn [f (forall [a] (-> a a)) x :int] :int
  f(x)
```

The `forall` type must appear as a parenthesized form `(forall [...] ...)` directly following the parameter symbol.

### Multiple type variables

```turmeric
(defn swap-apply [f (forall [a b] (-> a b a)) x : int y : int] : int
  (f x y))
```

```sweet-exp
defn swap-apply [f (forall [a b] (-> a b a)) x :int y :int] :int
  f(x y)
```

### What you may pass

A `forall [a]` parameter is a promise to the callee: it may apply the function
at any type it likes. So the function a caller passes must be generic wherever
the forall body has a bare bound variable -- its parameter there is a type
parameter or un-annotated:

```turmeric
(defn id [A] [x : A] : A x)    ; generic: OK
(defn id-word [x] x)           ; un-annotated, one untyped word: OK
(defn inc [x : int] : int (+ x 1))

(apply-poly id 7)              ; => 7
(apply-poly inc 7)             ; error[TUR-E0001]: rank-2 argument 'inc' is not generic
```

```sweet-exp
defn id [A] [x :A] :A x
defn id-word [x] x
defn inc [x :int] :int {x + 1}

apply-poly(id 7)               ; => 7
apply-poly(inc 7)              ; error[TUR-E0001]: rank-2 argument 'inc' is not generic
```

An `any` parameter is refused too, although it accepts every type: `any` is
a tagged value, and a forall argument is called with one untyped word, so the
function would read a garbage tag (`(defn ida [x : any] : any x)` passed to
`apply-poly` printed `0`). Write it with a type parameter instead.

Without the check, `inc` would be applied to whatever type the callee chose --
a callee that also did `(f "hi")` added one to the string's address. A lambda
is checked the same way, and so is a function defined later in the file.

For a constrained forall, `(forall [a] [(Show a)] (-> a cstr))`, pass a
generic function with the same constraint: `(defn shown [A] [(Show A)] [x : A]
: cstr (show x))`.

**The opt-out.** When you know the callee only ever applies the function at a
type it handles, ascribe it to the forall type. The ascription is trusted, not
checked:

```turmeric
(apply-poly (:: inc (forall [a] (-> a a))) 7)  ; => 8
```

```sweet-exp
apply-poly((:: inc (forall [a] (-> a a))) 7)  ; => 8
```

Not checked yet: the function's result type, and a bound variable under a type
application, such as a parameter of type `(List a)`.

### Existential types

Use `exists` to hide a type implementation:

```turmeric
(defn make-counter [] : (exists [s] s)
  (pack 0 (exists [s] s)))

(defn use-counter [c : (exists [s] s)] : int
  (open c [s v] 1))
```

```sweet-exp
defn make-counter [] : (exists [s] s)
  pack(0 (exists [s] s))

defn use-counter [c : (exists [s] s)] : int
  open(c [s v] 1)
```

`(pack value (exists ...))` boxes a value; `(open e [a v] body)` unpacks it, with
`v` of the hidden type `a` (it cannot be returned as anything else).

See `docs/archive/existential-types-plan.md` for the full existential type spec.

## Common patterns

### Pattern 1: Uniform transformation

Apply a single function uniformly over data, without fixing its type:

```turmeric
(defn apply-to-both [f (forall [a] (-> a a)) x : int y : int] : int
  (+ (f x) (f y)))

(defn inc [x : int] : int (+ x 1))

; `inc` is int-only and apply-to-both uses it only at int, so it is ascribed
; to the forall type (see "What you may pass"); it is wrapped automatically
(apply-to-both (:: inc (forall [a] (-> a a))) 3 7)  ; => 12
```

```sweet-exp
defn apply-to-both [f (forall [a] (-> a a)) x :int y :int] :int
  {f(x) + f(y)}

defn inc [x :int] :int
  {x + 1}

; `inc` is int-only and apply-to-both uses it only at int, so it is ascribed
; to the forall type (see "What you may pass"); it is wrapped automatically
apply-to-both((:: inc (forall [a] (-> a a))) 3 7)  ; => 12
```

### Pattern 2: Church-numeral iteration

Apply a polymorphic function n times -- the essence of Church numerals:

```turmeric
(defn church-apply [f (forall [a] (-> a a)) n : int x : int] : int
  (if (<= n 0)
    x
    (church-apply f (- n 1) (f x))))

(church-apply (:: inc (forall [a] (-> a a))) 5 0)       ; => 5
(church-apply (:: double-it (forall [a] (-> a a))) 3 1) ; => 8
```

```sweet-exp
defn church-apply [f (forall [a] (-> a a)) n :int x :int] :int
  if {n <= 0}
    x
    church-apply(f {n - 1} f(x))

church-apply((:: inc (forall [a] (-> a a))) 5 0)       ; => 5
church-apply((:: double-it (forall [a] (-> a a))) 3 1) ; => 8
```

This works because HRT5 correctly propagates the poly fn type through recursive calls.

### Pattern 3: CPS-style composition

Thread a value through a sequence of polymorphic transforms:

```turmeric
(defn pipe2 [f (forall [a] (-> a a)) g (forall [a] (-> a a)) x : int] : int
  (g (f x)))

(pipe2 (:: inc (forall [a] (-> a a))) (:: double-it (forall [a] (-> a a))) 3)
; => (3+1)*2 = 8
```

```sweet-exp
defn pipe2 [f (forall [a] (-> a a)) g (forall [a] (-> a a)) x :int] :int
  g(f(x))

pipe2((:: inc (forall [a] (-> a a))) (:: double-it (forall [a] (-> a a))) 3)
; => (3+1)*2 = 8
```

### Pattern 4: Let-binding of poly fn aliases

You can bind a poly fn to a local name and use it multiple times:

```turmeric
(defn apply-poly [f (forall [a] (-> a a)) x : int] : int
  (f x))

(defn id [A] [x : A] : A x)

(defn use-poly-id [] : int
  (let [f id]  ; f is a poly fn alias for id
    (+ (apply-poly f 10) (apply-poly f 20))))
```

```sweet-exp
defn apply-poly [f (forall [a] (-> a a)) x :int] :int
  f(x)

defn id [A] [x :A] :A x

defn use-poly-id [] :int
  let [f id]  ; f is a poly fn alias for id
    {apply-poly(f 10) + apply-poly(f 20)}
```

The `source_binding` mechanism tracks `f` back to `id` so the correct wrapper is generated.

### Pattern 5: Forwarding poly fn parameters

Pass a received poly fn parameter to another function expecting a poly fn:

```turmeric
(defn apply-once [f (forall [a] (-> a a)) x : int] : int
  (f x))

(defn apply-twice [f (forall [a] (-> a a)) x : int] : int
  (apply-once f (apply-once f x)))  ; f forwarded directly
```

```sweet-exp
defn apply-once [f (forall [a] (-> a a)) x :int] :int
  f(x)

defn apply-twice [f (forall [a] (-> a a)) x :int] :int
  apply-once(f apply-once(f x))  ; f forwarded directly
```

No wrapper is created when forwarding -- the existing `tur_poly_fn_t` is passed through.

### Pattern 6: Typeclass methods with rank-N parameters

Define typeclass methods that accept polymorphic function arguments:

```turmeric
(defclass Transform []
  (transform [[f (forall [a] (-> a a))] x] : int))

(definstance Transform []
  (transform [f x] (f x)))

; Call with a generic function, or ascribe an int-only one
(.transform id 5)                               ; => 5
(.transform (:: inc (forall [a] (-> a a))) 5)  ; => 6
```

```sweet-exp
defclass Transform []
  transform([[f (forall [a] (-> a a))] x] :int)

definstance Transform []
  transform([f x] f(x))

; Call with a generic function, or ascribe an int-only one
.transform(id 5)                               ; => 5
.transform((:: inc (forall [a] (-> a a))) 5)  ; => 6
```

### Pattern 7: Rank-3 types

A function whose parameter is itself rank-2 -- it accepts a "polymorphic function handler":

```turmeric
(defn with-poly-id [h (forall [a] (-> (forall [b] (-> b b)) a))] : int
  (h id))
```

```sweet-exp
defn with-poly-id [h (forall [a] (-> (forall [b] (-> b b)) a))] :int
  h(id)
```

Rank-3 arguments are passed by pointer internally (compound literal protocol).

## Limitations

### Container storage not supported

Storing `(forall [a] ...)` values in lists, options, or other containers is not
supported. The poly fn representation (`tur_poly_fn_t`) is a struct, not an
opaque `int64_t`, so it cannot be stored in a generic container without a
wrapper. Use direct function calls or closure capture instead.

### Non-function values cannot be rank-2 arguments

```turmeric
; ERROR: 5 is not a function
(apply-poly 5 42)  ; => error: arg 1: expected ptr<void>, got int
```

```sweet-exp
; ERROR: 5 is not a function
apply-poly(5 42)  ; => error: arg 1: expected ptr<void>, got int
```

### No rank-2 inference at call sites

You must annotate rank-2 parameters explicitly in `defn`. The compiler does not infer rank-N types from usage:

```turmeric
; Without annotation, this is just (-> ptr<void> int int), not a rank-2 fn
(defn apply [f x])  ; NOT rank-2

; With annotation, it's a proper rank-2 function
(defn apply [f (forall [a] (-> a a)) x : int] : int  ; rank-2
  (f x))
```

```sweet-exp
; Without annotation, this is just (-> ptr<void> int int), not a rank-2 fn
defn apply [f x]  ; NOT rank-2

; With annotation, it's a proper rank-2 function
defn apply [f (forall [a] (-> a a)) x :int] :int  ; rank-2
  f(x)
```

### Closures cannot be rank-2 arguments directly

Only named global functions (and let-bound aliases of global functions) can be passed as rank-2 arguments. Closures (functions capturing variables) require special support.

## Runtime representation

A rank-2 polymorphic function value is represented as `tur_poly_fn_t`:

```c
typedef struct { void *env; int64_t (*fn)(void *, int64_t); } tur_poly_fn_t;
```

The compiler automatically:

1. Wraps a global function into a `tur_poly_fn_t` at the call site
2. Calls `f.fn(f.env, x)` inside the callee body
3. Passes `tur_poly_fn_t` by value when forwarding between functions

## Error messages

| Error | Cause | Fix |
|-------|-------|-----|
| `arg N: expected ptr<void>, got int` | Non-function passed as poly fn arg | Pass a named function instead |
| `rank-2 argument must be a named function` | Tried to pass a non-global binding | Use a top-level `defn` instead of a local fn |
| `rank-2 argument 'f' is not generic` (TUR-E0001) | The function's parameter is a concrete type where the forall has a bound variable | Make it generic (`[A] [x : A]`), or ascribe it to the forall type: `(:: f (forall ...))` |
| `function 'X' expects M argument(s), got N` | Arity mismatch | Check the poly fn param counts |

## See also

- [`tests/fixtures/hrt-*/`](https://github.com/turmeric-lang/turmeric/tree/main/tests/fixtures/) -- Working examples of every HRT feature
- [gadts-guide.md](gadts-guide.md) -- GADTs and equality witnesses; skolem equalities produced
  by GADT `match` arms interact with HRT bidirectional checking to enable type
  refinement without casts
- [`tests/fixtures/errors/hrt-*/`](https://github.com/turmeric-lang/turmeric/tree/main/tests/fixtures/errors/) -- Error cases with expected diagnostics
