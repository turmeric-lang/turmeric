# A top-level `def` of a `:heap` value is read into a typed slot without a cast

**Severity: medium** (compile failure under clang; gcc only warns, so Linux CI
passes and macOS fails).

**Status: RESOLVED 2026-10-11** (see [Resolution](#resolution)). Found
2026-10-10 on `tur` v0.64.0, sharing one
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

## Resolution

Fix direction 1, at the one table every arm bridge already reads. The arm
bridges (`if_arm_word_ptr_cast` for `if`, `emit_merge_assign_bridged` for
`match`) ask `emit_localvar_lookup_ctype` what C type an arm's identifier was
emitted as, and cast word <-> pointer when it disagrees with the merge temp. A
local is recorded when it is declared; a global never was, so the lookup
answered "unknown" and the bridge left the assignment alone.

`emit_global_def_forward_decls` (`src/compiler/emit_module.c`) -- which runs in
both `emit_program` and the per-module path, ahead of every function body --
now records each `def`'s declared spelling (`type_c_name` of its type, the same
string its declaration prints) under its C name. Thread-locals (read through a
getter) and thin function-pointer globals (their own declaration form) are
skipped. With the entry in place the arm becomes
`__t = (tur_adt_K *)(intptr_t)(b_hynode_8);`.

Covered: `if` arms, ascribed arms (`(:: b-node K)`), `match` arms, `cond`, a
`let`-bound `if` of globals, and the same reads of another module's exported
`def`s. **Pinned by** `heap-adt-def-global-merge-arm` and
`heap-adt-def-global-merge-arm-cross-module` (on the old compiler gcc warns
`-Wint-conversion` 9 and 2 times, which the run.sh ratchet fails, and clang
refuses to compile either).
