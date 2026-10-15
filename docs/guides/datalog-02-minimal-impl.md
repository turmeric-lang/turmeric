---
title: Datalog Tutorial Pt. 2: Minimal Implementation
category: Tutorials
description: Create a database and a query system to go with it
---

# Datalog Tutorial Pt. 2: Minimal Implementation

This session walks through `examples/datalog/minimal.tur` line by line. By the
end you will have a working, append-only fact database with a predicate-based
query interface in roughly 250 lines of Turmeric.

Run the complete example at any time:

```sh
./build/tur run examples/datalog/minimal.tur
```

---

## The Value ADT

The first design decision is how to represent typed values. The tutorial uses a
simple tagged union (algebraic data type) with three variants:

```turmeric
(defdata Value
  (LongVal :int)    ;; 64-bit integer
  (StrVal :int)     ;; :cstr pointer stored as int
  (EntityVal :int)) ;; entity ID reference
```

```sweet-exp
defdata Value
  (LongVal :int)    ; 64-bit integer
  (StrVal :int)     ; :cstr pointer stored as int
  (EntityVal :int)  ; entity ID reference
```

`LongVal` holds a 64-bit integer (ages, counts, timestamps).
`StrVal` holds a string -- the `:cstr` pointer is stored as a plain `:int`
so it can pass through the database machinery without triggering the borrow
checker.
`EntityVal` holds an entity ID, enabling typed cross-entity references.

**A `defdata` value moves by default.** Binding one and then using it twice is
an error:

```
error [TUR-E0201]: cannot copy unique value 'e-val' --
unique values may be used at most once
```

That is the right default for an ADT that owns something, but a `Value` here
is a tag plus a machine word -- there is nothing to own, and the later steps of
this tutorial genuinely do want to use one twice (unify a term against it,
*then* bind the term to it). Annotate the type `:copy` when you reach that
point:

```turmeric
(defdata Value :copy
  (LongVal :int)
  (StrVal :int)
  (EntityVal :int))
```

`examples/datalog/datalog.tur` carries the annotation for exactly this reason;
`minimal.tur` does not need it, because nothing in it uses a `Value` twice.
Reach for `:copy` when the checker asks for it, not before -- the error names
the binding and the second use, so it is a precise instruction rather than a
puzzle.

Constructors wrap the raw int:

```turmeric
(defn long-val [n : int] : int  (LongVal n))
(defn str-val  [s : cstr] : int (StrVal (cstr->int s)))
(defn entity-val [e : int] : int (EntityVal e))
```

```sweet-exp
defn long-val [n :int] :int
  LongVal(n)
defn str-val [s :cstr] :int
  StrVal(cstr->int(s))
defn entity-val [e :int] :int
  EntityVal(e)
```

The printer dispatches on the constructor:

```turmeric
(defn print-value [v : int] : nil
  (match v
    (LongVal n)   (println n)
    (StrVal s)    (println-cstr s)
    (EntityVal e) (println e)))
```

```sweet-exp
defn print-value [v :int] :nil
  match(v
    (LongVal n)   println(n)
    (StrVal s)    println-cstr(s)
    (EntityVal e) println(e))
```

---

## Datum Layout

A datum is four 64-bit integers stored consecutively on the heap:

```
[entity :int64, attr (cstr ptr) :int64, value (Value ptr) :int64, tx :int64]
```

The C helper allocates and fills the struct:

```turmeric
(defn datum-new [entity : int attr : int value : int tx : int] : int
  ```c
  int64_t *d = malloc(4 * sizeof(int64_t));
  d[0] = entity; d[1] = attr; d[2] = value; d[3] = tx;
  return (int64_t)(intptr_t)d;
  ```)
```

````sweet-exp
defn datum-new [entity :int attr :int value :int tx :int] :int
  ```c
  int64_t *d = malloc(4 * sizeof(int64_t));
  d[0] = entity; d[1] = attr; d[2] = value; d[3] = tx;
  return (int64_t)(intptr_t)d;
  ```
````

Accessors project individual fields:

```turmeric
(defn datum-entity [d : int] : int
  ```c return ((int64_t*)(intptr_t)d)[0]; ```)
(defn datum-attr   [d : int] : int
  ```c return ((int64_t*)(intptr_t)d)[1]; ```)
(defn datum-value  [d : int] : int
  ```c return ((int64_t*)(intptr_t)d)[2]; ```)
(defn datum-tx     [d : int] : int
  ```c return ((int64_t*)(intptr_t)d)[3]; ```)
```

```sweet-exp
defn datum-entity [d :int] :int
  ```c return ((int64_t*)(intptr_t)d)[0]; ```
defn datum-attr [d :int] :int
  ```c return ((int64_t*)(intptr_t)d)[1]; ```
defn datum-value [d :int] :int
  ```c return ((int64_t*)(intptr_t)d)[2]; ```
defn datum-tx [d :int] :int
  ```c return ((int64_t*)(intptr_t)d)[3]; ```
```

Note the `return (int64_t)(intptr_t)d` pattern -- this is the canonical way to
hand a heap pointer back to Turmeric as an opaque `:int`.

---

## The Database

The database is a two-word header on the heap:

```
[datums-vec-ptr :int64, next-tx :int64]
```

`datums-vec-ptr` points to a growable C vector (`{data, len, cap}`) of datum
pointers. `next-tx` is the transaction counter; each call to `db-assert!`
increments it before stamping the new datum.

```turmeric
(defn db-new [] : int
  ```c
  int64_t *db = malloc(2 * sizeof(int64_t));
  struct { int64_t *data; size_t len; size_t cap; } *v = malloc(sizeof(*v));
  v->data = NULL; v->len = 0; v->cap = 0;
  db[0] = (int64_t)(intptr_t)v;
  db[1] = 0;
  return (int64_t)(intptr_t)db;
  ```)
```

````sweet-exp
defn db-new [] :int
  ```c
  int64_t *db = malloc(2 * sizeof(int64_t));
  struct { int64_t *data; size_t len; size_t cap; } *v = malloc(sizeof(*v));
  v->data = NULL; v->len = 0; v->cap = 0;
  db[0] = (int64_t)(intptr_t)v;
  db[1] = 0;
  return (int64_t)(intptr_t)db;
  ```
````

`db-assert!` increments `next-tx`, allocates a new datum, appends it to the
vector, and returns the transaction number:

```turmeric no-check
(defn db-assert! [db :int entity :int attr :int value :int] :int
  ...
  p[1] += 1;
  int64_t tx = p[1];
  ...
  v->data[v->len++] = (int64_t)(intptr_t)d;
  return tx;
```

```sweet-exp
defn db-assert! [db :int entity :int attr :int value :int] :int
  ...
  p[1] += 1;
  int64_t tx = p[1];
  ...
  v->data[v->len++] = (int64_t)(intptr_t)d;
  return tx;
```

The doubling-growth vector ensures amortized O(1) append.

---

## Result Vec

Query results are collected in a `rvec` -- another growable C vector, typed
`:ptr<void>` to keep the Turmeric ownership checker out of query logic:

```turmeric
(defn rvec-new  [] :ptr<void>  ...)
(defn rvec-len  [v :ptr<void>] :int  ...)
(defn rvec-get  [v :ptr<void> i :int] :int  ...)
(defn rvec-push! [v :ptr<void> val :int]  ...)
```

```sweet-exp
defn rvec-new [] :ptr<void>  ...
defn rvec-len [v :ptr<void>] :int  ...
defn rvec-get [v :ptr<void> i :int] :int  ...
defn rvec-push! [v :ptr<void> val :int]  ...
```

`:ptr<void>` is an escape hatch that tells the compiler "I own this memory,
do not track it". Use it sparingly and only when you are certain the vector
outlives all uses of its contents.

---

## String Helpers

Attribute names are C string literals. Two helpers convert between Turmeric
`:cstr` and the opaque `:int` representation used inside the database:

```turmeric
(defn cstr->int [s :cstr] :int
  ```c return (int64_t)(intptr_t)s; ```)

(defn cstr-eq? [a :int b :int] :bool
  ```c
  return strcmp((const char*)(intptr_t)a, (const char*)(intptr_t)b) == 0;
  ```)
```

````sweet-exp
defn cstr->int [s :cstr] :int
  ```c return (int64_t)(intptr_t)s; ```

defn cstr-eq? [a :int b :int] :bool
  ```c
  return strcmp((const char*)(intptr_t)a, (const char*)(intptr_t)b) == 0;
  ```
````

`cstr->int` turns a string literal (whose address is stable for the lifetime of
the program) into an opaque integer for storage. `cstr-eq?` compares two such
stored strings using `strcmp`.

> **This local `cstr-eq?` is not the stdlib one.** `stdlib/cstr.tur` exports a
> `cstr-eq? [a : cstr b : cstr] : bool` -- byte-wise content equality on real
> `cstr` values, available via `(import cstr :refer [cstr-eq?])`. The helper
> above takes the *erased* `:int` storage handles this tutorial stores in its
> datums, so the two are not interchangeable and importing `cstr` into this
> file would collide. Reach for the stdlib one in ordinary code: `=` has no
> `cstr` overload at all, so `(= a b)` on two `cstr` values is a `TUR-E0006`
> operator-lookup error rather than a comparison.

---

## Query Combinators

`db-q` is the core query driver -- it accepts a predicate and returns a fresh
result vec of all matching datums:

```turmeric
(defn db-q [db :int pred] :ptr<void>
  (let [n (db-count db)
        result (rvec-new)
        ^mut i 0]
    (while (< i n)
      (do
        (let [d (db-ref db i)]
          (when (pred d)
            (rvec-push! result d)))
        (set! i (+ i 1))))
    result))
```

```sweet-exp
defn db-q [db :int pred] :ptr<void>
  let [n db-count(db)
       result rvec-new()
       ^mut i 0]
    while {i < n}
      do
        let [d db-ref(db i)]
          when pred(d)
            rvec-push!(result d)
        set! i {i + 1}
    result
```

Higher-level combinators return closures suitable as `pred`:

| Combinator | Description |
|------------|-------------|
| `(q-entity e)` | Datums for entity `e` |
| `(q-attr a)` | Datums with attribute `a` |
| `(q-ea e a)` | Entity `e` AND attribute `a` |
| `(q-and p q)` | Datums matching both `p` and `q` |
| `(q-or p q)` | Datums matching either `p` or `q` |
| `(q-not p)` | Datums NOT matching `p` |

Example -- find all names:

```turmeric
(let [names (db-q db (q-attr ":user/name"))
      ^mut i 0]
  (while (< i (rvec-len names))
    (do
      (print-value (datum-value (rvec-get names i)))
      (set! i (+ i 1)))))
```

```sweet-exp
let [names db-q(db q-attr(":user/name"))
     ^mut i 0]
  while {i < rvec-len(names)}
    do
      print-value(datum-value(rvec-get(names i)))
      set! i {i + 1}
```

---

## Running the Demo

The `demo` function in `minimal.tur` asserts six facts (two users, three
attributes each), then exercises the query combinators:

```turmeric
;; All user names
(db-q db (q-attr ":user/name"))

;; Everything known about entity 1 (Alice)
(db-q db (q-entity 1))

;; Users who are NOT entity 2 (i.e. not Bob), filtered by having an age attr
(db-q db (q-and (q-attr ":user/age") (q-not (q-entity 2))))
```

```sweet-exp
; All user names
db-q(db q-attr(":user/name"))

; Everything known about entity 1 (Alice)
db-q(db q-entity(1))

; Users who are NOT entity 2 (i.e. not Bob), filtered by having an age attr
db-q(db q-and(q-attr(":user/age") q-not(q-entity(2))))
```

Expected output:

```
-- All user names --
Alice
Bob
-- Alice's facts --
:user/name
Alice
:user/email
alice@example.com
:user/age
30
-- Users with age NOT 25 --
30
```

---

## Memory Note

The tutorial uses `malloc` directly with no `free`. For a real application you
would want to free each datum, the internal vector, and the database header when
the database goes out of scope. For the tutorial, the process exits immediately
after `main` returns, so the OS reclaims all memory.

---

## Exercises

1. **Boolean values** -- add a `(BoolVal :bool)` variant to `Value` and assert
   a `:user/admin` fact with `(BoolVal true)`. Update `print-value` to handle
   the new case.

2. **Count query** -- write `db-count-matching [db pred]` that returns how many
   datums satisfy `pred` without allocating an `rvec`.

3. **Distinct entities** -- write a query that returns the set of unique entity
   IDs for all datums matching a given attribute.

---

## Next

[03 -- Query API](datalog-03-query-api.md) -- Add value equality, attribute+value queries,
temporal as-of snapshots, pull, history, and retraction.
