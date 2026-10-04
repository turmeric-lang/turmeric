# `#lang r7rs`: `apply` of a variadic procedure refuses a list longer than eight

**Narrowed 2026-10-04: the standard variadic procedures are fixed** (fix
direction 2). `r7rs-apply-list__` hands a list of more than eight to
`r7rs-apply-std-many__` (stdlib/r7rs/prelude.tur). That recognises `+`, `*`,
`-`, `/`, `max`, `min`, `=`, `<`, `>`, `<=`, `>=`, `list`, `append`,
`string-append`, `vector` and `string` by identity, and computes each from the
list the way its own body computes it from its rest chain. The folds are
self tail calls, so `(apply + (iota 1000000))` costs no stack. Both back ends,
pinned by `tests/fixtures/r7rs-apply-standard-variadic-many`, which also pins
inexact contagion through `max`/`min`. **Still open:** a program's own
`(define (f . xs) ...)` applied to more than eight. It now fails with a
message that says so. That case needs fix direction 1: a primitive that reads
a closure's fixed count (`__tur_dyn_variadic_fixed`) and calls it with a
pre-built rest chain, plus its interpreter twin.

**Severity:** low (was medium). `(apply + lst)`, `(apply max lst)`, `(apply append
lists)` and `(apply string-append strs)` are everyday Scheme, and SICP code
(and its readers' exercise answers) use them on lists of any length. All of
these callees are variadic and have no arity ceiling, yet `apply` panics
as soon as the list has nine elements. Both back ends. Found 2026-10-03
probing SICP code (docs/upcoming/r7rs-srfi-18-216-sicp-plan.md).

This is not [r7rs-apply-more-than-four-arguments](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/r7rs-apply-more-than-four-arguments.md)
reopened. That report raised the dynamic-call ceiling to eight on purpose,
for *fixed*-arity procedures. The defect here is that the ceiling is also
applied to *variadic* procedures, which the dynamic call already handles by
packing the surplus into the rest list.

## Repro

```scheme
#lang r7rs
(import (scheme base) (scheme write))
(write (apply + (list 1 2 3 4 5 6 7 8 9 10)))
(newline)
```

```
$ tur run apply.tur          # and tur --interpret apply.tur
error: apply: more than 8 arguments is not supported; pass the rest as a list
```

Expected: `55`. (Measured on origin/main b87ca3553, compiled and interpreted.)

## Root cause

`r7rs-apply-list__` (stdlib/r7rs/prelude.tur, near line 1759) dispatches on
the list's length with one arm per count from 0 to 8, then fails. It never
asks whether `f` is variadic.

## Fix directions

- For a variadic `f` with `k` fixed parameters (`k` <= 8), call it with the
  first `k` elements and hand the remaining list over as the rest argument
  directly, without spreading it. The dynamic call already packs surplus
  arguments into the `(Cons any)` rest chain (r7rs-compiled-dynamic-shapes
  R6), so the representation exists; what is missing is a way for the prelude
  to see `f`'s fixed count and pass a pre-built rest.
- Short of that: special-case the standard variadic procedures in `apply`
  (`+`, `*`, `-`, `/`, `max`, `min`, `append`, `list`, `string-append`,
  `vector`, `string`) by folding over the list. That covers most real usage
  but not a user's own `(define (f . xs) ...)`.

Pin with a fixture on both back ends: `(apply + (iota 100))`, `(apply max
...)`, `(apply append ...)`, and a user-defined `(define (sum . xs) ...)`
applied to 20 elements.

## Guide upkeep

`docs/guides/sicp-guide.md` documents this defect in its "Rough edges" list
(and the workaround in the chapter it affects). When this is fixed, delete
that entry and any workaround text that only exists because of it.
