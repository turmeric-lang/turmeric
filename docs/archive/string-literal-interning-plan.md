# Plan: one object per distinct string literal in a unit

> **Status:** DONE 2026-10-23. Literals are stored once per unit
> (from [cstr-literal-address-differs-by-engine](cstr-literal-address-differs-by-engine.md),
> which this plan resolved). `cstr_codegen_register` / `cstr_codegen_emit`
> live beside `atom_cstr` in `src/compiler/emit_core.c`; the band is written
> first at final assembly in both the program and the implementation-unit
> paths. A literal emitted outside that window (a preamble, a header, after the
> band is written) keeps the inline spelling. Fixtures:
> `cstr-literal-one-object-per-unit` (both harnesses, and `tur jit`) and
> `cstr-literal-embedded-nul-own-object` (compiled).

## Problem

```turmeric
(defn main [] : int
  (println (if (= (:: ":user/name" :int) (:: ":user/name" :int)) 1 0))
  0)
```

`tur run`: 1. `tur jit`: 0. The emitter writes each string literal inline at its
use (`atom_cstr`), so whether two identical literals are one object is up to the
C compiler: gcc merges them within a unit (`-fmerge-constants`), c2mir does not.
An erasing `(:: s :int)` hands that address to an ordinary program.

## Design

Copy what keywords already do. `sym_codegen_register` (`emit_core.c`) gives each
interned symbol one TU-local static record and returns its C name; every use
references it. Add the same for string text:

- `const char *cstr_codegen_register(const char *text, size_t len)` -- a per-unit
  table from literal bytes to `__tur_str_<N>`; the first registration of a text
  emits `static const char __tur_str_<N>[] = "...";` into the unit's literal
  band (where the symbol records go), later ones return the same name.
- `atom_cstr` returns the name instead of the quoted text. It has three callers
  (`emit_expr.c` `EX_CSTR_LIT`, `emit_core.c`, `emit_cps_ir.c`'s `CA_CSTR` atom),
  so every path that emits a literal goes through it.
- Escaping, embedded NULs and the length: key the table on (bytes, length) and
  emit the array with an explicit initializer when the text holds a NUL, so
  `"a\0b"` and `"a"` are different literals.
- Places that need a C string-literal *token*, not an expression -- a
  `static` initializer of another global, `#pragma` text, an inline-C body the
  user wrote -- keep the inline spelling. Audit `atom_cstr`'s callers for these.

The guarantee is per unit. Across units (a `--shared` multi-module build) two
units' literals stay two objects, as they are under gcc without LTO; the
jit-guide note already says a literal's address is not an identity across units.

## Interpreter

`(:: "hi" :int)` interpreted is the string itself, not an address
(type-forms-gaps), and `=` on two such values compares by content, so it already
answers 1. Nothing to change for this plan; whether that ascription should be an
address at all is separate.

## Cost

Every program with a string literal changes its C (157 snapshots), and the
runtime preamble's own literals are not affected (they are C source, not
`atom_cstr`). Regenerate the snapshots in the same change. The emitted-float and
fconv lint scripts read the program half; check they do not key on literal
spelling.

## Steps

1. `cstr_codegen_register` + the literal band; `atom_cstr` through it.
2. Fixture: the program above on `tur run`, `tur jit` and `--interpret`, all 1;
   plus a literal with an embedded NUL beside its prefix.
3. Regenerate snapshots; run the JIT harness (c2mir is the engine this fixes)
   and `benchmarks/audit/run-corpus.py` parity.
4. jit-guide and c-integration-guide: the "two identical literals are two
   objects" notes become "one object per unit, not across units". Archive the
   report.
