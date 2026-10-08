# `spices/notebook`: docs, examples, and integration improvements

> **Status:** nothing exists beyond the current notebook spice (v0.2.0) and
> its single example. The spice ships 12 source modules, 10 test suites, a
> README, a guide (`docs/guides/notebook-guide.md`), and exactly one example
> notebook (`examples/math-walkthrough.tur.md`) -- which covers KaTeX math
> rendering and nothing else. There are zero example notebooks for `plot`,
> `linalg`, `stats`, `frame`, or any combined data-analysis workflow. The
> guide has no section on using external spices from notebook cells, no
> section on the image-hook + plot integration, and no section on data
> analysis. The notebook's `build.tur` declares `test`, `ansi`, `png`, and
> `watch` as deps but does not declare `plot`, `linalg`, `stats`, or `frame`
> -- so a user who `tur install`s notebook cannot `(import plot/core ...)` in
> a cell without separately adding those spices to their project.
> **Open:** all phases. **Track:** post-v1 -- nothing on the v1 line depends
> on this; it is written down so the design survives.
> **Type:** spice (in `../turmeric-spices/`), plus guide and example files in
> the turmeric repo's `docs/guides/` and the spice's `examples/`.
> **Sequencing:** independent. No cross-spice dependency beyond the spices
> it documents (`plot`, `linalg`, `stats`, `frame`), all of which already
> exist and ship their own tests.

## 0. Summary

The notebook spice is the literate-programming entry point for Turmeric --
`.tur.md` files with embedded code cells, a TUI, HTML export, and scripted
`exec` mode. It works. What it does not have is anything showing a user *what
to do with it* beyond rendering LaTeX math.

The gap is not in the evaluator or the TUI. The gap is in the examples and
the guide. A user who installs `tur-notebook` and wants to plot a function,
fit a regression, or load a CSV and summarize it has to reverse-engineer
the integration from source -- because:

- The one example notebook (`math-walkthrough.tur.md`) is about KaTeX, not
  about data analysis.
- The guide (`docs/guides/notebook-guide.md`) documents the TUI, the CLI
  subcommands, the cell attributes, the image hook, and the math rendering,
  but has no section on importing external spices from cells, no worked
  example of plot-to-PNG-to-image-hook, and no data-analysis walkthrough.
- The notebook's `build.tur` does not declare `plot`, `linalg`, `stats`, or
  `frame` as deps, so `tur install notebook` does not make them available to
  cells. A user must know to add them to their own project's `build.tur`
  separately, and the guide does not say so.

This plan proposes four phases:

- **C1** -- five example notebooks: plot walkthrough, linear algebra
  walkthrough, statistics walkthrough, data-frame walkthrough, and a
  combined data-analysis workflow that ties them together.
- **C2** -- guide improvements: a "Using External Spices" section, a
  "Plotting from Notebook Cells" section, a "Data Analysis Workflows"
  section, and cross-references from the existing image-hook and math
  sections.
- **C3** -- notebook spice improvements: declare `plot`, `linalg`, `stats`,
  and `frame` as `:optional true` deps so examples work out of the box after
  `tur install notebook`, and add a `nb check` subcommand that validates a
  notebook file's cell syntax without evaluating.
- **C4** -- HTML export improvements: render data frames as HTML tables in
  cell output, and render plot images inline in HTML (not just the TUI).

The acceptance criterion is not "the examples run" but "a new user can
follow the guide from `tur install notebook` to a working plot-in-HTML
export without reading the spice source."

## 1. What exists today, and the four real gaps

### 1.1 What the notebook spice already has

| Capability | What exists | File |
| --- | --- | --- |
| Markdown + code cell parsing | cmark-based parser, cell extraction, attribute parsing | `src/notebook/cmark.tur`, `cell.tur`, `format.tur` |
| Session-backed evaluation | libturi embedded evaluator, shared session across cells | `src/notebook/session.tur` |
| Cell evaluation with caching | eval-cell, eval-all, eval-from, SHA-256 cache keys, depends | `src/notebook/eval.tur` |
| Image hook | `__NB_IMG__:` stdout marker, PNG path recording, TUI display | `src/notebook/image.tur` |
| HTML export | standalone HTML with KaTeX, embedded CSS, image data URLs | `src/notebook/render-html.tur` |
| Markdown render | terminal markdown rendering | `src/notebook/render-md.tur` |
| TUI | cell navigation, editing, evaluation, search, keybindings | `src/notebook/tui.tur` |
| CLI | `nb render`, `nb export`, `nb tui`, `nb exec`, `nb new` | `src/notebook/main.tur` |
| Tests | 10 test suites (cmark, cell, format, session, eval, cache, render-md, render-html, tui, exec) | `tests/notebook/` |
| Guide | `docs/guides/notebook-guide.md` -- 318 lines, covers TUI/CLI/format/image/math | `docs/guides/` |
| Example | `examples/math-walkthrough.tur.md` -- KaTeX math only | `examples/` |
| README | spice README with install instructions and CLI summary | `README.md` |

The evaluator and the image hook are the two pieces that make the rest of
this plan possible, and both already work:

- **`session-eval`** (in `session.tur`) calls `turi_eval_typed` from libturi,
  capturing stdout via a temp file. It supports `lang=1` (sweet-exp) by
  prepending `#lang turmeric/sweet`. All cells in a notebook share one
  `TuriEnv`, so bindings from earlier cells are visible in later ones. This
  is exactly the Jupyter model, and it means a cell can `(import plot/core
  ...)` and the next cell can call the imported function.
- **`image-hook-record-path`** (in `image.tur`) prints `__NB_IMG__: <path>`
  to stdout. `session-eval` strips these lines from the captured output
  and collects the paths into `cell-output.image-paths`. The TUI displays
  them inline (Kitty/iTerm2/sixel/text). The HTML exporter embeds them as
  base64 data URLs. The mechanism is end-to-end functional; it just has no
  example showing a user how to drive it from a plot.

### 1.2 Gap 1 -- no examples beyond math

The spice ships exactly one example: `math-walkthrough.tur.md`, which
demonstrates inline math, display math, multi-line equations, a `math=true`
output cell, and a deliberately broken KaTeX expression. It is a good
example of what it covers. It covers nothing else.

There is no example showing:

- A plot rendered to PNG and displayed in the TUI or exported to HTML.
- A linear algebra computation (matrix multiply, solve, decomposition).
- A statistical analysis (summary statistics, distribution PDF/CDF,
  hypothesis test, regression).
- A data frame workflow (load CSV, filter, group-by, summarize).
- A combined workflow (load CSV -> compute statistics -> plot results ->
  export to HTML).

These are the workflows that justify a notebook over a plain script, and
none of them are demonstrated.

### 1.3 Gap 2 -- the guide does not cover external spices or data analysis

The guide has 318 lines across 10 sections: Installing, Quick Start,
Notebook File Format, Cell Types, exec Subcommand, new Subcommand, Image
Hook, Math (KaTeX), TUI Key Bindings, and Further Reading. The Image Hook
section shows the `image-hook-record-path` call but does not connect it to
`plot-write-png` -- a user reading it learns the mechanism but not the
workflow.

Missing sections:

- **"Using External Spices from Cells"** -- how to `(import plot/core ...)`
  in a cell, and the requirement that the spice be declared in the
  project's `build.tur` (or the notebook's, after C3).
- **"Plotting from Notebook Cells"** -- the full pipeline: build
  renderers, call `plot-write-png`, call `image-hook-record-path`, see the
  image in the TUI or HTML.
- **"Data Analysis Workflows"** -- loading a CSV with `frame/csv`, computing
  summary statistics with `stats/summary`, fitting a regression with
  `stats/regress`, plotting the fit with `plot/line`.

### 1.4 Gap 3 -- notebook does not declare analysis spices as deps

The notebook's `build.tur` declares four spices:

```turmeric
:spices #map{
  "test"  #map{:path "../test"  ...  :optional true}
  "ansi"  #map{:path "../ansi"  ...}
  "png"   #map{:path "../png"   ...}
  "watch" #map{:path "../watch" ...}
}
```

`plot`, `linalg`, `stats`, and `frame` are not declared. A user who runs
`tur install notebook` and writes a cell with `(import plot/core ...)` gets
an import error, because the plot spice is not fetched or built. The user
must know to add `plot` (and `linalg`, `stats`, `frame`) to their own
project's `build.tur` separately.

This is not a bug -- the notebook spice's own code does not import those
spices, so it does not need them to build. But it is a documentation gap:
the guide does not tell the user to do this, and the examples (after C1)
will not work without it.

The resolution is in C3: declare them as `:optional true` so the notebook
builds without them (its own code does not use them), but `tur install
notebook` fetches them so cells can import them. The `:optional true` flag
means a missing dep does not fail the build -- it only means the import is
unavailable, which is the correct behavior for a notebook that may or may
not be used for data analysis.

### 1.5 Gap 4 -- HTML export does not render data frames or tables

The HTML exporter (`render-html.tur`) renders cell output as
`<pre><code>...</code></pre>` for stdout and as KaTeX math for `math=true`
cells. It embeds images as base64 data URLs via `png->data-url`. It does
not:

- Render a `frame` as an HTML `<table>` -- the cell's stdout is a
  `print-frame` text dump, which is readable in the TUI but ugly in HTML.
- Render plot images inline in the HTML body -- images are embedded as
  data URLs in `<img>` tags, but only if the image hook was used. A cell
  that calls `plot-write-png` without `image-hook-record-path` produces a
  PNG file on disk that the HTML exporter never sees.

The first is a quality-of-life improvement; the second is a documentation
gap (the guide should show the two-call pattern) that C2 addresses. The
frame-to-table rendering is C4 because it requires the HTML exporter to
recognize a frame value in the cell output, which the current
`cell-output-value-repr` field does not carry type information for.

## 2. Design

### 2.1 Example notebooks

Five example notebooks, each in `spices/notebook/examples/`:

**`plot-walkthrough.tur.md`** -- the image-hook + plot pipeline:

```turmeric
(import plot/core  :refer [plot-write-png])
(import plot/line  :refer [function])
(import plot/decor :refer [axes tick-grid])
(import plot/style :refer [default-line-style default-plot-opts])
(import notebook/image :refer [image-hook-record-path])

(defn quadratic [x :float] :float (* x x))

(plot-write-png
  (vec-of (tick-grid)
          (axes)
          (function quadratic -2.0 2.0 128
                    (default-line-style) "x^2"))
  (default-plot-opts)
  "/tmp/nb-plot-quadratic.png")

(image-hook-record-path "/tmp/nb-plot-quadratic.png")
```

The example should show: (1) a function plot, (2) a scatter plot with
`points`, (3) a histogram with `discrete-histogram`, and (4) a density
estimate with `density`. Each cell writes a PNG and records it. The prose
explains the two-call pattern (write PNG, then record path) and why it
exists (the image hook is a stdout marker, not a return value).

**`linalg-walkthrough.tur.md`** -- matrix operations and decompositions:

```turmeric
(import linalg/mat  :refer [mat-of mat-mul mat-transpose mat-print])
(import linalg/vec  :refer [la-vec-of la-vec-dot la-vec-norm])
(import linalg/solve :refer [mat-solve])
(import linalg/decomp :refer [lu lu-solve])

(def A (mat-of 2 2 1.0 2.0 3.0 4.0))
(def b (la-vec-of 5.0 6.0))
(mat-print (mat-solve A b))
```

The example should show: (1) matrix creation and printing, (2) matrix
multiply and transpose, (3) solving a linear system, (4) LU decomposition
and back-substitution, and (5) a small least-squares fit. The prose
connects each step to the `linalg/*` module it comes from.

**`stats-walkthrough.tur.md`** -- distributions, summary, tests, regression:

```turmeric
(import stats/dist    :refer [dnorm pnorm qnorm rnorm])
(import stats/summary :refer [col-mean col-sd col-median])
(import stats/test    :refer [t-test-2samp])
(import stats/regress :refer [ols])
(import stats/fmt     :refer [print-test print-fit])

;; Normal distribution: PDF at 0, CDF at 1, quantile at 0.975
(println (dnorm 0.0 0.0 1.0))   ;; => 0.3989...
(println (pnorm 1.0 0.0 1.0))   ;; => 0.8413...
(println (qnorm 0.975 0.0 1.0)) ;; => 1.96...
```

The example should show: (1) distribution functions (pdf/cdf/quantile/random),
(2) summary statistics on a vector of samples, (3) a two-sample t-test,
and (4) an OLS regression with diagnostics. The prose explains the `d*`/
`p*`/`q*`/`r*` naming convention (density/CDF/quantile/random, matching R).

**`frame-walkthrough.tur.md`** -- CSV loading, filtering, grouping:

```turmeric
(import frame/csv   :refer [read-csv-string])
(import frame/frame  :refer [frame-nrows frame-ncols frame-head])
(import frame/filter :refer [filter-mask])
(import frame/group  :refer [group-by grouped-count agg agg-mean])
(import frame/print  :refer [print-frame])

(def df (read-csv-string "name,age,city\nAlice,30,NYC\nBob,25,SF\nCarol,35,NYC\n"))
(println (frame-nrows df))
(print-frame (frame-head df 3))
```

The example should show: (1) loading a CSV from a string (so the example
is self-contained, no external file), (2) inspecting shape and head, (3)
filtering rows, (4) group-by with aggregation (count, mean), and (5)
printing the result. The prose notes that `read-csv` (file-based) is the
production path and `read-csv-string` is for examples and tests.

**`data-analysis.tur.md`** -- the combined workflow:

A single notebook that loads a CSV, computes summary statistics, fits a
regression, plots the data and the fit, and exports to HTML. This is the
notebook that justifies the notebook format: each step is a cell, the
prose explains the analysis, and the final HTML export is a shareable
report. It should use `frame`, `stats`, and `plot` together, demonstrating
the cross-spice workflow that no individual spice guide shows.

### 2.2 Guide improvements

Three new sections in `docs/guides/notebook-guide.md`:

**"Using External Spices from Cells"** -- after the "Cell Types" section.
Explains that cells run in a shared libturi session, so `(import ...)` in
one cell makes the module available in subsequent cells. Lists the spices
that are most useful in notebooks (`plot`, `linalg`, `stats`, `frame`) and
notes that they must be declared in the project's `build.tur` (or that C3
makes them available via the notebook's own `:optional` deps). Includes a
table:

| Spice | Import | What it provides |
| --- | --- | --- |
| `plot` | `(import plot/core :refer [plot-write-png])` | 2D visualization, PNG output |
| `linalg` | `(import linalg/mat :refer [mat-of mat-mul])` | Dense linear algebra |
| `stats` | `(import stats/dist :refer [dnorm pnorm])` | Distributions, tests, regression |
| `frame` | `(import frame/csv :refer [read-csv])` | Data frames, CSV I/O |

**"Plotting from Notebook Cells"** -- after the "Image Hook" section. The
full two-call pipeline with a worked example. Explains that
`plot-write-png` writes a PNG to disk and `image-hook-record-path` tells
the notebook to display it. Shows the pattern in both turmeric and
sweet-exp cell syntax. Notes that the TUI displays images via Kitty/iTerm2/
sixel/text and the HTML exporter embeds them as base64 data URLs.

**"Data Analysis Workflows"** -- after the plotting section. A condensed
version of the `data-analysis.tur.md` example, showing the cross-spice
pipeline: load CSV with `frame/csv`, summarize with `stats/summary`, fit
with `stats/regress`, plot with `plot/line`. Links to the full example
notebook in `examples/`.

### 2.3 Optional deps and `nb check`

**Optional deps.** Add `plot`, `linalg`, `stats`, and `frame` to the
notebook's `build.tur` as `:optional true`:

```turmeric
"plot"  #map{:path "../plot"  :url "..." :ref "..." :subdir "spices/plot"  :optional true}
"linalg" #map{:path "../linalg" :url "..." :ref "..." :subdir "spices/linalg" :optional true}
"stats" #map{:path "../stats"  :url "..." :ref "..." :subdir "spices/stats"  :optional true}
"frame" #map{:path "../frame"  :url "..." :ref "..." :subdir "spices/frame"  :optional true}
```

The notebook's own code does not import these spices, so `:optional true`
means the build succeeds without them. When present (which `tur install`
ensures by fetching all declared spices, including optional ones), cells
can `(import plot/core ...)` without the user adding anything to their
project. When absent, the import fails with a clear error -- which is
correct, because the spice is not installed.

The `:ref` for each should match the latest tag at the time of
implementation. The `:path` entries are for workspace-local development
(matching the existing `ansi`, `png`, `watch` entries).

**`nb check` subcommand.** A new CLI subcommand that parses a notebook
file and validates cell syntax without evaluating. This is useful for CI
and for users who want to catch typos before running. It should:

- Parse the `.tur.md` file with `notebook/format`.
- For each cell, check that the source parses (via `turi_eval_typed` with
  a dry-run flag, or via the compiler's parse-only mode if available).
- Report cell IDs, line numbers, and any parse errors.
- Exit 0 on success, 1 on any parse error.

This is a small addition to `main.tur` -- a new subcommand handler that
calls `parse-file` and iterates cells, calling a parse-only entry point.
If libturi does not expose a parse-only mode (it likely does not), the
fallback is to evaluate each cell in a throwaway session and report
errors, which is what `exec --all` already does but without printing
output. The `nb check` name is chosen to match `tur check`.

### 2.4 HTML export improvements

**Frame-to-table rendering.** The HTML exporter currently renders cell
output as `<pre><code>stdout</code></pre>`. For cells whose output is a
`frame` (detected by the `type_tag` from `turi_eval_typed`), the exporter
should render an HTML `<table>` instead. This requires:

1. `session-eval` already captures `type_tag` (a 128-byte string from
   `turi_eval_typed`). The tag for a frame value needs to be identified --
   likely `"frame"` or `"Frame"` based on how `frame/frame.tur` registers
   its type. If the tag is not distinctive enough, a cell attribute
   `{html=table}` can opt in instead.
2. The exporter reads the `type_tag` and, if it indicates a frame, calls
   `frame->str` with a structured format (or a new `frame->html` helper)
   to produce an HTML table. The frame's `print-frame` already produces
   aligned text; an HTML table is a rendering of the same data.
3. The table is wrapped in `<div class="cell-output-table">` with the
   vendored CSS.

This is C4 because it requires the exporter to understand frame values,
which is a type-awareness the current exporter does not have. The
simpler alternative -- a cell attribute `{render=html}` that treats the
cell's stdout as raw HTML -- is a fallback if the type-tag path is not
reliable, but it is less safe (user stdout as HTML is an XSS vector in
shared notebooks). The type-tag path is preferred.

**Inline plot images in HTML.** This already works: `session-eval`
collects `__NB_IMG__:` paths into `cell-output.image-paths`, and
`render-html.tur` embeds them as base64 data URLs via `png->data-url`.
The gap is documentation, not code -- C2's "Plotting from Notebook Cells"
section closes it. No code change needed here.

### 2.5 Purity, ownership, and the inline-C budget

The notebook spice is already heavy on inline C -- `session.tur`,
`eval.tur`, `render-html.tur`, and `image.tur` all use it extensively,
because they interface with libturi (the embedded evaluator) and libc
(stdout capture, file I/O, base64 encoding). This plan adds **no new
inline C** to the notebook spice:

- The example notebooks are `.tur.md` files -- no code, no inline C.
- The guide improvements are markdown -- no code, no inline C.
- The optional deps are `build.tur` changes -- no inline C.
- `nb check` is a new subcommand handler in `main.tur` that calls
  existing functions (`parse-file`, `session-open`, `session-eval`,
  `session-close`). If it uses `session-eval` for the fallback path, it
  reuses existing inline C; no new inline-C bodies.
- The frame-to-table rendering in `render-html.tur` may need one small
  inline-C helper to check the `type_tag` string, but the tag is already
  captured as a `char[128]` in `session-eval`'s inline C. The exporter
  can read it via a new `cell-output-type-tag` accessor, which is one
  inline-C body returning a `cstr`. That is the **one** new inline-C
  body in this plan, and it is in C4.

The interpreter-coverage concern from the CRDT and probabilistic plans
does not apply here: the notebook spice already has inline C in its core
modules, so `tests/run-turi.sh` already PASS-skips those modules. The
example notebooks are data, not test fixtures, and are not run by the
test suite.

### 2.6 Testing

The example notebooks are validated by a new test in
`tests/notebook/exec_test.tur` (or a new `examples_test.tur`) that runs
`nb exec --all` on each example and asserts exit code 0. This catches
regressions in the examples without requiring a full evaluation of every
cell's output. The test should be gated on the optional deps being
present -- if `plot` is not installed, the plot example test is skipped,
not failed.

The guide improvements are validated by the existing
`check-guide-pairs.py` CI check, which verifies that every code block in
a guide has a matching toggle pair (turmeric + sweet-exp). The new
sections must follow the same toggle-pair convention as the existing
guide.

The `nb check` subcommand is validated by a test that runs `nb check` on
a valid notebook (exit 0) and on a notebook with a syntax error (exit 1).

The frame-to-table rendering is validated by a test that exports a
notebook with a frame-valued cell to HTML and asserts the output contains
`<table`.

## 3. Back-end and dialect caveats

- **Interpreter:** the example notebooks use `plot`, `linalg`, `stats`,
  and `frame`, all of which contain inline C. They will not run under
  `tur --interpret`. This is expected and documented -- the examples are
  for the compiled path, which is the notebook's primary use case (the
  TUI and HTML exporter are compiled-only).
- **JIT:** nothing here is JIT-specific. The notebook's `session-eval`
  uses the tree-walking interpreter (libturi), not the JIT, so cell
  evaluation is not affected by JIT state.
- **Saffron:** notebook cells through `any` are out of scope. The cell
  evaluator already captures `type_tag` from `turi_eval_typed`, which is
  the typed path; the `any` path is not used.
- **Sweet-exp cells:** the guide's toggle-pair convention requires every
  turmeric code block to have a matching sweet-exp block. The example
  notebooks are `.tur.md` files, not guide pages, so they do not need
  toggle pairs -- but the guide's new sections do. The examples
  themselves can use either dialect; the `lang` attribute on the fence
  controls which (turmeric is the default, `lang=sweet` selects
  sweet-exp).

## 4. Why a spice, and why the deps go in the spice

The notebook spice is already a spice; this plan improves it. The
question is whether `plot`, `linalg`, `stats`, and `frame` should be
declared as deps in the notebook's `build.tur` or left to the user's
project.

The argument for declaring them in the notebook spice: `tur install
notebook` should give the user a working notebook that can plot and
analyze data. Requiring the user to separately `tur install plot`,
`tur install linalg`, `tur install stats`, and `tur install frame` is a
poor first-run experience, and the guide has to explain it, which is
more complexity than just declaring the deps.

The argument against: the notebook spice's own code does not use those
spices, so declaring them is "unused deps." But `:optional true` means
they are not required for the build, and the workspace model in
`turmeric-spices` means they are already present as siblings. The cost
is zero for workspace development and one `tur fetch` for external
installers, which is the correct behavior.

The plan says **declare them as `:optional true`**. This is the same
pattern the notebook already uses for `test` -- the spice does not need
`test` to build, but it declares it so `tur test` works. The analysis
spices are the same: the spice does not need them to build, but it
declares them so cells can import them.

No stdlib additions are proposed. The notebook's improvements are
self-contained in the spice and the guide.

## 5. Phases

- **C1 -- example notebooks.** Five `.tur.md` files in
  `spices/notebook/examples/`: `plot-walkthrough.tur.md`,
  `linalg-walkthrough.tur.md`, `stats-walkthrough.tur.md`,
  `frame-walkthrough.tur.md`, and `data-analysis.tur.md`. Each is a
  self-contained literate notebook with prose explaining the workflow
  and code cells demonstrating the spice's API. The `data-analysis`
  notebook ties `frame`, `stats`, and `plot` together.

  Acceptance: each example runs with `nb exec --all` and exits 0 (gated
  on the optional deps being present, which they are in the workspace).
  The `math-walkthrough.tur.md` example is unchanged.

- **C2 -- guide improvements.** Three new sections in
  `docs/guides/notebook-guide.md`: "Using External Spices from Cells",
  "Plotting from Notebook Cells", "Data Analysis Workflows". Each
  section follows the existing guide's format (prose + code blocks with
  toggle pairs). Cross-references from the existing Image Hook and
  Math sections to the new sections.

  Acceptance: `check-guide-pairs.py` passes (all toggle pairs are
  complete). The guide reads as a continuous document -- a user can
  follow it from install to a working plot-in-HTML export without
  reading the spice source.

- **C3 -- optional deps and `nb check`.** Two changes to the notebook
  spice:

  1. Add `plot`, `linalg`, `stats`, and `frame` to `build.tur` as
     `:optional true` with `:path` entries for workspace development
     and `:url`/`:ref`/`:subdir` for external installs. The `:ref`
     values should match the latest tags at implementation time.
  2. Add a `nb check` subcommand to `main.tur` that parses a notebook
     file and validates cell syntax without evaluating. Exit 0 on
     success, 1 on any parse error.

  Acceptance: `tur build` succeeds with and without the optional deps
  present. `nb check` on a valid notebook exits 0; on a notebook with
  a syntax error exits 1 with a useful error message.

- **C4 -- HTML export improvements.** Frame-to-table rendering in
  `render-html.tur`: detect a frame value in cell output via the
  `type_tag` from `session-eval`, and render it as an HTML `<table>`
  instead of `<pre><code>`. This requires one new inline-C accessor
  (`cell-output-type-tag`, returning the `cstr` type tag) and a
  `frame->html` helper (or a structured `frame->str` mode).

  Acceptance: a notebook with a frame-valued cell exports to HTML with
  a `<table>` in the cell output, not a `<pre>` text dump. The existing
  `<pre>` rendering is preserved for non-frame outputs.

C1 and C2 are the useful unit; a user can follow the guide and run the
examples after C2. C3 makes the examples work out of the box after
`tur install notebook`. C4 is a quality-of-life improvement for HTML
export. C1-C2 can land together; C3 can land independently; C4 is last.

## 6. Risks and open questions

- **Optional dep `:ref` values drift.** The `:ref` for each optional dep
  must match a real tag in `turmeric-spices`. If the tag does not exist
  at install time, `tur fetch` fails for that dep. Since the deps are
  `:optional true`, the failure is non-fatal for the build, but it means
  the import is unavailable. Mitigate by pinning to the latest stable
  tag at implementation time and updating the `:ref` when the notebook
  spice is released. The workspace `:path` entries are unaffected.
- **`type_tag` for frame values is unverified.** The `turi_eval_typed`
  call in `session-eval` captures a `type_tag` string, but the exact
  value for a `frame` return is not known without probing. If the tag
  is not distinctive (e.g., it is `"ptr"` or `"any"` for all opaque
  types), the frame-to-table rendering in C4 cannot use it and must
  fall back to a cell attribute (`{render=table}`). Probe this in C4
  before committing to the type-tag path; the fallback is documented.
- **Example notebooks depend on spices that may not be installed.** A
  user who installs notebook without the optional deps (e.g., on a
  minimal system) cannot run the examples. The guide should say so
  plainly: "These examples require `plot`, `linalg`, `stats`, and
  `frame`, which are declared as optional deps. If they are not
  installed, the cells will fail with an import error." The `nb check`
  subcommand from C3 can detect missing imports at parse time and
  report them as warnings.
- **Sweet-exp toggle pairs in the guide.** The guide's new sections
  must include sweet-exp toggle pairs for every turmeric code block, or
  `check-guide-pairs.py` fails. The sweet-exp syntax for imports and
  function calls is different (curly-infix, no parens around top-level
  forms), and the examples must be correct in both dialects. This is
  mechanical but error-prone -- the existing guide already has toggle
  pairs, so the pattern is established, but the new sections have more
  code blocks than the existing ones.
- **`nb check` may need a parse-only mode that libturi does not
  expose.** `turi_eval_typed` evaluates, it does not parse-only. If
  there is no parse-only entry point in libturi, `nb check` must
  evaluate each cell in a throwaway session, which is slower and has
  side effects (a cell with `(println ...)` prints to the captured
  stdout). The fallback is to evaluate with output suppressed (which
  `session-eval` already does via stdout capture) and report errors.
  This is acceptable for a `check` subcommand -- it is not a
  parse-only tool, it is a "does this notebook run" tool. The name
  `check` is chosen to match `tur check`, which also evaluates.
- **Frame-to-table rendering is XSS-adjacent.** Rendering a frame as
  an HTML table means putting cell values (which may come from a CSV)
  into HTML. If the CSV contains `<script>` tags, the HTML export
  would execute them. Mitigate by HTML-escaping all cell values in
  the table renderer. The existing `md-emit-html` in `cmark.tur`
  already escapes markdown text; the frame renderer should do the
  same. This is a C4 concern and is noted here so it is not forgotten.

## 7. File inventory

| Phase | File | Action |
| --- | --- | --- |
| C1 | `spices/notebook/examples/plot-walkthrough.tur.md` | new |
| C1 | `spices/notebook/examples/linalg-walkthrough.tur.md` | new |
| C1 | `spices/notebook/examples/stats-walkthrough.tur.md` | new |
| C1 | `spices/notebook/examples/frame-walkthrough.tur.md` | new |
| C1 | `spices/notebook/examples/data-analysis.tur.md` | new |
| C2 | `docs/guides/notebook-guide.md` | edit (3 new sections) |
| C3 | `spices/notebook/build.tur` | edit (4 optional deps) |
| C3 | `spices/notebook/src/notebook/main.tur` | edit (`nb check` subcommand) |
| C3 | `spices/notebook/tests/notebook/check_test.tur` | new |
| C4 | `spices/notebook/src/notebook/session.tur` | edit (`cell-output-type-tag` accessor) |
| C4 | `spices/notebook/src/notebook/render-html.tur` | edit (frame-to-table) |
| C4 | `spices/notebook/tests/notebook/render_html_test.tur` | edit (frame table test) |

Files in `spices/notebook/` are in the `turmeric-spices` repo. Files in
`docs/guides/` are in the `turmeric` repo. The plan is written from the
turmeric repo (where this file lives), and the spice changes are
described as they would be made from a turmeric-spices-rooted session.

## 8. References

- In-tree: [notebook-guide.md](../guides/notebook-guide.md) (the guide
  this plan extends), [notebook-spice-plan.md](../archive/history/notebook-spice-plan.md)
  (the original NB0-NB12 milestone plan, archived), [notebook-katex-plan.md](../archive/notebook-katex-plan.md)
  (the KaTeX integration plan, archived).
- Spice source: `spices/notebook/build.tur` (manifest), `src/notebook/session.tur`
  (libturi session, `turi_eval_typed`, stdout capture, `__NB_IMG__` parsing),
  `src/notebook/image.tur` (image hook, `image-hook-record-path`, `png->data-url`),
  `src/notebook/eval.tur` (cell evaluation, caching, depends), `src/notebook/render-html.tur`
  (HTML export, image embedding).
- Spice APIs: `spices/plot/build.tur` (`plot/core`, `plot/line`, `plot/decor`,
  `plot/style`, `plot/area`), `spices/linalg/build.tur` (`linalg/mat`, `linalg/vec`,
  `linalg/decomp`, `linalg/solve`, `linalg/fmt`), `spices/stats/build.tur`
  (`stats/dist`, `stats/summary`, `stats/test`, `stats/regress`, `stats/fmt`),
  `spices/frame/build.tur` (`frame/csv`, `frame/frame`, `frame/filter`,
  `frame/group`, `frame/print`).
- Format: [crdt-spice-plan.md](crdt-spice-plan.md) (the plan format this
  file follows), [probabilistic-spice-plan.md](probabilistic-spice-plan.md)
  (a more recent example of the same format).
