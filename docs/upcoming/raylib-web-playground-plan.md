# Plan: a raylib playground in the browser ("Try Turmeric with a screen")

> **Status:** Draft, 2026-10-23. Nothing built. The one decision this plan
> needs from its owner before P0 starts is in
> [section 3](#3-the-central-decision-how-does-typed-code-reach-raylib)
> (interpreter-with-baked-spices first, build service second -- recommended)
> and [section 4](#4-where-it-lives) (its own repo -- recommended).
> **Type:** Web platform / interpreter (`src/turi/`, `src/web/`) / build
> driver (`src/driver/cmd_build.c`) / a new site.
> **Builds on:** [wasm-spices-plan](wasm-spices-plan.md) (the raylib web arm,
> `tur build --target wasm`, the `:web` manifest block, the measured
> Asyncify cost), [script-spice-deps-plan](script-spice-deps-plan.md)
> (`defscript :spices`, landed 2026-10-22 behind `--enable=script-deps`),
> [spice-repl-plan](../archive/spice-repl-plan.md) (compiled spice exports
> bound as interpreter natives through `src/turi/ffi_thunk.c`),
> [extras-repo-split-plan](extras-repo-split-plan.md) (the pattern for a
> sibling repo that pins a `tur` release).
> **Not this plan:** a general web-app scaffold (`tur new-web`), which stays
> in wasm-spices-plan W4; any change to the raylib spice's native arm.

## 1. Goal

A single-page app, in the spirit of `try.turmeric-lang.com`, where a visitor
edits a raylib program on the left and sees it **run** in a small screen on
the right, with the program's `println` output in a console under the
screen. Examples are one click away. A program may use raylib, raygui, and
other spices that work in a browser. Run is near-instant; nothing is
installed.

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

Checked against `main` (v0.70.0) and turmeric-spices on 2026-10-23.

| Piece | Where | What it gives this plan |
|---|---|---|
| Try Turmeric | `web/try/`, `web/main.js` (6.8k lines), `web/public/eval-worker.js` | An editor-plus-console SPA with Monaco, LSP in a worker, share URLs (`share-codec.js`), an examples menu, a CSP (`csp.js`), a service worker, Playwright tests, a Cloudflare Worker deploy. **It runs the tree-walking interpreter** (`libturi_wasm`, `src/web/wasm_glue.c`), not compiled code: `turi_wasm_eval` in a Web Worker. |
| raylib's web arm | turmeric-spices `spices/raylib/build.tur` (`:wasm-options` `PLATFORM=Web`, `GRAPHICS=ES3`), `raylib/web.tur` (`run-main-loop`, `cancel-main-loop`, `with-web-game-loop`), `c/raylib/rl_web_loop.c` | raylib 5.5 builds under `emcmake` and renders at 60 fps in Chromium (wasm-spices-plan section 12). The frame-callback shim exists. |
| `tur build --target wasm` | `src/driver/cmd_build.c`, `pkg_web_compose_link_flags` in `src/compiler/pkg.c` | Compiles a project with `emcc`, composes the GL/audio/asyncify/heap link line from the `:web` block. Produces `.js` + `.wasm`; **no HTML shell**, no `--preload-file`, no `*args*` (wasm-spices-plan W1 is not built). |
| Spice exports as interpreter natives | `src/turi/spice_loader.c` (build + dlopen + manifest), `src/turi/ffi_thunk.c` (`tur_ffi_install_spice_bindings`, by-value records, F5 callbacks from C back into Turmeric), `turi_env_attach_spice` (`src/turi/eval.h:700`) | The REPL already binds a compiled spice's exported defns as natives, marshals int/float/cstr/bool/nil and records, and lets C call a Turmeric closure. Every raylib handle type is `defopaque ... :int`, so raylib's whole surface is int-class. **Natively this goes through `dlopen`**; there is no static-image constructor yet. |
| Interpreter under Asyncify | `src/turi/env.h:33` (Emscripten fibers for continuations, "requires `-sASYNCIFY=1`") | The interpreter already has an Asyncify-aware build shape, so linking the playground module with `-sASYNCIFY` is not new ground. Whether `tur_wasm` is linked with it today: **verify in P0** (`src/CMakeLists.txt` names no `ASYNCIFY`). |
| `defscript :spices` | `script-spice-deps-plan`, `--enable=script-deps` | A single file can declare the spices it needs. This is the syntax a playground program should use to say what it pulls in, whichever architecture serves it. |
| Spice compatibility matrix | wasm-spices-plan section 9 | Which spices can be in the browser at all. Pure and inline-C spices work; cmake deps that are portable C work; anything socket-shaped does not. |

Two facts from that table shape everything below:

1. **Try Turmeric interprets.** There is no C compiler in the browser and
   MIR does not target wasm, so a program typed into the page cannot be
   compiled there. Either the interpreter calls into a raylib that was linked
   into the page's module ahead of time, or a server compiles the program.
2. **The browser calls the program, not the reverse.** A raylib frame loop
   on the page's main thread must yield every frame. Compiled programs do
   that with `run-main-loop` or Asyncify. An interpreted blocking `while`
   loop needs Asyncify too, which the interpreter build already assumes.

## 3. The central decision: how does typed code reach raylib?

Three architectures. The recommendation is **A now, B as the second
phase, designed so the screen pane cannot tell them apart.**

### A. Interpreter + a baked spice image (no server)

One Emscripten module -- call it the **arcade module** -- links
`libturi_wasm` with the web build of `libraylib.a`, raygui's one TU, and the
AOT-compiled exports of a curated spice set (raylib, raygui, ecs,
ecs-raylib, math, linalg, json, regex, plutovg, png, ...: the "Works" rows of
the matrix). At startup the module installs those exports as interpreter
natives through the existing `ffi_thunk` shim, and embeds the spices'
`.tur` sources next to the stdlib so `(import raylib/core ...)` elaborates
with real types. The page then evaluates the user's program with
`turi_wasm_eval`, exactly as Try Turmeric does.

- The module runs on the **main thread of a sandboxed iframe** (the screen
  pane), not in a Web Worker: raylib's GLFW port needs the canvas and DOM
  input events, which a worker does not have. The iframe gives the Stop
  button for free (tear the iframe down), isolates a runaway program from the
  editor page, and keeps arrow keys from scrolling the editor.
- Linked with `-sASYNCIFY`. raylib's web `WindowShouldClose` yields to the
  browser through `emscripten_sleep`, so **the blocking `while` style runs
  unchanged**, at the measured cost of a fixed ~16 ms added per frame
  (wasm-spices-plan section 12, finding 3). For a playground that is fine:
  a 30-60 fps demo, not a shipping game. `run-main-loop` with an
  interpreter closure also works, through F5 callbacks.
- "Pulling in other spices" means: **the spices baked into the module**,
  chosen from a chip row in the UI or named in a `defscript :spices` header
  (which the page reads and the module satisfies from its baked set, or
  refuses by name). Adding a spice is a rebuild and redeploy of the module,
  not a runtime fetch.
- Cost: inline-C in user code is still refused (as on Try Turmeric today);
  interpreted semantics, not compiled; one large module (libturi is already
  what Try loads, plus ~4.5 MB of raylib, plus the spices).

### B. Build service (compile on a server)

The page POSTs the program; a service runs `tur build --target wasm` with
`--enable=script-deps` and warm caches (emsdk, the raylib web archive, each
spice's objects), and returns `.js` + `.wasm` (+ `.data`). The iframe loads
that bundle.

- Any wasm-clean spice, pulled on demand by `defscript :spices`; inline-C in
  user code; real compiled semantics and full-speed `:callback` mode.
- Cost: a server that can run `emcc`. The site is on Cloudflare Workers,
  which cannot; the service needs a container (Cloudflare Containers,
  Fly.io, or a small VM). A cold compile is seconds, not milliseconds;
  compiling arbitrary C is an abuse surface (sandbox, no network, CPU and
  wall-clock limits, rate limits, content hash cache). The turmeric repo's
  deploy must not depend on it being up.

### B'. The JIT engine on the page (amendment, 2026-10-23)

[wasm-jit-plan](wasm-jit-plan.md) would give B's two wins -- inline C and
any inline-C spice compiled from source -- **without the server**: c2mir and
the MIR interpreter run in the browser (`turmeric-lang/c2mp` already does
this on the same MIR fork this repo vendors). If that plan lands, the Build
button runs client-side and B's service is not built. raylib itself is a
cmake dep either way, so A's baked module stays the first step; the two
compose rather than compete.

### C. Both, behind one screen pane

The screen pane's contract is "load an Emscripten `MODULARIZE`d bundle into
the iframe, hand it a canvas, forward stdout/stderr to the console, report
exit." A loads the baked arcade module and evals; B loads a per-program
bundle. The editor, examples, share URLs, LSP and console are identical.

### Decision

**A first.** It reuses the most (Try Turmeric's whole page, the interpreter,
`ffi_thunk`, the raylib web arm), needs no server, and gets a visitor from
"paste" to "pixels" in under a second. **B second**, as the "Build" button
beside "Run": it is what makes "any spice" and inline-C true, and it is
where the examples that outgrow the interpreter go. C's iframe contract is
written in P2 so B drops in without touching the page.

The honest limits of A go on the page: a small "Interpreted; inline C is not
available; blocking loops cost ~16 ms a frame" note next to the Run button,
the way Try Turmeric notes the eval watchdog.

## 4. Where it lives

Three candidates were asked about. Recommendation: **its own repository**.

| Option | For | Against |
|---|---|---|
| **Own repo, `turmeric-lang/turmeric-arcade`** (working name; the site would be `arcade.turmeric-lang.com` or `play.`) -- recommended | Its build needs emsdk, raylib's web archive and a pinned turmeric-spices checkout; none of that belongs in the compiler repo's deploy. Own release clock (a new example or spice should not wait on a compiler tag). The extras-repo-split pattern exists: pin a `tur` release (`scripts/install-tur.sh` in turmeric-spices is the template), pin a spices ref, build the module in its own CI. The editor-palette contract (`web/editor-palette.json` + `tests/check-editor-palette.py`) already shows how a sibling repo stays in step with this one. | Duplicates the parts of `web/main.js` it needs (eval-worker pattern, `share-codec.js`, `csp.js`, examples menu, theme). The LSP worker needs the spice sources embedded too, so it builds its own `lsp-worker` module rather than reusing Try's. Mitigation: copy, do not abstract; the arcade page is a fraction of `main.js`. |
| A fifth page in `web/` (`web/arcade/index.html`, next to `try/`, `tour/`, `trowel/`, `ci/` in `vite.config.js`) | Maximum reuse of `main.js`; one deploy; the LSP worker could be one module for both pages. | `tur run wasm` and `deploy-web` would need emsdk's raylib build and a spices checkout; the compiler's release-cut workflow would depend on turmeric-spices being green. That coupling is the thing extras-repo-split was undoing. |
| A spice | -- | A spice is a library `tur` builds; this is a site. The closest fit, a `:bin` that serves the page, still needs the web build pipeline, which spices do not have. The *examples* can be a spice-shaped directory (plain `.tur` files with `defscript` headers), but the app is not one. |

What lands **in this repo** regardless: the compiler- and interpreter-side
plumbing in section 6, because the arcade module is built by `tur` and the
page talks to `libturi_wasm`.

What lands **in turmeric-spices**: nothing new is required for A beyond the
`:web #map{:supported ...}` stubs wasm-spices-plan W0 already calls for,
which the arcade's spice picker reads to decide what is offered.

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
  boots the arcade module and evaluates the program it receives by
  `postMessage`. Stop removes the iframe. A second Run replaces it. No module
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
  deltas in the shell, so a visitor sees the Asyncify cost rather than
  wondering.
- **Pop out** opens the same `screen.html?share=...` in a new tab at native
  size; **fullscreen** uses the Fullscreen API on the iframe (gesture
  required, so it is a button). Neither resizes the canvas (6.4 deferred
  resize); they scale.
- **Watchdog.** A program that never yields (no raylib call in its loop)
  freezes only the iframe; the parent's Stop still works because it is a
  different document. A 30 s "still running, no frame drawn" prompt offers
  Stop.

### 5.3 Spices

- A **Spices** menu lists what the arcade module carries, grouped by the
  matrix tiers, each with its one-line description from `build.tur`. Ticking
  one inserts or updates a `defscript :spices` header at the top of the
  buffer (the same way the `#lang` picker edits the `#lang` line in place:
  the buffer is the source of truth).
- The header is the **real** `defscript` form, so a program copied out of the
  page runs natively with `tur --enable=script-deps file.tur` and fetches
  the same spices. Under A the page satisfies the header from the baked set
  and refuses an unknown name with "not in this build; try Build" (which is B).
- Imports work without the header too, for the baked set: a visitor who
  pastes the raylib README's quick start should not have to know about
  `defscript`. The header exists for portability and for B.

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
variant whose module embeds the baked spices' `src/` trees, so completion
and hover know `draw-circle`. Diagnostics come from the same elaboration the
eval uses, so a program the LSP accepts is one the module runs.

## 6. Compiler- and interpreter-side work (this repo)

Ordered by dependency. Items A0-A3 are what A needs; B0-B2 are what B needs.

- **A0. Verify the thunk dispatcher under emcc.** `src/runtime/ffi_dispatch_thunk.c`
  is shape-keyed C with no MIR dependency on paper; `extern_thunk_native`
  gates on `tur_jit_ffi_provider()`, which the spice-image path
  (`ffi_native_shim`) does not. Build `libturi_wasm` with the dispatcher and
  call one int-class export. Also confirm whether `tur_wasm` is linked with
  `-sASYNCIFY` today, and if not, what the interpreter's fiber shim costs
  when it is (binary size: wasm-spices-plan measured +21.7 % on a tiny
  program; libturi is not tiny).
- **A1. A static spice image.** Today a `TurSpiceImage` is built by
  `tur build --shared`, `dlopen`ed, and its exports manifest read from disk
  (`spice_loader.c`). Add the static twin: `tur build --target wasm --image
  <spice>` emits the spice's object code plus a generated
  `<spice>_image.c` holding the manifest text and a `{ name, fnptr }` table,
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
- **B0. The HTML shell is not needed by B** -- the iframe is the shell -- but
  W1's `--preload-file` asset packaging and the `*args*` hook are, and W0's
  `:web :supported false` hard error is what turns "this spice cannot be in a
  browser" into a message with the spice's name.
- **B1. `tur build --target wasm` on a `defscript` file.** Today
  `--enable=script-deps` is wired through `tur run` / `tur <file>`; the
  build service calls `tur build --target wasm --enable=script-deps
  program.tur -o out/program.js` and needs the script-dep cache honoured on
  that path. SD6 (cache the compiled script) becomes the service's
  content-hash cache.
- **B2. A build-service reference implementation**, in the arcade repo: one
  container image (emsdk, a pinned `tur`, the raylib web archive and each
  baked spice's objects prebuilt into the image), a 2-endpoint HTTP service
  (`POST /build` -> bundle URL; `GET /bundle/<hash>/*`), per-request
  `timeout`, no outbound network, a seccomp or gVisor sandbox, and a
  size/rate limit. Deployed as a Cloudflare Container bound to the arcade
  Worker, or on Fly.io if Containers' limits bite. The page shows "Build"
  only when `/build` answers a health probe, so the site degrades to A
  alone with the service down.

## 7. Phases

### P0 -- Spike (this repo, 1-2 days)

Answer A0 by measurement, the way wasm-spices-plan section 12 did: a
hand-linked module (`libturi_wasm` + web `libraylib.a` + the raylib spice's
AOT objects + a hand-written image table) that evaluates the section 1
program on a page and draws its text. Record binary size with and without
`-sASYNCIFY`, time-to-first-frame, and fps for the blocking loop. If the
dispatcher or the fibers do not survive emcc, that is the finding, and B
moves first.

### P1 -- The arcade module (this repo)

A1-A4, the `tur_wasm_arcade` target, a CI job that builds it (the existing
`Set up Emscripten` step in `ci.yml` plus the raylib web archive, cached by
raylib ref), and a smoke test through Playwright's Chromium: first frame,
console line, Stop.

### P2 -- The page (arcade repo)

Scaffold from Try's `web/`: Vite, Monaco, the eval and LSP workers (the eval
side moved into the iframe shell), `share-codec.js`, `csp.js`, the theme.
Build the screen pane to the section 5.2 contract. Spices menu, examples
menu, share. Deploy to its own Worker with COOP/COEP headers (the interpreter
module is `-pthread`; keep that, the host serves the headers already).
Playwright: the Try suite's shape (`web/tests/`) trimmed to the arcade's
features.

### P3 -- Examples and docs

The section 8 set, each verified natively and in the page. A short guide in
the arcade repo's README; one paragraph and a link from this repo's
`web-emscripten-tutorial.md` and the raylib spice README ("try it in the
browser").

### P4 -- Build (B0-B2)

The service, the Build button, and the first example that needs it
(`inline-c-shader.tur`, or a spice outside the baked set). Gate by a health
probe so P2's site never depends on it.

### P5 -- Hardening

Mobile layout; a `?example=` deep link; the FPS counter's Asyncify note;
telemetry-free usage counts if the owner wants them (the CSP forbids third
party scripts, keep it that way).

## 8. Examples

Six, chosen so each shows one more thing. All are written in the **blocking
style** the native raylib README uses, because under A the interpreter runs
that unchanged, and B runs it under `:asyncify`. A `with-web-game-loop`
variant of the first one is included to show the callback style that a
shipping game would use under B.

Key codes are raylib's: Left 263, Right 262, Up 265, Down 264, Space 32,
W 87, S 83, R 82. The examples define them as `def`s rather than magic
numbers; a `raylib/keys` module of named constants in the spice would be a
welcome small follow-up and is not required by this plan.

### 8.1 `bouncing-ball.tur` -- the hello world

Motion, `dt`, keyboard, text. State in a `let` with `set!`.

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
(let [x 240.0 y 100.0 vx 140.0 vy 90.0]
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
(let [x 240.0 y 100.0 vx 140.0 vy 90.0]
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
and is the example that shows the interpreter's speed honestly: a 5 184
cell step per frame is the budget A has to meet at 30 fps. If it cannot,
the example drops to 64x36 and says why in its header comment, and it is
the first candidate for the Build button.

### 8.6 `ecs-circles.tur` -- a third spice, with macros

`ecs` + `ecs-raylib`: `Pos2`/`Vel2`/`Radius`/`Tint` components, the
`integrate-2d` and `render-circles` system macros, 200 entities bouncing in
the pane, a key to spawn more. Shows macro-defined systems elaborating
against baked spice exports, and `with-game-loop` -- which under A runs as
is, since it is the blocking form.

### Later (needs B)

- `inline-c-plasma.tur`: a plasma effect whose inner loop is an inline-C
  body writing an `Image` through `image-set!`, uploaded as a texture each
  frame. Interpreted it is refused; compiled it is the showcase.
- `sdf-sphere.tur`: `sdf-raylib`'s `gpu-preview` example, if its shader
  path survives WebGL2 (wasm-spices-plan 9.2 lists it as unaudited).

## 9. Risks and open questions

1. **The interpreter's frame budget.** A tree-walker drawing a few hundred
   shapes per frame is fine; a per-pixel loop is not. P0 measures 8.5's grid
   step. The Build button is the answer for anything past it, which is why B
   is a phase and not a maybe.
2. **Asyncify and the interpreter together.** The interpreter's fibers and
   raylib's `emscripten_sleep` both ride Asyncify; nesting a yield inside an
   interpreter continuation is untested. P0's spike includes a `call/cc`
   program that also draws.
3. **Module size.** libturi + raylib + spices + Asyncify instrumentation.
   Try already ships libturi; the delta is what P0 reports. If it is bad,
   split: the LSP worker keeps the plain module, only the iframe loads the
   arcade one, and the arcade one drops the trace/SMT/tutorial exports
   (`-sEXPORTED_FUNCTIONS` in `src/CMakeLists.txt` lists them).
4. **`dlopen`-shaped assumptions in `spice_loader.c`.** A1 may find the
   image struct wants a library path, a rebuild subprocess, or a `tur_bin`;
   the static constructor must not fake those. Keep `TurSpiceImage`'s
   accessors as the contract and give it a second constructor, not a mode.
5. **Keyboard focus between two documents.** The parent's shortcuts (Run,
   the command palette) must still fire when the iframe has focus; the iframe
   forwards `Ctrl/Cmd+Enter` and `Escape` to the parent by `postMessage`.
6. **The examples' native twin.** CI running raylib natively needs a display
   (`xvfb-run` on Linux) and an exit after N frames; the `--frames` argument
   through `*args*` is a small convention the examples share, not a raylib
   feature.
7. **Name and domain** are the owner's call; nothing in the plan depends on
   them.

## 10. Relationship to existing plans

- **wasm-spices-plan.** This plan consumes its raylib arm (done) and W0's
  `:web :supported` stubs (not done); it does not need W1's HTML shell (the
  iframe is the shell) but does need W1's `*args*` hook and, for B, the asset
  preload. W4's `tur new-web` is unaffected. If the arcade lands first, W6's
  web-games guide gains a "try it in the browser" link.
- **script-spice-deps-plan.** `defscript :spices` is the portability story
  for every example here; B1 is its `--target wasm` arm. SD6's compiled-script
  cache is the build service's cache.
- **aot-compiled-repl-plan.** Unrelated in mechanism (MIR does not target
  wasm) but the same user wish: compiled semantics at an interactive prompt.
  B is that wish for the browser.
- **godot-binding-web-plan.** Shares the Emscripten profile concerns
  (`MODULARIZE`, `EXPORT_NAME`); the iframe-per-run contract in 5.2 is one
  that plan could reuse for a Godot web export preview.
