---
title: Substructural Types
category: Type Safety
description: `^linear`, `^affine`, `^relevant` type disciplines
tags:
  - substructural-types
  - linear
  - affine
  - relevant
  - borrowing
---

# Substructural Types in Turmeric

Turmeric supports three substructural type disciplines. Each discipline
restricts which structural rules a value may use:

| Annotation   | May be dropped | May be duplicated | Summary                    |
|--------------|---------------|-------------------|----------------------------|
| `^linear`    | No            | No                | Used exactly once          |
| `^affine`    | Yes           | No                | Used at most once          |
| `^relevant`  | No            | Yes               | Used at least once         |
| (none)       | Yes           | Yes               | No restrictions (default)  |

These disciplines are enabled by default; no flag is required.

---

## When to use each discipline

### `^linear` -- use exactly once

Use `^linear` for values that represent exclusive ownership of a resource, where both
dropping the resource unintentionally and aliasing it are errors.

```turmeric
(defn open-file  [path : cstr]             : ^linear FileHandle)
(defn close-file [^linear fh : FileHandle] : unit)

(defn copy-file [src dst : cstr] : unit
  (let [fh (open-file src)]
    ;; fh must be consumed exactly once
    (close-file fh)))
```

```sweet-exp
defn open-file  [path : cstr]             : ^linear FileHandle
defn close-file [^linear fh : FileHandle] : unit

defn copy-file [src dst : cstr] : unit
  let [fh open-file(src)]
    ;; fh must be consumed exactly once
    close-file(fh)
```

`ref<T>` bindings are automatically inferred as `^linear` unless another
annotation is present. You must explicitly `(drop! r)` or consume the ref --
it will not be silently freed.

**A linear value may be bound at module level.** Linearity constrains how many
times a value is *used*, not where it may live, so a top-level `def` holding
one is fine:

```turmeric
(def hub-mutex (mutex-new))   ; Mutex is (defopaque Mutex :ptr<void> :linear)
```

The initializer produces the value once, at static-init, and it lives for the
process -- one production, one lifetime. This is the intended spelling for a
process-lifetime mutex, connection pool, or shared registry; see
[mutable-globals-guide](mutable-globals-guide.md#any-type-may-be-bound-at-module-level).

(Older code sometimes casts such a global through `:int` and back at each
borrow. That worked around a codegen bug, not a linearity rule, and the bug was
fixed 2026-08-29.)

### `^affine` -- use at most once

Use `^affine` when a value may be discarded but must not be aliased. This is
appropriate for one-shot tokens, single-use callbacks, or initialization keys.

```turmeric
(defn initialize [^affine key : EncryptionKey] : unit
  ...)

;; OK: use once
(let [^affine k (generate-key)]
  (initialize k))

;; OK: discard without use (weakening allowed)
(let [^affine k (generate-key)]
  0)

;; ERROR TUR-E0150: affine value used more than once
(let [^affine k (generate-key)]
  (initialize k)
  (initialize k))
```

```sweet-exp
defn initialize [^affine key : EncryptionKey] : unit
  ...

;; OK: use once
let [^affine k generate-key()]
  initialize(k)

;; OK: discard without use (weakening allowed)
let [^affine k generate-key()]
  0

;; ERROR TUR-E0150: affine value used more than once
let [^affine k generate-key()]
  initialize(k)
  initialize(k)
```

### `^relevant` -- use at least once

Use `^relevant` for values that must be observed but may be inspected multiple times.
This is appropriate for audit logs, mandatory acknowledgements, or results that
must not be silently discarded.

```turmeric
(defn log-and-store [^relevant msg : str] : unit
  (log msg)     ;; first use (duplication OK)
  (store msg))  ;; second use

;; ERROR TUR-E0151: relevant value dropped without use
(let [^relevant msg "important event"]
  0)
```

```sweet-exp
defn log-and-store [^relevant msg : str] : unit
  log(msg)     ;; first use (duplication OK)
  store(msg)   ;; second use

;; ERROR TUR-E0151: relevant value dropped without use
let [^relevant msg "important event"]
  0
```

---

## Borrowing a linear handle: `^borrow` parameters

A `^linear` (or `^affine`) handle must be consumed exactly once, but resources
typically expose **non-consuming accessors** -- reads that observe the handle
and hand it back for further use (`fs/tmpfile-path`, `mutex-lock`,
`mutex-unlock`, ...). Marking such a parameter `^borrow` lets the function read
its argument **without consuming it**: the single-consumption obligation stays
with the caller for a later consuming op.

```turmeric
;; mutex-lock / mutex-unlock borrow; mutex-free consumes.
(defn mutex-lock [^borrow m : Mutex] : nil ...)
(defn mutex-free [m : Mutex]         : nil ...)   ;; consumes

(let [m (mutex-new)]
  (mutex-lock m)      ;; borrow -- m not consumed
  (mutex-unlock m)    ;; borrow again -- still fine
  (mutex-free m))     ;; the single legal consumption
```

Rules:

- A `^borrow` parameter keeps its **nominal** type (`Mutex`, `TmpFile`, ...);
  it is not a `&T` borrow *type*, so opaque identity is checked as usual.
- Borrowing an **already-consumed** handle is `TUR-E0101`
  (`mutex-free` then `mutex-lock`).
- A handle that is only ever borrowed, never consumed, is still `TUR-E0100`
  (dropped without being consumed).
- `^borrow` is a no-op for non-linear arguments, so the same accessor works
  whether or not the handle's type is under a linear discipline.

---

## Closures inherit the discipline of what they consume

**A closure that consumes a captured `^linear` or `^unique` value is itself
linear or unique.** The obligation travels with the closure, so every check that
applied to the captured value applies to the closure that swallowed it:

```turmeric
(let [b (bytes-alloc 8)
      f (fn [] : void (bytes-free b))]
  (f)
  (f))           ;; ERROR TUR-E0101: linear value used after being consumed
```

| Shape | `^linear` | `^unique` |
|---|---|---|
| The consuming closure called twice | `TUR-E0101` | `TUR-E0201` |
| Aliased first (`g` = `f`), then each called once | `TUR-E0101` | `TUR-E0201` |
| `(rc/of f)` on the consuming closure | `TUR-E0103` | `TUR-E0202` |
| The consuming closure never called | `TUR-E0100` | -- |

The last row is not a technicality: a consuming closure that is dropped unused
means the captured resource is never released, which is exactly what
"linear value dropped without being consumed" is for.

**Consuming, not merely capturing -- that distinction is the whole rule.** A
closure that only *reads* its capture is unaffected at any arity; a read-only
capture called twice is as legal as it ever was. A blanket "captures a linear
value" rejection would take out every read-only handler closure in the tree
(the `httpd` middleware family alone), which is why the rule is scoped to
consumption.

### Known limitation -- loops

Linear checking is flow-sensitive but not iteration-sensitive, so one syntactic
consumption site reads as one consumption however many times it runs:

```turmeric
(while (< i 3) (f))             ;; accepted
(while (< i 3) (bytes-free b))  ;; also accepted -- same, without the closure
```

This is a property of the checker, not of closures -- the direct form behaves
identically, and a closure is neither stronger nor weaker here than writing the
consumption out by hand.

---

## Stdlib macros

### `(must-use expr)`

Wraps `expr` so that the binding holding its value is inferred as `^relevant`.
The bound value must be used at least once before its scope exits.

```turmeric
(let [r (must-use (acquire-resource))]
  (process r)   ;; OK -- ^relevant allows duplication
  (process r))

;; ERROR TUR-E0151: relevant value 'r' dropped without use
(let [r (must-use (acquire-resource))]
  0)
```

```sweet-exp
let [r must-use(acquire-resource())]
  process(r)   ;; OK -- ^relevant allows duplication
  process(r)

;; ERROR TUR-E0151: relevant value 'r' dropped without use
let [r must-use(acquire-resource())]
  0
```

### `(with-resource [name init] body...)`

Scoped resource binding -- equivalent to `(let [name init] body...)`.
Useful to signal intent that `name` is a resource to be consumed within `body`.
With a `ref<T>` init, the binding is inferred as `^linear` and must be
explicitly consumed (via `(drop! name)` or a consuming call).

```turmeric
(with-resource [r (ref 42)]
  (drop! r))    ;; must consume the linear ref
```

```sweet-exp
with-resource [r ref(42)]
  drop!(r)    ;; must consume the linear ref
```

---

## Error codes

| Code        | Meaning                                      |
|-------------|----------------------------------------------|
| `TUR-E0100` | Linear value dropped without being consumed  |
| `TUR-E0101` | Linear value used after being consumed       |
| `TUR-E0102` | Cannot copy a linear value                    |
| `TUR-E0103` | Cannot wrap a linear value in `rc<T>`         |
| `TUR-E0150` | Affine value used more than once             |
| `TUR-E0151` | Relevant value dropped without being used    |

Use `tur --explain TUR-E0150` (or any other code) for a detailed explanation
and fix suggestions.

---

## Interaction with other features

### `ref<T>` and linear inference

All `ref<T>` let bindings and `:ref` parameters are automatically inferred
as `^linear`. You do not need an explicit `^linear` annotation -- but you
must explicitly consume the ref.

```turmeric
(defn consume [r : ref] : int
  (drop! r)
  0)
```

```sweet-exp
defn consume [r :ref] :int
  drop!(r)
  0
```

### Affine vs. move semantics

Turmeric's ownership model is affine by default for move types (`CK_UNIQUE`):
values can be dropped implicitly but cannot be aliased. The explicit `^affine`
annotation makes this restriction visible in the type signature, enabling
the elaborator to catch double-use at compile time.

### Pattern matching

Substructural annotations are checked at each match arm independently.
A `^linear` value bound inside a match arm must be consumed within that arm.

```turmeric
(match opt
  (some v) (let [^linear r (ref v)]
             (drop! r))   ;; must consume before arm exits
  none     0)
```

```sweet-exp
match opt
  (some v)
  let [^linear r ref(v)]
    drop!(r)   ;; must consume before arm exits
  none
  0
```

---

## Borrows and Lifetimes (TY4)

A borrow (`(& x)` / `(&mut x)`) is a non-owning reference whose validity is tied
to the value it points at. Turmeric enforces, at compile time, that **a borrow
never outlives the value it borrows from** (TUR-E0105):

```turmeric
;; ERROR (TUR-E0105): `r` borrows `x`, which lives only in the inner let.
(defn bad [] : int
  (let [r (let [x 42] (& x))]   ;; x dies when the inner let ends ...
    (deref r)))                 ;; ... but r still points at it

;; OK: the borrow does not outlive its referent.
(defn deref-outer [] : int
  (let [x 42]
    (let [r (& x)]              ;; x outlives r
      (deref r))))
```

The check is flow-sensitive through `do`, `let`, and both `if` branches, so a
borrow produced inside a nested block is still attributed to its referent. It
fires at two positions: a `let` binding whose initializer is an escaping borrow,
and a function whose result is a borrow of one of its own locals. A borrow of an
equal-or-outer-scope binding is always fine.

### Borrow types and explicit `'a` lifetimes

Borrows are first-class **type annotations**, written Rust-style with the
lifetime immediately after the borrow sigil:

```turmeric
;; &'a int        -- immutable borrow with lifetime 'a
;; &mut 'a int    -- mutable borrow with lifetime 'a
;; &int           -- borrow with an elided (implicitly fresh) lifetime
```

Lifetimes are **implicitly quantified**: every distinct `'a` in a signature is a
fresh lifetime parameter, and a repeated `'a` refers to the same one (just like
type variables). A borrow may also be a **return type**, which is how an output
borrow is tied back to an input:

```turmeric
;; The result borrows from x and y (both 'a), so it is valid for as long as
;; the shorter of the two arguments.
(defn longer [x : &'a int y : &'a int] : &'a int
  (if (> @x @y) x y))
```

### Inter-procedural borrow checking

Because the return lifetime is tied to a parameter, the borrow checker follows
the relationship **across calls**. A borrow returned from a call is treated as a
borrow of the corresponding argument, so it cannot outlive that argument's
referent:

```turmeric
(defn idb [x : &'a int] : &'a int x)

;; ERROR (TUR-E0105): (idb (& a)) borrows the local `a`; returning it lets that
;; borrow escape `bad`, where `a` no longer exists.
(defn bad [] : &int
  (let [a 5]
    (idb (& a))))
```

This reuses the same TUR-E0105 escape machinery as the direct case, so a single
diagnostic is reported (no double-reporting).

### Lifetime elision and outlives constraints

A borrow parameter or return that omits a lifetime gets one by the classic
elision rules -- each borrow parameter gets its own lifetime (rule 1), a sole
input lifetime flows to an elided borrow return (rule 2), and a receiver-style
first borrow parameter lends its lifetime to the return (rule 3). Explicit `'a`
annotations override elision for the borrows that carry them.

Nested borrows imply outlives constraints: `&'a &'b T` requires the inner
reference to outlive the outer, i.e. `'b` outlives `'a`. The resulting
constraints are solved with cycle detection, so a contradictory, cyclic
signature -- e.g. one parameter `&'a &'b int` paired with another `&'b &'a int`
-- is rejected (TUR-E0106) rather than looping. The elision rules, the
name-to-id interning, and the cycle-safe solver are unit-tested in
`tests/lifetime_unit.c`; end-to-end fixtures live in `tests/fixtures/lifetime-*`
and `tests/fixtures/errors/lifetime-*`.

## See also

- [Uniqueness Types Guide](uniqueness-types-guide.md) -- `^unique` (at-most-one-reference discipline)
- `tur --explain TUR-E0100` -- linear drop without consume
- `tur --explain TUR-E0150` -- affine used twice
- `tur --explain TUR-E0151` -- relevant dropped without use
