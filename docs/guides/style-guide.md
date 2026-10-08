---
title: Turmeric Style Guide
category: Reference
description: Canonical idioms and formatting conventions for Turmeric code -- function arity, indentation, naming, and inline-C style
---

# Turmeric Style Guide

> **Status:** Living document -- updated as idioms are established.
> **Last Updated:** 2026-08-19

---

## Function Arity

### Parameter count

There is **no hard cap** on positional parameters -- a function may declare
an arbitrary number, matching the emitted C. Functions with more than ~5
positional parameters are still a code smell, and declaring more than **16**
emits the `TUR-W0041` lint nudge back to this guide. The high ceiling is an
escape hatch for generated code, macro expansions, and wide interop shims,
not a target.

### More than 5 params -- reach for `defstruct`

When a function needs many named, independent inputs, pack them into a
struct and pass a single options value:

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
defstruct CsvOpts [delim       :int   ;; field separator (e.g. 44 = ',')
   quote       :int   ;; quote char (e.g. 34 = '"')
   has-header  :int   ;; 1 = first row is header
   infer-rows  :int   ;; rows to sample for type inference
   null-str    :cstr  ;; string that represents NULL (e.g. "")
  ]
defn read-csv [src :cstr opts :CsvOpts] :int
  ...
```

#### Default values via partial application (Haskell-style idiom)

With currying, locking in a default options value is a one-liner:

```turmeric
(def default-csv-opts (CsvOpts 44 34 1 100 ""))

;; read-csv-fast is a closure with opts baked in.
;; Call it with just the filename.
(def read-csv-fast (read-csv default-csv-opts))

(read-csv-fast "data.csv")
```
```sweet-exp
def default-csv-opts CsvOpts(44 34 1 100 "")
;; read-csv-fast is a closure with opts baked in.
;; Call it with just the filename.
def read-csv-fast read-csv(default-csv-opts)
read-csv-fast("data.csv")
```

`(read-csv default-csv-opts)` returns a closure `(fn [src :cstr] :int ...)`.
No macro magic, no keyword arguments -- just currying.

### Genuine variadic interfaces -- use `& rest :type`

When a function takes an *unknown number of values of the same type*
(`println`, `format`, aggregation column lists, etc.), use a variadic
rest parameter:

```turmeric
(defn println-all [first : cstr & rest : cstr] : void
  (println first)
  ;; rest is a cons-list of :cstr; walk it with head/tail helpers
  ...)

(println-all "hello")              ;; rest = nil (0)
(println-all "a" "b" "c")         ;; rest = cons("b", cons("c", 0))
```
```sweet-exp
defn println-all [first :cstr & rest :cstr] :void
  println(first)
  ;; rest is a cons-list of :cstr; walk it with head/tail helpers
  ...
println-all("hello")
;; rest = nil (0)
println-all("a" "b" "c")
;; rest = cons("b", cons("c", 0))
```

#### Rules for `& rest`

- **One `&` per parameter list** -- the rest parameter must be last.
- **Type annotation required** -- `& rest :int`, `& rest :cstr`, etc.
- **Nil when absent** -- calling with zero rest args passes `rest = 0`.
- **No inline-C in variadic bodies** -- inline-C blocks declare fixed C
  signatures. Wrap inline-C in a fixed-arity helper and call it from
  the variadic body instead.
- **Not auto-curried** -- variadic `defn` does not produce a curried entry
  point. You can under-saturate up to the required positional params
  (returning a variadic closure), but you cannot partially apply into
  the rest slot.

#### Cons-list manipulation in `#fx{Unsafe}` code

The rest parameter is an `int64_t` holding a pointer to a linked list
of `__tur_cons_cell { int64_t head; int64_t tail; }` cells, or `0`
(nil). Inline-C helpers that walk it:

```turmeric
(defn cons-list-sum [lst : int] #{Unsafe} : int
  ```c
  typedef struct { int64_t head; int64_t tail; } __tur_cons_cell;
  int64_t acc = 0;
  __tur_cons_cell *p = (__tur_cons_cell *)(intptr_t)lst;
  while (p) { acc += p->head; p = (__tur_cons_cell *)(intptr_t)p->tail; }
  return acc;
  ```)
```
```sweet-exp
defn cons-list-sum [lst :int] #{Unsafe} :int
  ```c
  typedef struct { int64_t head; int64_t tail; } __tur_cons_cell;
  int64_t acc = 0;
  __tur_cons_cell *p = (__tur_cons_cell *)(intptr_t)lst;
  while (p) { acc += p->head; p = (__tur_cons_cell *)(intptr_t)p->tail; }
  return acc;
  ```
```

Or use a pure tail-recursive helper:

```turmeric
(defn cons-head [lst : int] #{Unsafe} : int
  ```c
  typedef struct { int64_t head; int64_t tail; } __tur_cons_cell;
  __tur_cons_cell *p = (__tur_cons_cell *)(intptr_t)lst;
  return p ? p->head : 0;
  ```)

(defn cons-tail [lst : int] #{Unsafe} : int
  ```c
  typedef struct { int64_t head; int64_t tail; } __tur_cons_cell;
  __tur_cons_cell *p = (__tur_cons_cell *)(intptr_t)lst;
  return p ? p->tail : 0;
  ```)

(defn list-sum-acc [lst : int acc : int] #{Unsafe} : int
  (if (= lst 0)
    acc
    (list-sum-acc (cons-tail lst) (+ acc (cons-head lst)))))
```
```sweet-exp
defn cons-head [lst :int] #{Unsafe} :int
  ```c
  typedef struct { int64_t head; int64_t tail; } __tur_cons_cell;
  __tur_cons_cell *p = (__tur_cons_cell *)(intptr_t)lst;
  return p ? p->head : 0;
  ```

defn cons-tail [lst :int] #{Unsafe} :int
  ```c
  typedef struct { int64_t head; int64_t tail; } __tur_cons_cell;
  __tur_cons_cell *p = (__tur_cons_cell *)(intptr_t)lst;
  return p ? p->tail : 0;
  ```

defn list-sum-acc [lst :int acc :int] #{Unsafe} :int
  if {lst = 0}
    acc
    list-sum-acc(cons-tail(lst) {acc + cons-head(lst)})
```

#### Performance note

Each variadic call with `k` rest args allocates `k` cons cells on the
heap. Variadics are for convenience calls, not hot paths. If you're
calling a variadic function in a tight loop, consider refactoring to a
fixed-arity helper.

### Quick decision guide

| Situation | Reach for |
|---|---|
| >5 named, independent params | `defstruct` options value |
| Default values + currying | `defstruct` + `(def fast (f defaults))` |
| Unknown number of same-type values | `& rest :type` variadic |
| Recursive accumulator threading context | closure-capture for context; fixed-arity for changing args |
| Genuinely >16 params | Something is wrong -- split the function |

---

## Naming Conventions

- **Predicates** end in `?`: `empty?`, `done?`, `nil?`
- **Mutating / effectful helpers** end in `!`: `assert!`, `push!`
- **Private / internal helpers** are prefixed with `__`: `__with-col-replace`
- **Module-prefixed names** use `/`: `frame/select`, `io/read-line`
- **kebab-case** everywhere for multi-word names

---

## Docstring Standard (`;;;`)

See the full standard in [CLAUDE.md](https://github.com/turmeric-lang/turmeric/blob/main/CLAUDE.md#docstring-standard-).

Quick reference:

```turmeric
;;; fn-name -- brief one-line summary.
;;;
;;; Parameters:
;;;   param -- description
;;;
;;; Returns:
;;;   description of return value
;;;
;;; Example:
;;;   (fn-name arg)  ; => expected result
;;;
;;; Since: Phase B1
(defn fn-name [param : int] : int
  ...)
```
```sweet-exp
;;; fn-name -- brief one-line summary.
;;;
;;; Parameters:
;;;   param -- description
;;;
;;; Returns:
;;;   description of return value
;;;
;;; Example:
;;;   (fn-name arg)  ; => expected result
;;;
;;; Since: Phase B1
defn fn-name [param :int] :int
  ...
```

---

## Indentation

Follow Clojure-style indentation -- see [CLAUDE.md](https://github.com/turmeric-lang/turmeric/blob/main/CLAUDE.md#indentation-style)
for the full rules.

Key points:

- Regular function calls: align args under the first arg.
- Special forms (`defn`, `fn`, `let`, `if`, `do`): 2-space body indent.
- Binding vectors: align binding names under each other.

---

## Inline-C Style

- Closing ` ``` ` and its `)` go on the **same line** (` ```) `).
- Do not use inline-C in variadic function bodies -- use a fixed-arity
  helper instead.

```turmeric
;; Good:
(defn file-size [f] : int
  ```
```sweet-exp
;; Good:
defn file-size [f] :int
```c
  return (int)ftell((FILE*)f);
  ```)

;; Bad (closing ``` on its own line breaks Markdown fences):
(defn file-size [f] :int
  ```c
  return (int)ftell((FILE*)f);
  ```
)
```
