# `#lang r7rs`: a program that defines one of its own names twice is refused

**Severity:** high for SICP readers -- found while transcribing SICP 2.1 for
[sicp-corpus](https://github.com/turmeric-lang/sicp-corpus) (plan stage T4).
**Status:** resolved 2026-10-04, the day it was found.

SICP refines procedures by redefining them: 2.1.1's `make-rat` is replaced
by one that reduces to lowest terms, and 2.1.2 replaces it again; 2.2.1
defines `length` twice; 2.3.3 defines `element-of-set?` three times. R7RS
5.3.1 makes that legal at a program's top level: a definition of a variable
that is already bound "has essentially the same effect as the assignment
expression `set!`". Every such program was refused on both back ends:

```scheme
#lang r7rs
(import (scheme base) (scheme write))
(define (make-rat n d) (cons n d))
(define (half) (make-rat 1 2))
(define (make-rat n d) (list 'rat n d))
(write (half))
```

```
error: defn: 'make-rat' is already defined
```

Expected: `(rat 1 2)` -- `half`, defined before the redefinition, calls the
new `make-rat` when it runs after it.

## Root cause

The lowering turned each top-level `(define (f ...) ...)` into its own
`defn`, and the elaborator refuses a second `defn` of one name
(src/compiler/elab_fns.c, "already defined"). The existing corpus file for
SICP 1.1 had worked around it by renaming (`abs2`, `abs3`).

## Fix

`redefinitions_to_set` (src/compiler/scheme_lower.c) runs over a program's
top level before the lowering's scans: a name defined more than once
becomes one variable -- its first definition `(define f (lambda ...))`, every
later one `(set! f ...)` -- which `collect_muts` then treats as any other
`set!` target. A library body is left alone (duplicate definitions there
are an error, R7RS 5.6.1), and so is a REPL turn, which already redefines
through its session state. The same pass fixes
[r7rs-saved-standard-procedure-follows-redefinition](r7rs-saved-standard-procedure-follows-redefinition.md).
Pinned by `tests/fixtures/r7rs-program-redefinition` on both back ends.
