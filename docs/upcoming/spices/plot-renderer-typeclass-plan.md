# Plan: `plot` Renderer Typeclass Collapse (U2, v1)

> Status: P0–P3 landed. **P4 and P5 done 2026-10-21** on turmeric-spices
> branch `claude/plot-renderer-typeclass-p4`, meeting the whole-plan
> acceptance below: all 30 renderer kinds are types with a `Renderer`
> instance (`bounds`, `draw`, and the defaulted `legend-label` /
> `frame-marks` / `categorical?` / `category-labels`); every public
> constructor returns `AnyRenderer`; `plot` & co. take a
> `(Vec AnyRenderer)`; the draw `switch`, the bounds `switch` and the slot
> bag are gone; callbacks are typed fn values; style and option handles are
> `LineStyle` / `PointStyle` / `FillStyle` / `PlotOpts`; renderer data is
> a Vec of typed records from `plot/data`. Pixel check: every plot body is
> byte-identical to pre-plan output except where a bug fix changed it
> (`arrows`' bounds, `contours`' levels, the interval kinds' outlines, the
> histograms' per-bar auto colors and horizontal category axis), and legend
> swatches show each renderer's own style, a row per stacked layer. Building
> the spice under clang needs turmeric branch
> `claude/exbox-thunk-carrier-result` (a compiler fix). **Remaining:** only
> the deprecated `plot-anyrenderers` alias, kept for one release. See P4.
> Tracks: spices-type-features-uplift-plan **U2 target — plot**
> Scope: `spices/plot/` only; P3 needed a compiler dependency after all, now
> satisfied (see Risks).

## Motivation

`plot` is the U2 "typeclass collapse" target whose dispatch is the most
deeply tag-erased in the repo. Every renderer constructor
(`function`, `lines`, `points`, `area`, `contour`, …) calls
`__make-renderer` with a numeric `kind` tag and packs heterogeneous
payload into a fixed bag of generic slots:

```turmeric
;; plot/line.tur
(defn function [f x-min : float x-max : float samples : int style : int label : cstr] : int
  (__renderer-bounds
    (__renderer-ints
      (__renderer-floats (__make-renderer 21 style 0 f 0 0 0 0 label "")
                         x-min x-max 0.0 0.0 0.0 0.0)
      samples 0 0 0)
    x-min x-max (__nan) (__nan)))
```

`__make-renderer` (`plot/core.tur:53`) allocates one C struct with
`kind`, `style1/2`, `data1..data5`, `f1..f6`, `i1..i4`, `label`, `text`,
and four bounds doubles. Each renderer kind interprets these slots
differently, and the meaning is recoverable **only** by reading the
central switch.

That switch lives twice in `plot/core.tur`:

| Site | Lines | Arms |
|---|---|---|
| `RK_*` enum | `core.tur:604-613` | ~32 kinds |
| bounds-union pass (`plot-into-canvas`) | `core.tur:692-771` | per-kind `UNION_*` |
| draw pass (`plot-into-canvas`) | `core.tur:~900-1543` | per-kind drawing |
| adaptive sampler kind switch | `core.tur:367-373, 389-411` | RK_FUNCTION/PARAMETRIC/POLAR/INVERSE |

This is exactly the U2 rubric's "≥3 parallel branches differing only in
payload type", taken to the extreme: one runtime `kind` int drives a
~32-arm `switch`, and the payload slots are `int64_t`-erased. The bugs
this invites are concrete:

- A constructor that stuffs a value into the wrong slot index is a silent
  mis-render, not a type error (`data2` means "category count" for
  `RK_DISCRETE_HIST` but "second point list" for `RK_LINES_INTERVAL`).
- Adding a renderer kind means editing three switches by hand and hoping
  none is missed; a missing arm is a no-op `default:`, not a diagnostic.
- The generic-slot signatures (`__renderer-floats` takes six bare
  `float`s) carry no information about which are meaningful for a kind.

The end state: each renderer kind is its own type with named, typed
fields; a `Renderer` typeclass provides `bounds` and `render` methods;
the central `switch` is replaced by coherent dispatch.

```turmeric
;; target shape
(defstruct LinesR  [points : (Vec Point)  style : LineStyle  label : cstr])
(defstruct FunctionR [f : (-> float float) x-min : float x-max : float
                      samples : int style : LineStyle label : cstr])

(definstance Renderer [LinesR]
  (bounds [r] (union-of-points (.points r)))
  (render [r ctx] (draw-polyline ctx (.points r) (.style r))))
```

---

## Non-goals

- Changing rendered output. Pixels must stay byte-identical; the existing
  tests assert on pixel content (`__count-non-background`,
  `__pixel-is-background?` in `core.tur:249-306`) and those must pass
  unchanged.
- Adding new renderer kinds or backends. There is exactly one backend
  (plutovg); the parallel-tracks note's "per-backend instances" phrasing
  is a misnomer — the dispatch axis is renderer **kind**, not output
  backend. (A second backend, if ever added, becomes a second typeclass;
  out of scope here.)
- Rewriting the intricate C drawing routines into Turmeric. The drawing
  code (adaptive sampling, tick selection, legend layout) stays in C; the
  plan only changes *how it is dispatched* and *how payload is typed*.
- Touching `plot/style.tur`'s `LineStyle`/`PointStyle`/`FillStyle`
  structs beyond importing them as field types.

---

## Strategy

The risk is concentrated in the 800-line C draw pass. The plan keeps that
code intact and attacks dispatch and payload typing around it, in two
independently-shippable halves:

1. **Refactor under the existing API (P0–P1):** split the monolithic
   `plot-into-canvas` switch into one standalone C draw function per kind
   and one bounds function per kind, still selected by the `kind` int. No
   public signature changes; pure internal de-monolithing that makes each
   arm independently callable and testable. This alone is valuable and
   reversible.
2. **Lift to types (P2–P5):** introduce per-kind structs + the `Renderer`
   typeclass whose methods call the P0 C functions, box mixed renderers
   into an existential so `(vec-of (function …) (points …))` still
   type-checks, and migrate constructors behind one-release shims.

If the type lift (half 2) hits a compiler limitation, half 1 still lands
and stands on its own.

---

## Phases

### P0 — De-monolith the draw + bounds switches (no API change)

**Tasks**
- In `plot/core.tur`, extract each `case RK_*:` body from the
  `plot-into-canvas` draw pass into a standalone `defn`
  `__draw-<kind>` taking `(canvas, renderer, viewport, bounds)` and the
  shared style/palette context. Keep them `#{Unsafe}` inline-C; move the
  per-arm code verbatim.
- Extract each `UNION_*` arm (`core.tur:692-771`) into
  `__bounds-<kind> [renderer] : BBox` (a 4-double struct or the existing
  bounds carrier).
- Replace both switches with a single thin dispatcher that still reads
  `r->kind` and calls the extracted function — behavior-preserving.
- Keep `__plot-sample-adaptive` as-is (it is already a standalone helper);
  the sampled-curve `__draw-*` functions call it.

**Acceptance**
- `tur test spices/plot/tests` passes with zero pixel-diff (existing image
  assertions unchanged).
- `plot/core.tur`'s `plot-into-canvas` no longer contains a `switch
  (r->kind)` in the draw or bounds passes — only a dispatch table.

### P1 — `BBox` and a render context value

**Tasks**
- Define `(defstruct BBox [x-min : float x-max : float y-min : float y-max : float])`
  in `plot/core.tur` (replacing the four loose doubles threaded through C).
- Define `(defstruct RenderCtx [...])` capturing what the draw pass
  computes once (viewport rect, resolved bounds, font handle, palette
  index cursor, legend position). The P0 `__draw-*` functions take a
  `RenderCtx` instead of a long positional argument list.

**Acceptance**
- `__bounds-<kind>` returns a `BBox`; the union pass folds `BBox`es.
- `__draw-<kind>` takes `(RenderCtx, renderer)`; no behavior change.

### P2 — Per-kind renderer structs + `Renderer` typeclass

**Tasks**
- For each renderer kind, define a `defstruct` carrying its real fields
  with real types, replacing the generic-slot packing. Group by source
  module: `LinesR`/`FunctionR`/`ParametricR`/`PolarR`/`InverseR`/`DensityR`
  in `plot/line.tur`; `PointsR`/`ErrorBarsR`/`VectorFieldR`/`ArrowsR` in
  `plot/point.tur`; `RectanglesR`/`AreaHistR`/`DiscreteHistR`/`StackedHistR`
  in `plot/area.tur`; the contour/decoration kinds similarly.
- Define the typeclass in `plot/core.tur` (same module as the eventual
  instances, to satisfy the orphan-instance check — mirror the ansi
  `Color` and json `Encode` precedent):

  ```turmeric
  (defclass Renderer [a]
    (bounds [r] : BBox)
    (render [r ctx : RenderCtx] : void))
  ```
- One `definstance Renderer [<KindR>]` per struct, delegating to the P0/P1
  `__bounds-<kind>` / `__draw-<kind>` functions. Each instance now reads
  named fields (`(.points r)`) instead of `data1`.

**Acceptance**
- `tur check` clean on all per-kind instances.
- A unit fixture constructs one struct of each kind and calls `bounds` and
  `render` directly (no Vec yet), producing the same pixels as the legacy
  constructor for that kind.

### P3 — Existential `Renderer` box for heterogeneous vecs — UNBLOCKED; foundation landed

The public API takes a `Vec` of mixed renderer kinds. Coherent typeclass
dispatch needs a uniform element type, so mixed kinds are boxed as an
existential carrying the `Renderer` dictionary.

**Originally blocked, now fixed in turmeric main.** The blocker reported here
(witness-indirected `open` dispatch was deferred) was resolved by a cluster of
turmeric PRs:

- **#452** -- `open`-site method dispatch now resolves through the packed
  constraint witnesses (the original blocker; the 2-instance repro that used
  to be `ambiguous method dispatch ... receiver erased to int64_t` now
  type-checks).
- **#456** -- `vec-get` preserves the existential element type (so a
  `(Vec (exists …))` round-trips through indexing).
- **#458** -- a by-value `:copy` struct payload is heap-boxed on a
  constrained `pack` (so renderer structs, not just `int`/`bool`, can be
  boxed).

**Landed (foundation):** `spices/plot/src/plot/core.tur`
- `any-renderer` macro -- `(pack x (exists [a] [(Renderer a)] a))` (the
  `exists` form must be literal at the pack site, so it is a macro, mirroring
  stdlib `showable`).
- `renderers-bbox : (Vec (exists [a] [(Renderer a)] a)) -> BBox` -- unions each
  element's `bounds` by `open`ing it and dispatching through the packed
  `Renderer` witness, i.e. the P3 payoff: the union is driven by coherent
  dispatch, not the legacy ~30-arm `kind` switch.
- `bbox-union` / `bbox-empty-box` helpers.
- Test: `tests/plot/any_renderer_test.tur` builds a heterogeneous
  `(Vec AnyRenderer)` over `LinesR`/`PointsR`/`LabelR` and checks the unioned
  box; the negative case `(any-renderer (make-struct NotR …))` fails to
  compile with `pack: no instance 'Renderer' for type 'NotR'` (verified;
  not committed as a CI fixture -- see "negative fixture" note below).

**Idioms / soft blockers the current compiler still imposes** (worked around
in the landed code, worth a turmeric report so the workarounds can be dropped
later):

1. **Vec element type is not inferred from pushes.**
   `(:: (vec-new) (Vec (exists [a] [(Renderer a)] a)))` must be ascribed;
   `vec-new` + `vec-push!` alone leaves the element type a free `A`.
2. **`letrec`-bound self-recursive closures mis-emit in C** (`'go'
   undeclared`). Iterate with a top-level recursive `defn` instead.
3. **A self-recursive `defn` that threads a by-value `:copy` struct as its
   accumulator mis-types the recursive call as the carrier `int`**
   (`then=BBox else=int`); ascribing past it then mis-emits
   (aggregate-vs-int). Worked around by folding a raw `ptr<void>` carrier and
   packing the `BBox` once at the end (`__rbb-union-corners!`).
4. **Returning a multi-field by-value `:copy` struct *parameter* directly**
   mis-emits (`incompatible types ... BBox from const BBox *`); a 2-field
   register-class struct is fine. Worked around by rebuilding the result with
   `make-struct` from the fields instead of returning the parameter.

**Negative fixture note:** plot's tests are nested (`tests/plot/`), so a
`tests/errors/` compile-fail fixture would be run (and fail) by CI's
descend-and-run path. The negative guarantee is verified by hand (above) and
documented; committing it as a CI fixture needs the plot suite flattened (as
httpd was) or the `requires.compile-fails` harness.

**Remaining P3/P4 work (not in this foundation):** the `Renderer` class still
only carries `bounds`; the `render` method over all ~30 kinds and the
migration of the public constructors to return `AnyRenderer` (so
`plot-into-canvas` consumes `(Vec AnyRenderer)` instead of int handles) are
the larger, pixel-sensitive remainder. The mechanism is now proven to work;
that migration is the next increment.

### P4 — Render the typed path; migrate constructors

**Landed (render bridge):** the typed `AnyRenderer` path is now *render-capable*
and proven pixel-identical to the legacy path, without de-monolithing the C
draw pass:
- `Renderer` gained a second method `to-legacy [r] : int` that lowers each
  typed struct to the exact slot-bag handle its legacy constructor emits
  (`LinesR`→RK_LINES 20, `PointsR`→RK_POINTS 30, `LabelR`→RK_LABEL 9).
- `anyrenderers->legacy` folds a `(Vec AnyRenderer)` into the legacy `(Vec int)`
  the draw pass consumes, dispatching `to-legacy` through each packed `Renderer`
  dictionary; `plot-anyrenderers` renders that vec through the existing `plot`
  pipeline.
- `tests/plot/render_bridge_test.tur` renders the *same* renderers through both
  the typed and legacy paths and asserts byte-identical surfaces
  (`__surfaces-equal?`) — the migration's correctness invariant, validated
  with plutovg linked.

This is the pixel-safe intermediate state: callers can build a typed,
type-checked `(Vec AnyRenderer)` and render it, while the draw code is
untouched. It exercised one more recursion-return soft blocker (a self-recursive
`defn` returning `(Vec int)` types its own call at the carrier `int`; ascribe
the recursive result).

**Done (constructor migration, 2026-10-21, turmeric-spices branch
`claude/plot-renderer-typeclass-p4`):**
- Every kind is a type in its constructor's module, with named, typed fields:
  `LinesR` `FunctionR` `ParametricR` `PolarR` `InverseR` `DensityR`
  (plot/line), `PointsR` `ErrorBarsR` `VectorFieldR` `ArrowsR` (plot/point),
  `FunctionIntervalR` `LinesIntervalR` `ParametricIntervalR` `PolarIntervalR`
  (plot/interval), `RectanglesR` `AreaHistogramR` `DiscreteHistogramR`
  `StackedHistogramR` (plot/area), `IsolineR` `ContoursR` `ContourIntervalsR`
  `ColorFieldR` (plot/contour), and in plot/decor a `Decoration` sum type
  (`XAxis` `YAxis` `Axes` `XTickLines` `YTickLines` `TickGrid` -- a
  `defstruct` cannot be empty) plus `HRuleR` `VRuleR` `LabelR`. Instances live
  with the types, which the orphan check allows.
- `(defalias AnyRenderer (exists [a] [(Renderer a)] a))`. Class names and
  aliases are global, so neither is exported by name; `AnyRenderer` resolves
  in every module.
- Every public constructor returns `AnyRenderer`; `plot`, `plot-write-png`,
  `plot-into-canvas`, `render` and `Backend`'s `render-to` take a
  `(Vec AnyRenderer)`. `plot-into-canvas` folds `renderers-bbox`, lowers with
  `anyrenderers->legacy`, hands both to the C frame worker
  (`__plot-into-canvas-legacy`), and frees the lowered handles.
- The bounds pass is typeclass dispatch: `__renderer-bbox` and its `switch`
  are deleted; each kind's `bounds` calls one per-shape helper
  (`__point-list-bbox`, `__error-bars-bbox`, `__arrows-bbox`, `__rects-bbox`,
  `__discrete-hist-bbox`, `__stacked-hist-bbox`) or `point-bbox` /
  `hint-bbox`. `bbox-union` keeps the left box on a tie, as the C union kept
  the first point, so the fold is bit-identical.
- Curve callbacks are typed function values (`^fat f : (fn [float] float)`,
  `(fn [float] int)` for a point, `(fn [float float] float|int)` for fields);
  the draw pass calls the closure handle with `TUR_APPLY1_T` / `TUR_APPLY2_T`.
  `fn` literals, capturing or not, now work. (A plain, non-`^fat` fn param
  cannot be stored in a struct field: turmeric
  `docs/reported/fn-param-stored-in-struct-field-miscompiles.md`.)
- The folds are top-level recursive `defn`s again: as `letrec` closures they
  leaked their env and a struct box per call on every `plot`
  (`docs/reported/letrec-struct-arg-and-env-leak.md`).
- Verified: a program rendering all 30 kinds in 34 scenes through the public
  API compiled unchanged against both versions; 33 PNGs are byte-identical.
  `arrows.png` differs because `arrows`' bounds read each arrow's second cons
  cell (a pointer) as y -- a heap-dependent, run-to-run-varying y range;
  `ArrowsR` spans both endpoints. Suite green on the Release and the Debug
  (ASan) compiler; the migrated path is ASan/UBSan/LSan clean (the remaining
  leaks are the `ptr<void>` result envelopes, which predate it).

**Done (per-kind draw, 2026-10-21, same branch):**
- `Renderer` gained `draw [r : a  ctx : DrawCtx] : void` (named `draw`, as
  `render` is the Backend entry point) and three methods with defaults for
  what the frame needs: `legend-label` (""), `frame-marks` (a `FrameMarks`
  of four bools; only `Decoration` overrides it) and `categories`
  (`(Option int)`; the two categorical histograms override it). `to-legacy`
  is gone.
- Each arm of `__draw-renderers`' switch moved verbatim into an inline-C
  draw function beside its kind (24 functions across plot/line, point,
  interval, area, contour, decor), wrapped in `do { } while (0)` so the arms'
  `break`s keep their meaning. What the arms shared -- SET_LINE_STYLE /
  SET_FILL_STYLE / SET_POINT_STYLE / MAP_POINT -- became `export-as` helpers
  in plot/core (`plot_ctx_line_style` etc.) behind same-named local macros,
  and the auto-color cursor moved into the `DrawCtx`. The three sampled
  interval kinds share one kernel (`plot_draw_curve_interval`) keyed by a
  curve-evaluation mode, as the sampler is.
- The frame worker is `__plot-frame` (returns the `DrawCtx`, or NULL when
  the viewport is too small -- the font it loaded is now freed then), the
  typed draw fold inside `__ctx-clip-begin` / `-end`, and the legend rows.
- `__make-renderer`, the slot setters, `anyrenderers->legacy` and
  `__free-renderers` are deleted. `plot/core` is 1748 lines (from 2081);
  the six kind modules carry their draws.
- A `:c-sources` C file was considered for the draw code and rejected:
  `export-as` functions are `static`, so separate C could not reach the
  closure-calling helpers, and vendored C cannot load in the in-process
  engine.
- Verified as before: 34 scenes byte-identical to the previous step; shards
  green on both compilers; ASan/UBSan clean.

**Done (typed styles, 2026-10-21, same branch):** `LineStyle`, `PointStyle`,
`FillStyle` and `PlotOpts` are `defopaque` over `ptr<void>` in plot/style,
built by raw `ptr<void>` producers wrapped with `::` (msgpack's Buf idiom).
Every field, constructor and entry point takes the kind it draws with, so a
`FillStyle` where a `LineStyle` is due is TUR-E0001
(`errors/wrong-style-kind.tur`). Fill lists are `(Vec FillStyle)`; their
draws walk the Vec by index. The C-ABI helpers (`plot_ctx_*_style`,
`plot_draw_curve_interval`) keep word-sized style parameters, since C callers
declare their prototypes and an opaque's C type is the compiler's choice. 34
scenes byte-identical.

**Done (typed data payloads, 2026-10-21, same branch):** a new module,
`plot/data`, defines `Point`, `Rect`, `ErrorBar`, `Arrow`, `Bar` and `Group`
(`:copy` structs; `Group` holds a `(Vec float)`), and every data-taking
constructor takes a `(Vec <record>)` -- `density` a `(Vec float)`,
`stacked-histogram`'s layer names a `(Vec cstr)`. Handing a renderer the
wrong record is TUR-E0001 (`errors/wrong-data-record.tur`).
- The C draws and bounds helpers are unchanged. Of the two shapes weighed
  (C reading the Vec's boxed-element layout, or a Turmeric adapter), the
  adapter was taken: each instance's `draw` / `bounds` lowers its Vec to the
  cons cells the C reads (`__points->cells` and friends, an opaque `Cells`),
  and frees them after. The boxed layout stays a compiler detail.
- `parametric` / `parametric-interval` take a `(fn [float] Point)` and
  `vector-field` a `(fn [float float] Point)`. The C calls them through two
  Turmeric-bodied `export-as` shims (`plot_eval_point`,
  `plot_eval_vector`) that apply the closure and store the fields through
  out-pointers, so no cons point is allocated per sample.
- The `categories` method (an `(Option int)` over the raw bar list) is
  split into two defaulted methods, `categorical?` and
  `category-labels : (Vec cstr)`, which the frame lowers for its tick labels;
  `error-bars` / `discrete-histogram` / `stacked-histogram` take
  `horizontal : bool` for their `invert : int`.
- Verified: 34 scenes byte-identical to the typed-styles step; shard green
  on release and Debug `tur` (11 tests, 5 compile-fail fixtures);
  ASan/UBSan clean, and the per-sample callback leaks are gone.

**Done (the two preserved bugs, 2026-10-21, same branch):**
- `contours` stored 0.0 for both ends of its level range, so all `n` levels
  drew the 0 contour. The draw now scans f over the sampled grid (NaN and
  infinities skipped) and spaces level i of n at
  min + (max - min) * (i + 1) / (n + 1). Only `contours.png` of the 34
  scenes changes.
- The frame `plutovg_canvas_save`d with no matching `restore`, leaving a
  saved state on a `CanvasBackend` caller's canvas; `__ctx-finish` restores
  it. Both have a test that fails on the old code.
- Under clang every plot test failed to build: the existential's
  carrier-adapter thunk for the defaulted `category-labels : (Vec cstr)`
  returned the instance's int64 carrier as `tur_adt_Vec__cstr *`
  (-Wint-conversion, an error to clang; a warning to gcc). Fixed in the
  compiler on its own branch, `claude/exbox-thunk-carrier-result`
  (fixture `exists-method-returns-carrier-vec`), which must merge before the
  spice is green on macOS.

**Done (legend swatches, 2026-10-21, same branch):**
- The legend swatch was `palette[row % 8]`, not what its renderer drew
  with. The three style helpers now record the first line, fill and point
  style each renderer resolves (auto colors included) into a per-renderer
  swatch the `DrawCtx` carries; the legend draws the point marker (now one
  `plot_ctx_marker` the points draw shares), else a fill box edged in the
  line style, else a line stroke. `contour-intervals` notes its first band.
  No `Renderer` method was needed: the swatch is a by-product of `draw`.
- Reviewing the band swatches showed the interval kinds never drew their
  outline (`plutovg_canvas_fill` clears the path the stroke then used); they
  `fill_preserve` now. A point `fill-color` or contour-band color of 8 or
  more indexed past the palette; both wrap.
- Pixels: 25 scenes change only in the swatch column, the five interval
  scenes gain outlines, nothing else moves. `legend_test` reads swatches
  back from the surface; it and the outline check fail on the old code.

**Done (stacked layers in the legend, 2026-10-21, same branch):**
- `Renderer` gained `legend-labels : (Vec cstr)` -- a renderer's legend
  rows, fresh for the caller to free -- whose default is its one
  `legend-label` (a default method body calling a sibling method works
  through the existential, under clang too). `StackedHistogramR` overrides
  it with a copy of its layer names. Swatches are per series: a draw calls
  `plot_ctx_series(dc, k)` before series k, and row k shows that swatch.
- An auto-colored stack layer re-resolved its fill for every cell, so a
  layer changed color from group to group; the draw now goes layer by
  layer, one resolve each. `discrete-histogram` and `rectangles` resolved
  their single fill per bar the same way; once now.
- A horizontal histogram's categories labelled the x axis, where its values
  run. A defaulted `categories-on-y?` moves them to y for the two
  histograms when `horizontal`.
- Pixels: only the five histogram / rectangle scenes and
  `categorical-mixed` (whose rule took the next auto color) change.

**Done (legend readability, 2026-10-21, same branch):** the legend is
measured before it is drawn (`__legend-size`: rows and the widest label, via
`plutovg_canvas_text_extents`) and sits on a translucent white box
(`__legend-box`). Knowing its size fixed bottom legends, which grew down out
of the plot, and long right-hand labels, which ran off its edge; and
`legend-pos` 0 now means no legend, as documented (the frame read it as 1).

**Remaining (follow-ups, not in this plan's acceptance):**
- `plot-anyrenderers` is a deprecated alias of `plot`, kept for one
  release.

**Acceptance**
- The typed path is pixel-identical to legacy `plot` -- done for every kind
  (33/34 scenes byte-identical; `arrows` is a bug fix; `contours` changed
  later by its own fix).
- Every example under `spices/plot/` and the guide compiles against the typed
  constructors -- done, and `__make-renderer` is removed.

### P5 — Tests, negative fixtures, image round-trip

> **Done 2026-10-21 with the P4 migration:** `tests/plot/renderer_test.tur`
> pins every kind's `bounds`; `tests/plot/callback_test.tur` checks `fn`
> literals draw what defns draw and that re-rendering is stable;
> `spices/plot/errors/run.sh` (the shard's errors stage) asserts five
> compile-fail fixtures with witness substrings -- no `Renderer` instance,
> a `(Vec int)` passed to `plot`, a mistyped callback, and (with the typed
> styles and data) a `FillStyle` where a `LineStyle` is due and `Rect`s
> where `Point`s are due. A per-kind
> construct→render→pixel-count check is not added: the line / point / area /
> interval / contour suites already render every kind.

**Tasks**
- Per-kind: keep the existing pixel-content assertions; add a
  construct→render→pixel-count check per `*R` struct.
- Add negative fixtures (gated on the P5 negative-fixture harness from the
  uplift plan, if available; otherwise a `tur check`-expected-fail script):
  - wrong field type in a constructor is a type error, not a slot mishap;
  - a non-`Renderer` value in the renderer vec is rejected.
- Wire all of the above into the spice's `tests/` so CI covers them.

**Acceptance**
- `tur test spices/plot/tests` green; pixel output identical to pre-plan.
- Negative fixtures fail to compile with clear diagnostics.

### Dependency graph

```
P0 (de-monolith C) ─► P1 (BBox/RenderCtx) ─► P2 (structs + class)
                                                   │
                                                   ▼
                                              P3 (existential box)
                                                   │
                                                   ▼
                                              P4 (migrate ctors) ─► P5 (tests)
```

P0 and P1 are pure refactors and ship on their own. P2 blocks on P1; P3
blocks on P2; P4/P5 close it out.

---

## Risks and open questions

1. **Existential boxing support. — MATERIALIZED, then RESOLVED.** This was
   the compiler dependency the plan flagged: ECS's PR #17 existential hides a
   *size index* (`exists [n']`), not a *type behind a constraint*, and
   `open`-site dispatch through packed witnesses was deferred. It was reported
   to the turmeric repo and fixed by PRs #452 (witness-indirected `open`
   dispatch) / #456 (`vec-get` element-type preservation) / #458 (by-value
   struct payload pack). The P3 foundation (`any-renderer`, `renderers-bbox`)
   now builds and dispatches heterogeneously on main. Four narrower
   codegen/inference rough edges remain (vec element-type inference; `letrec`
   closure emit; struct-accumulator recursion; multi-field `:copy` struct
   param return) — all worked around in the landed code and enumerated under
   P3; each is worth its own turmeric report so the workarounds can be removed.

2. **Pixel-identity regression budget.** The draw code is sensitive
   (adaptive subdivision thresholds, tick rounding). The plan's
   verbatim-move discipline in P0 is what protects this; review P0 as a
   pure cut-paste with no edits to arithmetic.

3. **Field-count ergonomics.** Some kinds have many fields
   (`StackedHistR`). `make-struct` positional construction gets unwieldy;
   consider whether a small builder is worth it (this overlaps U6 typed
   variadic builders — defer unless it blocks P2).

4. **`text`/`label` ownership.** `__make-renderer` deep-copies `label`
   and `text` (`core.tur:80-93`). The typed structs should hold `cstr`
   with the same ownership contract; confirm GC vs malloc lifetime when
   the struct replaces the C-owned copy. **Settled in P4:** a typed struct
   holds the caller's `cstr`; `to-legacy` deep-copies it into the slot bag
   for one draw and `__free-renderers` frees the copy after.

---

## Acceptance (whole plan)

- `tur test spices/plot/tests` is green with byte-identical rendered
  output before and after.
- `plot-into-canvas` contains no `switch (r->kind)`; renderer dispatch is
  via the `Renderer` typeclass.
- Each renderer kind is a `defstruct` with named, typed fields; the
  generic `data1..5 / f1..6 / i1..4` slot bag is gone from the public
  constructor path.
- A negative fixture demonstrates that a mistyped renderer field or a
  non-`Renderer` vec element is a compile-time error.
