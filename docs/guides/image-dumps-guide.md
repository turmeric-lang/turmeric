---
title: Application Image Dumps
category: CLI Tools
description: Warm-start a Turmeric program by saving and restoring a post-init continuation, Lisp/Smalltalk/pdumper style.
since: Phase AI
tags:
  - continuations
  - serialization
---

# Application Image Dumps

Expensive init -- loading config, parsing schemas, populating caches, building
indices, registering handlers -- is paid on every program start. For
long-running daemons it amortizes; for CLIs and short-lived workers it
dominates wall-clock latency.

Lisp (`save-lisp-and-die`), Smalltalk (image saves), and Emacs (`pdumper`)
solved this decades ago: dump the heap after init, load it back on later
starts, skip init. Turmeric's `tur/image` does the same with a **serializable
continuation** captured at a quiescent post-init point.

## What it does

`(with-image-cache-after-init PATH init loop)` runs `init` then `loop` on a
cold start, and -- crucially -- writes the continuation that runs `loop` to an
image file in between. On a later run, if a valid image exists, it restores
that continuation and jumps straight into `loop`, skipping `init` entirely.

The image file is a fixed 72-byte header (`src/runtime/image.h`) followed by
the Phase 21 TSER payload bytes of the captured continuation and, when the
program declares `defimage-global`s, a second section holding their values
(see "Globals"). No new wire format -- it frames Phase 21's serial codec.

## When to use it

- Init is slow (>~100 ms) and deterministic.
- Cold startup latency matters (CLIs, test harnesses, short-lived workers).
- The post-init state can be expressed as a serializable continuation.

## When *not* to use it

- **The process holds live OS handles** across the capture point -- open
  sockets, GPU contexts, in-flight async I/O. These are `STAG_PTR` opaque and
  rejected by `TUR-E0018` at the `serial-shift` site; re-acquire them after
  load (see "Resources" below).
- **Init has side effects you must not skip** (e.g. it registers something the
  OS forgets between runs).
- **You ship a different binary every deploy** -- images are pinned to the
  binary that wrote them (see "Build-stamp safety").
- **The image could come from an untrusted source** -- loading an image is
  `eval` (see "Security").

## The APIs

```turmeric
(load "stdlib/image.tur")

;; Primary combinator: split init from loop.
(with-image-cache-after-init "/var/cache/app.img"
  do-init      ; a (fn [] int) -- runs only on cold start
  main-loop)   ; a (fn [] int) -- runs on both cold and warm starts

;; Single-body wrapper (capture happens *after* body returns).
(with-image-cache "/var/cache/app.img" do-everything)

;; Low-level building blocks.
(save-image! "/var/cache/app.img")   ; capture here, write to disk
(load-image! "/var/cache/app.img")   ; restore + resume
```

`with-image-cache-after-init` is a **macro**, not a function: `init` and `loop`
are spliced into call position so the captured tail `(loop)` is a call to a
*named* top-level function. This is required -- the serial-shift collector
reconstructs frames by stable name, so the resumed tail must be a named
(serializable) continuation, never a heap closure. **Pass top-level `defn`
names**, not anonymous `fn`s, for `init` and `loop`.

`load-image!` returns `0` both on a validation failure *and* when the resumed
continuation legitimately evaluates to `0`. When the cold/warm decision matters,
gate on `image/loadable?` first (which `with-image-cache-after-init` does
internally) rather than on the return value.

## Globals: why your `def` isn't in the image

The image captures *the continuation*, which means **only values transitively
closed over by the captured frames** are in it. A top-level `def ^mut` that the
captured continuation doesn't reference is **not** in the image -- a foot-gun
if you expect whole-heap Lisp-image semantics: a global written during `init`
is silently back at its initial value after a warm start, because `init` is
exactly the code a warm start skips.

`defimage-global` declares a global that **is** in the image:

```turmeric
(defimage-global counter  :int   0)       ; T must have a Serializable instance
(defimage-global greeting :cstr  "cold")
(defimage-global ratio    :float 1.5)

(defn do-init [] : int
  (set! counter 42)                       ; written on the cold start only ...
  (set! ratio 7.1)
  0)

(defn main [] : int
  (image/track-globals! counter greeting ratio)   ; at the TOP of main
  (with-image-cache-after-init "/var/cache/app.img" do-init do-loop))
  ;; ... and a warm start's do-loop sees counter = 42, ratio = 7.1
```
```sweet-exp
defimage-global counter  :int   0       ; T must have a Serializable instance
defimage-global greeting :cstr  "cold"
defimage-global ratio    :float 1.5

defn do-init [] : int
  set!(counter 42)                       ; written on the cold start only ...
  set!(ratio 7.1)
  0

defn main [] : int
  image/track-globals!(counter greeting ratio)   ; at the TOP of main
  with-image-cache-after-init("/var/cache/app.img" do-init do-loop)
  ;; ... and a warm start's do-loop sees counter = 42, ratio = 7.1
```

`(defimage-global name :T initial)` is `(def ^mut name :T initial)` plus three
generated defns: `name/image-ser` and `name/image-deser` (the global's
`Serializable` instance for `T`, monomorphically) and `name/image-track!`, which
registers the pair by name. **Tracking is not automatic**: compiled top-level
forms do not execute, so `(image/track-globals! name ...)` goes at the top of
`main`, before the cache call -- the same rule as the hooks below, and for the
same reason (it has to run on the warm start too). `image/global-count` tells
you how many are registered.

On save, the registry is snapshotted through each global's `serialize` into a
**second TSER section** written after the continuation bytes; the header's
`globals_offset` locates it and `payload_len` covers both. On load, the section
is restored through each `deserialize` **before** the continuation resumes, so
the resumed `loop` reads the post-init values. A record whose name is not
tracked in the loading process is skipped; a tracked global absent from the
image keeps its current value. An image with no globals has `globals_offset`
0 and reads exactly as before.

### `TUR-W0706`: an `init` write that the image will not carry

The compiler lints the cold-start root of every `with-image-cache-after-init`
call: if `init` -- or any named function it calls -- does `set!` on a top-level
global that no `defimage-global` declared, it warns at the call:

```
warning [TUR-W0706]: `do-init` writes the global `hidden`, which is not an
image global: the image holds the captured continuation, not the heap, and
init is skipped on a warm start, so the write is silently absent after load
-- declare it with (defimage-global hidden :T initial) and track it at the
top of main, or thread the value through the captured continuation
```

The walk is the same one `#writes` frames use (transitive over named callees;
a call through a function-typed parameter is opaque and does not warn). Only
`init` is linted: `loop` runs on both paths, so its writes are not a warm-start
hazard. The single-body `with-image-cache` takes a closure and is not linted --
prefer the after-init combinator when you have mutable globals.

## Resources: the reload-hook pattern

OS-handle-backed values (files, sockets, GPU contexts) cannot be in the image.
The application must re-acquire them after load. Two hook kinds bracket the
image lifecycle:

- a **reload hook** runs on a warm start, after a valid image is selected and
  *before* the captured continuation resumes user code -- the place to re-open
  files, re-bind sockets, or re-establish GPU contexts;
- a **finalize hook** runs on a cold start, immediately *before* the image
  bytes are written -- the place to flush buffers or drop transient state that
  should not be captured.

Both are registered with `image/register-reload-hook!` /
`image/register-finalize-hook!` and run in registration order. The
`with-image-cache` combinators and `save-image!` / `load-image!` invoke them
for you at the right point.

```turmeric
(defimage-reload-hook reopen-log
  (do (reopen-the-log!) 0))            ; defines (defn reopen-log [] :int ...)

(defimage-finalize-hook flush-log
  (do (flush-the-log!) 0))

(defn main [] : int
  (image/register-reload-hook!   reopen-log)   ; install BOTH hooks at the top
  (image/register-finalize-hook! flush-log)    ; of main -- see the note below
  (with-image-cache-after-init "/var/cache/app.img" expensive-init main-loop))
```
```sweet-exp
defimage-reload-hook reopen-log
  do
    reopen-the-log!()
    0            ; defines (defn reopen-log [] :int ...)

defimage-finalize-hook flush-log
  do
    flush-the-log!()
    0

defn main [] : int
  image/register-reload-hook!(reopen-log)   ; install BOTH hooks at the top
  image/register-finalize-hook!(flush-log)  ; of main -- see the note below
  with-image-cache-after-init("/var/cache/app.img" expensive-init main-loop)
```

> **Register at the top of `main`, not inside `init`.** A compiled Turmeric
> program runs only `main` (top-level forms do not execute), so hooks cannot
> self-register at load time -- `defimage-reload-hook` is sugar for a named
> `defn`, not a registration. And a *warm* start skips `init` entirely, so a
> hook registered there would be missing on exactly the run that needs the
> reacquisition. Install hooks before the with-image-cache call so they are
> present on both paths.

### The standard-hooks library

`stdlib/image_hooks.tur` packages the common cases so you do not hand-roll the
reopen logic. The tracked-file table is the workhorse: declare files during
init, install the reopen hook once, and read fresh handles back after a warm
resume.

```turmeric
(load "stdlib/image.tur")
(load "stdlib/image_hooks.tur")

(defn do-init [] : int
  (image-hooks/track-file! "/var/run/app.sock" "r+")   ; declare what to reopen
  0)

(defn do-loop [] : int
  (let [h (image-hooks/slot-handle 0)]                 ; live FILE* after resume
    (read-through h)))

(defn main [] : int
  (image-hooks/use-reopen-tracked!)   ; reload hook: reopen every tracked file
  (image-hooks/use-flush-stdio!)      ; finalize hook: fflush stdout/stderr
  (with-image-cache-after-init "/var/cache/app.img" do-init do-loop))
```
```sweet-exp
load "stdlib/image.tur"
load "stdlib/image_hooks.tur"

defn do-init [] : int
  image-hooks/track-file!("/var/run/app.sock" "r+")   ; declare what to reopen
  0

defn do-loop [] : int
  let [h image-hooks/slot-handle(0)]                 ; live FILE* after resume
    read-through(h)

defn main [] : int
  image-hooks/use-reopen-tracked!()   ; reload hook: reopen every tracked file
  image-hooks/use-flush-stdio!()      ; finalize hook: fflush stdout/stderr
  with-image-cache-after-init("/var/cache/app.img" do-init do-loop)
```

On a cold start the slot handle is `0` (the reload hook does not run); on a warm
start the hook reopens each tracked path and `image-hooks/slot-handle` returns
the fresh `FILE*` (as an int handle to cast in inline-C).

## Build-stamp safety

Loading an image written by a *different* binary is undefined behavior (frame
reconstruction maps names to this binary's functions). The build stamp catches
this cheaply at load time.

The stamp is a **SHA-256 of the running executable**, computed at load time
(via `/proc/self/exe` on Linux). Same binary -> same stamp; a different binary
-> a different file -> a different stamp -> the image is rejected cleanly (a
`0` return / cold-start fallback), never resumed as garbage.

> This differs from the plan's AI4.1 linker-section stamp (a two-pass build
> embedding the digest in a `.tur_build_stamp` section). Hashing the running
> executable delivers the same *safety* contract without platform-specific
> linker magic. It does **not** guarantee a byte-identical stamp across two
> independent builds of the same source (a reproducible-build nicety, not a
> safety property); two builds produce two binaries, so an image from build A
> will not load under build B -- which is exactly the safety behavior we want.

Inspect and validate images from the CLI without resuming them:

```sh
tur image-info   app.img            # print magic/version/stamp/payload-len/...
tur image-verify app.img            # structural check (magic/version/CRC)
tur image-verify app.img ./myapp    # also require the stamp to match ./myapp
```

`tur image-verify` exits `0` on success, `1` on a build-stamp mismatch, and
`2` on a structural failure (bad magic/version/CRC or unreadable file).

> **Dynamic libraries (AI4.4):** the stamp covers the main executable only. If
> the program `dlopen`s spice plugins or other `.so`s, the stamp does not see
> them; dynamic-spice + image-dumps is a follow-up. Treat images from a
> `dlopen`-ing binary with extra caution.

> **The `--unsafe-image-skip-build-check` escape hatch (AI4.3):** the stdlib
> path supports skipping the stamp check by passing `stamp = 0` to
> `image/load-resume-file!`. **Do not use this in production** -- it removes the
> only guard against resuming a foreign image. The matching `tur run --image`
> CLI flag (plan AI6.1/AI6.2) is not yet implemented.

## Security

**Loading an image is equivalent to `eval`** -- it resumes captured code. The
build-stamp check prevents *accidental* mismatch; it does **not** make a
malicious-but-stamp-matching image safe. Treat image files as trusted
executables:

- Store them under the same trust boundary as the binary (`/var/cache/app/`
  owned `root:root`; `~/.cache/app/` owned by the user).
- **Never** load an image from an untrusted source (HTTP, user upload, shared
  `/tmp`).

Signed images (an `Ed25519` signature in the header) are possible future work
if fleet warm-start becomes a use case.

## Prior art

| System | Mechanism | Cross-binary? |
|---|---|---|
| SBCL `save-lisp-and-die` | dump whole heap | pinned to the runtime |
| CCL `save-application` | dump whole heap | pinned |
| Smalltalk image save | dump object memory | pinned to the VM |
| Emacs `pdumper` | portable dump of post-init heap | pinned to the binary |
| Java CRaC | checkpoint/restore via CRIU | pinned |
| Linux CRIU | whole-process checkpoint | pinned |
| **Turmeric `tur/image`** | **serializable continuation + framed header** | **pinned via build stamp** |

Turmeric's twist: instead of dumping the whole heap, it captures a single
typed, `Serializable`-checked continuation at a quiescent point the application
chooses -- so the captured state is exactly what the typechecker allows to
cross the boundary.

## See also

- `docs/guides/serializable-continuations-guide.md` -- the Phase 21 substrate.
- `docs/archive/history/application-image-dumps-plan.md` -- the full plan (AI0--AI8),
  including the phases not yet implemented (AI6.1 `tur run --image`, perf
  benchmarks).
- `src/runtime/image.h` -- the canonical header layout; `tests/image_unit.c`
  pins it.
