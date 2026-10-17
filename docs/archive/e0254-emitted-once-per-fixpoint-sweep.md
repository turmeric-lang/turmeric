# TUR-E0254 is printed once per effect-inference sweep

**RESOLVED 2026-10-17** -- see [Resolution](#resolution-2026-10-17).

**Severity:** low (diagnostic noise). One mistake is reported several times.

## Repro

```
$ tur check tests/fixtures/errors/effect-row-occurs/input.tur 2>&1 | grep -c TUR-E0254
3
```

A three-deep call chain prints it four times.

## Root cause

`src/passes/effect_check.c:collect_effects_in_expr` emits TUR-E0254 while
walking a body. That walk runs once per sweep of the inference fixpoint, so
the error is printed on every sweep until the fixpoint settles.

## Fix directions

- Record the condition during the fixpoint and emit it once, during the
  validation phase that follows; or
- dedupe by call node.

## Resolution (2026-10-17)

`effect_check.c` records the call sites it has reported in the current pass
(`e0254_first_report`), so a site prints TUR-E0254 once however many sweeps and
later walks reach it: `tur check tests/fixtures/errors/effect-row-occurs/input.tur
2>&1 | grep -c TUR-E0254` is 1. A reported site also makes `effect_check_pass`
return non-zero. It used to return 0: the compiled build stopped one pass
later, at `PASS_CPS`'s `diag_had_error()` check, and `tur --interpret` printed
the error and then ran the program to exit 0 -- which `run-turi.sh` caught once its errors pass started
requiring a non-zero exit
([turi-ignores-effect-row-errors](turi-ignores-effect-row-errors.md)).
`errors/effect-row-occurs` and `errors/effect-poly-escape` now fail under the
interpreter too. No fixture pins the count itself: the needle harnesses match
substrings, not occurrences.
