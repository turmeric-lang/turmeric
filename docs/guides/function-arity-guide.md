---
title: Function Arity Style Guide
category: Language Basics
description: When to use positional parameters, defstruct options, and variadic rest parameters -- plus the rules governing each
tags:
  - functions
  - style
---

# Function Arity Style Guide

There is **no hard cap** on positional parameters -- a function may declare an
arbitrary number, matching the emitted C (which has no limit of its own).
Functions with more than ~5 positional parameters are still a code smell, and
declaring more than **16** emits the `TUR-W0041` lint nudge back to this
guide. The high ceiling is an escape hatch for generated code, macro
expansions, and wide interop shims, not a target. This guide covers when to
reach for each arity style and the rules each one carries.

---

## Quick decision guide

| Situation | Reach for |
|---|---|
| >5 named, independent params | `defstruct` options value |
| Default values + currying | `defstruct` + `(def fast (f defaults))` |
| Unknown number of same-type values | `& rest :type` variadic |
| Recursive accumulator threading context | closure-capture for context; fixed-arity for changing args |
| Genuinely >16 params | Something is wrong -- split the function |

---

## More than 5 params -- reach for `defstruct`

When a function needs many named, independent inputs, pack them into a struct
and pass a single options value:

```turmeric
(defstruct CsvOpts
  [delim       : int   ;; field separator (e.g. 44 = ',')
   quote       : int   ;; quote char (e.g. 34 = '"')
   has-header  : int   ;; 1 = first row is header
   infer-rows  : int   ;; rows to sample for type inference
   null-str    : cstr  ;; string that represents NULL (e.g. "")
  ])

(defn read-csv [src : cstr opts : CsvOpts] : int
  ...)
```
```sweet-exp
defstruct CsvOpts
  [delim       : int
   quote       : int
   has-header  : int
   infer-rows  : int
   null-str    : cstr]

defn read-csv [src : cstr  opts : CsvOpts] : int
  ...
```

**Default values via partial application** (Haskell-style idiom):

```turmeric
(def default-csv-opts (CsvOpts 44 34 1 100 ""))

;; read-csv-fast already has opts baked in; call it with just the filename.
(def read-csv-fast (read-csv default-csv-opts))

(read-csv-fast "data.csv")
```
```sweet-exp
def default-csv-opts CsvOpts(44 34 1 100 "")

def read-csv-fast read-csv(default-csv-opts)

read-csv-fast("data.csv")
```

This composes cleanly with currying: `(read-csv default-csv-opts)` returns a
closure `(fn [src :cstr] :int ...)` that already has the defaults locked in.

---

## Genuine variadic interfaces -- use `& rest :type`

When a function takes an *unknown number of values of the same type*
(e.g., `println`, `format`, aggregation column lists), use a variadic rest
parameter:

```turmeric
(defn println-all [first : cstr  & rest : cstr] : void
  (println first)
  ;; rest is a cons-list of :cstr; walk it with head/tail helpers
  ...)

(println-all "hello")          ;; rest = nil
(println-all "a" "b" "c")      ;; rest = cons("b", cons("c", 0))
```
```sweet-exp
defn println-all [first : cstr  & rest : cstr] : void
  println(first)
  ...

println-all("hello")
println-all("a" "b" "c")
```

The rest type is **fully type-checked** -- not just primitives. User-defined
types (`defopaque` newtypes, structs, ADTs, type applications) are resolved to
their full type and each rest argument is checked by identity at the call site:

```turmeric
(defopaque Route :int)
(defopaque Middleware :int)

(defn launch [& routes : Route] : int ...)

(launch (route!) (route!))     ;; OK -- all Route
(launch (route!) (make-mw))    ;; ERROR: rest arg 1 (expected Route, got Middleware)
```
```sweet-exp
defopaque Route :int
defopaque Middleware :int

defn launch [& routes : Route] : int
  ...

launch(route!() route!())     ; OK
launch(route!() make-mw())    ; ERROR
```

Because of this, do not declare the rest as `:int` and cast the opaque
handles back inside the body -- write the real type. A bare `:int` rest
rejects opaque/struct/ADT values; pass the declared type instead. For a mix
of distinct handle types, prefer two explicit `:list<T>` parameters over a
single untyped rest.

---

## Rules for `& rest`

- **One `&` per parameter list** -- the rest parameter must be last.
- **Type annotation required** -- `& rest :int`, `& rest :Route`, etc. An
  unknown type name is a hard error (it is never silently demoted to `:int`).
- **Typed by the declared element type** -- primitive rest uses a fast
  TypeKind compare; a user-defined rest type compares full type identity.
  A declared type parameter (`(defn f [A] [& xs :A] ...)`) is a polymorphic
  rest: it accepts any element type, but `A` is a single type variable, so
  all rest args in one call must unify to the same type (a mixed-type call
  is a `TUR-E0001` on the offending arg). This homogeneity is what backs
  `vec-of`/`hamt-of` element checking.
- **Nil when absent** -- calling with zero rest args passes `rest = 0`.
- **No inline-C in variadic bodies** -- inline-C blocks declare fixed C
  signatures; wrap the inline-C in a fixed-arity helper and call it from
  the variadic body.
- **Not auto-curried** -- variadic `defn` does not produce a curried entry
  point. You can still under-saturate up to the required positional params
  (which returns a variadic closure), but you cannot partially apply into
  the rest slot.

---

## Walking the rest cons-list in `#fx{Unsafe}` code

The rest parameter is a `int64_t` holding a pointer to a linked list of
`__tur_cons_cell { int64_t head; int64_t tail; }` cells, or `0` (nil).
Inline-C helpers that walk it look like:

```turmeric
(defn cons-list-sum [lst : int] #fx{Unsafe} : int
  ```c
  typedef struct { int64_t head; int64_t tail; } __tur_cons_cell;
  int64_t acc = 0;
  __tur_cons_cell *p = (__tur_cons_cell *)(intptr_t)lst;
  while (p) { acc += p->head; p = (__tur_cons_cell *)(intptr_t)p->tail; }
  return acc;
  ```)
```

Or use a pure tail-recursive helper:

```turmeric
(defn list-sum-acc [lst : int  acc : int] #fx{Unsafe} : int
  (if (= lst 0)
    acc
    (list-sum-acc (cons-tail lst) (+ acc (cons-head lst)))))
```

---

## See also

- [polymorphism-guide.md](polymorphism-guide.md) -- overview of all
  polymorphism mechanisms, including variadics in context
- `CLAUDE.md`, "Function Arity Style Guide" -- the authoritative rules
  (this doc is derived from it)
