---
title: Uniqueness Types
category: Type Safety
description: `^unique`: at-most-one-reference ownership
tags:
  - uniqueness-types
  - ownership
  - linear
  - mutation
  - aliasing
---

# Uniqueness Types in Turmeric

Uniqueness types enforce the **at-most-one-reference** discipline: a value with `^unique`
has at most one live reference at any point. Unlike linear types (`^linear`, which enforce
exactly-once usage), a unique value may be dropped freely -- it just cannot be aliased.

Uniqueness checking is enabled by default; no flag is required.

---

## The two ownership primitives

| Kind | May be dropped | May be duplicated | Aliasing | Summary |
|---|---|---|---|---|
| `^unique` | Yes | No | No | At most one live reference |
| `^linear` | No | No | No | Must be used exactly once |
| (none) | Yes | Yes | Yes | No restrictions (default) |

`^unique` is the **aliasing axis**: you can drop it, but you cannot create a second
reference to the same value. `^linear` is the **usage-count axis**: you must consume it
exactly once.

`ref<T>` is modelled as `CK_UNIQUE` internally -- uniqueness types make this
explicit in the source language.

---

## Basic usage

### In-place mutation

```turmeric
;; sort! requires exclusive access -- no other reference may exist
(defn sort! [^unique v : (vec int)] : unit
  (in-place-quicksort v))

(defn example [] : unit
  (let [v (vec-new 10)]
    (sort! v)         ;; ownership transferred into sort!
    (println v)))     ;; OK: sort! returned
```

```sweet-exp
;; sort! requires exclusive access -- no other reference may exist
defn sort! [^unique v : (vec int)] : unit
  in-place-quicksort(v)

defn example [] : unit
  let [v vec-new(10)]
    sort!(v)         ;; ownership transferred into sort!
    println(v)       ;; OK: sort! returned
```

### Pipeline of unique operations

```turmeric
;; Each step hands off ownership to the next
(defn buf-write [^unique buf : Buffer data : cstr] : ^unique Buffer ...)

(defn pipeline [^unique buf : Buffer] : ^unique Buffer
  (-> buf
    (buf-write "header")
    (buf-write "body")
    (buf-write "footer")))
```

```sweet-exp
;; Each step hands off ownership to the next
defn buf-write [^unique buf : Buffer data : cstr] : ^unique Buffer ...

defn pipeline [^unique buf : Buffer] : ^unique Buffer
  -> buf
    buf-write("header")
    buf-write("body")
    buf-write("footer")
```

---

## Alias prevention

Creating a second binding that refers to the same unique value is a
compile-time error.

```turmeric
(defn modify [^unique ^mut x : int] : unit
  (set! x (+ x 1)))

(defn bad [] : unit
  (let [x 42
        y x]       ;; alias created
    (modify x)))  ;; ERROR TUR-E0200: 'x' is not unique (aliased by 'y')
```

```sweet-exp
defn modify [^unique ^mut x : int] : unit
  set!(x {x + 1})

defn bad [] : unit
  let [x 42
       y x]       ;; alias created
    modify(x)     ;; ERROR TUR-E0200: 'x' is not unique (aliased by 'y')
```

Passing a `^unique` value to a function **transfers ownership** -- the source
binding is consumed, just like a move.

---

## Unique mutable parameters: `^unique ^mut`

Combine `^unique` with `^mut` for exclusive mutable access: the argument is
moved in, and the parameter is mutable inside the body.

> **`^mut` on a parameter does NOT make it an out-parameter.** Turmeric passes
> by value, so `^mut` means "this binding is mutable *inside this body*", not
> "the caller will see my writes". A `defstruct` argument is copied into the
> callee, so `(set! (.field p) v)` mutates the callee's copy and the caller's
> value is unchanged -- in both the compiled backend and the interpreter.
> That is why `increment!` below *returns* `n` rather than relying on the
> mutation being visible.
>
> To let a callee mutate a value the caller can observe, use `rc<T>` (shared,
> reference-counted) or a `:heap` struct (interior-mutable by declaration).
> A `&Struct` receiver is rejected outright: `set! (.field s)` requires a
> struct or an `rc<Struct>`.

```turmeric
(defn increment! [^unique ^mut n : int] : ^unique int
  (set! n (+ n 1))
  n)
```

```sweet-exp
defn increment! [^unique ^mut n : int] : ^unique int
  set!(n {n + 1})
  n
```

Rules:

- A `^unique ^mut` parameter may mutate its own binding and return a new
  `^unique T`. Returning it is how the new value reaches the caller.
- You cannot create a `^unique ^mut` reference while any `&T` or `&mut T` borrows are live.

---

## Uniqueness inference

You do not have to hand-write `^unique` on a let-binding whose initializer is
already a uniquely-owned value. The elaborator infers uniqueness from the
initializer's type (`CK_UNIQUE` -- e.g. a `ref<T>` factory result or a call that
returns `^unique`) and applies the same alias / use-after-consume checks:

```turmeric
(defn make-ref [n : int] : ref
  (ref/from-rc (rc/of n)))

(defn example [] : unit
  (let [r (make-ref 42)]   ;; inferred ^unique -- no annotation needed
    (use-once r)
    (use-once r)))          ;; ERROR TUR-E0201: 'r' already consumed
```

```sweet-exp
defn make-ref [n : int] : ref
  ref/from-rc(rc/of(n))

defn example [] : unit
  let [r make-ref(42)]     ;; inferred ^unique -- no annotation needed
    use-once(r)
    use-once(r)             ;; ERROR TUR-E0201: 'r' already consumed
```

At an `if`/`match` join the uniqueness of the two arms is combined: two unique
arms join to unique, but a unique arm joined with a shared arm downgrades to
shared (a safe weakening -- the result may be aliased through the shared arm).
Re-annotate `^unique` on the binding to force the stricter discipline.

---

## Common patterns

`stdlib/unique.tur` (auto-loaded) captures the recurring linear-resource shapes so you stop open-coding the manual annotations:

| Form | Use |
|---|---|
| `(with-unique [name init] body...)` | Scope a `^unique` binding over a body without writing the annotation |
| `(consume x f)` | Thread a unique `x` through a single transforming call `(f x)` |
| `(replace old new)` | Displace a unique value, returning the old one (value-semantics `mem::replace` stand-in) |

```turmeric
;; Instead of hand-rolling (let [^unique buf (make-buf)] ...):
(with-unique [buf (make-buf)]
  (consume buf buf-finish))      ;; ownership threaded into buf-finish
```

```sweet-exp
with-unique [buf make-buf()]
  consume(buf buf-finish)        ;; ownership threaded into buf-finish
```

---

## Interaction with `rc<T>`

Wrapping a `^unique` value inside `rc<T>` is forbidden -- shared reference counting
would introduce aliases:

```turmeric
(rc/new my-unique-val)  ;; ERROR TUR-E0202: cannot wrap unique value in rc<T>
```

```sweet-exp
rc/new(my-unique-val)  ;; ERROR TUR-E0202: cannot wrap unique value in rc<T>
```

A closure that *consumes* a captured `^unique` value is itself `^unique`, so
`(rc/of that-closure)` is rejected the same way (and calling it twice is
`TUR-E0201`) -- see
[Closures inherit the discipline of what they consume](substructural-types-guide.md#closures-inherit-the-discipline-of-what-they-consume).

---

## Error codes

| Code | Meaning |
|---|---|
| `TUR-E0200` | Value is not unique -- aliased by another binding |
| `TUR-E0201` | Cannot copy a unique value |
| `TUR-E0202` | Cannot wrap a unique value in `rc<T>` |

Use `tur --explain TUR-E0200` (or any other code) for a detailed explanation and
fix suggestions.

---

## Relationship to other type disciplines

| Feature | Relationship |
|---|---|
| `ref<T>` | Internally `CK_UNIQUE`; `^unique` makes the constraint explicit |
| `&mut T` | Temporary unique borrow; `^unique` is a permanent ownership transfer |
| `^linear` | Orthogonal: linear controls usage count; unique controls alias count |
| `^affine` | `^affine` restricts duplication but not aliasing |
| `rc<T>` | Explicitly non-unique -- wrapping a `^unique` value is rejected |

---

## See also

- [Substructural Types Guide](substructural-types-guide.md) -- `^linear`, `^affine`, `^relevant`
- `tur --explain TUR-E0200` -- alias violation
- `tur --explain TUR-E0201` -- copy of unique value
