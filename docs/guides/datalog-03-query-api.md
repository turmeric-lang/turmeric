---
title: Datalog Tutorial Pt. 3: Query API
category: Tutorials
description: Create a database and a query system to go with it
tags:
  - datalog
  - query-api
  - queries
  - temporal
---

# Datalog Tutorial Pt. 3: Query API

This session covers `examples/datalog/query.tur`, which extends the minimal
implementation with:

- Value equality (`value-eq?`)
- Attribute+value queries (`q-av`)
- Temporal as-of snapshots (`db-as-of`)
- Pull API (`pull`)
- History (`history`)
- Retraction (`db-retract!`, `retracted?`)

Run the example on any engine:

```sh
./build/tur run examples/datalog/query.tur
./build/tur --interpret examples/datalog/query.tur
```

---

## Value Equality

The minimal implementation compares attribute strings with `cstr-eq?` but has
no way to compare Values. `query.tur` adds `value-eq?`:

```turmeric
(defn value-eq? [a : Value b : Value] : bool
  (match a
    (LongVal x)   (match b (LongVal y)   (= x y)        _ false)
    (StrVal x)    (match b (StrVal y)    (cstr-eq? x y) _ false)
    (EntityVal x) (match b (EntityVal y) (= x y)        _ false)))
```

```sweet-exp
defn value-eq? [a : Value b : Value] : bool
  match(a
    (LongVal x)   match(b (LongVal y)   {x = y}       _ false)
    (StrVal x)    match(b (StrVal y)    cstr-eq?(x y) _ false)
    (EntityVal x) match(b (EntityVal y) {x = y}       _ false))
```

The outer `match` dispatches on `a`'s constructor; the inner `match` checks that
`b` has the same constructor and then compares the payload. Strings compare by
contents.

A wildcard arm `_ false` handles mismatched constructors (e.g. comparing a
`LongVal` to an `EntityVal`).

---

## Attribute+Value Query (`q-av`)

`q-av` finds all datums with a specific attribute name AND value:

```turmeric
(defn q-av [a : cstr v : Value] : (fn [Datum] bool)
  (fn [d : Datum] : bool
    (and (cstr-eq? (.attr d) a)
         (value-eq? (.value d) v))))
```

```sweet-exp
defn q-av [a : cstr v : Value] : (fn [Datum] bool)
  (fn [d : Datum] : bool
    (and (cstr-eq? (.attr d) a)
         (value-eq? (.value d) v)))
```

Usage -- find the entity with age 31:

```turmeric
(db-q db (q-av ":user/age" (LongVal 31)))
```

```sweet-exp
db-q(db q-av(":user/age" LongVal(31)))
```

This returns all datums where `attr = :user/age` AND `value = LongVal(31)`.

---

## Temporal As-Of (`db-as-of`)

`db-as-of` returns the datums recorded at or before a given transaction
number:

```turmeric
(defn db-as-of [db : Db as-of-tx : int] : (Vec Datum)
  (db-q db (fn [d : Datum] : bool (<= (.tx d) as-of-tx))))
```

```sweet-exp
defn db-as-of [db : Db as-of-tx : int] : (Vec Datum)
  db-q(db (fn [d : Datum] : bool (<= (.tx d) as-of-tx)))
```

Example -- capture Alice's initial age, then update it and compare:

```turmeric
;; tx-age-1 is the transaction number returned by this assert
(let [tx-age-1 (db-assert! db 1 ":user/age" (LongVal 30))]
  ;; Later update
  (db-assert! db 1 ":user/age" (LongVal 31))

  ;; Snapshot at tx-age-1 -- only sees age=30
  (let [snap (db-as-of db tx-age-1)]
    ...))
```

```sweet-exp
; tx-age-1 is the transaction number returned by this assert
let [tx-age-1 db-assert!(db 1 ":user/age" LongVal(30))]
  ; Later update
  db-assert!(db 1 ":user/age" LongVal(31))

  ; Snapshot at tx-age-1 -- only sees age=30
  let [snap db-as-of(db tx-age-1)]
    ...
```

The snapshot is a fresh `Vec` of copies of the matching datums. That is cheap
-- a datum is four words -- and safe, because a datum never changes after it
is asserted: the live database and the snapshot can never disagree about one.
Free the snapshot with `vec-free` when you are done with it.

---

## Pull (`pull`)

Pull collects every datum for a single entity:

```turmeric
(defn pull [db : Db e : int] : (Vec Datum)
  (db-q db (q-entity e)))
```

```sweet-exp
defn pull [db : Db e : int] : (Vec Datum)
  db-q(db q-entity(e))
```

This is equivalent to `SELECT * FROM datums WHERE entity = e`. The result
includes every attribute asserted for that entity, including historical ones.

To get only current attributes you would filter out superseded values
(e.g. by taking the datum with the highest `tx` for each attribute) -- that
filtering is left as an exercise.

---

## History (`history`)

History returns all datums for a given entity and attribute, oldest first:

```turmeric
(defn history [db : Db entity : int attr : cstr] : (Vec Datum)
  (db-q db (q-ea entity attr)))
```

```sweet-exp
defn history [db : Db entity : int attr : cstr] : (Vec Datum)
  db-q(db q-ea(entity attr))
```

No sort is needed. The log is append-only and a datum's `tx` is its position
in it, so `db-q`, which scans the log from the front, already returns the
matches in ascending `tx` order.

Usage:

```turmeric
(let [hist (history db 1 ":user/age")]
  ;; Iterate: oldest tx first
  ...)
```

```sweet-exp
let [hist history(db 1 ":user/age")]
  ; Iterate: oldest tx first
  ...
```

Each datum in `hist` carries its `tx`, so you can print `(.tx d)` next to
`(.value d)` to see the timeline.

---

## Retraction

Retraction is represented as a new fact, not a deletion:

```turmeric
(defn db-retract! [db : Db entity : int attr : cstr] : int
  (db-assert! db entity ":db/retract" (StrVal attr)))
```

```sweet-exp
defn db-retract! [db : Db entity : int attr : cstr] : int
  db-assert!(db entity ":db/retract" StrVal(attr))
```

This asserts a datum whose attribute is `:db/retract` and whose value is the
name of the attribute being retracted.

`retracted?` checks whether a retraction exists at or before a given
transaction:

```turmeric
(defn retracted? [db : Db entity : int attr : cstr as-of-tx : int] : bool
  (let [^mut i 0
        ^mut found false]
    (while (< i (db-count db))
      (let [d (db-ref db i)]
        (when (and (= (.entity d) entity)
                   (cstr-eq? (.attr d) ":db/retract")
                   (<= (.tx d) as-of-tx)
                   (value-eq? (.value d) (StrVal attr)))
          (set! found true)))
      (set! i (+ i 1)))
    found))
```

```sweet-exp
defn retracted? [db : Db entity : int attr : cstr as-of-tx : int] : bool
  let [^mut i 0
       ^mut found false]
    while {i < db-count(db)}
      let [d db-ref(db i)]
        when (and (= (.entity d) entity)
                  (cstr-eq? (.attr d) ":db/retract")
                  (<= (.tx d) as-of-tx)
                  (value-eq? (.value d) (StrVal attr)))
          set! found true
      set! i {i + 1}
    found
```

Usage -- retract Bob's email and verify:

```turmeric
(db-retract! db 2 ":user/email")
(println (retracted? db 2 ":user/email" (db-count db)))
;; => true
```

```sweet-exp
db-retract!(db 2 ":user/email")
println(retracted?(db 2 ":user/email" db-count(db)))
; => true
```

The retraction is itself a datum with a transaction number, so you can ask
"was the email retracted before tx 10?" by passing `10` as `as-of-tx`.

---

## Demo Walkthrough

The `demo` function in `query.tur` exercises all of the above:

1. Assert Alice (entity 1) and Bob (entity 2) with name, email, age.
2. Assert a new age for Alice (birthday update).
3. **Pull** all facts for Alice.
4. **As-of** snapshot: show Alice's age before the birthday.
5. **History**: print all (tx, age) pairs for Alice.
6. **Retract** Bob's email, then check `retracted?`.
7. **q-av**: find the entity with age 31.

Expected output:

```
-- pull: all Alice facts --
:user/name
Alice
:user/email
alice@example.com
:user/age
30
:user/age
31
-- as-of tx-age-1: Alice's age before birthday --
30
-- history: Alice :user/age across all tx --
3
30
7
31
-- retracted? Bob :user/email --
true
-- q-av: entity with :user/age = 31 --
1
```

---

## Combining Combinators

The query combinators compose. To find the users aged 30 whose age has not
been retracted, you could write:

```turmeric
(db-q db
  (q-and
    (q-av ":user/age" (LongVal 30))
    (q-not (fn [d : Datum] : bool (retracted? db (.entity d) ":user/age" 999)))))
```

```sweet-exp
db-q(db
  q-and(q-av(":user/age" LongVal(30))
        q-not((fn [d : Datum] : bool (retracted? db (.entity d) ":user/age" 999)))))
```

Combining `q-and`, `q-or`, `q-not`, `q-av`, and custom predicates gives you a
concise, composable query language without a parser.

---

## Exercises

1. **Current value** -- write `current-val [db entity attr]` that returns the
   value of the datum with the highest `tx` for that entity and attribute, as
   an `(Option Value)` (`None` when there is no such datum).

2. **Retraction-aware pull** -- extend `pull` to skip any attribute that has been
   retracted as-of the current maximum transaction.

3. **Multi-value query** -- write `q-in [attr values]` that matches datums
   whose attribute is `attr` and whose value is one of the elements of
   `values`, a `(Vec Value)`.

---

## Next

[04 -- Indexing](datalog-04-indexing.md) -- Replace the O(n) linear scan with a hash-based
EAVT index for O(1) average-case entity+attribute lookups.
