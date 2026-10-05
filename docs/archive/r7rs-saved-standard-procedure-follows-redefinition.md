# `#lang r7rs`: a variable holding a standard procedure follows the program's later redefinition of it

**Severity:** high for SICP readers. Chapter 4.1's metacircular evaluator
opens with

```scheme
(define apply-in-underlying-scheme apply)
```

and then defines its own `apply`. On the compiled back end the saved
variable becomes the *new* `apply`, so the evaluator calls itself forever
(a hang, no error). Under `tur --interpret` the program dies with
`unbound variable: apply--user`. Neither back end runs SICP 4.1 as written.
Not specific to `apply`: `square` does the same. Found 2026-10-03 probing
SICP code (docs/archive/r7rs-srfi-18-216-sicp-plan.md).

## Repro

```scheme
#lang r7rs
(import (scheme base) (scheme write))
(define saved apply)
(define (apply f args) 'mine)
(write (eq? saved apply)) (newline)
(write (saved + (list 1 2))) (newline)
```

Expected: `#f`, then `3`.

| Back end | Output |
|---|---|
| compiled (`tur run`) | `#t`, then `mine` |
| `tur --interpret` | `tur: unbound variable: apply--user` |

The same with `square` in place of `apply`: compiled prints `mine` for
`(saved-sq 3)`; interpreted is `unbound variable: square--user`. (Measured on
origin/main b87ca3553.)

## Root cause (partial)

A program's definition of a standard name is respelled `<name>--user`
(src/compiler/scheme_lower.c, around lines 1200-1235). Two consequences, one
per back end:

- **Compiled:** the reference to `apply` on the right of `(define saved
  apply)` appears to be resolved to the program's own `apply--user`, because
  the program defines `apply` *somewhere* in the file, rather than to the
  standard binding that is in effect at that point. Then `saved` and `apply`
  are the same procedure.
- **Interpreter:** the reference is respelled `apply--user` too, but at the
  moment the `define` runs nothing is bound under that name yet, hence
  "unbound variable".

Not traced further. Confirm which reference gets respelled (`--dump-*` on the
lowered program) before fixing.

## What R7RS says, and what SICP needs

R7RS 5.2 makes redefining an imported binding an error in a *library*, but
a program's top level is the place SICP readers work. The behavior SICP
relies on is the R5RS/MIT one: the top-level definition replaces the
variable, and an expression evaluated *earlier* got the value that was there
then. `saved` must hold the original procedure.

## Fix directions

- A top-level `(define name expr)` whose `expr` names a standard procedure
  the program later redefines should capture the standard binding (the
  prelude's `r7rs-apply`, say), not the `--user` respelling. Only references
  that execute after the program's `define` should see the program's version.
  Simplest correct rule: before the redefining `define` in source order, a
  reference means the standard procedure.
- Pin with a fixture on both back ends covering `apply`, `eval` (see
  [r7rs-redefining-eval-with-scheme-eval-fails-to-compile](r7rs-redefining-eval-with-scheme-eval-fails-to-compile.md)),
  and an ordinary name like `square`; then the SICP 4.1 evaluator itself as a
  corpus test.

## Guide upkeep

`docs/guides/sicp-guide.md` documents this defect in its "Rough edges" list
(and the workaround in the chapter it affects). When this is fixed, delete
that entry and any workaround text that only exists because of it.

## Resolution (2026-10-04)

Fixed in the Scheme lowering (`redefinitions_to_set`,
src/compiler/scheme_lower.c), together with
[r7rs-program-redefinition-refused](r7rs-program-redefinition-refused.md).
A program that defines a standard procedure's name now gets what R7RS 5.3.1
and MIT Scheme give: one top-level variable that starts out holding the
standard procedure (a `(define <name> --std--<name>)` inserted after the
program's imports; `rn_global` resolves the `--std--` spelling to the
standard binding), and the program's own definitions of it become `set!`s.
An expression evaluated before the program's `define` -- SICP 4.1's
`(define apply-in-underlying-scheme apply)` -- gets the standard procedure;
one evaluated after gets the program's. The report's repro prints `#f` and
`3` on both back ends. Pinned by `tests/fixtures/r7rs-program-redefinition`
(saved `apply`, redefined `square` and `cons`), on both back ends.
