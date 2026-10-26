---
title: Unsafe Code
category: Interoperability
description: The (unsafe ...) block, raw pointers and raw memory, bit reinterpretation, and when to reach for something safer
tags:
  - ffi
  - memory-safety
---

# Unsafe Code

Turmeric's type checker cannot see inside a raw pointer. The handful of
builtins that read and write memory directly -- `ptr-deref`, `raw-malloc`,
`transmute` and the rest -- are therefore allowed only where a program says it
is taking responsibility for them: inside an `(unsafe ...)` block, or in a
function declared `#fx{Unsafe}`. This guide says what each one does, what it
does *not* check, and what to use instead when you do not actually need it.

The per-form reference is [tur/forms/unsafe](../html/api/tur-forms-unsafe.html);
this page is the when-and-why.

## The `unsafe` block and the `Unsafe` effect

```turmeric
(defn first-word [p : ptr<void>] : int
  (unsafe (ptr-deref p)))
```

```sweet-exp
#lang sweet-exp

defn first-word [p : ptr<void>] : int
  unsafe ptr-deref(p)
```

Outside the block, `(ptr-deref p)` is refused at compile time. Inside it, the
builtins on this page and calls to `#fx{Unsafe}` functions are allowed. The
block **discharges** the `Unsafe` effect: `first-word` above has no `Unsafe` in
its row, so its callers treat it as an ordinary function. That is the point of
the block -- a safe wrapper over an unsafe primitive, with the unsafety kept in
one place.

A function declared `#fx{Unsafe}` gets the same permission for its whole body,
and passes the obligation on: each caller needs a block (or `#fx{Unsafe}`) of
its own.

```turmeric
(defn poke! [p : ptr<void> v : int] #fx{Unsafe} : nil
  (ptr-write p v))
```

```sweet-exp
#lang sweet-exp

defn poke! [p : ptr<void> v : int] #fx{Unsafe} : nil
  ptr-write(p v)
```

Lints keep the blocks small: an empty block warns, a block of more than 20
forms warns (`--lint-unsafe-max-lines=N` changes the limit), and
`--lint-unsafe` also warns on a block nested in another. Borrowing from a
`ptr<void>` (`(& p)`) needs the block too.

Under a sandboxed interpreter these builtins need the `TURI_CAP_UNSAFE`
capability ([sandboxing-guide](sandboxing-guide.md)).

## Raw memory

| Form | C | Notes |
|---|---|---|
| `(raw-malloc n)` | `malloc(n)` | `n` bytes, uninitialised; a null pointer on failure |
| `(raw-free p)` | `free(p)` | a null pointer is a no-op |
| `(raw-realloc p n)` | `realloc(p, n)` | the block may move: use the result, not `p` |
| `(raw-memcpy dst src n)` | `memcpy` | the blocks must not overlap |
| `(raw-memset dst b n)` | `memset` | the low 8 bits of `b`, into each of `n` bytes |

The pointer from `raw-malloc` is **move-typed**: handing it to a function
consumes it, and a second use is TUR-E0201. The builtins on this page only
borrow it, so a block can read, write and free the same pointer.

## Pointers are words, offsets are bytes

The pointer builtins treat memory as 8-byte words (`int64_t`), and that is the
one thing to remember about them:

| Form | C | Unit |
|---|---|---|
| `(ptr-deref p)` | `*(int64_t *)p` | reads one word, returned as `int` |
| `(ptr-write p v)` | `*(int64_t *)p = v` | writes one word |
| `(array-get-unchecked p i)` | `((int64_t *)p)[i]` | `i` counts **words** |
| `(array-set-unchecked p i v)` | `((int64_t *)p)[i] = v` | `i` counts **words** |
| `(ptr-add p n)` / `(ptr-sub p n)` | `(char *)p + n` / `- n` | `n` counts **bytes** |
| `(ptr-null? p)` | `p == NULL` | |
| `(ptr-of x)` | `&x` | dangling once `x`'s scope ends |

So the second word of a block is `(array-get-unchecked p 1)` or
`(ptr-deref (ptr-add p 8))` -- not `(ptr-add p 1)`.

```turmeric
(defn squares-sum [n : int] : int
  (unsafe
    (let [p      (raw-malloc (* n 8))
          ^mut i 0]
      (while (< i n)
        (array-set-unchecked p i (* i i))
        (set! i (+ i 1)))
      (let [last     (ptr-deref (ptr-add p (* (- n 1) 8)))
            ^mut acc 0
            ^mut j   0]
        (while (< j n)
          (set! acc (+ acc (array-get-unchecked p j)))
          (set! j (+ j 1)))
        (raw-free p)
        (+ acc last)))))

(defn main [] : int
  (println (squares-sum 4))   ; 0+1+4+9 = 14, plus the last word 9 => 23
  0)
```

```sweet-exp
#lang sweet-exp

defn squares-sum [n : int] : int
  unsafe
    let [p      raw-malloc({n * 8})
         ^mut i 0]
      while {i < n}
        array-set-unchecked(p i {i * i})
        set!(i {i + 1})
      let [last     ptr-deref(ptr-add(p {{n - 1} * 8}))
           ^mut acc 0
           ^mut j   0]
        while {j < n}
          set!(acc {acc + array-get-unchecked(p j)})
          set!(j {j + 1})
        raw-free(p)
        {acc + last}

defn main [] : int
  println(squares-sum(4))   ; 0+1+4+9 = 14, plus the last word 9 => 23
  0
```

Nothing here is checked at run time. An index past the block, a pointer used
after `raw-free`, or a `ptr-of` that outlived its local is undefined behaviour,
exactly as in C. A Debug build of a program compiled with
`TUR_CC_FLAGS=-fsanitize=address` is the quickest way to find one.

## Casts and bit reinterpretation

| Form | What it does |
|---|---|
| `(unsafe-cast x T)` | `(int64_t)x`: a C conversion to `int`. `T` is required but not read; a `float` truncates, `true` is 1 |
| `(transmute x T)` | copies `x`'s bits into a `T` of the **same size** (checked at compile time) |
| `(reinterpret x T)` | the same bit copy as `transmute` |

```turmeric
(defn main [] : int
  (unsafe
    (println (transmute 1.5 :int))                       ; 4609434218613702656
    (println (transmute (transmute 1.5 :int) :float))    ; 1.5
    (println (unsafe-cast 7.9 :int)))                    ; 7
  0)
```

```sweet-exp
#lang sweet-exp

defn main [] : int
  unsafe
    println(transmute(1.5 :int))                       ; 4609434218613702656
    println(transmute(transmute(1.5 :int) :float))     ; 1.5
    println(unsafe-cast(7.9 :int))                     ; 7
  0
```

`(transmute (:: 1 :int32) :int)` is a compile error: the sizes differ, and
there is no defined way to fill the missing bytes.

## Calling C

`(c-call f args ...)` calls a function declared with `extern-c`, with the
result typed `int` whatever the declaration says. Calling the `extern-c` name
directly keeps the declared types and needs no block, so prefer that; `c-call`
is for the rare call whose declared result you want erased. `dlopen`, `dlsym`
and `dlclose` load libraries at run time; [ffi-guide](ffi-guide.md) covers them
with `call-ptr`.

## Reach for something safer first

Most programs that want one of these want something else:

| You want | Use instead |
|---|---|
| a float's bit pattern, or a float from bits | `float->bits` / `bits->float` (`tur/bits`), no block needed |
| the number in a float, as an int | `float->int` (`tur/math`) or `(as int x)` |
| to call a C library | `extern-c`, which keeps the declared types ([c-integration-guide](c-integration-guide.md)) |
| a buffer of words | `(Vec int)` -- bounds-checked, freed with `vec-free` |
| a fallible C constructor | inline C returning `(Result T E)` ([inline-c-results-guide](inline-c-results-guide.md)) |
| an opaque C handle | `defopaque`, so the checker can tell handles apart |

## Related

- [ffi-guide](ffi-guide.md) -- `dlopen` / `dlsym` / `call-ptr`
- [c-integration-guide](c-integration-guide.md) -- `extern-c` and inline C
- [effect-rows-internals-guide](effect-rows-internals-guide.md) -- how the
  `Unsafe` effect is checked
- [sandboxing-guide](sandboxing-guide.md) -- `TURI_CAP_UNSAFE`
