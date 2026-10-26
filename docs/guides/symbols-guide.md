---
title: Symbols Guide
category: Language Basics
description: First-class runtime symbols (:Sym type) with compile-time interning and optional dynamic str->sym table
tags:
  - strings
---

# Runtime Symbols: `:Sym`

> Covers the `:Sym` type, per-TU codegen, cross-TU interning, map-literal +
> typeclass integration, the stdlib surface, and the opt-in dynamic
> `str->sym` intern table.

Turmeric's `:foo` keyword syntax parses cleanly and interns its name at read
time. In *expression position*, `:foo` is a first-class expression of type
`:Sym` whose runtime value is a unique, deduplicated pointer into `.rodata`.

```turmeric
(println (sym->str :hello))   ; => hello
```
```sweet-exp
println(sym->str(:hello))
; => hello
```

## The model

A `:Sym` value is a non-null pointer to a static, process-lifetime record:

```c
struct __tur_sym {
    uint64_t hash;   /* precomputed xxHash64 of the name */
    uint32_t len;    /* byte length, excluding NUL */
    uint32_t _pad;
    char     name[]; /* NUL-terminated UTF-8 */
};
```

Every distinct `:foo` lowers to **one** record, so:

- **Equality is pointer equality** -- two `:foo` references are the same
  pointer (`==`); `:foo` and `:bar` are distinct.
- **Hashing is a single field load** -- skipping any string work or allocation. The
  hash is precomputed by the compiler.
- **`sym->str` is free** -- it returns the address of the embedded name; no
  copy, no `strlen`.

The C identifier of each record is `__tur_sym_` followed by a percent-encoded
mangling of the name, so punctuated keywords (`:a-b`, `:*x*`) still produce a
unique, ASCII-only symbol.

In a multi-module build (`tur build <dir>`) the records are emitted with
external weak linkage, so the linker folds the per-module copies of `:foo`
into a single object -- `:foo` is one pointer across the whole program, not
just within a single file. Single-file `emit-c` output keeps the records
`static` so it stays self-contained.

## Expression position vs. syntactic position

The runtime value for `:foo` does **not** change any of its syntactic uses.
These are consumed by earlier elaborator passes:

- Type annotations: `:int`, `:cstr`, `:Sym`
- Import directives: `:refer [...]`, `:as foo`
- Struct field selectors: `(:name p)`
- `:else` in `cond`/`case` (the bare symbol `else` names the same clause)

The `:Sym` value appears only in **expression position**, where `:foo`
evaluates to the interned symbol.

## The `:Sym` type

`:Sym` is a nullary type that prints as `:Sym`. It is **not** a subtype of
`cstr`; conversion is explicit via `sym->str`. A value of type `:Sym` is
freely copyable (it is just an interned pointer).

You can annotate parameters and returns with it:

```turmeric
(defn label [tag : Sym] : cstr
  (sym->str tag))
```
```sweet-exp
defn label [tag :Sym] :cstr
  sym->str(tag)
```

## Stdlib surface (`stdlib/sym.tur`)

`sym.tur` is auto-loaded into the global namespace. It provides:

| Function | Signature | Notes |
|---|---|---|
| `sym->str` | `(sym->str s :Sym) :cstr` | embedded name, no allocation |
| `sym=?`    | `(sym=? a :Sym b :Sym) :bool` | pointer-identity equality |

```turmeric
(sym->str :hello)   ; => "hello"
(sym=? :a :a)       ; => true
(sym=? :a :b)       ; => false
```
```sweet-exp
sym->str(:hello)
; => "hello"
sym=?(:a :a)
; => true
sym=?(:a :b)
; => false
```

## Equality, hashing, and maps

`:Sym` participates in the `Eq`, `Hash`, and `MapKey` typeclasses (instances
live in `stdlib/sym.tur`):

```turmeric
(eq? :foo :foo)   ; => true   (pointer identity)
(eq? :foo :bar)   ; => false
(hash :foo)       ; precomputed field load
```
```sweet-exp
eq?(:foo :foo)
; => true   (pointer identity)
eq?(:foo :bar)
; => false
hash(:foo)
; precomputed field load
```

A keyword key in a map literal is a first-class `:Sym` key rather than a
content-hashed string:

```turmeric no-check
(let [m #map{:foo 10 :bar 20}]
  (map-get m :foo)      ; => 10
  (map-has? m :missing) ; => false (0))
```
```sweet-exp
let [m #map{:foo 10 :bar 20}]
  map-get(m :foo)
  ; => 10
  map-has?(m :missing)
  ; => false (0))
```

The map is keyed by `Sym` pointer identity (via `Hash[Sym]` + `MapKey[Sym]`),
so lookups are pointer comparisons with the precomputed hash -- no string
work. String keys (`#map{"foo" 1}`) keep their hash-by-content lowering.

## Showing symbols

`Show [Sym]` renders a symbol with its leading colon, so `(show :hello)` is
`":hello"`. The instance lives in `stdlib/typeclass-show.tur` rather than
`sym.tur`, because that is where the `Show` class itself is defined and
`sym.tur` loads first.

```turmeric
(show :hello)   ; => ":hello"
```
```sweet-exp
show(:hello)
; => ":hello"
```

Sym-keyed and Sym-element collections render through it as well, so
`#map{:a 1}` displays as `#map{:a 1}` and `#set{:x :y}` as `#set{:x :y}` --
in compiled code, the interpreter, `tur repl`, and the web REPL alike.

## Dynamic interning: `str->sym` (opt-in)

Literal `:foo` symbols need no runtime table. To build a symbol from a string
at runtime (deserialization, REPL tools), load the opt-in module:

```turmeric
(load "stdlib/sym-dynamic.tur")

(eq? :hello (str->sym "hello"))   ; => true  (same pointer as the literal)
(eq? (str->sym "x") (str->sym "x")) ; => true  (stable; one record allocated)
(sym->str (str->sym "round"))     ; => "round"
```
```sweet-exp
load("stdlib/sym-dynamic.tur")
eq?(:hello str->sym("hello"))
; => true  (same pointer as the literal)
eq?(str->sym("x") str->sym("x"))
; => true  (stable; one record allocated)
sym->str(str->sym("round"))
; => "round"
```

`str->sym` returns a stable, process-lifetime symbol; repeated calls with the
same name return the same pointer (allocating at most one record), and the
table is mutex-guarded for concurrent use. For a name that also appears as a
literal in the program, `str->sym` returns the **same pointer** as the literal
(a startup constructor seeds the table with the static records).

Keep it opt-in: loading `sym-dynamic.tur` links the process-global intern
table (`src/runtime/symbols.c`). Programs that use only literal `:foo` symbols
never link it. `str->sym` hashes and locks, so it is slower than a literal --
prefer literals on hot paths.

> **Multi-module note:** `str->sym("foo")` matches a literal `:foo` only when
> that literal appears in a module that also loads `sym-dynamic.tur` (or is
> linker-folded with one). A `:foo` confined to a module that never loads the
> dynamic surface is interned as a separate record. Single-file programs are
> unaffected.

See `docs/archive/history/runtime-symbols-plan.md` for the full phase breakdown.
