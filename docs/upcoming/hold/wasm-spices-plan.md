# Emscripten Spice Support + Web-App Scaffold Plan

> **Status:** Draft Plan (revised)
> **Last Updated:** 2026-09-30
> **Type:** Tooling / Web Platform
> **Driving goal of this revision:** ship a raylib game written in Turmeric
> that runs in a browser, and pin down the browser integration points that
> the first draft left undefined.

---

## Revision note (2026-09-30)

The 2026-05-23 draft has drifted from the tree and carried an internal
contradiction. What changed here:

- **Fixed stale references.** The `emcmake` branch is `src/compiler/pkg.c:4007`
  (was cited as `pkg.c:1375`); the target parameter is declared at
  `src/compiler/pkg.h:420`.
- **Dropped `--target web`.** `tur build --target wasm` already exists
  (`src/main.c:12821`) and *hard-rejects* any other target value. A second,
  near-synonymous target name is a trap. One target (`wasm`), with the `:web`
  manifest block deciding whether a browser shell is emitted.
- **Fixed the manifest syntax.** The draft wrote `:web #{...}` and once
  `#fx{:canvas}`. `#{...}` is not map syntax -- maps are `#map{...}`, which is
  what every real `build.tur` uses. `#fx{...}` is an effect row.
- **Resolved the "unmodified" contradiction.** Goals promised raylib programs
  "run unmodified"; the main-loop section then said the native `while` form
  "gets a clear migration error." Both cannot hold. Resolution is two modes
  with a measured tradeoff -- see [Frame loop](#61-the-frame-loop-two-modes).
- **Corrected the raylib API names.** The draft's goal mentioned
  `window-should-close?` and `swap-buffers`. raylib exports
  `window-should-close` (returns `:bool`, no `?`) and `end-drawing`;
  `swap-buffers` is an **opengl** spice function. The two APIs were conflated.
- **Added the section this plan was missing:**
  [Browser integration points](#6-browser-integration-points), including the
  callback boundary, which the repo's own Region Store Hooks STRICT RULE makes
  a correctness requirement rather than a detail.
- **Reversed three "resolved" decisions** (threads, memory growth, audio
  unlock) on evidence -- flagged inline in
  [Amended decisions](#11-amended-decisions).
- **Answered every open question by measurement (2026-09-30).** A second
  pass took the plan's own open questions to a real toolchain -- emcc
  5.0.5-git, CMake 4.3.3, raylib 5.5, Chromium -- rather than leaving them as
  leads. That closed all five, **completed phase W1.5** (raylib renders in a
  browser at 60 fps), **filed one bug**, and **corrected three claims this
  plan had made from upstream documentation**: the Asyncify cost, the
  `simulate_infinite_loop` advice, and the fixed-heap reversal. Section 12 is
  now results rather than questions, and the affected subsections of 6 and 7
  carry the corrections.
- **Re-sequenced the phases raylib-first.** raylib is the forcing function
  that exercises canvas, audio, input, assets and the frame loop at once;
  OpenGL falls out of it nearly free rather than gating it.

---

## 1. Overview

Today `just wasm` builds exactly one artifact: the `try.turmeric-lang.com`
REPL. That is an **interpreter** compiled to WASM (`src/web/wasm_glue.c`,
linked by the `tur_wasm` custom target at `src/CMakeLists.txt:1914`), driven
from JS through `turi_wasm_eval`.

`tur build --target wasm <file.tur>` also already exists and takes a different
path: it compiles the user's program to C and links it with `emcc` instead of
`cc` (`src/main.c:3492`). That is the **AOT application** path, and it is the
one this plan is about. It works today only in the most literal sense -- it
produces a `.js` + `.wasm` pair with none of the things a browser application
needs: no shell page, no canvas, no `-sMODULARIZE`, no asset packaging, no
GL flags, and no frame loop.

This plan lays out:

1. A **per-spice compatibility matrix** for Emscripten, so users know which
   `turmeric-spices` work in the browser today, which need work, and which
   never will.
2. The **`:web` manifest key** that lets spice authors and app authors declare
   browser needs, and the build tooling that consumes it.
3. The **browser integration points** -- the frame loop, the callback
   boundary, canvas sizing, input capture, audio unlocking, assets, memory and
   startup -- specified concretely enough to implement.
4. A **v0 web-app scaffold**: one command producing a deployable static site
   that runs a Turmeric raylib game in the browser.

The goal is that a user with a `build.tur` and no web knowledge can run
`tur new-web my-game` and get a deployable static site.

### Relationship to the existing Emscripten tutorial

[docs/guides/web-emscripten-tutorial.md](../../guides/web-emscripten-tutorial.md)
covers the **interpreter-embedding** path end to end: build the REPL module,
call `turi_wasm_eval` from a page, COOP/COEP, deploying. It has no coverage of
the AOT path, canvas, graphics or frame loops -- checked, and there is no
overlap to reconcile. This plan is the sibling document for the AOT app path,
and W6 below adds the corresponding guide.

---

## 2. Goals / Non-Goals

### Goals (v0)

- One opinionated path: **Vite + a single `index.html` + one `<canvas>`**.
- `tur build --target wasm` on a project with a `:web` block produces a
  static, deployable `web-dist/` (GH Pages, Netlify, Cloudflare Pages).
- **A raylib program that renders and takes input runs in the browser.**
  Programs written in the existing blocking style (`while (not
  (window-should-close)) ... (end-drawing)`) run *without source changes* in
  compatibility mode, at a documented frame-rate cost; a one-line change moves
  them to the full-speed callback mode. Both modes are supported; neither is
  an error.
- OpenGL programs run through the GLFW Emscripten shim (`-sUSE_GLFW=3` +
  WebGL2).
- Pure-Turmeric and inline-C-only spices work with no per-spice changes.
- A spice that cannot work in a browser fails at **configure** time with the
  spice named, not at link time with an undefined symbol.

### Non-Goals (v0)

- Hot-reload of `.tur` source in the browser (the REPL does that; apps
  recompile from the CLI).
- React/Vue/Svelte integration.
- Multi-page apps, routing, SSR.
- Shader/font/image *transformation* pipelines. v0 passes static assets
  through unchanged.
- Off-main-thread execution. See
  [Memory and threads](#68-memory-and-threads) -- for the app target this is
  now an explicit non-goal, not merely deferred.
- WebGPU / WebRTC / WebTransport.

---

## 3. Verified state of the toolchain

Everything in this section was checked against the tree on 2026-09-30. It
matters because the first draft's plan-of-record cited a line that had moved
by ~2600 lines and a subcommand that does not exist.

| Claim | Where | State |
|---|---|---|
| `--target wasm` accepted by `tur build` | `src/main.c:12821` | Exists. Any other value is a hard error: `unknown target '%s' (supported: wasm)`. |
| wasm builds swap `cc` for `emcc` | `src/main.c:3492`-`3507` | Exists. |
| wasm default flags | `src/main.c` (`cc_flags`) | `-O2 -std=c99 -Wall -fno-strict-aliasing -s WASM=1`. Nothing else -- no MODULARIZE, no shell, no GL. |
| `:cmake-deps` configured through `emcmake` | `src/compiler/pkg.c:4007` | Exists: `emcmake cmake -S ...`. **Does not pass `-DPLATFORM=Web`**, which is fatal for raylib rather than merely suboptimal -- verified: the configure fails with `Could NOT find X11` (section 12, Q1). It also suppresses `CMAKE_POLICY_VERSION_MINIMUM` on this arm (`pkg.c:4023`), a filed bug (Q2). |
| `target` parameter on the cmake build | `src/compiler/pkg.h:420` | `NULL` for native, `"wasm"` for Emscripten. |
| `--shared` and `--target` mutually exclusive | `src/main.c:12846` | Yes. |
| `:build-opts :c-sources` / `:c-includes` | `collect_build_aux`, `src/main.c:3544` | Exists. Vendored `.c` files compile as extra TUs and link in. This is where the frame-loop trampoline goes. |
| REPL asset packaging | `src/CMakeLists.txt` `tur_wasm` | Uses `--embed-file`, **not** `--preload-file`. The draft asserted `--preload-file`; that is the right choice for *app* assets but is not what the REPL does, so it is a new mechanism, not an existing one. |
| REPL link flags | `src/CMakeLists.txt` `tur_wasm` | `-sMODULARIZE=1 -sALLOW_MEMORY_GROWTH=1 -sALLOW_TABLE_GROWTH=1 -pthread -sEXIT_RUNTIME=0 -sSTACK_SIZE=16777216 -sINITIAL_MEMORY=67108864 -sEXPORT_NAME=TurmericModule`. Reusable as a starting point, but see the reversals -- `-pthread` and `ALLOW_MEMORY_GROWTH` are the wrong defaults for a game. |
| `^fat` callbacks reach inline C as `int64_t` | `docs/guides/c-integration-guide.md:889` | Dispatch with `TUR_APPLY*`. This is the frame-callback mechanism. |

### raylib spice, as it actually is

`raylib/build.tur` declares `:name "tur-raylib"`, `:version "0.3.0"`, and a
single `:cmake-deps` entry pinning `raysan5/raylib` at `5.5` with
`:targets ["raylib"]` and `BUILD_SHARED_LIBS=OFF`, `BUILD_EXAMPLES=OFF`,
`BUILD_GAMES=OFF`. There is no `:web` block and no `PLATFORM` option.

`raylib/core` exports `init-window close-window begin-drawing end-drawing
begin-mode-3d end-mode-3d clear-background set-target-fps get-frame-time
window-should-close`. The `raylib__*.c` / `.h` files at the spice root are
`/* generated by tur (phase 2) */` but committed -- **not** a place to add
hand-written C.

`ecs-raylib/loop` already ships the boilerplate wrapper this plan needs a web
twin of:

```turmeric
(defmacro with-game-loop [w title width height fps body]
  `(do
     (init-window ~width ~height ~title)
     (set-target-fps ~fps)
     (while (not (window-should-close))
       (let [dt (get-frame-time)]
         (begin-drawing)
         (clear-background (raywhite))
         ~body
         (end-drawing)))
     (close-window)))
```

That macro is the single most useful asset in the tree for this work: it is
the shape almost every raylib program in `turmeric-spices` takes, and it is
already a *callback-shaped* abstraction in disguise. See
[State hoisting](#63-state-hoisting-the-real-porting-cost).

---

## 4. The `:web` key in `build.tur`

App authors and spice authors describe browser needs declaratively. Read when
`--target wasm` is passed.

```turmeric
(defpackage my-game
  :name    "my-game"
  :version "0.1.0"
  :spices #map{
    "raylib" #map{:url    "https://github.com/turmeric-lang/turmeric-spices"
                  :ref    "raylib-v0.3.0"
                  :subdir "spices/raylib"}
  }
  :web #map{
    :canvas       true               ; mount a <canvas id="canvas">
    :canvas-size  [800 600]
    :hidpi        true               ; scale the drawing buffer by devicePixelRatio
    :audio        true               ; wire Web Audio + the unlock gate
    :capture-keys ["Tab" "ArrowUp" "ArrowDown" "ArrowLeft" "ArrowRight" "Space"]
    :persist      false              ; IDBFS, opt-in (sqlite)
    :main-loop    :callback          ; :callback | :asyncify | :none
    :gl           :es3               ; :es3 (WebGL2, default) | :es2 (WebGL1)
                                     ;   drives BOTH raylib's OPENGL_VERSION
                                     ;   and emcc's MAX_WEBGL_VERSION -- 6.9
    :heap         134217728          ; fixed heap; see Memory and threads
    :threads      false              ; DEFAULT false for apps (reversal, see 11)
    :title        "My Game"
    :lazy-assets  ["assets/music/long-track.ogg"]
  })
```

### Spice-side declaration

Library spices advertise what they require of the host page. The app's `:web`
block wins on conflict; spice blocks are unioned. For `tur-raylib`:

```turmeric
(defpackage tur-raylib
  ...
  :web #map{
    :supported true
    :requires  #set{:canvas :audio}
    :main-loop :callback
    :link-flags ["-sUSE_GLFW=3" "-sMAX_WEBGL_VERSION=2"
                 "-sEXPORTED_RUNTIME_METHODS=ccall"]
    :cmake-options #map{:PLATFORM "Web"}
  })
```

Note `#set{...}` for `:requires` (a set of keywords) and `#map{...}` for the
key-value blocks -- the draft used `#{...}` for both, which is neither.

### How the build tool consumes it

`tur build --target wasm` walks the dependency graph, collects every `:web`
block, unions them, and:

1. Hard-errors if any spice in the graph carries `:web #map{:supported false}`,
   naming the spice(s) and pointing at the matrix in section 9.
2. Picks a main-loop strategy (section 6.1) and validates it against what the
   graph requires.
3. Composes the `emcc` link line from the union of `:link-flags` plus the
   built-in base (section 8).
4. Passes each spice's `:cmake-options` through to `emcmake` -- the missing
   piece today, since `pkg.c:4007` configures with no `-DPLATFORM=Web`.
5. Writes/refreshes `web-dist/index.html` from a template with the canvas,
   title, loading indicator and audio-unlock affordance.

---

## 5. Why the browser fights a game loop

One paragraph of orientation, because every integration point below descends
from it.

A browser page runs on a single event-loop thread that also services layout,
input and compositing. A WASM module that enters `while (1)` never returns to
that loop, so nothing repaints, no input is delivered, and the tab hangs. The
page does not get a frame because the program drew one; it gets a frame
because the program *returned* and the browser then chose to composite. Every
difficulty below -- the loop, the callback lifetime, the audio gesture, the
canvas size -- is a consequence of control inversion: the browser calls the
program, not the reverse.

---

## 6. Browser integration points

This is the section the first draft lacked. Each subsection states the
problem, the decision, and what has to be built.

### 6.1 The frame loop: two modes

There are exactly two ways to run a raylib frame loop under Emscripten, and
upstream raylib supports both. The first draft picked callback-only and then
promised "unmodified" programs in its goals; that is the contradiction.

**Mode A -- `:asyncify` (compatibility).** Link with `-sASYNCIFY`. Emscripten
instruments the module so a blocking call can suspend and resume, which lets
`while (not (window-should-close))` yield to the browser. The existing source
runs **unchanged**.

The cost is not merely "some overhead," and this is the number that decides
the mode choice, and it is **measured** (section 12, extra finding 3), not
taken from upstream's blanket warning: raylib's web `WindowShouldClose` waits
a **fixed ~16 ms** (raylib 5.5; 12 ms in 5.6) *in addition to* the frame's own
time. That wait is **additive, not a frame-rate cap**, which makes the cost
depend entirely on how much slack the frame has:

| frame work | `:callback` | `:asyncify` |
|---|---|---|
| ~0 ms | 60.0 fps | 59.5 fps |
| +8 ms | 60.0 fps | **40.0 fps** |

At zero work the wait coincides with one vsync and Asyncify is free; by 8 ms
of real work it has cost a third of the frame rate (`1000/(8+16)` predicts
41.7). Binary cost measured **+21.7%** on a minimal program, growing with how
much code Asyncify must instrument. So Asyncify is a genuinely usable mode for
a game with frame time to spare -- not merely a triage step, which is what an
earlier revision of this plan claimed on a misread of the upstream wiki.

**Mode B -- `:callback` (shipping).** The program registers a per-frame
function and returns. Emscripten calls it once per display refresh via
`emscripten_set_main_loop`. Full speed, no Asyncify, no fixed wait. The cost
is that per-frame state can no longer live in `main`'s stack frame, which is
the real porting work (section 6.3).

**Decision.** Support both, default to `:callback`, and make `:asyncify` a
documented one-key fallback rather than an error. Rationale: `:callback` is
the only mode that produces a shippable game, so it must be the default and
the path the scaffold generates; `:asyncify` is what makes an existing
`turmeric-spices` example or a user's half-finished game render in a browser
*today*, which is worth a great deal for adoption and demos. Emitting a
"migration error" for the blocking form, as the draft proposed, throws that
away for no gain.

`:none` remains for non-interactive programs: run `main()` once, no loop.
Used by offline tools (plutovg rendering to a PNG, scscm compiling text).

### 6.2 The callback boundary, and the region note it requires

This is the load-bearing integration point, and it carries a correctness
requirement from this repo's own rules.

In `:callback` mode a Turmeric closure has to reach
`emscripten_set_main_loop`. The mechanism exists:
[c-integration-guide.md:889](../../guides/c-integration-guide.md) -- a
function-typed parameter marked `^fat` arrives in inline C as an `int64_t`
closure handle, dispatched with `TUR_APPLY*`.

The subtlety is lifetime. `emscripten_set_main_loop(cb, fps, 0)` **returns
immediately**; the browser invokes `cb` afterwards, after `main` has already
returned. So the frame closure -- and everything it captures -- must outlive
the call that registered it, and must be stored somewhere the browser can
reach later. Storing a caller's word into a C global that outlives the
enclosing bracket is precisely the case CLAUDE.md's **Region Store Hooks
STRICT RULE** governs:

> Any primitive that writes a caller's word into memory that can outlive a
> `with-region` / `bt-scope` bracket MUST note that word.

And the automatic note does **not** apply here. The rule exempts a *typed*
node parameter, because the emitter notes it at body entry. A `^fat`
parameter arrives **erased**, as `int64_t` -- the type no longer says it is a
node -- which is exactly the case the rule says needs the manual macro. So:

```turmeric
;;; run-main-loop -- register a per-frame callback and return to the browser.
;;;
;;; Parameters:
;;;   frame -- called once per display refresh
;;;   fps   -- target rate; 0 means "use the display refresh rate"
;;;
;;; Since: W2
(defn run-main-loop [^fat frame : (fn [] #fx{} unit) fps : int] : void
  ```c
  #include <emscripten/emscripten.h>
  /* The browser calls the trampoline after main() has returned, so this
     closure outlives the bracket that built it. It arrives ERASED (int64_t),
     so the emitter's body-entry note does not cover it. */
  TUR_REGION_NOTE(frame);
  tur_web_frame_set(frame);
  emscripten_set_main_loop(tur_web_frame_trampoline, (int)fps, 0);
  ```)
```

A missed note here is a silent use-after-rewind on the default build -- the
failure mode `docs/archive/region-escape-through-unhooked-stores.md` documents
-- and it would present as a game that renders one correct frame and then
garbage, which is an expensive thing to debug from that symptom. Per the same
rule, this joins the hooked set **in the same change** as a fixture:
`tests/fixtures/region-escape-via-main-loop`, modeled on the existing
`region-escape-via-callcc` (the closest analogue: `call/cc`'s stack image is
also a live capture the runtime must not rewind under).

`tur_web_frame_set` / `tur_web_frame_trampoline` are a ~20-line vendored C
shim holding the handle in a file-scope static and calling `TUR_APPLY0`. It
goes in a new `raylib/web/` source declared through
`:build-opts :c-sources` -- an existing mechanism (`collect_build_aux`,
`src/main.c:3544`). It must **not** go in `raylib__*.c`: those are
`/* generated by tur (phase 2) */`.

Two smaller traps at this boundary:

- **`simulate_infinite_loop`.** Pass **`1`**. It unwinds out of `main` by
  throwing a JS exception, so nothing after the call runs -- and on the web
  that is correct rather than regrettable: there is no window-close event, and
  closing the tab reclaims the GL context, textures and heap together, so
  `close-window` and any Turmeric-side teardown have nothing to do.
  **Do not read `0` as the safe choice**: it does not preserve `main`'s frame
  either. Both values abandon it before the first callback, which is exactly
  why the note above is required and why a C++ build facing the same problem
  had to move its state to `static` storage (section 12, extra finding 2).
- **Use `emscripten_set_main_loop_arg`, not the plain form**, and pass the
  `^fat` closure handle as the `void *arg`. The trampoline casts it back and
  `TUR_APPLY0`s it, which removes the file-scope static it would otherwise
  need. The region note is still required: Emscripten retains the handle
  across the bracket either way.
- **`-sEXIT_RUNTIME=0`** matters when `simulate_infinite_loop` is `0`, where
  `main` returns normally and the default teardown would leave the callback
  firing into a dead module. With `1` the unwind keeps the runtime alive on its
  own; set it anyway, since it costs nothing and the two choices should not be
  coupled. The REPL already sets it.

### 6.3 State hoisting: the real porting cost

The draft treated the blocking-to-callback conversion as a "build-time rewrite
or a runtime shim that yields one iteration per call." Neither is cheap,
because of how real programs are written. From `raygui/examples/hello-gui.tur`:

```turmeric
(defn main [] : int
  (init-window 800 600 "hello-gui")
  (set-target-fps 60)
  (let [speed     1.0
        enabled   0
        name-buf  (make-text-buf 64)
        name-edit 0]
    (while (not (window-should-close))
      (begin-drawing)
      ...
      (set! speed (gui-slider ... speed 0.0 10.0))
      (set! enabled (gui-check-box ... enabled))
      (end-drawing))
    (free-text-buf name-buf))
  (close-window)
  0)
```

Mutable per-frame state lives in a `let` **inside `main`** and is updated with
`set!`. A frame callback cannot see it: by the time the browser calls back,
that frame is gone. So the conversion is not syntactic -- it has to relocate
state, which is why a mechanical source rewrite is the wrong tool.

Three options, in the order they should be offered:

1. **A `with-web-game-loop` macro**, the web twin of `ecs-raylib/loop`'s
   `with-game-loop`. The macro already owns the `let`, so it can lift those
   bindings into a heap state box, emit a frame closure that reads and writes
   them, and register it. For any program already using `with-game-loop`, the
   port is **changing the import** -- no body changes. This is the path to
   push, and it is the argument for landing the macro before hand-written
   examples proliferate.
2. **An explicit state-threading form**, for code not using the macro:
   `(run-main-loop-with state : S (fn [S] S))`, where the frame function takes
   the state and returns the next one. Honest about the control inversion and
   needs no mutation.
3. **`:asyncify`**, which needs no restructuring at all. This is why mode A
   has to stay supported rather than erroring: for a program shaped like the
   one above, it is a link-flag change against a source rewrite.

### 6.4 Canvas: size, DPI, resize

Undefined in the draft. There are three distinct sizes and conflating them is
the most common source of a blurry or mis-scaled game:

- The **CSS size** (`style.width/height`) -- how large the canvas appears.
- The **drawing-buffer size** (`canvas.width/height`) -- how many pixels the
  GL context actually renders.
- **`devicePixelRatio`** -- the ratio between them on a HiDPI display.

`init-window w h` sets the drawing buffer. If CSS size and buffer size
disagree, the browser scales the result: a 800x600 buffer in a
1600x1200 CSS box is a soft, upscaled image, and on a 2x display a canvas
sized 800x600 in CSS renders 800x600 real pixels into a 1600x1200 physical
area, which looks blurry on every Mac.

v0 decisions:

- `:canvas-size [w h]` sets both the CSS size and, multiplied by
  `devicePixelRatio` when `:hidpi true`, the drawing buffer.
- The shell reads `devicePixelRatio` **before** `init-window` and passes the
  scaled values in, so raylib and the canvas agree from the first frame.
- Resize is **opt-in and off by default**. A game whose backbuffer changes
  size mid-run needs to re-derive projection matrices and re-layout UI, which
  is a game-specific concern raylib does not solve. v0 ships a fixed canvas
  and a documented `emscripten_set_canvas_element_size` escape hatch, rather
  than a resize handler that silently breaks every fixed-layout game.
- Fullscreen is likewise out of v0: it requires a user gesture and changes
  the drawing-buffer size, so it inherits the whole resize problem.

### 6.5 Input capture

Undefined in the draft, and games break on it immediately. The browser owns
the keyboard and mouse before the canvas does:

- **Arrow keys and Space scroll the page.** A platformer where jumping also
  scrolls the document is the default behavior, not a bug to find later.
- **Tab moves focus** out of the canvas, after which the game receives no key
  events at all.
- **A canvas gets keyboard events only when focusable and focused** --
  `tabindex="0"` plus a click, or explicit focus on load.
- **Mouse-look needs Pointer Lock**, which requires a user gesture and can be
  exited by the user at any time (Esc); the game must handle losing it.
- **Right-click opens the context menu** unless suppressed.

v0: `:capture-keys [...]` lists keys the shell calls `preventDefault` on,
defaulting to the set that breaks games (arrows, Space, Tab). The canvas gets
`tabindex="0"` and is focused on the first click, alongside the audio-unlock
gesture (section 6.6) -- the same click can do both. Pointer Lock is an
escape hatch, not scaffolded.

The keyboard default deserves its bluntness: silently swallowing keys the page
might want is the lesser evil against a game that scrolls itself.

### 6.6 Audio, and the gesture gate

**Reversal.** The draft's resolved decision #6 said the AudioContext
user-gesture requirement is "the user's responsibility; no scaffolded unlock UI
in v0." That should flip, for a reason specific to this platform: every
browser starts an `AudioContext` in a `suspended` state and will not start it
until a real user gesture, so a game that calls `init-audio-device` on load is
**silent, with no error anywhere**. No console message, no failed call --
audio simply never starts. That is the single worst failure shape to hand a
user as "your responsibility," because there is nothing to search for.

The scaffold is also the only component that *can* fix it: the gesture has to
be handled on the page, before the module runs, which is exactly the file
`tur new-web` generates.

v0, when `:audio true`:

- The shell renders a click-to-start overlay and does not call the module's
  entry point until it is clicked.
- That click resumes the `AudioContext` and focuses the canvas (6.5) in one
  gesture -- a start button is a thing users expect in a browser game, so this
  costs no UX.
- The link line adds `-sEXPORTED_RUNTIME_METHODS=ccall`, without which raylib's
  web audio path fails at runtime with `Uncaught ReferenceError: ccall is not
  defined`. Non-obvious and worth having in the base flags rather than
  rediscovered.

The remaining question is whether raylib 5.5's `raudio` module works against
our toolchain at all; upstream
[raysan5/raylib#690](https://github.com/raysan5/raylib/issues/690) and the
live `audio_music_stream` example say it does. That is what the W1.5 spike
confirms before W2 commits to it.

### 6.7 Assets, and the path-matching trap

Assets are packaged with `--preload-file` into a `.data` sidecar fetched at
startup and mounted into MEMFS. Note this is a **new** mechanism for this
repo, not an existing one: the REPL uses `--embed-file` (which inlines bytes
into the `.js`, fine for the stdlib, wrong for multi-megabyte game assets).

The trap, which upstream calls out explicitly: **the path in the code must
match the path given to the packager** -- absolute in both or relative in
both. A mismatch does not fail the build; it produces a runtime "Failed to
open file" and a game with missing textures. v0 therefore packages
`web/assets/**` at exactly the relative path the native target uses, so
`(load-texture "assets/player.png")` resolves identically in both targets,
and the tooling refuses an absolute asset path rather than letting the
mismatch through.

`:lazy-assets [...]` switches selected files to fetch-on-demand, for a big
blob that should not block startup.

### 6.8 Memory and threads

**Two reversals here**, both against the draft's "match the REPL" reasoning.
The REPL and a game have opposite constraints, so inheriting its flags is
wrong in both cases.

**Threads: default OFF for the app target.** The draft resolved "pthreads on
by default, matching the REPL," with COOP/COEP headers templated and a
documented GH Pages caveat. But `-pthread` needs `SharedArrayBuffer`, which
needs `Cross-Origin-Opener-Policy: same-origin` and
`Cross-Origin-Embedder-Policy: require-corp`. Those headers cannot be set on
GitHub Pages at all, and `require-corp` additionally breaks any
cross-origin subresource that does not opt in. The entire point of the app
target is a static site a user can drop anywhere -- so defaulting to a
configuration that cannot be hosted on the most common free static host, to
match an artifact with different needs, inverts the priority. raylib's web
build does not need threads. Default `:threads false`; `:threads true` stays
available and keeps the `_headers` template for hosts that can serve them.

**Memory: prefer a fixed heap, allow growth when the working set is not
known.** Upstream raylib marks `-sALLOW_MEMORY_GROWTH=1` **"NOT
RECOMMENDED"**, because growth invalidates cached views into the heap and
costs a copy at every grow -- a frame-time spike exactly when a level loads.
So a fixed `:heap` (default 128 MB, matching raylib's own
`BUILD_WEB_HEAP_SIZE`) is the better default.

But this is a preference, not the flat reversal an earlier revision of this
plan made of it. A working raylib web game on this machine uses growth
deliberately, and the reason generalizes: a texture cache sized by whatever
the asset manifest names is not knowable at link time. The spike measured
60 fps *with* growth on, so the cost is not visible at small scale.

Two consequences when growth is on: raise `:heap` first if you can bound the
working set, and **export the heap views you rely on** --
`-sEXPORTED_RUNTIME_METHODS=HEAPF32` is what makes Emscripten's
`updateMemoryViews()` re-publish `Module.HEAPF32` after each grow, without
which raylib's audio path reads a stale or absent view (section 12, extra
finding 5).

One interaction to record: `-sASYNCIFY` has its own stack, and the default is
often too small for a deep call graph -- the symptom is a runtime abort
mentioning the Asyncify stack. If mode A shows it, raise
`-sASYNCIFY_STACK_SIZE`.

### 6.9 Startup, loading, and exit

- **First frame.** A `.data` preload plus a multi-megabyte `.wasm` means
  seconds of blank canvas on a cold load. The shell ships a loading indicator
  driven by Emscripten's `setStatus` hook; the click-to-start overlay (6.6)
  covers the tail of it, since the user cannot click until assets are in.
- **GL context choice, and the two-knob hazard.** The GL version is set in
  **two independent places**: raylib's own `OPENGL_VERSION` (`"ES 2.0"` /
  `"ES 3.0"`) at configure time, and emcc's `-sMAX_WEBGL_VERSION=2` on the
  *program* link line. (An earlier revision wrote `-sMIN_WEBGL_VERSION=2`,
  a different setting -- it raises the floor, not the ceiling.)

  **Measured:** v0 targets **ES3 + WebGL2**, which reports
  `OpenGL ES 3.0 (WebGL 2.0)` and `full NPOT textures supported`, against
  ES2's `limited NPOT support (no-mipmaps, no-repeat)`. raylib's Web branch
  defaults `GRAPHICS` to ES2, so ES3 must be named explicitly.

  Set them from **one** `:web :gl` key. If they disagree -- ES3 raylib linked
  without `MAX_WEBGL_VERSION=2` -- the link succeeds, Emscripten hands back a
  WebGL1 context, raylib's shaders fail with `unsupported shader version 300`,
  and the program dies on `TypeError: Failed to execute 'attachShader' ...
  parameter 2 is not of type 'WebGLShader'`. Nothing in that says "you did not
  get WebGL2", so the manifest must make the pair unrepresentable rather than
  documenting the trap. See section 12, Q4.
- **Exit.** `close-window` on the web is close to meaningless -- there is no
  window to reclaim and the tab is still there. In `:callback` mode, quitting
  means `emscripten_cancel_main_loop`, then leaving the module resident. v0
  maps a completed game to "cancel the loop and draw a final frame," and does
  not attempt to tear down the module.

---

## 7. Composed `emcc` link line

What the tool produces, so the flags live in one place rather than spread
across the prose above.

**Base (always):**

```
-sWASM=1 -sMODULARIZE=1 -sEXPORT_NAME=TurmericApp -sEXIT_RUNTIME=0
-sINITIAL_MEMORY=<:heap, default 134217728>
-O2 -std=c99 -Wall -fno-strict-aliasing
```

**When `:canvas true`:** `-sUSE_GLFW=3 -sGL_ENABLE_GET_PROC_ADDRESS`, plus
`-sMAX_WEBGL_VERSION=2` when `:gl` is `:es3`. `GL_ENABLE_GET_PROC_ADDRESS` is
insurance, not a requirement -- a minimal ES2 program runs clean without it,
but a real game using shaders has been seen to die at `InitWindow()` (section
12, extra finding 4).

**When `:audio true`:** `-sEXPORTED_RUNTIME_METHODS=ccall,HEAPF32`. Both:
`ccall` per upstream, and `HEAPF32` because miniaudio reads the heap as
`Module.HEAPF32.buffer` from its `ScriptProcessorNode` callback and Emscripten
no longer publishes those views by default. Omitting `HEAPF32` is a
`TypeError` thrown out of the audio callback, and only once audio is actually
unlocked (6.6).

**When `:main-loop :asyncify`:** `-sASYNCIFY` (+ `-sASYNCIFY_STACK_SIZE` as needed)
**When `:threads true`:** `-pthread -sPTHREAD_POOL_SIZE_STRICT=0` + the `_headers` template
**When assets exist:** `--preload-file web/assets@assets`
**When `:persist true`:** `-sFORCE_FILESYSTEM=1` + IDBFS mount
**When `:heap` is unset and the working set is unbounded:** `-sALLOW_MEMORY_GROWTH=1`, which then *requires* the `HEAPF32` export above (6.8).

**Passed to `emcmake` for raylib:** `-DPLATFORM=Web` -- **not optional**, and
not something raylib infers: without it the configure fails with
`Could NOT find X11` (section 12, Q1). Plus `-DOPENGL_VERSION="ES 3.0"` for
`:gl :es3`, using raylib's own enum_option rather than poking `CMAKE_C_FLAGS`.

**One `:gl` key drives both GL knobs.** `-DOPENGL_VERSION` (configure) and
`-sMAX_WEBGL_VERSION` (link) must agree; a mismatch links clean and dies on a
`TypeError` in `attachShader`. The tool composes both from `:web :gl` and
never exposes them separately. See 6.9.

---

## 8. Main-loop strategy table

| Strategy | Trigger | What we do | Cost |
|---|---|---|---|
| `:none` | No spice requests a loop | Run `main()` once. | -- |
| `:callback` | **Default** when a canvas spice is present | Frame closure registered via `emscripten_set_main_loop`; `main` returns. Needs `-sEXIT_RUNTIME=0` and the region note (6.2). | State must leave `main` (6.3). |
| `:asyncify` | Opt-in, or `tur build` suggests it when it sees a blocking loop | Link `-sASYNCIFY`; existing `while` loop yields. | A fixed ~16 ms wait **added to** frame time: measured 59.5 fps at ~0 ms of work, 40.0 fps at 8 ms (callback holds 60.0 at both). Binary +21.7% on a minimal program. |

---

## 9. Per-spice Emscripten compatibility matrix

### 9.1 Audited

| Spice | Tier | Browser status | Notes |
|-------|------|----------------|-------|
| `tur-test`     | 1 pure | Works | No I/O. |
| `tur-math`     | 1 pure | Works | No I/O. |
| `tur-c-dsl`    | 1 pure | Works | Compile-time codegen; runtime is a no-op. |
| `tur-glsl`     | 1 pure | Works | Output is shader text. |
| `tur-tidal`    | 1 inline-C | Works | Pure string transformation. |
| `tur-scscm`    | 1 inline-C | Partial | String compiler works. OSC client needs a WebSocket bridge (post-v0). |
| `tur-opengl`   | 2 cmake | Via shim | GLFW Emscripten port; restrict to the GLES 3.0 subset (WebGL2). |
| `tur-raylib`   | 2 cmake | First-class | raylib 5.5 has `PLATFORM=Web`. Main loop per section 6.1. |
| `tur-sqlite`   | 2 cmake | Via MEMFS/IDBFS | MEMFS by default; IDBFS via `:web :persist true`. |
| `tur-png`      | 3 cmake | Works | libpng + zlib compile clean under emcc. |
| `tur-plutovg`  | 3 cmake | Works | No platform deps. |
| `tur-json`     | 3 cmake | Works | yyjson is portable C. |
| `tur-regex`    | 3 cmake | Works | PCRE2 supports Emscripten upstream. |
| `tur-wav`      | 3 cmake | Works | Pure decode/encode. |
| `tur-http`     | 3 cmake | Remap | mbedTLS cannot reach the network; remap to `fetch()` via `EM_JS`. v0 stubs with a clear error. |
| `tur-osc`      | 3 cmake | Never | UDP unavailable in browsers; needs a WebSocket-OSC bridge. |
| `tur-rtaudio`  | 3 cmake | Research | Would need a Web Audio (`AudioWorklet`) backend. Post-v0. |
| `tur-rtmidi`   | 3 cmake | Research | Would need Web MIDI. Smaller than rtaudio; still post-v0. |
| `tur-postgres` | 3 cmake | Never | libpq has no browser story. Hard-error. |
| `tur-valkey`   | 3 cmake | Never | TCP client. Hard-error. |

### 9.2 Not yet audited -- coverage gap

The matrix above was written against a 20-spice tree. `turmeric-spices` now
holds **47**. Recording the gap explicitly rather than letting the table read
as complete:

`ansi`, `crdt`, `ecs`, `ecs-raylib`, `frame`, `httpd`, `linalg`, `msgpack`,
`notebook`, `plot`, `raygui`, `sdf-raylib`, `secret`, `signal`, `stats`,
`template`, `thread-pool`, `tls`, `tourist`, `tourist-session`,
`tourist-session-valkey`, `tourist-ws`, `watch`, `ws-client`, `ws-core`,
`ws-server`, `zlib`.

Three of those matter for this plan's goal and should be audited in W0, since
they are raylib-adjacent and a game will reach for them:

- **`ecs-raylib`** -- owns `with-game-loop`, the macro section 6.3 proposes a
  web twin of. Should ship that twin.
- **`raygui`** -- its examples are the blocking-loop-with-`let`-state shape
  that motivates 6.3; the best porting test cases in the tree.
- **`sdf-raylib`** -- `raylib/integration.tur` renders per frame and has its
  own camera loop; check whether it needs the same treatment.

Provisional reads on the rest, to be confirmed rather than trusted: the pure
and compute spices (`ansi`, `crdt`, `linalg`, `msgpack`, `plot`, `signal`,
`stats`, `template`, `ecs`, `frame`, `secret`, `zlib`) should fall on the
works side; everything socket-shaped (`httpd`, `tls`, `ws-*`, `tourist*`,
`watch`) will not work without a remap, for the same reason as `tur-osc`;
`thread-pool` depends on the `:threads` decision in 6.8 and is unavailable
under the new default.

---

## 10. Phases

Re-sequenced raylib-first: raylib exercises canvas, audio, input, assets and
the frame loop simultaneously, so it finds integration bugs that a pure-spice
phase cannot, and it is the driving goal. OpenGL becomes a near-free
follow-on rather than a gate.

### Phase W0 -- Audit + manifest plumbing

- Land this revision.
- Add a `:web #map{:supported true|false :reason "..."}` stub to every spice
  manifest, defaulting `false` for the never-works rows. Close the 9.2 gap,
  starting with `ecs-raylib`, `raygui`, `sdf-raylib`.
- Wire `:web` parsing into `pkg.c` and thread `:cmake-options` into the
  existing `emcmake` configure at `pkg.c:4007` -- today it passes no
  `-DPLATFORM`.
- Hard-error on `:supported false` in the dep graph, naming the spice.

### Phase W1 -- AOT app shell, no canvas

- Produce `index.html` + `main.js` + `app.wasm` for pure-Turmeric /
  inline-C-only programs, with `-sMODULARIZE=1 -sEXPORT_NAME=TurmericApp
  -sEXIT_RUNTIME=0` and a fixed heap.
- `*args*` from `?arg=foo&arg=bar`, plus a `turi_wasm_set_args` escape hatch
  callable before the entry point. (CLAUDE.md's rule on how Turmeric code
  *reads* args -- `*args*` / `stdlib/args.tur` only -- is unchanged.)
- `--preload-file web/assets@assets` with the relative-path guard from 6.7;
  `:lazy-assets` opt-out.
- Validate with the `tur-tidal` + `tur-scscm` "compile a tune to text" demo.

### Phase W1.5 -- raylib spike: DONE 2026-09-30

The probe ran outside the Turmeric toolchain (raw emcc against raylib 5.5, so
nothing here waited on W0/W1). **The browser story is real.** Full results in
[section 12](#12-resolved-by-measurement-2026-09-30); what W2 needs to know:

1. **Does raylib 5.5 build for web?** Yes. `emcmake cmake -DPLATFORM=Web` then
   `cmake --build` -> `libraylib.a`, 4.5 MB, exit 0, clean under CMake 4.3.3
   with no policy flag. **`-DPLATFORM=Web` is mandatory** -- without it the
   configure dies on `Could NOT find X11`, so W0's `:cmake-options`
   passthrough is a prerequisite, not a nicety.
2. **Does a triangle render?** Yes -- 800x600 canvas, `Platform backend: WEB
   (HTML5)`, **60.0 fps** in Chromium, zero errors. Confirmed on both GL
   paths: ES2/WebGL1 and ES3/WebGL2. ES3 is the pick (6.9).
3. **`raudio`?** The module loads (`raudio:.... loaded (optional)`) on every
   variant. **Sound was not driven to output** -- that needs the gesture gate
   from 6.6 and is the one W1.5 question left for W2, now with the
   `ccall,HEAPF32` export requirement already pinned down (section 12, extra
   finding 5).

One bug fell out and is filed:
[wasm-arm-suppresses-cmake-policy-min](../../archive/wasm-arm-suppresses-cmake-policy-min.md).
raylib itself is immune (its floor is exactly 3.5), so W2 is not blocked on
the fix -- but the second cmake dep will be.

**Still worth doing as a fixture:** all of the above was measured by hand.
W2 should land the ES3-vs-ES2 context assertion and the 60 fps floor as
something CI can run, because both are the kind of thing a toolchain bump
breaks silently.

### Phase W2 -- raylib: canvas, loop, input, audio

- `tur-raylib` `:web` block per section 4.
- `raylib/web` module: `run-main-loop` (6.2), the `:c-sources` trampoline
  shim, and `with-web-game-loop` (6.3).
- **`tests/fixtures/region-escape-via-main-loop`**, and `raylib/web` joins the
  hooked-store set in CLAUDE.md -- same change, per the strict rule.
- `-sASYNCIFY` mode A behind `:main-loop :asyncify`, with its measured cost
  documented at the point of use: a fixed ~16 ms added to frame time, which is
  free at low frame work and ~40 fps by 8 ms of it (section 12, extra finding
  3).
- Canvas sizing/HiDPI (6.4), key capture (6.5), audio unlock overlay (6.6),
  loading indicator (6.9).
- Port one `raygui` example both ways -- macro swap and Asyncify -- as the
  end-to-end proof and the documentation's worked example.

### Phase W3 -- OpenGL

- `tur-opengl` `:web` block with the GLFW shim flags (mostly shared with W2).
- `(opengl/web/run-main-loop frame-fn)` over the same trampoline.
- Port a `tests/fixtures/opengl/*` triangle demo.

### Phase W4 -- `tur new-web` scaffold generator

- Template at `templates/web-app/`; `--with raylib` / `--with opengl` choose
  the starter `main.tur`. Generates the `:callback`-mode `main.tur`, so the
  default a new user meets is the shippable one.
- `tur dev --target wasm` runs `vite dev` with a `.tur` watcher.
- Vite remains the right default: already in the repo for the REPL, its ES
  module dev server matches `-sMODULARIZE=1`, zero-config `.wasm`/`.data`
  handling, and the scaffold is a template rather than a runtime, so it is
  cheap to replace.
- **Port discipline:** the scaffold's dev server and any browser test must
  pick their own port. Port 3000 is the developer's own Try Turmeric dev
  server and is off-limits per CLAUDE.md -- a `PreToolUse` hook enforces it.

### Phase W5 -- Storage + polish

- `tur-sqlite` `:web :persist true` -> IDBFS preload + `FS.syncfs` on quit.
- Fullscreen and canvas resize, if a demand appears (both deferred in 6.4 for
  the projection/layout reasons given).

### Phase W6 -- Docs

- A `web-games-guide.md` sibling to the existing interpreter-focused
  `web-emscripten-tutorial.md`: the two loop modes and how to choose, the
  state-hoisting port, and the integration traps from section 6 (asset paths,
  `ccall`, audio gesture, key capture, HiDPI).
- Per CLAUDE.md, a guide links plans and reports by **GitHub URL**, not
  relative path -- only `guides/` and `api/` are published.

### Phase W7 (post-v0) -- Network + audio I/O

- `tur-http` over `fetch()` via `EM_JS`.
- Decide rtaudio/rtmidi: AudioWorklet backend, or a separate `tur-webaudio`
  spice that does not pretend to be rtaudio.
- OSC-over-WebSocket bridge for `tur-scscm` / `tur-osc`.

---

## 11. Amended decisions

Carried from the 2026-05-23 list, with three reversals marked.

1. **Bundling.** Single monolithic `app.wasm` for v0. Reserve
   `:web :side-module true` so per-spice lazy-loaded modules can come later.
   *(Unchanged.)*
2. **Threads -- REVERSED.** Was "on by default, matching the REPL." Now
   **off by default** for the app target: `-pthread` requires COOP/COEP, which
   GitHub Pages cannot serve, and the app target exists to be droppable on a
   static host. `:threads true` opts in and keeps the `_headers` template.
   Reasoning in 6.8.
3. **Memory -- AMENDED (reversed, then softened on evidence).** Was
   unsettled, with the REPL's `-sALLOW_MEMORY_GROWTH=1` as the implied
   default. A previous revision flipped that to a hard **fixed heap** on
   upstream's "NOT RECOMMENDED". Now: **prefer** a fixed `:heap` (default
   128 MB) where the working set is known, **allow growth** where it is not --
   a working raylib web game uses growth for exactly that reason, and the
   spike measured 60 fps with it on. If growth is on, exporting `HEAPF32` is
   not optional. Reasoning in 6.8.
4. **File-system surface.** MEMFS by default, lost on tab close; `:persist
   true` for IDBFS + `FS.syncfs`. No NODEFS in browser builds. *(Unchanged.)*
5. **`*args*`.** From `?arg=foo&arg=bar`, with a `turi_wasm_set_args` escape
   hatch. *(Unchanged.)*
6. **Asset pipeline.** `web/assets/**` preloaded into MEMFS at the same
   relative path the native target uses, with `:lazy-assets` as the
   fetch-on-demand opt-out. *(Unchanged, plus the path-matching guard in
   6.7.)*
7. **Audio unlock -- REVERSED.** Was "the user's responsibility; no
   scaffolded unlock UI." Now **the scaffold ships the gesture gate**: without
   it a game is silent with no error of any kind, and the page is the only
   place the gesture can be handled. Reasoning in 6.6.
8. **Main loop -- AMENDED.** Was callback-only, with a "clear migration error"
   for the blocking form, which contradicted the "runs unmodified" goal. Now
   **both modes**, `:callback` default, `:asyncify` a supported fallback.
   Reasoning in 6.1.
9. **Error UX.** Hard-error when the graph holds `:supported false`, naming
   the spice(s) and pointing at section 9. No auto-stubs. *(Unchanged.)*
10. **Target name -- NEW.** No `--target web`. `--target wasm` already exists
    and rejects other values; `:web` in the manifest decides whether a
    browser shell is emitted.

---

## 12. Resolved by measurement (2026-09-30)

Every question this plan opened is now answered against a real toolchain on
the development machine -- **emcc 5.0.5-git, CMake 4.3.3, raylib 5.5, Chromium
via Playwright**. Raw logs and the spike sources are disposable; the numbers
and mechanisms are below. Two answers **corrected this plan**, one found a
**bug now filed**, and the end-to-end spike (phase W1.5) is **done**: raylib
renders in a browser at 60 fps.

### Q1. Does raylib's CMake autodetect Emscripten? **No -- and it fails loudly.**

Answered twice over. By reading: `CMakeOptions.txt:5` is
`enum_option(PLATFORM "Desktop;Web;Android;Raspberry Pi;DRM;SDL" ...)`, and
`cmake/EnumOption.cmake` takes `list(GET ${var}_VALUES 0 default)` -- so
**`PLATFORM` defaults to `Desktop`**. There is no `EMSCRIPTEN` / `Emscripten`
test anywhere in raylib's `cmake/` tree or either `CMakeLists.txt`.

By running it, which is the part that matters:

```sh
emcmake cmake -S <raylib-5.5> -B out -DBUILD_EXAMPLES=OFF   # no -DPLATFORM
# => exit 1
# CMake Error at .../FindPackageHandleStandardArgs.cmake:290 (message):
#   Could NOT find X11 (missing: X11_X11_LIB)
# Call Stack: src/external/glfw/src/CMakeLists.txt:181 (find_package)
```

So the `:cmake-options` passthrough in W0 is **load-bearing, not a no-op** --
the hedge in the previous revision was wrong. Without `-DPLATFORM=Web`,
raylib takes the Desktop branch, builds its bundled GLFW, and that GLFW looks
for **X11**.

**The failure message names neither Emscripten nor `PLATFORM`.** A user
building a raylib spice for wasm today is told to install X11, which is a
dead end. W0 should detect this shape and say what it means, because nobody
will guess it.

Independently corroborated: `terminal-est/build-web/CMakeCache.txt` on this
machine -- a working raylib web build configured *by emcmake* -- records
`PLATFORM:STRING=Desktop`, and its `CMakeLists.txt` carries an explicit
`if(EMSCRIPTEN) set(RAYLIB_PLATFORM "Web")` with the comment *"'Desktop' is
raylib's default, so the native build is unchanged by naming it."*

With the flag, both halves are clean:

```sh
emcmake cmake -S <raylib-5.5> -B out -DPLATFORM=Web ...  # exit 0
cmake --build out -j8                                    # exit 0 -> libraylib.a, 4.5 MB
```

and the bundled GLFW is never added (0 mentions of x11/glfw in the log),
because `PLATFORM=Web` uses the Emscripten GLFW port instead.

### Q2. `CMAKE_POLICY_VERSION_MINIMUM` on the wasm arm: **a real bug. Filed.**

Not deliberate. `src/compiler/pkg.c:4023` guards the flag with `!wasm`, so a
`:cmake-deps` entry with a pre-3.5 floor builds natively and dies for wasm.
Measured with a three-line project at `cmake_minimum_required(VERSION 3.2)`:

| Arm | Flag | Exit | Message |
|---|---|---|---|
| native | passed (pkg.c does) | 0 | -- |
| wasm | suppressed (pkg.c does) | **1** | `Compatibility with CMake < 3.5 has been removed from CMake.` |
| wasm | passed | 0 | -- |

raylib dodges it because its own floor is *exactly* `3.5`, the lowest CMake 4
still accepts -- which is why this has never been hit. The second cmake dep
finds it; `hiredis` is named in pkg.c's own comment as the motivating case.

Filed as
[docs/archive/wasm-arm-suppresses-cmake-policy-min.md](../../archive/wasm-arm-suppresses-cmake-policy-min.md).
Fix is dropping the `!wasm` conjunct: the `cmake_major_version() >= 4` test
already handles the CMake 3.x noise the comment was guarding against, and it
is arm-independent.

### Q3. Asyncify + `-pthread` together: **they link. No refusal needed.**

```sh
emcc t.c -sASYNCIFY          -o a.js   # ok, 45,762 B wasm
emcc t.c -sASYNCIFY -pthread -o b.js   # ok, 112,502 B wasm
```

No error, no warning. "Known-awkward" is not borne out at the link level, so
the tool should **not** refuse the combination. It stays an odd thing to want
(threads are off by default per 6.8, and the frame loop is single-threaded
either way), but that is the user's call, not a validation error.

### Q4. GLES2 vs GLES3: **ES3/WebGL2 is the right floor, and the mismatch failure is vicious.**

Both build and both run. The difference is measured, not theoretical:

| | raylib `GRAPHICS` | Context reported | NPOT textures |
|---|---|---|---|
| ES2 (raylib's Web **default**) | `GRAPHICS_API_OPENGL_ES2` | `OpenGL ES 2.0 (WebGL 1.0)`, GLSL ES 1.00 | `WARNING: GL: NPOT textures extension not found, limited NPOT support (no-mipmaps, no-repeat)` |
| ES3 + `-sMAX_WEBGL_VERSION=2` | `GRAPHICS_API_OPENGL_ES3` | `OpenGL ES 3.0 (WebGL 2.0)`, GLSL ES 3.00 | `INFO: GL: NPOT textures extension detected, full NPOT textures supported` |

ES2's NPOT restriction is a real constraint on a real game -- no mipmaps and
no repeat on any texture whose dimensions are not powers of two -- and ES3
also ran with **zero warnings** where ES2 logged one. That decides it: **ES3
+ WebGL2 is the v0 floor**, `OPENGL_VERSION="ES 3.0"` via raylib's own
enum_option. (raylib's Web branch defaults `GRAPHICS` to ES2 when nothing
sets it, so this must be named explicitly.)

The important finding is the **mismatch** failure. The GL version is set in
**two places that do not know about each other** -- raylib's `OPENGL_VERSION`
at configure time and emcc's `MAX_WEBGL_VERSION` at link time. Linking an
ES3-compiled raylib *without* `-sMAX_WEBGL_VERSION=2` links clean, then at
runtime:

```
INFO:  > Version: OpenGL ES 2.0 (WebGL 1.0 ...)      <- silently a WebGL1 context
WARNING: SHADER: [ID 3] Compile error: '' : unsupported shader version 300
TypeError: Failed to execute 'attachShader' on 'WebGLRenderingContext':
           parameter 2 is not of type 'WebGLShader'.
```

A JS `TypeError` about `attachShader`, with nothing saying "you asked for
WebGL2 and did not get it." **So `:web` must carry one `:gl` key that derives
both knobs**, and the tool must never let a user set them separately. That is
a concrete change to section 4, and it is the single best argument in this
document for the manifest owning the flag composition rather than documenting
it.

### Q5. Where does `with-web-game-loop` live? **`raylib/web`, and it is now clearly its own thing.**

Still a judgment call rather than a measurement, but the measurements settled
it. The web loop is not `with-game-loop` with a different registration call:
it must also **drop `set-target-fps`** (6.1, and Q-extra below), which
`ecs-raylib/loop` calls unconditionally. A macro that differs in its body,
not just its tail, is its own macro. It goes in `raylib/web`, with
`ecs-raylib` re-exporting for ECS users.

### Extra findings the spike produced

Not questions this plan asked, and all three change the guidance.

**1. `set-target-fps` is actively harmful on web, and `ecs-raylib` calls it.**
raylib's `SetTargetFPS` *sleeps* to hit a rate, and sleeping is the one thing
that must not happen on the thread servicing the page. Pass `0` to
`emscripten_set_main_loop*` instead, which asks for `requestAnimationFrame`
-- it matches the display and stops entirely when the tab is hidden.
`ecs-raylib/loop`'s `with-game-loop` emits `(set-target-fps ~fps)`
unconditionally, so its web twin must not.

**2. `simulate_infinite_loop = 1` is correct, and section 6.2's advice to
pass `0` was wrong in its reasoning.** The previous revision warned that `1`
means nothing after the call runs. True, and that is the point: on the web
there is no window-close event and closing the tab reclaims the GL context,
textures and heap in one go, so the teardown is unreachable *and* unnecessary.
What matters is the part that advice got backwards -- **main's stack frame is
abandoned in both modes**, so `-sEXIT_RUNTIME=0` plus `0` does not save it.
A working C++ raylib web build on this machine states it directly:

> `emscripten_set_main_loop_arg()` does not return, it unwinds out of `main()`
> and lets the browser call `run_frame()` from the event loop afterwards.
> **Anything left on main's stack would be dangling by the first frame.**
> Static storage sidesteps that.

That is [6.2](#62-the-callback-boundary-and-the-region-note-it-requires)'s
requirement reached independently, in C++, for the same reason. It is the
strongest available evidence that the `TUR_REGION_NOTE` on the frame closure
is load-bearing and not defensive: a language with no regions at all still had
to move that state to static storage to survive the first frame.

Use `emscripten_set_main_loop_arg` (not the plain form) and pass the `^fat`
closure handle as the `void *arg`, which removes the file-scope static the
trampoline would otherwise need. The region note is still required -- the
handle is retained by Emscripten across the bracket either way.

**3. Asyncify's cost is a fixed additive wait, not a flat frame-rate cap.**
The previous revision said "~30 fps" from the upstream wiki. Measured, that
is wrong in both directions. Same program, both modes, 2-second rolling
average in Chromium:

| frame work | `:callback` | `:asyncify` |
|---|---|---|
| ~0 ms (one triangle) | 60.0 fps | 59.5 fps |
| +8 ms busy-work | 60.0 fps | **40.0 fps** |

The additive model predicts `1000/(8+16) = 41.7`; measured 40.0. So the wiki's
*mechanism* is right and its headline is misleading: the fixed ~16 ms wait is
**free when the frame has slack** (at zero work it coincides with one vsync,
giving ~60 fps) and costs roughly a third of the frame rate once real work
reaches 8 ms. Binary cost was **+21.7%** (140,371 -> 170,770 bytes), not the
"~2x" the wiki warns of -- though that ratio grows with how much code Asyncify
must instrument, so a real game will sit above it.

Revised guidance, which is *more* favorable to Asyncify than this plan was:
it is a genuinely usable mode for a game with frame time to spare, not merely
a triage step. `:callback` stays the default because it has no such ceiling.

**4. `-sGL_ENABLE_GET_PROC_ADDRESS` is conditional, not mandatory.** A
working C++ build on this machine notes that without it "raylib 5.5 links and
then dies at `InitWindow()`." That did **not** reproduce on the minimal ES2
spike, which ran clean at 61 fps without the flag. So it is needed on some
feature path (custom shaders / extension loading) and not by every program.
Include it -- it is free insurance -- but do not document it as required, and
do not let its absence be the first suspect for an `InitWindow` failure.

**5. `-sEXPORTED_RUNTIME_METHODS` needs `HEAPF32` for audio, not just
`ccall`.** Section 7 has `ccall` from the upstream wiki. The real-world note
is that miniaudio -- what raylib's `raudio` is built on -- reaches the heap as
`Module.HEAPF32.buffer` from inside its `ScriptProcessorNode` callback, and
Emscripten no longer hangs heap views off `Module` by default. Without it the
read is `undefined.buffer`, a `TypeError` thrown out of the audio callback.
Naming it is also what makes `updateMemoryViews()` re-publish the view after
each heap growth. Export **both**.

Note how that interacts with 6.6: it "only bites once the AudioContext is
actually running", so it hides until someone taps to unlock audio -- which is
why it looked like a mobile-Safari bug.

**6. Memory growth: reversal 3 was too strong.** 6.8 argued for a fixed heap
on upstream's "NOT RECOMMENDED". But the working game build on this machine
*does* use `-sALLOW_MEMORY_GROWTH=1`, with a good reason: a texture cache
sized by what the asset manifest names is not known at link time. Softened:
prefer a fixed `:heap` when the working set is known, allow growth when it is
not, and if growth is on, **export the heap views you rely on** (finding 5)
so they survive it. The spike used growth and measured 60 fps, so the
frame-time cost is not visible at this scale.

### What remains genuinely open

One item, and it is a product question rather than a technical one:

- **Is dropping pre-WebGL2 devices acceptable for v0?** Q4 settles that ES3 is
  technically better; it does not settle the audience. WebGL2 has been in
  every evergreen desktop and mobile browser for years, so the exposure is old
  Android and pre-2021 iOS. Decide from the Try Turmeric site's own numbers if
  they exist, and note that ES2 remains a one-key fallback either way
  (`:web #map{:gl :es2}`), so this is reversible and not worth blocking on.

## 13. Relationship to existing work

- The WASM **REPL** (`web/`, `src/web/wasm_glue.c`, the `tur_wasm` target at
  `src/CMakeLists.txt:1914`) is untouched. The app scaffold is a sibling
  target. Its flag set is a useful reference but not a template -- see the two
  reversals in 6.8.
- [docs/guides/web-emscripten-tutorial.md](../../guides/web-emscripten-tutorial.md)
  documents the interpreter-embedding path (`turi_wasm_eval`, COOP/COEP,
  Vite wiring). No canvas, graphics or loop coverage; no overlap. W6 adds the
  AOT-app sibling guide.
- `src/compiler/pkg.c:4007` already branches on `target == "wasm"` for
  `emcmake`. v0 reuses that branch and adds `:web` parsing plus
  `:cmake-options` passthrough alongside.
- [godot-binding-web-plan.md](godot-binding-web-plan.md) is blocked on this
  plan and needs its Emscripten profile to agree with ours -- particularly
  `-sMODULARIZE` and `EXPORT_NAME`, since Godot's web export runs its own
  `Module` instance. **Its link to this plan is stale:** it points at
  `../wasm-spices-plan.md`, but both files now live in `hold/`, so the
  relative path resolves to a nonexistent `docs/upcoming/wasm-spices-plan.md`.
  Fix when either plan next moves.
- `docs/archive/history/plutovg-spice-plan.md` and
  `docs/archive/scscm-tidal-spices-plan.md` remain consistent: both spices are
  on the works side of the matrix and are W1's validation demo.
