# `#lang r7rs`: an imported SRFI or on-demand library is recompiled on every build

**RESOLVED 2026-10-12.** Direction 1 is built: a program that imports a
library from the stdlib is three C units, and the imported libraries' unit is
compiled once and cached beside the prelude's. `r7rs-srfi-13` builds warm in
2.4 s instead of 4.4 s, `r7rs-eval` in 4.1 s instead of 5.6 s. See
"Resolution" at the end.

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

`tests/check-r7rs-prelude-split.sh`, the `imp/*` cases (see "Resolution").

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

## Resolution (2026-10-12): direction 1, a third unit

A split build of a program that imports a library from the stdlib now writes
three C units:

| unit | holds | cached by |
| --- | --- | --- |
| library (`EMIT_SPLIT_LIB`) | runtime preamble state, the auto-loaded stdlib | its text -- unchanged, the same object as before for every program |
| imported-library (`EMIT_SPLIT_IMP`, new) | every imported stdlib library's definitions, with external linkage | its text, in the same `prelude/` cache |
| program (`EMIT_SPLIT_CLIENT`) | the program; declares what the two others define | -- |

**The emitter** (`src/compiler/emit_module.c`, "r7rs-programs-compile-slowly:
the split build").  The modes are ordered LIB < IMP < CLIENT, and every item
and binding has a *home* unit (`emit_split_binding_home`, `split_item_home`):
the library unit's when elaborated in the auto-load window (`is_from_stdlib`),
the imported-library unit's when elaborated while an imported stdlib file's
forms were (`is_from_implib`, new, with `AdtDef.from_implib` and
`InlineC.from_implib`), the program's otherwise.  A unit defines the items of
its own home, declares those of an earlier one, and never sees a later one's
(`SPLIT_OWN_LATER`, filtered out as the library unit always filtered the
program).  That splits `emit_split_lib_owns`'s two questions the report named:
it still answers "external linkage" (defined here or declared from an earlier
unit), and `emit_split_defines` answers "this unit defines it".  Verbatim text
is rewritten per home -- the runtime preamble as a declarer in the IMP unit, its
own file-scope C blocks as a definer (`cprelude_own`).  Its fat boxes
(`__tur_ifatbox_N`), keyword records and function signatures are recorded with
the library unit's, so the program names whichever unit made one.  Its
initializers run from `__tur_split_imp_init`, which the program's
`__tur_static_init` calls after `__tur_split_lib_init`.

The origin flag is the elaboration window, not the span's file: a lambda
lifted while the program's own code is elaborated -- out of a SRFI macro's
expansion, say -- is the program's whatever file its span names, since its
body may name the program's globals, which the IMP unit never sees.

**Names that do not move with the program.**  An imported library's ids came
from the program's counter, so its lambdas (`__fn_192`) were renumbered by any
change to the program ahead of them in the two elaboration passes -- the
problem `next_stdlib_id` solved for the prelude.  An imported stdlib file's
forms now draw from `next_implib_id` (base 2,000,000; `ids_implib`, set per
form in both passes and in the module loader).  Seven `expected.c` snapshots
of programs importing a stdlib module moved by that renumbering alone.
`diag_file_is_imported_stdlib` decides what such a file is: past the
auto-loaded band, and under the stdlib root by canonical path (the elaborator
registers `stdlib/srfi/14.scm`, the driver resolves an absolute root).

**The driver** (`src/main.c`).  `compile_to_c` emits LIB, IMP, CLIENT; the
library unit is renamed by its own exports alone, so its text (and hash) is
the same whatever is imported.  The IMP unit compiles through
`prelude_split_object` like the library unit, with its own piece prefix
(`tur_spi_`: its object links beside the library unit's), in pieces only when
it exports at least 150 names -- every piece compiles the ~550 KB preamble the
unit carries (~0.5 s of cc), which is the wrong trade for a small SRFI.  The
shell-quoted `'-D...'` trap the prototype hit is fixed for this unit (a quoted
`-I`/`-D` token is accepted there; the library unit's key is untouched).
Fallbacks: an IMP unit the emitter refuses (a library's top-level statement --
`(sicp extras)` calls `(amb-reset!)` -- or dynamic variable) is a two-unit
build as before; three units that fail to compile or link retry as two before
one.  `TUR_IMPORT_SPLIT=0` turns the third unit off.

**What it does not do: share an SRFI's unit between programs.**  The SRFI
prune (`srfi_prune_program`) still runs per program, so an SRFI's unit holds
what this program reaches: cached for every rebuild of the program, but a
program that reaches other SRFI procedures compiles its own.  Keeping all of
SRFI 13 instead made its unit the same for every program, and cost a one-line
program ~1.9 s more of Debug-`tur` emission per build (the declarations in
the program unit, the definitions in the IMP unit) -- more than a cold compile
of the pruned unit, and on every build.  `(scheme read)`, `(scheme eval)` and
the other unpruned libraries do get one unit per import set: `r7rs-read`,
`r7rs-strings` and `r7rs-bignums` link the same object.

**Measured** (Linux, Debug `tur`, 4 cores, `tur build`, serial):

| program | before (every build) | after, cold IMP unit | after, warm |
| --- | --- | --- | --- |
| `r7rs-closure-one-capture-call` (no import) | 1.4 s | -- | 1.4 s |
| `r7rs-srfi-13` | 4.4 s | 4.0 s | 2.4-2.7 s |
| `r7rs-eval` | 5.6 s | 6.9 s | 4.1-4.2 s |
| `r7rs-read` | 2.1 s | 2.6 s | 1.8 s |

What is left of `r7rs-eval`'s warm build is its own code and the ASan
program unit (a trivial program unit compiled with `-fsanitize=address` is
1.8 s of cc on its own).

Over the corpus -- the 126 compiled `#lang r7rs` fixtures built one after
another, each from a fresh cache directory (`cold`) and then again (`warm`):

| | before | after |
| --- | --- | --- |
| warm | 267 s | 230 s (-14%) |
| cold | 270 s | 284 s (+5%) |

The cold column is the one CI pays where ccache misses (`tests/run.sh` builds
fixtures through ccache, so an unchanged unit's text is a hit there).  Most
SRFI fixtures are their import set's only program, so cold they compile their
IMP unit once -- the definitions they compiled before, plus a second unit's
fixed cost: ~0.5 s of cc for the preamble each unit carries, which is what
the +14 s is.  A developer rebuilding a program gets the warm column.
