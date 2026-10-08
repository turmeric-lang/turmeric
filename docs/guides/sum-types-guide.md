---
title: Sum Types and `Either`
category: Language Basics
description: Declaring binary and n-ary sum types with `defdata`, pattern matching with `match`, exhaustiveness behaviour and the `^non-exhaustive` opt-out, the FFI layout, and the stdlib `Either` module.
---

# Sum Types and `Either`

A *sum type* (a.k.a. tagged union or algebraic data type) is a value that is
*one of* several shapes, each carrying its own payload. Turmeric spells these
with `defdata` and takes them apart with `match`.

```turmeric
(defdata Shape
  (Circle :int)            ; radius
  (Rect   :int :int))      ; width, height

(defn area [s : int] : int
  (match s
    (Circle r)   (* 3 (* r r))
    (Rect  w h)  (* w h)))
```

Sweet-exp equivalent:

```turmeric
#lang sweet-exp

defdata Shape
  Circle(:int)          ; radius
  Rect(:int :int)       ; width, height

defn area [s : Shape] : int
  match s
    (Circle r)
    *(3 *(r r))
    (Rect w h)
    *(w h)
```

The canonical binary sum is `Either L R` from `stdlib/either.tur`: a value is
either `(Left l)` carrying an `L`, or `(Right r)` carrying an `R`.

## When to use a sum vs. a struct

| Use a... | When the value is... |
|----------|----------------------|
| `defstruct` (record) | *all of* several fields at once (a point has both an x **and** a y) |
| `defdata` (sum) | *one of* several alternatives (a result is a success **or** an error) |

Reach for a sum whenever you would otherwise smuggle a "which case is this?"
discriminant alongside a payload whose meaning depends on it -- an error code
plus a maybe-valid value, a parse result that is either a value or a failure,
a protocol step that branches. The sum makes the discriminant and the payload
inseparable, so the type checker forces you to handle every case.

## Declaring a sum: `defdata`

```turmeric
(defdata Either :copy [L R]
  (Left L)
  (Right R))
```

- **Name** -- the type constructor (`Either`).
- **`:copy` / `:move`** (optional) -- value discipline. `:copy` makes a value
  freely reusable (right for immutable, resource-free sums like `Either`);
  the default `:move` enforces single-use ownership. Put it right after the
  name.
- **Type parameters** `[L R]` (optional) -- a parametric sum. Each constructor
  may use these in its payload types.
- **Constructors** `(Left L) (Right R)` -- one parenthesised arm each. A
  constructor may take zero payloads (`(Nil)`), concrete payloads
  (`(Circle :int)`), or type-parameter payloads (`(Left L)`).

Constructors become ordinary callables: `(Left 3)`, `(Right "ok")`.

### Record-style variants (named fields)

A variant's payload can be a *named-field block* instead of positional
payloads -- the same `[name : type ...]` shape `defstruct` uses. This is the
sum-of-records form (Rust's `enum Foo { Bar { x: int, y: int } }`):

```turmeric
(defdata Shape :copy
  (Circle [radius : float])
  (Rect   [w : float h : float])
  (Square [side : float]))
```

A record-style variant constructs **positionally** or **by keyword** (order
free), exactly like a struct:

```turmeric
(Circle 2.0)               ; positional
(Rect :h 4.0 :w 3.0)       ; keyword, reordered
```

and `match` binds its fields **by position or by name**:

```turmeric
(match s
  (Circle r)          (* 2.0 (* 3.14 r))     ; positional
  (Rect :h hv :w wv)  (* 2.0 (+ wv hv))       ; by-name, reordered
  (Square side)       (* 4.0 side))
```

Positional and record styles can mix across the variants of one `defdata`;
each variant commits to a single style. A positional variant still uses
keyword payload types (`(Circle :float)`); a record variant uses the bare
struct field syntax (`[radius : float]`).

`:copy` traverses every variant: a `:copy` sum whose payload includes an
owning pointer (`rc<T>`, `ref<T>`, `weak<T>`) is rejected, naming the
offending field and variant -- the same rule `defstruct :copy` enforces,
lifted across the sum.

## Pattern matching: `match`

`match` dispatches on the constructor and binds each payload:

```turmeric
(match e
  (Left  l)  (handle-error l)
  (Right r)  (use r))
```

Sweet-exp equivalent -- each arm is a pattern line followed by its body
line(s); a pattern and its body do **not** share a line, because sweet-exp
would read the pair as one list:

```turmeric
#lang sweet-exp

match e
  (Left l)
  handle-error(l)
  (Right r)
  use(r)
```

- Bind a payload to a name (`l`, `r`) to use it in that arm's body.
- Use `_` to ignore a payload: `(Left _)`.
- Sub-match a payload that is itself a sum either by matching again on the
  bound variable, or by nesting the pattern directly in the field position.

```turmeric
;; nested directly
(match outer
  (Left n)          n
  (Right (Nada))    -1
  (Right (Just k))  k)

;; the same thing with an inner match
(match outer
  (Left n)   n
  (Right r)  (match r
               (Nada)   -1
               (Just k) k))
```

Sweet-exp equivalent:

```turmeric
#lang sweet-exp

match outer
  (Left n)
  n
  (Right (Nada))
  -1
  (Right (Just k))
  k
```

Nesting goes to any depth, and a scalar literal is a pattern too, so a
specific arm can precede a general one for the same constructor:

```turmeric
(match e
  (Lit v)         v
  (Add (Lit 0) r) (eval-expr r)     ; only when the left operand is 0
  (Add l r)       (+ (eval-expr l) (eval-expr r)))
```

Arms are tried in order, and a nested arm that fails falls through to the next
arm -- including through a failed `when` guard. The nested arms for one
constructor have to cover it: a later arm for the same constructor binding
plain names, sub-patterns that provably cover the sub-type, or a `_` arm.
Otherwise the compiler says so:

```
match: the nested patterns for constructor 'Add' are not exhaustive --
add a `(Add ...)` arm binding plain names, or a `_` arm after it
```

## Updating a record variant inside a `match` arm

Inside a `match` arm that has destructured a bare-symbol scrutinee to a
specific record variant, the scrutinee is **narrowed** to that variant for the
duration of the arm. That means `(with s [field val ...])` can reconstruct
through the matched ctor without the caller re-listing every field, and
`(.field s)` reads the variant's field directly:

```turmeric
(defdata Shape :copy
  (Circle [radius : float])
  (Rect   [w : float h : float]))

(defn grow [s : Shape] : Shape
  (match s
    (Circle r) (with s [radius {r * 2.0}])   ; s narrowed to Circle for this arm
    (Rect w h) (with s [w {w * 2.0}])))       ; s narrowed to Rect for this arm
```

- The scrutinee must be a **bare symbol** (`match s ...`), not a compound
  expression. Compound scrutinees have no name to narrow.
- The ADT (and the matched variant) must be `:copy`. Move-only ADTs still
  reject at TUR-E0296.
- The matched variant must be **record-style**. Narrowing to a positional
  variant rejects with a message pointing at the missing field names.
- Narrowing is **per-arm**: once the arm exits, the scrutinee's type widens
  back to the full ADT, so a later `(with s ...)` outside the arm still needs
  its own narrowing context.
- A `when`-guarded arm narrows through the guard too, and a bare `(with s ...)`
  on a multi-variant ADT outside any arm rejects with **TUR-E0302** telling
  you to wrap the update in a `match` arm.

## Exhaustiveness

A `match` whose scrutinee is a known sum type must cover **every**
constructor (or include a wildcard). A missing arm is a **hard error**:

```
match: non-exhaustive patterns -- constructor 'Left' of 'Either' not covered
```

This is deliberate: the compiler will not let a new constructor silently slip
past existing matches.

### Opting out: `^non-exhaustive`

When you have *proven* a case cannot occur by other means, place the
`^non-exhaustive` marker immediately after `match` (before the scrutinee) to
suppress the diagnostic:

```turmeric
(defn unwrap-right [e : int] : int
  (match ^non-exhaustive e
    (Right r) r))      ; Left is statically impossible here, by construction
```

Sweet-exp equivalent:

```turmeric
#lang sweet-exp

defn unwrap-right [e : Ei] : int
  ; Left is statically impossible here, by construction
  match ^non-exhaustive e
    (Right r)
    r
```

The marker is the *only* escape hatch; without it a non-exhaustive match does
not compile. Use it sparingly and leave a comment explaining why the missing
arm is unreachable.

It used to be spelled `#fx{NonExhaustive}`. That spelling still opts out, with
a `TUR-D0004` deprecation warning (an error under `--Werror=deprecated`):
`#fx{...}` is the effect row, and this marker is an attribute of the `match`,
not an effect -- so it moved to the `^attr` form every other attribute uses.

## The `Either` module

`stdlib/either.tur` is not auto-loaded; pull it in explicitly:

```turmeric
(load "stdlib/either.tur")
```

It provides:

| Form | Meaning |
|------|---------|
| `(Left l)` / `(Right r)` | constructors |
| `(left? e)` / `(right? e)` | arm predicates |
| `(from-left dflt e)` / `(from-right dflt e)` | extract a payload or fall back to `dflt` |
| `(either on-left on-right e)` | eliminate by applying one of two functions |
| `(either-map f e)` | map `f` over a `Right`, pass `Left` through (right-biased) |
| `(either-map-left f e)` | map `f` over a `Left`, pass `Right` through |
| `Functor [(Either E)]` | right-biased `fmap` instance |

`Either` is **right-biased** by convention: `Right` is the "success"/"main"
arm and `Left` the "error"/"other" arm, so `either-map` and the `Functor`
instance act on `Right` and leave `Left` untouched -- matching Haskell's
`Either`.

```turmeric
(load "stdlib/either.tur")

(defn inc [x : int] : int (+ x 1))

(defn main [] : int
  (println (from-right -1 (fmap (Right 41) inc)))  ; => 42
  (println (from-left  -1 (fmap (Left 9)  inc)))   ; =>  9  (Left untouched)
  0)
```

### Worked migration: parsing with error detail

`str->int` / `cstr->parse-int` fold every failure -- a NULL pointer, an empty
string, trailing garbage -- into the sentinel `0`, indistinguishable from a
legitimately parsed `0`. `str->int-checked` (also in `stdlib/str.tur`) returns
an `Either` instead, so the failure detail survives:

```turmeric
(load "stdlib/str.tur")

(from-right -1 (str->int-checked "42"))   ; => 42
(right? (str->int-checked "0"))           ; => true   (a real zero, not failure)
(left?  (str->int-checked "12x"))         ; => true   (Left 1: trailing garbage)
(left?  (str->int-checked ""))            ; => true   (Left 0: empty/NULL)
```

## FFI layout

A `defdata` lowers to a single, type-erased tagged struct. For `Either`:

```c
typedef struct tur_adt_Either {
    int tag;                       /* 0 = Left, 1 = Right */
    union {
        struct { int64_t _0; } Left;
        struct { int64_t _0; } Right;
    } as;
} tur_adt_Either;
```

- `tag` is the discriminant: `0` for the first constructor, `1` for the
  second, and so on for n-ary sums.
- Payloads are erased to `int64_t` at the C boundary (the same way `Option[A]`
  and `Result[A B]` erase theirs); the elaborator restores the proper type on
  extraction.
- Each constructor has a `ctor_<Name>` factory (`ctor_Left`, `ctor_Right`)
  that heap-allocates the struct, sets the tag, and returns an `int64_t`
  handle.

Inline-C blocks can therefore construct and destructure sum values directly:

```turmeric
(defn right-payload [e : int] #{Unsafe} : int
  ```c
  tur_adt_Either *p = (tur_adt_Either *)(intptr_t)e;
  return p->as.Right._0;
  ```)
```

## Writing a typeclass instance over a sum

A type constructor with a free parameter can carry a typeclass instance. For a
binary sum, fix one parameter with a partially-applied head -- `(Either E)`
leaves the kind-(`* -> *`) functor over the `Right` arm:

```turmeric
(definstance Functor [(Either E)]
  (fmap [container fn]
    ```c
    tur_adt_Either *p = (tur_adt_Either *)(intptr_t)container;
    if (p->tag != 1) return container;          /* Left: identity */
    return ctor_Right(fn.fn(fn.env, p->as.Right._0));
    ```))
```

The instance is **not** an orphan when it lives in the module that defines the
sum (here, `stdlib/either.tur`), even though `Functor` is declared elsewhere --
the owning module of *either* the class or a type argument may host the
instance.

## See also

- [sum-types-either-plan](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/sum-types-either-plan.md) -- the design plan and ADR.
- [`stdlib/result.tur`](https://github.com/turmeric-lang/turmeric/blob/main/stdlib/result.tur) / `stdlib/option.tur` --
  the unary-payload tagged structs that predate `Either`.
- [[data-literals-guide]] -- literal construction syntax that composes with
  sum payloads.
