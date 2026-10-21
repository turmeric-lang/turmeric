# `#lang r7rs`: the prelude split prints wrong symbols on Windows

**Fix landed 2026-09-28; the split stays off on Windows until a Windows run
confirms it.** A split build has no weak data now, and the link-level
diagnosis is confirmed with MinGW-w64's GNU ld (below). What is left is one
run on a Windows host: `TUR_PRELUDE_SPLIT=1 bash tests/run.sh` over the
`r7rs-*` fixtures (MSYS2/UCRT64). Then drop the `!defined(__linux__)`
gate's Windows half in `prelude_split_applies` (src/driver/pipeline.c), and archive this
report.

## Diagnosis, confirmed (2026-09-28, MinGW-w64 13.2 / GNU ld, cross from Linux)

It is not only folding. Take ONE object that defines a weak data record
and reads its `name` field, which is an offset of 16 into the record. GNU ld
resolves that reference to the wrong bytes. `lea` pointed at the record's
start, or, for a second record, at `__dyn_tls_init_callback+0x10`. The
symbol itself stays undefined (`U`) in the executable, beside a per-object
`.weak.__tur_sym_x.<name>` default. Two objects that both define the record
get two defaults, so the records do not fold either. The same record with
`__attribute__((selectany))` (PE's COMDAT) is one definition, and every
reference, at any offset, is right.

Something else turned up. The library unit defines no keyword records at all
(`nm` finds no `__tur_sym_` in the cached prelude object). So on Windows it
was the program unit's weak records, not any sharing between the two units,
that went wrong.

## Fix (2026-09-28)

- **The split has no weak data.** `sym_codegen_emit`
  (src/compiler/emit_core.c): the library unit defines each record it uses
  with strong external linkage and notes it
  (`split_lib_note_sym`, `emit_split_note_export`). The program unit, emitted
  next in the same process, declares those `extern` and keeps a `static`
  record for a keyword only it quotes. The SYM5 seed still registers both
  units' records, and the first registration wins. `tests/check-r7rs-prelude-split.sh`
  now fails on a weak `__tur_sym_` symbol in either unit.
- **Separate compilation (SYM2) uses `selectany` on PE.** The records of
  separately compiled modules (`tur build --shared`, which keeps separate
  compilation) are declared through a `TUR_SYM_LINKAGE` macro:
  `__attribute__((selectany))` under `_WIN32`/`__CYGWIN__`, and
  `__attribute__((weak))` elsewhere, which is unchanged.

---

**Severity:** low while the split is off there, and it is: on Windows,
`prelude_split_applies` (src/driver/pipeline.c) declines unless `TUR_PRELUDE_SPLIT=1`.
Windows builds keep the one-unit build and its 3 s-per-program cost. The
split itself (docs/archive/r7rs-programs-compile-slowly.md) cuts that to
about 1 s everywhere else.

## Repro

MSYS2/UCRT64, from the first CI run of the split (rjungemann/turmeric#952,
`Windows build + suite 1/3`, 20 r7rs fixtures failing):

```sh
TUR_PRELUDE_SPLIT=1 ./build/tur run tests/fixtures/r7rs-type-errors-raise/input.tur
```

It prints `(|uired field| "car: not a pair" (5))` where the fixture expects
`(caught "car: not a pair" (5))`. `r7rs-vector-index-error` prints
`#(ray |!: out of memory|)` for `#(a b)`. Every quoted symbol comes out as the
tail of some unrelated string literal. The two units compile and link
without complaint, so the one-unit fallback (which retries on a cc or link
failure) never fires.

## Root cause (suspected, not confirmed on a Windows box)

Both units define the keyword records they reference, and the library unit
and the program unit share many of them (`'caught`, `'a`, ...).
`sym_codegen_emit` (src/compiler/emit_core.c) gives them
`__attribute__((weak)) const` in a split build, so that the linker folds each
name to one record (SYM2). ELF and Mach-O do fold them, and every symbol then
has one address. On PE/COFF, GNU ld implements a weak definition as a weak
external plus a default. A reference that should reach the folded record
lands on bytes elsewhere in `.rdata`, at an offset into another object's
string literals. That is what the output looks like: `uired field` is the
middle of `"required field"`.

## Fix directions

- **Stop relying on weak data in the split.** Fat boxes already take this
  shape. The library unit defines each keyword record it uses, with strong
  external linkage, and records its name. The program unit emits `extern`
  for those names and `static` records for keywords only it uses. The SYM5
  intern-table seed is unchanged, since first registration wins. This works
  on every platform. It also takes away the only reason the split needs
  weak symbols.
- Confirm the diagnosis first on a Windows host: `nm` both objects and the
  linked executable, and check whether `__tur_sym_*` resolves to one address.
