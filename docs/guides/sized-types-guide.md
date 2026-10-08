---
title: Sized Types
category: Type System
description: Tracking data-structure sizes in the type system for memory layout, stack allocation, and type-safe array operations
---

# Sized Types in Turmeric

Sized types track the **size** of data structures at compile time. This enables
memory layout verification, stack allocation of fixed-size buffers, type-safe
array operations, and FFI structs that carry size annotations through Turmeric
wrappers.

Sized types are enabled by default; the layer is built on the GADT machinery.

> **Static checking.** Size indices are lifted to the type level: a
> length-`n` vector's type mentions `n`, and a dimension mismatch whose sizes
> are statically known is a **compile-time** error (`TUR-E0260`), not just a
> runtime assertion. This covers the cases that matter in practice:
>
> - **Cross-parameter unification.** A signature that names the same size
>   variable in two or more parameters -- e.g.
>   `(defn pairwise-sum [xs : (SizedVec n) ys : (SizedVec n)] ...)` -- unifies
>   `n` across all the slots. Passing a length-2 vector for `xs` and a
>   length-3 vector for `ys` is rejected at compile time. Multi-index
>   carriers (`(LaMatN m n)`) unify per-index position.
> - **Propagation through wrappers.** A `defstruct`/`defopaque` field whose
>   type carries a size index keeps it: projecting `(.fst p)` and `(.snd p)`
>   into a shared-`n` callee recovers each field's static size and rejects a
>   mismatch.
> - **Polymorphic helpers.** A function over `(SizedVec n)` whose body re-wraps
>   or threads the value preserves the index without per-call re-annotation.
> - **The claims themselves.** A `defn`'s declared return-type index is
>   reconciled with its body, and a `(:: e (SizedBuf (Static k)))` ascription
>   with the index `e` already carries -- so a wrong annotation is rejected
>   where it is written rather than trusted by every downstream call. An
>   ascription on a value whose index is open (the `sized-buf-new` family's
>   polymorphic `n`) is the pin-by-ascription idiom: accepted, and the pinned
>   index then unifies like a declared return type.
>
> Where both sides of a comparison fold to a known constant, the check fires
> statically. The runtime predicates/assertions (`size-eq?`, `size-compat?`,
> `size-assert-eq!`, `sized-matrix-assert-shape!`) remain the fall-through for
> the genuinely dynamic cases: a size that is only known at run time (read from
> input, computed by non-constant arithmetic) or an open size expression with
> free variables that cannot be folded. Size arithmetic on indices (`+`, `*`
> for concat-typed vectors) is also a runtime check today. The historical
> roll-out is recorded in the
> archived [sized-types-completion-plan.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/sized-types-completion-plan.md)
> (phases SZ6–SZ9) and
> [sized-types-cross-param-unification-plan.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/sized-types-cross-param-unification-plan.md).

## Table of Contents

1. [Size Foundations](#size-foundations)
   - [StaticInt](#staticint)
   - [Size expressions](#size-expressions)
   - [SizedVec](#sizedvec)
2. [Size Predicates and Assertions](#size-predicates-and-assertions)
3. [Size Inference](#size-inference)
4. [Flat Buffers and Memory Layout](#flat-buffers-and-memory-layout)
   - [Heap allocation](#heap-allocation)
   - [Stack allocation](#stack-allocation)
   - [Allocation dispatch](#allocation-dispatch)
5. [SizedMatrix](#sizedmatrix)
6. [SizedBitVec](#sizedbitvec)
7. [FFI Integration](#ffi-integration)
8. [Error Messages](#error-messages)
9. [API Reference](#api-reference)

---

## Size Foundations

### StaticInt

`StaticInt` is a compile-time integer annotation. It carries an integer value
intended for use as a type-level literal.

```turmeric
(import stdlib/sized)

(let [si (static-int 10)]
  (println (static-int-val si)))   ; => 10
```

```sweet-exp
import stdlib/sized

let [si static-int(10)]
  println(static-int-val(si))   ; => 10
```

Arithmetic on `StaticInt` values:

```turmeric
(let [a (static-int 3)
      b (static-int 4)]
  (println (static-int-val (static-int-add a b)))   ; => 7
  (println (static-int-val (static-int-mul a b))))  ; => 12
```

```sweet-exp
let [a static-int(3)
     b static-int(4)]
  println(static-int-val(static-int-add(a b)))   ; => 7
  println(static-int-val(static-int-mul(a b)))   ; => 12
```

### Size expressions

`Size` is a GADT that represents arithmetic expressions over compile-time sizes.
Its constructors are:

| Constructor     | Meaning                        |
|-----------------|--------------------------------|
| `(Static n)`    | Literal size `n`               |
| `(Add s1 s2)`   | Sum of two sizes               |
| `(Mul s1 s2)`   | Product of two sizes           |

Use `size-static`, `size-add`, and `size-mul` to build `Size` expressions, and
`size-eval` to reduce them to a runtime integer:

```turmeric
(let [s (size-add (size-static 3) (size-mul (size-static 2) (size-static 5)))]
  (println (size-eval s)))   ; => 13
```

```sweet-exp
let [s size-add(size-static(3) size-mul(size-static(2) size-static(5)))]
  println(size-eval(s))   ; => 13
```

`size-normalize` flattens a `Size` expression to `(Static n)`:

```turmeric
(let [s (size-normalize (size-add (size-static 4) (size-static 6)))]
  (println (size-eval s)))   ; => 10
```

```sweet-exp
let [s size-normalize(size-add(size-static(4) size-static(6)))]
  println(size-eval(s))   ; => 10
```

`size-simplify` folds constant sub-expressions and collapses identities such as
`0 + s = s` and `1 * s = s`:

```turmeric
(let [s (size-simplify (size-mul (size-static 1) (size-add (size-static 2) (size-static 3))))]
  (println (size-eval s)))   ; => 5
```

```sweet-exp
let [s size-simplify(size-mul(size-static(1) size-add(size-static(2) size-static(3))))]
  println(size-eval(s))   ; => 5
```

### SizedVec

`SizedVec` is a linked-list collection that carries a `Size` annotation. It is
primarily useful when you need a simple sized container and do not require
flat memory layout (see [Flat Buffers](#flat-buffers-and-memory-layout) for
that).

```turmeric
(let [v (sized-vec-of-3 10 20 30)]
  (println (sized-vec-len v))       ; => 3
  (println (sized-vec-get v 1)))    ; => 20
```

```sweet-exp
let [v sized-vec-of-3(10 20 30)]
  println(sized-vec-len(v))       ; => 3
  println(sized-vec-get(v 1))    ; => 20
```

`sized-vec-push` prepends an element and returns a new `SizedVec` whose size
is `n + 1`:

```turmeric
(let [v  (sized-vec-of-2 1 2)
      v2 (sized-vec-push v 0)]
  (println (sized-vec-len v2)))   ; => 3
```

```sweet-exp
let [v  sized-vec-of-2(1 2)
     v2 sized-vec-push(v 0)]
  println(sized-vec-len(v2))   ; => 3
```

---

## Size Predicates and Assertions

Five predicates compare two `Size` expressions at runtime:

```turmeric
(size-eq? (size-static 4) (size-add (size-static 2) (size-static 2)))   ; => true
(size-lt? (size-static 3) (size-static 4))                               ; => true
(size-le? (size-static 4) (size-static 4))                               ; => true
(size-gt? (size-static 5) (size-static 3))                               ; => true
(size-ge? (size-static 4) (size-static 4))                               ; => true
```

```sweet-exp
size-eq?(size-static(4) size-add(size-static(2) size-static(2)))   ; => true
size-lt?(size-static(3) size-static(4))                             ; => true
size-le?(size-static(4) size-static(4))                             ; => true
size-gt?(size-static(5) size-static(3))                             ; => true
size-ge?(size-static(4) size-static(4))                             ; => true
```

`size-compat?` is the runtime analog of the subtyping rule -- two sizes are
compatible when they evaluate to the same integer:

```turmeric
(size-compat? (size-static 8) (size-mul (size-static 2) (size-static 4)))
; => true
```

```sweet-exp
size-compat?(size-static(8) size-mul(size-static(2) size-static(4)))
; => true
```

Two assertion functions panic with a descriptive message when the condition
fails:

```turmeric
; passes silently
(size-assert-eq! (size-static 4) (size-add (size-static 2) (size-static 2)))

; passes silently (3 <= 10)
(size-assert-le! (size-static 3) (size-static 10))

; panics: "sized type mismatch"
(size-assert-eq! (size-static 4) (size-static 5))
```

```sweet-exp
; passes silently
size-assert-eq!(size-static(4) size-add(size-static(2) size-static(2)))

; passes silently (3 <= 10)
size-assert-le!(size-static(3) size-static(10))

; panics: "sized type mismatch"
size-assert-eq!(size-static(4) size-static(5))
```

### Static checking (SZ7)

`size-assert-eq!` and `size-assert-le!` are checked **statically** when both
of their size arguments reduce to compile-time constants -- the `Size` expression is folded at elaboration and compared:

```turmeric
; COMPILE-TIME error TUR-E0260 (no runtime check is emitted):
(size-assert-eq! (size-static 4) (size-static 5))

; accepted at compile time -- both sides fold to 4:
(size-assert-eq! (size-static 4) (size-add (size-static 2) (size-static 2)))
```

```sweet-exp
; COMPILE-TIME error TUR-E0260 (no runtime check is emitted):
size-assert-eq!(size-static(4) size-static(5))

; accepted at compile time -- both sides fold to 4:
size-assert-eq!(size-static(4) size-add(size-static(2) size-static(2)))
```

Run `tur explain TUR-E0260` for the full diagnostic.

**Runtime fallback (the boundary).** When at least one size is *not*
statically known -- for example a dimension derived from a runtime length or
passed through a function parameter -- the checker cannot decide the equation
and lowers to the existing runtime assertion shown above. It never silently
accepts a possibly-wrong size:

```turmeric
(let [n (read-length)]              ; n is a runtime value
  ; not statically known -> runtime assertion (panics if n != 4)
  (size-assert-eq! (size-static n) (size-static 4)))
```

```sweet-exp
let [n read-length()]              ; n is a runtime value
  ; not statically known -> runtime assertion (panics if n != 4)
  size-assert-eq!(size-static(n) size-static(4))
```

Folding covers `Static`/`Add`/`Mul` (and the `size-static`/`size-add`/
`size-mul` value forms) at the assertion call site itself. Sizes that arrive
through a wrapper function still fall back to the runtime check *for the
assertion forms above*; the **type-index** path (below) does propagate through
wrappers, so the more common dimension checks are caught statically.

### Cross-parameter unification and index propagation

Beyond the assertion forms, the elaborator unifies size **indices** carried in
parameter and field types:

```turmeric
; both parameters share `n`; a 2-vs-3 call is a COMPILE-TIME TUR-E0260:
(defn pairwise-sum [xs : (SizedVec n) ys : (SizedVec n)] : int ...)

(pairwise-sum (SVCons 1 (SVCons 2 (SVNil)))            ; n := 2
              (SVCons 10 (SVCons 20 (SVCons 30 (SVNil)))))  ; n := 3 -> TUR-E0260
```

The same unification recovers indices across multi-index carriers
(`(LaMatN m n)` -- the shared inner dimension of a matrix multiply), through
`defstruct`/`defopaque` field projections (`(.fst p)` / `(.snd p)` into a
shared-`n` callee), and through length-polymorphic helpers whose body re-wraps
or threads the value. None of these require per-call re-annotation. The
[fixtures under `tests/fixtures/sized-*`](https://github.com/turmeric-lang/turmeric/tree/main/tests/fixtures) exercise each
case (accept + reject).

The boundary is the same as for the assertion forms: an index that is only
known at run time, or an open size expression with free variables that cannot
be folded to a constant, stays polymorphic and falls through to the runtime
predicates.

### Written size claims are checked, not trusted

Both places a size index is *written* rather than inferred are reconciled
with the value they describe, at the seam where the claim is made:

```turmeric
(defopaque LaMatN [m n] :int)
(defn mk23 [] : (LaMatN (Static 2) (Static 3)) (:: 0 :LaMatN))

; COMPILE-TIME TUR-E0260: 'bogus' declares return size 5 but its body has size 2
(defn bogus [] : (LaMatN (Static 5) (Static 5)) (mk23))

(defn mk4 [] : (SizedBuf (Static 4)) (:: (sized-buf-new-zeroed 4) :SizedBuf))

; COMPILE-TIME TUR-E0260: ascription declares size 3 but the expression has size 4
(let [d (:: (mk4) (SizedBuf (Static 3)))] ...)
```

Without the return-seam check, `(takes55 (bogus))` would have been accepted
even though `(takes55 (mk23))` is rejected -- the annotation laundered the
mismatch past the argument check that trusted it.

The pin-by-ascription idiom stays legitimate. `sized-buf-new` /
`sized-buf-new-zeroed` return a polymorphic `(SizedBuf n)`, so
`(:: (sized-buf-new-zeroed 4) (SizedBuf (Static 4)))` has nothing static to
disagree with and is accepted; the pinned `4` is then load-bearing, flowing
through a `let` binding into the cross-parameter unifier exactly as a
declared return type does. What is **not** checked is the allocation itself:
nothing ties the runtime `k` passed to `sized-buf-new` to the `n` an
ascription pins, so `(:: (sized-buf-new-zeroed 4) (SizedBuf (Static 99)))`
is still accepted. Tying `k` to `n` needs a size-witnessed constructor and is
a stdlib follow-up (see
[docs/archive/declared-size-index-never-checked-against-value.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/declared-size-index-never-checked-against-value.md)).

---

## Size Inference

When the number of elements is known at the call site, use the fixed-arity
constructors to let the compiler infer the size:

```turmeric
(let [v1 (sized-vec-of-1 42)          ; size inferred as 1
      v2 (sized-vec-of-2 1 2)         ; size inferred as 2
      v3 (sized-vec-of-3 1 2 3)       ; size inferred as 3
      v4 (sized-vec-of-4 1 2 3 4)]    ; size inferred as 4
  (println (sized-vec-len v3)))       ; => 3
```

```sweet-exp
let [v1 sized-vec-of-1(42)           ; size inferred as 1
     v2 sized-vec-of-2(1 2)          ; size inferred as 2
     v3 sized-vec-of-3(1 2 3)        ; size inferred as 3
     v4 sized-vec-of-4(1 2 3 4)]     ; size inferred as 4
  println(sized-vec-len(v3))         ; => 3
```

When the size is determined at runtime (e.g. from a list value), use
`sized-vec-from-list`:

```turmeric
(let [lst (list 10 20 30)]
  (let [v (unsafe (sized-vec-from-list lst))]
    (println (sized-vec-len v))))   ; => 3
```

```sweet-exp
let [lst list(10 20 30)]
  let [v unsafe(sized-vec-from-list(lst))]
    println(sized-vec-len(v))   ; => 3
```

`sized-vec-from-list` requires `#fx{Unsafe}` because it casts the cons cell
layout directly.

### Type-level index inference (SZ8)

When a sized GADT carries a type-level size index, the index of a constructed
value is inferred automatically by threading operand indices through the
constructors -- no annotation is needed:

```turmeric
(defgadt SizedVec [n]
  (SVNil : (SizedVec (Static 0)))
  (SVCons int (SizedVec n) : (SizedVec (Add (Static 1) n))))

; (SVCons _ (SVCons _ (SVNil))) infers (SizedVec 2)
```

```sweet-exp
defgadt SizedVec [n]
  SVNil : (SizedVec (Static 0))
  SVCons int (SizedVec n) : (SizedVec (Add (Static 1) n))

; (SVCons _ (SVCons _ (SVNil))) infers (SizedVec 2)
```

Pass `--dump-sizes` to print the inferred index for each
constructor application:

```
size: SVNil  : (SizedVec 0)
size: SVCons : (SizedVec 1)
size: SVCons : (SizedVec 2)
```

A `SizedVec` parameter written without an index (`v : SizedVec`) is
length-polymorphic: it elaborates against a fresh size variable and accepts any
length. Inference covers literal constructor chains and linear `Add`/`Mul` over
`Static` and one variable; an index that depends on an unknown operand (a
parameter, a runtime-built vector) is left polymorphic -- `--dump-sizes` shows
it as `(SizedVec ?)` -- rather than being guessed. See
[sized-types-index-spec.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/sized-types-index-spec.md) section 6 for the full
inference boundary.

---

## Flat Buffers and Memory Layout

`SizedBuf` stores elements in a contiguous `int64_t *data` array. This is more
cache-friendly than `SizedVec` (which allocates one struct per element) and is
the right choice for numerical work, bulk operations, or FFI.

C struct layout:

```c
struct { int64_t len; int64_t *data; }
```

All `SizedBuf` operations require `#fx{Unsafe}` at the call site.

### Heap allocation

```turmeric
(import stdlib/sized-buf)

(let [b (unsafe (sized-buf-new-zeroed 4))]
  (unsafe (sized-buf-set! b 0 10))
  (unsafe (sized-buf-set! b 1 20))
  (unsafe (sized-buf-set! b 2 30))
  (unsafe (sized-buf-set! b 3 40))
  (println (unsafe (sized-buf-get  b 2)))    ; => 30
  (println (unsafe (sized-buf-sum  b)))      ; => 100
  (println (unsafe (sized-buf-min  b)))      ; => 10
  (println (unsafe (sized-buf-max  b)))      ; => 40
  (unsafe (sized-buf-free b)))
```

```sweet-exp
import stdlib/sized-buf

let [b unsafe(sized-buf-new-zeroed(4))]
  unsafe(sized-buf-set!(b 0 10))
  unsafe(sized-buf-set!(b 1 20))
  unsafe(sized-buf-set!(b 2 30))
  unsafe(sized-buf-set!(b 3 40))
  println(unsafe(sized-buf-get(b 2)))    ; => 30
  println(unsafe(sized-buf-sum(b)))       ; => 100
  println(unsafe(sized-buf-min(b)))       ; => 10
  println(unsafe(sized-buf-max(b)))       ; => 40
  unsafe(sized-buf-free(b))
```

`sized-buf-fill!` sets every element to the same value:

```turmeric
(let [b (unsafe (sized-buf-new 8))]
  (unsafe (sized-buf-fill! b 7))
  (println (unsafe (sized-buf-sum b)))   ; => 56
  (unsafe (sized-buf-free b)))
```

```sweet-exp
let [b unsafe(sized-buf-new(8))]
  unsafe(sized-buf-fill!(b 7))
  println(unsafe(sized-buf-sum(b)))   ; => 56
  unsafe(sized-buf-free(b))
```

`sized-buf-copy!` copies all elements from one buffer into another (both must
have the same length):

```turmeric
(let [src (unsafe (sized-buf-new-zeroed 3))
      dst (unsafe (sized-buf-new-zeroed 3))]
  (unsafe (sized-buf-set! src 0 1))
  (unsafe (sized-buf-set! src 1 2))
  (unsafe (sized-buf-set! src 2 3))
  (unsafe (sized-buf-copy! src dst))
  (println (unsafe (sized-buf-get dst 1)))   ; => 2
  (unsafe (sized-buf-free src))
  (unsafe (sized-buf-free dst)))
```

```sweet-exp
let [src unsafe(sized-buf-new-zeroed(3))
     dst unsafe(sized-buf-new-zeroed(3))]
  unsafe(sized-buf-set!(src 0 1))
  unsafe(sized-buf-set!(src 1 2))
  unsafe(sized-buf-set!(src 2 3))
  unsafe(sized-buf-copy!(src dst))
  println(unsafe(sized-buf-get(dst 1)))   ; => 2
  unsafe(sized-buf-free(src))
  unsafe(sized-buf-free(dst))
```

### Stack allocation

`sized-buf-with-stack` allocates `n` elements on the stack via `alloca` and
calls a non-capturing function with the buffer. No `malloc`/`free` overhead;
the buffer is reclaimed automatically when the enclosing function returns.

```turmeric
(defn fill-and-sum [b] : int
  (unsafe (sized-buf-set! b 0 10))
  (unsafe (sized-buf-set! b 1 20))
  (unsafe (sized-buf-set! b 2 30))
  (unsafe (sized-buf-sum b)))

(println (unsafe (sized-buf-with-stack 3 fill-and-sum)))   ; => 60
```

```sweet-exp
defn fill-and-sum [b] :int
  unsafe(sized-buf-set!(b 0 10))
  unsafe(sized-buf-set!(b 1 20))
  unsafe(sized-buf-set!(b 2 30))
  unsafe(sized-buf-sum(b))

println(unsafe(sized-buf-with-stack(3 fill-and-sum)))   ; => 60
```

The callback must be a named function (not an anonymous lambda) because `alloca`
memory must not escape the frame.

### Allocation dispatch

`sized-buf-compute` demonstrates the dispatch strategy that your own code can
mirror: use the stack when the size is small and known, fall back to the heap
otherwise.

```turmeric
; n <= 64: uses alloca (stack)
(println (unsafe (sized-buf-compute 10)))    ; => 45  (0+1+...+9)

; n > 64: uses malloc (heap)
(println (unsafe (sized-buf-compute 100)))   ; => 4950
```

```sweet-exp
; n <= 64: uses alloca (stack)
println(unsafe(sized-buf-compute(10)))    ; => 45  (0+1+...+9)

; n > 64: uses malloc (heap)
println(unsafe(sized-buf-compute(100)))   ; => 4950
```

`sized-buf-size` returns the element count as a `Size` expression so you can
pass it to size predicates and assertions:

```turmeric
(let [b (unsafe (sized-buf-new 6))]
  (println (size-eval (unsafe (sized-buf-size b))))   ; => 6
  (unsafe (sized-buf-free b)))
```

```sweet-exp
let [b unsafe(sized-buf-new(6))]
  println(size-eval(unsafe(sized-buf-size(b))))   ; => 6
  unsafe(sized-buf-free(b))
```

Converting a `SizedVec` to a flat `SizedBuf`:

```turmeric
(let [v (sized-vec-of-3 1 2 3)
      b (unsafe (sized-buf-from-sized-vec v))]
  (println (unsafe (sized-buf-sum b)))   ; => 6
  (unsafe (sized-buf-free b)))
```

```sweet-exp
let [v sized-vec-of-3(1 2 3)
     b unsafe(sized-buf-from-sized-vec(v))]
  println(unsafe(sized-buf-sum(b)))   ; => 6
  unsafe(sized-buf-free(b))
```

---

## SizedMatrix

`SizedMatrix` is a 2-D flat row-major matrix.

C struct layout:

```c
struct { int64_t rows; int64_t cols; int64_t *data; }
```

All operations require `#fx{Unsafe}`.

```turmeric
(import stdlib/sized-matrix)

(let [m (unsafe (sized-matrix-new-zeroed 3 4))]
  ; Shape
  (println (unsafe (sized-matrix-rows m)))    ; => 3
  (println (unsafe (sized-matrix-cols m)))    ; => 4
  (println (size-eval (unsafe (sized-matrix-size m))))   ; => 12

  ; Element access
  (unsafe (sized-matrix-set! m 0 0 1))
  (unsafe (sized-matrix-set! m 0 1 2))
  (unsafe (sized-matrix-set! m 1 0 5))
  (println (unsafe (sized-matrix-get m 0 1)))   ; => 2

  ; Bulk operations
  (println (unsafe (sized-matrix-row-sum   m 0)))   ; => 3  (1+2+0+0)
  (println (unsafe (sized-matrix-col-sum   m 0)))   ; => 6  (1+5+0)
  (println (unsafe (sized-matrix-total-sum m)))     ; => 8

  ; Shape assertion (panics if shape does not match)
  (unsafe (sized-matrix-assert-shape! m (Static 3) (Static 4)))

  (unsafe (sized-matrix-free m)))
```

```sweet-exp
import stdlib/sized-matrix

let [m unsafe(sized-matrix-new-zeroed(3 4))]
  ; Shape
  println(unsafe(sized-matrix-rows(m)))    ; => 3
  println(unsafe(sized-matrix-cols(m)))    ; => 4
  println(size-eval(unsafe(sized-matrix-size(m))))   ; => 12

  ; Element access
  unsafe(sized-matrix-set!(m 0 0 1))
  unsafe(sized-matrix-set!(m 0 1 2))
  unsafe(sized-matrix-set!(m 1 0 5))
  println(unsafe(sized-matrix-get(m 0 1)))   ; => 2

  ; Bulk operations
  println(unsafe(sized-matrix-row-sum(m 0)))    ; => 3  (1+2+0+0)
  println(unsafe(sized-matrix-col-sum(m 0)))    ; => 6  (1+5+0)
  println(unsafe(sized-matrix-total-sum(m)))      ; => 8

  ; Shape assertion (panics if shape does not match)
  unsafe(sized-matrix-assert-shape!(m (Static 3) (Static 4)))

  unsafe(sized-matrix-free(m))
```

`sized-matrix-fill!` sets every element to the same value:

```turmeric
(let [m (unsafe (sized-matrix-new 2 2))]
  (unsafe (sized-matrix-fill! m 9))
  (println (unsafe (sized-matrix-total-sum m)))   ; => 36
  (unsafe (sized-matrix-free m)))
```

```sweet-exp
let [m unsafe(sized-matrix-new(2 2))]
  unsafe(sized-matrix-fill!(m 9))
  println(unsafe(sized-matrix-total-sum(m)))   ; => 36
  unsafe(sized-matrix-free(m))
```

`sized-matrix-row-size` returns the column count as a `Size` expression,
useful for checking row-vector widths:

```turmeric
(let [m (unsafe (sized-matrix-new 3 4))]
  (println (size-eval (unsafe (sized-matrix-row-size m))))   ; => 4
  (unsafe (sized-matrix-free m)))
```

```sweet-exp
let [m unsafe(sized-matrix-new(3 4))]
  println(size-eval(unsafe(sized-matrix-row-size(m))))   ; => 4
  unsafe(sized-matrix-free(m))
```

---

## SizedBitVec

`SizedBitVec` packs bits tightly: `n` bits occupy `ceil(n / 8)` bytes.

C struct layout:

```c
struct { int64_t len; uint8_t *bits; }
```

Bit `i` is stored at byte `i/8`, at position `i%8` within that byte (LSB = bit
0). All operations require `#fx{Unsafe}`.

```turmeric
(import stdlib/sized-bits)

(let [bv (unsafe (sized-bitvec-new 16))]
  ; Set some bits
  (unsafe (sized-bitvec-set!    bv 0))
  (unsafe (sized-bitvec-set!    bv 3))
  (unsafe (sized-bitvec-set!    bv 7))

  ; Read bits
  (println (unsafe (sized-bitvec-get bv 0)))   ; => 1
  (println (unsafe (sized-bitvec-get bv 1)))   ; => 0

  ; Popcount
  (println (unsafe (sized-bitvec-count bv)))   ; => 3

  ; Clear and toggle
  (unsafe (sized-bitvec-clear!  bv 0))
  (unsafe (sized-bitvec-toggle! bv 1))
  (println (unsafe (sized-bitvec-count bv)))   ; => 2

  ; Fill all bits
  (unsafe (sized-bitvec-fill! bv 1))
  (println (unsafe (sized-bitvec-count bv)))   ; => 16

  ; Length and size
  (println (unsafe (sized-bitvec-len  bv)))                      ; => 16
  (println (size-eval (unsafe (sized-bitvec-size bv))))          ; => 16

  ; Length assertion (panics if length does not match)
  (unsafe (sized-bitvec-assert-len! bv (Static 16)))

  (unsafe (sized-bitvec-free bv)))
```

```sweet-exp
import stdlib/sized-bits

let [bv unsafe(sized-bitvec-new(16))]
  ; Set some bits
  unsafe(sized-bitvec-set!(bv 0))
  unsafe(sized-bitvec-set!(bv 3))
  unsafe(sized-bitvec-set!(bv 7))

  ; Read bits
  println(unsafe(sized-bitvec-get(bv 0)))   ; => 1
  println(unsafe(sized-bitvec-get(bv 1)))   ; => 0

  ; Popcount
  println(unsafe(sized-bitvec-count(bv)))   ; => 3

  ; Clear and toggle
  unsafe(sized-bitvec-clear!(bv 0))
  unsafe(sized-bitvec-toggle!(bv 1))
  println(unsafe(sized-bitvec-count(bv)))   ; => 2

  ; Fill all bits
  unsafe(sized-bitvec-fill!(bv 1))
  println(unsafe(sized-bitvec-count(bv)))   ; => 16

  ; Length and size
  println(unsafe(sized-bitvec-len(bv)))                      ; => 16
  println(size-eval(unsafe(sized-bitvec-size(bv))))          ; => 16

  ; Length assertion (panics if length does not match)
  unsafe(sized-bitvec-assert-len!(bv (Static 16)))

  unsafe(sized-bitvec-free(bv))
```

---

## FFI Integration

Size annotations can be carried through Turmeric wrappers over opaque C
pointers, providing type-safe size information without any runtime cost.

```turmeric
; Wrapper that reports the byte size of a point struct via an inline-C accessor.
(defn ffi-point-size [] #{Unsafe} : Size
  ```c
  return ctor_Static(sizeof(struct { int64_t x; int64_t y; }));
  ```)

; Wrapper that reports the element count for a fixed-size C array.
(defn ffi-array-size [] #{Unsafe} : Size
  ```c
  return ctor_Static(16);
  ```)

; Wrapper that reports the field count of a struct.
(defn ffi-struct-field-count [] #{Unsafe} : Size
  ```c
  return ctor_Static(3);
  ```)

(println (size-eval (unsafe (ffi-point-size))))         ; => 16
(println (size-eval (unsafe (ffi-array-size))))         ; => 16
(println (size-eval (unsafe (ffi-struct-field-count)))) ; => 3
```

```sweet-exp
; Wrapper that reports the byte size of a point struct via an inline-C accessor.
defn ffi-point-size [] #{Unsafe} :Size
  ```c
  return ctor_Static(sizeof(struct { int64_t x; int64_t y; }));
  ```

; Wrapper that reports the element count for a fixed-size C array.
defn ffi-array-size [] #{Unsafe} :Size
  ```c
  return ctor_Static(16);
  ```

; Wrapper that reports the field count of a struct.
defn ffi-struct-field-count [] #{Unsafe} :Size
  ```c
  return ctor_Static(3);
  ```

println(size-eval(unsafe(ffi-point-size())))         ; => 16
println(size-eval(unsafe(ffi-array-size())))         ; => 16
println(size-eval(unsafe(ffi-struct-field-count()))) ; => 3
```

The pattern is: return `ctor_Static(n)` from inline C to inject a sized
annotation into Turmeric's type system.

---

## Error Messages

Passing an `int` where a `Size` is expected is a compile-time error:

```turmeric
; ERROR: TUR-E0001: expected <adt>, got int
(size-add 3 4)
```

```sweet-exp
; ERROR: TUR-E0001: expected <adt>, got int
size-add(3 4)
```

Shape or length assertions produce descriptive runtime messages:

```turmeric
; Panics: "row mismatch" (or "col mismatch")
(unsafe (sized-matrix-assert-shape! m (Static 2) (Static 2)))

; Panics: "sized type mismatch"
(unsafe (sized-bitvec-assert-len! bv (Static 8)))

; Panics: "sized type mismatch"
(size-assert-eq! (size-static 4) (size-static 5))
```

```sweet-exp
; Panics: "row mismatch" (or "col mismatch")
unsafe(sized-matrix-assert-shape!(m (Static 2) (Static 2)))

; Panics: "sized type mismatch"
unsafe(sized-bitvec-assert-len!(bv (Static 8)))

; Panics: "sized type mismatch"
size-assert-eq!(size-static(4) size-static(5))
```

---

## API Reference

### stdlib/sized.tur

#### StaticInt

| Function             | Signature                              | Description                       |
|----------------------|----------------------------------------|-----------------------------------|
| `static-int`         | `(-> int StaticInt)`                   | Wrap an integer as a `StaticInt`  |
| `static-int-val`     | `(-> StaticInt int)`                   | Unwrap to a plain integer         |
| `static-int-add`     | `(-> StaticInt StaticInt StaticInt)`   | Add two `StaticInt` values        |
| `static-int-mul`     | `(-> StaticInt StaticInt StaticInt)`   | Multiply two `StaticInt` values   |

#### Size

| Function          | Signature                        | Description                                       |
|-------------------|----------------------------------|---------------------------------------------------|
| `size-static`     | `(-> int Size)`                  | Literal size from an integer                      |
| `size-add`        | `(-> Size Size Size)`            | Sum of two sizes                                  |
| `size-mul`        | `(-> Size Size Size)`            | Product of two sizes                              |
| `size-eval`       | `(-> Size int)`                  | Evaluate to a runtime integer                     |
| `size-normalize`  | `(-> Size Size)`                 | Flatten to `(Static n)`                           |
| `size-simplify`   | `(-> Size Size)`                 | Algebraic simplification                          |

#### Predicates and assertions

| Function           | Signature                      | Description                                      |
|--------------------|--------------------------------|--------------------------------------------------|
| `size-eq?`         | `(-> Size Size bool)`          | True if sizes evaluate equal                     |
| `size-lt?`         | `(-> Size Size bool)`          | True if s1 < s2                                  |
| `size-le?`         | `(-> Size Size bool)`          | True if s1 <= s2                                 |
| `size-gt?`         | `(-> Size Size bool)`          | True if s1 > s2                                  |
| `size-ge?`         | `(-> Size Size bool)`          | True if s1 >= s2                                 |
| `size-compat?`     | `(-> Size Size bool)`          | True if sizes are equal (runtime subtyping)      |
| `size-assert-eq!`  | `(-> Size Size nil)`           | Panic if s1 != s2                                |
| `size-assert-le!`  | `(-> Size Size nil)`           | Panic if s1 > s2                                 |

#### SizedVec

| Function               | Signature                         | Description                                  |
|------------------------|-----------------------------------|----------------------------------------------|
| `sized-vec-of-1`       | `(-> int SizedVec)`               | 1-element vec; size inferred as 1            |
| `sized-vec-of-2`       | `(-> int int SizedVec)`           | 2-element vec; size inferred as 2            |
| `sized-vec-of-3`       | `(-> int int int SizedVec)`       | 3-element vec; size inferred as 3            |
| `sized-vec-of-4`       | `(-> int int int int SizedVec)`   | 4-element vec; size inferred as 4            |
| `sized-vec-from-list`  | `#fx{Unsafe} (-> int SizedVec)`     | Convert a cons list; size inferred at runtime |
| `sized-vec-push`       | `(-> SizedVec int SizedVec)`      | Prepend an element; size becomes n+1         |
| `sized-vec-len`        | `(-> SizedVec int)`               | Element count                                |
| `sized-vec-get`        | `(-> SizedVec int int)`           | Bounds-checked element read                  |
| `sized-vec-size`       | `(-> SizedVec Size)`              | Size as a `Size` expression                  |

### stdlib/sized-buf.tur

| Function                  | Signature                          | Description                                       |
|---------------------------|------------------------------------|---------------------------------------------------|
| `sized-buf-new`           | `#fx{Unsafe} (-> int int)`           | Heap-allocate n elements (uninitialized)          |
| `sized-buf-new-zeroed`    | `#fx{Unsafe} (-> int int)`           | Heap-allocate n elements, zeroed                  |
| `sized-buf-free`          | `#fx{Unsafe} (-> int nil)`           | Free a heap-allocated buffer                      |
| `sized-buf-len`           | `#fx{Unsafe} (-> int int)`           | Element count                                     |
| `sized-buf-get`           | `#fx{Unsafe} (-> int int int)`       | Bounds-checked element read                       |
| `sized-buf-set!`          | `#fx{Unsafe} (-> int int int int)`   | Bounds-checked element write; returns buf         |
| `sized-buf-fill!`         | `#fx{Unsafe} (-> int int int)`       | Set all elements to a value                       |
| `sized-buf-copy!`         | `#fx{Unsafe} (-> int int nil)`       | Copy src into dst (equal lengths required)        |
| `sized-buf-sum`           | `#fx{Unsafe} (-> int int)`           | Sum of all elements                               |
| `sized-buf-min`           | `#fx{Unsafe} (-> int int)`           | Minimum element                                   |
| `sized-buf-max`           | `#fx{Unsafe} (-> int int)`           | Maximum element                                   |
| `sized-buf-size`          | `#fx{Unsafe} (-> int Size)`          | Element count as a `Size` expression              |
| `sized-buf-with-stack`    | `#fx{Unsafe} (-> int fn int)`        | Stack-allocate n elements; call f with buffer     |
| `sized-buf-compute`       | `#fx{Unsafe} (-> int int)`           | Stack when n<=64, heap otherwise (demo)           |
| `sized-buf-from-sized-vec`| `#fx{Unsafe} (-> SizedVec int)`      | Convert a `SizedVec` to a flat heap buffer        |

### stdlib/sized-matrix.tur

| Function                    | Signature                                 | Description                                  |
|-----------------------------|-------------------------------------------|----------------------------------------------|
| `sized-matrix-new`          | `#fx{Unsafe} (-> int int int)`              | Allocate rows x cols (uninitialized)         |
| `sized-matrix-new-zeroed`   | `#fx{Unsafe} (-> int int int)`              | Allocate rows x cols, zeroed                 |
| `sized-matrix-free`         | `#fx{Unsafe} (-> int nil)`                  | Free the matrix                              |
| `sized-matrix-rows`         | `#fx{Unsafe} (-> int int)`                  | Row count                                    |
| `sized-matrix-cols`         | `#fx{Unsafe} (-> int int)`                  | Column count                                 |
| `sized-matrix-size`         | `#fx{Unsafe} (-> int Size)`                 | Total element count as `Size`                |
| `sized-matrix-row-size`     | `#fx{Unsafe} (-> int Size)`                 | Column count as `Size`                       |
| `sized-matrix-get`          | `#fx{Unsafe} (-> int int int int)`          | Bounds-checked element read at (r, c)        |
| `sized-matrix-set!`         | `#fx{Unsafe} (-> int int int int int)`      | Bounds-checked element write at (r, c)       |
| `sized-matrix-fill!`        | `#fx{Unsafe} (-> int int int)`              | Set all elements to a value                  |
| `sized-matrix-row-sum`      | `#fx{Unsafe} (-> int int int)`              | Sum of row r                                 |
| `sized-matrix-col-sum`      | `#fx{Unsafe} (-> int int int)`              | Sum of column c                              |
| `sized-matrix-total-sum`    | `#fx{Unsafe} (-> int int)`                  | Sum of all elements                          |
| `sized-matrix-assert-shape!`| `#fx{Unsafe} (-> int Size Size nil)`        | Panic on shape mismatch                      |

### stdlib/sized-bits.tur

| Function                   | Signature                          | Description                                    |
|----------------------------|------------------------------------|------------------------------------------------|
| `sized-bitvec-new`         | `#fx{Unsafe} (-> int int)`           | Allocate n bits, all zero                      |
| `sized-bitvec-free`        | `#fx{Unsafe} (-> int nil)`           | Free the bit vector                            |
| `sized-bitvec-len`         | `#fx{Unsafe} (-> int int)`           | Bit count                                      |
| `sized-bitvec-size`        | `#fx{Unsafe} (-> int Size)`          | Bit count as a `Size` expression               |
| `sized-bitvec-get`         | `#fx{Unsafe} (-> int int int)`       | Read bit i; returns 0 or 1                     |
| `sized-bitvec-set!`        | `#fx{Unsafe} (-> int int int)`       | Set bit i to 1; returns bv                     |
| `sized-bitvec-clear!`      | `#fx{Unsafe} (-> int int int)`       | Clear bit i to 0; returns bv                   |
| `sized-bitvec-toggle!`     | `#fx{Unsafe} (-> int int int)`       | Flip bit i; returns bv                         |
| `sized-bitvec-count`       | `#fx{Unsafe} (-> int int)`           | Popcount (number of set bits)                  |
| `sized-bitvec-fill!`       | `#fx{Unsafe} (-> int int int)`       | Set all bits to v (0 or 1); returns bv         |
| `sized-bitvec-assert-len!` | `#fx{Unsafe} (-> int Size nil)`      | Panic if bit count does not match expected     |
