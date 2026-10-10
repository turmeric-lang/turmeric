# Notebook `session-eval` never sets `module_base_dir`, so `(import ...)` fails in every cell

**RESOLVED 2026-10-10.** A cell imports the way a REPL turn would, and the
module search is the one `tur run <notebook>` would use. Three walls stood in
front of the first import, not one -- the report named only the first:

- **No search path.** libturi gained `turi_env_set_search_path_for(env, path)`
  (`src/turi/eval.h`): the module base dir becomes the notebook's directory and
  the extra search dirs become the enclosing spice's `src/`, each `:spices`
  dep's `src/` on disk, and the workspace siblings' `src/`. That walk-up moved
  out of `src/main.c` (`find_spice_root` / `auto_append_spice_includes`) into
  tur_core as `src/compiler/spice_search.c`, so the per-file commands and an
  embedder share one copy; main.c keeps its LS2 bookkeeping on hooks. The
  manifest reader gives up when `diag_had_error()` is already set -- as it is
  after a failed cell -- so the call runs it on a clean diag slate and puts the
  session's back.
- **A top-level `(import ...)` was an error everywhere** ("import is only
  allowed inside defmodule"), the REPL included (its guide listed it as a v1
  limit). `turi_env_set_toplevel_imports(env, true)` makes one legal on an env
  that asks: `:refer`, `:as` (kept per session, `Elab.toplevel_alias_*`) and
  `:for-macros`, through the same `elab_apply_import` a defmodule header now
  uses. Off by default, so a file's imports still belong to its defmodule on
  every back end; `tur repl` turns it on.
- **No stdlib.** A bare `turi_env_new` env has only the elaborator builtins, so
  a spice module naming `(Vec float)` did not elaborate.
  `turi_env_preload_stdlib(env, root)` runs the `--interpret` program preload
  (not the REPL's Show slice, which loads stdlib `String` globally and then a
  module defining its own -- frame/ownstr -- cannot export it). The notebook
  bakes its root in with the autolink hint `-DTUR_NB_STDLIB=@TUR_STDLIB_ROOT@`.

The notebook (`turmeric-spices`, `spices/notebook/src/notebook/session.tur`)
opens a session per file with `session-open-for path` -- all five callers --
and `session-reset` re-applies the configuration to its fresh env; against an
older libturi it falls back to `turi_env_set_module_base_dir` behind
`#ifdef TURI_HAS_SEARCH_PATH_FOR`.

Pinned by `tests/turi/embed-peripherals.c` Gap 9 (a scratch workspace: own
`src/`, a `:path` dep, a workspace sibling and a module beside the note, all
imported top-level; `:as` and `:refer` on a later turn; a re-run turn; reset;
the controls -- a bare env, and toplevel imports off), `tests/turi/repl-smoke.sh`
(`:refer` and `:as` at the prompt), and the notebook's own
`tests/session_test.tur` case 5 (import in one cell, call in the next, across
a reset). `math-walkthrough` checks clean; it loaded a module that never existed
(`stdlib/math`), now `(load "stdlib/math.tur")`.

**What is still in the way of the other example notebooks** is a different
wall, filed as
[notebook-cells-cannot-call-inline-c-spices](../reported/notebook-cells-cannot-call-inline-c-spices.md):
their imports resolve now, but `plot`, `linalg`, `stats` and `frame` do their
work in inline-C bodies, which the interpreter does not run. Running them
also turned up two inline-C executor defects, fixed with this:
`ic_exec_free` handed an emulated constructor's value-pool pointer to libc
`free` (an ASan bad-free in `(dist-free (dist-normal ...))`), and the
fat-pointer constructor case read `d->p1` / `out->length` as `->p` / `->len`
and miscompiled stats' `random-n` (`tests/run-interp-inline-c-constructor.sh`).

**Status at filing:** open.

**Severity:** medium (feature gap). The notebook's embedded evaluator cannot
resolve imports, so every example notebook that uses an external spice --
`plot`, `linalg`, `stats`, `frame` -- fails at the first cell. The notebook
is the literate-programming entry point for Turmeric, and the plan that
documents these workflows
([notebook-docs-examples-plan](../upcoming/notebook-docs-examples-plan.md))
ships five example notebooks that all import external spices.

Filed 2026-10-05 while implementing C4 of that plan. The `{html=table}`
cell attribute was added as a workaround for the related frame-detection gap
(the type tag is `"int"` for untyped frames), but the import failure is the
upstream blocker: no example notebook that imports an external spice can run.

## Repro

Build the notebook spice from `turmeric-spices` (vendored `tur` v0.63.2):

```
tur build spices/notebook -o /tmp/tur-nb
```

Create a minimal notebook `/tmp/import-probe.tur.md`:

````markdown
```turmeric
(import ansi/ansi :refer [ansi-red])
(println (ansi-red "hello"))
```
````

Export to HTML:

```
/tmp/tur-nb export html /tmp/import-probe.tur.md --out /tmp/
```

The cell exits with `status=error` and the HTML shows
`<div class="cell-output-meta status-error">`. Running `nb check` reports
`[] ERROR: elaboration error`.

The same failure occurs with `(import plot/core ...)`, `(import frame/csv
...)`, and every other external spice. Only the prelude (built-in functions
like `+`, `println`, `def`) is available.

## Root cause

`session-open` in `spices/notebook/src/notebook/session.tur` creates a fresh
`TuriEnv` via `turi_env_new()` but never calls
`turi_env_set_module_base_dir()`:

```c
// session.tur, session-open
NotebookSession *session = malloc(sizeof(*session));
session->env = turi_env_new();
return (int64_t)(intptr_t)session;
```

The `TuriEnv` struct has a `module_base_dir` field
(`vendor/tur/include/turi/env.h:436`) that controls where `(import ...)`
resolves module paths. When it is NULL (the default), the import resolver has
no base directory to search, so every import fails with a generic
"elaboration error".

The public setter exists and is documented
(`vendor/tur/include/turi/eval.h:524`):

```c
void turi_env_set_module_base_dir(TuriEnv *env, const char *path);
```

It is simply never called.

## Fix directions

1. **Minimal:** Call `turi_env_set_module_base_dir` in `session-open` with a
   sensible default -- either the notebook file's directory (passed in as a
   new parameter or set via a separate `session-set-base-dir` call) or the
   spice install directory (`~/.turmeric/spices/` or equivalent). The
   notebook CLI already knows the file path; it can derive the directory and
   pass it through.

2. **Per-file:** Add a `session-set-base-dir` function that the CLI calls
   after `session-open`, passing the directory of the `.tur.md` file being
   rendered. This lets imports resolve relative to the notebook, matching
   the user's mental model (a notebook next to `data.csv` can
   `(import frame/csv ...)` and load `data.csv` with a relative path).

3. **Search path:** Additionally, set the env's extra search directories to
   include the spice install path so `(import plot/core ...)` resolves
   without the user adding the spice to their project. The `build.tur`
   `:optional true` deps added in C3 make the spices available on disk; the
   evaluator just needs to know where to find them.

Direction 2 is the most natural: the notebook file's directory is the right
base for imports, and the CLI already has the file path. Direction 3 can be
layered on top once the spice install path is known at runtime.

## What works today

Cells that use only the prelude (arithmetic, `println`, `def`, `if`, `fn`,
`let`, etc.) evaluate correctly. Sweet-exp cells (`lang=1`) work. The shared
session persists bindings across cells. The `nb check` subcommand (added in
C3) correctly reports the error -- it is not a silent failure.

## What does not work

Any cell containing `(import ...)` targeting an external spice or a
user-authored module. This includes all five example notebooks from C1
(`plot-walkthrough`, `linalg-walkthrough`, `stats-walkthrough`,
`frame-walkthrough`, `data-analysis`) and the guide examples from C2.

The `{html=table}` cell attribute added in C4 is a rendering workaround for
the frame-detection gap, not for this import gap. It lets a cell opt into
table rendering when its stdout matches the `print-frame` pipe-delimited
format, but the cell still cannot import `frame/csv` to produce that output.
