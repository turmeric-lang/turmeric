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

## Direction 2 prototyped and measured, 2026-10-08 -- not landed

Prototyped and reverted: what it needs, what it buys, and why it does not
pay on CI as it stands.

**What it took.**  An imported library's items are recognisable without a new
flag: their file is a `stdlib/` path past the auto-load band
(`file_id >= diag_autoload_file_ids_end()`, the binding's `span.file_id` for a
defn or def).  Giving those to the library unit in `emit_split_lib_owns` and
in each arm of `split_item_owner` (a type `SPLIT_OWN_BOTH`, inline C and the
default arm `SPLIT_OWN_LIB`), and running `srfi_prune_program` only when the
split will not (a declined split prunes before its single-unit fallback),
was enough for SRFI 13.

**A trap it hit: `prelude_split_object` drops a shell-quoted define.**  The
library unit's flag scan keeps a token only if it begins `-I` / `-D`, but the
resolved `@TUR_STDLIB_ROOT@` of `(scheme eval)` is the single-quoted
`'-DTUR_R7RS_STDLIB="<root>"'`.  With `eval.tur`'s C in the library unit the
define never reached its compile, and every `(scheme eval)` program died at
start-up: `the embedded R7RS evaluator did not start (stdlib at 'stdlib')`.
Accepting a whole-quoted `'-I...'` / `'-D...'` token (the compile runs through
`system()`, so the shell unquotes it as it does for the client) fixed it.  It
is latent today only because that C is the client unit's.

**Measured** (Linux, Debug `tur`, 4 cores, `tur build`, same session):

| program | before (every build) | prototype, cold | prototype, warm |
| --- | --- | --- | --- |
| `r7rs-closure-one-capture-call` (no import) | 1.16 s | -- | 1.16 s |
| `r7rs-srfi-13` | 4.0 s | 5.8 s | 2.0-2.3 s |
| `r7rs-eval` | 4.5 s | 10.5 s | 3.2 s |

**Why it was not landed.**  The cold column is paid once per IMPORT SET, and
the corpus has 32 of them among its 129 compiled r7rs fixtures (84 with none,
8 `(scheme read)`, 5 `(scheme eval)`, 3 `(srfi 216)`, then mostly
singletons).  CI restores only the compiler's ccache, never
`/tmp/tur-build/prelude/`, so every job would compile the whole prelude cold
up to 31 more times: roughly what it saves per build, spent again on misses.
It also breaks `tests/check-r7rs-prelude-split.sh`'s premise that
`r7rs-named-let-sum` and `r7rs-strings` share one library object
(`r7rs-strings` imports `(scheme read)`).  A developer rebuilding one program
gains the whole warm column; a cold CI run does not.

So direction 1 is the one to build: a SECOND cached unit holding only the
imported libraries (keyed by their text, as the prelude's is), with the
prelude object still shared by every program.  Its cold cost is today's
per-build cost, paid once.  What makes it more than the prototype is a third
split mode: that unit is a client of the prelude unit (its preamble state
`extern`, the prelude's functions declared) and the owner of the imported
items (defined with external linkage, recorded as exports for the
`EMIT_SPLIT_PREFIX` rename), while the program's unit declares both -- about
fifteen `g_emit_split == EMIT_SPLIT_LIB` / `EMIT_SPLIT_CLIENT` decisions in
`emit_module.c`, `emit_core.c`, `emit_fns.c`, `emit_cps_ir.c` and
`emit_split.c` each need a third answer, and `emit_split_lib_owns` currently
answers two questions at once ("give it external linkage" and "only declare
it here") that such a unit needs answered differently.
