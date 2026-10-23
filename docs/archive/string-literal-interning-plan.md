# Plan: one object per distinct string literal in a unit

> **Status:** DONE 2026-10-23. Decision taken 2026-10-21: store literals once per
> unit (from [cstr-literal-address-differs-by-engine](cstr-literal-address-differs-by-engine.md),
> which this plan resolved). See [Outcome](#outcome-2026-10-23).

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

## Outcome (2026-10-23)

Done as designed, with three refinements found while building it:

- **The band is spliced in last.** `cstr_codegen_splice` writes it at an offset
  recorded right after the runtime preamble (ahead of every band that can name
  a literal), but only once the whole unit is written, so a literal registered
  by a late emitter -- main's body, the CPS group flush -- is in it too. Both
  unit emitters (`emit_program`, `emit_implementation`) reset the table beside
  `sym_codegen_reset` and splice at their end.
- **No explicit initializer for a NUL.** `emit_c_string` already spells a NUL
  as `\000`, so `static const char __tur_str_<N>[] = "a\000b";` holds every
  byte plus the terminator; keying on (bytes, length) is what keeps it apart
  from `"a"`.
- **`__attribute__((unused))` on each record.** A body emitted speculatively and
  discarded can leave a record nothing names, which `-Wall`'s
  `-Wunused-const-variable` would report.

The audit of call sites found nothing that needs a literal *token*: `atom_cstr`
has two callers (`EX_CSTR_LIT`, the CPS `CA_STR` atom), and the name is an
address constant wherever the inline literal was, static initializers included.
The other `emit_c_string` callers -- a string `match` pattern's `strcmp`
operand, panic/diagnostic text, the keyword records' names -- compare or print
contents and stay inline.

Pinned by `cstr-literal-one-object-per-unit` (the repro, plus a literal from
another function and the empty string; 1 on `tur run`, `tur jit` and
`--interpret`) and `cstr-literal-nul-is-distinct` (`requires.compiled`:
interpreted, the ascription is the string and `=` stops at the NUL). 158
snapshots regenerated. `tests/run.sh` and `tests/run-jit.sh` (all three
shards) report no failures; the multi-unit harnesses (`run-build-project`,
`run-build-shared`, `run-any-type-id-multi-module`, the r7rs split checks)
pass. The emitted-float and fconv lints key on the preamble marker and the
inline-C ranges, not on literal spelling.
