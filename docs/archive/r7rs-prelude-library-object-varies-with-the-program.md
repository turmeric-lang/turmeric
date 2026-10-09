# `#lang r7rs`: the cached prelude object varies with the program, so "first builds" keep happening

**RESOLVED 2026-10-09.** Cause 3, the last one, is fixed: an `-I` naming a
directory that holds no C header no longer reaches the cache key. See
"Resolution of cause 3" at the end.

**Narrowed 2026-10-07: causes 1 and 2 below are fixed, and so is a fourth
one this report missed. What is left is cause 3 (flags) and eviction.** On
the same corpus walk as below (129 `#lang r7rs` fixtures, in order, cold
cache), the count of library objects went from **10 to 2**: one shared by
every fixture, and one for programs that import `(scheme eval)`. Those link
the sanitized libturi, which is part of the cache key by design.

- **Cause 2 was not "a lambda".** Both repro programs below also define `f`,
  and the CPS coloring resolved stdlib `__cons-fmap`'s callback parameter `f`
  to the program's global `f` by name. A program defining any global `f`,
  `g`, `k`, ... compiled that stdlib function differently. Fixed as
  [cps-coloring-resolves-a-parameter-to-a-same-named-global](../archive/cps-coloring-resolves-a-parameter-to-a-same-named-global.md).
  `a.tur` and `b.tur` below now link one object.
- **Cause 1 was downstream of cause 2.** The shifted `__defer_env_N` /
  `__ps_N` counters were the fresh names the missing CPS twin did not
  consume. With cause 2 gone, the library text does not differ.
- **Cause 4: the program's own inline-C directives.** Every `#include` and
  object-like `#define` lifted from the top of an inline-C block was written
  into both units, the program's included. A crew module's
  `#define CREW_WORKERS 8` gave each `r7rs-threads-*` fixture its own object.
  Each hoisted entry now records whether the stdlib asked for it, and the
  library unit writes only those (`emit_hoisted_includes`,
  `src/compiler/emit_module.c`). A program's own file-scope C block was
  already the client unit's alone in intent; now it is in the library pass
  too.

`tests/check-r7rs-prelude-split.sh` now builds a program with a global `f`
and one importing a module whose C block `#define`s a macro, and fails if
either links a different library object than `r7rs-named-let-sum`.
`tests/run.sh`'s warm-up program therefore warms the object every non-`eval`
fixture links (fix direction 3 is moot).

**Fix direction 4 is done too:** a reused object's mtime is stamped, and
after compiling a new one the cache drops the oldest beyond 24 that no build
has used for a day (`prelude_cache_prune`, `src/main.c`).

**Still open:** cause 3 (`-I`/`-D` flags that reach no header still fork the
key). The original text follows.

**Severity:** medium. Each distinct library object costs a cold compile of
5-14 s (Debug `tur`, 4 cores, gcc 13). That happens once per variant, not
once per `tur` version as the prelude split intended. A user pays it again
whenever a program's shape changes, for example the first time a program
contains a `lambda`. In CI it is a flake source. On PR #1075, three legs failed
with `tur build timed out (>10s)` on two new r7rs fixtures that had no
`expected.timeout`. One of them, `9lives-r7rs-program`, simply sorts first and
so paid the cold compile for the variant most fixtures share. Nothing is
miscompiled. Found 2026-10-04.

The prelude split
([r7rs-programs-compile-slowly](../archive/r7rs-programs-compile-slowly.md),
with [r7rs-prelude-library-cold-compile](../archive/r7rs-prelude-library-cold-compile.md))
compiles the library unit once, caches it under
`<tmpdir>/tur-build/prelude/<hash>.o`, and links it into later builds. The
design rests on one claim, stated in `src/compiler/emit_split.h`: the library
unit "does not depend on the program". It does.

## What is not the cause

The slowness looks like it comes from importing libraries. It doesn't. A
program that imports a user library reuses the default object: on a cold
cache, `(import (scheme base) (scheme write) (mylib util))` built in 1.3 s
straight after a plain program warmed it. Importing on-demand libraries
(`(scheme eval)`, `(scheme file)`) does legitimately need a different
library unit, because more of the stdlib is in it.

## Repro

```sh
mkdir -p /tmp/v && cd /tmp/v && export ASAN_OPTIONS=detect_leaks=0
printf '#lang r7rs\n(display 1)\n' > a.tur
printf '#lang r7rs\n(import (scheme base) (scheme write))\n(define f (lambda (x) (+ x 1)))\n(display (f 1))\n(newline)\n' > b.tur
D=$(mktemp -d)
time TMPDIR=$D tur build a.tur -o x    # 5.3 s: cold, one object
time TMPDIR=$D tur build a.tur -o x    # 1.3 s: warm
time TMPDIR=$D tur build b.tur -o x    # 5.4 s: a SECOND object
ls $D/tur-build/prelude/*.o | wc -l    # 2
```

A program that defines a procedure with `(define (f x) ...)` forks the
object in the same way.

### Across the corpus

Every `#lang r7rs` fixture was built once, in order, on a cold cache, with
the Debug `tur` on origin/main + #1075, on a 4-core container:

- 114 fixtures; **10 library objects**. The September fix recorded 7 (79
  fixtures shared one object, 16 used 6 variants).
- The 10 builds that created an object took 5.2-14.2 s each (75 s
  together). The other 104 averaged 2.1 s.
- The ones that created an object: `9lives-r7rs-program` (the variant most
  fixtures then reuse), `docs-r7rs-guide-examples`, `r7rs-apply-many-args`,
  `r7rs-eval`, `r7rs-file-ports`, and five `r7rs-threads-*` fixtures (each
  thread fixture gets its own object).
- `r7rs-apply-many-args` imports only `(scheme base) (scheme write)`, yet it
  still gets its own object.

`tests/run.sh` warms the cache before the suite with `(display 1)`
(`_r7rs_warm`, around tests/run.sh:1299). That program has no lambda, so it
warms a variant most fixtures do not link. The first fixture of the common
variant still compiles cold, inside its timed build.

The cache is never evicted. One day of work in this container left 42
objects, 89 MB, in `/tmp/tur-build/prelude`.

## Root cause

`tur build` emits the library unit from the **same elaborated program** as
the client unit (`src/main.c:1227`, `emit_split_set_mode(EMIT_SPLIT_LIB)`
then `emit_program`). Three things the program decides reach the library
unit's text, and so its cache hash. I diffed two library units, one for
`a.tur` and one for `b.tur` (1,537,923 vs 1,539,106 bytes, 11,850 differing
lines):

1. **Fresh-name counters.** Names such as `__defer_env_240` / `__defer_241`
   and `__ps_3690` are numbered by counters that the program's own
   elaboration has already advanced. The same stdlib code comes out as
   `__defer_env_236` / `__ps_3686` for a different program.
2. **Whole-program lowering decisions.** With a lambda in the program, the
   stdlib's `_un/uncons-fmap` gets a CPS twin
   (`tur_sl__un_uncons_hyfmap__cps` and its join `_un_uncons_hyfmap_j0`). In
   a program without one, the twin is not emitted. The analysis that decides
   which functions need a CPS form runs over the whole program.
3. **Build flags that do not change the unit.** The hash covers every `-I`
   and `-D` the link line would pass (`prelude_split_object`, src/main.c).
   So `tur build -I . a.tur` and `tur build a.tur` produce byte-identical
   library text but two objects (5.3 s each, cold).

`srfi_prune_program` (src/main.c, just before the split) also makes the
library depend on which SRFI definitions the program reaches, but that one
is by design.

## Fix directions

1. **Make the library text a function of the stdlib only.** Emit the
   library unit with its own fresh-id space: reset or fix the counters'
   base before `EMIT_SPLIT_LIB`, or derive the names from the definition
   instead of a global counter. Then make the stdlib's CPS/lowering choices
   independent of the program. Either always emit the forms a program could
   need, or decide them from the stdlib's own call graph. This is the real
   fix. `tests/check-r7rs-prelude-split.sh` could then assert one object
   for every fixture that imports no on-demand library.
2. **Hash only what reaches the library unit.** Drop `-I`/`-D` flags that
   name no header the unit includes, or hash the preprocessed include set
   instead of the flag text.
3. **Interim mitigation:** have `run.sh`'s `_r7rs_warm` program contain a
   `lambda` and a procedure `define`, so it warms the variant most fixtures
   use. This hides the CI cost only; a user's first builds stay slow.
4. **Evict.** Age out `prelude/*.o` (by mtime, or keep the newest N) so the
   cache does not grow without bound.

## Pinned by

Nothing yet. A check that builds two programs differing only in a lambda,
on a fresh `TMPDIR`, and asserts one `prelude/*.o` would pin fix 1.

## Resolution of cause 3 (2026-10-09)

`prelude_split_object` (`src/main.c`) now hashes a copy of the compile flags
in which a build's own `-I <dir>` appears only if the directory could supply a
header (`dir_may_supply_headers`): a file named like one (`.h`, `.hh`, `.hpp`,
`.hxx`, `.inc`, `.def`) up to three levels down, or a directory too large to
walk (4,000 entries). The usual `-I` -- a Turmeric module path, or `-I .` in
a project of `.tur` files -- has none, so `tur build -I . a.tur` and `tur
build a.tur` now link one object; an `-I` with a header in it still gets its
own. The flag is passed to the compile either way.

Measured on a cold `TMPDIR`: `r7rs-named-let-sum` built plain, then with
`-I .`, leaves one object (two before); then with `-I hdr` (`hdr/x.h`), two.

What stays in the key on purpose: the `-I`/`-D` tokens a program's own
`__tur_autolink__` hints add. In the stdlib only `(scheme eval)` has any, and
an eval program links the sanitized libturi, which is its own object by
design (the first section above).

Pinned by `tests/check-r7rs-prelude-split.sh`: `stable/withI` builds the
`withf` program with `-I` on its own (header-free) directory and fails if it
links a different object.
