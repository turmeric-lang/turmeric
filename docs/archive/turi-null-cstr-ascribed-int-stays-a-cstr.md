# `tur --interpret`: `(:: s int)` on a `cstr` keeps it a `cstr`, so a null check against 0 is always false

**Severity:** medium (silent wrong value, interpreter only). Under
`tur --interpret`, ascribing a `cstr` to `:int` does nothing. The value keeps
its string tag, so `(= (:: s int) 0)` is `false` even when `s` is null, and
`(println (:: s int))` prints an empty line where the compiled program prints
`0`. The compiled program reinterprets the pointer as a word, as the
ascription says. The stdlib uses this exact idiom as its null check:
`re/compile-union` tests `(= (:: pat int) 0)`.

Found 2026-10-17 while closing the inline-C JIT's `cstr` ownership gap
([aot-compiled-repl-plan](../upcoming/aot-compiled-repl-plan.md) C1, the value
boundary's check 3). With that fixed, `tests/fixtures/re-union-patterns` runs
to the end under `--interpret`, and this is its one wrong line: an empty
alternation should be the never-match regex (`empty-no`), but the null check
misses, so `re/compile` gets a null pattern and the result matches anything
(`empty-yes`). It reproduces with the JIT off and with no inline-C at all.

## Repro

```turmeric
(defn mk [] : cstr (:: 0 cstr))
(defn main [] : int
  (let [p (mk)
        q (:: 0 cstr)]
    (println (= (:: p int) 0))
    (println (= (:: q int) 0))
    (println (:: p int)))
  0)
```

```
$ tur run repro.tur          # compiled
true
true
0
$ tur --interpret repro.tur
false
false

```

## Root cause

`eval_unary_post`'s ascription arm (`src/turi/eval.c:8793`, `case TY_INT:
case TY_INT64:`) converts only a `TURI_FLOAT`. Every other tag is returned
unchanged, so a `TURI_CSTR` stays one. The equality that follows then compares
a string with the integer 0 and answers `false`. The reverse ascription, `:int`
to `:cstr`, does re-tag (`turi_cstr_from_carrier`, the `TY_CSTR` case just
below), so the two directions are not symmetric.

## Fix directions

1. Re-tag a NULL `cstr` ascribed to an int-class type as `turi_int(0)`. That
   fixes the null-check idiom without changing how a non-null string travels.
2. A full re-tag (every `cstr` ascribed to `:int` becomes a `TURI_INT` holding
   the address) matches the compiled semantics, but coordinate it with the
   inline-C JIT's value boundary first. Today `re.tur`'s
   `(str-free (:: s int))` reaches compiled `str-free` as a `cstr`, which the
   boundary copies (check 3). As a `TURI_INT` holding an arena address, check
   2 would refuse it instead, and `re-union-patterns` would fail again, this
   time cleanly.

Pin it with the repro above as a fixture with no inline-C, so
`tests/run-turi.sh` runs it.

## Resolution (2026-10-18)

The first fix direction. `eval_unary_post` (`src/turi/eval.c`) re-tags a NULL
`cstr` ascribed to an int-class type (`int`, `int64`, and the narrow ints) as
`turi_int(0)`, in the `EX_ASCRIBE` arm and the `EX_REINTERPRET` arm both. The
null check `(= (:: s int) 0)` answers `true` and `(println (:: s int))` prints
`0`, as compiled does. `re-union-patterns` under `tur --interpret` now prints
`empty-no` (before: `empty-yes`) and matches its `expected.stdout` end to end.

A non-null `cstr` still keeps its string tag through `(:: s int)`. That is the
second fix direction, deliberately not taken: as a `TURI_INT` holding an arena
address it would be refused by the inline-C JIT's value boundary (check 2)
where today it is copied (check 3), and `re.tur`'s `(str-free (:: s int))`
depends on the copy. The difference only shows when a program prints or
compares the address itself.

Pinned by the `turi-null-cstr-ascribed-int` fixture (no inline-C, so
`tests/run-turi.sh` runs it as well as `run.sh`).
