---
title: Signal Primitives Expansion Plan (SUPERSEDED)
category: Archive
description: Original plan to flesh out tur-signal with resonant filters, bitcrusher, saturation, comb filter, real wavetable oscillator, generic lookup table, scale, a working FM synth example, and FFT primitives. Archived after the tur-signal spice was found to no longer build against the current type system; superseded by language-readiness-for-typed-signal-plan + tur-signal-rebuild-plan + stdlib-arrow-scaleback-plan.
---

# `tur/signal` Primitives Expansion -- Plan (SUPERSEDED)

> **Status: superseded and archived.**
>
> This plan assumed the existing `tur-signal` spice was a working baseline
> that just needed a `:float` sample migration (Phase 0) plus a long list
> of new primitives. Execution attempts revealed that the spice did not
> compile against the current `tur` / stdlib at all -- the underlying
> ABI it depended on (raw `int64_t(*)(int64_t)` arrow casts, untyped pair
> handles, removed stdlib symbols) had moved on. Patching the spice
> forward reintroduced exactly the patterns the typeclass + fat-closure
> work eliminated.
>
> The spice was removed; the rebuild and the language work it depends on
> are now tracked separately:
>
> - `docs/upcoming/language-readiness-for-typed-signal-plan.md` --
>   the spike-style investigation of language/stdlib gaps that any
>   typed signal library needs.
> - `docs/upcoming/tur-signal-rebuild-plan.md` -- the rebuild,
>   gated on the readiness plan's verdicts.
> - `docs/upcoming/stdlib-arrow-scaleback-plan.md` -- removes the
>   disabled Arrow typeclass scaffolding from stdlib while keeping the
>   bare-function combinators the rebuild depends on.
> - `docs/reported/signal-spice-broken-build.md` -- the report that
>   triggered the rescope.
>
> Below is the original plan text, preserved for reference. Do not
> execute it.

---



## Goal

Round out the `tur-signal` spice (in the sibling repo
`../turmeric-spices/spices/signal/`) from "enough surface area to demo arrows"
to "enough DSP to build small synths and analyzers". Specifically, add:

- **Resonant filters**: 3-pole (-18 dB/oct) and 4-pole (-24 dB/oct) ladder LPF
  with a resonance parameter.
- **Bitcrusher**: bit-depth + sample-rate reduction.
- **Saturation**: soft-clip / `tanh`-style waveshaper.
- **Comb filter**: feedforward + feedback variants (the building block under
  Karplus-Strong, plus stand-alone use as flange/chorus/resonator).
- **Wavetable oscillator** that is *actually* a wavetable (the current
  `wavetable-osc` is a stub that calls `sine`).
- **Lookup table** primitive (generic linear-interp table read; wavetable
  becomes a thin wrapper over it).
- **`scale`** (a.k.a. `lerp-range` / `map-range`): linearly remap `n` from
  `[in-low, in-high]` to `[out-low, out-high]`.
- **FM synth example** that produces a recognisable bell/brass tone (the
  current `fm-voice` is a one-liner that calls `voice` with the modulator
  frequency stuffed into the filter-cutoff slot -- it does not actually do FM).
- **FFT primitives**: forward/inverse complex and real FFTs, plus a windowing
  helper. Picks: KissFFT vendored under `signal/c/` vs. a small in-tree
  radix-2 implementation. This plan recommends both, in phases (see below).

## Motivation

The existing `signal/synth.tur` advertises a long export list -- `fm-voice`,
`wavetable-osc`, `sine-wavetable`, `karplus-strong`, `granular`, ... -- but
most are placeholders:

| Export | Today (`signal/synth.tur`) | Status |
|---|---|---|
| `fm-voice` | `(voice carrier-freq params gate-sig mod-index modulator-freq)` | Stub -- ignores `modulator-freq` semantically |
| `wavetable-osc` | `(sine freq 0.0)` | Stub |
| `sine-wavetable` | `vec` filled with `0` | Stub |
| `square-wavetable` | `vec` of `(:: 1.0 :int)` / `(:: -1.0 :int)` | Half-stub |
| `sawtooth-wavetable` | `vec` filled with `0` | Stub |
| `granular` | returns `source` unchanged | Stub |
| `karplus-strong` | `(sawtooth freq)` | Stub |

So a "fix the stubs" pass is overdue *anyway*. This plan adds the new
primitives the user asked for AND uses the same C-helper plumbing to
replace the stubs with real implementations.

The filter situation is similar: `signal/dsp.tur` only has a first-order
EMA (`low-pass`, `high-pass`). For anything synth-y, a 1-pole LPF without
resonance is not enough -- hence 3-pole and 4-pole ladder LPFs.

## Full stub audit -- everything currently lying about its semantics

The user-asked-for primitives overlap with a broader pile of placeholders
that should be fixed in the same pass. Everything in this table currently
either returns the wrong shape or silently ignores an argument; users
calling these get a function-shaped object that doesn't do what its name
or docstring claims.

| Symbol | File:line | Current body | Real behaviour needed |
|---|---|---|---|
| `svf-low-pass` | `synth.tur:25` | `(low-pass cutoff)` -- ignores `q` | Resonant SVF (state-variable filter), or alias to `ladder-lpf-2p` once that exists |
| `adsr-gen` | `synth.tur:22` | `(adsr-fixed params 1.0)` -- hard-codes a 1.0 s gate, ignores the actual `gate-sig` | Gate-driven ADSR that re-triggers on rising edge and releases on falling edge |
| `fm-voice` | `synth.tur:147` | Calls `voice` with `mod-index` in the filter-cutoff slot -- not FM at all | Two-op PM/FM as sketched below |
| `wavetable-osc` | `synth.tur:165` | `(sine freq 0.0)` -- ignores the table | Phase-accumulator + `lut-read` from the table |
| `sine-wavetable` | `synth.tur:178` | `vec-set!` with `0` in every slot | Generate one cycle of `sin(2*pi*i/N)` |
| `sawtooth-wavetable` | `synth.tur:210` | `vec-set!` with `0` in every slot | Generate one cycle of ramp in `[-1, 1)` |
| `square-wavetable` | `synth.tur:194` | Half = `1.0`, half = `-1.0` -- ignores the `duty` argument | Honour `duty`: first `floor(N*duty)` slots `+1`, rest `-1` |
| `granular` | `synth.tur:258` | `source` (returns input unchanged) | Real grain scheduler with grain-rate, grain-duration, randomness |
| `additive-voice` | `synth.tur:278` | `(voice base-freq params gate-sig 0.5 0.5)` -- ignores `harmonics` | Sum of sines weighted by `(amplitude, frequency-multiple)` pairs |
| `karplus-strong` | `synth.tur:297` | `(sawtooth freq)` -- not even feedback | Noise-burst -> `comb-fb delay decay` (see comb-filter section) |
| `step-sequencer` | `synth.tur:435` | Plays `vec-get notes 0` as a `lead-synth` -- ignores tempo, gate-duration, and the rest of the vector | Time-based note dispatch driven by `tempo` and per-note duration |
| `left-signal` | `core.tur:92` | `sig-a` unchanged | Either-typed wrapper -- explicitly deferred until Left/Either lands. **Keep as documented limitation, not part of this plan.** |
| `right-signal` | `core.tur:104` | `sig-b` unchanged | Same as above. |

### Missing functions referenced by examples

`examples/03_dsp.tur` calls **functions that don't exist anywhere**:

- `triangle` (oscillator)
- `offset` (DC offset SF)
- `invert` (sign flip)
- `abs-sf` (absolute value SF)
- `multiply` (pointwise product on a Pair signal)
- `clip lo hi` (asymmetric hard clip; distinct from the symmetric
  `hard-clip` in `synth.tur:28`)

The example must currently fail to compile. Either the example is dead
code (delete it) or the functions are real omissions (implement them).
**Default: implement them** -- they are all sub-10-line SFs and the
example is an entry point users hit early.

`noise-burst <duration>` (white noise gated by an envelope) is similarly
needed by the real `karplus-strong` rewrite. Adding it lets several
other patches simplify.

### Phasing for the audit fixes

The stub fixes interleave naturally with the user-asked-for primitives:

- The small missing oscillators / shapers (`triangle`, `offset`, `invert`,
  `abs-sf`, `multiply`, `clip`, `noise-burst`) land in Phase 1.5 alongside
  `scale` and `saturate-*` -- they are all small pure SFs.
- `square-wavetable` duty-cycle fix lands with the wavetable phase
  (Phase 9).
- `sine-wavetable` / `sawtooth-wavetable` real generators land in Phase 9.
- `wavetable-osc` real implementation lands in Phase 9.
- `karplus-strong` rewrite lands in Phase 7 (immediately after `comb-fb`).
- `fm-voice` rewrite lands in Phase 10 with the FM example.
- `adsr-gen` gate-driven rewrite lands in a new Phase 8.5 (it gates
  `voice`/`fm-voice`/`wavetable-voice` correctness, so it should land
  before the FM example).
- `additive-voice`, `granular`, `step-sequencer` are bigger rewrites with
  no dependents inside this plan; they get a new Phase 14 "scheduler /
  harmonic / grain rewrites".
- `svf-low-pass` -- decide whether to remove it (callers can use
  `ladder-lpf-4p` directly) or alias it to a real 2-pole SVF. Default:
  remove, since nothing user-facing relies on the specific 2-pole shape.

## Background -- how SFs are wired today

A Signal Function `SF a b` is a curried function:

```
SF a b = (Time -> a) -> (Time -> b)
```

i.e. `(fn [sig] (fn [t] ...))`. The convention in `signal/dsp.tur` is that
the outer closure captures parameters, the middle closure captures the
input signal *and any per-instance mutable state* (allocated via a C helper
like `__dsp_alloc_state`), and the inner `(fn [t] ...)` reads/writes that
state on each sample.

State cells are raw `double*` allocated with `calloc` and passed around as
`:int` handles. Inline-C `memcpy`'s reinterpret the `:int` sample value as a
`double` (samples are bit-cast `:float` -> `:int` -- see the
`samples are now native :float` note at the top of `synth.tur`, which the
inline-C blocks have not yet caught up with).

**Important constraint:** inline-C blocks have fixed C signatures, so a
variadic body cannot embed inline-C directly -- wrap inline-C in a
fixed-arity helper. (See the per-arity guidance in `CLAUDE.md`.)

Every new primitive in this plan follows that same shape.

### Module-level imports needed for new C code

Most new helpers will need `<math.h>` (`tanh`, `floor`, `pow`, `sin`, `cos`).
The existing `__dsp_sin` already calls `sin`, so the inline-C path already
links libm via the codegen prelude -- no build-system change is required.

## Phase 0 -- finish the `:float` sample migration (prerequisite)

`signal/synth.tur:7` notes:

> All sample values are now native `:float`.

but the existing C helpers still `memcpy` between `int64_t` and `double`,
and every SF body bit-casts at its boundaries (`(:: x :float)` going in,
`(:: y :int)` going out). The migration is half-done. **Finish it before
adding any new inline-C**, otherwise every primitive in this plan
inherits the bit-cast idiom and the cleanup gets twelve times harder.

### What "done" looks like

- A `Sample` alias resolved to `:float` (or `:float` used directly --
  pick one and stick with it).
- SF type is `Time -> Sample` end-to-end: `(fn [t] ...)` returns `:float`,
  not `:int`-bit-cast-of-`:float`.
- Every inline-C helper takes/returns `double` (Turmeric `:float`)
  directly instead of `int64_t`-then-`memcpy`. The `int64 sig_val; memcpy(&x,
  &sig_val, 8)` dance disappears.
- All `(:: ... :int)` / `(:: ... :float)` casts inside DSP code are gone
  (boundary casts at the gate-signal `bool` lane stay).
- The `__signal_call1` helper in `core.tur:112` returns `:float`, not `:int`.
- Examples that print samples (`examples/01_basics.tur`, `02_signals.tur`,
  `03_dsp.tur`) keep working -- if they printed bit-cast garbage before,
  they now print real floats.

### Scope of Phase 0 changes

Every function in these files touches sample values, so all of them get
revisited:

| File | What changes |
|---|---|
| `src/signal/core.tur` | `__signal_call1` returns `:float`; `constant`/`pair-signals` ergonomics if they assume `:int` |
| `src/signal/dsp.tur` | All 8 oscillator/filter/amp SFs: drop `memcpy`, return `double` directly |
| `src/signal/envelope.tur` | `__adsr_fixed_sample` already returns `:float`; the `(:: ... :int)` wrap at `synth.tur` callsite goes away |
| `src/signal/synth.tur` | `__sample-add`/`__sample-mul`/`__sample-clip` already take `:float`; remove the `(:: ... :int)` / `(:: ... :float)` boundary noise; `voice`/`voice-sf`/`poly-synth`/`wavetable-voice`/`ks-voice` body cleanups |
| `examples/0{1,2,3}_*.tur` | Verify still compile and print sensible values |
| `tests/signal/*.tur` | Update any expected-value assertions |

### Validation gate

Phase 0 ships when:

- All existing tests pass with sample values flowing as `:float`.
- `grep -n "memcpy(&" src/signal/` returns nothing in sample-conversion
  contexts (state-pointer aliasing inside ring buffers is fine).
- `grep -nE "::\s+:(int|float)\b" src/signal/` is empty inside sample
  expressions (gate-signal boolean conversions stay).

Only then do Phases 1+ start adding new primitives. Every new inline-C
body in those phases takes/returns `double` directly -- no new
`int64 + memcpy` patterns. **If a Phase 1+ patch reintroduces the bit-cast
idiom, reject it.**

## New primitives

Each primitive below lists: signature, semantics, state, and a sketch of the
C body. All live in `signal/dsp.tur` unless noted.

### `scale` -- linear range remap

```turmeric
;;; scale -- map x linearly from [in-lo, in-hi] to [out-lo, out-hi].
(defn scale [x : float in-lo : float in-hi : float out-lo : float out-hi : float] : float
  ```c
  return out_lo + (x - in_lo) * (out_hi - out_lo) / (in_hi - in_lo);
  ```)
```

Pure function (not an SF). Used everywhere we have a normalised
`[0,1]` knob that needs to drive a Hz value or a filter coefficient.

Companion convenience: `scale-clamped` (clips x to `[in-lo, in-hi]` first)
and an SF wrapper `(scale-sf in-lo in-hi out-lo out-hi)` that lifts the
pure function over a signal.

**Naming**: avoid colliding with any existing `scale` in stdlib. If there
is a stdlib `scale` we will namespace as `signal/scale` (the spice already
publishes everything under `signal/dsp`, so this is automatic).

### 3-pole and 4-pole resonant LPF (ladder filter)

```turmeric
;;; ladder-lpf-4p -- 4-pole resonant low-pass (Moog-style ladder).
;;;
;;; Parameters:
;;;   cutoff :float -- cutoff in Hz (mapped via SR; see notes below)
;;;   q      :float -- resonance 0..1 (self-oscillates near 1.0)
;;;
;;; Returns: SF<Sample,Sample>.
(defn ladder-lpf-4p [cutoff : float q : float] ...)
```

Same shape for `ladder-lpf-3p`. State is 4 (or 3) `double` cells per
instance, allocated with a new `__dsp_alloc_state_n(n :int)` helper that
generalises `__dsp_alloc_state` to N cells.

Implementation: the classic non-linear Stilson/Smith ladder reduces to
four cascaded one-pole LPFs with global feedback `-4 * res * y4 -> input`.
Pseudocode (per-sample, inside the inner `(fn [t] ...)`):

```c
double f = 2.0 * sin(M_PI * cutoff / SR);   // bilinear-ish prewarp
double fb = res * (1.0 - 0.15 * f * f);     // tame at high cutoff
double in = x - fb * stage4;
stage1 += f * (tanh(in)      - tanh(stage1));
stage2 += f * (tanh(stage1)  - tanh(stage2));
stage3 += f * (tanh(stage2)  - tanh(stage3));
stage4 += f * (tanh(stage3)  - tanh(stage4));
y = stage4;
```

The 3-pole variant drops `stage4` and feeds back from `stage3`.

**Sample rate (SR)**: there is currently no global SR constant in the spice.
Add `(def __signal-sr 48000.0)` at the top of `signal/dsp.tur` and let
callers override via a future `(with-sample-rate ...)` form. For now,
`48000.0` is hard-coded; this is documented in the module docstring.

### Bitcrusher

```turmeric
;;; bitcrush -- reduce bit depth and (optionally) sample rate.
;;;
;;; Parameters:
;;;   bits      :float -- effective bit depth, 1.0..16.0 (non-integer OK)
;;;   downsample :int  -- hold each output sample for N input samples (>=1)
(defn bitcrush [bits : float downsample : int] ...)
```

State: a hold counter (`int`) and a held-sample (`double`).

Per-sample body:

```c
if (--counter <= 0) {
    double levels = pow(2.0, bits);
    held = floor(x * levels + 0.5) / levels;
    counter = downsample;
}
y = held;
```

### Saturation (soft clip)

Two flavors, both pure SFs (no state):

```turmeric
;;; saturate-tanh -- soft-clip via tanh(drive * x) / tanh(drive).
(defn saturate-tanh [drive : float] ...)

;;; saturate-cubic -- cheap polynomial soft clip: x - x^3/3, hard-clipped at +/- sqrt(3).
(defn saturate-cubic [drive : float] ...)
```

`saturate-tanh` is the musical default; `saturate-cubic` is the cheap
fixed-point option. `hard-clip` (already in `signal/synth.tur`) stays for
discontinuous limiting.

### Comb filter

```turmeric
;;; comb-ff -- feedforward comb: y[n] = x[n] + g * x[n - delay].
;;; comb-fb -- feedback comb:    y[n] = x[n] + g * y[n - delay].
(defn comb-ff [delay-samples : int g : float] ...)
(defn comb-fb [delay-samples : int g : float] ...)
```

State: a `double*` ring buffer of `delay-samples` cells, plus a write
index. New C helper:

```turmeric
(defn __dsp_alloc_ring [n : int] : int
  ```c
  size_t n_cells = (size_t)n + 1;             // index 0 holds write-head
  double *p = (double *)calloc(n_cells, sizeof(double));
  if (!p) { fprintf(stderr, "dsp: ring alloc failed\n"); abort(); }
  return (int64_t)(intptr_t)p;
  ```)
```

This is the same primitive Karplus-Strong needs, so once `comb-fb` exists
the `karplus-strong` stub becomes:

```turmeric
(defn karplus-strong [freq : float decay : float noise-duration : float]
  (let [delay (:: (/ __signal-sr freq) :int)
        comb  (comb-fb delay decay)
        noise (noise-burst noise-duration)]   ; new helper: white noise gated
    (>>> noise comb)))
```

i.e. replacing the stub falls out of the comb work for free.

### Lookup table

```turmeric
;;; lut-read -- linearly-interpolated read from a vec<Sample> at fractional index.
;;;
;;; Parameters:
;;;   table :Vec<Sample>
;;;   index :float -- fractional, wraps modulo length
;;;
;;; Returns: :float -- interpolated sample.
(defn lut-read [table index : float] : float ...)
```

Inline-C grabs the vec backing pointer and length, computes
`i0 = floor(index) mod n`, `i1 = (i0+1) mod n`, `frac = index - floor(index)`,
returns `(1-frac)*t[i0] + frac*t[i1]`. The "grab vec backing pointer" step
is the only fiddly part -- mirror whatever idiom existing inline-C code uses
to walk `Vec<int>` (search `signal/synth.tur` and other spices for
`vec-data`/`vec-get` C-level access patterns; add a helper only if no such
idiom exists).

### Wavetable oscillator (real one)

With `lut-read` and the existing `__dsp_fmod`, the real wavetable osc is
about five lines:

```turmeric
(defn wavetable-osc [wavetable freq : float]
  (let [n (:: (vec-length wavetable) :float)]
    (fn [sig]
      (let [state (__dsp_alloc_state)]   ; phase, in samples
        (fn [t]
          ```c
          double *phase = (double *)(intptr_t)state;
          double step = freq * n / __signal_sr;
          double y = /* lut_read(wavetable, *phase) */;
          *phase = fmod(*phase + step, n);
          ...
          ```)))))
```

(Real code goes via the `lut-read` defn so we are not duplicating the
interpolation logic in C.)

Then `sine-wavetable`, `square-wavetable`, `sawtooth-wavetable`,
`triangle-wavetable` (new) become real table generators:

```turmeric
(defn sine-wavetable [size : int]
  (let [tbl    (vec-create size 0)
        ^mut i 0]
    (while (< i size)
      (let [phase (* 6.283185307179586 (/ (:: i :float) (:: size :float)))]
        (vec-set! tbl i (:: (__dsp_sin phase) :int)))
      (set! i (+ i 1)))
    tbl))
```

## FM synth example

Add `examples/04_fm_synth.tur` and a matching exported preset
`fm-bell` in `signal/synth.tur`. The current `fm-voice` is replaced
with a real two-operator implementation:

```turmeric
;;; fm-voice -- two-operator FM with modulator envelope.
(defn fm-voice [carrier-freq : float modulator-freq : float mod-index : float
                params gate-sig]
  (let [;; Modulator: sine, scaled by mod-index (the depth in Hz)
        mod-env  (adsr-gen params)
        mod-osc  (sine modulator-freq 0.0)
        mod-sig  (>>> mod-osc (gain mod-index))
        mod-out  (__arrow_call1 mod-sig (constant 0))

        ;; Carrier: phase-modulated sine, freq = carrier + mod-out(t)
        ;; (Strictly this is PM, which is musically equivalent to FM for our
        ;; purposes and easier to implement on a per-sample basis.)
        carrier  (fn [t]
                   (let [m     (:: (__arrow_call1 mod-out t) :float)
                         phase (+ (* 6.283185307179586 carrier-freq
                                     (:: t :float))
                                  m)]
                     (:: (__dsp_sin phase) :int)))

        ;; Amplitude envelope
        amp-env  (__arrow_call1 (adsr-gen params) gate-sig)]
    (fn [t] (:: (__sample-mul (:: (carrier t) :float)
                              (:: (__arrow_call1 amp-env t) :float))
                :int))))
```

The example wires three voices into a chord with a bell-ish patch
(`carrier:modulator = 1:1.4`, `mod-index = 200..600 Hz`, fast modulator
decay) and renders to a WAV via the existing example I/O pattern (see
`examples/03_dsp.tur` for the println-based smoke pattern; the new example
should additionally write a file if the spice has a wav-writer helper, or
print samples otherwise -- match whatever is currently in use).

## FFT primitives

Two candidates:

**Option A -- vendor KissFFT.**
- Single-header-ish (`kiss_fft.c` + `kiss_fft.h` + `kiss_fftr.{c,h}` for the
  real transform). BSD-3.
- Drop under `spices/signal/c/kissfft/` and reference from spice C blocks
  with a `#include "kissfft/kiss_fftr.h"`.
- Pros: well-tested, fast enough, supports non-power-of-2 sizes, real and
  complex transforms.
- Cons: extra files in the spice tree; need to confirm the spice build
  picks up sibling `.c` files (the current spice only uses inline-C inside
  `.tur`). Likely requires a small `:c-sources` manifest addition or a
  new compiler flag.

**Option B -- in-tree radix-2 FFT in pure Turmeric / inline-C.**
- ~80 lines of inline-C, power-of-2 sizes only, no extra build plumbing.
- Pros: zero new build machinery; ships with the spice; easy to audit.
- Cons: slow for huge N; we own correctness; no real-FFT optimisation
  (just call the complex FFT with imaginary = 0).

**Recommendation: B first, A as a follow-up gated on
[[spices-c-sources-plan]] landing.**

Reasoning: 99% of "I want an FFT in my synth" use cases are
`N in {256, 512, 1024, 2048}`, all powers of 2, and a textbook
Cooley-Tukey radix-2 in inline-C will be plenty. Vendoring KissFFT
requires the spice build system to aggregate auxiliary `.c` files, which
currently does not exist -- that is its own piece of work and has its own
plan ([[spices-c-sources-plan]]). The signal spice keeps its current
"single `.tur` file with inline-C" shape, and we only revisit if profiling
on a real consumer demands it.

### Phase B API sketch

```turmeric
;;; fft-forward -- in-place complex radix-2 FFT.
;;;
;;; Parameters:
;;;   re :Vec<Sample> -- real components, length must be a power of 2
;;;   im :Vec<Sample> -- imaginary components, same length as re
;;;
;;; Returns: () -- mutates re and im in place.
(defn fft-forward [re im] ...)

;;; fft-inverse -- in-place inverse FFT (normalises by 1/N).
(defn fft-inverse [re im] ...)

;;; fft-real -- forward FFT of a real input, returns (re, im) vectors.
(defn fft-real [x] ...)

;;; window-hann -- Hann window of given size as a vec<Sample>.
(defn window-hann [size : int] ...)

;;; window-hamming
;;; window-blackman
```

Validation hook: a fixture that builds a sine at 440 Hz, FFTs a 1024-sample
window, asserts the bin index of the peak matches `(round (* 440 1024 / SR))`
within +/- 1.

A Phase A KissFFT swap-in is then "drop the radix-2 body, link KissFFT" and
the API does not change.

## File layout

Everything lives in the sibling spice repo:

```
../turmeric-spices/spices/signal/
  src/signal/
    core.tur          # unchanged
    dsp.tur           # + scale, ladder-lpf-{3,4}p, bitcrush, saturate-*,
                      #   comb-{ff,fb}, lut-read, fft-*, window-*
    envelope.tur      # unchanged
    synth.tur         # fm-voice replaced; wavetable-osc / *-wavetable real;
                      #   karplus-strong rewritten on top of comb-fb
  examples/
    04_fm_synth.tur   # new
    05_filters.tur    # new -- demos ladder + bitcrush + saturate
    06_fft.tur        # new -- spectral analysis demo
  tests/
    test_scale.tur
    test_ladder.tur
    test_bitcrush.tur
    test_comb.tur
    test_lut.tur
    test_fft.tur
  build.tur           # :exports updated to include all new names
```

This repo (`turmeric`) only carries this plan doc; no source changes here.

## Phasing

Land in dependency order; each phase is independently mergeable in the
spices repo. **Phase 0 is a hard gate** -- nothing else starts until
samples flow as `:float` end-to-end:

0. **`:float` sample migration** (prerequisite -- see Phase 0 section).
   No new inline-C ships until this lands.
1. **`scale` + `scale-sf`** -- pure, no state, smallest possible PR.
   First test of the post-migration "samples are `:float`" idiom.
1.5. **Missing oscillators and shapers**: `triangle`, `offset`, `invert`,
   `abs-sf`, `multiply`, `clip`, `noise-burst`. All small pure SFs;
   unblocks `examples/03_dsp.tur` and the Karplus-Strong rewrite.
2. **`saturate-tanh` / `saturate-cubic`** -- pure SF, no state.
3. **State helpers**: `__dsp_alloc_state_n`, `__dsp_alloc_ring`.
4. **`bitcrush`** -- 1-cell + 1-counter state.
5. **`comb-ff` / `comb-fb`** -- ring buffer.
6. **`karplus-strong` rewrite** on top of `comb-fb` + `noise-burst`
   (replaces a stub).
7. **`ladder-lpf-3p` / `ladder-lpf-4p`** -- multi-cell state, tanh
   non-linearity. Decide `svf-low-pass` fate (remove or alias).
7.5. **`square-wavetable` duty fix** (one-line, but logically belongs
   with the wavetable phase).
8. **Gate-driven `adsr-gen` rewrite** -- ADSR that follows the actual
   gate signal instead of the hard-coded 1.0 s. Gates correctness of
   every voice that depends on it.
9. **`lut-read`** + real `wavetable-osc` + real `sine-wavetable` /
   `sawtooth-wavetable` / `triangle-wavetable` (new) generators
   (replaces three stubs).
10. **FM synth**: real `fm-voice` + `fm-bell` preset + `04_fm_synth.tur`
    example.
11. **Filter / saturation example** `05_filters.tur`.
12. **FFT primitives** (Option B -- in-tree radix-2) + `06_fft.tur`
    example + spectrum-peak fixture.
13. **Larger stub rewrites** (no consumer inside this plan, lower urgency):
    real `additive-voice` (sum of harmonic sines), real `granular`
    (grain-rate scheduler with ring-buffer source), real `step-sequencer`
    (tempo-driven dispatch over the whole note vector).
14. **(Follow-up)** KissFFT vendoring proposal, gated on
    [[spices-c-sources-plan]] landing first, only revisit if a consumer
    pushes on N > 4096 perf or non-power-of-2 sizes.

## Validation

Each phase ships a `tests/test_<thing>.tur` that:

- For pure functions (`scale`, `saturate-*`): checks specific input/output
  pairs with tolerance.
- For stateful SFs (`bitcrush`, `comb`, `ladder`): feeds an impulse and
  checks the first ~16 output samples against hand-computed expectations.
- For oscillators (`wavetable-osc`, FM carrier): samples 1 cycle and
  asserts shape (max/min/zero crossings) rather than exact values.
- For FFT: peak-bin assertion on a known sine.

Run the spice's existing test pattern (`tur run` against
`tests/<file>.tur`) and confirm zero failures.

For the turmeric repo side: nothing to validate here -- this is a plan doc.

## Open questions

- **Sample rate plumbing.** Hard-code `48000.0` initially, or add
  `(with-sample-rate sr body...)` dynvar from day one? Default: hard-code,
  promote to a dynvar in a follow-up once a second consumer needs a
  different SR.
- **Float-vs-int sample type.** ~~Default: document.~~ **Resolved: Phase
  0 finishes the `:float` migration before any new inline-C lands.**
- **FFT vector type.** `Vec<Sample>` where `Sample` is bit-cast `:int`, or a
  dedicated `Vec<:float>` once that exists? Default: match whatever the
  rest of the spice uses; FFT is not the right place to force a type-system
  change.
- **KissFFT licensing.** BSD-3 is fine to vendor, but the spice repo has no
  third-party-code policy yet. If we go to Phase A, write the policy
  *first*.

## Non-goals

- Polyphony / voice allocation engine (`poly-synth` already exists; this
  plan does not redesign it).
- A general "spices may include `.c` files" build feature -- needed only if
  we go to KissFFT, deferred until then.
- A real-time audio backend. The spice targets *offline* signal generation
  (sample-by-sample evaluation). RtAudio / PortAudio bindings are a
  separate plan.
- WAV file I/O extensions. If a wav-writer helper does not already exist
  in the spice, the FM example uses println-style sample dumps to match
  the existing examples.
- MIDI input.

## Acceptance checklist

- [ ] **Phase 0**: samples flow as `:float` end-to-end; no
      `int64 + memcpy` bit-casts inside DSP code; existing examples and
      tests still pass.
- [ ] `triangle`, `offset`, `invert`, `abs-sf`, `multiply`, `clip`,
      `noise-burst` implemented and exported; `examples/03_dsp.tur`
      compiles and runs without modification.
- [ ] `scale`, `scale-sf` exported and tested.
- [ ] `saturate-tanh`, `saturate-cubic` exported and tested.
- [ ] `bitcrush` exported with a working bit-depth + downsample test.
- [ ] `comb-ff`, `comb-fb` exported, impulse-response test passes.
- [ ] `karplus-strong` no longer delegates to `sawtooth`; uses `comb-fb`.
- [ ] `ladder-lpf-3p`, `ladder-lpf-4p` exported; cutoff sweep + resonance
      self-oscillation observable in tests.
- [ ] `lut-read` exported; `wavetable-osc` reads the real table and the
      `*-wavetable` generators produce non-zero, period-correct tables.
- [ ] `fm-voice` produces audibly FM-shaped output (test asserts non-sine
      spectrum); `fm-bell` preset exported; `examples/04_fm_synth.tur`
      runs cleanly.
- [ ] `05_filters.tur` example runs cleanly.
- [ ] `fft-forward`, `fft-inverse`, `fft-real`, `window-{hann,hamming,blackman}`
      exported; sine-peak fixture passes.
- [ ] `examples/06_fft.tur` runs cleanly.
- [ ] `signal/synth.tur:7` docstring matches the post-migration reality
      (`:float` end-to-end, no `int`-bit-cast).
- [ ] `square-wavetable` honours the `duty` argument.
- [ ] `adsr-gen` follows the actual gate signal (re-triggers on rising
      edge, releases on falling edge); existing voice presets reflect
      the change in their gate behaviour.
- [ ] `svf-low-pass` either removed or aliased to a real resonant
      filter; no caller relies on its stub identity.
- [ ] **Phase 13**: real `additive-voice` sums weighted harmonic sines;
      real `granular` schedules grains by `grain-rate`; real
      `step-sequencer` walks the entire note vector at the requested
      tempo.
- [ ] `build.tur` `:exports` list updated for every new public name.
