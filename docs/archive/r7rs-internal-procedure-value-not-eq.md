# `#lang r7rs`: an internal procedure used as a value is not `eq?` to itself (compiled)

**Severity:** high for SICP readers -- found transcribing SICP 3.3.5 for
[sicp-corpus](https://github.com/turmeric-lang/sicp-corpus) (plan stage T4):
the constraint system's connector skips the constraint that set it with
`eq?`, so a `constant` was told its own value and stopped with "Unknown
request: CONSTANT". **Status:** resolved 2026-10-04, the day it was found.

## Repro

```scheme
#lang r7rs
(import (scheme base) (scheme write))
(define (twice)
  (define (me r) r)
  (list me me))
(define l (twice))
(write (eq? (car l) (cadr l)))
```

Expected `#t` (R7RS 5.3.2: an internal define binds one location; both
references read it). Compiled printed `#f`; `tur --interpret` was right.

## Root cause

`lower_body_inner` (src/compiler/scheme_lower.c) lowers a body's procedure
defines to one Turmeric `letrec` of functions. A function used as a value is
converted to a closure object at each use, so every reference was a fresh
object, and `eq?` (identity of the carrier word) saw two procedures.

## Fix

An internal procedure the body uses as a value -- anywhere but at the head
of a call (`form_uses_as_value`) -- is bound the way a value define is: once,
as an `any`, through the existing hoisted-cell path when an earlier
definition mentions it. Every reference then reads the one object. A
procedure that is only ever called stays a letrec function, so ordinary
helpers keep their direct calls. Pinned by
`tests/fixtures/r7rs-internal-procedure-identity` on both back ends (main
prints `#f #f` for its first two lines).
