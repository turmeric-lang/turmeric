## Spice Plan: tur-signal

> **Status:** Draft Plan
> **Last Updated:** 2026-05-24
> **Type:** Spice Extraction

---

## Overview

Move the four `stdlib/signal/*.tur` modules out of the core stdlib and into a
new pure-Turmeric spice in the `turmeric-spices` monorepo. Final shape:

| Spice | Tag | Depends on | Purpose |
|-------|-----|------------|---------|
| `tur-signal` | `signal-v0.1.0` | `arrow` (stdlib) | Arrow-based signal processing: SF combinators, DSP filters, ADSR envelopes, synth voices |

`tur-signal` is **Tier 1** (pure Turmeric, no `cmake-dep`, no inline-C beyond
what already exists in the source files for IEEE-754 bit-pattern helpers).
Modeled after [`tur-frame`](frame-spice-plan.md) and the existing
[`tur-scscm`](https://github.com/turmeric-lang/turmeric-spices/tree/main/spices/scscm)
layout (single spice, nested module sub-tree).

### Why move it

1. **Stdlib churn.** Signal processing is a niche library; keeping it in the
   stdlib forces every Turmeric user to carry it and pays the per-file
   typecheck cost on every `tur check` of the core distribution.
2. **Independent versioning.** The SF Arrow story is still evolving
   (`docs/archive/signal-processing-arrows-plan.md`); a spice can iterate on
   its own tag cadence without bumping the core release.
3. **Consistent with peers.** `scscm`, `tidal`, `math`, `frame` are all
   domain libraries already living in `turmeric-spices`. `signal` is the
   same shape -- pure Turmeric, arrow-typeclass-flavored, single owner.

### Source files (in repo today)

| File | Lines | Imports |
|------|------:|---------|
| `stdlib/signal/core.tur`     | 115 | (none) |
| `stdlib/signal/dsp.tur`      | 253 | (none) |
| `stdlib/signal/envelope.tur` |  88 | (none) |
| `stdlib/signal/synth.tur`    | 609 | `signal/core`, `signal/dsp`, `signal/envelope` |

The only cross-file dependency is `synth.tur` -> the other three. The
modules reference the `arrow` stdlib module (via `__arrow_call1` / `__arrow_call2`
helpers) which stays in stdlib.

---

## Target layout in `turmeric-spices`

```
../turmeric-spices/spices/signal/
├── build.tur
├── README.md
├── spices/                 # vendored test spice (mirrors tur-frame layout)
│   └── (empty or test only)
└── src/
    └── signal/
        ├── core.tur
        ├── dsp.tur
        ├── envelope.tur
        └── synth.tur
```

The nested `src/signal/...` form makes consumer imports read `(import signal/core)`,
matching how `tur-frame` exposes `frame/buffer`, `frame/column`, etc.

### `build.tur` skeleton

```turmeric
(defpackage tur-signal
  :name        "tur-signal"
  :version     "0.1.0"
  :description "Arrow-based signal processing: SF combinators, DSP, envelopes, synth voices"
  :license     "MIT"
  :spices #{
    "test" #{:url    "https://github.com/turmeric-lang/turmeric-spices"
             :ref    "test-v0.1.0"
             :subdir "spices/test"
             :optional true}
  }
  :exports #{
    "signal/core"     ["constant" "time-signal" "sample" "map-signal"
                       "lift" "compose-signals" ...]
    "signal/dsp"      ["sine-wave" "square-wave" "saw-wave" "triangle-wave"
                       "low-pass" "high-pass" "svf-low-pass" ...]
    "signal/envelope" ["adsr" "adsr-params" "gate->envelope" ...]
    "signal/synth"    ["voice" "poly-voice" ...]
  })
```

Final export lists are filled in by grepping public `defn` / `defmacro` /
`defstruct` symbols out of each source file during the move (skip `__`-prefixed
internals).

---

## Migration steps

### 1. Bootstrap the spice (in `../turmeric-spices/`)

1. `mkdir -p spices/signal/src/signal spices/signal/spices`
2. Copy `stdlib/signal/{core,dsp,envelope,synth}.tur` to
   `spices/signal/src/signal/`.
3. In `synth.tur`, rewrite the three imports from
   `(import stdlib/signal/X.tur)` to `(import signal/X)` (drop the
   `stdlib/` prefix and the `.tur` suffix; this is the module-search-path
   form used by other spices).
4. Wrap each file in a `(defmodule signal/<name> (export ...))` form to
   match the json/frame spice convention (currently the stdlib files are
   bare top-level forms).
5. Write `build.tur` with the export table (see skeleton above).
6. Write `README.md` modeled on `spices/frame/README.md`: overview,
   install snippet (`:spices { "signal" {...} }`), quick-start example.
7. Add `requires.typecheck-skip` only if there are unresolved typeclass
   issues at spice-check time (frame and raylib both ship this marker).

### 2. Tests for the spice

Two options; pick **a** for v0.1.0 unless arrow_tests becomes hard to port:

a. **Move `tests/tip/arrow_tests.tur`** into
   `spices/signal/tests/arrow_tests.tur`. This file already uses the
   stripped `(import signal/core)` form, so it is the canonical target.
   Wire it up via the `tur-test` runner pattern used by `spices/frame/tests`.

b. **Doctests only**: rely on `tools/doctest.py` to regenerate from the
   `;;;` Example blocks. The spice would need its own doctest runner
   target (small justfile addition in `turmeric-spices`).

Cover at least one ADSR shape, one filter sanity check, one oscillator
period check.

### 3. Add to the `turmeric-spices` monorepo README

Insert a new row in `../turmeric-spices/README.md`:

```
| [`tur-signal`](spices/signal/) | Arrow-based signal processing (SF, DSP, ADSR, synth) | 1 -- pure Turmeric | -- |
```

Sort placement: alongside `tur-scscm` / `tur-tidal` (audio-adjacent Tier 1).

### 4. Tag and publish

```sh
cd ../turmeric-spices
git checkout -b signal-spice
# ... commits ...
git tag signal-v0.1.0
git push origin signal-spice signal-v0.1.0
```

The release tag is what consumers pin via `:ref "signal-v0.1.0"`.

### 5. Update the turmeric repo (this checkout)

After the spice is published and verified end-to-end with `tur fetch`:

1. **Delete** `stdlib/signal/` entirely.
2. **Delete** `tests/doctest-generated/signal-core.tur` (regenerated by
   `tools/doctest.py`; once the source files are gone it won't recreate it).
3. **Delete** the old `tests/arrow_tests.tur` if `tests/tip/arrow_tests.tur`
   covers the same ground after migration (both currently exist; the
   `tip/` variant uses the new-style imports and is the keeper). Otherwise
   port the remaining cases into the spice's test file.
4. **Move** the entire `examples/signal-processing/` directory into
   `../turmeric-spices/spices/signal/examples/`. The examples live with the
   spice they exercise -- consistent with `raylib`, `opengl`, and other
   spices that ship their own example trees. During the move:
   - Rewrite `(import stdlib/signal/X.tur)` -> `(import signal/X)` in
     `02_signals.tur` and `03_dsp.tur`.
   - `01_basics.tur` has no `signal/` import; move it for cohesion but no
     rewrite needed.
   - The examples then run via `tur run examples/02_signals.tur` from
     inside `spices/signal/`, where `build.tur` already declares the
     module search path -- no per-example `:spices` block required.
5. **Justfile**: remove the `run-signal-processing-*` targets
   (`justfile:133-140`). They referenced `examples/signal-processing/`
   which no longer exists in this repo. If we want a one-liner to drive
   the spice examples, add a target like
   `run-spice-signal-example name="01_basics": @cd ../turmeric-spices/spices/signal && tur run examples/{{name}}.tur`
   -- optional, low priority.
6. **Docs sweep**:
   - `docs/programming-turmeric-plan.md:136,301` -- these mention "signal/"
     for POSIX signal handling, not our DSP module. Unrelated; leave alone.
   - `docs/archive/stubs-and-workarounds.md:396-404` -- update the file path
     to point at the new spice location (or note the migration).
   - `docs/archive/autodoc-plan.md:82` -- remove `signal/` from the stdlib
     subdir list.
   - `docs/archive/signal-processing-arrows-plan.md` -- prepend a
     "**Status: extracted to `tur-signal` spice as of v0.1.0**" banner.
     Don't rewrite paths; it's archived.

### 6. Doctest pipeline

`tools/doctest.py` walks `stdlib/` for `;;;` Example blocks. Once
`stdlib/signal/` is gone the generated `tests/doctest-generated/signal-*.tur`
files vanish on the next `python3 tools/doctest.py stdlib/`. No code
change required. If we want doctest coverage in the spice itself, run
`python3 ../turmeric/tools/doctest.py src/signal/ --out tests/doctest/`
from inside the spice and check in the result.

### 7. CLAUDE.md / per-spice docs

The spice gets its own `CLAUDE.md` only if the agent loop will operate
inside `../turmeric-spices/spices/signal/` (see `docs/per-spice-docs-plan.md`
for the convention). For v0.1.0 a `README.md` is sufficient.

---

## Import-path compatibility

Old (stdlib):

```turmeric
(import stdlib/signal/core.tur)
(import stdlib/signal/dsp.tur)
```

New (spice, via auto-discovery from `:spices` manifest):

```turmeric
(import signal/core)
(import signal/dsp)
```

There is **no backwards-compatible shim** -- stdlib will not re-export the
spice. This is consistent with how `frame`, `json`, `raylib` etc. are
imported. Consumers must either:

- Add `signal` to their `build.tur :spices` block, or
- Use `tur run` and let auto-discovery handle it (per `CLAUDE.md`
  "Per-file Commands Inside a Spice").

Call sites in this repo that need updating (full list from grep):

| Path | Action |
|------|--------|
| `tests/arrow_tests.tur` | delete or port (uses old `stdlib/signal/...tur` form) |
| `tests/tip/arrow_tests.tur` | move into the spice as its tests |
| `tests/doctest-generated/signal-core.tur` | delete (auto-regenerated) |
| `examples/signal-processing/` (whole dir) | move to `spices/signal/examples/`; rewrite imports in `02_signals.tur` and `03_dsp.tur` |
| `justfile:133-140` (`run-signal-processing-*` targets) | delete |

---

## Rollout order (so nothing breaks)

1. PR 1 (turmeric-spices): land `spices/signal/` with tests passing.
   Tag `signal-v0.1.0`.
2. PR 2 (turmeric): rewrite examples + tests to depend on the published
   spice. CI now exercises the spice path end-to-end.
3. PR 3 (turmeric): delete `stdlib/signal/` and stale generated artifacts.
   Keep this PR small and reversible.

Splitting 2 and 3 means there is a window where both the stdlib and spice
copies coexist; the order keeps `main` green at every step.

---

## Open questions

- **Does the SF Arrow story require a stdlib hook?** `arrow.tur` provides
  `__arrow_call1` / `__arrow_call2` which `signal/core.tur` calls. These
  stay in stdlib (they're general-purpose Arrow plumbing). Confirm
  during the move that no other internal helper needs to follow.
- **ADSR contract types.** `envelope.tur` defines `ADSRParams`; check
  whether any stdlib code outside `signal/` uses it. Grep says no, but
  worth a final pass before deletion.
---

## Acceptance criteria

- [ ] `../turmeric-spices/spices/signal/` exists with `build.tur`, `README.md`,
      `src/signal/{core,dsp,envelope,synth}.tur`.
- [ ] `signal-v0.1.0` git tag pushed to `turmeric-spices`.
- [ ] `cd ../turmeric-spices/spices/signal && tur check src/signal/synth.tur`
      succeeds (or fails only on documented typeclass stubs gated by
      `requires.typecheck-skip`).
- [ ] At least one spice-side test runs green via `tur-test`.
- [ ] `cd ../turmeric-spices/spices/signal && tur run examples/03_dsp.tur`
      runs cleanly.
- [ ] `stdlib/signal/` and `examples/signal-processing/` deleted from this
      repo; `just build && just test` green.
- [ ] `turmeric-spices/README.md` lists `tur-signal` in the spice table.
