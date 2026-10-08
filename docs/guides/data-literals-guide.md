---
title: Data Literals Guide
category: Language Basics
description: Compact literal syntax for maps, vecs, and sets using #map{...}, #set{...}, and [...]
---

# Data Literals: `#map{...}`, `#set{...}`, and `[...]`

Turmeric's data literals give map, vec, and set construction a compact
literal syntax whose slot values are ordinary expressions, evaluated at
runtime in the surrounding scope -- a lambda included, so a `#map{...}` of
`(fn ...)` values is a dispatch table. They are sugar over the `hamt-of`,
`vec-of`, and `set-of` stdlib macros.

```turmeric no-check
(def payload #map{:name name :age (+ age 1) :active 1})
(def points  [(make-point 0 0) (make-point 1 1) origin])
(def small   #set{1 2 3})
```
```sweet-exp
def payload #map{:name name :age {age + 1} :active 1}
def points [make-point(0 0) make-point(1 1) origin]
def small #set{1 2 3}
```

## The three forms

| Syntax | Lowers to | Notes |
|---|---|---|
| `[e1 e2 e3 ...]` *(expression position)* | `(vec-of e1 e2 e3 ...)` | element type inferred from the first element |
| `#map{k1 v1 k2 v2 ...}` | `(hamt-of k1' v1 k2' v2 ...)` | keys normalized (see below); last duplicate key wins |
| `#set{e1 e2 e3 ...}` | `(set-of e1 e2 e3 ...)` | duplicate elements collapse |
| `#refine{ var : T \| pred }` | contract-type annotation | see [Contract Types Guide](contract-types-guide.md) |
| `#rat{n/d}` | `(rat/of! n' d')` | normalized at **read** time; see [Numeric literals](#numeric-literals-rat-and-cx) |
| `#cx{re im}` | `(complex/of re im)` | two ordinary expression slots |

```turmeric no-check
[1 2 3]                       ; => (vec-of 1 2 3)
#map{:a 1 :b x}               ; => map {:a -> 1, :b -> x}
#set{:literal x (compute)}    ; (compute) evaluated once
```
```sweet-exp
[1 2 3]
; => (vec-of 1 2 3)
#map{:a 1 :b x}
; => map {:a -> 1, :b -> x}
#set{:literal x (compute)}
; (compute) evaluated once
```

### Element types

`[...]` and the value slots of `#map{...}` accept any scalar element type --
`:int`, `:cstr`, `:bool`, `:nil`, or `:float` -- not just `:int`. All
elements of a vector must unify to one type, and all map values to one type
(a mixed literal such as `[1 "x"]` is a `TUR-E0001` on the offending
element). Elements are stored through an `:int` carrier slot, so reads
recover the type with an ascription:

```turmeric no-check
[1.5 2.5 3.5]                 ; Vec[float]
(:: (vec-get fs 0) :float)    ; => 1.5

#map{1 "one" 2 "two"}         ; Map[int cstr]
(:: (map-get m 1) :cstr)      ; => "one"
```
```sweet-exp
[1.5 2.5 3.5]
; Vec[float]
::(vec-get(fs 0) :float)
; => 1.5
#map{1 "one" 2 "two"}
; Map[int cstr]
::(map-get(m 1) :cstr)
; => "one"
```

Map keys may be any `Hash`/`MapKey` scalar -- **int**, **keyword**, **string**,
**bool**, **float32**, **float**, or a user type with `Hash`/`Eq`/`MapKey`
instances. The literal has type `Map[K V]`: the typed `map-get`/`map-assoc`/
`map-has?`/`map-dissoc` surface reads and writes it, and a key/value-type
mismatch is a `TUR-E0001`. Int and keyword keys are int-valued (a keyword
normalizes to a content hash); string keys are compared by content (two
distinct pointers with equal text are one key). See
[`#map{...}` keys](#map-keys) below.

> **Keyword keys:** a keyword key is a first-class `:Sym` value rather than
> a content-hashed string -- the map is keyed by `Sym` pointer identity via
> `Hash[Sym]` / `MapKey[Sym]`. See [the symbols guide](symbols-guide.md).
> Aggregate (multi-word struct/ADT) *key* types must supply their own
> `MapKey` instance; *values* may be any type (scalars, floats, and heap
> handles like `Vec[A]` all ride the int64 carrier).

Slots are arbitrary expressions -- variable references, calls, nested
literals -- and the normal typechecker handles them.

## `[...]` is only a vec in *expression* position

The reader parses every `[...]` as the same vector form. Binding forms
(`defn`/`fn`/`defmethod` parameter lists, and `let`/`loop`/`for`/... binding
vectors) consume that form as a *binding spec* before it is ever treated as a
value. Only a `[...]` that reaches expression position lowers to `(vec-of
...)`. This is the same rule Clojure uses: `[x y]` is always a vector; the
surrounding form decides whether it is read as a binding spec or a value.

```turmeric
(defn add [x : int y : int] : int (+ x y))   ; [x :int y :int] is a param list
(let [a 1 b 2] (+ a b))                   ; [a 1 b 2] is a binding vector
(def v [1 2 3])                           ; [1 2 3] is a vec literal
```
```sweet-exp
defn add [x :int y :int] :int
  {x + y}
; [x :int y :int] is a param list
let [a 1 b 2]
  {a + b}
; [a 1 b 2] is a binding vector
def v [1 2 3]
; [1 2 3] is a vec literal
```

## `#map{...}` keys

Keys must be a **keyword**, **string literal**, or **int literal** -- anything
else (a variable, a call) is rejected at read time with `TUR-E0282`. Computed
keys are intentionally disallowed so a reader can tell keys from values at a
glance.

How a key lowers depends on its form:

- An **int** key passes through unchanged (identity-keyed, zero overhead).
- A **keyword** key lowers to `(hamt/hash-str "name")` -- the keyword's content
  hash *is* the int key, so two equal keywords collide deliberately. Look a
  keyword key back up by hashing it the same way:

  ```turmeric
  (let [m #map{:name 1 :age 2}
        k (hamt/hash-str "name")]
    (map-get m k))             ; => 1
  ```
```sweet-exp
let [m #map{:name 1 :age 2}
        k (hamt/hash-str "name")]
  map-get(m k)
; => 1
```

- A literal whose keys are all **string literals** lowers to a *content-keyed*
  map. The string itself is the key (compared by content, not by pointer or
  hash), so two distinct pointers with equal text are one key. Read string
  keys with the same unified `map-get` / `map-has?` / `map-dissoc` accessors
  (they dispatch through `Hash[cstr]` / `MapKey[cstr]` by content):

  ```turmeric
  (let [m #map{"name" 1 "age" 2}]
    (:: (map-get m "name") :int))   ; => 1
  ```
```sweet-exp
let [m #map{"name" 1 "age" 2}]
  ::(map-get(m "name") :int)
; => 1
```

  A direct `(hamt-of "k" v ...)` call with string keys is content-keyed too --
  there is one `hamt-of` builder for every key type (the old `smap-of` /
  `smap-*` split was retired in TMS3). (Runtime-built string keys must outlive
  the map -- the HAMT does not copy keys; see
  [GMK / TCE4](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/typed-collection-elements-plan.md).) 

An odd number of slot forms is a `TUR-E0280` read error.

## `#set{...}` elements and hashing

Set elements are arbitrary value expressions (no key-form restriction). Each
element is used as **its own hash** -- the identity-hash convention the typed
`Set[A]` uses everywhere (`(set-add s 42 42)`). Scalar element types (int,
etc.) dedupe by value:

```turmeric no-check
#set{1 1 2}        ; => set with two elements, {1, 2}
#set{x {x + x} y}  ; each element expression evaluated exactly once
```
```sweet-exp
#set{1 1 2}
; => set with two elements, {1, 2}
#set{x {x + x} y}
; each element expression evaluated exactly once
```

> **Note on hashing.** Scalar elements are their own hash (the identity-hash
> convention above). Keyword elements are first-class `:Sym` values, so
> `#set{:a :b}` builds a `Set[Sym]` content-keyed on the interned symbol;
> membership tests take the symbol's hash explicitly, e.g.
> `(set-member? s (hash :a) :a)`. See [the symbols guide](symbols-guide.md).

### Element homogeneity and `(Set any)`

Like `[...]` and `#map{...}`, a set literal is homogeneous: every element must
have the set's element type `A`, and a mismatch is a `TUR-E0001` at the
offending element (`function 'set-add-elem__' arg 2: expected tyvar, got cstr`),
exactly as `vec-of` reports one at `vec-push!`. A set of mixed values is spelled
`(Set any)` -- widen each element with `(:: e any)` -- and the stdlib's
`Hash [any]` / `MapKey [any]` instances key each `any` by its payload, so an
`any` holding `"two"` is content-keyed like a bare `"two"`, an `any` holding
`7.1` compares by value like a bare `7.1`, and a bare `1` finds the boxed `1`:

```turmeric no-check
(set-of (:: 1 any) (:: "two" any) (:: 7.1 any) (:: "two" any))  ; => (Set any), 3 members
(set-member? s (hash 1) 1)                                       ; => true
```

In a `#lang saffron` file the literal does this widen itself, so
`#set{1 "two" 7.1}` is a `(Set any)` with no annotation -- the same rule that
makes `[1 "two" 7.1]` a `(Vec any)` there.

## Empty literals

```turmeric no-check
#map{}   ; => empty Map
#set{}   ; => empty Set
[]       ; => empty Vec  (expression position)
```
```sweet-exp
#map{}
; => empty Map
#set{}
; => empty Set
[]
; => empty Vec  (expression position)
```

### Typed empty literals -- `[]:T` / `#set{}:T` / `#map{}:(K V)`

An empty literal has no element to infer a type from, so a later typed
operation (`.eq?`, `.push!`) can't recover its element type. Pin it with a
fused `:T` element-type suffix immediately after the closer:

```turmeric no-check
[]:int             ; empty Vec[int]
#set{}:cstr        ; empty Set[cstr]
[]:(Vec int)       ; empty Vec[Vec[int]]  (parenthesize a compound element type)
#map{}:(cstr int)  ; empty Map[cstr, int]  (a pair -- Map takes two parameters)
```
```sweet-exp
[]:int
; empty Vec[int]
#set{}:cstr
; empty Set[cstr]
[]:(Vec int)
; empty Vec[Vec[int]]  (parenthesize a compound element type)
#map{}:(cstr int)
; empty Map[cstr, int]  (a pair -- Map takes two parameters)
```

`#map{}` is the one two-parameter case, so its suffix is a parenthesised
**pair** of types rather than a single one: `#map{}:(cstr int)` is
`Map[cstr, int]`. The parentheses are required -- the arity has to come from
somewhere, and a bare `#map{}:cstr int` would swallow whatever token followed
the literal. This does not clash with the compound-element reading above
(`[]:(Vec int)` is one element type that happens to be parenthesised): which
reading applies is fixed by the literal kind, not by the shape of the suffix.

The `#set{}:cstr` above pins the element type, but note the element *type* you
pick still matters for lifetimes: a `Set[cstr]` of **computed** keys borrows
each key pointer and dangles once the source is freed. When the keys are built or
stored (not static literals), use `#set{}:String` -- a `Set[String]` copies each
key into a box the set owns. See [strings-guide.md](strings-guide.md).

The suffix desugars to an ascription on the literal, so `[]:int` is exactly
`(:: (vec-of) (Vec int))`, `#set{}:T` is `(:: (set-of) (Set T))`, and
`#map{}:(K V)` is `(:: #map{} (Map K V))` -- the generated code is identical,
and the suffix is neither stronger nor weaker than the ascription it stands
for. It replaces the verbose
`(:: (vec-new) (Vec int))` scaffolding that empty containers previously
needed:

```turmeric
;; before
(let [a (:: (vec-new) (Vec int))]
  (vec-push! a 1) ...)

;; after
(let [a []:int]
  (vec-push! a 1) ...)
```
```sweet-exp
;; before
let [a (:: (vec-new) (Vec int))]
  vec-push!(a 1)
  ...
;; after
let [a []:int]
  vec-push!(a 1)
  ...
```

Rules:

- The `:` must be **immediately adjacent** to the closing `]` / `}` (no
  intervening whitespace). A space-separated `[] :int` is two forms, not a
  suffix -- this is what keeps binding vectors (`[x :int y :int]`, whose `]`
  is followed by a space) unaffected.
- A `::` after the closer is **not** a suffix (it is the ascription operator).
- The suffix also works on non-empty literals (`[1 2 3]:int`), where it is
  merely a redundant re-statement of the inferred type.
- Missing type after the `:` is a read error: *"expected an element type
  after the ':' container-literal suffix"*.
- On `#map{}` the suffix must be a parenthesised pair. `#map{}:int`,
  `#map{}:(int)`, and `#map{}:(a b c)` are all read errors naming the
  expected `#map{}:(<key> <value>)` shape, rather than reaching the
  elaborator as a mis-arity type application.

## Numeric literals: `#rat{...}` and `#cx{...}`

The numeric tower ships two more literals, for the exact rational type in
[`stdlib/rational.tur`](https://github.com/turmeric-lang/turmeric/blob/main/stdlib/rational.tur) and the complex type in
[`stdlib/complex.tur`](https://github.com/turmeric-lang/turmeric/blob/main/stdlib/complex.tur). Both are always on -- they are
core data-literal dispatches, not `#lang` layers.

### `#rat{n/d}` -- exact rationals

```turmeric no-check
#rat{3/4}      ; => (rat/of! 3 4)
#rat{-3/4}     ; => (rat/of! -3 4)
#rat{3/-4}     ; => (rat/of! -3 4)   -- the sign moves to the numerator
#rat{6/8}      ; => (rat/of! 3 4)    -- normalized at READ time
#rat{5}        ; => (rat/of! 5 1)    -- a bare whole number
#rat{1/0}      ; => read-time error (TUR-E0284)
```

Bare `3/4` is deliberately **not** rational syntax. `read_number` stops at `/`,
so `3/4` already reads as `3` followed by the symbol `/4`; making it a rational
would collide with `/` as division and with module-qualified names like
`tur/list`. Hence the `#`-dispatch.

The body is read **raw**, not as forms. That matters because curly-infix is
enabled in every dialect: an ordinarily-read `{3 / 4}` would be infix division,
not a literal. So the slots of `#rat{...}` are plain int64 digit runs with an
optional sign -- no expressions, no whitespace tricks beyond padding.

Normalizing at read time means `#rat{6/8}` and `#rat{3/4}` are the *same*
literal, which is what makes structural equality on a `Rational` equal
mathematical equality:

```turmeric no-check
(eq? #rat{2/4} #rat{1/2})   ; => true
```

### `#cx{re im}` -- complex numbers

```turmeric no-check
#cx{3.25 -1.5}        ; => (complex/of 3.25 -1.5)
#cx{3.25 {1.0 + 0.5}} ; => (complex/of 3.25 (+ 1.0 0.5))
```

Unlike `#rat{...}`, the two slots are read as ordinary **forms**, so a computed
component composes -- including a curly-infix inner expression, which is exactly
what it looks like.

`#cx` rather than `#c`: a single-letter dispatch is too scarce a name to spend,
and `#c` reads as "C" in a codebase full of inline-C blocks.

There is deliberately no `i` imaginary suffix (`4.0i`). It only pays off with
mixed `float + Complex` arithmetic, which means an implicit widening coercion in
operator resolution -- a real type-system decision that should not ride along on
a literal-syntax change.

## Errors

| Code | Condition |
|---|---|
| `TUR-E0280` | Odd number of slot forms in `#map{...}` (unmatched key) |
| `TUR-E0281` | Unexpected EOF inside `#map{...}` or `#set{...}` |
| `TUR-E0282` | Invalid key form in `#map{...}` (must be keyword, string, or int literal) |
| `TUR-E0283` | Unknown `#<tag>{...}` dispatch tag |
| `TUR-E0284` | Malformed `#rat{...}`: bad body, trailing junk, zero denominator, or a part that does not fit in int64 |
| `TUR-E0285` | `#cx{...}` given other than exactly two slot forms |

## Relationship to the JSON reader macro

The `#json(...)` reader macro ([json-guide.md](json-guide.md)) embeds a
verbatim JSON blob, validated at compile time. The data literals cover the
"literal shape, computed values" case that a JSON-only reader cannot:
`#map{...}` accepts arbitrary value expressions. Use `#json(...)` for
pasting a literal JSON blob verbatim; for everything else, write the map
literal directly.

## Sweet-expression interaction

The reader dispatch sits below the sweet-expression layer, so the literals
work transparently inside `#lang sweet-exp` files alongside neoteric calls
and curly-infix arithmetic:

```turmeric no-check
#lang sweet-exp

(defn build [] :int
  (let [m #map{:a 1 :b 2}]
    map-count(m)))
```
```sweet-exp
#lang sweet-exp

defn build [] :int
  let [m #map{:a 1 :b 2}]
    map-count(m)
```
