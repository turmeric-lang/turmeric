# Plan: `plot` Renderer Typeclass Collapse (U2, v1)

> Status: P0–P2 landed (per-kind structs, `Renderer` `bounds`, `Backend`
> output collapse). **P3 foundation landed** (existential `AnyRenderer` +
> `renderers-bbox`) after the turmeric blocker was fixed (PRs #452/#456/#458).
> **P4 render bridge landed**: `to-legacy` + `plot-anyrenderers` render a typed
> `(Vec AnyRenderer)` through the existing pipeline, validated pixel-identical
> to legacy `plot` (see P4). The remaining ~27 per-kind structs + the
> constructor migration + the eventual per-kind `render` C de-monolith are the
> pixel-sensitive remainder. Soft codegen/inference blockers worked around in
> the landed code are listed under P3/P4.
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

**Remaining (the larger, pixel-sensitive migration):**
- Rewrite the public constructors in `plot/line.tur`, `point.tur`,
  `area.tur`, `contour.tur`, `interval.tur`, `decor.tur` to build the
  typed struct and return `AnyRenderer` (currently they return the legacy
  `int` handle; `plot-anyrenderers` bridges the other direction).
- Add the remaining ~27 per-kind structs + `Renderer` instances (only
  `LinesR`/`PointsR`/`LabelR` exist today) with `bounds` + `to-legacy`.
- Eventually replace `to-legacy` + the monolithic `__draw-renderers` switch
  with a per-kind `render` method (the P0-per-kind C de-monolith), at which
  point the slot-bag handle and `__make-renderer` can be retired.
- Keep `__make-renderer` and the generic-slot helpers exported but
  deprecated for one release; update the README / `plutovg-guide.md` examples.

**Acceptance**
- `plot-anyrenderers` is pixel-identical to legacy `plot` for the typed kinds
  (done for `LinesR`/`PointsR`/`LabelR`).
- Once constructors migrate: every example under `spices/plot/` and the guide
  compiles against the typed constructors; removing `__make-renderer` is the
  only follow-up.

### P5 — Tests, negative fixtures, image round-trip

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
   the struct replaces the C-owned copy.

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
