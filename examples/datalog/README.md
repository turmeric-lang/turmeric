# EAVT Immutable Database -- Examples

This directory contains a progressive series of examples showing how to build an
immutable, append-only fact database with Entity-Attribute-Value-Transaction (EAVT)
semantics in Turmeric. The design is inspired by [Datomic](https://docs.datomic.com/).

## What is EAVT?

Every fact in the database is a four-tuple:

| Component   | Description                        | Example            |
|-------------|------------------------------------|--------------------|
| Entity      | Unique integer ID for an "object"  | `42` (a user)      |
| Attribute   | Property name (namespaced keyword) | `:user/name`       |
| Value       | Typed payload                      | `"Alice"` (string) |
| Transaction | Monotonic counter (when asserted)  | `7` (tx number)    |

The database is **append-only**: new facts are added; old facts are never deleted.
Logical deletion is modelled as a `:db/retract` fact. This enables temporal
queries ("what did we know at tx 5?") and full audit history.

## Files

### `minimal.tur`

The foundational implementation (~180 lines). Contains:

- `(defdata Value ...)` -- the Value ADT (LongVal, StrVal, EntityVal)
- `(defstruct Datum ...)` -- a single fact: entity, attribute, value, tx
- `Db`, a `(Vec Datum)`, with `db-new / db-count / db-ref / db-assert!` --
  the append-only store
- `db-q` -- filter datums with a predicate closure into a fresh `(Vec Datum)`
- `q-entity / q-attr / q-ea / q-and / q-or / q-not` -- query combinators
- A demo asserting facts about Alice and Bob, then querying them

**Run:**
```
./build/tur run examples/datalog/minimal.tur
```

### `query.tur`

Extends `minimal.tur` with richer query capabilities:

- `value-eq?` -- compare two Value ADT values for equality
- `q-av` -- query by attribute name and value (find "all entities where :user/age = 30")
- `db-as-of` -- snapshot the database at a given transaction number
- `pull` -- get all facts for a single entity
- `history` -- get all transactions for a (entity, attr) pair, oldest first
- `db-retract! / retracted?` -- retraction encoded as `:db/retract` facts

**Run:**
```
./build/tur run examples/datalog/query.tur
```

### `indexed.tur`

Adds a hash-bucketed EAVT index for O(1) average-case lookups by (entity, attr):

- `eavt-idx-new / eavt-idx-insert! / eavt-idx-lookup` -- 64-bucket hash index
  of log positions, keyed by entity and an interned attribute id
- `idb-new / idb-assert! / idb-q-ea` -- the log and its index kept in step

In a real system every `idb-q-ea` call skips the full linear scan and goes
directly to the relevant bucket. For small datasets the difference is invisible;
for millions of facts it is essential.

**Run:**
```
./build/tur run examples/datalog/indexed.tur
```

### `blog.tur`

A complete blog system showing how EAVT scales to a real domain:

- Users, posts, and comments as separate entity ranges
- Attribute name constants (`user/name`, `post/author`, etc.)
- `create-user / create-post / create-comment` batch helpers
- `get-attr` / `str-attr` / `ref-attr` -- `(Option ...)` lookups of one value
- `get-posts-by-user / get-comments-on-post` join queries
- `print-blog-post` -- pretty-prints a post with author and nested comments

**Run:**
```
./build/tur run examples/datalog/blog.tur
```

### `datalog.tur`

A Datalog-style query layer on top of the EAVT database:

- `Term` ADT -- `Var` (bind a variable), `Lit` (match a literal), `Wild` (skip)
- `Binding` -- a persistent assoc-list mapping variable names to Values
- `match-pattern` -- expands a binding set by pattern-matching against the database
- `datalog` -- runs a sequence of clauses to produce all satisfying bindings

The demo shows a two-clause join: "find all post titles by the user named Alice."

**Run:**
```
./build/tur run examples/datalog/datalog.tur
```

## Key Concepts

### Append-only storage

`db-assert!` always adds a new fact and increments the transaction counter. There
is no update or delete at the storage level. This means you can always reconstruct
the database as it was at any previous point in time using `db-as-of`.

### Schema-on-read

There is no schema declaration. Any string can be used as an attribute name.
Conventions such as namespace prefixes (`:user/`, `:post/`) are just strings --
the database has no idea they are related.

### Retraction as a fact

Retracting a fact means asserting a new `:db/retract` datum whose value is the
attribute name being retracted. The original fact remains in the log. Queries that
need to respect retractions must check for the `:db/retract` marker.

### Value ADT

```turmeric
(defdata Value :copy
  (LongVal :int)      ;; 64-bit integer
  (StrVal :cstr)      ;; string
  (EntityVal :int))   ;; reference to another entity
```

Use `(LongVal n)`, `(StrVal s)`, or `(EntityVal e)` to construct a value and
`match` to take it apart. `:copy` because facts are immutable: a value is
read and compared, never consumed.

### Every engine

The examples are plain typed Turmeric -- no inline C -- so each one runs
compiled (`tur run`), on the JIT (`tur jit`) and in the interpreter
(`tur --interpret`, which is also what the REPL, the notebook and the web
playground use), with the same output. `tests/check-examples.sh` holds them
to that (`examples/examples-interpret.txt`).

## Tutorial Docs

Step-by-step explanations of the first three files live in `docs/guides/`:

- [`datalog-01-concepts.md`](../../docs/guides/datalog-01-concepts.md) -- EAVT model, immutability, comparison with SQL
- [`datalog-02-minimal-impl.md`](../../docs/guides/datalog-02-minimal-impl.md) -- walk through `minimal.tur` line by line
- [`datalog-03-query-api.md`](../../docs/guides/datalog-03-query-api.md) -- walk through `query.tur` additions
- [`datalog-04-indexing.md`](../../docs/guides/datalog-04-indexing.md) -- why indexing matters and how `indexed.tur` works

## Interactive Tutorial

Load the interactive REPL tutorial with:

```
:tutorial eavt
```

This walks you through creating a database, asserting facts, and running queries
step by step in the Turmeric REPL.
