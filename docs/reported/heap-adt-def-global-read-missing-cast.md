# A top-level `def` of a `:heap` value is read into a typed slot without a cast

**Severity: medium** (compile failure under clang; gcc only warns, so Linux CI
passes and macOS fails).

**Status: OPEN.** Found 2026-10-10 on `tur` v0.64.0, sharing one
`ParseErrorKind` node per kind in turmeric-spices `dom-core` so a failed parse
would stop allocating one. Worked around there: the nodes are private `def`s
behind exported zero-argument functions (`(limit-kind)`), and a function's
return path casts correctly.

## Repro

```turmeric
(defdata K :heap :copy (A) (B))
(def a-node (A))
(def b-node (B))

(defn pick [i : int] : K
  (if (= i 1) b-node a-node))

(defn main [] : int
  (println (match (pick 1) (A) "a" (B) "b"))
  0)
```

```
$ CC=clang tur build heapdef.tur
error: incompatible integer to pointer conversion assigning to 'tur_adt_K *'
       (aka 'struct tur_adt_K *') from 'int64_t' (aka 'long') [-Wint-conversion]
```

gcc prints the same thing as a `-Wint-conversion` warning and the program
prints `b`.

## Root cause

The global is declared with the carrier spelling, and the `if` merge temp
with the typed one. The arm assignment reads the global as-is:

```c
static int64_t a_hynode_7;
...
static int64_t pick(int64_t i) {
        tur_adt_K * __t175;
        if ((i) == (INT64_C(1))) {
            __t175 = b_hynode_8;        /* int64_t -> tur_adt_K *, no cast */
        } else {
            __t175 = a_hynode_7;
        }
        return (int64_t)(intptr_t)__t175;
}
```

An ascription does not help. `(if (= i 1) (:: b-node K) (:: a-node K))`
emits the same assignment. Binding the global first
(`(let [a a-node b b-node] (if (= i 1) b a))`) or reading it through a
function (`(defn __a [] : K a-node)`) both compile.

## Fix directions

1. Bridge at the read: an `EX_VAR` of a global whose recorded C spelling is
   `int64_t` and whose type is a `:heap` ADT should be cast to the typed
   pointer wherever the destination is typed. The `let` path already does
   this, which is why binding first works.
2. Or declare the global with the typed spelling (`tur_adt_K *`) so the
   carrier never appears. Check the initializer's
   `(int64_t)(intptr_t)` store and every cross-module `extern` of the global
   agree on the new spelling.
