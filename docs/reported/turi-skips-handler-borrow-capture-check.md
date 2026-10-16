# `tur --interpret` skips the effect-handler borrow-capture check

**Severity:** low-medium. A program the compiler rejects runs under the
interpreter, and the harness that should catch it cannot
([needle-files-without-trailing-newline-assert-nothing](needle-files-without-trailing-newline-assert-nothing.md)).

## Repro

```
$ tur check tests/fixtures/errors/effect-handler-borrow/input.tur
... borrow 'r' cannot be captured by an effect handler case
$ tur --interpret tests/fixtures/errors/effect-handler-borrow/input.tur
42
$ echo $?
0
```

## Root cause

The compiled pipeline's borrow pass runs `borrow_check_program`,
`borrow_check_effect_handler_captures` and `lifetime_check_program`
(`src/main.c`). The interpreter's turn (`turi_eval_impl`, `src/turi/eval.c`)
calls `lifetime_check_program` but never
`borrow_check_effect_handler_captures`.

## Fix directions

- Call `borrow_check_effect_handler_captures` next to
  `lifetime_check_program` in `turi_eval_impl`, and stop on its error.
- Fix the fixture's `expected.diag` trailing newline so `run-turi.sh` pins it.
