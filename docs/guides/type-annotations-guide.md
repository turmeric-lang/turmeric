---
title: Type Annotation Syntax
category: Type System
description: Compound type annotation syntax: `(-> a b)`, `(vec T)`, `forall`, and more
---

# Type Annotation Syntax

Turmeric supports both simple keyword-style type annotations and compound type
expressions. This guide covers the full annotation syntax available today.

---

## Basic annotations

The simplest form is a colon followed by a type name. A space between `:` and the
name is required when the type is a symbol (not fused to a keyword):

```turmeric
;; Primitive types
(defn add [a : int  b : int] : int ...)
(defn ok? [r : ptr<void>]   : bool ...)
(defn greet [s : cstr]      : nil ...)

;; The old fused-keyword style is still accepted for primitives
(defn add [a :int b :int] :int ...)
```

```sweet-exp
;; Primitive types
defn add [a : int  b : int] : int ...
defn ok? [r : ptr<void>]   : bool ...
defn greet [s : cstr]      : nil ...

;; The old fused-keyword style is still accepted for primitives
defn add [a :int b :int] :int ...
```

New code should prefer the spaced form (`: int`) because it composes cleanly
with compound types.

### `nil` and `void` are the same type

A function that returns no value is annotated `: nil` or `: void`. These are
two spellings of one type (`TY_NIL`, emitted as C `void`) and are
interchangeable everywhere -- return position, parameter position, fused
(`:nil`) or spaced (`: nil`). Prefer `: nil`; `: void` is kept for interop
code that reads more naturally in C terms.

There is no `unit` type -- `: unit` is an error.

Note for anyone reading older code or bug reports: the two spellings used to
diverge in one place. A bare `nil` in type position parses as a nil *literal*,
not a symbol, and two forward-declaration pre-passes only unwrapped the symbol
shape, so a `: nil` callee that had not been elaborated yet was forward-typed
`int` and its call sites broke in the emitted C. Fixed 2026-08-29; see
[docs/archive/forward-referenced-nil-call-bound-to-auto-type.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/forward-referenced-nil-call-bound-to-auto-type.md).

---

## Compound type expressions

When the type is not a single name, write it as a parenthesised list after `: `:

```turmeric
;; Function type (arrow)
(defn apply [f : (-> int int)  x : int] : int ...)

;; Parameterised container
(defn sum [v : (vec int)] : int ...)

;; Nested
(defn transform [v : (vec (option int))] : (vec int) ...)
```

```sweet-exp
;; Function type (arrow)
defn apply [f : (-> int int)  x : int] : int ...

;; Parameterised container
defn sum [v : (vec int)] : int ...

;; Nested
defn transform [v : (vec (option int))] : (vec int) ...
```

### Function types: `(-> arg... ret)`

`->` takes one or more arguments; the last position is the return type:

```turmeric
(-> int)             ;; nullary function returning int
(-> int int)         ;; int -> int
(-> int int int)     ;; (int, int) -> int
(-> cstr (vec int))  ;; cstr -> (vec int)
```

```sweet-exp
(-> int)             ;; nullary function returning int
(-> int int)         ;; int -> int
(-> int int int)     ;; (int, int) -> int
(-> cstr (vec int))  ;; cstr -> (vec int)
```

### Container types

| Form | Meaning |
|---|---|
| `(vec T)` | Growable array of `T` |
| `(option T)` | Optional value (`some` / `none`) |
| `(result T E)` | Success or error (`ok` / `err`) |
| `(pair A B)` | Two-element pair |
| `(rc T)` | Reference-counted shared pointer |
| `(ref T)` | Unique, move-only reference |

```turmeric
(defn first  [p : (pair int bool)] : int ...)
(defn clone  [p : (rc Buffer)]     : (rc Buffer) ...)
(defn consume [r : (ref Socket)]   : unit ...)
```

```sweet-exp
defn first  [p : (pair int bool)] : int ...
defn clone  [p : (rc Buffer)]     : (rc Buffer) ...
defn consume [r : (ref Socket)]   : unit ...
```

### Polymorphic types

Use type variables (bare symbols) in generic function signatures:

```turmeric
(defn identity [x : a] : a ...)
(defn map-vec  [v : (vec a)  f : (-> a b)] : (vec b) ...)
```

```sweet-exp
defn identity [x : a] : a ...
defn map-vec  [v : (vec a)  f : (-> a b)] : (vec b) ...
```

### Universal and existential quantifiers

```turmeric
;; Universally quantified (explicit forall)
(defn id [a b] : (forall [a] (-> a a)) ...)

;; Existentially quantified
(defn pack [] : (exists [a] (pair a (-> a int))) ...)
```

```sweet-exp
;; Universally quantified (explicit forall)
defn id [a b] : (forall [a] (-> a a)) ...

;; Existentially quantified
defn pack [] : (exists [a] (pair a (-> a int))) ...
```

### Higher-kinded type arguments

For functions parameterised over a type constructor, use the `^f` / `^^f` kind
annotations (see [hkt-guide.md](hkt-guide.md)):

```turmeric
(defn fmap [^f x : (^f a)  fn : (-> a b)] : (^f b) ...)
```

```sweet-exp
defn fmap [^f x : (^f a)  fn : (-> a b)] : (^f b) ...
```

---

## The `|` operator in symbols

`|` is a valid symbol character, enabling Haskell- and Arrows-style operators:

```turmeric
(defn ||| [a : arr  b : arr] : arr ...)  ;; parallel composition
(defn |>  [x : a   f : (-> a b)] : b ...) ;; pipe
```

```sweet-exp
defn ||| [a : arr  b : arr] : arr ...  ;; parallel composition
defn |>  [x : a   f : (-> a b)] : b ... ;; pipe
```

---

## Annotations on let bindings

Type annotations work on `let` bindings too:

```turmeric
(let [x : int 42]
  (println x))

(let [f : (-> int int) (fn [n] (* n 2))]
  (println (f 21)))
```

```sweet-exp
let [x : int 42]
  println(x)

let [f : (-> int int) (fn [n] {n * 2})]
  println(f(21))
```

---

## Substructural annotations

Ownership annotations (`^unique`, `^linear`, `^affine`, `^relevant`) precede the
type expression:

```turmeric
(defn consume [^linear  fh : FileHandle] : unit ...)
(defn sort!   [^unique  v  : (vec int)]  : unit ...)
(defn log     [^relevant msg : str]      : unit ...)
```

```sweet-exp
defn consume [^linear  fh : FileHandle] : unit ...
defn sort!   [^unique  v  : (vec int)]  : unit ...
defn log     [^relevant msg : str]      : unit ...
```

See [substructural-types-guide.md](substructural-types-guide.md) and
[uniqueness-types-guide.md](uniqueness-types-guide.md).

---

## See also

- [hkt-guide.md](hkt-guide.md) -- higher-kinded types and kind annotations
- [hrt-guide.md](hrt-guide.md) -- rank-2 / rank-3 polymorphism
- [substructural-types-guide.md](substructural-types-guide.md) -- `^linear`, `^affine`, `^relevant`
- [uniqueness-types-guide.md](uniqueness-types-guide.md) -- `^unique`
