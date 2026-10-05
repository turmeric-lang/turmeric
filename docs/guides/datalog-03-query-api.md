---
title: Datalog Tutorial Pt. 3: Query API
category: Tutorials
description: Create a database and a query system to go with it
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

Run the example:

```sh
./build/tur run examples/datalog/query.tur
```

---

## Value Equality

The minimal implementation uses `cstr-eq?` to compare attribute strings but has
no way to compare Values. `query.tur` adds `value-eq?`:

```turmeric
(defn value-eq? [a : int b : int] : bool
  (match a
    (LongVal x)   (match b (LongVal y)   (= x y)        _ false)
    (StrVal x)    (match b (StrVal y)    (cstr-eq? x y) _ false)
    (EntityVal x) (match b (EntityVal y) (= x y)        _ false)))
```

```sweet-exp
defn value-eq? [a :int b :int] :bool
  match a
    (LongVal x)
    match b
      (LongVal y)
      = x y
      _
      false
    (StrVal x)
    match b
      (StrVal y)
      cstr-eq?(x y)
      _
      false
    (EntityVal x)
    match b
      (EntityVal y)
      = x y
      _
      false
```

The outer `match` dispatches on `a`'s constructor; the inner `match` checks that
`b` has the same constructor and then compares the payload.

A wildcard arm `_ false` handles mismatched constructors (e.g. comparing a
`LongVal` to an `EntityVal`).

---

## Attribute+Value Query (`q-av`)

`q-av` finds all datums with a specific attribute name AND value:

```turmeric
(defn q-av [a : cstr v : int]
  (fn [d]
    (and (cstr-eq? (datum-attr d) (cstr->int a))
         (value-eq? (datum-value d) v))))
```

```sweet-exp
defn q-av [a :cstr v :int]
  (fn [d]
    {cstr-eq?(datum-attr(d) cstr->int(a)) and value-eq?(datum-value(d) v)})
```

Usage -- find the entity with age 31:

```turmeric
(db-q db (q-av ":user/age" (long-val 31)))
```

```sweet-exp
db-q(db q-av(":user/age" long-val(31)))
```

This returns all datums where `attr = :user/age` AND `value = LongVal(31)`.

---

## Temporal As-Of (`db-as-of`)

`db-as-of` returns a filtered result vec containing only datums recorded at or
before a given transaction number:

```turmeric
(defn db-as-of [db : int as-of-tx : int] : ptr<void>
  (db-q db (fn [d] (<= (datum-tx d) as-of-tx))))
```

```sweet-exp
defn db-as-of [db :int as-of-tx :int] :ptr<void>
  db-q(db (fn [d] {datum-tx(d) <= as-of-tx}))
```

Example -- capture Alice's initial age, then update it and compare:

```turmeric
;; tx-age-1 is the transaction number returned by this assert
(let [tx-age-1 (db-assert! db 1 (cstr->int ":user/age") (long-val 30))]
  ;; Later update
  (db-assert! db 1 (cstr->int ":user/age") (long-val 31))

  ;; Snapshot at tx-age-1 -- only sees age=30
  (let [snap (db-as-of db tx-age-1)]
    ...))
```

```sweet-exp
; tx-age-1 is the transaction number returned by this assert
let [tx-age-1 db-assert!(db 1 cstr->int(":user/age") long-val(30))]
  ; Later update
  db-assert!(db 1 cstr->int(":user/age") long-val(31))

  ; Snapshot at tx-age-1 -- only sees age=30
  let [snap db-as-of(db tx-age-1)]
    ...
```

The snapshot is a fresh result vec of datum pointers. The datums themselves are
not copied -- both the live database and the snapshot refer to the same
heap-allocated datum structs, which are immutable.

---

## Pull (`pull`)

Pull collects every datum for a single entity:

```turmeric
(defn pull [db : int e : int] : ptr<void>
  (db-q db (fn [d] (= (datum-entity d) e))))
```

```sweet-exp
defn pull [db :int e :int] :ptr<void>
  db-q(db (fn [d] {datum-entity(d) = e}))
```

This is equivalent to `SELECT * FROM datums WHERE entity = e`. The result
includes every attribute asserted for that entity, including historical ones.

To get only current attributes you would filter out superseded values
(e.g. by taking the datum with the highest `tx` for each attribute) -- that
filtering is left as an exercise.

---

## History (`history`)

History returns all datums for a given entity and attribute, sorted by
transaction number ascending:

```turmeric
(defn history [db : int entity : int attr : cstr] : ptr<void>
  (let [raw (db-q db (fn [d]
                       (and (= (datum-entity d) entity)
                            (cstr-eq? (datum-attr d) (cstr->int attr)))))]
    ;; Insertion sort by tx
    (let [n (rvec-len raw)]
      ...
    raw)))
```

```sweet-exp
defn history [db :int entity :int attr :cstr] :ptr<void>
  let [raw db-q(db (fn [d]
                     {{datum-entity(d) = entity} and cstr-eq?(datum-attr(d) cstr->int(attr))}))]
    ; Insertion sort by tx
    let [n rvec-len(raw)]
      ...
      raw
```

The sort is insertion sort -- O(n^2) but adequate for the tutorial scale
(single-digit attribute histories per entity). Production implementations would
use an index.

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

Each datum in `hist` carries its `tx`, so you can print `(datum-tx d)` next to
`(datum-value d)` to see the timeline.

---

## Retraction

Retraction is represented as a new fact, not a deletion:

```turmeric
(defn db-retract! [db : int entity : int attr : cstr] : int
  (db-assert! db entity (cstr->int ":db/retract") (StrVal (cstr->int attr))))
```

```sweet-exp
defn db-retract! [db :int entity :int attr :cstr] :int
  db-assert!(db entity cstr->int(":db/retract") StrVal(cstr->int(attr)))
```

This asserts a datum whose attribute is `:db/retract` and whose value is the
name of the attribute being retracted.

`retracted?` checks whether a retraction exists at or before a given
transaction:

```turmeric
(defn retracted? [db : int entity : int attr : cstr as-of-tx : int] : bool
  (let [n (db-count db)
        ^mut i 0
        ^mut found false]
    (while (< i n)
      (do
        (let [d (db-ref db i)]
          (when (and (and (= (datum-entity d) entity)
                     (cstr-eq? (datum-attr d) (cstr->int ":db/retract")))
                     (<= (datum-tx d) as-of-tx))
            (match (datum-value d)
              (StrVal s) (when (cstr-eq? s (cstr->int attr)) (set! found true))
              _ nil)))
        (set! i (+ i 1))))
    found))
```

```sweet-exp
defn retracted? [db :int entity :int attr :cstr as-of-tx :int] :bool
  let [n db-count(db)
       ^mut i 0
       ^mut found false]
    while {i < n}
      do
        let [d db-ref(db i)]
          when {{{datum-entity(d) = entity} and cstr-eq?(datum-attr(d) cstr->int(":db/retract"))} and {datum-tx(d) <= as-of-tx}}
            match datum-value(d)
              (StrVal s)
              when cstr-eq?(s cstr->int(attr))
                set! found true
              _
              nil
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

Expected output (abbreviated):

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
8
31
-- retracted? Bob :user/email --
true
-- q-av: entity with :user/age = 31 --
1
```

---

## Combining Combinators

The query combinators compose. To find all non-retracted ages above 25 you
could write:

```turmeric
(db-q db
  (q-and
    (q-av ":user/age" (long-val 30))
    (q-not (fn [d] (retracted? db (datum-entity d) ":user/age" 999)))))
```

```sweet-exp
db-q(db
  q-and(
    q-av(":user/age" long-val(30))
    q-not((fn [d] retracted?(db datum-entity(d) ":user/age" 999)))))
```

Combining `q-and`, `q-or`, `q-not`, `q-av`, and custom predicates gives you a
concise, composable query language without a parser.

---

## Exercises

1. **Current value** -- write `current-val [db entity attr]` that returns the
   value of the datum with the highest `tx` for that entity and attribute.

2. **Retraction-aware pull** -- extend `pull` to skip any attribute that has been
   retracted as-of the current maximum transaction.

3. **Multi-value query** -- write `q-in [attr values]` that matches datums
   whose attribute is `attr` and whose value is one of the elements of `values`
   (a list of Value pointers).

---

## Next

[04 -- Indexing](datalog-04-indexing.md) -- Replace the O(n) linear scan with a hash-based
EAVT index for O(1) average-case entity+attribute lookups.
