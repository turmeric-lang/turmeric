---
title: Type-Level Rows
category: Type System
description: The `#row{...}` reader form and the kind-`[*]` row type -- writing rows, row-kinded (`^&`) parameters, the row algebra (concat/union/intersect/canon), erasure, and what rows deliberately cannot do
---

# Type-Level Rows in Turmeric

A **row** is a sequence of types that lives entirely at the type level. You
write one with the `#row{...}` reader form:

```turmeric no-check
#row{Pos Vel}                 ; a positional row of two component types
#row{id : int  name : cstr}   ; a labeled row of two named columns
```

A row is not a value and never becomes one. It exists so that a type
constructor can be parameterised by *a set of types of arbitrary size* --
"a query over `Pos` and `Vel`", "a table with an `id` and a `name` column" --
and have the compiler check that set the same way it checks any other type
argument.

Rows are the mechanism behind `tur-ecs`'s typed `Query` (see
[ecs-guide.md](ecs-guide.md)); this guide covers the underlying language
feature.

## Rows are not effect rows

Turmeric has two things called "rows", with two different kinds, two different
reader forms, and no interaction between them:

| | Type row | Effect row |
|---|---|---|
| Reader form | `#row{Pos Vel}` | `#fx{Io Unsafe}` |
| Kind | `[*]` (`KIND_TYPEROW`) | `Row` (`KIND_ROW`) |
| Elements are | types | effect labels |
| Attaches to | a `^&` type parameter | a function signature |
| Open tails / row variables | no | yes (`{Io \| e}`) |
| Guide | this one | [effects-system-guide.md](effects-system-guide.md) |

When [polymorphism-guide.md](polymorphism-guide.md) says "row polymorphism", it
means the effect kind. This guide is about the type kind.

## The kind `[*]`

Turmeric's kind system (see [hkt-guide.md](hkt-guide.md)) normally deals in `*`
and arrow kinds:

- `*` -- a plain type: `int`, `Pos`, `Option<int>`
- `* -> *` -- a unary type constructor: `Option`, `Vec`

A row adds one more sort of kind:

- `[*]` -- a *row*: a kind-level list of types

`[*]` is not `*`. A row cannot be used where a type is expected, and a type
cannot be used where a row is expected. Both directions are kind errors, which
is the whole point: `(Query #row{Pos Vel})` is checked, not merely parsed.

## Writing a row

### Positional rows

Elements are ordinary type expressions, separated by whitespace. Compound types
are fine:

```turmeric no-check
#row{}                             ; the empty row
#row{int bool}
#row{Pos Vel Hp}
#row{(Option int) (Vec cstr)}
```

Every element must name a type that actually exists. An undeclared name is an
error rather than a silently-invented placeholder -- this is what catches a
typo'd component name in an ECS query:

```
error: unknown type name 'Velocityy' in #row{...} element position
```

A row holds at most **255** elements.

### Labeled rows

A slot may instead be spelled `name : type`, giving a row of named fields:

```turmeric no-check
#row{id : int  name : cstr}
```

Two labeled rows with identical element types but different names are *distinct
types* and will not unify, so a `Tbl #row{id : int}` cannot be passed where a
`Tbl #row{name : int}` is wanted.

Labels are all-or-nothing within a single literal. Mixing bare and labeled
slots is `TUR-E0290`, and a repeated label is `TUR-E0291`:

```turmeric no-check
#row{id : int  Foo}          ; TUR-E0290 -- typed-field literal needs `name : type` slots
#row{id : int  id : cstr}    ; TUR-E0291 -- duplicate field name `id`
```

## Declaring a row-kinded parameter -- `^&`

A type parameter marked `^&` has kind `[*]` and accepts a row. It works on
`defstruct`, `deftype`, `defdata`, `defgadt`, and `defn`:

```turmeric no-check
(defstruct Query [^&reads ^&writes] (world :int))
(deftype   Frame [^&cols]           (Frame :int))
(defdata   Tagged [^&cols]          (Tagged :int))
(defgadt   QueryG [^&cols]          (QueryG : (QueryG #row{int})))
(defn      frame-id [^&r] [f : (Frame r)] : (Frame r) f)
```

The `^&` is a *declaration site* marker only. Everywhere else -- field types,
return annotations, call-site ascriptions -- you write the bare name (`r`, not
`^&r`).

At every application site the argument's kind is checked against the
parameter's:

```turmeric no-check
(defstruct Box   [a]   (v :int))   ; kind * parameter
(defstruct Query [^&r] (world :int))

(Box   #row{int bool})   ; TUR-E0012 -- parameter 1 expects kind '*', argument has kind '[*]'
(Query int)              ; TUR-E0012 -- parameter 1 expects kind '[*]'
```

Under-application is allowed, as with any type constructor: given two row
parameters, `(Query #row{Pos})` is a partially applied constructor of kind
`[*] -> *`, not an arity error.

## Where a row may appear

A row may appear **only as a type argument to a `^&` parameter**. It is not a
value, and it is not a type a value can have:

```turmeric no-check
(def r #row{int bool})
;; error: #row{...} is a type-level row and can only appear in a type
;; annotation, not as a value expression

(defn f [x : #row{int bool}] : int 5)
;; error [TUR-E0012]: `#row{...}` is a type-level row (kind [*]); a value
;; cannot have row type
```

The second check is applied at every nested position of a value type, so a row
hidden inside an arrow argument is rejected the same way.

## The row algebra

Four type-level operations combine rows. Each is written in type position and
produces a row usable anywhere a literal row is:

| Form | Meaning |
|---|---|
| `(row-concat A B ...)` | `A ++ B ++ ...` -- order-preserving, duplicates kept |
| `(row-union A B ...)` | set union -- order-preserving, deduplicated |
| `(row-intersect A B ...)` | elements present in every operand |
| `(row-canon R)` | sorted copy of `R` (unary) |

All four preserve a labeled row's field names; see
[Labels through the algebra](#labels-through-the-algebra) for the rules that
follow from that.

`row-concat` / `row-union` / `row-intersect` are variadic: zero operands give
the empty row, one operand passes through. Every operand must itself be a row;
a non-row operand is an error:

```
error: 'row-union' operand 2 must be a #row{...} (kind [*]), got a non-row type
```

A worked union -- joining two component sets, with `Pos` deduplicated:

```turmeric
(defopaque Pos :int)
(defopaque Vel :int)
(defopaque Hp  :int)

(defstruct Query [^&comps] (world :int))

(defn join [w : int] : (Query (row-union #row{Pos Vel} #row{Pos Hp}))
  (:: (make-struct Query w) (Query #row{Pos Vel Hp})))

(defn run [q : (Query #row{Pos Vel Hp})] : int
  (.world q))

(defn main [] : int
  (println (run (join 7)))
  0)
```

## Row equality -- order matters, `row-canon` opts out

Two rows are equal when they have the same elements **in the same order**.
`#row{int bool}` and `#row{bool int}` are different types.

This is a deliberate default: it keeps ordinary `type_eq` a cheap positional
walk, and it means a row can encode an ordered thing (a column list, an
argument order) as well as an unordered one.

When you want permutation-insensitivity, ask for it explicitly with
`row-canon`, which sorts the row into a canonical order. Two callers who both
canonicalise agree regardless of how they spelled the row:

```turmeric
(defstruct Frame [^&cols] (n :int))

(defn make-ab [n : int] : (Frame (row-canon #row{int bool}))
  (:: (make-struct Frame n) (Frame (row-canon #row{int bool}))))

(defn make-ba [n : int] : (Frame (row-canon #row{bool int}))
  (:: (make-struct Frame n) (Frame (row-canon #row{bool int}))))

(defn use-canon [f : (Frame (row-canon #row{int bool}))] : int (.n f))

(defn main [] : int
  (println (use-canon (make-ab 1)))
  (println (use-canon (make-ba 2)))
  0)
```

The relaxation is scoped to the annotations that opt in. `type_eq` itself is
never weakened, so a module that does not write `row-canon` keeps strict
order-sensitive equality.

## Labels through the algebra

Every operation forwards a labeled row's field names, so a label survives the
round trip and keeps doing its job -- `(row-canon #row{id : int})` stays
distinct from `(row-canon #row{name : int})`.

Two rules follow from labels being part of a slot's identity:

**Labeled rows dedup and intersect on the `(name, type)` pair.** `row-union`
collapses two slots only when they agree on *both*, and `row-intersect` keeps a
slot only when both sides label it the same:

```turmeric no-check
(row-union     #row{id : int} #row{id : int})     ;; => #row{id : int}     (one slot)
(row-intersect #row{id : int} #row{name : int})   ;; => #row{}             (labels differ)
```

`row-canon` sorts labeled rows by `(field_name, type_name)`, with the field name
leading -- otherwise `#row{a : int  b : int}` and `#row{b : int  a : int}` would
compare equal at every slot on type alone and keep their two different input
orders.

**Labels are all-or-nothing across an operation**, the same rule the literal
enforces. Mixing a labeled operand with a bare one is `TUR-E0290`:

```turmeric no-check
(row-union #row{id : int} #row{int})
;; error: 'row-union' operand 2 mixes a labeled row with a bare one; labels are
;; all-or-nothing across the whole operation (TUR-E0290)
```

The empty row is the exception, because it contributes no slots to disagree
about: `(row-union R #row{})` is the identity whether or not `R` is labeled.

A labeled fold can also reach a state no literal could spell -- `row-concat`
keeps duplicates outright, and `row-union` keeps two slots that share a name but
disagree on type. Either way the result carries a repeated field name, which is
`TUR-E0291`:

```turmeric no-check
(row-concat #row{id : int} #row{id : int})
;; error: 'row-concat' result has duplicate field name `id` (TUR-E0291)
```

## Rows are erased

Row arguments are phantom. They participate in type checking and in name
mangling, and then they are gone -- a struct with two row parameters emits a C
struct holding only its real fields:

```c
typedef struct tur_adt_Query {
    int64_t world;
} tur_adt_Query;
```

So the checking is free at runtime. There is no row descriptor, no reflection
over a row, and nothing to pay for at a call boundary. It also means a row can
never be inspected at runtime: if you need the component list *as data*, you
must build that data yourself alongside the type.

## Worked example -- an ECS system's read and write sets

The motivating use. A `Query` carries two independent rows, so a system's
signature states exactly which components it reads and which it writes:

```turmeric
(defopaque Pos :int)
(defopaque Vel :int)

(defstruct Query [^&reads ^&writes] (world :int))

(defn integrate [w : int] : (Query #row{Pos Vel} #row{Pos})
  (:: (make-struct Query w) (Query #row{Pos Vel} #row{Pos})))

(defn run-system [q : (Query #row{Pos Vel} #row{Pos})] : int
  (.world q))

(defn main [] : int
  (println (run-system (integrate 11)))
  0)
```

```sweet-exp
#lang sweet-exp

defopaque Pos :int
defopaque Vel :int

defstruct Query [^&reads ^&writes] (world :int)

defn integrate [w : int] : (Query #row{Pos Vel} #row{Pos})
  :: make-struct(Query w) (Query #row{Pos Vel} #row{Pos})

defn run-system [q : (Query #row{Pos Vel} #row{Pos})] : int
  .world(q)

defn main [] : int
  println $ run-system $ integrate 11
  0
```

Passing this `Query` to a function expecting `(Query #row{Pos} #row{Pos})` is a
type error at the boundary -- the write set is part of the type, not a comment.
`tur-ecs` builds on this with linear `WriteCap<T>` capabilities so the *body*
cannot write outside its declared set either; see
[ecs-vs-haskell-ecs.md](ecs-vs-haskell-ecs.md).

## Worked example -- labeled columns

```turmeric
(defstruct Tbl [^&cols] (rows :int))

(defn users [n : int] : (Tbl #row{id : int  name : cstr})
  (:: (make-struct Tbl n) (Tbl #row{id : int  name : cstr})))

(defn row-count [t : (Tbl #row{id : int  name : cstr})] : int
  (.rows t))

(defn main [] : int
  (println (row-count (users 3)))
  0)
```

## Being generic over a row

`^&` on a `defn` quantifies a row parameter, so a function can accept a
container over *any* row and return one over that same row:

```turmeric
(defstruct Frame [^&cols] (n :int))

(defn frame-id [^&r] [f : (Frame r)] : (Frame r) f)

(defn make-int-frame [n : int] : (Frame #row{int})
  (:: (make-struct Frame n) (Frame #row{int})))

(defn main [] : int
  (let [f (make-int-frame 11)
        g (frame-id f)]
    (println (.n g))
    0))
```

This is genuine parametricity over the row -- `frame-id` cannot look at `r`,
and every instantiation shares one erased body. What it is *not* is the ability
to compute on `r`; see the next section.

## What rows deliberately cannot do

Rows are a closed, checked, erased list of types. They are not PureScript's
`Row Type` or Koka's effect rows, and the differences bite in predictable
places.

**No computing on an abstract row.** The row operations require operands that
resolve to concrete `#row{...}` literals. A row *variable* is rejected:

```turmeric no-check
(defn add-tag [^&r] [q : (Q r)] : (Q (row-union r #row{Tag})) q)
;; error: 'row-union' operand 1 must be a #row{...} (kind [*]), got a non-row type
```

So "take any query and add a `Tag` component to it" is not expressible. You can
be generic over a row, or you can compute with rows, but not both at once.

**No open rows or row tails.** There is no `#row{Pos | r}` -- no way to say
"contains at least `Pos`". Row membership constraints (PureScript's
`Row.Cons`/`Row.Lacks`, Koka's `<exn|e>`) have no analogue. Where you want "any
world with `Pos` and `Vel`", the idiom is a typeclass constraint
(`(HasPos W) (HasVel W) => ...`), not a row.

**No type-level head/tail/length/map.** The row algebra is the whole vocabulary.
There is no way to fold over a row to derive another type, which is what would
be needed to build records-as-rows on top of this. The trade-offs of going
further are recorded in
[`docs/design/tuple-variadic-vs-hlist.md`](https://github.com/turmeric-lang/turmeric/blob/main/docs/design/tuple-variadic-vs-hlist.md).

## Error reference

| Code | Trigger |
|---|---|
| *(uncoded)* | `#row{...}` in value-expression position |
| `TUR-E0012` | a value typed by a bare row; row argument to a `*` parameter; non-row argument to a `^&` parameter |
| *(uncoded)* | unknown type name in a `#row{...}` element |
| *(uncoded)* | non-row operand to `row-concat` / `row-union` / `row-intersect` / `row-canon` |
| `TUR-E0001` | two rows that do not unify (different elements, order, or labels) |
| `TUR-E0281` | unterminated `#row{` literal |
| `TUR-E0290` | a `#row{...}` mixing bare and `name : type` slots; a row operation mixing a labeled operand with a bare one |
| `TUR-E0291` | duplicate field name in a labeled row, including one produced by `row-concat` / `row-union` |

## Prior art

The nearest analogues elsewhere, for orientation:

- **PureScript** -- `Row Type` is a real kind and `( id :: Int, name :: String )`
  is a labeled row, as here. Unlike Turmeric its rows are unordered by
  construction and support open tails and membership constraints
  (`Prim.Row.Union` / `Nub` / `Cons` / `Lacks`), so they can be computed on
  abstractly.
- **Haskell `DataKinds`** -- `'[Int, Bool] :: [Type]` is exactly the kind
  `KIND_TYPEROW` implements. Ordered, like Turmeric's default; the
  `type-level-sets` library normalizes by sorting, which is what `row-canon`
  does.
- **Ur/Web** -- kind `{Type}`, with `++` requiring a disjointness proof and
  type-level `map`/`fold` over rows. The closest thing to a "compute on an
  abstract row" story.
- **Koka**, **Links**, **Unison** -- Rémy-style rows over *effect* labels, which
  is Turmeric's `#fx{...}` kind rather than this one. Cited in
  [bibliography.md](bibliography.md).
- **Bevy** (Rust) / **apecs** (Haskell) -- the ECS use case, encoded as a tuple
  type instead of a row; access conflicts are resolved at runtime rather than
  by a kind check.

## See also

- [hkt-guide.md](hkt-guide.md) -- the kind system rows extend
- [reader-forms-guide.md](reader-forms-guide.md) -- the `#tag{...}` reader family
- [ecs-guide.md](ecs-guide.md) -- rows in `tur-ecs`'s `Query`
- [effects-system-guide.md](effects-system-guide.md) -- the *other* rows
- [polymorphism-guide.md](polymorphism-guide.md) -- where row polymorphism sits
  among Turmeric's other polymorphism mechanisms
