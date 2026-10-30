# Plan: a raylib playground in the browser ("Try Turmeric with a screen")

> **Status:** P0 go on J and **P1 done** (2026-10-30); **P2 next**
> ([section 7](#p2----the-page-arcade-repo)). P1
> ([section 7](#p1----the-arcade-module-this-repo)): the arcade module is a
> CMake target (`tur_wasm_arcade`) with a CI smoke test, the fallback
> switch (J3 e) is in, spices are packages the page mounts (J2, `defscript`
> headers included), `life.tur`'s step is measured (64x36, not 96x54), and
> turmeric-arcade builds the module, mounts the packages and runs its
> `examples/` tree in CI.
> Every decision this plan asked for is made: **J** -- the browser JIT engine
> on the page with raylib linked in, compiling the raylib spice's inline C from
> source ([section 3](#3-the-central-decision-how-does-typed-code-reach-raylib));
> **JSPI** for the blocking frame loop (J4, prerequisite 3), which is a J-only
> route and so also settled J over A; and **its own repository**
> ([section 4](#4-where-it-lives)),
> **[turmeric-lang/turmeric-arcade](https://github.com/turmeric-lang/turmeric-arcade)**,
> deployed to **play.turmeric-lang.com**. What is built:
> - the arcade repo's scaffold (2026-10-30): Try's site shape (COOP/COEP, one
>   CSP, the Worker), the screen pane of [5.2](#52-the-screen-pane) running a
>   program in an iframe on the JSPI module and streaming its lines,
>   `scripts/fetch-engine.sh` building that module from the turmeric commit
>   `scripts/engine.lock` pins, and Playwright tests green in Chromium 153;
> - the P0 spike, whose day one answered its first three questions yes (the
>   [results](#p0-results-2026-10-30-day-one-go-on-j)): the quick-start
>   program draws through struct-by-value calls, callback mode survives
>   `main`, and the blocking loop yields through raylib's own sleep under
>   JSPI;
> - P1's first part (2026-10-30): `tools/wasm-jit/arcade/` (the raylib web
>   archive, the module, a smoke test), the `tur_wasm_arcade` target and the
>   `wasm-arcade` CI job, and `turi_wasm_jit_set_fallback`; then J2's spice
>   packages (`pack-spices.py`, `spices.js`) and the glue satisfying a
>   `defscript` header from them.
>
> Of the [prerequisites](#prerequisites-before-p0), 1-5, 8-10, 12 and 13 are
> closed or spiked; 6 and 7 (c2mir's per-call vararg buffer, a tail call
> keeping its allocas) are open and gate only the examples (P3); 11 is
> turmeric-spices work for the Spices menu. History: drafted 2026-10-23 with
> an interpreter-plus-baked-images design (A, now the fallback); revised
> 2026-10-29 once the browser JIT landed ([wasm-jit-plan](wasm-jit-plan.md)
> P0-P5 and P6's C1, 2026-10-28) made compiling the spice in the page
> possible, which is what flipped the recommendation to J.
> **Type:** Web platform / browser JIT module (`tools/wasm-jit/`,
> `src/web/wasm_jit_glue.c`, `src/jit_engine.c`) / a new site. The interpreter
> side (`src/turi/`) and the build driver (`src/driver/cmd_build.c`) only if the
> contingent track A is taken.
> **Builds on:** [wasm-jit-plan](wasm-jit-plan.md) (the browser JIT engine,
> `turmeric-jit.{js,wasm}`, the shared `stdlib.data` package, the import
> table), [wasm-spices-plan](wasm-spices-plan.md) (the raylib web arm,
> `tur build --target wasm`, the `:web` manifest block, the measured
> Asyncify cost), [script-spice-deps-plan](script-spice-deps-plan.md)
> (`defscript :spices`, landed 2026-10-22 behind `--enable=script-deps`),
> [spice-repl-plan](../archive/spice-repl-plan.md) (compiled spice exports
> bound as interpreter natives through `src/turi/ffi_thunk.c`),
> [extras-repo-split-plan](../archive/extras-repo-split-plan.md) (the pattern for a
> sibling repo that pins a `tur` release).
> **Not this plan:** a general web-app scaffold (`tur new-web`), which stays
> in wasm-spices-plan W4; any change to the raylib spice's native arm.

## 1. Goal

A single-page app, in the spirit of `try.turmeric-lang.com`, where a visitor
edits a raylib program on the left and sees it **run** in a small screen on
the right, with the program's `println` output in a console under the
screen. Examples are one click away. A program may use raylib, raygui, and
other spices that work in a browser. Run starts in well under a second (the
browser JIT measured 125-180 ms in the front half and 110-170 ms in c2mir for a
small program; a program that pulls in the raylib spice compiles its inline C
too, which P0 measures); nothing is installed.

Concretely, this program -- the raylib README's quick start, unchanged --
pasted into the editor and run, draws its text in the screen pane:

```turmeric
(import raylib/core  :refer [init-window window-should-close begin-drawing
                             end-drawing clear-background])
(import raylib/text  :refer [draw-text])
(import raylib/color :refer [raywhite black])

(init-window 480 270 "hello")
(while (not (window-should-close))
  (begin-drawing)
  (clear-background (raywhite))
  (draw-text "Hello, Turmeric!" 120 120 20 (black))
  (end-drawing))
```

Non-goals for v0: multiplayer, saving projects server-side, mobile touch
controls beyond what raylib's web backend already maps, WebGPU, a debugger.

## 2. What exists today, and what it means for this

Checked against `main` (v0.72.0) and turmeric-spices on 2026-10-23; the browser
JIT rows and the spice findings were re-checked on 2026-10-29.

| Piece | Where | What it gives this plan |
|---|---|---|
| Try Turmeric | `web/try/`, `web/main.js` (6.8k lines), `web/public/eval-worker.js`, `web/public/jit-worker.js`, `web/engine.js` | An editor-plus-console SPA with Monaco, LSP in a worker, share URLs (`share-codec.js`), an examples menu, a CSP (`csp.js`), a service worker, Playwright tests, a Cloudflare Worker deploy, COOP/COEP headers (`web/public/_headers`). It has **two engines** behind the language menu's Engine section: Interpret (`libturi_wasm`, `turi_wasm_eval` in a Web Worker -- the tree-walker) and, since 2026-10-28, **Compile** (`turmeric-jit.{js,wasm}`, `turi_wasm_jit_run` in `jit-worker.js`). |
| The browser JIT | [wasm-jit-plan](wasm-jit-plan.md); `src/web/wasm_jit_glue.c`, `tools/wasm-jit/` (`build-module.sh`, `gen-symtab.py`, `headers.txt`), `src/jit_engine.c` (`__EMSCRIPTEN__` arm), `tur_front` (`src/driver/pipeline.c` and four siblings) | The driver's front half emits C in the page, c2mir compiles it to MIR, MIR's interpreter runs it. **Inline C and `extern-c` work.** Runtime calls go through generated exact-signature stubs, not a JS round trip; `setjmp` works; `exit`/`abort` are caught. 5.27 MB wasm (1.84 MB gzipped), a second module loaded on first use. Corpus: 2,488 compiled, 191 fell back right, 254 failed, 241 skipped (`tests/jit-wasm-baseline.txt`); every Try example compiles or falls back with a reason. **Not yet:** it runs on a Web Worker and `main` runs on the calling thread to completion (no frame loop), no module in it knows about raylib, and its compile has no spice search path (`compile_to_c` is called with none). Since 2026-10-30 it links `setjmp` on wasm exception handling (recursion ~700 in the worker, 1,100 on a main thread), which rules Asyncify out of this module and JSPI in (prerequisite 3). |
| The shared package | `stdlib.data` (CMake `tur_stdlib_pkg`, wasm-jit-plan 5.11) | Files the page needs at runtime ship as an Emscripten `file_packager` package mounted at `/stdlib` by a loader linked into both modules, not embedded in the wasm. The same mechanism can carry spice sources (section 5.3). |
| raylib's web arm | turmeric-spices `spices/raylib/build.tur` (`:cmake-deps` raylib 5.5, `:wasm-options` `PLATFORM=Web`, `GRAPHICS=ES3`; `:build-opts :c-sources ["c/raylib/rl_web_loop.c"]`), `raylib/web.tur` (`run-main-loop`, `cancel-main-loop`, `with-web-game-loop`) | raylib 5.5 builds under `emcmake` and renders at 60 fps in Chromium (wasm-spices-plan section 12). The frame-callback shim exists. **Checked 2026-10-29: every module under `src/raylib/` is Turmeric with inline-C bodies that `#include <raylib.h>` and call raylib directly** (Color, Vector2, Rectangle, textures and the rest are `malloc`'d boxes passed to raylib by value, `DrawCircleV(*v, ...)`); the only native C of the spice's own is the 79-line `rl_web_loop.c`. |
| `tur build --target wasm` | `src/driver/cmd_build.c`, `pkg_web_compose_link_flags` in `src/compiler/pkg.c` | Compiles a project with `emcc`, composes the GL/audio/asyncify/heap link line from the `:web` block. Produces `.js` + `.wasm`; **no HTML shell**, no `--preload-file`, no `*args*` (wasm-spices-plan W1 is not built). |
| Spice exports as interpreter natives | `src/turi/spice_loader.c` (build + dlopen + manifest), `src/turi/ffi_thunk.c` (`tur_ffi_install_spice_bindings`, by-value records, F5 callbacks from C back into Turmeric), `turi_env_attach_spice` (`src/turi/eval.h:700`) | The REPL already binds a compiled spice's exported defns as natives, marshals int/float/cstr/bool/nil and records, and lets C call a Turmeric closure. Every raylib handle type is `defopaque ... :int`, so raylib's whole surface is int-class. **Natively this goes through `dlopen`**; there is no static-image constructor yet. |
| Interpreter under Asyncify | `src/turi/env.h:33` (Emscripten fibers for continuations, "requires `-sASYNCIFY=1`") | The interpreter already has an Asyncify-aware build shape, so linking the playground module with `-sASYNCIFY` is not new ground. Whether `tur_wasm` is linked with it today: **verify in P0** (`src/CMakeLists.txt` names no `ASYNCIFY`). |
| `defscript :spices` | `script-spice-deps-plan`, `--enable=script-deps` | A single file can declare the spices it needs. This is the syntax a playground program should use to say what it pulls in, whichever architecture serves it. |
| Spice compatibility matrix | wasm-spices-plan section 9 | Which spices can be in the browser at all. Pure and inline-C spices work; cmake deps that are portable C work; anything socket-shaped does not. |

Three facts from that table shape everything below:

1. **There is a C compiler in the browser now, and it is slow-start but
   compiled.** The original draft said the opposite (no C compiler, MIR does
   not target wasm) and built its recommendation on that. c2mir and MIR's
   interpreter do run on wasm32, so a program typed into the page can be
   compiled *and* its inline C (including the raylib spice's) can be compiled
   with it. What the browser cannot do is link: `libraylib.a` has to be inside
   the module ahead of time, and its entry points have to be in the module's
   import table (wasm-jit-plan 5.3).
2. **The browser calls the program, not the reverse.** A raylib frame loop
   on the page's main thread must yield every frame. Compiled programs do
   that with `run-main-loop` or Asyncify. An interpreted blocking `while`
   loop needs Asyncify too. **Neither wasm module is linked with
   `-sASYNCIFY` today** (`src/CMakeLists.txt` and `tools/wasm-jit/build-module.sh`
   name none); that alone is why 49 corpus fixtures fail on the JIT module
   (`emscripten_fiber_swap`, an unowned family in wasm-jit-plan P5). Those
   failures say nothing yet about whether Asyncify *would* work under the MIR
   interpreter: nobody has linked the module with it. So the blocking style is
   an open question for the JIT arm, not a given; the callback style
   (`with-web-game-loop`, `run-main-loop`) needs no Asyncify.
3. **raylib needs the main thread.** Its GLFW port wants the canvas and DOM
   input events, which a worker lacks. The browser JIT runs in a worker today.

## 3. The central decision: how does typed code reach raylib?

Three architectures. The recommendation is **J now, A only if J's spike
fails, and no build service** -- designed so the screen pane cannot tell
them apart.

### J. The JIT engine on the page, raylib linked in (recommended)

One Emscripten module -- the **arcade module** -- is `turmeric-jit` (wasm-jit-plan:
the front half, c2mir, MIR's interpreter, the resident runtime) with
`libraylib.a` (the web build, ES3) and raygui's one TU linked in, and the
raylib entry points added to the import table (`gen-symtab.py`, wasm-jit-plan
5.3) so `MIR_link` finds `InitWindow` and `DrawCircleV`. The page compiles the
user's program with `turi_wasm_jit_run` exactly as the Compile engine does. The
spices are **source**, not images: `raylib/core`, `raylib/shapes`, ... are
Turmeric files whose inline-C bodies `#include <raylib.h>`; c2mir compiles
them with the program, and `raylib.h` is served from MEMFS beside the musl
subset (wasm-jit-plan 5.4). `rl_web_loop.c`, the one native TU the spice owns,
is compiled into the module with emcc like raylib itself.

- **Why this beats A for raylib.** It deletes A0/A1/A2 (the per-export `__ffi`
  shims, the static `TurSpiceImage` constructor, `turi_wasm_attach_images`,
  the sandbox cap that lets natives in without granting `extern-c`): the
  spice needs no marshalling layer because it is not called across an
  interpreter boundary, it is compiled in the same unit as its caller. By-value
  records (raygui's `Rect`) are C structs, not a `note_records` problem. User
  code may contain inline C and `extern-c`, which A refuses, so
  `inline-c-plasma.tur` (section 8, "Later") is in scope on day one.
  Compiled semantics: what the page runs is what `tur jit` runs, and the
  example's native twin (section 5.4) exercises the same emitter output.
- **Speed.** The MIR interpreter on wasm32 measured about 7x slower than
  `clang -O0` on a tight loop; the tree-walker is about 20x slower than MIR's
  interpreter natively (wasm-jit-plan section 4). That is the difference
  between "life.tur at 5,184 cells a frame misses 30 fps" and "probably
  meets it", which P0 measures rather than assumes.
- **Adding a spice is data, not a rebuild**, for every spice that is pure or
  inline-C: its `src/` tree is a file package fetched on demand (section 5.3),
  the way `stdlib.data` is. Only a spice with a native dep (raylib's
  `libraylib.a`, raygui's TU) needs a module relink.
- **Honest limits.** (1) Recursion depth is about 1,100 Turmeric calls on a
  main thread, 700 in a worker, since the module links `setjmp` on wasm
  exception handling (2026-10-30,
  [depth report](../reported/wasm-jit-recursion-depth-is-about-300.md);
  wasm-jit-plan's 1,696 was a pre-P5 C spike), not the tree-walker's
  heap-bounded depth; the engine toggle says so. (2) A program the engine refuses (threads,
  an autolink) falls back to the tree-walker, which in *this* module has no
  raylib, so for the arcade the fallback must be an error that names the reason,
  not a second engine. Today `turi_wasm_jit_run` falls back unconditionally
  (`run_interpreted` in `src/web/wasm_jit_glue.c`), so the arcade needs a
  switch to turn that off (J3). (3) The ~125-180 ms front half and ~110-170 ms c2mir
  are a fixed cost on every Run; a spice's inline C adds to c2mir's share. (4)
  The corpus still has 254 failures; the ones that matter here are
  [ilp32-word-slots-truncate-64-bit-carriers](../reported/ilp32-word-slots-truncate-64-bit-carriers.md)
  shapes 2-3 (`vec`/HAMT values, `life.tur`'s grid) and the open Asyncify family.
- **What nobody has measured**, so it is P0's job and not an assumption: calling
  a C function that takes a struct by value (`DrawCircleV`, `DrawRectangleRec`,
  `DrawText` with a `Color`) from MIR-interpreted wasm32 code (expected to work,
  J1 says why); an interpreted
  function as the frame callback `emscripten_set_main_loop` calls (a thunk is a
  table entry since mir#16, so it should work); `simulate_infinite_loop=1`
  unwinding out of a `main` that is running *inside* the MIR interpreter's
  `eval` frames (the P5 corpus has two `unwind` traps); and the module on the
  main thread of an iframe. Section 7 lists them with the prerequisites.
- **The blocking `while` style is the real fork inside J.** raylib's web
  `WindowShouldClose` and `EndDrawing` yield through `emscripten_sleep`, which
  needs Asyncify. What sits between `main` and that sleep, read from the code
  on 2026-10-29: the glue enters `main` through `MIR_interp_arr` (no JS frame
  at the bottom, `src/jit_engine.c`), and an interpreted-to-interpreted call is
  dispatched in C (`wasm_ff_dispatch`). But on wasm the interpreter's `eval`
  calls `setjmp` in its own frame (`INTERP_SETJMP`, `mir-interp.c:47`,
  turmeric-lang/mir#17), and LLVM's SjLj lowering then routes the calls `eval`
  makes through Emscripten's `invoke_*` wrappers -- JS frames, under every
  native call the program makes, 5.5's stubs included. Asyncify has to unwind
  through those. (That assumes the link's SjLj is Emscripten's JS-based one: it
  passes no `-fwasm-exceptions`, and wasm-native SjLj is incompatible with
  Asyncify outright. Confirmed 2026-10-29: a trap's stack trace on the module
  reads `eval` -> `invoke_iiiiii` (JS) -> `call_insn_execute`, and
  `MIR_interp_arr` -> `invoke_viiiii` -> `eval`.) The other JS frame, the one-import instance a host-to-interpreted
  call goes through (`mir_wasm_set_interp_entry`, `mir-wasm.c`), Asyncify does
  not instrument; the blocking style never sleeps under one, but a sleep inside
  a comparator or a frame callback would. **Measured 2026-10-29 (prerequisite
  3): Asyncify works under the MIR interpreter.** A JIT module relinked with
  `-sASYNCIFY` runs a compiled `while` loop over `^mut` state that calls
  `emscripten_sleep(16)` from inline C each frame; the sleep unwinds through
  `eval`, the `invoke_*` wrappers and the stubs, and node's event loop runs
  meanwhile. The cost: the module goes from 5.46 to 8.31 MB (1.89 to 2.63 MB
  gzipped), the 10-million-step loop runs about 1.6x slower, and recursion
  depth does not change. So the blocking examples can run unchanged; P0 still
  has to show raylib's own sleep path and the fps in a browser. **A second
  route, measured 2026-10-30:** build the module with wasm-native `setjmp`
  (which also removes the JS wrappers and doubles the recursion depth, see
  the [depth report](../reported/wasm-jit-recursion-depth-is-about-300.md))
  and link `-sJSPI` instead of Asyncify. The same loop yields, the wasm does
  not grow at all, and the interpreter is not slowed. Asyncify cannot be used
  with that build (wasm exceptions), so the two routes are two object sets.
  **Decided 2026-10-30: JSPI.** The arcade module is the default JIT build
  (wasm-native `setjmp` since the same day) with `TUR_WASM_JIT_JSPI=1`;
  Asyncify is not built. Its cost is browser support: the blocking examples
  need a browser with JSPI (Chrome today; Firefox has an Intent to Ship,
  Safari 27 beta has it), the page says so where it is missing, and the
  callback examples (`with-web-game-loop`) run everywhere.

### A. Interpreter + a baked spice image (fallback)

Everything the previous draft of this section said, kept because it is the
plan if J's spike fails (most likely cause: the frame loop -- the callback
outliving `main`, or no workable Asyncify -- or the main-thread/iframe boot). One module links `libturi_wasm` with
the web `libraylib.a`, raygui's TU and the AOT-compiled exports of a curated
spice set. At startup it installs those exports as interpreter natives through
the existing `ffi_thunk` shim and embeds the spices' `.tur` sources next to the
stdlib, so `(import raylib/core ...)` elaborates with real types. The page then
evaluates the program with `turi_wasm_eval`, as Try Turmeric's Interpret
engine does.

- It runs on the **main thread of a sandboxed iframe**, linked with `-sASYNCIFY`
  (the tree-walker already has an Asyncify-aware shape, `src/turi/env.h:33`), so
  the blocking `while` style runs unchanged at the ~16 ms-a-frame cost
  (wasm-spices-plan section 12, finding 3). `run-main-loop` with an interpreter
  closure also works, through F5 callbacks.
- "Other spices" means the spices **baked into the module**: adding one is a
  rebuild and redeploy.
- Cost: inline C in user code is refused; interpreted semantics; one large
  module; and A0-A4 (section 6), which is real interpreter work.

### B. Build service (not planned)

A server that runs `tur build --target wasm` and returns a bundle was the
original second phase, for "any wasm-clean spice" and inline C. J provides both
in the page, with no container, no sandbox for arbitrary C, no rate limits and
no deploy that depends on a service being up, so **B is dropped from the
schedule** and kept only as an escape hatch for a program that outgrows the
MIR interpreter. If it is ever needed, the screen pane's contract (C below)
already admits it, and wasm-jit-plan section 8's T2 (a MIR-to-wasm generator)
is the cheaper route to speed than a server.

### C. One screen pane for all of them

The screen pane's contract is "load an Emscripten `MODULARIZE`d bundle into
the iframe, hand it a canvas, forward stdout/stderr to the console, report
exit." J loads the arcade module and compiles; A loads its module and evals;
B would load a per-program bundle. The editor, examples, share URLs, LSP and
console are identical, so the architecture can change under the page.

### Decision

**J first, behind a P0 spike; A if J fails the spike; B not scheduled.** J reuses
Try Turmeric's Compile engine as built (the front half, c2mir, the shared
package, the engine line), needs no server, and removes the interpreter-side
work A needed. It trades a ~0.3 s Run for compiled semantics and inline C.
The blocking frame loop runs under JSPI, not Asyncify (decided 2026-10-30).

The honest limits of the chosen engine go on the page: a small "Compiled in
your browser (MIR interpreter); recursion limit ~1,100 calls; threads are not
available" note next to the Run button, the way Try Turmeric notes the
eval watchdog.

## 4. Where it lives

Three candidates were asked about. Recommendation, and **the owner's decision
on 2026-10-30: its own repository**, created the same day as
`turmeric-lang/turmeric-arcade`, to deploy to `play.turmeric-lang.com`.

| Option | For | Against |
|---|---|---|
| **Own repo, `turmeric-lang/turmeric-arcade`**, the site at `play.turmeric-lang.com` -- recommended, and chosen | Its build needs emsdk, raylib's web archive and a pinned turmeric-spices checkout; none of that belongs in the compiler repo's deploy. Own release clock (a new example or spice should not wait on a compiler tag). The extras-repo-split pattern exists: pin a `tur` release (`scripts/install-tur.sh` in turmeric-spices is the template), pin a spices ref, build the module in its own CI. The editor-palette contract (`web/editor-palette.json` + `tests/check-editor-palette.py`) already shows how a sibling repo stays in step with this one. | Duplicates the parts of `web/main.js` it needs (eval-worker pattern, `share-codec.js`, `csp.js`, examples menu, theme). The LSP worker needs the spice sources embedded too, so it builds its own `lsp-worker` module rather than reusing Try's. Mitigation: copy, do not abstract; the arcade page is a fraction of `main.js`. |
| A fifth page in `web/` (`web/arcade/index.html`, next to `try/`, `tour/`, `trowel/`, `ci/` in `vite.config.js`) | Maximum reuse of `main.js`; one deploy; the LSP worker could be one module for both pages. | `tur run wasm` and `deploy-web` would need emsdk's raylib build and a spices checkout; the compiler's release-cut workflow would depend on turmeric-spices being green. That coupling is the thing extras-repo-split was undoing. |
| A spice | -- | A spice is a library `tur` builds; this is a site. The closest fit, a `:bin` that serves the page, still needs the web build pipeline, which spices do not have. The *examples* can be a spice-shaped directory (plain `.tur` files with `defscript` headers), but the app is not one. |

What lands **in this repo** regardless: the compiler- and engine-side plumbing
in section 6, because the arcade module is the JIT module (`tools/wasm-jit/`,
`src/web/wasm_jit_glue.c`) plus a raylib link, and the page talks to it.

What lands **in turmeric-spices**: the `:web #map{:supported ...}` stubs
wasm-spices-plan W0 already calls for, which the arcade's spice picker reads to
decide what is offered. Under J nothing in the raylib spice itself changes (it
is inline C already); `rl_web_loop.c` is compiled into the module as it is.

## 5. The page

### 5.1 Layout

```
+--------------------------------------+-------------------------------+
|  Run  Stop   Examples v  Spices v    |  [screen 480x270, scaled]     |
|  Share   #lang v                     |     ... canvas ...            |
+--------------------------------------+  fps 60  [pop out] [full]     |
|                                      +-------------------------------+
|   Monaco editor                      |  console                      |
|                                      |  > Hello from frame 1         |
|                                      |                               |
+--------------------------------------+-------------------------------+
```

The left half is Try Turmeric's editor pane. The right half is a vertical
split: the **screen** above, the **console** below, with Try's existing
drag handle between them. On a narrow viewport the three stack
(editor, screen, console), screen first after Run.

### 5.2 The screen pane

- **An iframe per run.** Run creates a sandboxed `<iframe>` (same origin,
  `sandbox="allow-scripts allow-same-origin"`), loads `screen.html`, which
  boots the arcade module on the **iframe's main thread** (raylib needs the
  canvas; the Compile engine's worker has none), compiles and runs the program
  it receives by `postMessage`, and prints the engine line (front, c2mir, run
  ms) to the console as Try's Compile engine does. Under JSPI
  `turi_wasm_jit_run` returns a Promise the shell awaits, and a `run-main-loop`
  program leaves `main` by Emscripten's `unwind` exception, which the shell
  takes as the normal end of `main`, not a trap. Stop removes the iframe. A second Run replaces it. No module
  teardown is ever attempted (wasm-spices-plan 6.9); the iframe is the
  teardown.
- **Canvas size.** Default 480x270 (16:9, four times smaller than a 1080p
  frame), set by the program's own `init-window w h`: the shell reads the
  requested size when raylib creates the context and sizes the canvas to it,
  then scales the canvas to fit the pane with CSS (`object-fit`-style,
  integer scale when it fits). `devicePixelRatio` scales the drawing buffer
  (wasm-spices-plan 6.4). A program asking for 1920x1080 gets it, scaled
  down; the pane stays the pane.
- **Focus and keys.** The iframe's canvas has `tabindex="0"`; Run focuses it,
  and a click on the screen re-focuses it. Arrow keys, Space and Tab are
  `preventDefault`ed **inside the iframe only**, so the editor keeps its
  own keys (6.5).
- **Audio.** Try's Run click is a user gesture on the *parent*, which does not
  unlock an `AudioContext` created later in a new iframe. So the screen shell
  shows a click-to-start overlay **only if** the module reports a suspended
  `AudioContext` after `init-audio-device` (6.6); silent programs never see
  it. Link flags carry `ccall,HEAPF32` (section 12, finding 5).
- **Console.** stdout and stderr stream from the iframe to the parent's
  console pane by `postMessage`, through the same line-assembly the eval
  worker uses (`emitStreamLine`, so a partial line still shows). An
  interpreter error ends the run and is printed red, with the line number
  linked back into the editor.
- **FPS and frame time** in the pane's corner, from `requestAnimationFrame`
  deltas in the shell, so a visitor sees the engine's cost (and Asyncify's, if
  J4 went that way) rather than wondering.
- **Pop out** opens the same `screen.html?share=...` in a new tab at native
  size; **fullscreen** uses the Fullscreen API on the iframe (gesture
  required, so it is a button). Neither resizes the canvas (6.4 deferred
  resize); they scale.
- **Watchdog.** A program that never yields (no raylib call in its loop)
  freezes only the iframe; the parent's Stop still works because it is a
  different document. A 30 s "still running, no frame drawn" prompt offers
  Stop.

### 5.3 Spices

- A **Spices** menu lists what the arcade offers, grouped by the matrix tiers,
  each with its one-line description from `build.tur`. Ticking one inserts or
  updates a `defscript :spices` header at the top of the buffer (the same way
  the `#lang` picker edits the `#lang` line in place: the buffer is the source
  of truth).
- The header is the **real** `defscript` form, so a program copied out of the
  page runs natively with `tur --enable=script-deps file.tur` and fetches
  the same spices. On the page the header is satisfied **from what is mounted**,
  never from the network: a name the arcade carries resolves, an unknown name
  is refused with the name and "not in this build".
- **Under J a spice is a file package.** Each spice's `src/` tree ships like
  `stdlib.data` does (wasm-jit-plan 5.11): one Emscripten `file_packager`
  package per spice, fetched when the program names it, mounted at
  `/spices/<name>/src`, and put on the front half's module search path. The
  inline-C bodies in those files are compiled by c2mir with the program, so a
  pure or inline-C spice (`math`, `ecs`, `ecs-raylib`, `linalg`, `json`'s
  Turmeric half, `raylib` itself, `raygui`) needs **no module relink**; only
  the native deps they call do (`libraylib.a`, raygui's TU, any cmake dep), and
  those are in the module and its import table. A spice's package set is its
  closure: `raylib` pulls in `math` (its `build.tur` `:spices`), and `raygui`'s
  inline C finds `raygui.h` through its manifest's `:c-includes`, which the page
  never reads, so the header goes to `/sysinclude` (J1). Whether a package can be
  mounted after the module has started is a P0 check; if not, the arcade
  builds one package of every offered spice, which is still a data rebuild and
  not a relink. **Built in P1 (2026-10-30):** a package is a JSON file of the
  spice's `src/` texts (`tools/wasm-jit/arcade/pack-spices.py`; raylib is 12
  files, 84 KB before compression), not a `file_packager` one, whose loader
  runs only before `main`; `spices.js` mounts a closure after start-up (math
  then raylib: 240-260 ms in the smoke test, both fetches included), and the
  module no longer embeds any spice. The page reads the names from the
  program's imports and its `defscript` header; the glue refuses a header name
  that is not mounted.
- Imports work without the header too, for the mounted set: a visitor who
  pastes the raylib README's quick start should not have to know about
  `defscript`. The header exists for portability.

### 5.4 Examples and sharing

- An **Examples** menu as on Try, fed from `examples/*.tur` at build time
  (a Vite plugin reads the directory; no hand-maintained JS object, unlike
  Try's `EXAMPLES` table). Each file's first `;;;` line is its menu entry.
- Share URLs reuse `share-codec.js` (compressed hash). A shared URL opens
  with the program loaded and **does not auto-run**: a run needs a gesture
  for audio and should not surprise someone who clicked a link.
- Every example is also a native program: `tur --enable=script-deps run
  examples/pong.tur` opens a window. The arcade repo's CI runs each example
  natively for a few frames (`xvfb-run`, a `--frames N` arg read from
  `*args*`) and in the page under Playwright (first frame drawn, no console
  error).

### 5.5 Editor intelligence

The LSP worker (`web/public/lsp-worker.js` pattern) is rebuilt as an arcade
variant that mounts the offered spices' `src/` trees (the same packages the
runtime fetches), so completion and hover know `draw-circle`. It runs the
interpreter module's elaboration only (`src/web/wasm_lsp.c`), so it needs no
raylib and no JIT. Diagnostics come from the same elaboration the front half
runs, so a program the LSP accepts is one the compiler accepts.

## 6. Compiler- and engine-side work (this repo)

Ordered by dependency. **J items are the plan**; the A items below are the
contingent track, kept in full because P0 may send the work there; the B items
are cut down to what survives.

### J -- the arcade module is the JIT module plus raylib

- **J0. Link raylib into the JIT module.** `tools/wasm-jit/build-module.sh`
  today links `tur_core` + `tur_front` + the web glue, the engine, and MIR /
  c2mir. Give it a list of extra objects and archives (the web `libraylib.a`,
  raygui's TU, `rl_web_loop.c`) and compose their link flags (`-sUSE_GLFW=3`,
  `-sMIN_WEBGL_VERSION=2`, the GL/audio/heap flags `pkg_web_compose_link_flags`
  in `src/compiler/pkg.c` already derives from a `:web` block) with the
  module's own (`-pthread`, `-sALLOW_MEMORY_GROWTH`, `-sSTACK_SIZE=16777216`).
  Open: how the archive is built for the arcade -- `tur build --target wasm` on a
  stub project already runs `emcmake` over raylib's `:cmake-deps`
  (`:wasm-options`), the arcade repo's CI can call it once and cache the
  archive by raylib ref. The arcade module is `tur_wasm_arcade` (or the JIT
  target with `TUR_WASM_JIT_EXTRAS=<list>`), not a fork of the script.
- **J1. The raylib entry points in the import table, at their real types.**
  `gen-symtab.py` builds the table from the runtime's declarations region and a
  libc list. Add a mode that reads prototypes from a header (`raylib.h`, then
  `raygui.h`'s function declarations) and registers each function at its real
  wasm signature. raylib is the first import set that passes **structs by
  value** (`Color` is four bytes, `Vector2` two floats, `Rectangle` four;
  `Texture2D`, `Camera3D` are bigger), which the wasm C ABI passes
  indirectly. The exact-signature stubs (5.5, `--stubs`) are generated per wasm
  signature, so they have to be generated from the same prototypes. The
  calling side is already designed for it: c2mir's wasm32 ABI passes every
  aggregate by address and returns one through an sret pointer
  (`c2mir/wasm32/cwasm32-ABI-code.c`), and `mir-wasm.c` passes a block argument
  as an `i32` address, which is what clang does too. The one known mismatch is a
  **single-element** struct, which clang passes as its scalar and c2mir passes by
  address; check `raylib.h` for one passed by value. P0 step 1 is one call to
  confirm, not the likeliest failure. `raylib.h` (and `raygui.h`) join
  `/sysinclude` through `headers.txt`'s mechanism.
  Also check how the autolink works for a spice's `:cmake-deps`: the JIT arm
  has no `dlopen` and turns a `-l<name>` marker into a refusal that names the
  library (wasm-jit-plan 5.7), but on the page there is no `build.tur` to
  produce the marker; confirm the program compiles without one and that a
  library the module already carries is simply satisfied.
- **J2. Spice roots on the page.** `turi_wasm_jit_run` calls
  `compile_to_c(path, &csrc, NULL, 0, NULL, 0)` with no include or spice
  directories, so `(import raylib/core)` cannot resolve. Add the mounted
  spice roots (`/spices/<name>/src`) to the front half's search path (the
  same `spice.c` discovery `tur` uses, in `tur_front`), and decide what a
  `defscript :spices` header does in the page: the front half reads the form
  under `--enable=script-deps`, which would try to fetch, so the glue satisfies
  it from the mounted roots instead and refuses an unknown name. The
  per-spice packages generalize `tur_stdlib_pkg` (wasm-jit-plan 5.11).
- **J3. The frame loop on the main thread.** `turi_wasm_jit_run` runs `main`
  to completion and tears the context down; a callback-style program's
  `main` calls `run-main-loop`, whose `emscripten_set_main_loop(..., 1)`
  *unwinds out of `main`* and lets the browser call an interpreted function
  later. So: (a) the MIR context and every image must outlive `main` (the REPL
  already keeps contexts, `g_jit_keep_contexts`), (b) the unwind exception
  passing through MIR's `eval` frames must not be mistaken for a trap by the
  page's trap handler (`trapped` retires the instance, wasm-jit-plan P4) and
  must leave the interpreter's frame state sound -- under JSPI it arrives as
  the export's rejected Promise, (c) the frame function's
  thunk must be callable by host code (a table entry since mir#16), and (d) the
  whole thing runs on the iframe's main thread, so `tur_run_on_big_stack`-style
  create-then-join code must run on the caller's stack as it already does on
  Emscripten (wasm-jit-plan P4, "the emitter deadlocked in the browser"); any
  other create-then-join in code the page reaches would hang the same way.
  (e) The glue's fallback to the tree-walker (`run_interpreted`) is switched
  off for the arcade, whose interpreter has no raylib: a refusal is reported
  with its reason instead.
- **J4. JSPI (decided 2026-10-30; prerequisite 3 measured both).** Build the
  arcade module with `TUR_WASM_JIT_JSPI=1` (`tools/wasm-jit/build-module.sh`:
  `-sJSPI -sJSPI_EXPORTS=turi_wasm_jit_run`, and `emscripten_sleep` in the
  import table), so `WindowShouldClose`'s sleep suspends the wasm stack and
  the blocking `while` runs unchanged: no size or speed cost, the depth of the
  wasm-`setjmp` build. The constraints: a sleep must have only wasm frames
  above it (the blocking style sleeps from `main`, entered with none; a sleep
  inside a frame callback called through the table's JS closure fails with
  `SuspendError`), and the browser must have JSPI (`'Suspending' in
  WebAssembly`; Chrome 137+ today). Without it the page runs the callback
  examples and tells the visitor the blocking ones need a browser with JSPI
  for now. Asyncify was measured too (+52% module, about 1.6x slower, and it
  needs objects compiled without `-fwasm-exceptions`, which the default build
  no longer produces) and is not built. If P0 step 3 finds raylib's own
  sleep path does not suspend, J4 falls back to "document the callback form
  and rewrite the examples", and `with-web-game-loop` becomes the front door.
- **J5. `*args*`.** `turi_wasm_jit_run(src, args)` already takes a
  newline-separated argument string (`split_args` in `wasm_jit_glue.c`);
  confirm it reaches `*args*` as `tur run` would, which retires the
  `--frames N` convention's wasm-spices-plan W1 dependency for J. (A needs W1's
  `turi_wasm_set_args` instead.)

### A -- contingent: interpreter + baked spice images

Only if P0 sends the work here. A0's measurements stand either way.

- **A0. How spice exports are called under emcc -- the call mechanism
  answered 2026-10-24; use the per-export `__ffi` shims, not the shape
  table.** Measured with emcc 6.0.13 / node: six exports and their `__ffi`
  shims copied verbatim from a `tur build --shared` of a small spice, plus
  the real generated table (`src/runtime/ffi_dispatch{,_thunk}.c`), each
  export called both ways. Natively all ten calls agree. Under wasm32 the
  shims return the right value for every export, while **the table traps --
  `RuntimeError: null function or function signature mismatch` -- for any
  export with a `bool` or `cstr` parameter**, because the exports keep their
  real C types (`bool f(bool)`, `int64_t g(const char *, int64_t)`) and the
  table calls every int-class argument through an `int64_t` function-pointer
  cast; on wasm32 a pointer and a `bool` are `i32`, and wasm checks the type
  of every indirect call. `int`, `float` and opaque handles (`:any` in the
  manifest, int class) work either way. In the raylib spice, 15 of 84 defns
  take or return a `cstr` or `bool` -- every `load-*` path and the input
  predicates (`is-key-down`, `is-key-pressed`, ...) -- so **the table cannot
  serve this plan even if it is kept**; the shims can. The `__ffi` shim is
  therefore part of A1's image (below), and this plan no longer depends on
  the table at all ([ffi-spices-integration-plan](../archive/ffi-spices-integration-plan.md)
  S5 deletes it). The table also adds ~28 KB to a linked wasm module, so
  dropping it trims the browser bundle.
  Still open from A0: `tur_wasm` is **not** linked with `-sASYNCIFY` today
  (`src/CMakeLists.txt`, the `tur_wasm` emcc link flags); what the
  interpreter's fiber shim costs with it (wasm-spices-plan measured +21.7 %
  on a tiny program; libturi is not tiny) is measured in P0.
- **A1. A static spice image.** Today a `TurSpiceImage` is built by
  `tur build --shared`, `dlopen`ed, and its exports manifest read from disk
  (`spice_loader.c`). Add the static twin: `tur build --target wasm --image
  <spice>` emits the spice's object code plus a generated
  `<spice>_image.c` holding the manifest text and a `{ name, fnptr, ffi_shim }`
  table -- `--image` emits the `__ffi` shims as `--shared` does
  (`emit_ffi_export_shims`), and the image sets each export's `ffi_shim`
  from the table, since the shim is how an export is called in the
  browser (A0),
  and `tur_spice_image_from_table(manifest, table)` constructs the image
  without `dlopen`. `turi_env_attach_spice_image(env, img)` then installs
  the bindings through `tur_ffi_install_spice_bindings_owned`. `--shared`
  and `--target` are mutually exclusive today (`cmd_build.c`); `--image`
  is the third mode, not a relaxation of that. The by-value record path
  (`note_records`) must survive: raygui's `Rect` is one.
- **A2. `turi_wasm_attach_images()` and embedded spice sources.** A new
  export in `src/web/wasm_glue.c` that attaches every baked image at init,
  before the prelude, and a CMake target (`tur_wasm_arcade`, or a
  `TUR_WASM_IMAGES=<list>` option on `tur_wasm`) that links the images'
  objects, the raylib web archive and raygui's TU, embeds the spices' `src/`
  under `/spices/<name>/src` with `--embed-file`, and adds those roots to
  the module search path. The interpreter's existing rule applies: an
  `import` of a module whose exports are bound natives keeps the natives
  rather than evaluating the inline-C source (`tur_ffi_native_spice_module`).
  The sandbox caps: the env must grant the attached images without granting
  `TURI_CAP_FFI` to user code (`extern-c` stays refused on the page).
- **A3. Main-thread eval and the frame loop.** `turi_wasm_eval` on the main
  thread, under Asyncify, with raylib's `WindowShouldClose` yielding. One
  fixture-style check that a `while (not (window-should-close))` program
  interpreted in the arcade module returns to the browser each frame
  (Playwright: `requestAnimationFrame` keeps firing while the program runs).
  `run-main-loop` with an interpreter closure goes through the F5 callback
  context; `tests/fixtures/region-escape-via-main-loop` already pins the
  compiled closure ABI, and the interpreter side needs its own case.
- **A4. `*args*` from the page.** wasm-spices-plan W1's `turi_wasm_set_args`
  escape hatch, so `--frames N` style arguments reach the program through
  `*args*` (CLAUDE.md's rule on how programs *read* args is unchanged).
### B -- what survives of the build service

- **B0. `:web :supported` stubs (wasm-spices-plan W0).** Still needed: it is
  what turns "this spice cannot be in a browser" into a message with the
  spice's name, and what the Spices menu reads. Asset packaging
  (`--preload-file`, W1) is needed for examples with textures or sounds under
  any architecture: in J an asset is a file in the program's MEMFS, so
  decide in P3 whether examples draw only shapes and text (no assets) or ship a
  small asset package beside the spice packages.
- **B1/B2. Cut.** A `defscript` file through `tur build --target wasm` and a
  container-hosted build service are not scheduled (section 3, B).

## 7. Phases

### Prerequisites before P0

Checked 2026-10-29 against the browser JIT as landed. Each row says what is
wrong or missing, whether it gates P0 (the spike), P1 (the module) or only
later, and where it is tracked. Nothing here needs the arcade repo to exist.

| # | Prerequisite | Gates | Why this plan cares | Tracked in |
|---|---|---|---|---|
| 1 | **Decide J over A, and the repo.** The owner confirms section 3's flip and section 4's own-repo choice. **Both settled 2026-10-30:** J, by the JSPI decision (JSPI is a J-only route), and its own repository (section 4). | P0 | P0's scope is different under each: J tests struct-by-value calls and the main-thread boot, A tests `ffi_thunk` shims under Asyncify. | this plan |
| 2 | **`-sSTACK_OVERFLOW_CHECK=2` on the JIT link.** One flag, one test. **Done 2026-10-29**: the module links with it (and `-sCHECK_NULL_WRITES=0`, see row 5), the page reports the abort as `stack overflow: recursion too deep`, and `jit-engine.spec.js` pins it. No measurable cost on the loop benchmark; +97 KB. | P0 | An iframe runs arbitrary user programs. Without the check a linear-stack overflow writes past the 16 MiB stack instead of stopping, and the spike's numbers (and the first user bug report) are of a corrupted module. | [wasm-linear-stack-overflow-unchecked](../reported/wasm-linear-stack-overflow-unchecked.md) |
| 3 | **Decide how Asyncify gets measured on the MIR interpreter.** Neither wasm module is linked with it, and 49 corpus fixtures fail on `emscripten_fiber_swap`. Link a throwaway JIT module with `-sASYNCIFY` and run a compiled program that calls `emscripten_sleep`; that is P0 step 3 and the answer to whether the blocking style survives. The question is the `invoke_*` frames that `eval`'s own `setjmp` (mir#17) puts under every call it makes (section 3, J), not the stubs. **Done 2026-10-29** (`tools/wasm-jit-spike/asyncify/`, under node): it works. A compiled `while` loop sleeping 16 ms a frame from inline C yields to the event loop; +52% module size (2.63 MB gzipped), about 1.6x slower on a tight loop, no change in depth. 6 of the 49 fiber fixtures pass with it; the rest abort in the fiber switch's rewind. Found on the way: recursion depth is about 300, not 1,700 ([report](../reported/wasm-jit-recursion-depth-is-about-300.md)). **JSPI, 2026-10-30:** on a module built with wasm-native `setjmp` (`-fwasm-exceptions -sSUPPORT_LONGJMP=wasm`, no MIR change), `-sJSPI` makes the same blocking loop yield in Chromium 153, main thread and worker, with **no size cost** (the wasm is identical) and the depth doubled (700 in the worker, 1,100 on the main thread); it needs those objects, not today's. Chrome ships JSPI; Firefox has an Intent to Ship (153) and Safari 27 beta has it, per web sources. So the blocking style has two routes: Asyncify (every browser, +52% module, objects compiled without `-fwasm-exceptions`, which the default build no longer produces) or JSPI (Chrome-first, free, the default build's objects) -- two object sets, so one or the other per module. **Decided 2026-10-30: JSPI** (J4); the report has the measurements. | P0 | The whole examples section (8) is written in the blocking style. | wasm-jit-plan P5 (the family is "unowned"); J4 |
| 4 | **Diagnose [wasm-jit-comparator-result-varies-between-runs](../archive/wasm-jit-comparator-result-varies-between-runs.md).** A list comparison gives a different answer on alternate runs of one module instance. **Done 2026-10-29**: not the MIR interpreter. `list-tail` read a `(Cons A)` cell's 4-byte tail and its padding as one `int64_t` (ILP32 shape 4), so what the allocator left in the padding decided the answer; fixed in `stdlib/list.tur`. | P1 | A frame loop is the longest-lived use the engine has had. A nondeterminism in the MIR interpreter's state on wasm is the kind of defect that shows up on frame 4,000, not in a 10-ms corpus run. Find out whether it is a MIR bug or a Turmeric one before building a site on it. | `docs/reported/` |
| 5 | **Close ILP32 shapes 2 and 3** ([ilp32-word-slots-truncate-64-bit-carriers](../reported/ilp32-word-slots-truncate-64-bit-carriers.md)): `tur_hamt_get`'s `void *` values (a double stored in a map comes back denormal) and `stdlib/hamt.tur`'s `tur_hamt_count` declared at the wrong width, plus the `typed/vec-*` fixtures; and shape 4, found 2026-10-29 while doing row 2: `stdlib/list.tur` reads a `(Cons A)` cell as `{int64_t head; int64_t tail}`, but on wasm32 its `tail` is a 4-byte pointer followed by padding, so a nil tail can read as non-nil and the walk reads address 0. Shape 1 (the DK word) is done; shape 3 is done (2026-10-30: `tur_hamt_count`, the `size_t` vector-header views, and `check-extern-c-widths.py` to keep declarations honest -- `vec-eq?` is right on wasm now); shape 2 is done (2026-10-30: the HAMT's keys and values are `int64_t` words end to end, so a map of floats is right on wasm, on both engines); shape 4's stdlib half is fixed (2026-10-29) and its emitter half too (2026-10-30: on a 4-byte-pointer target a parametric ADT's layouts keep 8-byte slots -- narrow members padded to the word, cells zeroed at construction -- so the erased base, each monomorph and a carrier-built rest-list cell agree as they do on LP64; `list-count` of five ints is 5 on the module and `list-of` of heap ADTs counts right too; native emission unchanged). What is left in the report is the untraced Option/Result box line. | P1 for correctness, P3 for the examples | `life.tur` is a `vec` of 0/1 ints, which survive; but any program that keeps a float (a position, a velocity) in a `vec` or a map is silently wrong on the page, and P3 has to audit each example for that shape. The same defect is in `tur build --target wasm`. | the report |
| 6 | **Variadic calls inside a loop** ([c2mir-wasm-vararg-buffer-allocas-per-call](../reported/c2mir-wasm-vararg-buffer-allocas-per-call.md)): c2mir allocas a variadic call's buffer at the call site and keeps it until the function returns, 16 B an iteration. | P3 (examples); know it in P2 | Only a function that never returns accumulates, so the callback style (the frame function returns every frame) and any helper `defn` are safe. The exposure is the blocking style with a `println` written inline in `main`'s own loop: once a frame is 60 a second (a million takes over four hours), but inside an inline per-entity loop of 200 entities it is 12,000 a second and overflows 16 MiB in about 80 seconds, with signs of memory corruption. Fix in the fork, or have the emitter hoist the buffer. | the report |
| 7 | **Tail calls through a frame that allocated** ([wasm-mir-interp-tail-call-keeps-allocas](../reported/wasm-mir-interp-tail-call-keeps-allocas.md)): ~350 B per call, a trap after about 24,000 calls, counting only tail calls from a function with an address-taken or aggregate local. Self tail calls are loops and unaffected. | P3 (examples) | Only a chain that never returns accumulates, so it is the blocking style written as mutual tail recursion *per frame* (`tick` -> `render` -> `tick`, two calls a frame: a trap after about 3 minutes at 60 fps). State changes alone (menu -> play) are too rare to matter, and the callback style returns to the browser every frame. Keep the examples off the shape until it is fixed. | the report |
| 8 | **A main-thread entry for the JIT module.** `jit-worker.js` and `web/jit-client.js` assume a worker; the glue's `main` runs to completion on the calling thread; the module instance is retired on a trap. The iframe needs: instantiate on the main thread, keep the context after `main` returns (J3), and a trap handler that tells a `set_main_loop` unwind from a real trap. **Spiked 2026-10-30 (P0):** the arcade's `screen.js` boots the module on the iframe's main thread with the canvas, awaits the JSPI Promise, and takes Emscripten's `unwind` as "main loop running" -- a `run-main-loop` program's closure is then called every frame by the browser with the MIR context intact, and a stack overflow or a trap is still reported as one. J3 e is done too (2026-10-30, P1): `turi_wasm_jit_set_fallback(0)` makes a refusal the answer (`{"engine":"refused", ...}`, its reason on stderr) instead of a tree-walker run without raylib; the arcade module's smoke test checks it. | P0 (spike), P1 | raylib's GLFW port needs the canvas. | J3 |
| 9 | **Spice roots on the compile path.** `turi_wasm_jit_run` compiled with no include or spice directories. **Done 2026-10-30:** `turi_wasm_add_include_dir(dir)` / `turi_wasm_clear_include_dirs()` (`src/web/wasm_glue.c`, exported from both modules) hold a process-wide list of module search directories -- the `src/` of each spice the page has mounted, e.g. `/spices/raylib/src` -- that every interpreter environment the glue creates borrows and that `turi_wasm_jit_run` hands `compile_to_c`, so `(import raylib/core)` resolves on both engines as it does under `tur run -I`. `tools/wasm-jit/run.js -I DIR` copies a host tree in and adds it; `tests/check-wasm-jit-imports.sh` (in the web CI job) runs a program whose module lives only under `-I` and checks the engine compiled it, inline C included. The page's part -- mounting a spice's package and calling the export -- is J2's, done in P1 along with the `defscript` header resolving against the mounted set. | P1 | `(import raylib/core)` cannot resolve without it; this is also what `defscript` needs. | J2 |
| 10 | **The import table for a non-runtime library**, including struct-by-value prototypes and `raylib.h` in `/sysinclude`. **Done 2026-10-30 (P0, J0/J1):** `build-module.sh` takes `TUR_WASM_JIT_EXTRA_OBJECTS` (objects and archives; every archive member joins the `--wasm-objects` list, so its functions get rows at their wasm signatures and exact-signature stubs -- no header parsing, and struct by value needs nothing special because an aggregate is an i32 address on both sides), `TUR_WASM_JIT_EXTRA_HEADERS` (into `/sysinclude`) and `TUR_WASM_JIT_EXTRA_LINK`. The spike's arcade build is `tools/wasm-jit/arcade/build.sh` since P1. raylib's whole surface is 2,290 typed rows; `DrawText` with a `Color` by value draws; a program compiles with no `-l` marker because there is no `build.tur` on the page, and the library in the module is simply found. | P0 (spike by hand), P1 | The first import set that is not the runtime or libc. | J1 |
| 11 | **`:web :supported` stubs** in turmeric-spices (wasm-spices-plan W0) for the Spices menu, and a decision about assets (shapes and text only, or a small package). | P2 / P3 | The picker reads them. | B0 |
| 12 | **Emscripten pin.** CI's "Set up Emscripten" step pins 6.0.10; wasm-jit-plan's corpus numbers were measured with 6.0.13. **Settled 2026-10-30:** everything current is 6.0.10 -- CI, the arcade repo's `scripts/engine.lock`, and the emsdk the JSPI measurements (prerequisite 3) and the arcade scaffold were built with -- so a P0 number is comparable to CI as it stands. Only the older corpus figures are 6.0.13; a bump to a newer SDK is a separate decision, taken in `ci.yml` and `engine.lock` together. | nothing (comparability only) | wasm-jit-plan risk 6. | `.github/workflows/ci.yml` |
| 13 | **Headless WebGL2 in CI.** Confirm the Playwright Chromium CI uses gives the arcade module a WebGL2 context (software GL is fine); the smoke test and every example's "first frame drawn" check depend on it. **Confirmed 2026-10-30:** Playwright's headless Chromium 153 (Chrome for Testing, as `npx playwright install chromium` provides it) returns a `WebGL 2.0 (OpenGL ES 3.0 Chromium)` context on SwiftShader (ANGLE over Vulkan, `MAX_TEXTURE_SIZE` 8192) with no flags at all, and the same with `--use-gl=angle --use-angle=swiftshader`; WebGL 1 is not offered, which raylib's GLES2 path would want, so the module is built for GLES3/WebGL2. Measured in this container, not on a GitHub runner; the arcade's first-frame test is what confirms it there. | P1 | wasm-spices-plan section 12 measured 60 fps in Chromium, not necessarily headless-on-CI. | P1's smoke test |

What is **not** a prerequisite, so nobody waits on it: the interpreter-side
`ffi_thunk` work (A0-A4, contingent), `tur new-web` (W4), `*args*` for the
JIT module (it already takes them, J5), the build service (cut), and C2 of
[aot-compiled-repl-plan](aot-compiled-repl-plan.md) (the page's REPL session;
the arcade is a program runner, and C1's per-defn images are already there).

Rows 1-3 and 8-10 are P0's own inputs and are the spike's work; rows
2 and 12 are an hour each; rows 4-5 are the real work and are worth doing
whichever architecture wins, because they fix the browser JIT and
`tur build --target wasm` for everyone.

### P0 -- Spike (this repo, 3-5 days)

Hand-link one module -- `turmeric-jit` + the web `libraylib.a` + `rl_web_loop.c`
+ a hand-written import table for the raylib calls the section 1 program makes --
and drive it from a plain main-thread page, then from a sandboxed iframe. (The
first draft said two days; building raylib's web archive, the hand import
table, spice roots in the glue and two links, with and without Asyncify, make
it nearer a week.) Record, in this order (each step gates the next):

1. **Struct by value.** A compiled `draw-circle-v` / `draw-rectangle-rec` / a
   `draw-text` with a `Color` draws what it should. Expected to pass (J1); if
   it does not, the fix is in c2mir's wasm32 target, and J waits on one fork
   PR rather than moving to A.
2. **Callback mode.** The section 1 program written with `with-web-game-loop`
   keeps drawing after `main` has unwound: `requestAnimationFrame` fires, the
   interpreted frame function runs, the context survives, and a `println` per
   frame reaches the console. Also: Stop (iframe teardown) leaves the editor
   page alone.
3. **Blocking mode under JSPI.** The `while (not (window-should-close))`
   form from section 1 on the JSPI module (`TUR_WASM_JIT_JSPI=1`) with raylib
   linked in: does raylib's own `emscripten_sleep` inside
   `WindowShouldClose`/`EndDrawing` suspend and resume, in Chromium, and what
   fps. Prerequisite 3 showed a sleep from compiled inline C does; raylib's is
   the same import called from native code. Run it with and without a
   `call/cc` that also draws. If it does not work, record why (a JS frame
   under the sleep is the likely shape) and J4 becomes "teach the callback
   form".
4. **Cost.** Time-to-first-frame cold and warm; front and c2mir milliseconds
   for the program with the raylib spice's inline C compiled in; the module's
   gzipped size with and without Asyncify and with raylib; `life.tur`'s
   96x54 grid step per frame (section 8.5's budget), as an interpreted loop.
5. **Boot.** The iframe under COOP/COEP; `SharedArrayBuffer`; a per-spice
   package mounted after startup (5.3); a trap and a `set_main_loop` unwind
   told apart by the page.

The spike's output is a short results section in this plan, as wasm-jit-plan
did for P1, and a go / no-go on J.

#### P0 results (2026-10-30, day one): **go on J**

Measured on the arcade module -- the spike's `arcade/build.sh`, since P1 `tools/wasm-jit/arcade/build.sh`: the
JSPI JIT build with raylib 5.5's web archive (`emcmake`, `PLATFORM=Web`,
`GRAPHICS_API_OPENGL_ES3`, compiled `-pthread -fwasm-exceptions` to match the
module), `rl_web_loop.c`, `raylib.h` in `/sysinclude`, and the raylib and math
spices' `src/` embedded at `/spices/<name>/src` -- driven by the arcade's
screen pane (`turmeric-arcade` `web/screen.js`, `web/tests/raylib.spec.js`)
in Playwright's headless Chromium 153 on SwiftShader, in this container.

1. **Struct by value: yes.** The quick-start program (wrapped in a
   `defmodule`; a top-level `import` is "only allowed inside defmodule", so
   section 1's bare snippet is not a program as written) draws
   "Hello, Turmeric!" on a RAYWHITE 480x270 frame: `DrawText` with a `Color`
   by value and `ClearBackground` through c2mir's wasm32 ABI into the
   archive, read back from the frame's pixels. No fork change was needed:
   the table rows and stubs come from the archive's members at their wasm
   signatures (row 10), 2,290 of them.
2. **Callback mode: yes.** `run-main-loop` with a closure: `main` unwinds
   (the shell reports `compiled -- main loop running`, the statement after
   the call never runs), the browser calls the interpreted closure every
   frame, a `println` every 30th frame reaches the console (240 frames in
   about 5 s, headless), the MIR context survives `main`. Stop removes the
   iframe and the editor page is untouched.
3. **Blocking mode under JSPI: yes.** `(while (not (window-should-close))
   ...)` runs unchanged: raylib's own `emscripten_sleep` inside its frame
   suspends the wasm stack and the loop resumes next tick -- 60 empty frames
   in 0.80 s headless (about 75 a second; raylib sleeps 16 ms a frame when
   it has nothing to wait for, so this is the sleep's granularity, not a
   ceiling), the console lines streaming meanwhile. Not yet run with a
   `call/cc` that also draws.
4. **Cost.** Module: 6.98 MB wasm, 2.37 MB gzipped (the plain JSPI module is
   5.54 / 1.89 MB; raylib adds 0.48 MB gzipped), plus the 4.5 MB
   `stdlib.data` both share. A Run of a 30-frame raylib program, cold
   iframe, from the click to raylib's "device initialized" line: 920 ms,
   of which the engine boot is about 290 ms; the engine line says
   `front 164 ms, c2mir 211 ms, run 46 ms` -- c2mir is up from 110-170 ms
   because the raylib spice's inline C compiles with the program. Warm is
   the same shape: every Run is a fresh iframe and a fresh instance (the
   browser caches the fetch). Not measured yet: `life.tur`'s grid step as
   an interpreted loop (section 8.5's budget).
5. **Boot.** The iframe under COOP/COEP with `SharedArrayBuffer` boots and
   raylib's GLFW port finds the canvas handed to the factory; a trap, a
   stack overflow and the `set_main_loop` unwind are told apart by the
   shell. Not done: a per-spice package mounted after startup (the spike
   embeds the two spices in the module; packages are J2/P1), audio and
   its click-to-start overlay (6.6), the glue's tree-walker fallback is
   still on (J3 e), and no browser without JSPI has been tried.

What P1 inherits: the `tur_wasm_arcade` target is the spike's build script
made a CMake target and a CI job (raylib's archive cached by ref), the
fallback switch (J3 e), spice packages instead of `--embed-file` (J2),
`life.tur`'s number, and the examples moved from `tests/raylib.spec.js`
into `examples/`.

### P1 -- The arcade module (this repo)

J0-J5, a `tur_wasm_arcade` target, a CI job that builds it (the existing
`Set up Emscripten` step in `ci.yml` plus the raylib web archive, cached by
raylib ref), and a smoke test through Playwright's Chromium: first frame,
console line, Stop. If P0 said no-go, this phase is A1-A4 instead.

**Done (2026-10-30):**

- `tools/wasm-jit/arcade/build-raylib.sh` builds raylib's web archive (the
  spice's `:wasm-options`, the module's `-pthread -fwasm-exceptions`; 9 s),
  and `tools/wasm-jit/arcade/build.sh` -- the spike's script, promoted --
  builds the module, calling `build-module.sh` with `TUR_WASM_JIT_JSPI=1` and
  the extras directly rather than through the JSPI spike.
- `tur_wasm_arcade` (`-DTUR_WASM_ARCADE=ON`, off by default;
  `TUR_ARCADE_RAYLIB_SRC`, `TUR_ARCADE_SPICES`, optional
  `TUR_ARCADE_RAYLIB_LIB`) builds it into `build-wasm/wasm-arcade/wasm-jit/`,
  never `web/public`. 45 s from a configured tree in this container.
- J3 e: `turi_wasm_jit_set_fallback(int)`, exported from both JIT modules.
  Off, a refusal answers `{"engine":"refused","exit":1,"reason":...}`; on
  (the default, Try's Compile engine) nothing changes.
- The `wasm-arcade` CI job: raylib at the spice's ref, its archive cached by
  ref, SDK and script hash, the module built, then
  `tools/wasm-jit/arcade/smoke/smoke.mjs` in Playwright's Chromium on a
  COOP/COEP page: the module boots in an iframe with JSPI and the two spices
  mounted (367 ms), the quick-start program's first frame is RAYWHITE with
  its text drawn (1.5 s from Run), a line printed on frame 30 arrives while
  the loop runs, Stop removes the iframe and the page stays live, and a
  `pthread_create` program comes back `refused` without `main` running.

**Then (2026-10-30):**

- turmeric-arcade builds this target in its CI (`scripts/fetch-engine.sh`,
  with `SPICES_REF` and `RAYLIB_REF` in `engine.lock`), so its raylib tests
  run there, and its shell turns the fallback off.
- J2: `tools/wasm-jit/arcade/pack-spices.py` writes one package per spice
  beside the module (`spices/<name>.json`, `spices/index.json` with each
  spice's non-optional `:spices` deps; a list not closed under them is
  refused), and `spices.js` is the loader a page takes with the module:
  `spicesFor(src, index)` reads the names a program asks for,
  `mountSpices` writes the closure under `/spices/<name>/src` and adds each
  to the search path, after start-up. The module embeds no spice (80 KB
  smaller). `turi_wasm_jit_run` satisfies a `defscript :spices` header from
  the mounted set: it compiles as under `--enable=script-deps`, and a name
  not mounted is `spice '<name>' is not in this build`, with nothing fetched.
  The smoke test checks both, and a header program copied out of the page is
  the same text `tur --enable=script-deps` runs.
- `life.tur`'s step, measured (8.5): 64x36, not 96x54.
- `build-raylib.sh` starts its CMake build over when pointed at another
  raylib checkout, rather than failing on CMake's source mismatch.

**And last (2026-10-30, turmeric-arcade):** its `engine.lock` pins the J2
merge, its screen mounts the program's spices through the engine's
`spices.js`, and its Examples menu is an `examples/` tree (one program a
file, titled by its `;;;` header) that `web/tests/raylib.spec.js` runs from
the same files -- the quick start, the frame counter and the
`run-main-loop` callback. P3 writes the section 8 set into that tree.

### P2 -- The page (arcade repo)

Scaffold from Try's `web/`: Vite, Monaco, the LSP worker, `share-codec.js`,
`csp.js`, the theme, the jit-client's protocol moved into the iframe shell.
Build the screen pane to the section 5.2 contract. Spices menu (packages from
5.3), examples menu, share.

**Already there from P0/P1 (turmeric-arcade, 2026-10-30):** Vite, the one
CSP, the Worker with COOP/COEP, the screen pane's iframe shell and its
postMessage contract (`web/screen.js`), streamed console lines, Stop, the
refusal and stack-overflow reports, an Examples menu read from `examples/`,
and spices mounted from packages. **The engine side of the LSP
(2026-10-30, P2):** `wasm_lsp.c` resolves imports through the page's include
dirs and reads a `defscript` header as the Compile engine does, both modules
export `turi_wasm_put_spice_file` (a spice source under `/spices/<name>/src/`
and nothing else), and `tur_wasm_arcade` ships `tools/wasm-jit/arcade/lsp-worker.js`
with the stock module beside the engine; the smoke test checks the quick
start analyses clean and an unknown import is flagged. **Left for P2:**
Monaco and the arcade's use of that worker in place of the textarea, the
Spices menu (it reads
`engine/spices/index.json` and edits a `defscript` header in place), share
links (`share-codec.js`), the theme, and the no-JSPI path (the callback
examples only, with a note). Deploy to its own Worker with COOP/COEP headers (the
module is `-pthread`; keep that, the host serves the headers already).
Playwright: the Try suite's shape (`web/tests/`) trimmed to the arcade's
features. Prerequisites 6-7 should be closed or noted on the page first.

### P3 -- Examples and docs

The section 8 set, each verified natively and in the page. A short guide in
the arcade repo's README; one paragraph and a link from this repo's
`web-emscripten-tutorial.md` and the raylib spice README ("try it in the
browser"). Prerequisite 5 must be closed, or each example audited for floats kept in a `vec` or map.

### P4 -- Hardening

Mobile layout; a `?example=` deep link; the FPS counter (and, if J4 went the
Asyncify way, its note); telemetry-free usage counts if the owner wants them
(the CSP forbids third party scripts, keep it that way). If the MIR
interpreter's speed is what limits the examples, the lever is wasm-jit-plan
section 8's T2 (a MIR-to-wasm generator), not a server.

## 8. Examples

Six, chosen so each shows one more thing. All are written in the **blocking
style** the native raylib README uses, because it is the form a visitor pastes
from the README and the form a native twin runs. **The blocking style runs
under JSPI** (decided 2026-10-30; P0 step 3 confirms raylib's own sleep path):
in a browser with JSPI these are shown as written; in one without, the page
says so and offers the callback twin. If P0 finds raylib's path does not
suspend, each example is shipped in the callback form of 8.2 and the
blocking text appears only in the example's header comment as "the native
version". A `with-web-game-loop` variant of the first one is included either
way, to show the callback style a shipping game uses.

Key codes are raylib's: Left 263, Right 262, Up 265, Down 264, Space 32,
W 87, S 83, R 82. The examples define them as `def`s rather than magic
numbers; a `raylib/keys` module of named constants in the spice would be a
welcome small follow-up and is not required by this plan.

### 8.1 `bouncing-ball.tur` -- the hello world

Motion, `dt`, keyboard, text. State in `^mut` `let` bindings with `set!` (a
plain binding is immutable: `set!` on one is a compile error).

```turmeric
;;; bouncing-ball -- a ball bounces; arrows nudge it; R resets.
(defscript :spices #map{"raylib" #map{:url "https://github.com/turmeric-lang/turmeric-spices"
                                      :ref "raylib-v0.3.0" :subdir "spices/raylib"}})

(import raylib/core   :refer [init-window window-should-close begin-drawing
                              end-drawing clear-background get-frame-time])
(import raylib/shapes :refer [draw-circle])
(import raylib/text   :refer [draw-text])
(import raylib/input  :refer [is-key-down is-key-pressed])
(import raylib/color  :refer [raywhite black orange])

(def key-left 263) (def key-right 262) (def key-up 265) (def key-down 264) (def key-r 82)
(def w 480) (def h 270) (def r 12.0)

(init-window w h "bouncing ball")
(let [^mut x  240.0
      ^mut y  100.0
      ^mut vx 140.0
      ^mut vy 90.0]
  (while (not (window-should-close))
    (let [dt (get-frame-time)]
      (when (is-key-down key-left)  (set! vx (- vx (* 300.0 dt))))
      (when (is-key-down key-right) (set! vx (+ vx (* 300.0 dt))))
      (when (is-key-down key-up)    (set! vy (- vy (* 300.0 dt))))
      (when (is-key-down key-down)  (set! vy (+ vy (* 300.0 dt))))
      (when (is-key-pressed key-r)  (do (set! x 240.0) (set! y 100.0) (set! vx 140.0) (set! vy 90.0)))
      (set! x (+ x (* vx dt)))
      (set! y (+ y (* vy dt)))
      (when (or (< x r) (> x (- 480.0 r))) (set! vx (- 0.0 vx)))
      (when (or (< y r) (> y (- 270.0 r))) (set! vy (- 0.0 vy)))
      (begin-drawing)
      (clear-background (raywhite))
      (draw-circle (float->int x) (float->int y) r (orange))
      (draw-text "arrows: push   R: reset" 10 10 14 (black))
      (end-drawing))))
```

(`float->int` is whichever stdlib coercion the examples settle on; P3
verifies each example against the real stdlib names and the float rule in
CLAUDE.md -- a probe literal with a fractional part, never `7`.)

### 8.2 `bouncing-ball-callback.tur` -- the same, as a frame callback

The `raylib/web` form: `with-web-game-loop` is arity-compatible with
`ecs-raylib/loop`'s `with-game-loop`, so the body is identical and the
state moves into a closure the browser can call after `main` has returned.
This is the example a visitor copies when they move from "demo" to "game".

```turmeric
(import raylib/web   :refer [with-web-game-loop])
;; ... same imports as 8.1 ...
(let [^mut x  240.0
      ^mut y  100.0
      ^mut vx 140.0
      ^mut vy 90.0]
  (with-web-game-loop 0 "bouncing ball" 480 270 60
    (do
      ;; same body as 8.1, dt is bound by the macro
      )))
```

### 8.3 `pong.tur` -- input, collision, score

Two paddles (W/S and Up/Down), a ball, a score line, `measure-text` to
centre it, a serve on Space. ~70 lines. Shows `is-key-down` polling for
held keys against `is-key-pressed` for edges, and `draw-rectangle`.

### 8.4 `raygui-tuner.tur` -- a second spice

The raygui `hello-gui` example reshaped: a slider for speed, a checkbox for
trails, a colour picker for the ball, driving the 8.1 ball. Shows two spices
composed through `defscript`, raygui's `Rect` by-value record crossing the
interpreter boundary (A1's record path), and the "panel on the left, canvas
on the right" layout at 480x270.

```turmeric
(defscript :spices #map{"raylib" #map{...} "raygui" #map{... :subdir "spices/raygui"}})
(import raygui/core     :refer [gui-load-style-default])
(import raygui/controls :refer [gui-slider gui-check-box gui-color-picker])
(import raygui/rect     :refer [rect])
;; (set! speed (gui-slider (rect 10.0 30.0 120.0 16.0) "0" "400" speed 0.0 400.0))
;; (set! trails (gui-check-box (rect 10.0 56.0 16.0 16.0) "trails" trails))
```

### 8.5 `life.tur` -- data structures at frame rate

Conway's Life on a 96x54 grid (one cell per 5x5 pixels at 480x270), a
`vec` of ints double-buffered, click to toggle a cell, Space to pause, R to
randomise. Shows stdlib `vec` and `get-mouse-position` (a `Vector2` handle),
and is the example that shows the engine's speed honestly: a 5 184
cell step per frame is the budget J has to meet at 30 fps (the MIR
interpreter on wasm32, measured about 7x slower than `clang -O0` on a tight
loop; under A it would be the tree-walker, ~20x slower again). If it cannot,
the example drops to 64x36 and says why in its header comment. A cell grid of
0/1 ints is unaffected by the ILP32 carrier defect (prerequisite 5); a grid
of floats would not be.

**Measured (2026-10-30, P1):** a step written plainly -- `vec-get` through a
wrapping `cell` helper, eight calls a cell, `vec-set!` into the other buffer
-- on the browser JIT module under node 22 (the same MIR interpreter as the
page; JSPI changes neither the wasm nor its speed), warm, on an idle 4-core
container: **96x54 is 32-39 ms a step, 64x36 is 17-21 ms**, against 33 ms
a frame at 30 fps. 96x54 spends the whole frame on the step before drawing
anything, so the example is 64x36, which leaves about half the frame for
2,304 `draw-rectangle` calls. The live counts match the native run's
(96x54 from one seed: 1597, then 1860 after one step, 646 after 60). A
step with the wrap hoisted out of the inner loop would buy some of it back;
the example should say which it does.

### 8.6 `ecs-circles.tur` -- a third spice, with macros

`ecs` + `ecs-raylib`: `Pos2`/`Vel2`/`Radius`/`Tint` components, the
`integrate-2d` and `render-circles` system macros, 200 entities bouncing in
the pane, a key to spawn more. Shows macro-defined systems elaborating
against the spices' sources (under J they are compiled with the program), and
`with-game-loop` -- the blocking form, so it follows P0 step 3.

### Later

- `inline-c-plasma.tur`: a plasma effect whose inner loop is an inline-C
  body writing an `Image` through `image-set!`, uploaded as a texture each
  frame. **In scope under J** (inline C compiles on the page); refused under A.
  It is the showcase for "this is compiled in your browser", and the first
  example to run into the MIR interpreter's speed, since its loop is
  per-pixel.
- `sdf-sphere.tur`: `sdf-raylib`'s `gpu-preview` example, if its shader
  path survives WebGL2 (wasm-spices-plan 9.2 lists it as unaudited).

## 9. Risks and open questions

1. **The engine's frame budget.** The MIR interpreter on wasm32 is fast enough
   for a few hundred shapes a frame and for a 5,184-cell grid, probably; a
   per-pixel loop is the edge (the plasma example). P0 measures 8.5's grid step
   and the plasma inner loop. The lever past it is wasm-jit-plan section 8's
   T2, a MIR-to-wasm generator; the build service is not the lever any more.
2. **JSPI and the MIR interpreter together.** Prerequisite 3 showed a compiled
   sleep suspends and resumes through `eval`'s frames in Chromium, main thread
   and worker, with no size cost. What is left is raylib's own sleep path (P0
   step 3) and the browsers: JSPI is Chrome today, Firefox (Intent to Ship,
   153) and Safari (27 beta) later, so until they ship the blocking examples
   are Chrome-only and the callback examples carry the other browsers. If
   raylib's path fails, the playground is callback-first, which changes the
   examples and the pitch ("paste the README quick start") but not the
   architecture.
3. **Struct-by-value calls from MIR on wasm32.** raylib passes `Color`,
   `Vector2`, `Rectangle`, `Texture2D` and `Camera3D` by value. Both c2mir's
   wasm32 ABI and `mir-wasm.c` pass aggregates by address, as clang does, so this
   is expected to work; the known gap is a single-element struct (J1). P0 step 1
   confirms. A failure is a c2mir wasm32 fix in the fork (one PR at a time, per
   CLAUDE.md), not a reason to move to A.
4. **A callback that outlives `main`.** `emscripten_set_main_loop(..., 1)`
   unwinds out of `main`, which is running inside MIR's `eval` frames, and the
   browser then calls an interpreted function whose context must still be
   alive. The corpus has two `unwind` traps; nobody has run this on purpose.
   P0 step 2.
5. **Module size.** `turmeric-jit.wasm` is 5.27 MB (1.84 MB gzipped); raylib
   adds about 4.5 MB raw; Asyncify instrumentation adds code size and slows the
   interpreter. Spice sources are data and do not add to the module. If the
   total is bad, the arcade module can drop the exports the arcade does not use
   (`-sEXPORTED_FUNCTIONS` in `src/CMakeLists.txt` lists the trace, SMT and
   tutorial ones), and the stdlib package is already shared (wasm-jit-plan
   5.11).
6. **Recursion depth.** J trades the tree-walker's heap-bounded depth for
   about 1,100 Turmeric calls on the iframe's main thread (700 in a worker),
   the module having linked with wasm-native `setjmp` since 2026-10-30
   ([wasm-jit-recursion-depth-is-about-300](../reported/wasm-jit-recursion-depth-is-about-300.md);
   the 1,700 quoted before was a C spike measured before P5). A game's frame
   loop is not deep; a user's recursive helper over a list of 400 is. The page
   reports `stack overflow: recursion too deep` and retires the instance, as
   Try's Compile engine does.
7. **Keyboard focus between two documents.** The parent's shortcuts (Run,
   the command palette) must still fire when the iframe has focus; the iframe
   forwards `Ctrl/Cmd+Enter` and `Escape` to the parent by `postMessage`.
8. **The examples' native twin.** CI running raylib natively needs a display
   (`xvfb-run` on Linux) and an exit after N frames; the `--frames` argument
   through `*args*` is a small convention the examples share, not a raylib
   feature. The JIT glue already carries arguments (J5).
9. **The fork.** Any c2mir or MIR change this needs (an aggregate-passing fix,
   an interpreter re-entry without a JS frame) is a PR to turmeric-lang/mir,
   of which CLAUDE.md allows one open at a time. Batch them behind P0's
   findings.
10. **Name and domain** were the owner's call, made 2026-10-30:
    `turmeric-lang/turmeric-arcade` at `play.turmeric-lang.com`.

## 10. Relationship to existing plans

- **wasm-jit-plan.** This plan is its first consumer that is not Try
  Turmeric: it takes the engine, the import table, the shared package and the
  engine line as built, and adds raylib to the link. Its section 5.10 already
  says the playground's Build button becomes this engine; section 3 above
  records that and reorders the architectures accordingly. The reported defects
  it leaves open (ILP32 shapes 2-3, the comparator, the vararg alloca, the
  tail-call alloca, the unchecked linear stack) are this plan's prerequisites,
  section 7.
- **wasm-spices-plan.** This plan consumes its raylib arm (done) and W0's
  `:web :supported` stubs (not done); it does not need W1's HTML shell (the
  iframe is the shell). W1's `*args*` hook is needed only by A; J has its own.
  W4's `tur new-web` is unaffected. If the arcade lands first, W6's
  web-games guide gains a "try it in the browser" link.
- **script-spice-deps-plan.** `defscript :spices` is the portability story
  for every example here. On the page the header is satisfied from mounted
  spice packages and never fetched (5.3); SD6's compiled-script cache has no
  role now that the build service is cut.
- **aot-compiled-repl-plan.** The same user wish -- compiled semantics at an
  interactive prompt -- and the same engine: C1's per-defn images run on the
  page already (wasm-jit-plan P6). C2 is not a prerequisite here.
- **godot-binding-web-plan.** Shares the Emscripten profile concerns
  (`MODULARIZE`, `EXPORT_NAME`); the iframe-per-run contract in 5.2 is one
  that plan could reuse for a Godot web export preview.
