# Immutable EAVT Database Tutorial — Design & Example Plan

> **Status:** Draft Plan
> **Last Updated:** 2026-05-10
> **Type:** Tutorial + Example Code
> **Inspiration:** [Datomic EAVT Model](https://docs.datomic.com/reference/entities.html)

---

## Executive Summary

This document outlines a **tutorial and example code** for building a simple **immutable fact database** with **EAVT semantics** in Turmeric. The EAVT model represents facts as tuples of Entity-Attribute-Value-Transaction, enabling powerful query patterns while maintaining immutability.

**Key properties:**
- **Immutable:** Once recorded, facts never change
- **Append-only:** New transactions add facts; old facts remain
- **Temporal:** Every fact has a transaction ID, enabling "as-of" queries
- **Schema-on-read:** No fixed schema; attributes are first-class

**Tutorial structure:**
1. Conceptual introduction to EAVT
2. Minimal working implementation
3. Query API with examples
4. Indexing for performance
5. Integration with Turmeric's type system and persistence

**Target audience:** Intermediate Turmeric developers familiar with Phase 5 (`ref<T>`, `rc<T>`), Phase 15 (typeclasses), and basic functional patterns.

---

## 1. EAVT Model Overview

### 1.1 The Four Components

| Component | Description | Example |
|-----------|-------------|---------|
| **Entity** | Unique identifier for an "object" | `42` (a user) |
| **Attribute** | Property name | `:user/name`, `:user/email` |
| **Value** | Property value | `"Alice"`, `"alice@example.com"` |
| **Transaction** | Temporal marker | `100` (tx id where fact was asserted) |

A **fact** (or **datum**) is a tuple: `[entity, attribute, value, tx]`

### 1.2 Comparison with Datomic

| Feature | Datomic | This Tutorial |
|---------|---------|---------------|
| EAVT model | ✅ | ✅ |
| Immutable | ✅ | ✅ |
| Temporal queries | ✅ | ✅ (basic as-of) |
| Retraction | ✅ | ✅ (via retractions as facts) |
| Schema | Required | Optional (schema-on-read) |
| Indexing | Multiple indices | Hash index (v1) |
| Persistence | DynamoDB, etc. | File-based (v1) |
| Query language | Datalog | Simple combinators (v1) |

### 1.3 Why EAVT?

**Traditional relational** — fixed schema, mutable rows:

| user_id | name  | email         |
|---------|-------|---------------|
| 42      | Alice | alice@ex.com  |
| 43      | Bob   | bob@ex.com    |

**EAVT (immutable facts)** — schema-on-read, append-only, temporal:

| entity | attribute    | value          | tx  |
|--------|--------------|----------------|-----|
| 42     | :user/name   | "Alice"        | 100 |
| 42     | :user/email  | "alice@ex.com" | 100 |
| 43     | :user/name   | "Bob"          | 101 |
| 43     | :user/email  | "bob@ex.com"   | 101 |

---

## 2. Tutorial Outline

### Session 1: Core Concepts (30 min reading + exercises)

**File:** `docs/tutorials/datalog/01-concepts.md`

- What is EAVT?
- Why immutability matters
- Basic operations: assert, retract, query
- Temporal nature: as-of queries
- Comparison with RDBMS, document stores, graph databases

**Exercises:**
1. Model a blog post with comments in EAVT
2. Represent a user profile with nested data
3. Design facts for a shopping cart

### Session 2: Minimal Implementation (45 min)

**File:** `docs/tutorials/datalog/02-minimal-impl.md`

- Define the Datum type
- Basic database structure
- `assert!` — add facts
- `q` — simple queries
- `db-as-of` — temporal queries

**Code:** `examples/datalog/minimal.tur`

### Session 3: Query API (60 min)

**File:** `docs/tutorials/datalog/03-query-api.md`

- Query combinators: `where`, `and`, `or`, `not`
- Value constraints: `=`, `>`, `<`, `in`
- Pull API: get entity with all attributes
- Aggregations: count, distinct

**Code:** `examples/datalog/query.tur`

### Session 4: Indexing (45 min)

**File:** `docs/tutorials/datalog/04-indexing.md`

- EAVT index (by default)
- AEVT index (for attribute-based queries)
- VAET index (for value-based queries)
- Performance tradeoffs

**Code:** `examples/datalog/indexed.tur`

### Session 5: Schema (Optional, 30 min)

**File:** `docs/tutorials/datalog/05-schema.md`

- Defining schemas for validation
- Cardinality (one/many)
- Type constraints
- Reference attributes

**Code:** `examples/datalog/schema.tur`

### Session 6: Persistence (45 min)

**File:** `docs/tutorials/datalog/06-persistence.md`

- Serializing the database
- Loading from disk
- Transaction log
- Snapshots

**Code:** `examples/datalog/persistent.tur`

---

## 3. Example Code Structure

```
examples/datalog/
├── minimal.tur          ;; ~50 lines: core types + basic ops
├── query.tur            ;; ~100 lines: full query API
├── indexed.tur          ;; ~80 lines: indexed database
├── schema.tur           ;; ~60 lines: schema validation
├── persistent.tur       ;; ~70 lines: file I/O
├── test_minimal.tur     ;; Tests for minimal.tur
├── test_query.tur       ;; Tests for query.tur
└── README.md            ;; Overview + how to run
```

---

## 4. Core Data Types

### 4.1 The Datum

```turmeric
;; examples/datalog/minimal.tur

;; Entity: a unique identifier (long for now, could be UUID)
(defalias Entity int64)

;; Attribute: a namespaced keyword-like identifier
;; Using cstr for simplicity; could be a dedicated type
defalias Attribute cstr)

;; Value: can be any Turmeric value that implements our Value traits
;; We use a tagged union to support various types
(defalias Value
  (variant
    LONG    int64
    BOOL    bool
    STR     cstr
    KEYWORD cstr
    ENTITY  Entity
    INST    int64))  ;; date/time as millis since epoch

;; A single fact/datum: Entity-Attribute-Value-Transaction
(defstruct Datum
  [entity  : Entity
   attr    : Attribute
   value   : Value
   tx      : int64])
```

### 4.2 The Database

```turmeric
;; A database is a collection of datums, organized for efficient querying
;; Version 1: simple vector of datums
(defstruct Database
  [datums : (Vector Datum)
   next_tx : int64])  ;; monotonically increasing transaction ID

;; Version 2: with indices for faster queries
(defstruct IndexedDatabase
  [datums : (Vector Datum)
   next_tx : int64
   eavt_index : (Map (Pair Entity Attribute) (Vector Datum))  ;; Entity -> Attr -> [Datum]
   aev_index : (Map Attribute (Map Entity (Vector Datum)))    ;; Attr -> Entity -> [Datum]
   vae_index : (Map Value (Map Attribute (Map Entity (Vector Datum))))])  ;; Value -> Attr -> Entity -> [Datum]
```

---

## 5. API Design

### 5.1 Construction

```turmeric
;; Create an empty database
(defn empty_db [] : Database
  (Database::new (Vector::new) 0))

;; Create an empty indexed database
(defn empty_indexed_db [] : IndexedDatabase
  (IndexedDatabase::new
    (Vector::new)
    0
    (Map::new)
    (Map::new)
    (Map::new)))
```

### 5.2 Asserting Facts

```turmeric
;; Add facts to the database
;; Returns a new database (immutable!)
(defn assert! [db : Database, facts : (list Datum)] : Database
  (let [new_tx (inc db.next_tx)]
    (let [new_datums (Vector::concat db.datums facts)]
      (Database::new new_datums new_tx))))

;; For indexed database: also update indices
(defn assert_indexed! [db : IndexedDatabase, facts : (list Datum)] : IndexedDatabase
  (let [new_tx (inc db.next_tx)]
    (letrec [add_to_eavt [e : Entity, a : Attribute, d : Datum]
             (let [key (Pair::new e a)]
               (Map::update db.eavt_index key (fn [v] (Vector::append v d))))
             
             add_to_aev [a : Attribute, e : Entity, d : Datum]
             (Map::update (Map::get_or_default db.aev_index a (Map::new)) e
                          (fn [v] (Vector::append v d)))
             
             add_to_vae [v : Value, a : Attribute, e : Entity, d : Datum]
             (let [ae_map (Map::get_or_default db.vae_index v (Map::new))]
               (let [new_ae (Map::update ae_map a (fn [v2] (Map::update v2 e (fn [v3] (Vector::append v3 d)))))]
                 (Map::set db.vae_index v new_ae)))]
      
      ;; For each fact, add to all indices
      (let [db1 (List::fold facts db
                     (fn [db d] (add_to_eavt d.entity d.attr d)))]
        (let [db2 (List::fold facts db1
                     (fn [db d] (add_to_aev d.attr d.entity d)))]
          (let [db3 (List::fold facts db2
                       (fn [db d] (add_to_vae d.value d.attr d.entity d)))]
            (IndexedDatabase::new db3.datums new_tx db3.eavt_index db3.aev_index db3.vae_index)))))))
```

### 5.3 Query API

```turmeric
;; Query interface: a predicate on Datum
(defalias Query (-> Datum bool))

;; Basic query: filter datums matching predicate
(defn q [db : Database, query : Query] : (list Datum)
  (Vector::filter query db.datums))

;; Query by entity
(defn entity_q [e : Entity] : Query
  (fn [d] (= d.entity e)))

;; Query by attribute
(defn attr_q [a : Attribute] : Query
  (fn [d] (= d.attr a)))

;; Query by entity and attribute
(defn eav_q [e : Entity, a : Attribute] : Query
  (fn [d] (and (= d.entity e) (= d.attr a))))

;; Query by attribute and value
(defn av_q [a : Attribute, v : Value] : Query
  (fn [d] (and (= d.attr a) (value_equal? d.value v))))

;; Composite queries
(defn and_q [q1 : Query, q2 : Query] : Query
  (fn [d] (and (q1 d) (q2 d))))

(defn or_q [q1 : Query, q2 : Query] : Query
  (fn [d] (or (q1 d) (q2 d))))

(defn not_q [q : Query] : Query
  (fn [d] (not (q d))))
```

### 5.4 Value Comparison

```turmeric
;; Compare two Values for equality
(defn value_equal? [a : Value, b : Value] : bool
  (match [a b]
    [(Value::LONG a_val) (Value::LONG b_val)] (= a_val b_val)
    [(Value::BOOL a_val) (Value::BOOL b_val)] (= a_val b_val)
    [(Value::STR a_val) (Value::STR b_val)] (= a_val b_val)
    [(Value::KEYWORD a_val) (Value::KEYWORD b_val)] (= a_val b_val)
    [(Value::ENTITY a_val) (Value::ENTITY b_val)] (= a_val b_val)
    [(Value::INST a_val) (Value::INST b_val)] (= a_val b_val)
    [_ _] false))

;; Compare two Values for ordering (when applicable)
(defn value_compare [a : Value, b : Value] : (option int)
  (match [a b]
    [(Value::LONG a_val) (Value::LONG b_val)] (some (compare a_val b_val))
    [(Value::STR a_val) (Value::STR b_val)] (some (str::compare a_val b_val))
    [(Value::INST a_val) (Value::INST b_val)] (some (compare a_val b_val))
    _ none))

;; Greater than
(defn value_gt? [a : Value, b : Value] : bool
  (match (value_compare a b)
    (some 1) true
    _ false))

;; Less than
(defn value_lt? [a : Value, b : Value] : bool
  (match (value_compare a b)
    (some -1) true
    _ false))
```

### 5.5 Pull API (Get Entity with Attributes)

```turmeric
;; Pull an entity: get all its attributes as a map
(defn pull [db : Database, e : Entity] : (Map Attribute Value)
  (let [entity_datums (q db (entity_q e))]
    (List::fold entity_datums (Map::new)
                (fn [m d] (Map::set m d.attr d.value)))))

;; Pull with specific attributes only
(defn pull_many [db : Database, e : Entity, attrs : (list Attribute)] : (Map Attribute Value)
  (let [result (Map::new)]
    (let [_ (List::for_each attrs
              (fn [a]
                (let [d (q db (and_q (entity_q e) (attr_q a)))]
                  (if (not (List::empty? d))
                    (Map::set result a (Datum::value (List::head d)))))))]
      result)))
```

### 5.6 Temporal Queries

```turmeric
;; Get database state as-of a specific transaction
(defn db_as_of [db : Database, as_of_tx : int64] : Database
  (let [filtered (Vector::filter (fn [d] (<= d.tx as_of_tx)) db.datums)]
    (Database::new filtered as_of_tx)))

;; Query as-of a specific time
(defn q_as_of [db : Database, query : Query, as_of_tx : int64] : (list Datum)
  (q (db_as_of db as_of_tx) query))

;; History of an entity's attribute
(defn history [db : Database, e : Entity, a : Attribute] : (list Datum)
  (let [datums (q db (and_q (entity_q e) (attr_q a)))]
    (List::sort datums (fn [d1 d2] (< d1.tx d2.tx)))))
```

### 5.7 Retraction

```turmeric
;; Retraction: we represent retractions as special datums
;; A retraction fact has a negative tx value (or we could use a special attribute)
;; Alternative: use a :db/retract attribute

defn retract [db : Database, e : Entity, a : Attribute] : Database
  (let [retraction (Datum::new e a (Value::BOOL true) (neg db.next_tx))]
    (assert! db (List::singleton retraction))))

;; Check if a fact is retracted as-of a given tx
(defn is_retracted? [db : Database, d : Datum, as_of_tx : int64] : bool
  (let [retractions (q db (and_q (entity_q d.entity)
                                 (attr_q ":db/retract")))]
    (List::exists retractions
                   (fn [r]
                     (and (= r.attr d.attr)
                          (<= r.tx as_of_tx)
                          (>= (neg r.tx) d.tx))))))

;; Query that respects retractions
(defn q_with_retraction [db : Database, query : Query, as_of_tx : int64] : (list Datum)
  (let [candidates (q_as_of db query as_of_tx)]
    (List::filter candidates (fn [d] (not (is_retracted? db d as_of_tx))))))
```

---

## 6. Indexed Database Implementation

### 6.1 Index Types

```turmeric
;; Index from Entity+Attribute to Datum list
(defalias EAVT_Index (Map (Pair Entity Attribute) (Vector Datum)))

;; Index from Attribute to (Entity to Datum list)
(defalias AEV_Index (Map Attribute (Map Entity (Vector Datum))))

;; Index from Value to (Attribute to (Entity to Datum list))
;; Note: Value needs to implement Hash and Eq for this to work
(defalias VAE_Index (Map Value (Map Attribute (Map Entity (Vector Datum)))))
```

### 6.2 Index Lookup Functions

```turmeric
;; Lookup by entity and attribute in EAVT index
(defn lookup_eavt [idx : EAVT_Index, e : Entity, a : Attribute] : (option (Vector Datum))
  (Map::get idx (Pair::new e a)))

;; Lookup by attribute in AEV index
(defn lookup_a [idx : AEV_Index, a : Attribute] : (option (Map Entity (Vector Datum)))
  (Map::get idx a))

;; Lookup by attribute and entity in AEV index
(defn lookup_ae [idx : AEV_Index, a : Attribute, e : Entity] : (option (Vector Datum))
  (match (lookup_a idx a)
    (some entities_map) (Map::get entities_map e)
    none none))

;; Lookup by value in VAE index
(defn lookup_v [idx : VAE_Index, v : Value] : (option (Map Attribute (Map Entity (Vector Datum))))
  (Map::get idx v))

;; Lookup by value and attribute in VAE index
(defn lookup_va [idx : VAE_Index, v : Value, a : Attribute] : (option (Map Entity (Vector Datum)))
  (match (lookup_v idx v)
    (some attr_map) (Map::get attr_map a)
    none none))

;; Lookup by value, attribute, and entity in VAE index
(defn lookup_vae [idx : VAE_Index, v : Value, a : Attribute, e : Entity] : (option (Vector Datum))
  (match (lookup_va idx v a)
    (some entity_map) (Map::get entity_map e)
    none none))
```

### 6.3 Indexed Query Functions

```turmeric
;; Query using EAVT index
(defn q_eavt [db : IndexedDatabase, e : Entity, a : Attribute] : (list Datum)
  (match (lookup_eavt db.eavt_index e a)
    (some datums) (Vector::to_list datums)
    none []))

;; Query using AEV index
(defn q_a [db : IndexedDatabase, a : Attribute] : (list Datum)
  (match (lookup_a db.aev_index a)
    (some entities_map)
      (Map::fold entities_map (fn [acc e datums] (List::concat acc (Vector::to_list datums))))
    none []))

(defn q_ae [db : IndexedDatabase, a : Attribute, e : Entity] : (list Datum)
  (match (lookup_ae db.aev_index a e)
    (some datums) (Vector::to_list datums)
    none []))

;; Query using VAE index
(defn q_v [db : IndexedDatabase, v : Value] : (list Datum)
  (match (lookup_v db.vae_index v)
    (some attr_map)
      (Map::fold attr_map
        (fn [acc a entity_map] (Map::fold entity_map (fn [acc2 e datums] (List::concat acc2 (Vector::to_list datums))) acc))
        [])
    none []))

(defn q_va [db : IndexedDatabase, v : Value, a : Attribute] : (list Datum)
  (match (lookup_va db.vae_index v a)
    (some entity_map) (Map::fold entity_map (fn [acc e datums] (List::concat acc (Vector::to_list datums))) [])
    none []))

(defn q_vae [db : IndexedDatabase, v : Value, a : Attribute, e : Entity] : (list Datum)
  (match (lookup_vae db.vae_index v a e)
    (some datums) (Vector::to_list datums)
    none []))
```

---

## 7. Example: Blog System

### 7.1 Defining the Schema (Optional)

```turmeric
;; examples/datalog/blog.tur

;; Schema attributes (optional - EAVT doesn't require schema)
def user/name   ":user/name"
def user/email  ":user/email"
def user/id     ":user/id"

def post/title    ":post/title"
def post/body     ":post/body"
def post/author   ":post/author"
def post/date     ":post/date"
def post/id       ":post/id"

def comment/body   ":comment/body"
def comment/author ":comment/author"
def comment/post   ":comment/post"
def comment/date   ":comment/date"
def comment/id     ":comment/id"
```

### 7.2 Creating Data

```turmeric
;; Create some users
defn create_users [] : (list Datum)
  (list::concat
    [(Datum::new 1 user/id (Value::LONG 1) 1)
     (Datum::new 1 user/name (Value::STR "Alice") 1)
     (Datum::new 1 user/email (Value::STR "alice@example.com") 1)
     
     (Datum::new 2 user/id (Value::LONG 2) 1)
     (Datum::new 2 user/name (Value::STR "Bob") 1)
     (Datum::new 2 user/email (Value::STR "bob@example.com") 1)]))

;; Create some posts
defn create_posts [user1_id : Entity, user2_id : Entity] : (list Datum)
  (list::concat
    [(Datum::new 101 post/id (Value::LONG 101) 2)
     (Datum::new 101 post/title (Value::STR "Hello World") 2)
     (Datum::new 101 post/body (Value::STR "My first post!") 2)
     (Datum::new 101 post/author (Value::ENTITY user1_id) 2)
     (Datum::new 101 post/date (Value::INST 1715308800000) 2)  ;; 2024-05-10
     
     (Datum::new 102 post/id (Value::LONG 102) 2)
     (Datum::new 102 post/title (Value::STR "Second Post") 2)
     (Datum::new 102 post/body (Value::STR "Another post.") 2)
     (Datum::new 102 post/author (Value::ENTITY user2_id) 2)
     (Datum::new 102 post/date (Value::INST 1715395200000) 2)])  ;; 2024-05-11

;; Create some comments
defn create_comments [post1_id : Entity, user1_id : Entity, user2_id : Entity] : (list Datum)
  (list::concat
    [(Datum::new 201 comment/id (Value::LONG 201) 3)
     (Datum::new 201 comment/body (Value::STR "Nice post!") 3)
     (Datum::new 201 comment/author (Value::ENTITY user2_id) 3)
     (Datum::new 201 comment/post (Value::ENTITY post1_id) 3)
     (Datum::new 201 comment/date (Value::INST 1715312400000) 3)  ;; 2024-05-10 02:00:00
     
     (Datum::new 202 comment/id (Value::LONG 202) 3)
     (Datum::new 202 comment/body (Value::STR "Thanks!") 3)
     (Datum::new 202 comment/author (Value::ENTITY user1_id) 3)
     (Datum::new 202 comment/post (Value::ENTITY post1_id) 3)
     (Datum::new 202 comment/date (Value::INST 1715316000000) 3)])  ;; 2024-05-10 03:00:00
```

### 7.3 Building the Database

```turmeric
(defn build_blog_db [] : IndexedDatabase
  (let [db (empty_indexed_db)]
    (let [db1 (assert_indexed! db (create_users))]
      (let [db2 (assert_indexed! db1 (create_posts 1 2))]
        (assert_indexed! db2 (create_comments 101 1 2))))))
```

### 7.4 Querying the Blog

```turmeric
;; Get all posts
defn get_all_posts [db : IndexedDatabase] : (list (Map Attribute Value))
  (let [post_entities (q_a db post/id)]
    (List::map post_entities
               (fn [d]
                 (pull db d.entity)))))

;; Get posts by a user
defn get_posts_by_user [db : IndexedDatabase, user_id : Entity] : (list (Map Attribute Value))
  (let [datums (q_ae db post/author user_id)]
    (List::map datums
               (fn [d]
                 (pull db d.entity)))))

;; Get comments on a post
defn get_comments_on_post [db : IndexedDatabase, post_id : Entity] : (list (Map Attribute Value))
  (let [datums (q_ae db comment/post post_id)]
    (List::map datums
               (fn [d]
                 (pull db d.entity)))))

;; Get a post with its author and comments
defn get_post_with_details [db : IndexedDatabase, post_id : Entity] : (Map Attribute (option Value))
  (let [post (pull db post_id)]
    (let [author_id (Map::get post post/author)]
      (let [author (match author_id (some e) (pull db e) none)]
        (let [comments (get_comments_on_post db post_id)]
          (Map::merge post
                     (Map::singleton ":post/author" author)
                     (Map::singleton ":post/comments" comments)))))))

;; Usage
defn example_queries []
  (let [db (build_blog_db)]
    (let [posts (get_all_posts db)]
      (println "All posts:" posts)
      
      (let [alice_posts (get_posts_by_user db 1)]
        (println "Alice's posts:" alice_posts)
        
        (let [post1 (get_post_with_details db 101)]
          (println "Post 101 with details:" post1))))))
```

---

## 8. Schema System (Optional Enhancement)

### 8.1 Schema Types

```turmeric
;; examples/datalog/schema.tur

;; Attribute cardinality
defalias Cardinality
  (variant
    ONE     ;; Single value
    MANY)   ;; Multiple values

;; Attribute type
defalias AttributeType
  (variant
    LONG_TYPE
    BOOL_TYPE
    STR_TYPE
    KEYWORD_TYPE
    ENTITY_TYPE
    INST_TYPE
    REF Entity)  ;; Reference to another entity (with specific type)

;; Schema for an attribute
defstruct AttributeSchema
  [cardinality : Cardinality
   type : AttributeType
   doc : cstr])

;; Database schema: map from attribute to its schema
defalias Schema (Map Attribute AttributeSchema))
```

### 8.2 Schema Validation

```turmeric
;; Validate a datum against a schema
(defn validate_datum [schema : Schema, d : Datum] : (Result unit cstr)
  (let [attr_schema (Map::get schema d.attr)]
    (match attr_schema
      none (ok unit)  ;; No schema = anything goes
      (some s)
        (match s.type
          (AttributeType::LONG_TYPE)
            (match d.value
              (Value::LONG _) (ok unit)
              _ (err (str::concat "Expected LONG for " d.attr)))
          (AttributeType::STR_TYPE)
            (match d.value
              (Value::STR _) (ok unit)
              _ (err (str::concat "Expected STR for " d.attr)))
          (AttributeType::ENTITY_TYPE)
            (match d.value
              (Value::ENTITY _) (ok unit)
              _ (err (str::concat "Expected ENTITY for " d.attr)))
          (AttributeType::REF target_type)
            (match d.value
              (Value::ENTITY e)
                (let [entity_type (get_entity_type schema e)]
                  (if (= entity_type target_type)
                    (ok unit)
                    (err (str::concat "Expected reference to " target_type ", got " entity_type))))
              _ (err (str::concat "Expected ENTITY for " d.attr)))
          ))))

;; Validate a list of datums
(defn validate_datums [schema : Schema, datums : (list Datum)] : (Result unit cstr)
  (List::fold datums (ok unit)
              (fn [acc d] (Result::bind acc (fn [_] (validate_datum schema d))))))
```

### 8.3 Schema Definition

```turmeric
defn blog_schema [] : Schema
  (let [schema (Map::new)]
    ;; User attributes
    (let [schema (Map::set schema user/id
                            (AttributeSchema::new Cardinality::ONE AttributeType::LONG_TYPE "User ID"))]
      (let [schema (Map::set schema user/name
                              (AttributeSchema::new Cardinality::ONE AttributeType::STR_TYPE "User name"))]
        (let [schema (Map::set schema user/email
                                (AttributeSchema::new Cardinality::ONE AttributeType::STR_TYPE "User email"))]
          
          ;; Post attributes
          (let [schema (Map::set schema post/id
                                (AttributeSchema::new Cardinality::ONE AttributeType::LONG_TYPE "Post ID"))]
            (let [schema (Map::set schema post/title
                                  (AttributeSchema::new Cardinality::ONE AttributeType::STR_TYPE "Post title"))]
              (let [schema (Map::set schema post/body
                                    (AttributeSchema::new Cardinality::ONE AttributeType::STR_TYPE "Post body"))]
                (let [schema (Map::set schema post/author
                                      (AttributeSchema::new Cardinality::ONE (AttributeType::REF user/id) "Post author"))]
                  (let [schema (Map::set schema post/date
                                        (AttributeSchema::new Cardinality::ONE AttributeType::INST_TYPE "Post date"))]
                    
                    ;; Comment attributes
                    (let [schema (Map::set schema comment/id
                                          (AttributeSchema::new Cardinality::ONE AttributeType::LONG_TYPE "Comment ID"))]
                      (let [schema (Map::set schema comment/body
                                            (AttributeSchema::new Cardinality::ONE AttributeType::STR_TYPE "Comment body"))]
                        (let [schema (Map::set schema comment/author
                                              (AttributeSchema::new Cardinality::ONE (AttributeType::REF user/id) "Comment author"))]
                          (let [schema (Map::set schema comment/post
                                                (AttributeSchema::new Cardinality::ONE (AttributeType::REF post/id) "Commented post"))]
                            (Map::set schema comment/date
                                      (AttributeSchema::new Cardinality::ONE AttributeType::INST_TYPE "Comment date"))
                          )))))))))))))))
```

---

## 9. Persistence

### 9.1 Serialization Format

```
Storage format (v1):
- Magic bytes: "EAVT\0"
- Version: 1 byte
- Number of datums: 8 bytes (little-endian)
- For each datum:
  - Entity: 8 bytes
  - Attribute length: 4 bytes
  - Attribute: N bytes (UTF-8)
  - Value type: 1 byte (0=LONG, 1=BOOL, 2=STR, 3=KEYWORD, 4=ENTITY, 5=INST)
  - Value:
    - LONG: 8 bytes
    - BOOL: 1 byte (0=false, 1=true)
    - STR/KEYWORD: 4 bytes length + N bytes
    - ENTITY: 8 bytes
    - INST: 8 bytes
  - Transaction: 8 bytes
```

### 9.2 Serialization Code

```turmeric
;; examples/datalog/persistent.tur

(defalias SerializedDatum (Vector uint8))

(defn serialize_value [v : Value] : (Vector uint8)
  (match v
    (Value::LONG x)
      (Vector::concat (Vector::singleton 0) (int64::to_be_bytes x))
    (Value::BOOL x)
      (Vector::concat (Vector::singleton 1) (Vector::singleton (if x 1 0)))
    (Value::STR x)
      (let [bytes (str::to_utf8 x)]
        (Vector::concat
          (Vector::singleton 2)
          (int32::to_be_bytes (Vector::length bytes))
          bytes))
    (Value::KEYWORD x)
      (let [bytes (str::to_utf8 x)]
        (Vector::concat
          (Vector::singleton 3)
          (int32::to_be_bytes (Vector::length bytes))
          bytes))
    (Value::ENTITY x)
      (Vector::concat (Vector::singleton 4) (int64::to_be_bytes x))
    (Value::INST x)
      (Vector::concat (Vector::singleton 5) (int64::to_be_bytes x))))

(defn serialize_datum [d : Datum] : SerializedDatum
  (Vector::concat
    (int64::to_be_bytes d.entity)
    (int32::to_be_bytes (str::length d.attr))
    (str::to_utf8 d.attr)
    (serialize_value d.value)
    (int64::to_be_bytes d.tx)))

(defn serialize_db [db : Database] : (Vector uint8)
  (let [magic (str::to_utf8 "EAVT\0")]
    (let [version (Vector::singleton 1)]
      (let [count (int64::to_be_bytes (Vector::length db.datums))]
        (let [datums (Vector::map db.datums serialize_datum)]
          (Vector::concat magic version count (Vector::concat_all datums)))))))
```

### 9.3 Deserialization Code

```turmeric
(defn deserialize_value [bytes : (Vector uint8), offset : int] : (Pair Value int)
  (let [type_byte (Vector::get bytes offset)]
    (match type_byte
      0  ;; LONG
        (let [value_bytes (Vector::slice bytes (inc offset) 8)]
          (Pair::new (Value::LONG (int64::from_be_bytes value_bytes)) (+ offset 9)))
      1  ;; BOOL
        (let [value_byte (Vector::get bytes (inc offset))]
          (Pair::new (Value::BOOL (= value_byte 1)) (+ offset 2)))
      2  ;; STR
        (let [len_bytes (Vector::slice bytes (inc offset) 4)]
          (let [len (int32::from_be_bytes len_bytes)]
            (let [str_bytes (Vector::slice bytes (+ offset 5) len)]
              (Pair::new (Value::STR (str::from_utf8 str_bytes)) (+ offset 5 len)))))
      3  ;; KEYWORD
        (let [len_bytes (Vector::slice bytes (inc offset) 4)]
          (let [len (int32::from_be_bytes len_bytes)]
            (let [kw_bytes (Vector::slice bytes (+ offset 5) len)]
              (Pair::new (Value::KEYWORD (str::from_utf8 kw_bytes)) (+ offset 5 len)))))
      4  ;; ENTITY
        (let [value_bytes (Vector::slice bytes (inc offset) 8)]
          (Pair::new (Value::ENTITY (int64::from_be_bytes value_bytes)) (+ offset 9)))
      5  ;; INST
        (let [value_bytes (Vector::slice bytes (inc offset) 8)]
          (Pair::new (Value::INST (int64::from_be_bytes value_bytes)) (+ offset 9)))
      _ (panic (str::concat "Unknown value type: " (int::to_string type_byte))))))

(defn deserialize_datum [bytes : (Vector uint8), offset : int] : (Pair Datum int)
  (let [entity_bytes (Vector::slice bytes offset 8)]
    (let [entity (int64::from_be_bytes entity_bytes)]
      (let [attr_len_bytes (Vector::slice bytes (+ offset 8) 4)]
        (let [attr_len (int32::from_be_bytes attr_len_bytes)]
          (let [attr_bytes (Vector::slice bytes (+ offset 12) attr_len)]
            (let [attr (str::from_utf8 attr_bytes)]
              (let [value_offset (+ offset 12 attr_len)]
                (let [value_result (deserialize_value bytes value_offset)]
                  (let [value (Pair::first value_result)]
                    (let [tx_bytes (Vector::slice bytes (Pair::second value_result) 8)]
                      (let [tx (int64::from_be_bytes tx_bytes)]
                        (Pair::new
                          (Datum::new entity attr value tx)
                          (+ (Pair::second value_result) 8)))))))))))))

(defn deserialize_db [bytes : (Vector uint8)] : (Result Database cstr)
  (let [magic (Vector::slice bytes 0 5)]
    (if (not (Vector::equal magic (str::to_utf8 "EAVT\0")))
      (err "Invalid magic bytes")
      (let [version (Vector::get bytes 5)]
        (if (= version 1)
          (let [count_bytes (Vector::slice bytes 6 8)]
            (let [count (int64::from_be_bytes count_bytes)]
              (let [data_start (+ 6 8)]
                (letrec [loop [remaining : int, offset : int, acc : (Vector Datum)]
                         (if (= remaining 0)
                           (ok (Database::new acc (Vector::length acc)))
                           (let [result (deserialize_datum bytes offset)]
                             (loop (dec remaining)
                                    (Pair::second result)
                                    (Vector::append acc (Pair::first result)))))]
                  (loop count data_start (Vector::new))))))
          (err (str::concat "Unsupported version: " (int::to_string version))))))))
```

### 9.4 File I/O

```turmeric
;; Note: This assumes Phase X file I/O is available
;; For now, we'll use a simple in-memory byte vector

defn save_db [db : Database, path : cstr] : (Result unit cstr)
  (let [bytes (serialize_db db)]
    (file::write_bytes path bytes))

defn load_db [path : cstr] : (Result Database cstr)
  (file::read_bytes path)
  (Result::bind bytes (fn [bytes] (deserialize_db bytes))))
```

---

## 10. Testing Strategy

### 10.1 Unit Tests

```turmeric
;; tests/datalog/test_minimal.tur

defn test_empty_db []
  (let [db (empty_db)]
    (assert (= (Vector::length db.datums) 0))
    (assert (= db.next_tx 0))))

defn test_assert []
  (let [db (empty_db)]
    (let [d (Datum::new 1 ":test/attr" (Value::LONG 42) 1)]
      (let [db2 (assert! db (List::singleton d))]
        (assert (= (Vector::length db2.datums) 1))
        (assert (= db2.next_tx 1))
        (let [d2 (Vector::get db2.datums 0)]
          (assert (= d2.entity 1))
          (assert (= d2.attr ":test/attr"))
          (assert (= d2.value (Value::LONG 42)))
          (assert (= d2.tx 1)))))))

defn test_query []
  (let [db (empty_db)]
    (let [facts [(Datum::new 1 ":user/name" (Value::STR "Alice") 1)
                 (Datum::new 2 ":user/name" (Value::STR "Bob") 1)]]
      (let [db2 (assert! db facts)]
        (let [users (q db2 (attr_q ":user/name"))]
          (assert (= (List::length users) 2)))))))
```

### 10.2 Query Tests

```turmeric
;; tests/datalog/test_query.tur

defn test_and_query []
  (let [db (empty_db)]
    (let [facts [(Datum::new 1 ":user/name" (Value::STR "Alice") 1)
                 (Datum::new 1 ":user/age" (Value::LONG 30) 1)
                 (Datum::new 2 ":user/name" (Value::STR "Bob") 1)]]
      (let [db2 (assert! db facts)]
        (let [alice_age (q db2 (and_q (entity_q 1) (attr_q ":user/age")))]
          (assert (= (List::length alice_age) 1))
          (assert (= (Datum::value (List::head alice_age)) (Value::LONG 30)))))))

defn test_or_query []
  (let [db (empty_db)]
    (let [facts [(Datum::new 1 ":user/name" (Value::STR "Alice") 1)
                 (Datum::new 2 ":user/name" (Value::STR "Bob") 1)]]
      (let [db2 (assert! db facts)]
        (let [alice_or_bob (q db2 (or_q (entity_q 1) (entity_q 2)))]
          (assert (= (List::length alice_or_bob) 2)))))))

defn test_not_query []
  (let [db (empty_db)]
    (let [facts [(Datum::new 1 ":user/name" (Value::STR "Alice") 1)
                 (Datum::new 1 ":user/admin" (Value::BOOL true) 1)
                 (Datum::new 2 ":user/name" (Value::STR "Bob") 1)]]
      (let [db2 (assert! db facts)]
        (let [non_admins (q db2 (and_q (attr_q ":user/name") (not_q (attr_q ":user/admin"))))]
          (assert (= (List::length non_admins) 1))
          (assert (= (Datum::entity (List::head non_admins)) 2)))))))
```

### 10.3 Index Tests

```turmeric
;; tests/datalog/test_indexed.tur

defn test_eavt_index []
  (let [db (empty_indexed_db)]
    (let [facts [(Datum::new 1 ":user/name" (Value::STR "Alice") 1)
                 (Datum::new 1 ":user/age" (Value::LONG 30) 1)]]
      (let [db2 (assert_indexed! db facts)]
        (let [result (q_eavt db2 1 ":user/name")]
          (assert (= (List::length result) 1))
          (assert (= (Datum::value (List::head result)) (Value::STR "Alice")))))))

defn test_aev_index []
  (let [db (empty_indexed_db)]
    (let [facts [(Datum::new 1 ":user/name" (Value::STR "Alice") 1)
                 (Datum::new 2 ":user/name" (Value::STR "Bob") 1)]]
      (let [db2 (assert_indexed! db facts)]
        (let [result (q_a db2 ":user/name")]
          (assert (= (List::length result) 2)))))))

defn test_vae_index []
  (let [db (empty_indexed_db)]
    (let [facts [(Datum::new 1 ":user/name" (Value::STR "Alice") 1)
                 (Datum::new 2 ":user/name" (Value::STR "Alice") 1)]]
      (let [db2 (assert_indexed! db facts)]
        (let [result (q_v db2 (Value::STR "Alice"))]
          (assert (= (List::length result) 2)))))))
```

### 10.4 Serialization Tests

```turmeric
;; tests/datalog/test_persistent.tur

defn test_serialize_deserialize []
  (let [db (empty_db)]
    (let [facts [(Datum::new 1 ":test/attr" (Value::LONG 42) 1)
                 (Datum::new 2 ":test/attr" (Value::STR "hello") 1)]]
      (let [db2 (assert! db facts)]
        (let [bytes (serialize_db db2)]
          (let [result (deserialize_db bytes)]
            (match result
              (ok db3)
                (assert (= (Vector::length db3.datums) 2))
                (let [d1 (Vector::get db3.datums 0)]
                  (assert (= d1.entity 1))
                  (assert (= d1.attr ":test/attr"))
                  (assert (= d1.value (Value::LONG 42))))
                (let [d2 (Vector::get db3.datums 1)]
                  (assert (= d2.entity 2))
                  (assert (= d2.value (Value::STR "hello"))))
              (err e) (panic (str::concat "Deserialization failed: " e)))))))))
```

---

## 11. Implementation Phases

### Phase 1: Core Types & Minimal Implementation (2-3 days)
- [ ] Define `Entity`, `Attribute`, `Value` types
- [ ] Define `Datum` struct
- [ ] Define `Database` struct
- [ ] Implement `empty_db`, `assert!`, basic `q`
- [ ] Write tests for minimal implementation
- [ ] Create `examples/datalog/minimal.tur`

### Phase 2: Query API (2-3 days)
- [ ] Implement query combinators (`and_q`, `or_q`, `not_q`)
- [ ] Implement value comparison functions
- [ ] Implement pull API
- [ ] Implement temporal queries (`db_as_of`, `history`)
- [ ] Implement retraction support
- [ ] Write query tests
- [ ] Create `examples/datalog/query.tur`

### Phase 3: Indexing (2 days)
- [ ] Define index structures
- [ ] Implement `IndexedDatabase`
- [ ] Implement `assert_indexed!` with index updates
- [ ] Implement indexed query functions (`q_eavt`, `q_a`, `q_v`, etc.)
- [ ] Write index tests
- [ ] Create `examples/datalog/indexed.tur`

### Phase 4: Example Application (1-2 days)
- [ ] Create blog system example
- [ ] Implement schema system (optional)
- [ ] Create comprehensive examples
- [ ] Create `examples/datalog/blog.tur`
- [ ] Create `examples/datalog/schema.tur`

### Phase 5: Persistence (2 days)
- [ ] Design serialization format
- [ ] Implement serialization functions
- [ ] Implement deserialization functions
- [ ] Write serialization tests
- [ ] Create `examples/datalog/persistent.tur`

### Phase 6: Documentation (2-3 days)
- [ ] Write `docs/tutorials/datalog/01-concepts.md`
- [ ] Write `docs/tutorials/datalog/02-minimal-impl.md`
- [ ] Write `docs/tutorials/datalog/03-query-api.md`
- [ ] Write `docs/tutorials/datalog/04-indexing.md`
- [ ] Write `docs/tutorials/datalog/05-schema.md` (optional)
- [ ] Write `docs/tutorials/datalog/06-persistence.md`
- [ ] Write `examples/datalog/README.md`

**Total estimated effort:** 11-18 days

---

## 12. Tutorial File Templates

### 01-concepts.md Template

```markdown
# EAVT Database: Core Concepts

## What is EAVT?

EAVT stands for Entity-Attribute-Value-Transaction. It's a data model that...

## Why Immutability?

Immutable data structures provide...

## Basic Operations

- Assert: Add facts
- Retract: Remove facts
- Query: Find facts
- As-of: Query historical state

## Comparison with Other Models

| Model | Strengths | Weaknesses |
|-------|-----------|------------|

## Exercises

1. Model a library catalog in EAVT
2. Represent a social network graph
3. Design temporal tracking for inventory
```

### 02-minimal-impl.md Template

```markdown
# Minimal EAVT Implementation

## Core Types

```turmeric
;; Entity, Attribute, Value, Datum definitions
```

## The Database

```turmeric
;; Database struct and construction
```

## Asserting Facts

```turmeric
;; assert! implementation
```

## Querying

```turmeric
;; Basic q function
```

## Running the Example

```bash
turmeric examples/datalog/minimal.tur
```

## Exercises

1. Add support for boolean values
2. Implement a `q_range` function for numeric ranges
3. Add transaction metadata
```

---

## 13. Open Questions & Design Decisions

### 13.1 Question: Entity IDs

| Option | Pros | Cons |
|--------|------|------|
| **Sequential ints** | Simple, fast | Not distributed-friendly |
| **UUIDs** | Globally unique | Larger, slower |
| **Hash of content** | Content-addressed | Deterministic, but complex |
| **User-provided** | Flexible | User must ensure uniqueness |

**Decision:** Start with sequential ints (simplest). Can add UUID support later.

### 13.2 Question: Value Types

| Approach | Pros | Cons |
|----------|------|------|
| **Tagged union** | Type-safe, extensible | More code |
| **Any type** | Flexible | No type safety |
| **Separate tables** | Optimized per-type | Complex |

**Decision:** Tagged union (`Value` variant) for type safety.

### 13.3 Question: Indexing Strategy

| Index | Query Type | Memory | Update Cost |
|-------|------------|--------|-------------|
| EAVT | Entity+Attr → Value | Medium | Low |
| AEV | Attr → Entity → Value | Medium | Low |
| VAET | Value → Attr → Entity → Tx | High | Medium |
| Full-text | Text search | Very High | High |

**Decision:** Implement EAVT and AEV for v1. Add VAET if needed.

### 13.4 Question: Query Language

| Approach | Pros | Cons |
|----------|------|------|
| **Functional combinators** | Composable, Turmeric-idiomatic | Less familiar |
| **Datalog-style** | Declarative, powerful | More complex implementation |
| **SQL-like strings** | Familiar | Not type-safe, parsing overhead |

**Decision:** Functional combinators for v1 (matches Turmeric style). Could add Datalog later.

---

## 14. Related Work

| System | Model | Language |
|--------|-------|----------|
| **Datomic** | EAVT + Datalog | Clojure |
| **DataScript** | EAVT + Datalog | JavaScript/ClojureScript |
| **XTDB** | EAVT + Datalog + Kafka | Clojure |
| **Dato** | EAVT | ClojureScript |

---

## 15. References

1. [Datomic Documentation — Entities](https://docs.datomic.com/reference/entities.html)
2. [Datomic Data Model](https://docs.datomic.com/data-model.html)
3. [DataScript](https://github.com/tonsky/datascript) — Lightweight Datomic alternative
4. [XTDB](https://xtdb.com/) — Open-source Datomic alternative
5. Richter, Stuart Halloway — "Database as a Value"

---

## 16. Datalog-esque Query Language

### 16.1 Overview

Rather than relying solely on functional combinators, a Datalog-inspired query layer provides a more **declarative, composable** query surface. The design stays minimal: no full unification engine, but enough pattern-matching expressiveness to write joins, negation, and aggregation concisely.

A query is a map with three keys:

```turmeric
;; Query structure
(defstruct DatalogQuery
  [find  : (list FindClause)   ;; what to return
   where : (list WhereClause)  ;; constraints / pattern clauses
   in    : (list cstr)])       ;; optional input variables (default: [])
```

### 16.2 Clauses

**Pattern clause** — match datums where any component may be a variable (prefixed `?`) or a literal:

```turmeric
;; WhereClause variants
(defalias WhereClause
  (variant
    ;; [?e :attr ?v] -- pattern match on entity, attribute, value
    PATTERN  (Triple Term Term Term)
    ;; (not [?e :attr ?v]) -- negation as failure
    NOT      WhereClause
    ;; (or [c1] [c2]) -- disjunction
    OR       (Pair WhereClause WhereClause)
    ;; (and [c1] [c2]) -- explicit conjunction (default is implicit)
    AND      (Pair WhereClause WhereClause)))

;; A term is either a variable or a literal value
(defalias Term
  (variant
    VAR  cstr    ;; "?name", "?e", etc.
    LIT  Value   ;; Value::STR "Alice", Value::LONG 42, etc.
    WILD))       ;; "_" -- match anything, bind nothing
```

**Find clause** — what variables (or aggregations) to return:

```turmeric
(defalias FindClause
  (variant
    FIND_VAR   cstr                          ;; ?e, ?name, ...
    FIND_PULL  (Pair cstr (list Attribute))  ;; (pull ?e [:user/name :user/email])
    FIND_AGG   (Pair AggFn cstr)))           ;; (count ?e), (distinct ?v)

(defalias AggFn
  (variant
    COUNT
    COUNT_DISTINCT
    SUM
    MIN
    MAX))
```

### 16.3 Execution Model

Execution is a simple **binding-propagation** pipeline:

1. Start with one empty binding map `{}`.
2. For each `WHERE` clause in order, expand the current set of bindings by pattern-matching against the database.
3. After all clauses, project the surviving bindings through the `FIND` spec.

```turmeric
;; A binding is a map from variable name to Value
(defalias Binding (Map cstr Value))

;; Execute a single pattern clause against the database, extending bindings
(defn match_pattern [db : Database, clause : WhereClause, bindings : (list Binding)]
  : (list Binding)
  (match clause
    (WhereClause::PATTERN e_term a_term v_term)
      (List::flat_map bindings
        (fn [b]
          (let [candidates (filter_by_bound db b e_term a_term v_term)]
            (List::filter_map candidates (fn [d] (extend_binding b e_term a_term v_term d))))))
    (WhereClause::NOT inner)
      (List::filter bindings
        (fn [b] (List::empty? (match_pattern db inner [b]))))
    (WhereClause::OR (Pair c1 c2))
      (List::concat (match_pattern db c1 bindings) (match_pattern db c2 bindings))
    (WhereClause::AND (Pair c1 c2))
      (match_pattern db c2 (match_pattern db c1 bindings))))

;; Run a full query
(defn datalog [db : Database, query : DatalogQuery] : (list (list Value))
  (let [initial_bindings [Map::new]]
    (let [bindings (List::fold query.where initial_bindings
                     (fn [bs clause] (match_pattern db clause bs)))]
      (List::map bindings (fn [b] (project_find query.find b))))))
```

### 16.4 Example Queries

```turmeric
;; Find all user names
(defn q_all_names [db : Database] : (list (list Value))
  (datalog db
    (DatalogQuery::new
      [(FindClause::FIND_VAR "?name")]
      [(WhereClause::PATTERN
          (Term::VAR "?e")
          (Term::LIT (Value::KEYWORD ":user/name"))
          (Term::VAR "?name"))]
      [])))

;; Find posts authored by Alice (join across entities)
(defn q_alice_posts [db : Database] : (list (list Value))
  (datalog db
    (DatalogQuery::new
      [(FindClause::FIND_VAR "?title")]
      [(WhereClause::PATTERN
          (Term::VAR "?alice")
          (Term::LIT (Value::KEYWORD ":user/name"))
          (Term::LIT (Value::STR "Alice")))
       (WhereClause::PATTERN
          (Term::VAR "?post")
          (Term::LIT (Value::KEYWORD ":post/author"))
          (Term::VAR "?alice"))
       (WhereClause::PATTERN
          (Term::VAR "?post")
          (Term::LIT (Value::KEYWORD ":post/title"))
          (Term::VAR "?title"))]
      [])))

;; Count comments per post
(defn q_comment_counts [db : Database] : (list (list Value))
  (datalog db
    (DatalogQuery::new
      [(FindClause::FIND_VAR "?post")
       (FindClause::FIND_AGG (Pair AggFn::COUNT "?comment"))]
      [(WhereClause::PATTERN
          (Term::VAR "?comment")
          (Term::LIT (Value::KEYWORD ":comment/post"))
          (Term::VAR "?post"))]
      [])))

;; Pull full entity for each matching post
(defn q_post_pull [db : Database] : (list (list Value))
  (datalog db
    (DatalogQuery::new
      [(FindClause::FIND_PULL "?post" [":post/title" ":post/body" ":post/date"])]
      [(WhereClause::PATTERN
          (Term::VAR "?post")
          (Term::LIT (Value::KEYWORD ":post/id"))
          (Term::WILD))]
      [])))
```

### 16.5 Input Variables

Queries can accept runtime parameters via the `:in` spec, bound before execution:

```turmeric
;; Find posts by a given author name (passed as input)
(defn q_posts_by_author [db : Database, author_name : cstr] : (list (list Value))
  (datalog_with_inputs db
    (DatalogQuery::new
      [(FindClause::FIND_VAR "?title")]
      [(WhereClause::PATTERN
          (Term::VAR "?author")
          (Term::LIT (Value::KEYWORD ":user/name"))
          (Term::VAR "?author_name"))
       (WhereClause::PATTERN
          (Term::VAR "?post")
          (Term::LIT (Value::KEYWORD ":post/author"))
          (Term::VAR "?author"))
       (WhereClause::PATTERN
          (Term::VAR "?post")
          (Term::LIT (Value::KEYWORD ":post/title"))
          (Term::VAR "?title"))]
      ["?author_name"])
    [(Value::STR author_name)]))

;; Execute a query with bound inputs
(defn datalog_with_inputs
    [db : Database, query : DatalogQuery, inputs : (list Value)]
    : (list (list Value))
  (let [seed_binding (List::zip query.in inputs
                       (fn [var val] (Pair::new var val)))]
    (let [initial [Map::from_pairs seed_binding]]
      (let [bindings (List::fold query.where initial
                       (fn [bs clause] (match_pattern db clause bs)))]
        (List::map bindings (fn [b] (project_find query.find b)))))))
```

### 16.6 Implementation Notes

| Concern | V1 Approach | Future Option |
|---------|-------------|---------------|
| Variable binding | `Map cstr Value` per result row | Trie-based sharing |
| Join order | Left-to-right clause order | Cost-based reordering |
| Negation | Negation-as-failure (stratified) | Full stratified Datalog |
| Aggregation | Post-projection grouping | Streaming aggregates |
| Recursion | Not supported | Recursive rules |
| Index use | Falls back to `q` filter scan | Planner picks best index |

The functional combinator API (Section 5.3) and the Datalog layer are **complementary**: combinators are ideal for programmatic query construction; Datalog is better for ad-hoc, human-readable queries. Both operate on the same `Database` or `IndexedDatabase` value.
