---
title: Datalog Tutorial Pt. 4: Indexing
category: Tutorials
description: Create a database and a query system to go with it
---

# Datalog Tutorial Pt. 4: Indexing

This session covers `examples/datalog/indexed.tur`, which adds a hash-bucketed
EAVT index on top of the minimal database. The index maps `(entity, attr)`
pairs to the log positions of the matching datums, giving O(1) average-case
lookup for the most common query pattern.

Run the example on any engine:

```sh
./build/tur run examples/datalog/indexed.tur
./build/tur --interpret examples/datalog/indexed.tur
```

---

## Why Index?

The minimal `db-q` scans every datum in the database for every query. With `n`
datums this is O(n) per query. For a blog with a thousand posts and ten thousand
comments, fetching Alice's posts requires scanning all ten-thousand datums just
to find the handful belonging to Alice.

An **EAVT index** pre-groups datums by `(entity, attr)`. Answering "what is
Alice's name?" becomes O(1) average: hash `(entity=1, attr=":user/name")`,
scan the one bucket it lands in.

---

## Index Data Structures

An index entry records one datum's key and where the datum sits in the log.
The index itself is a table of attribute names and a fixed table of 64
buckets, each a `Vec` of entries:

```turmeric
(defstruct IdxEntry :copy
  [entity : int    ;; the datum's entity
   attr   : int    ;; the datum's attr-id
   pos    : int])  ;; the datum's index in the log

(defstruct EavtIdx :copy
  [names   : (Vec cstr)              ;; attr-id -> attribute name
   buckets : (Vec (Vec IdxEntry))])  ;; 64 buckets of entries
```

```sweet-exp
defstruct IdxEntry :copy
  [entity : int    ; the datum's entity
   attr   : int    ; the datum's attr-id
   pos    : int]   ; the datum's index in the log

defstruct EavtIdx :copy
  [names   : (Vec cstr)              ; attr-id -> attribute name
   buckets : (Vec (Vec IdxEntry))]   ; 64 buckets of entries
```

Storing the log position rather than a copy of the datum keeps an entry three
words, whatever a datum holds. `EavtIdx` is `:copy` because it is a pair of
handles: copying it copies the handles, not the tables, so every copy sees the
same index.

---

## Attribute Ids

The bucket key uses an **attr-id**, a small integer the index hands out per
distinct attribute name, instead of the name itself. Names are compared by
contents once, when they are interned; the buckets compare integers from then
on:

```turmeric
(defn attr-find [idx : EavtIdx attr : cstr] : (Option int)
  (let [^mut i     0
        ^mut found (:: (none) (Option int))]
    (while (< i (vec-len (.names idx)))
      (when (cstr-eq? (vec-get (.names idx) i) attr)
        (set! found (some i)))
      (set! i (+ i 1)))
    found))

(defn attr-intern [idx : EavtIdx attr : cstr] : int
  (match (attr-find idx attr)
    (Some id) id
    (None)    (do (vec-push! (.names idx) attr)
                  (- (vec-len (.names idx)) 1))))
```

```sweet-exp
defn attr-find [idx : EavtIdx attr : cstr] : (Option int)
  let [^mut i     0
       ^mut found (:: (none) (Option int))]
    while {i < vec-len(.names(idx))}
      when cstr-eq?(vec-get(.names(idx) i) attr)
        set! found some(i)
      set! i {i + 1}
    found

defn attr-intern [idx : EavtIdx attr : cstr] : int
  match(attr-find(idx attr)
    (Some id) id
    (None)    (do (vec-push! (.names idx) attr)
                  (- (vec-len (.names idx)) 1)))
```

Inserting interns the name, so a new attribute gets the next id. A lookup only
*finds* it: asking about an attribute the index has never seen answers `None`
and an empty result, without adding the name.

### Why an id, not the string's address

It is tempting to key on the `cstr`'s address instead, and compare those
integers. Do not: whether two identical string literals share one address is
up to the C compiler. gcc merges them within a file, so the compiled program
happens to work; `tur jit` hands the same C to c2mir, which gives each literal
its own storage, and every lookup comes back empty. Turmeric gives a `cstr` no
identity -- `=` does not even accept one -- so compare names by their contents,
once, as `attr-find` does.

---

## Hash Function

The bucket is chosen by:

```turmeric
(defn eavt-bucket [idx : EavtIdx entity : int attr : int] : (Vec IdxEntry)
  (vec-get (.buckets idx) (mod (+ (* entity 31) attr) eavt-nbuckets)))
```

```sweet-exp
defn eavt-bucket [idx : EavtIdx entity : int attr : int] : (Vec IdxEntry)
  vec-get(.buckets(idx) mod({{entity * 31} + attr} eavt-nbuckets))
```

where `eavt-nbuckets` is 64. The hash is intentionally minimal -- this is a
tutorial, not a production hash map.

---

## Insertion

`eavt-idx-insert!` interns the attribute and appends an entry to its bucket:

```turmeric
(defn eavt-idx-insert! [idx : EavtIdx entity : int attr : cstr pos : int] : nil
  (let [id (attr-intern idx attr)]
    (vec-push! (eavt-bucket idx entity id) (IdxEntry entity id pos))
    nil))
```

```sweet-exp
defn eavt-idx-insert! [idx : EavtIdx entity : int attr : cstr pos : int] : nil
  let [id attr-intern(idx attr)]
    vec-push!(eavt-bucket(idx entity id) IdxEntry(entity id pos))
    nil
```

Appending is amortized O(1) and keeps each bucket in insertion order, which is
log order.

---

## Lookup

`eavt-idx-lookup` scans the one bucket the key hashes to, collecting the log
positions of the entries that match `(entity, attr)` exactly:

```turmeric
(defn eavt-idx-lookup [idx : EavtIdx entity : int attr : cstr] : (Vec int)
  (let [result (:: (vec-new) (Vec int))]
    (match (attr-find idx attr)
      (Some id)
        (let [bucket (eavt-bucket idx entity id)
              ^mut i 0]
          (while (< i (vec-len bucket))
            (let [e (vec-get bucket i)]
              (when (and (= (.entity e) entity) (= (.attr e) id))
                (vec-push! result (.pos e))))
            (set! i (+ i 1))))
      (None) nil)
    result))
```

```sweet-exp
defn eavt-idx-lookup [idx : EavtIdx entity : int attr : cstr] : (Vec int)
  let [result (:: (vec-new) (Vec int))]
    (match (attr-find idx attr)
      (Some id)
        (let [bucket (eavt-bucket idx entity id)
              ^mut i 0]
          (while (< i (vec-len bucket))
            (let [e (vec-get bucket i)]
              (when (and (= (.entity e) entity) (= (.attr e) id))
                (vec-push! result (.pos e))))
            (set! i (+ i 1))))
      (None) nil)
    result
```

The entity and attribute are both checked because different keys can land in
the same bucket. With 64 buckets and well-distributed data, each bucket holds
on average `n / 64` entries. For the tutorial scale (dozens of datums) this is
O(1) in practice.

---

## Indexed Database (`idb`)

The indexed database is the log and its index, kept in step:

```turmeric
(defstruct Idb :copy
  [db  : Db
   idx : EavtIdx])

(defn idb-new [] : Idb
  (Idb (db-new) (eavt-idx-new)))
```

```sweet-exp
defstruct Idb :copy
  [db  : Db
   idx : EavtIdx]

defn idb-new [] : Idb
  Idb(db-new() eavt-idx-new())
```

`idb-assert!` appends to the log and indexes the new datum's position:

```turmeric
(defn idb-assert! [idb : Idb entity : int attr : cstr value : Value] : int
  (let [tx (db-assert! (.db idb) entity attr value)]
    ;; The datum is the last entry in the log
    (eavt-idx-insert! (.idx idb) entity attr (- (db-count (.db idb)) 1))
    tx))
```

```sweet-exp
defn idb-assert! [idb : Idb entity : int attr : cstr value : Value] : int
  let [tx db-assert!(.db(idb) entity attr value)]
    ; The datum is the last entry in the log
    eavt-idx-insert!(.idx(idb) entity attr {db-count(.db(idb)) - 1})
    tx
```

`idb-q-ea` is the indexed fast path for entity+attribute queries. It turns
the positions the index returns into a fresh `Vec` of the datums:

```turmeric
(defn idb-q-ea [idb : Idb entity : int attr : cstr] : (Vec Datum)
  (let [positions (eavt-idx-lookup (.idx idb) entity attr)
        result    (:: (vec-new) (Vec Datum))
        ^mut i    0]
    (while (< i (vec-len positions))
      (vec-push! result (db-ref (.db idb) (vec-get positions i)))
      (set! i (+ i 1)))
    (vec-free positions)
    result))
```

```sweet-exp
defn idb-q-ea [idb : Idb entity : int attr : cstr] : (Vec Datum)
  let [positions eavt-idx-lookup(.idx(idb) entity attr)
       result    (:: (vec-new) (Vec Datum))
       ^mut i    0]
    while {i < vec-len(positions)}
      vec-push!(result db-ref(.db(idb) vec-get(positions i)))
      set! i {i + 1}
    vec-free(positions)
    result
```

`idb-free` frees the buckets, both tables and the log, in that order.

---

## Performance Comparison

| Operation | Minimal (db-q) | Indexed (idb-q-ea) |
|-----------|---------------|-------------------|
| Find Alice's name | O(n) scan | O(1) avg hash lookup |
| Find all users | O(n) scan | O(n/64) per bucket |
| Insert a fact | O(1) append | O(1) append + O(1) hash insert |
| Memory | datums only | datums + index entries |

For the tutorial dataset (8 datums), the difference is imperceptible. For a
database with one million datums, `idb-q-ea` returns results in microseconds
while `db-q` would take milliseconds.

---

## Demo Walkthrough

The `demo` in `indexed.tur` inserts three users and then does indexed lookups.
`print-values!` prints each datum's value and frees the result:

```turmeric
(idb-assert! idb 1 ":user/name"  (StrVal "Alice"))
(idb-assert! idb 1 ":user/age"   (LongVal 30))
(idb-assert! idb 1 ":user/email" (StrVal "alice@example.com"))
...

;; Indexed lookup: entity=1, attr=:user/name
(print-values! (idb-q-ea idb 1 ":user/name"))

;; Entity 3 has no email -- should return empty result
(let [results (idb-q-ea idb 3 ":user/email")]
  (println (vec-len results))  ;; => 0
  (vec-free results))
```

```sweet-exp
idb-assert!(idb 1 ":user/name"  StrVal("Alice"))
idb-assert!(idb 1 ":user/age"   LongVal(30))
idb-assert!(idb 1 ":user/email" StrVal("alice@example.com"))
...

; Indexed lookup: entity=1, attr=:user/name
print-values!(idb-q-ea(idb 1 ":user/name"))

; Entity 3 has no email -- should return empty result
let [results idb-q-ea(idb 3 ":user/email")]
  println(vec-len(results))  ; => 0
  vec-free(results)
```

Expected output, from all three engines:

```
-- idb-q-ea entity=1 :user/name --
Alice
-- idb-q-ea entity=2 :user/age --
25
-- idb-q-ea entity=3 :user/email (should be empty) --
0
```

---

## Limitations and Extensions

**Hash collisions** -- the 64-bucket table degrades to O(n/64) per bucket as n
grows. A production implementation would rehash when the load factor exceeds a
threshold (typically 0.75).

**No AEV or VAE index** -- the tutorial only implements the EAVT index. For
queries like "find all entities with attribute `:user/name`" you still need a
linear scan. An AEV (Attribute-Entity-Value) index would accelerate
attribute-only queries.

**No retraction awareness** -- the index stores all asserted datums, including
those later retracted. `idb-q-ea` may return datums that have been logically
retracted; callers must apply `retracted?` from `query.tur` if needed.

**A linear name table** -- `attr-find` scans the names, which is fine for a
schema's few dozen attributes and would want a hash table in a real database.

---

## Exercises

1. **Load factor** -- modify the index to track the number of inserted entries.
   Print a warning when the load factor (entries / buckets) exceeds 1.5.

2. **AEV index** -- add a second index that maps `attr -> [datum]` to support
   `idb-q-a [idb attr]`. Where does it go in the `Idb` struct?

3. **Retraction-aware lookup** -- write `idb-q-ea-current [idb entity attr]`
   that uses the EAVT index to get candidates and then filters out any datum
   whose entity+attr has a `:db/retract` fact.

4. **Benchmark** -- insert 1 000 datums with random entity/attr combinations
   and compare the time for 1 000 `db-q` calls vs `idb-q-ea` calls, timing
   each loop with `monotonic-ns` from `stdlib/time.tur`.

---

## Summary

The five example files form a progression:

| File | Concepts | Lines |
|------|----------|-------|
| `minimal.tur` | Value ADT, Datum, Database, db-q, combinators | ~180 |
| `query.tur` | value-eq?, q-av, db-as-of, pull, history, retraction | ~240 |
| `indexed.tur` | EAVT hash index, attr-ids, idb, idb-assert!, idb-q-ea | ~240 |
| `blog.tur` | Multi-entity domain, EntityVal references, Option lookups | ~240 |
| `datalog.tur` | Term ADT, Binding, pattern matching, join via clause chains | ~270 |

Each file is self-contained and runs on all three engines:
`./build/tur run`, `./build/tur jit` or `./build/tur --interpret` with
`examples/datalog/<file>.tur`.
