---
title: Datalog Tutorial Pt. 2: Minimal Implementation
category: Tutorials
description: Create a database and a query system to go with it
---

# Datalog Tutorial Pt. 2: Minimal Implementation

This session walks through `examples/datalog/minimal.tur` line by line. By the
end you will have a working, append-only fact database with a predicate-based
query interface in under 200 lines of typed Turmeric: a sum type for values, a
struct for facts and a `Vec` for the log. There is no inline C, so the same
file runs compiled, on the JIT and in the interpreter (which is what the REPL,
the notebook and the web playground use):

```sh
./build/tur run examples/datalog/minimal.tur
./build/tur jit examples/datalog/minimal.tur
./build/tur --interpret examples/datalog/minimal.tur
```

---

## The Value ADT

The first design decision is how to represent typed values. The tutorial uses a
tagged union (algebraic data type) with three variants:

```turmeric
(defdata Value :copy
  (LongVal :int)      ;; 64-bit integer
  (StrVal :cstr)      ;; string
  (EntityVal :int))   ;; reference to another entity
```

```sweet-exp
defdata Value :copy
  (LongVal :int)      ; 64-bit integer
  (StrVal :cstr)      ; string
  (EntityVal :int)    ; reference to another entity
```

`LongVal` holds a 64-bit integer (ages, counts, timestamps).
`StrVal` holds a string.
`EntityVal` holds an entity ID, enabling typed cross-entity references.

**Why `:copy`.** A `defdata` value moves by default: binding one and then using
it twice is an error.

```
error [TUR-E0201]: cannot copy unique value 'v' --
unique values may be used at most once
```

That is the right default for an ADT that owns something. A fact, though, is
immutable: a `Value` is read, compared and printed, never consumed, and the
query code below reads the same datum more than once (test it against a
predicate, *then* keep it). `:copy` says so. If you leave it off, the checker
names the binding and the second use, so the fix is never a puzzle.

Values are built with their constructors directly -- `(LongVal 30)`,
`(StrVal "Alice")`, `(EntityVal 1)` -- and taken apart with `match`. The printer
dispatches on the constructor:

```turmeric
(defn print-value [v : Value] : nil
  (match v
    (LongVal n)   (println n)
    (StrVal s)    (println s)
    (EntityVal e) (println e)))
```

```sweet-exp
defn print-value [v : Value] : nil
  match(v
    (LongVal n)   println(n)
    (StrVal s)    println(s)
    (EntityVal e) println(e))
```

`match` is checked for exhaustiveness: leave out the `EntityVal` arm and it is
a compile error, not a silent fall-through.

---

## The Datum

A datum is one fact -- an entity, an attribute, a value and the transaction
that recorded it. A struct says exactly that:

```turmeric
(defstruct Datum :copy
  [entity : int     ;; the object the fact is about
   attr   : cstr    ;; the property, e.g. ":user/name"
   value  : Value   ;; the payload
   tx     : int])   ;; the transaction that asserted it
```

```sweet-exp
defstruct Datum :copy
  [entity : int     ; the object the fact is about
   attr   : cstr    ; the property, e.g. ":user/name"
   value  : Value   ; the payload
   tx     : int]    ; the transaction that asserted it
```

`(Datum 1 ":user/name" (StrVal "Alice") 1)` builds one, and `(.entity d)`,
`(.attr d)`, `(.value d)` and `(.tx d)` read its fields. It is `:copy` for the
same reason `Value` is: facts are never mutated, only read.

---

## The Database

The database is the log of datums, oldest first. The stdlib's growable
`(Vec A)` is exactly that, and a type alias gives it a name:

```turmeric
(defalias Db (Vec Datum))

(defn db-new [] : Db
  (vec-new))

(defn db-count [db : Db] : int
  (vec-len db))

(defn db-ref [db : Db i : int] : Datum
  (vec-get db i))
```

```sweet-exp
defalias Db (Vec Datum)

defn db-new [] : Db
  vec-new()

defn db-count [db : Db] : int
  vec-len(db)

defn db-ref [db : Db i : int] : Datum
  vec-get(db i)
```

`db-assert!` appends a fact stamped with the next transaction number and
returns that number:

```turmeric
(defn db-assert! [db : Db entity : int attr : cstr value : Value] : int
  (let [tx (+ (db-count db) 1)]
    (vec-push! db (Datum entity attr value tx))
    tx))
```

```sweet-exp
defn db-assert! [db : Db entity : int attr : cstr value : Value] : int
  let [tx {db-count(db) + 1}]
    vec-push!(db Datum(entity attr value tx))
    tx
```

Every assert appends exactly one datum, so the transaction number is simply
the datum's 1-based position in the log: no separate counter to keep in step.
`vec-push!` grows the vector by doubling, so appends are amortized O(1).

---

## Query Results

A query returns its matches as a fresh `(Vec Datum)`, in log order. The caller
owns it and frees it with `vec-free` when done. The datums in it are copies,
so freeing a result never touches the database itself.

---

## Comparing Strings

Attributes are strings, and two strings are the same attribute when they hold
the same characters. `stdlib/cstr.tur` provides that comparison:

```turmeric
(load "stdlib/cstr.tur")   ;; cstr-eq?: string equality by contents

(cstr-eq? ":user/name" ":user/name")   ;; => true
```

```sweet-exp
load "stdlib/cstr.tur"   ; cstr-eq?: string equality by contents

cstr-eq?(":user/name" ":user/name")   ; => true
```

`=` has no `cstr` overload at all, so `(= a b)` on two strings is a
`TUR-E0006` operator-lookup error rather than a comparison. Do not reach for a
string's address as a cheap identity either: whether two identical string
literals share one address is up to the C compiler, and the JIT does not merge
them, so a lookup keyed that way finds its facts compiled and nothing under
`tur jit` (see the [jit-guide](jit-guide.md#two-identical-string-literals-are-two-objects)).

---

## Query Combinators

`db-q` is the core query driver -- it accepts a predicate and returns a fresh
`Vec` of all matching datums:

```turmeric
(defn db-q [db : Db pred : (fn [Datum] bool)] : (Vec Datum)
  (let [result (:: (vec-new) (Vec Datum))
        ^mut i 0]
    (while (< i (db-count db))
      (let [d (db-ref db i)]
        (when (pred d)
          (vec-push! result d)))
      (set! i (+ i 1)))
    result))
```

```sweet-exp
defn db-q [db : Db pred : (fn [Datum] bool)] : (Vec Datum)
  let [result (:: (vec-new) (Vec Datum))
       ^mut i 0]
    while {i < db-count(db)}
      let [d db-ref(db i)]
        when pred(d)
          vec-push!(result d)
      set! i {i + 1}
    result
```

`(:: (vec-new) (Vec Datum))` names the element type of the empty vector, which
nothing else in the expression would pin down.

Higher-level combinators return closures suitable as `pred`. Each one is a
`(fn [Datum] bool)`, so they compose:

```turmeric
(defn q-attr [a : cstr] : (fn [Datum] bool)
  (fn [d : Datum] : bool (cstr-eq? (.attr d) a)))

(defn q-and [p : (fn [Datum] bool) q : (fn [Datum] bool)] : (fn [Datum] bool)
  (fn [d : Datum] : bool (and (p d) (q d))))
```

```sweet-exp
defn q-attr [a : cstr] : (fn [Datum] bool)
  (fn [d : Datum] : bool (cstr-eq? (.attr d) a))

defn q-and [p : (fn [Datum] bool) q : (fn [Datum] bool)] : (fn [Datum] bool)
  (fn [d : Datum] : bool (and (p d) (q d)))
```

| Combinator | Description |
|------------|-------------|
| `(q-entity e)` | Datums for entity `e` |
| `(q-attr a)` | Datums with attribute `a` |
| `(q-ea e a)` | Entity `e` AND attribute `a` |
| `(q-and p q)` | Datums matching both `p` and `q` |
| `(q-or p q)` | Datums matching either `p` or `q` |
| `(q-not p)` | Datums NOT matching `p` |

Example -- find all names. `print-values` prints the value of each datum in a
result:

```turmeric
(let [names (db-q db (q-attr ":user/name"))]
  (print-values names)
  (vec-free names))
```

```sweet-exp
let [names db-q(db q-attr(":user/name"))]
  print-values(names)
  vec-free(names)
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

Expected output, from all three engines:

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

The database and every query result are `Vec`s, and the demo frees each one
with `vec-free` when it is done with it. The datums and values inside are
plain `:copy` data with nothing of their own to free.

One allocation is still left behind: the predicate closure a combinator
(`q-and`, `q-not`) builds over other predicates is never freed, because the
demo's `demo` is compiled through the CPS backend. That is a compiler leak with an open report
([closure-env-capturing-closures-not-released](https://github.com/turmeric-lang/turmeric/blob/main/docs/reported/closure-env-capturing-closures-not-released.md)),
not something the example should work around. A plain predicate passed straight
to `db-q` is freed when the query returns
([struct-param-closure-env-never-freed](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/struct-param-closure-env-never-freed.md),
resolved 2026-10-22).

---

## Exercises

1. **Boolean values** -- add a `(BoolVal :bool)` variant to `Value` and assert
   a `:user/admin` fact with `(BoolVal true)`. Update `print-value` to handle
   the new case; `match` will tell you every other place that needs one.

2. **Count query** -- write `db-count-matching [db pred]` that returns how many
   datums satisfy `pred` without allocating a result `Vec`.

3. **Distinct entities** -- write a query that returns the set of unique entity
   IDs for all datums matching a given attribute.

---

## Next

[03 -- Query API](datalog-03-query-api.md) -- Add value equality, attribute+value queries,
temporal as-of snapshots, pull, history, and retraction.
