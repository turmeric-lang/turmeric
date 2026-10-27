# `expected.diag` in a happy-path fixture is never read

**Severity:** low-medium (test rot: two pinned warnings have already drifted
unnoticed). Found 2026-10-27 while adding `refine-instance-variance-unknown`.

## Summary

`tests/run.sh` reads `expected.diag` only on the `errors/` path
(`tests/run.sh:1267`). A happy-path fixture pins compile warnings with
`expected.stderr` (`tests/run.sh:1140`), which is substring-matched against the
combined stderr. A happy-path fixture that carries `expected.diag` instead
passes whatever the compiler prints, and nothing says so.

## Repro

```sh
echo "NOT PRESENT XYZ" >> tests/fixtures/refine-class-param-inheritance/expected.diag
TUR_TEST_FILTER='^refine-class-param-inheritance$' timeout 720 bash tests/run.sh
# PASS refine-class-param-inheritance
git checkout tests/fixtures/refine-class-param-inheritance/expected.diag
```

## The five affected fixtures (2026-10-27)

| Fixture | Do its `expected.diag` lines appear in stderr today? |
|---|---|
| `refine-class-param-inheritance` | yes |
| `refine-stateful-nonstrict-warns` | yes |
| `warn-image-global-unregistered` | yes |
| `gadt-struct-namespace-prefer` | **no** -- the warning was reworded to "defdata 'Stack' supersedes the same-named struct 'Stack'" |
| `warn-no-entry-point-near-miss` | **no** -- `TUR-W0624` no longer fires at all: `tur build`/`tur run` of the input print nothing. The emitter warns only when the synthesized `main` has no body (`!body.len`, `src/compiler/emit_module.c:22964`), so something now always contributes to `body`. Not diagnosed further |

## Fix directions

1. Rename the three that hold to `expected.stderr`.
2. `gadt-struct-namespace-prefer`: update the pin to the current wording, then
   rename.
3. `warn-no-entry-point-near-miss`: find what fills `body` for an empty
   program and restore the warning (a real regression), then rename.
4. Make the mistake loud: have `run.sh` FAIL a happy-path fixture that carries
   `expected.diag` (or `unexpected.diag`), naming `expected.stderr` as the file
   it wanted.
