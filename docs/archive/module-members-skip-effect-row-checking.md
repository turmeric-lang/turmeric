# Functions inside a `defmodule` are never effect-row checked

**Severity: medium (silent loss of a checked guarantee).** `effect_check_pass`
walks only the program's top-level `EX_FN_DEF` items. A `defn` inside a
`(defmodule ...)` body lives in `EX_DEFMODULE`'s `mod->body` and is never
visited, so its declared `#fx{...}` row is never resolved, never inferred and
never checked: `#fx{}` on a module member is a promise nothing reads. Filed
2026-10-02, found while measuring effect-row-honesty-plan W4 against
turmeric-spices.

**Status: RESOLVED 2026-10-02** by fix direction 1, as a hard error (not the
warning-first migration of direction 2: the measured blast radius was three
files, all spice tests) -- see [Resolution](#resolution) at the end.

## Minimal repro

```turmeric
(defmodule m
  (export loud)
  (defn loud [] #fx{} : int (do (println "x") 0)))
(defn main [] : int 0)
```

```sh
./build/tur check repro.tur ; echo rc=$?
```

Observed: no diagnostic, `rc=0`. Expected: `TUR-E0009` -- `loud` declares
`#fx{}` and prints, which is `#fx{IO}` since W4. The same `defn` at top level
is rejected. An undeclared name in a member's row (`#fx{Typo}`) is not
reported either (TUR-E0026 runs during the same resolution), and neither is a
`(perform ...)` of an effect the member's row omits.

## Root cause

`src/passes/effect_check.c`, `effect_check_pass`: every step -- Step 0's row
resolution, the fixed point, Step 3's validation, the closure / call-site /
unreachable-handler sweeps -- iterates `program->as.program.items` and skips
anything that is not `EX_FN_DEF`. Module members are inside `EX_DEFMODULE`
items.

The CPS coloring had the same blind spot and fixed it by descending into
`mod->body` (`src/passes/cps.c`, the `CPS_ADD_FN_NODE` loop and its comment:
"A module member never appears as a top-level EX_FN_DEF"). The effect pass
never got the equivalent change.

## Blast radius of fixing it

This is why it is filed rather than fixed alongside W4: checking module
members turns every annotated member that prints, or that calls a member
whose row grew, into a compile error. Known instances in turmeric-spices,
where test programs are themselves modules -- each a `main` declared
`#fx{Unsafe}` that calls `println`:

- `spices/sqlite/tests/linear_handles_test.tur` (`main`, verified)
- `spices/tls/tests/conn_linear_test.tur` (`main`)
- `tidal_test.tur` (two sites)

Those were found by a static scan, not by running a fixed compiler; the
real count needs the fix and a sweep. Inside this repo, the stdlib loads
modules too (`effect-row-cross-private` shows `wrapper` and `run-internal`
absent from `--dump-effects` today).

## Fix directions

1. **Descend into `EX_DEFMODULE` bodies in every step of
   `effect_check_pass`**, the way `cps.c` does -- probably by collecting the
   function items once (top-level plus module members) into a list each step
   iterates. Keep the module-context bookkeeping (`s_current_analysis_module`,
   private-effect filtering) correct for members. Then sweep this repo and
   turmeric-spices and annotate what breaks (`#fx{Unsafe IO}` on those
   `main`s).
2. **Land it behind a warning first** if the spices sweep is large: report
   member violations as a warning for a release, then promote. Weaker, but
   it is a migration, unlike W1's undeclared-name error, which broke nothing.

`--dump-effects` should list module members too once they are inferred, so
the gap is visible from the outside.

## Resolution

**Mechanism.** `effect_check_pass` and `effect_check_dump_effects` no longer
walk `program->as.program.items`.  Both build one list first
(`effect_check_items`, `src/passes/effect_check.c`): every top-level item, with
`(defmodule ...)` bodies **and top-level `(do ...)` forms** spread in place,
recursively -- the same flattening the emitter does (`flatten_program_items`,
`emit_core.c`).  Every step iterates that list: Step -1's `defeffect`
registration, Step 0's row resolution (so `TUR-E0026` reaches a member's
`#fx{Typo}`), the ADT field rows, the typeclass passes, the index, the fixed
point, Step 3's validation and the closure / call-site / unreachable-handler
sweeps.  `s_current_analysis_module` already came from the member's binding,
so private-effect filtering needed no change.

**A second hole, same cause.** A macro that expands to
`(do (defn a ...) (defn b ...))` keeps its defns inside an `EX_DO`, and those
were skipped exactly like module members.  The flattening covers both.

**And a third, already live on `main`.** The binding-to-FnDef index was a
fixed 1024-slot array that dropped entries silently once full.  A callee missing
from the index contributes only its *declared* row, so every inferred effect of
a function past slot 1024 vanished from its callers: an unannotated printing
function defined late enough could be called from a `#fx{}` function and the
caller was accepted.  Not hypothetical -- a sample of 150 fixtures put the
`#lang r7rs` programs at 1241-1587 indexed functions *before* this change
(`r7rs-srfi-13` the largest), with the stdlib prelude alone around 460.  The
inferred row also feeds codegen (the CPS coloring in `cps.c`, and
`emit_cps_ir.c`'s runtime-pure direct call), so the loss was not only a missing
diagnostic.  The index is now a growable open-addressed hash on the binding
pointer (first add wins, as the linear scan's first match did).

**Blast radius, measured.**  `bash tests/run.sh`: 3501 passed, 0 failed, with
the fix and before the new fixtures -- no stdlib or fixture module member had a
dishonest row.  turmeric-spices, every `.tur` outside `web/` (777 files),
`tur check` before and after: exactly three files go from clean to
`TUR-E0009`, the three this report predicted --
`sqlite/tests/linear_handles_test.tur` (`main`),
`tls/tests/conn_linear_test.tur` (`main`) and `tidal/tests/tidal/tidal_test.tur`
(`print-bool`, `__run-checks`), each a `#fx{Unsafe}` that prints.  The fix in
that repo is `#fx{Unsafe}` -> `#fx{Unsafe IO}` on those four rows; it checks
clean under both the old and the new compiler, so it can land in either order.
No file went the other way.

**`--dump-effects`** lists module members now (`effect-row-cross-private` gains
`run-internal : #{InternalLog}` and `wrapper : #{}`, and the stdlib's module
members appear beside its top-level functions).

**Pinned by:**

- `tests/fixtures/errors/effect-row-module-member` -- a member's `#fx{}` that
  prints, one that calls an unannotated sibling that prints, one that
  performs a module-local effect its row omits (`TUR-E0009` x3), and an
  undeclared name in a member's row (`TUR-E0026`).
- `tests/fixtures/errors/effect-row-toplevel-do-defn` -- the top-level `do`
  shape.
- `tests/fixtures/errors/effect-row-past-1024-fns` -- 1100 filler functions,
  then an unannotated printing `noisy` called from a `#fx{}` `quiet`.  The old
  compiler accepted it; with 300 fillers it rejected it, which is the cap.
- `tests/fixtures/effect-row-module-member-ok` -- honest member rows compile
  and run: a member that handles its own effect, an unannotated member whose
  inferred `{IO}` reaches a top-level `#fx{IO}` caller.
- `tests/run-flags.sh` `dump-effects-module-members`.
