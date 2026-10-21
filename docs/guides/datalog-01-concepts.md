---
title: Datalog Tutorial Pt. 1: Concepts
category: Tutorials
description: Create a database and a query system to go with it
---

# Datalog Tutorial Pt. 1: Concepts

## What is EAVT?

EAVT stands for **Entity-Attribute-Value-Transaction**. It is a data model where
every piece of information is stored as an atomic fact -- a four-element tuple:

```
[entity, attribute, value, transaction]
```

- **Entity** -- a unique integer that identifies an "object" (a user, a post, an
  order, anything you want to track)
- **Attribute** -- a string name describing what aspect of the entity this fact
  records (e.g. `:user/name`, `:user/email`, `:post/title`)
- **Value** -- the actual payload, typed as one of: a 64-bit integer (LongVal),
  a string (StrVal), or a reference to another entity (EntityVal)
- **Transaction** -- a monotonic integer counter stamped when the fact was
  recorded; it allows you to query the database "as of" any point in time

A single fact is also called a **datum** (plural: **datums** or **data**).

## A Running Example

Suppose you want to store two users and some facts about them. In EAVT you would
assert the following datums:

| entity | attribute    | value              | tx |
|--------|--------------|--------------------|----|
| 1      | :user/name   | "Alice"            | 1  |
| 1      | :user/email  | "alice@example.com"| 2  |
| 1      | :user/age    | 30                 | 3  |
| 2      | :user/name   | "Bob"              | 4  |
| 2      | :user/email  | "bob@example.com"  | 5  |
| 2      | :user/age    | 25                 | 6  |

Each row is one datum. The database is the collection of all datums asserted so far.

## Why Immutability?

Traditional databases store mutable rows. When you update Alice's age from 30 to
31, the old value is gone. You cannot answer "how old was Alice last week?" without
a separate audit table.

EAVT databases are **append-only**: asserting a new age for Alice adds a new datum;
the old datum remains:

| entity | attribute  | value | tx |
|--------|------------|-------|----|
| 1      | :user/age  | 30    | 3  |
| 1      | :user/age  | 31    | 99 |

This gives you:

- **Full audit history** -- every change is recorded with a transaction timestamp
- **Time travel** -- query the state of the database as-of any past transaction
- **Reproducibility** -- a pure function of the log up to tx N gives the same
  result every time
- **No locking** -- readers can always see a consistent snapshot without blocking
  writers

## The Three Basic Operations

### Assert

Adding a new fact. In Turmeric:

```turmeric
(db-assert! db 1 ":user/name" (StrVal "Alice"))
```

```sweet-exp
db-assert!(db 1 ":user/name" StrVal("Alice"))
```

This returns the transaction number (`tx`) that was assigned to the fact.
Every call to `db-assert!` takes the next transaction number.

### Query

Filtering facts with a predicate. The core function is:

```turmeric
(db-q db pred)
```

```sweet-exp
db-q(db pred)
```

where `pred` is a function `(fn [datum] :bool ...)`. It returns a vector of
all datums for which `pred` returns `true`.

Higher-level combinators build predicates:

```turmeric
(q-entity 1)          ;; datums for entity 1
(q-attr ":user/name") ;; datums with attribute :user/name
(q-ea 1 ":user/age")  ;; entity 1 AND attribute :user/age
(q-and p q)           ;; datums matching both p and q
(q-or  p q)           ;; datums matching either p or q
(q-not p)             ;; datums NOT matching p
```

```sweet-exp
q-entity(1)          ; datums for entity 1
q-attr(":user/name") ; datums with attribute :user/name
q-ea(1 ":user/age")  ; entity 1 AND attribute :user/age
q-and(p q)           ; datums matching both p and q
q-or(p q)            ; datums matching either p or q
q-not(p)             ; datums NOT matching p
```

### Retract

Logical deletion is expressed as asserting a special `:db/retract` datum. The
original datum stays in the log; queries that care about retractions must check
for the marker:

```turmeric
(db-retract! db entity ":user/email")
;; Asserts: [entity, ":db/retract", StrVal(":user/email"), next-tx]
```

```sweet-exp
db-retract!(db entity ":user/email")
; Asserts: [entity, ":db/retract", StrVal(":user/email"), next-tx]
```

This means retraction is itself a fact with a timestamp, so you can query
"was this attribute retracted before tx 50?"

## Temporal Queries (as-of)

Because every datum has a `tx`, you can reconstruct the database state at any
past transaction:

```turmeric
(db-as-of db snapshot-tx)
;; Returns a Vec of all datums with tx <= snapshot-tx
```

```sweet-exp
db-as-of(db snapshot-tx)
; Returns a Vec of all datums with tx <= snapshot-tx
```

This enables "time travel" -- you can ask "what did the database look like before
the last update?"

## Pull API

Pull collects all facts for a single entity:

```turmeric
(pull db entity-id)
;; Returns a Vec of all datums whose entity is entity-id
```

```sweet-exp
pull(db entity-id)
; Returns a Vec of all datums whose entity is entity-id
```

This is the EAVT equivalent of `SELECT * FROM ... WHERE entity = ?`.

## History

The history of an attribute for an entity shows how its value evolved over time:

```turmeric
(history db entity ":user/age")
;; Returns a Vec of all datums for (entity, ":user/age"), oldest tx first
```

```sweet-exp
history(db entity ":user/age")
; Returns a Vec of all datums for (entity, ":user/age"), oldest tx first
```

## Comparison with Other Data Models

### Relational (SQL)

| Feature         | SQL                         | EAVT                            |
|-----------------|-----------------------------|---------------------------------|
| Schema          | Required, fixed upfront     | Optional, schema-on-read        |
| Update          | Mutates row in place        | Appends new fact; old preserved |
| History         | Requires audit table        | Built-in (all facts retained)   |
| Time travel     | Requires extra columns      | Free (filter by tx)             |
| Joins           | Explicit JOIN syntax        | Predicate composition           |
| Sparse data     | NULLs everywhere            | Only store facts that exist     |
| Polymorphism    | Single type per column      | Any attribute on any entity     |

Consider a blog post in SQL:

```sql
CREATE TABLE posts (
  id      INTEGER PRIMARY KEY,
  title   VARCHAR(200),
  body    TEXT,
  author  INTEGER REFERENCES users(id)
);
```

In EAVT you just assert facts:

```
[101, :post/title,  "Hello World",  tx=10]
[101, :post/body,   "My first post", tx=10]
[101, :post/author, EntityRef(1),   tx=10]
```

No schema declaration needed. You can add `:post/draft` or `:post/views` to
some posts but not others without altering any table.

### Document Store (MongoDB / JSON)

Document stores store nested JSON objects. They are flexible but lack temporal
semantics -- updating a field overwrites it. EAVT trades the convenience of
nested structure for append-only history.

### Graph Database (Neo4j)

Graph databases model nodes and edges. EAVT can model graphs by using EntityVal
references as edge endpoints, but the primary focus is on time-ordered facts
rather than graph traversal.

## The Value ADT

Turmeric uses a tagged union to represent the three Value types:

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

(`:copy` because a fact is immutable: its value is read and compared, never
consumed. Part 2 explains it.)

Use `match` to inspect a value:

```turmeric
(match some-value
  (LongVal n)   (println n)
  (StrVal s)    (println s)
  (EntityVal e) (println e))
```

```sweet-exp
match(some-value
  (LongVal n)   println(n)
  (StrVal s)    println(s)
  (EntityVal e) println(e))
```

Build values with the constructors themselves:

- `(LongVal 42)` -- an integer
- `(StrVal "hello")` -- a string
- `(EntityVal 7)` -- a reference to entity 7

## Exercises

1. **Model a library** -- design EAVT facts for books, authors, and checkouts.
   Which facts would you assert when a book is checked out? How would you
   represent the return?

2. **Audit trail** -- write a query that returns all tx numbers in which entity 1
   was modified (hint: filter all datums by entity, collect distinct tx values).

3. **Sparse attributes** -- entity 1 has `:user/name` and `:user/email`. Entity 2
   has `:user/name` but no email. Write the facts and a query that returns only
   entities that have both attributes.

4. **Value comparison** -- extend `q-av` to support range queries: find all
   entities where `:user/age` is greater than 25. What information do you need
   to add to `value-eq?`?
