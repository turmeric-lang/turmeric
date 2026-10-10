# Plan: test `tur-scscm` against hcsynth and scsynth, and write its guide

> Status: draft -- not started. Baseline measured 2026-10-10 (below).
> Scope: `spices/scscm/` in turmeric-spices; hosts are hypercollider
> (`hclang`/`hcsynth`) and stock SuperCollider (`sclang`/`scsynth`).
> Type: spice / test infrastructure / documentation.
> Related: [`scscm-browser-repl-plan.md`](scscm-browser-repl-plan.md) builds on
> T1-T3 here.

---

## 1. What "test against a synth server" can mean

`tur-scscm` is a compiler: scscm text in, **sclang text** out. Neither `scsynth`
nor `hcsynth` reads sclang. Something has to interpret it:

```
scscm --[tur-scscm]--> sclang text --[sclang | hclang]--> OSC / SynthDefs --> scsynth | hcsynth --> audio
```

So the thing under test is the first arrow, and "against scsynth/hcsynth" means
**the sclang the spice emits must be valid, must build the same SynthDefs, and
must sound the same** as the reference compiler's output when played on each
server. The spice has no server client of its own (the README's SC6, an OSC
client, is unimplemented and blocked on `tur-osc`), and this plan does not add
one: a direct OSC path cannot build SynthDefs, because `scsynth` takes compiled
SynthDef bytes that only `sclang`/`hclang` produce.

Three layers, cheapest first:

| Layer | Needs | Question it answers |
|---|---|---|
| A. Compile | `tur` only | Does the spice emit the right sclang? |
| B. hcsynth | an `hclang` that boots | Does that sclang run, and sound right, on hypercollider? |
| C. scsynth | stock SuperCollider | ...and on the original? Do the two servers agree? |

## 2. Baseline (measured 2026-10-10, v0.63.9)

**The spice compiles and runs.** `compile-text` / `compile-file` work from a
`#fx{Unsafe IO}` caller and return the `lex-ok` / `lex-err` pair, not a
`result`. The spice README documents a `result<...>` API, `ok?`, and `load
"spices/tur-scscm/..."` paths that no longer exist; it needs rewriting (T5).

**It is nearly untested.** `tests/` holds `tree_test.tur` only (13 assertions on
`scscm/tree`). The lexer, parser, expander, codegen and `compile` modules have no
test, despite the README's "SC5 -- tests" box being ticked.

**It does not match the reference compiler.** hypercollider ships a JS compiler
(`cli/lhc.js`, with `docs/scscm/SCSCM_LANGUAGE_REFERENCE.md`) and 21 `.scscm`
programs (`test/fixtures/scscm_parity/`, `tests/fixtures/scscm/`,
`cli/examples/`). Compiling all 21 with both:

- **3 are identical** modulo a trailing `;` (`defn`, `var`, `hello`);
- **18 differ**, and several differences are *wrong sclang*, not style:

| Input | `lhc.js` | `tur-scscm` | Verdict |
|---|---|---|---|
| synth/fn params with defaults | `{ \|freq=440, amp=0.1\| ... }` | `{ \|freq, 440, amp, 0.1\| ... }` | **invalid**: defaults become extra parameters |
| `(dict :doneAction 2)` | `(doneAction: 2)` | `dict(\doneAction, 2)` | **invalid**: sclang has no `dict` function |
| list / clock helpers | `[60, 62, ...]`, `TempoClock.new(...)` | `list(60, 62, ...)`, `metronome(140)` | **invalid**: neither function exists |
| `(if c a b)` | `if (c) { a } { b }` | `if(c, { a }, { b })` | equivalent |
| last statement | trailing `;` | none | cosmetic |
| `:some-keyword` | `\some_keyword` | `\some-keyword` | differs; `-` is not legal in a sclang symbol literal without quoting |
| `(fn () ...)` | `{ \|\| ... }` | `{ ... }` | equivalent |

The categories above come from reading the first differing line of each file;
the remaining 18 were not triaged one by one. T1 does that.

**Neither host boots here today.** `hclang` v0.1.5 (native, from the release
tarball) stops with `class library compilation FAILED` at
`HC/ScscmCompileCli.sc` line 266 and `HC/ScscmCompiler.sc` line 202. With the
hypercollider working copy cleaned, the source tree's `node cli/hclang.js` still
stops with `hc_wasm_eval_boot_sequence failed (-1)`; the `build/wasm` artifacts
predate the cleanup and probably need a rebuild (not done here: it is
hypercollider's build tree). `scsynth`/`sclang` are not installed.

**Useful hclang surface found:** `--script f.scd|f.scscm`, `-o out.wav`
(offline render), `--scsynth-host/--scsynth-port` (forward OSC to a real
`scsynth`), `--no-server-boot`, `--lang scscm|scd`, `--scscm-debug`.

## 3. Decision needed first: which dialect is canonical?

The spice README documents bracket parameters (`(defsynth name [p] body)`,
`;;` comments); hypercollider's language uses paren parameters
(`(fn (freq 440 amp 0.1) ...)`, `;` comments, `dict`, `Synth "x" ...`). For
simple programs the spice already accepts the hypercollider form. Two ways to
close the gap:

1. **hypercollider's dialect is canonical** (recommended). `lhc.js` and its
   language reference are the existing contract, the browser IDE and native
   hosts already ship it, and the 21-file corpus is a ready acceptance test. The
   spice becomes a second implementation of the same language.
2. **The spice defines its own dialect.** Then the corpus is advisory, and the
   guide documents the differences.

This plan assumes (1). If you choose (2), T1's golden files become the spice's
own and T2-T4 still apply unchanged (they test sclang, not scscm).

## 4. Tasks

### T0. A known-good host (prerequisite, not spice work)

Get `hclang`/`hcsynth` booting: rebuild `build/wasm` from the current tree
(`just build`, Emscripten), or fix the v0.1.5 class-library failure and cut a
release. Record the working invocation. Install `scsynth` + `sclang` separately
(macOS: the SuperCollider cask; Debian/Ubuntu: `supercollider-server` and
`supercollider-language`, package names to be confirmed; headless `sclang` needs
`QT_QPA_PLATFORM=offscreen`). **Done when** `hclang --script hello.scd` prints
and `sclang -e '"hi".postln; 0.exit'` prints.

### T1. Layer A: compile parity (no host)

- Check the 21-file corpus into `spices/scscm/tests/corpus/` with the
  `lhc.js` output beside each as the golden (regenerable by one script; pin the
  hypercollider commit it came from).
- A `tur test` harness compiles each with `compile-file` and diffs against the
  golden after a documented normalization (final `;`, whitespace). Cosmetic
  differences that are provably equivalent (`if`) get a normalization rule, not
  a golden edit.
- Fix the **invalid-output** classes first: parameter defaults, `dict`, `list`,
  `metronome`, keyword hyphens, `(fn () ...)`. Then the rest as triage finds.
- Unit tests for lexer, parser, expander and codegen where the corpus is thin
  (string escapes, nested quasiquote, user `defmacro`, error positions).

**Done when** all 21 compile to something the normalization accepts, and
`tur test spices/scscm/tests` runs more than the tree test.

### T2. Layer B: hcsynth, offline and deterministic

For each corpus program that makes sound: compile with the spice, render with
`hclang --script out.scd -o out.wav`, then assert on the **audio**, not the text:

- the WAV is not silent (RMS above a floor);
- a pure-tone program has its fundamental at the expected frequency (zero-crossing
  count over a fixed window is enough; no FFT dependency);
- level matches the program's `amp` within tolerance.

Also render the same program compiled by `lhc.js` and compare the two WAVs. This
is the test that cannot be fooled by two different texts that mean the same
thing, and it makes T1's normalization rules safe to loosen.

**Done when** the sound-making corpus programs pass on hcsynth and the
spice-vs-`lhc.js` WAVs agree within tolerance.

### T3. Layer C: scsynth, and hcsynth against scsynth

Two routes, because they test different things:

- **Offline (CI-friendly):** `sclang` runs the compiled program with
  `Score.recordNRT`-style rendering through `scsynth -N`, producing a WAV the
  same T2 assertions read.
- **Live smoke (manual / opt-in):** `scsynth -u 57110` booted, then
  `hclang --script out.scd --scsynth-host 127.0.0.1 --scsynth-port 57110`
  forwarding OSC, to prove the hclang-to-scsynth path with spice output.

Then compare the scsynth WAV with the hcsynth WAV for the same program. They are
different engines (native vs WASM, different float paths), so the comparison is
tolerance-based (RMS and fundamental, not sample-exact), and any UGen that
differs is a **hypercollider** finding to file there, not a spice bug.

**Done when** the corpus's sound-making programs agree across the two servers
within a stated tolerance, with every exception listed and explained.

### T4. CI wiring

- Layer A runs in the ordinary spices `tur test`.
- Layers B and C run only when a host is present, like the `requires.*` markers:
  the harness reports `SKIP: no hclang`/`SKIP: no scsynth`, never a failure, and
  a CI leg installs each host (hclang from a pinned release tarball with SHA-256
  check, as `scripts/install-tur.sh` does for `tur`; SuperCollider from the
  distro on Linux).
- Pin versions of both hosts in one file so a host upgrade is a visible diff.
- Per `CLAUDE.md`'s test-writing rules: `main` returns the failure count, and
  each new test is run once against a deliberately broken input to prove it can
  fail.

### T5. The guide: `docs/guides/scscm-guide.md`

Written **after** T1-T3, from commands that were run, with each host's
instructions marked by what was actually verified. Outline:

1. What scscm is and what the spice does (and does not: no server client).
2. Installing the spice; the compile API (`compile-text`, `compile-file`, the
   `lex-ok?` pair, the `Unsafe IO` effect row) -- replacing the stale README text.
3. The language: a pointer to the canonical reference plus the spice-specific
   differences, if any remain after T1.
4. **Running on hcsynth:** obtaining `hclang`, the exact command, offline render
   to WAV, `--scsynth-host` forwarding.
5. **Running on scsynth:** installing SuperCollider per OS, booting `scsynth`,
   running the compiled program, offline NRT.
6. Troubleshooting: class-library errors, `--lang`, `--scscm-debug`, no sound.

Also fix the README's API section and roadmap (tick SC6 as "out of scope, see
guide").

## 5. Risks and open questions

- **Dialect ownership (section 3).** Blocks T1's golden files.
- **Two compilers to keep in step.** The spice and `lhc.js` will drift again
  unless the corpus runs in both repos' CI. Consider hypercollider running the
  spice's corpus, or a shared corpus repo.
- **hypercollider is a moving host.** The v0.1.5 tarball fails to boot; T0 may
  uncover more. T2/T3 cannot start until it does.
- **Cross-engine tolerance** is a judgment call; pick it from measurement, write
  it down.
- **GPL.** SuperCollider and hypercollider are GPL-3.0; the spice is MIT. Running
  them as separate processes in CI is fine; bundling them is not (see the browser
  plan).
