---
title: Datalog Tutorial Pt. 4: Indexing
category: Tutorials
description: Create a database and a query system to go with it
---

# Datalog Tutorial Pt. 4: Indexing

This session covers `examples/datalog/indexed.tur`, which adds a hash-chained EAVT
index on top of the minimal database. The index maps `(entity, attr)` pairs to
linked lists of matching datum pointers, giving O(1) average-case lookup for the
most common query pattern.

Run the example:

```sh
./build/tur run examples/datalog/indexed.tur
```

---

## Why Index?

The minimal `db-q` scans every datum in the database for every query. With `n`
datums this is O(n) per query. For a blog with a thousand posts and ten thousand
comments, fetching Alice's posts requires scanning all ten-thousand datums just
to find the handful belonging to Alice.

An **EAVT index** pre-groups datums by `(entity, attr)`. Answering "what is
Alice's name?" becomes O(1) average: hash `(entity=1, attr=":user/name")`,
follow the chain of matching entries.

---

## Index Data Structures

The index uses two C structs:

```c
/* One entry in a hash bucket chain */
struct eavt_entry {
  int64_t entity;          /* the entity ID */
  int64_t attr;            /* the attr cstr pointer (stored as int64) */
  int64_t datum;           /* the datum pointer (stored as int64) */
  struct eavt_entry *next; /* next entry in the chain, or NULL */
};

/* The index itself: a fixed-size array of bucket head pointers */
struct eavt_idx {
  struct eavt_entry **buckets; /* array of length nbuckets */
  size_t nbuckets;             /* number of buckets (64 in this tutorial) */
};
```

The Turmeric wrappers for these structs are in `indexed.tur`:

```turmeric
(defn eavt-idx-new  [] : ptr<void>  ...)
(defn eavt-idx-insert! [idx entity attr datum] : nil  ...)
(defn eavt-idx-lookup  [idx entity attr] : ptr<void>  ...)
```

```sweet-exp
defn eavt-idx-new [] :ptr<void>  ...
defn eavt-idx-insert! [idx entity attr datum] :nil  ...
defn eavt-idx-lookup [idx entity attr] :ptr<void>  ...
```

---

## Hash Function

The bucket is chosen by:

```c
size_t h = (size_t)(entity * 31 + attr) % i->nbuckets;
```

`attr` is the numeric value of the `cstr` pointer. Because C string literals
with the same content may share the same address (string interning), comparing
pointer values works correctly for attributes that come from the same literal
pool. The hash is intentionally minimal -- this is a tutorial, not a production
hash map.

---

## Insertion

`eavt-idx-insert!` prepends a new `eavt_entry` to the appropriate bucket chain:

```turmeric
(defn eavt-idx-insert! [idx : ptr<void> entity : int attr : int datum : int] : nil
  ```c
  struct eavt_idx *i = (struct eavt_idx *)idx;
  size_t h = (size_t)(entity * 31 + attr) % i->nbuckets;
  struct eavt_entry *e = malloc(sizeof(*e));
  e->entity = entity; e->attr = attr; e->datum = datum;
  e->next = i->buckets[h];
  i->buckets[h] = e;
  ```)
```

````sweet-exp
defn eavt-idx-insert! [idx :ptr<void> entity :int attr :int datum :int] :nil
  ```c
  struct eavt_idx *i = (struct eavt_idx *)idx;
  size_t h = (size_t)(entity * 31 + attr) % i->nbuckets;
  struct eavt_entry *e = malloc(sizeof(*e));
  e->entity = entity; e->attr = attr; e->datum = datum;
  e->next = i->buckets[h];
  i->buckets[h] = e;
  ```
````

Prepending is O(1) and maintains insertion order within each chain.

---

## Lookup

`eavt-idx-lookup` iterates the chain for the computed bucket, collecting all
entries that match `(entity, attr)` exactly:

```turmeric
(defn eavt-idx-lookup [idx : ptr<void> entity : int attr : int] : ptr<void>
  ```c
  ...
  size_t h = (size_t)(entity * 31 + attr) % i->nbuckets;
  struct eavt_entry *e = i->buckets[h];
  while (e != NULL) {
    if (e->entity == entity && e->attr == attr) {
      /* push e->datum into result rvec */
    }
    e = e->next;
  }
  return result;
  ```)
```

````sweet-exp
defn eavt-idx-lookup [idx :ptr<void> entity :int attr :int] :ptr<void>
  ```c
  ...
  size_t h = (size_t)(entity * 31 + attr) % i->nbuckets;
  struct eavt_entry *e = i->buckets[h];
  while (e != NULL) {
    if (e->entity == entity && e->attr == attr) {
      /* push e->datum into result rvec */
    }
    e = e->next;
  }
  return result;
  ```
````

With 64 buckets and well-distributed data, each chain has on average
`n / 64` entries. For the tutorial scale (dozens of datums) this is O(1) in
practice.

---

## Indexed Database (`idb`)

The indexed database wraps a raw database and its EAVT index together:

```
idb: int64_t[2] = [raw-db-ptr, eavt-idx-ptr]
```

Three functions compose the indexed database:

```turmeric
(defn idb-new   [] : int             ...)  ;; allocate db + index
(defn idb-db    [idb : int] : int     ...)  ;; extract raw db pointer
(defn idb-idx   [idb : int] : ptr<void> ...) ;; extract eavt idx pointer
```

```sweet-exp
defn idb-new [] :int  ...             ; allocate db + index
defn idb-db [idb :int] :int  ...     ; extract raw db pointer
defn idb-idx [idb :int] :ptr<void>  ... ; extract eavt idx pointer
```

`idb-assert!` calls `db-assert!` on the raw database and then inserts the new
datum into the index:

```turmeric
(defn idb-assert! [idb : int entity : int attr : int value : int] : int
  (let [db  (idb-db idb)]
    (let [idx (idb-idx idb)]
      (let [tx (db-assert! db entity attr value)]
        (let [n (db-count db)]
          (let [datum (db-ref db (- n 1))]
            (eavt-idx-insert! idx entity attr datum)
            tx))))))
```

```sweet-exp
defn idb-assert! [idb :int entity :int attr :int value :int] :int
  let [db idb-db(idb)]
    let [idx idb-idx(idb)]
      let [tx db-assert!(db entity attr value)]
        let [n db-count(db)]
          let [datum db-ref(db {n - 1})]
            eavt-idx-insert!(idx entity attr datum)
            tx
```

`idb-q-ea` is the indexed fast path for entity+attribute queries:

```turmeric
(defn idb-q-ea [idb : int entity : int attr : cstr] : ptr<void>
  (eavt-idx-lookup (idb-idx idb) entity (cstr->int attr)))
```

```sweet-exp
defn idb-q-ea [idb :int entity :int attr :cstr] :ptr<void>
  eavt-idx-lookup(idb-idx(idb) entity cstr->int(attr))
```

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

The `demo` in `indexed.tur` inserts three users and then does indexed lookups:

```turmeric
(idb-assert! idb 1 (cstr->int ":user/name")  (str-val "Alice"))
(idb-assert! idb 1 (cstr->int ":user/age")   (long-val 30))
(idb-assert! idb 1 (cstr->int ":user/email") (str-val "alice@example.com"))
...

;; Indexed lookup: entity=1, attr=:user/name
(let [results (idb-q-ea idb 1 ":user/name")]
  ...)

;; Entity 3 has no email -- should return empty result
(let [results (idb-q-ea idb 3 ":user/email")]
  (println (rvec-len results)))  ;; => 0
```

```sweet-exp
idb-assert!(idb 1 cstr->int(":user/name")  str-val("Alice"))
idb-assert!(idb 1 cstr->int(":user/age")   long-val(30))
idb-assert!(idb 1 cstr->int(":user/email") str-val("alice@example.com"))
...

; Indexed lookup: entity=1, attr=:user/name
let [results idb-q-ea(idb 1 ":user/name")]
  ...

; Entity 3 has no email -- should return empty result
let [results idb-q-ea(idb 3 ":user/email")]
  println(rvec-len(results))  ; => 0
```

Expected output:

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

**Hash collisions** -- the 64-bucket table degrades to O(n/64) per chain as n
grows. A production implementation would rehash when the load factor exceeds a
threshold (typically 0.75).

**No AEV or VAE index** -- the tutorial only implements the EAVT index. For
queries like "find all entities with attribute `:user/name`" you still need a
linear scan. An AEV (Attribute-Entity-Value) index would accelerate
attribute-only queries.

**No retraction awareness** -- the index stores all asserted datums, including
those later retracted. `idb-q-ea` may return datums that have been logically
retracted; callers must apply `retracted?` from `query.tur` if needed.

**Integer key storage** -- `attr` is stored as a raw pointer integer. This
means two logically identical strings stored at different addresses will hash to
different buckets. In practice, Turmeric string literals are interned by the C
compiler, so this is not an issue for constant attribute names.

---

## Exercises

1. **Load factor** -- modify the index to track the number of inserted entries.
   Print a warning when the load factor (entries / nbuckets) exceeds 1.5.

2. **AEV index** -- add a second index that maps `attr -> [datum]` to support
   `idb-q-a [idb attr]`. How do you store the AEV index alongside the EAVT
   index in the `idb` header?

3. **Retraction-aware lookup** -- write `idb-q-ea-current [idb entity attr]`
   that uses the EAVT index to get candidates and then filters out any datum
   whose entity+attr has a `:db/retract` fact.

4. **Benchmark** -- insert 1 000 datums with random entity/attr combinations
   and compare the time for 1 000 `db-q` calls vs `idb-q-ea` calls. Use the
   system `clock()` function via an inline C block.

---

## Summary

The four example files form a progression:

| File | Concepts | Lines |
|------|----------|-------|
| `minimal.tur` | Value ADT, Datum, Database, db-q, combinators | ~150 |
| `query.tur` | value-eq?, q-av, db-as-of, pull, history, retraction | ~250 |
| `indexed.tur` | EAVT hash index, idb, idb-assert!, idb-q-ea | ~220 |
| `blog.tur` | Multi-entity domain, EntityVal references, helpers | ~250 |
| `datalog.tur` | Term ADT, Binding, pattern matching, join via clause chains | ~350 |

Each file is self-contained and can be run independently with
`./build/tur run examples/datalog/<file>.tur`.
