# Notebook `session-eval` never sets `module_base_dir`, so `(import ...)` fails in every cell

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
