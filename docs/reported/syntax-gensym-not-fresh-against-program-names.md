# `syntax-gensym` in a `defmacro*` can return a name the program already uses

**Severity: medium-low (silent variable capture in a procedural macro; a workaround exists).**

Found 2026-10-28 while rewriting the `P-of` macros of `stdlib/numvec.tur`
([simd-and-vectorization-plan](../upcoming/simd-and-vectorization-plan.md) N0).

The template `(gensym "tmp")` skips every name the reader has interned, so a
hand-written `tmp_0` in the program is never handed out (macros-guide, "manual
hygiene"). `syntax-gensym`, its `defmacro*` counterpart, promises the same
contract, but it checks only the macro-time env's own symbol table, which
has not seen the program. So it hands out `tmp_0` even when the call site
binds `tmp_0`, and the expansion captures it. There is no diagnostic; the
answer is just wrong.

## Repro

```turmeric
(defmacro* with-ten [e]
  (let [t (syntax-gensym "tmp")]
    `(let [~t 10] (+ ~t ~e))))

(defn main [] : int
  (let [tmp_0 1]
    (println (with-ten tmp_0)))   ; prints 20, expected 11
  0)
```

`tur expand` shows `(let [tmp_0 10] (+ tmp_0 tmp_0))`.

## Root cause

`native_syntax_gensym` (`src/turi/interpreter_natives.c`) loops on
`symtab_contains(&e->st, ...)`, where `e` is the macro-time env. The program
being expanded interns its symbols in the compiler's own table, which that
check never consults; the counter is also a function-static, unrelated to the
elaborator's `gensym_fresh`.

## Fix directions

- Have the macro-time env check (or share) the elaborating program's symbol
  table, as `gensym_fresh` does.
- Or route `syntax-gensym` through `gensym_fresh` when the call comes from an
  expansion.

## Workaround in tree

`stdlib/numvec.tur` mints the binder in a template `defmacro` with `(gensym)`
and passes it to its `defmacro*` (`numvec-of__`) as an argument.
