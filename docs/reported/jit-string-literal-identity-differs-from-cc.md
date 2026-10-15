# Two identical string literals share an address under cc but not under `tur jit`

**Severity:** low-medium (engine divergence; silent wrong answer). Found
2026-10-14 by the performance audit's WP0 output-parity check
([performance-audit-plan](../archive/performance-audit-plan.md)), which
refuses to time a program whose engines disagree.

## Repro

```turmeric
(defn cstr->int [s : cstr] : int
  ```c return (int64_t)(intptr_t)s; ```)

(defn main [] :int
  (println (if (= (cstr->int ":user/name") (cstr->int ":user/name")) 1 0))
  0)
```

Release `tur` v0.65.1, Ubuntu 24.04, gcc 13.3: `tur build` + run prints `1`;
`tur jit` prints `0`.

The visible casualty is `examples/datalog/indexed.tur`. Its EAVT index keys
attributes by `(cstr->int ":user/name")`, so a lookup finds the fact only if
the literal at the query site has the same address as the one at the assert
site. Compiled, both `idb-q-ea` queries answer (`Alice`, `25`); under
`tur jit` both come back empty, with exit 0 and no diagnostic.

## Root cause

C leaves it unspecified whether identical string literals are distinct
objects. gcc merges them within a translation unit (`-fmerge-constants`, on at
`-O2`), so the cc path happens to give them one address. c2mir, which the JIT
hands the same emitted C, gives each occurrence its own storage. Neither is
wrong as C. The program depends on an accident of the cc path.

## Fix directions

1. **The example:** key the index by string contents (the file already has
   `cstr-eq?`) or intern the attribute names once into a table, instead of
   comparing literal addresses.
2. **The language, if literal identity is meant to hold:** have the emitter
   give each distinct literal text one `static const char[]` per unit and
   reference it everywhere, so every engine sees one address. That makes the
   cc behaviour a guarantee instead of a gcc default.
3. **At least:** say in the inline-C guide that a literal's address is not a
   stable identity across engines, and have the corpus parity check (the
   audit's `run-corpus.py`) keep catching it.
