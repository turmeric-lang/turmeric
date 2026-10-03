# `#lang r7rs`: defining `eval` in a program that imports `(scheme eval)` fails to compile

**Severity:** medium. The compiled back end emits two C functions with the
same name and `cc` rejects the unit; the user sees a C compiler error, not a
Scheme one. SICP 4.1 defines its own `eval`, and a reader who has added
`(scheme eval)` to their imports (to compare against the real one, say)
hits this. `tur --interpret` is fine. Found 2026-10-03 probing SICP code
(docs/upcoming/r7rs-srfi-18-216-sicp-plan.md).

## Repro

```scheme
#lang r7rs
(import (scheme base) (scheme write) (scheme eval))
(define (eval e env) 'mine)
(write (eval 5 0))
(newline)
```

```
$ tur run ev.tur
.../tur-build/..._ev_tur.c:13069:21: error: redefinition of 'r7rs_hyeval'
1 error generated.
tur: cc invocation failed (status 256)
```

Expected: `mine` (which `tur --interpret` prints). Without the `(scheme
eval)` import the compiled program prints `mine` too. (Measured on
origin/main b87ca3553.)

## Root cause (not traced)

Two definitions mangle to `r7rs_hyeval`: one comes from the `(scheme eval)`
support in stdlib/r7rs/eval.tur, and the other is the program's `eval`. The
program's definition was evidently not respelled `eval--user` (the
mechanism at src/compiler/scheme_lower.c:1200-1235 that keeps a program's
standard names apart from the library's) when `(scheme eval)` is imported.
Start by checking how the `(scheme eval)` import registers `eval` in the
rename tables, versus `(scheme base)` names.

## Fix directions

- Respell the program's `eval` like every other standard name it redefines.
- A C-level redefinition should never reach `cc`: if the emitter can see a
  duplicate top-level C name, report it as a Turmeric diagnostic naming the
  Scheme identifier.
- Related: [r7rs-saved-standard-procedure-follows-redefinition](r7rs-saved-standard-procedure-follows-redefinition.md);
  fix and pin both with the same SICP 4.1 fixture.

## Guide upkeep

`docs/guides/sicp-guide.md` documents this defect in its "Rough edges" list
(and the workaround in the chapter it affects). When this is fixed, delete
that entry and any workaround text that only exists because of it.
