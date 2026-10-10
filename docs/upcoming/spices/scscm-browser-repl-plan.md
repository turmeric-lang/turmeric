# Plan: a browser scscm REPL on hcsynth, with Saffron as the default language

> Status: draft -- not started. Facts below were read from the tree on 2026-10-10.
> Scope: a **new public GPL-3.0 repo in the turmeric-lang org** named
> **HyperREPL** (`turmeric-lang/hyper-repl`, working name) holding the REPL; `spices/scscm/` (turmeric-spices) supplies
> the compiler; the hypercollider browser build supplies the audio runtime.
> Try Turmeric (`web/`) is a reference for patterns, not the host. No compiler
> change in the MVP.
> Type: web / spice integration.
> Depends on: T0-T3 of [`scscm-host-testing-plan.md`](scscm-host-testing-plan.md)
> (a booting hclang/hcsynth, and a spice that emits valid sclang).

---

## 0. The assumption to confirm

"Default to Saffron" is read here as: **the REPL's session language is
`#lang saffron`** -- the dynamically typed dialect of Turmeric
([saffron-guide](../../guides/saffron-guide.md)) -- and scscm is what a
Saffron program produces and plays, rather than the text you type. If the intent
was the other reading (the REPL *input* is Saffron syntax compiled straight to
sclang, with no scscm text in between), section 4's pipeline loses a stage and
gains a Saffron-to-sclang backend; ask before building that one, it is a much
larger piece of work than anything below.

## 1. What exists

**Try Turmeric** (`web/`): a Monaco editor page. All evaluation runs in a
Worker (`web/public/eval-worker.js`) hosting the `turi` interpreter compiled to
WASM; `main.js` posts `{type:'eval', input, lang}` and receives results. `#lang`
dialects, Saffron among them, are selected by the buffer's first line and listed
by the worker's `lang-registry` message. The page is cross-origin isolated
(`Cross-Origin-Opener-Policy: same-origin`, `Cross-Origin-Embedder-Policy:
require-corp`, set in `web/public/_headers` and `web/worker.js`) and has a strict
CSP (`web/csp.js`): `default-src 'self'`, `script-src 'self' 'wasm-unsafe-eval'`
(plus one pinned CDN prefix for Mermaid), `connect-src 'self'`, `worker-src
'self'`, no inline script.

**The scscm spice cannot run in that interpreter.** User inline C is a permanent
`turi` carve-out
([turi-parity-guide](../../guides/turi-parity-guide.md), the Inline-C row), and
the spice's lexer, parser, expander, codegen, compile and tree modules contain
about 54 inline-C blocks (27, 12, 6, 5, 2, 2 by module). It compiles and runs
fine ahead of time. So Try's interpreter cannot call `tur-scscm`; something else
must.

**hypercollider already runs in a browser.** `src/platform/test/sc_ide.html` is
a 4,600-line IDE: CodeMirror, `hclang.wasm` (sclang) feeding `hcsynth.wasm`
(scsynth) in an AudioWorklet (`sc_wasm_audioworklet.js`, with a main-thread
fallback), and a pure-JS scscm compiler (`lhc_compile.js`, loaded lazily). Its
release artifacts, measured from the v0.1.5 tarball:

| file | raw | gzip |
|---|---|---|
| `hclang.wasm` | 1.83 MB | 0.67 MB |
| `hclang.data` (class library) | 2.66 MB | 0.66 MB |
| `hclang_classlib.pack` (alternative to `.data`; not clear which a browser loads) | 1.34 MB | 0.33 MB |
| `hcsynth.wasm` | 1.51 MB | 0.44 MB |
| `hcsynth.data` | 0.28 MB | 0.24 MB |
| `hclang.js` + `hcsynth.js` | 0.33 MB | 0.09 MB |

About **2.1 MB gzipped** (6.6 MB raw) for first sound, before the scscm compiler.
That is a lazy-load, user-initiated cost, not a page-load cost.

**Not yet working:** neither the v0.1.5 `hclang` nor the source tree's Node CLI
boots today (see the testing plan, section 2), so the browser build is unverified.
It is the first thing to establish.

## 2. Options for the compile step

| | How | For | Against |
|---|---|---|---|
| **O1** | Reuse hypercollider's `lhc_compile.js` | works in sc_ide today; ~zero work | not `tur-scscm`; keeps two compilers alive and the Turmeric one unexercised |
| **O2** | AOT-compile `tur-scscm` to a small standalone WASM module exporting `scscm_compile(text) -> sclang` | the real spice; inline C is fine ahead of time; no interpreter involved; reuses the Emscripten path in [web-emscripten-tutorial](../../guides/web-emscripten-tutorial.md) | a second WASM artifact and build step |
| **O3** | Remove inline C from the spice so `turi` can run it | the REPL could `import scscm/compile` directly | a rewrite of ~54 blocks, mostly the lexer; the carve-out is permanent so it must be *pure* Turmeric; large |

**Recommendation: O2.** The spice is the thing this project is about; O1 would
make the REPL a demo of someone else's compiler. O1 remains a fallback to ship
the audio half early (B2 below does not depend on the choice).

## 3. Why Saffron is a good default here

An scscm program is an s-expression, and Saffron is an s-expression language
with `any` as its default type: no annotations to type before you can hear
something. A Saffron program can *generate* scscm -- build the text of a voice
per partial, transpose a pattern -- which is the live-coding move. And the
Saffron half is pure `turi`, so it runs in Try's existing worker with **no
native bridge** in the MVP.

Checked on v0.63.9 (`tur --interpret`): a Saffron function that builds scscm
**text** with `str-concat` / `int->str` runs and prints correctly. Two things
that look natural do **not** work today and the plan must not assume them:
a mixed list of symbols, numbers and strings (`(list 'Synth "sine" ...)`) cannot
be passed to `println` ("no operator for a value of that type"), and `map` /
`range` are not defined without a stdlib load. So the MVP returns *text*; a
data-to-scscm-text printer for quoted forms is its own task (B1b).

## 4. Pipeline

```
 Monaco, #lang saffron  (default for this page)
        |  Run
        v
 eval worker (turi)  --- evaluates the Saffron program, which returns scscm text
        |
        v
 scscm.wasm (Worker)  --- tur-scscm compile-text -> sclang text      [O2]
        |
        v
 hclang.wasm  --- evaluates sclang, builds SynthDefs, sends OSC
        |
        v
 hcsynth.wasm in an AudioWorklet  --> speakers
```

A Saffron program's **result is the scscm program**, as a string:

```scheme
#lang saffron
(load "stdlib/str-build.tur")
(defn voice [n]
  (str-concat (str-concat "(Synth \"sine\" (dict :freq " (int->str (* 110 n)))
              " :amp 0.05))"))
(voice 2)        ; => (Synth "sine" (dict :freq 220 :amp 0.05)), which the REPL plays
```

(This exact `voice` was run under `tur --interpret`; it prints the string shown.)
Returning a quoted *form* instead of text is a later nicety (B1b). Errors from each stage are
reported against that stage (Saffron eval, scscm compile, sclang eval) with the
generated text one click away, since scscm errors currently surface at generated
sclang positions (hypercollider's source-map phase H4 is not done).

## 5. Phases

### B0. Prerequisites (not in this repo)

- A booting `hclang.wasm`/`hcsynth.wasm` browser build, and a statement of which
  data file (`.data` or `.pack`) the browser loads.
- Testing plan T1: the spice emits valid sclang for the corpus. Without it the
  REPL produces errors for ordinary synth definitions (parameter defaults today
  compile to invalid sclang).
- The licensing decision in section 7.

### B1. `scscm.wasm` (O2)

A tiny Turmeric entry point exporting `scscm_compile`, built with the existing
`tur` -> C -> `emcc` path into HyperREPL's `public/scscm/`. Runs in its own Worker
(consistent with the eval worker; a pathological input cannot freeze the page).
**Done when** the testing plan's corpus compiles identically through the WASM
module and the native build, in a Playwright test.

### B1b. Forms as results (optional)

Let a Saffron program return a quoted form and have the page print it as scscm
text. Needs a printer for `turi` data (symbols, keywords, numbers, strings, nested
lists) that does not exist for mixed lists today; `scscm/tree`'s `sx->sexp` prints
the spice's own `Sx` type, not `turi` values, so it is a model, not a reuse.

### B2. Audio runtime loader

A module that lazily fetches `hclang`/`hcsynth` on the first Run (never at page
load), starts the `AudioContext` from a user gesture, wires the worklet, exposes
`evalSclang(text)` and a post window. Serving everything same-origin keeps a strict CSP
(`connect-src 'self'`, `worker-src 'self'`, worklet under `script-src 'self'`)
possible. The multi-MB artifacts must stay **out of any service worker's precache**
(Try's `web/public/sw.js` is the cautionary example); cache them at runtime with their own versioned key.
**Done when** `evalSclang('{ SinOsc.ar(440) * 0.1 }.play')` is audible in a real
browser (manual) and a Playwright test passes against a stubbed `AudioContext`.

### B3. The page

The page lives in HyperREPL (section 7), not in `web/`. It borrows Try's
patterns (Monaco setup, theme, the share codec in `web/share-codec.js`, the eval
Worker and its watchdog) by copying what it needs -- the sources are MIT, so
they can be included in a GPL-3.0 work -- rather than depending on Try's build.
turmeric-lang.com links to it. Default buffer starts with `#lang saffron`; Run executes
the section 4 pipeline; a Stop button silences everything; a few example
programs; scscm syntax highlighting is a later nicety. **Done when** the examples
make sound and the page passes the existing layout/CSP specs.

### B4. In-session control (optional, after the MVP)

The MVP can only return a program. Live coding wants `(play! ...)` *inside* a
running Saffron program (loops, `after`, routines). That needs natives registered
into the `turi` session -- `turi_env_register_native` exists in libturi
(`src/runtime/globals.h`), but `src/web/wasm_glue.h` exposes no way to register a
JS-backed native, so this is glue work in the compiler repo, plus an async
story (a native that waits on the audio runtime must not block the worker that
holds the interpreter).

### B5. Tests and CI

Playwright against a stubbed `AudioContext` for everything except sound itself;
the compile stages tested against the corpus; one manual real-audio checklist.
Port 3000 is the developer's own server -- the suite already picks its own.

## 6. Constraints that will bite

- **Autoplay:** audio can only start from a user gesture; Run is that gesture.
- **Cross-origin isolation (COOP/COEP):** a separate repo means a separate
  deployment, and the host must be able to send `Cross-Origin-Opener-Policy:
  same-origin` / `Cross-Origin-Embedder-Policy: require-corp` if the hypercollider
  build needs `SharedArrayBuffer` (not established; check in B0). GitHub Pages
  cannot set response headers; Cloudflare (as Try uses) can. Every subresource
  must then be same-origin or send CORP, so the artifacts are served from the
  same site, not fetched from a release URL.
- **No live `scsynth` from the page** by default. A strict CSP with `connect-src
  'self'` (as Try has) blocks a WebSocket to hypercollider's local OSC bridge. The
  HyperREPL sets its own CSP; allowing `ws://localhost` is a separate, deliberate
  choice and not part of the MVP.
- **First-sound latency:** ~2.1 MB gzipped plus class-library startup. Show
  progress, and measure time-to-sound before promising a number.
- **Memory:** two WASM runtimes and a worklet heap alongside Try's interpreter;
  measure on a phone before offering this on mobile.
- **Interpreter hangs:** Try already has an eval watchdog that restarts the
  worker; the sclang runtime needs its own.

## 7. Open questions

1. **Licensing -- decided.** hypercollider carries the same licensing as
   SuperCollider (GPL; its `COPYING` is GPL-3.0). The REPL is a **public
   GPL-3.0 repo in the turmeric-lang org**, so the combined bundle (REPL page,
   `scscm.wasm`, hclang/hcsynth WASM) is one GPL-3.0 work; the MIT inputs (the
   `tur-scscm` spice, the Turmeric runtime it links, patterns copied from Try)
   stay MIT in their own repos and may be included in it. Requirements that follow:
   - the README carries the attribution and license notices: SuperCollider and
     hypercollider (GPL), the sc3-plugins and their authors
     (`hypercollider/docs/SC3_PLUGINS_LICENSES.md` is the source list), STK's
     permissive license notice, and the MIT notices for `tur-scscm`, Turmeric and
     any copied Try code;
   - a `COPYING` (GPL-3.0) at the repo root, and per-file headers on new files;
   - the corresponding source is available: hypercollider pinned at the exact
     commit the shipped WASM was built from, plus the build scripts, linked from
     the page and the README;
   - the shipped WASM is reproducible from those pins (B0's artifact decision).
   Turmeric's own repos and Try stay MIT; turmeric-lang.com only links out.
   (Not legal advice.)
2. **Which reading of "Saffron default"** (section 0).
3. **Where the artifacts live -- decided: a git submodule.** hypercollider is a
   submodule of HyperREPL pinned at the commit the shipped WASM is built from, so
   the pin is the source offer (a clone at that commit is the corresponding
   source) and bumping hypercollider is one visible diff. HyperREPL's build runs
   the Emscripten build of the submodule (or consumes a release built from that
   same commit and checks it against the pin). Open: the deployment host
   (Cloudflare vs Pages, see the COOP/COEP note) and whether CI builds the WASM or
   a release artifact is consumed.
4. **Is O1-first acceptable** as a stepping stone to ship sound early?
