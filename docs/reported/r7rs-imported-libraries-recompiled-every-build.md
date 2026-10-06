# `#lang r7rs`: an imported SRFI or on-demand library is recompiled on every build

**Severity:** medium (build time). Every build of a program that imports an
SRFI or `(scheme eval)` spends 2-4 s more in cc than a trivial program does,
on every build and with a warm cache. These are the 25 slowest warm r7rs
builds in the corpus (4-12 s each, Debug `tur`), and the reason most of them
carry a 60 s `expected.timeout`. Found 2026-10-05 by the audit in
[docs/notes/r7rs-performance-audit.md](../notes/r7rs-performance-audit.md).

## Repro

```sh
export ASAN_OPTIONS=detect_leaks=0 TMPDIR=$(mktemp -d)
tur build tests/fixtures/r7rs-closure-one-capture-call/input.tur -o x   # warms the cache
time tur build tests/fixtures/r7rs-closure-one-capture-call/input.tur -o x  # ~2.0 s
time tur build tests/fixtures/r7rs-srfi-13/input.tur -o x                   # ~6.4 s, same cached prelude object
TUR_SHOW_CC=1 tur build tests/fixtures/r7rs-srfi-13/input.tur -o x 2>&1 | grep -o 'prelude/[0-9a-f]*\.o'
```

Both builds link the same `prelude/<hash>.o`. The difference is the program
unit. The trivial one is 514 KB of C and compiles in 0.6 s. SRFI 13's is
1.1 MB and takes 4.2 s at `-O2`.

## Root cause

The prelude split (`src/main.c`, `emit_split_set_mode(EMIT_SPLIT_LIB)`; see
`src/compiler/emit_split.h`) puts only the **auto-loaded** stdlib in the
cached library unit. A library the program imports is spliced in as part of
the program. That covers `(srfi N)`, and `r7rs/eval.tur` plus `read.tur` for
`(scheme eval)`. Its definitions are therefore emitted into the program unit,
and cc compiles them on every build. Compared with a trivial program's unit
(by defined function names):

- `r7rs-srfi-13`: 376 more functions (91 `srfi13_*`, 15 `srfi14_*`, 148
  lambdas, 97 drop glues).
- `r7rs-eval`: 206 more functions (143 `r7rs_*` from `r7rs/eval.tur` and
  `read.tur`).

`srfi_prune_program` drops the SRFI definitions nothing reaches. That keeps
the unit from being larger still, but whatever remains is compiled every
time.

## Fix directions

1. **A cached unit per imported library.** Emit each imported library's
   definitions with external linkage into their own unit, and cache its
   object under a hash, as `prelude_split_object` does. That makes the
   pruning per library rather than per program: keep the library's exports.
   This depends on the library text being program-independent, which is the
   same problem as
   [r7rs-prelude-library-object-varies-with-the-program](r7rs-prelude-library-object-varies-with-the-program.md).
2. **Fold the import set into the library unit's key.** Put imported
   libraries in the library unit and let the cache hold one object per
   import set. Fixtures that import the same SRFIs then share an object.
   This is simpler, but every new import set pays a cold compile.
3. **Interim:** compile the program unit at a lower level when it carries
   library code. `-O1 -foptimize-sibling-calls` took SRFI 13's unit from
   4.2 s to 2.3 s. This costs run time, so it is a maintainer call (see the
   table in docs/archive/r7rs-prelude-library-cold-compile.md).

## Pinned by

Nothing yet. A check could build `r7rs-srfi-13` twice on a warm cache and
assert that the program unit holds no `srfi13_` definition.
