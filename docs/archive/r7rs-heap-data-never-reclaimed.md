# `#lang r7rs`: a Scheme program never frees its data

**RESOLVED 2026-09-25 (compiled back end).** The r7rs-gc collector graduated
([r7rs-gc-plan](r7rs-gc-plan.md)): it is the allocator of every compiled
single-unit `#lang r7rs` program on Linux and macOS, so what this report
describes is garbage the next collection reclaims. `TUR_R7RS_GC=0` or
`--no-r7rs-gc` builds without it (a program that starts threads). The
interpreter keeps its values for the life of the process by design
(gc-guide). Original report follows.


**Severity:** medium. By design today, and the largest memory finding of
r7rs-lang-plan T8's audit: every pair, vector, string, bytevector, record,
promise, parameter, box, bignum, ratio, complex number and procedure a
Scheme program allocates stays allocated until the process exits. A
long-running Scheme program's memory only grows.

## Repro

```scheme
#lang r7rs
(import (scheme base) (scheme write))
(define (churn i)
  (if (= i 0) 'done
      (begin (list i i i i) (churn (- i 1)))))
(write (churn 1000000))
(newline)
```

The lists are dead the moment they are built. Peak RSS of the compiled
program (default `-O2`), measured 2026-09-25:

| iterations | peak RSS |
|---|---|
| 100,000 | 45 MB |
| 1,000,000 | 429 MB |

About 430 bytes an iteration: four pairs, the `list` call's rest-argument
chain and the list built from it. Compiled with `-fsanitize=address` and run
with LeakSanitizer on, every one of them is reported. Every `r7rs-*` fixture
leaks the same way (T8's audit: 0.5 KB to 8 MB each, on both back ends).

## Root cause

This is the memory model, not a missed free. The Scheme types are `:heap`
structs in stdlib/r7rs/prelude.tur (`R7rsPair`, `R7rsString`, ...), and
docs/guides/gc-guide.md is explicit that "a bare non-`rc<T>` box is never
auto-freed on *any* path". Typed Turmeric has the same property -- a
`defstruct ... :heap` value built and dropped in a loop leaks too -- but a
Turmeric program chooses `rc<T>`, regions or the substructural types when it
cares. A Scheme program has no way to say any of that: its values are shared,
mutable and cyclic, which is the case only a tracing collector (or RC plus
the cycle collector) handles.

The interpreter (`tur --interpret`) keeps its values for the life of the
process by design as well (gc-guide, "Closures are never freed
individually").

## What T8 did instead

- Fixed the SCRATCH leaks, which are real bugs under any model: memory the
  prelude made for one call and nobody could reach afterwards (the printer's
  spellings, `quotient`'s message, the bignum core's temporaries, the string
  re-encodings, `equal?`'s tables). See the T8 note in
  docs/archive/r7rs-lang-plan.md for the table of what is freed.
- Gated memory SAFETY rather than leaks: `tests/run-r7rs-sanitize.sh`
  (ctest `tur_r7rs_sanitize`) runs every Scheme fixture under ASan and UBSan
  with leak detection off, because of this report.

## The experiment (`--enable=r7rs-gc`)

A conservative mark-sweep collector now exists as an experiment,
[docs/archive/r7rs-gc-plan.md](r7rs-gc-plan.md): built with
`--enable=r7rs-gc`, a compiled `#lang r7rs` program allocates everything
through it, and the repro above peaks at 10 MB in 0.23 s (from 429 MB,
0.39 s). `tests/run-r7rs-gc.sh` (ctest `tur_r7rs_gc`) runs every Scheme
fixture under it with frequent collections, and checks that the repro fits
in 256 MiB with it and not without. This report stays open until the
collector graduates: it is compiled only, single-threaded (a thread start
under the flag is refused with the reason), Linux/glibc and macOS (the
macOS roots ran green on CI's `macos-latest` leg, 105 s), and it does not
scan memory libc or the backtracking trail allocate (the plan's Limits). Since the second pass
(2026-09-25) the runtime archive allocates through it, so a Scheme value
kept in a Turmeric map or `rc<T>` cell is seen.

## Fix directions

- Allocate the Scheme types through the RC runtime (`rc<T>` layout,
  `RCK_*` walkers per struct) and turn the Bacon-Rajan cycle collector on for
  `#lang r7rs` programs. The dynamic substrate's `any` words would need
  retain/release at every copy, which is the cost.
- Or a conservative tracing collector for the Scheme heap only (Boehm-style) --
  the direction the experiment above took --
  with the prelude's inline C allocating through it. The T5 continuation
  images and the DK frames are then roots, which also retires
  [r7rs-callcc-memory-never-freed](r7rs-callcc-memory-never-freed.md).
- Either way the fixture gate can then check leaks again: opt the `r7rs-*`
  fixtures into `tests/run-leak-check.sh` (`requires.leak-check`).

## Guide upkeep

`docs/guides/r7rs-guide.md` ("Where it differs from R7RS") carries one bullet,
beginning "**Data is never freed.**", for this report and three others: the
`call/cc` clause is [r7rs-callcc-memory-never-freed](r7rs-callcc-memory-never-freed.md),
the caught-`raise` clause is
[r7rs-caught-raise-leaks-runtime-records](r7rs-caught-raise-leaks-runtime-records.md),
and "on all but a few prelude paths" is
[r7rs-remaining-scratch-leaks](r7rs-remaining-scratch-leaks.md). Its second
paragraph is the `--enable=r7rs-gc` escape hatch. When the collector graduates
(or this is answered another way), delete the bullet whole -- there is nothing
left to hedge, and the guide should not carry a "it used to grow" note.
