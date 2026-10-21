# Two identical string literals share an address under cc but not under `tur jit`

**RESOLVED 2026-10-21** -- see [Resolution](#resolution-2026-10-21).

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

## Resolution (2026-10-21)

Fix directions 1 and 3. Direction 2 (one `static const char[]` per distinct
literal text, so the cc behaviour becomes a guarantee) is not taken: plain
Turmeric cannot observe a literal's address -- `=` refuses a `cstr`
(TUR-E0006) -- so the only programs that depend on it reach through inline C
or an erasing ascription, and promising literal identity would rewrite every
emitted string literal (and every snapshot) to support a property the
language never offered.

1. **The example.** `examples/datalog/indexed.tur` keys its EAVT index on an
   interned attribute id: `attr-id` keeps a table of the names it has seen,
   compares by `strcmp`, and returns the same small integer for equal
   contents whatever their address. The asserts and `idb-q-ea` call it where
   they called `cstr->int`. `tur run` and `tur jit` both print `Alice` and
   `25` now. (`tur --interpret` still stops at `db-assert!`'s third argument,
   `the REPL JIT cannot pass this value as a C scalar`, before and after this
   change: a `:heap` value word handed to an inline-C `:int` parameter, not
   this defect.) The other four datalog examples already compared names with
   `strcmp` and agree across engines.
2. **The docs.** jit-guide gains "Two identical string literals are two
   objects" under its permanent constraints, and c-integration-guide's
   pitfalls table a row for keying on a literal's address. datalog-04-indexing
   (the tutorial that walks `indexed.tur`) explains `attr-id` in place of its
   old claim that literals are interned by the C compiler.
3. **The parity check.** `benchmarks/audit/corpus.txt` runs `indexed.tur` on
   `cc,jit` again (it had been cc-only because of this report);
   `run-corpus.py --only datalog/indexed` passes both engines against the
   pinned output.
