# `tur test` marks a sound build.tur malformed after an expected-error test -- RESOLVED

**Severity: high (CI-breaking).** Inside a spice, `tur test` failed every
test file after the first passing `;; tur-test-expect-error:` test with
TUR-E0624 ("build.tur could not be read (see the error above)"), with no
error above. turmeric-spices' `ecs` went from 72/72 to dozens of failures,
which turned that repo's CI red for every PR. Found and fixed 2026-10-17.

## Repro

`tests/cli/tur-test-expect-error-then-pass`: a `build.tur`, a test that must
fail to compile (`;; tur-test-expect-error: TUR-E0001`), then two ordinary
tests. `tur test` printed `.FF` and two TUR-E0624s.

## Root cause

- `tur test` reads each file's manifest (`discover_manifest_reader_macros`)
  before `cmd_build`, whose `diag_reset()` clears the error flag.
- The expected-error test's compile leaves that flag set.
- `pkg_manifest_read_status` (`src/compiler/pkg.c`) judged its own read
  by `diag_had_error()`: after `read_all`, and again after the slot
  parsers. So the next file's read found the flag up and marked a sound
  manifest malformed.
- The malformed verdict is sticky for the process (`pkg_manifest_reassert`,
  added by manifest-reader-error-exits-zero), so every remaining file failed.
  It reports once, which is why the "error above" was missing: there was
  none.

## Resolution

Both checks now compare `diag_error_serial()` across the read, i.e. "did
this read emit an error". Slot parsers report through `diag_emit`, so their
errors still count. Pinned by the CLI case above (`tur_cli_tests`).
turmeric-spices' `ecs` passes 72/72 again.
