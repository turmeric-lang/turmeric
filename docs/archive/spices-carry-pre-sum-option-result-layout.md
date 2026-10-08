# (spice repo) Spices still speak the pre-sum Option/Result layout

**RESOLVED 2026-08-27** in `rjungemann/turmeric-spices` --
"migrate every spice off the pre-sum Option/Result layout"
(branch `claude/sum-option-result-layout`).  See
[Resolution](#resolution) at the bottom.

**Severity: high for the spice repo (46 files across 12 spices), invisible to
this repo's CI.**  Filed 2026-08-27 at the SR2b/SR3 landing, from a read-only
sweep of `rjungemann/turmeric-spices` at its current head.

## Summary

SR2b converted stdlib `Option`/`Result` from discriminated records to real
sums with the tagged layout `{ int tag; union { ... } as; }` (tag 0 =
None/Ok, payload at offset 8).  The in-tree stdlib, fixtures, preamble
helpers, and interpreter were migrated in lockstep -- but the spices repo was
not, and it carries both failure modes:

- **126 inline-C sites in 34 files hand-roll the retired layouts**
  (`struct { bool is_ok; int64_t ok_val; int64_t err_val; }` /
  `{ bool is_some; int64_t value; }`).  By file count: tourist 14,
  notebook 10, httpd 7, tidal 5, tourist-session-valkey 2, plutovg 2,
  http 2, scscm 1, regex 1, postgres 1, plot 1.  These are **silent wrong
  bytes**, not compile errors: the old `is_ok = true` byte reads back
  through the new layout as `tag == 1` -- which is **Err** -- and the old
  24-byte Result's `err_val` sits at offset 16 where the new 16-byte union
  puts nothing.  An ok result built the old way reads as an error carrying
  garbage.
- **35 record-accessor reads** (`(.is-some x)` / `(.value x)` /
  `(.ok-val r)` / `(.err-val r)`) in tourist-session (27), regex (4),
  json (3), tourist-session-valkey (1).  These fields do not exist on the
  sum, so they are **clean elaboration errors** -- loud, at least.

Only 7 files already go through the preamble helpers and need nothing.

## Why this repo's CI cannot see it

The `requires.spices` fixtures auto-skip when the sibling
`../turmeric-spices/` checkout is absent, which it is on the CI runners that
validated SR2b.  A green turmeric suite says nothing about the spices.

## Fix directions

The migration is mechanical and has a complete worked example: turmeric
commit `5acef8b0` ("sr2b: convert stdlib Option/Result to real defdata
sums") migrated the same two patterns across the stdlib and ~40 fixtures.

- Hand-rolled structs: replace with the preamble helpers (`tur_box_ok` /
  `tur_box_err` / `tur_is_ok` / `tur_ok_value` / `tur_err_value`,
  `tur_box_some` / `tur_is_some` / `tur_opt_value`, `TUR_NONE`), which carry
  the canonical layout and a `_Static_assert` pinning it.  Where a helper
  does not fit, the layout to spell is `{ int tag; int64_t payload }` with
  tag 0 = Ok/None -- see
  `docs/guides/inline-c-results-guide.md`.
- Accessor reads: replace with `match` on the constructors, or the stdlib
  accessors (`some?` / `unwrap` / `ok?` / `ok-val` / `err-val`).
- `(none)` note: since SR3 slice A the carrier None IS the null pointer
  again, so spice code that already treated 0 as none (the pre-SR2b
  convention) is correct on the read side without changes.

## Verification

Clone the spices next to this repo
(`git clone https://github.com/turmeric-lang/turmeric-spices ../turmeric-spices`)
and run `bash tests/run.sh` so the `requires.spices` fixtures stop skipping;
then each spice's own test suite.

## Resolution

Both failure modes are migrated across the spices repo; nothing in this repo
needed to change.

**The corruption was real and was reproduced before fixing.** Building a
smoke program against the unmigrated `test/assert` with a current `tur`:
`assert-ok` on a genuine `(ok 7)` printed *"expected ok, got err"*,
`assert-err` on `(err 41)` printed the inverse, and `result-err` on that
same err returned `7` -- reading offset 16, past the end of the 16-byte
box. After the migration the same program prints `ok-ok / err-err /
some-some / none-none / 7 / 41`.

- **131 inline-C sites in 47 files** now build and inspect through the
  emitted preamble (`tur_box_ok` / `tur_err_ptr` / `tur_ok_int` /
  `tur_is_ok` / `tur_ok_value` / `tur_err_value`, `tur_some_int` /
  `tur_is_some` / `tur_opt_value` / `TUR_NONE`). No hand-rolled
  `{ bool is_ok; ... }` / `{ bool is_some; ... }` struct remains.
- **43 record-accessor reads** (`.is-ok` / `.ok-val` / `.err-val` /
  `.is-some` / `.value`) became the stdlib accessors `ok?` / `ok-val` /
  `err-val` / `some?` / `unwrap`.
- **Four `(struct tur__option__Option *)` casts** in the tourist fixtures
  also had to go: `(Option Response)` lowers to the `int64_t` carrier, so
  the monomorph struct-pointer spelling no longer compiles.

`plot/core.tur` was the sharpest case -- half migrated. Its `__ok?` /
`__ok-val` readers already went through the preamble while
`__surface-create`, `__canvas-create`, `__surface-write-png` and both
`RETURN_OK`/`RETURN_ERR` macro pairs still *wrote* the retired struct, so
the module was misreading its own writes.

**Measured:** every spice suite that runs without external C deps, before
and after -- 102 failing test files -> 75, **zero regressions**, 27 fixed
(all 22 `json` suites, 6 of 10 `tourist-session`). The remaining 75 fail
identically on an unmodified tree for unrelated reasons: missing cmake
deps, a pre-existing `cstr`-return `-Wint-conversion` in four tourist
fixtures, and mbedTLS headers being `#include`d inside a function body in
`http/client.tur` (which only trips when a sibling checkout has them
fetched). Those three are separate defects, not part of this migration.
