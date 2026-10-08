---
title: HAMT Guide
category: Data Structures and Libraries
description: Persistent hash maps with structural sharing (HAMT)
---

# HAMT Guide

Persistent, immutable hash maps with structural sharing via Hash Array Mapped Tries.

## Overview

A **Hash Array Mapped Trie (HAMT)** is an immutable hash map where every modification produces a new version of the map rather than mutating the original. Old and new versions share unmodified subtrees, so both time and space costs of updates are O(log N) rather than O(N).

Turmeric's HAMT uses xxHash64 for hashing, 5-bit hash chunks (32 slots per level), and reference counting for memory management. Keys and values are untyped pointers -- the caller owns their lifetimes.

## Creating and Freeing

```turmeric
(import "stdlib/hamt.tur")

;; Create an empty map (ref_count = 1)
(def m (hamt/new))

;; Free when done (decrements ref count; frees if zero)
(hamt/free m)

;; Share ownership (increments ref count; returns same pointer)
(def m2 (hamt/retain m))
```
```sweet-exp
import "stdlib/hamt.tur"

;; Create an empty map (ref_count = 1)
def m hamt/new()

;; Free when done (decrements ref count; frees if zero)
hamt/free(m)

;; Share ownership (increments ref count; returns same pointer)
def m2 hamt/retain(m)
```

### Memory model

The HAMT itself does **not** free keys or values -- the caller owns those lifetimes. Each operation that returns a new `Hamt*` has `ref_count = 1`; the old map is unchanged and must be freed independently.

> This is the low-level trie, where keys are pre-hashed and the caller manages
> every lifetime. At the typed `Map`/`Set` layer built on top, an owned
> `String` key gets the collection to copy and own the key bytes -- so a
> *computed* key never dangles. See [strings-guide.md](strings-guide.md) for the
> owned-key story.

## Core Operations

All mutations return a **new** HAMT; the original is untouched.

```turmeric
;; Hash a string key
(def h (hamt/hash-str "hello"))

;; Insert or update
(def m1 (hamt/set m  h "hello" "world"))
(def m2 (hamt/set m1 h "hello" "updated"))  ; m1 still has "world"

;; Delete a key
(def m3 (hamt/del m2 h "hello"))

;; Lookup -- returns nil if not found
(def val (hamt/get m2 h "hello"))  ; => "updated"

;; Membership test
(hamt/has? m2 h "hello")  ; => true

;; Count -- O(1)
(hamt/count m2)  ; => 1
```
```sweet-exp
;; Hash a string key
def h hamt/hash-str("hello")

;; Insert or update
def m1 hamt/set(m  h "hello" "world")
def m2 hamt/set(m1 h "hello" "updated")  ; m1 still has "world"

;; Delete a key
def m3 hamt/del(m2 h "hello")

;; Lookup -- returns nil if not found
def val hamt/get(m2 h "hello")  ; => "updated"

;; Membership test
hamt/has?(m2 h "hello")  ; => true

;; Count -- O(1)
hamt/count(m2)  ; => 1
```

## Hashing Keys

Two built-in hash functions cover the common cases:

```turmeric
;; Hash a NUL-terminated C string
(def h (hamt/hash-str "my-key"))

;; Hash a pointer value directly (identity map)
(def ptr (some-object))
(def h  (hamt/hash-ptr ptr))
```
```sweet-exp
;; Hash a NUL-terminated C string
def h hamt/hash-str("my-key")

;; Hash a pointer value directly (identity map)
def ptr some-object()
def h   hamt/hash-ptr(ptr)
```

For custom key types, compute a 64-bit hash yourself (e.g. via inline C calling `tur_hamt_hash_xxh64`) and pass it directly to `hamt/set` / `hamt/get` etc.

### String keys compare by content

`hamt/set` and friends compare keys by pointer identity, which is wrong for
strings: two content-equal keys built at runtime have distinct addresses and
the probe misses. Use the content-keyed wrappers instead -- they hash *and*
compare by content, and take no separate hash argument:

```turmeric
(def m2 (hamt/set-cstr m "name" v))
(hamt/get-cstr  m2 "name")   ; => v, even for a runtime-built key
(hamt/has-cstr? m2 "name")   ; => true
(def m3 (hamt/del-cstr m2 "name"))
```
```sweet-exp
def m2 hamt/set-cstr(m "name" v)
hamt/get-cstr(m2 "name")
; => v, even for a runtime-built key
hamt/has-cstr?(m2 "name")
; => true
def m3 hamt/del-cstr(m2 "name")
```

A `^persistent` map with `:cstr` keys lowers to these wrappers, so `assoc` /
`get` / `has?` / `dissoc` on it compare by content. Non-cstr keys keep identity
semantics.

## Merging Maps

```turmeric
;; Merge b into a -- b wins on collision
(def merged (hamt/merge a b))

;; Merge with a custom conflict resolver
;; fn receives (val-from-a val-from-b ctx) and returns the winning value
(def merged (hamt/merge-with a b resolve-fn nil))
```
```sweet-exp
;; Merge b into a -- b wins on collision
def merged hamt/merge(a b)

;; Merge with a custom conflict resolver
;; fn receives (val-from-a val-from-b ctx) and returns the winning value
def merged hamt/merge-with(a b resolve-fn nil)
```

## Iteration

```turmeric
;; Allocate an iterator (at least 64 bytes; use inline C for stack allocation)
(def iter (malloc 128))
(hamt/iter-init iter m)

;; Allocate output slots
(def hash-out (malloc 8))
(def key-out  (malloc 8))
(def val-out  (malloc 8))

(loop
  (when (hamt/iter-next iter hash-out key-out val-out)
    (println (ptr-deref key-out))
    (recur)))

(hamt/iter-free iter)
```
```sweet-exp
;; Allocate an iterator (at least 64 bytes; use inline C for stack allocation)
def iter malloc(128)
hamt/iter-init(iter m)

;; Allocate output slots
def hash-out malloc(8)
def key-out  malloc(8)
def val-out  malloc(8)

loop
  when hamt/iter-next(iter hash-out key-out val-out)
    println(ptr-deref(key-out))
    recur()

hamt/iter-free(iter)
```

Iteration order is unspecified (hash order, not insertion order).

## Higher-Order Operations

```turmeric
;; Map: new HAMT with each value replaced by (fn val ctx)
(def doubled (hamt/map m double-fn nil))

;; Filter: new HAMT keeping only entries where (fn key val ctx) is true
(def filtered (hamt/filter m keep?-fn nil))

;; Reduce: fold all entries; returns final accumulator
(def sum (hamt/reduce m add-fn (int->ptr 0) nil))

;; Merge-with: merge a and b; call (fn val-a val-b ctx) for duplicate keys
(def merged (hamt/merge-with a b resolver nil))
```
```sweet-exp
;; Map: new HAMT with each value replaced by (fn val ctx)
def doubled hamt/map(m double-fn nil)

;; Filter: new HAMT keeping only entries where (fn key val ctx) is true
def filtered hamt/filter(m keep?-fn nil)

;; Reduce: fold all entries; returns final accumulator
def sum hamt/reduce(m add-fn int->ptr(0) nil)

;; Merge-with: merge a and b; call (fn val-a val-b ctx) for duplicate keys
def merged hamt/merge-with(a b resolver nil)
```

The `fn` arguments are C function pointers. Use inline C to define them:

```turmeric
(defn my-map-fn [] : ptr
  ```c
  void *double_val(void *val, void *ctx) {
      return (void*)((intptr_t)val * 2);
  }
  return (void*)double_val;
  ```)
```
```sweet-exp
defn my-map-fn [] :ptr
  ```c
  void *double_val(void *val, void *ctx) {
      return (void*)((intptr_t)val * 2);
  }
  return (void*)double_val;
  ```
```

## Transient Mode

Transients allow **batch construction** with in-place mutation, then seal back into an immutable map. This avoids allocating intermediate copies during bulk inserts.

```
;; Fork a transient from an existing map (original unchanged)
(def t (hamt/transient m))

;; Mutate in place -- no new HAMT created per operation
(hamt/transient-set! t h1 "a" "alpha")
(hamt/transient-set! t h2 "b" "beta")
(hamt/transient-del! t h1 "a")

;; Seal into an immutable map; t must not be used after this
(def result (hamt/persistent! t))
```

**Rule:** never read from or mutate a transient after calling `hamt/persistent!`.

## Debugging

```turmeric
;; Print a human-readable "{k->v, ...}" string (caller must free result)
(def s (hamt/show m))
(println s)
(free s)

;; Dump the trie structure in DOT format to stderr (for Graphviz)
(hamt/dump m)
```
```sweet-exp
;; Print a human-readable "{k->v, ...}" string (caller must free result)
def s hamt/show(m)
println(s)
free(s)

;; Dump the trie structure in DOT format to stderr (for Graphviz)
hamt/dump(m)
```

## Quick Reference

| Function | Description |
|---|---|
| `hamt/new` | Create empty map |
| `hamt/free` | Decrement ref count / free |
| `hamt/retain` | Increment ref count |
| `hamt/set m h k v` | Insert/update; returns new map |
| `hamt/del m h k` | Delete key; returns new map |
| `hamt/get m h k` | Lookup; nil if absent |
| `hamt/has? m h k` | Membership test |
| `hamt/count m` | Number of entries, O(1) |
| `hamt/merge a b` | Merge; b wins on collision |
| `hamt/merge-with a b fn ctx` | Merge with conflict resolver |
| `hamt/map m fn ctx` | Transform values |
| `hamt/filter m fn ctx` | Retain matching entries |
| `hamt/reduce m fn init ctx` | Fold all entries |
| `hamt/iter-init iter m` | Start iteration |
| `hamt/iter-next iter h k v` | Advance; returns false when done |
| `hamt/iter-free iter` | Release iterator resources |
| `hamt/transient m` | Fork mutable transient |
| `hamt/transient-set! t h k v` | Mutate transient (insert/update) |
| `hamt/transient-del! t h k` | Mutate transient (delete) |
| `hamt/persistent! t` | Seal transient into immutable map |
| `hamt/set-cstr m k v` | Insert/update a string key, compared by content |
| `hamt/get-cstr m k` | Lookup a string key by content |
| `hamt/has-cstr? m k` | Membership test on a string key, by content |
| `hamt/del-cstr m k` | Delete a string key, by content |
| `hamt/hash-str s` | xxHash64 of a C string |
| `hamt/hash-ptr p` | Hash a pointer value |
| `hamt/show m` | Heap-allocated display string |
| `hamt/dump m` | DOT dump to stderr |

## Common Patterns

### Build from a list

```turmeric
;; Use a transient for O(N) bulk construction
(defn list->hamt [pairs]
  (let [t (hamt/transient (hamt/new))]
    (for-each pairs
      (fn [[k v]]
        (hamt/transient-set! t (hamt/hash-str k) k v)))
    (hamt/persistent! t)))
```
```sweet-exp
;; Use a transient for O(N) bulk construction
defn list->hamt [pairs]
  let [t hamt/transient(hamt/new())]
    for-each pairs
      (fn [[k v]]
        hamt/transient-set!(t hamt/hash-str(k) k v))
    hamt/persistent!(t)
```

### Accumulate versions

```turmeric
;; Each update is a new version; retain old ones cheaply
(def v0 (hamt/new))
(def v1 (hamt/set v0 (hamt/hash-str "x") "x" "1"))
(def v2 (hamt/set v1 (hamt/hash-str "y") "y" "2"))

;; v0, v1, and v2 all remain valid; share structure
```
```sweet-exp
;; Each update is a new version; retain old ones cheaply
def v0 hamt/new()
def v1 hamt/set(v0 hamt/hash-str("x") "x" "1")
def v2 hamt/set(v1 hamt/hash-str("y") "y" "2")

;; v0, v1, and v2 all remain valid; share structure
```

### Config overlay

```turmeric
;; Merge user config on top of defaults (user wins)
(def config (hamt/merge defaults user-config))
```
```sweet-exp
;; Merge user config on top of defaults (user wins)
def config hamt/merge(defaults user-config)
```

## See Also

- [C Integration Guide](c-integration-guide.md) -- Passing C function pointers and inline C
- [Threading Guide](threading-guide.md) -- Sharing immutable HAMTs across threads safely
- [STM Guide](stm-guide.md) -- Storing HAMTs inside TVars for concurrent updates
- [src/runtime/hamt.h](https://github.com/turmeric-lang/turmeric/blob/main/src/runtime/hamt.h) -- Full C API with inline documentation
