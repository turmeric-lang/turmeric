# Research spike: should turmeric-godot use the JIT instead of shelling out to `cc`?

> **QUESTION 1 ANSWERED YES, 2026-09-08, ON WINDOWS.** The spike says to try
> Linux or macOS first "where the JIT is known to work". Windows turned out to
> be both the harder case and the one where the answer matters most, so it went
> first. All three legs pass.
>
> Measured with a stand-in plugin rather than the real shim -- a DLL that links
> libturi exactly as the GDExtension does, loaded with `LoadLibrary` by a host
> that stands in for Godot. That isolates "can the JIT work inside a
> dynamically-loaded module" from "does godot-cpp build", which is a 20-minute
> question with a different answer.
>
> ```
>   [plugin] compile rc=0
>   JIT-compiled go() = 42
> ```
>
> 1. **A JIT-enabled `libturi` links into a plugin.** One requirement the shim
>    does not meet today: `libturi.a` carries `U MIR_gen` / `U MIR_link`, so
>    `libtur_mir.a` must join the link line. `SConstruct` links only `libturi.a`
>    and has no option for a second archive.
>
>    Note the trap on the way: linking *succeeds* without `tur_mir` as long as
>    nothing references the JIT, because a static archive contributes only the
>    members something needs. A shim that links a JIT-enabled libturi but never
>    calls the embedding API is green and JIT-less.
>
> 2. **MIR survives being inside a dynamically-loaded module.** The plugin
>    compiled C in-process and ran it. No W^X or executable-memory problem
>    appeared on Windows -- which answers the Windows half of question 2 and
>    leaves the macOS hardened-runtime half open.
>
> 3. **JIT'd code calls an exported symbol in the plugin.** This is the one that
>    matters for the natives. The compiled source was
>    `extern int godot_answer(int); int go(void){ return godot_answer(21); }`,
>    `godot_answer` was `__declspec(dllexport)` in the plugin, and it returned
>    42. `MIR_link` resolved it through `jit_import_resolver` ->
>    `dlsym(RTLD_DEFAULT)` ([jit_engine.c:424](../../src/jit_engine.c)).
>
>    **So the AOT route's import-library problem does not exist here.** A PE DLL
>    cannot link with unresolved symbols, so the staged-subprocess route needs
>    the extension's DLL on its link line (MinGW `ld` links against a DLL
>    directly; godot-cpp also emits a `liblib<name>.a` implib, unconsumed). The JIT route
>    has no link step to fail: it needs the entry points EXPORTED, which is one
>    attribute or link flag.
>
> ### What this does NOT show
>
> - Not inside Godot. No engine, no `.gdextension`, no script lifecycle. It
>   shows the mechanism, not the integration.
> - Not the real shim -- a stand-in plugin with one exported function.
> - Windows only, Release libturi only. The macOS `MAP_JIT` / hardened-runtime
>   question (spike question 2) is untouched, and J5 in
>   [godot-binding-refresh-plan.md](../archive/godot-binding-refresh-plan.md)
>   records that MIR's interpreter tier is *not* an escape hatch there.
> ### Question 4 answered too: the cache is not needed on this route
>
> The spike says question 4 "turns entirely on" measured compile time, so it was
> measured. Windows, Release `-DTUR_JIT=ON` build, warm:
>
> | | JIT (`tur jit`) | cc path (`tur build`) | cc + `--runtime=split` |
> | --- | --- | --- | --- |
> | 6-line program | **103 ms** | 1378 ms | 1208 ms |
>
> Roughly **13x**, and that understates it: the shim's AOT path adds staging --
> creating the cache tree, writing `build.tur` and the source -- plus a
> **subprocess spawn**, which CLAUDE.md records as costing about 50x more on
> Windows than on Linux. The 1378 ms is `tur build` invoked directly, with none
> of that.
>
> The more useful number is how it scales, because a Godot script is not six
> lines:
>
> | program | lines | JIT total |
> | --- | --- | --- |
> | `bench` | 6 | 103 ms |
> | `structural-eq` | 438 | 95.5 ms |
> | `hkt-stdlib-suite` | 722 | 96.5 ms |
>
> Essentially **flat**. c2mir is 84% of the time (86 ms of 103) and the runtime
> preamble is the bulk of what it parses, so compile cost is dominated by a
> fixed cost rather than by the script. A 722-line program is not measurably
> dearer than a 6-line one.
>
> At ~100 ms per load the on-disk cache stops earning its keep: that is below
> perceptible for a script load, and comparable to what the cache's own hash,
> stat and `dlopen` would cost. Deleting `aot_cache.cpp`'s staging, hashing and
> subprocess machinery would remove a great deal of surface area, including all
> of its platform-specific parts -- which is what the spike hoped for.
>
> **Caveat that keeps this from being a licence to delete:** iOS cannot JIT and
> Web has no JIT, and per J4 in the refresh plan those platforms cannot use the
> `dlopen`-based AOT path either -- their fallback is the interpreter. So the
> cache is a desktop-scoped question, and "when is it bypassed" remains a better
> framing than "delete it".
>
> ### Method note
>
> The first attempt returned NULL and looked like a JIT failure. It was not: I
> had declared `tur_jit_compile_image` with an invented 5-parameter signature
> against a real 6-parameter one. C linkage does not check signatures, so it
> linked cleanly and passed garbage. That is exactly the silent ABI mismatch the
> native signature table exists to prevent -- committed here by hand, in a probe
> written to investigate it.

**Summary:** The Godot GDExtension compiles every `.tur` script by staging a
transient project and running `tur build --shared` as a **subprocess**, which
means every machine that runs a Turmeric-scripted Godot game needs a working C
toolchain. `libturi` can now carry the MIR JIT and already exposes an embedding
API. Spike whether the shim should compile in-process instead.

**Severity:** Enhancement / research. The AOT path works; this is about whether
the product is shippable to non-developers.

**Type:** Timeboxed research spike. Platform-independent motivation, but see the
Windows note -- that is where it goes from "nice" to "load-bearing."

---

> ### MEASURED 2026-09-09 -- question 4 has a number, and question 5 has an answer
>
> From the AOT-natives work in
> [godot-aot-staged-build-lacks-godot-natives.md](godot-aot-staged-build-lacks-godot-natives.md),
> which made a real script AOT-compile end to end for the first time. Measured on
> macOS arm64 with `tur` v0.46.0 and Godot 4.3, not reasoned about.
>
> **Question 4 -- "measure compile time for a representative script".**
> `examples/paddle-pong-tur/scripts/ball.tur` (34 lines of gameplay) stages to
> 669 lines of Turmeric, because the declarations module carries the `godot-*`
> `extern-c` prototypes plus the baked-in prelude. That emits **10,364 lines of
> C** and a full `tur build --shared` -- fork, elaborate, emit, invoke `cc`,
> link -- takes **0.50 s wall, stable across three runs.** Per script, cold.
>
> So the on-disk cache cannot be deleted on the strength of speed alone: half a
> second per script at load is fine for one script and is 25 s for fifty. But
> most of that 0.50 s is the *declarations module*, not the user's code -- the
> user's 34 lines are 4,751 of the 10,364 emitted lines and the shared
> `tg-godot` module is the other 5,610. An in-process route that compiles the
> declarations once per process instead of once per script attacks exactly the
> dominant term, which the subprocess route structurally cannot.
>
> **Question 5 -- "replacement or second path?"** The AOT work settles part of
> this: whatever compiles, compiles the *same source*. The declarations module
> and the script rewriting in `aot/aot_natives.cpp` are route-independent -- the
> JIT would elaborate exactly that text, because compiled-mode elaboration is
> what makes the declarations necessary in the first place (see the 2026-09-07
> finding at the top of the AOT report). A JIT path therefore reuses the staging
> layer wholesale and replaces only `std::system("tur build --shared")` +
> `dlopen` with `tur_jit_compile_image` + `tur_jit_image_sym`. That is a much
> smaller second path than this spike assumed when it wrote "two paths mean two
> things to keep correct".
>
> **Question 3 is now load-bearing, not incidental.** `__tur_static_init` is
> what the emitted `__attribute__((constructor))` calls, and the staged library
> genuinely uses it -- `__tur_fatbox_init()` and `atexit(tur_region_shutdown)`
> live there. c2mir discards `constructor`, so an in-process route must call it
> explicitly or the image's fat-closure boxes are never filled.
>
> **Still unanswered by this work:** questions 1 (does MIR survive inside a
> Godot plugin), 2 (W^X in a host process), and 6 (fixture-level JIT
> correctness). Nothing here touched the JIT.

## The problem with the current design

`aot_cache.cpp` in `../turmeric-godot` does this per script:

1. Stage `<project>/.godot/turmeric-cache/<hash>/` with a `build.tur` and a copy
   of the source.
2. `std::system()` a `tur build --shared ...` command line.
3. `dlopen` the resulting shared library and `dlsym` each export from the
   manifest.

Step 2 is the problem. It requires, on the end user's machine:

- the `tur` binary,
- a C compiler that `tur` can invoke,
- and on Windows, a whole MSYS2/UCRT64 installation.

That is a reasonable ask of a *developer* and an unreasonable one of anyone who
just wants to run a game. It also puts a process spawn and a full C compile on
the script-load path, which is why the cache exists at all.

The JIT removes the external compiler: c2mir is vendored into `libturi`, which
the GDExtension **already statically links**.

## What already exists in our favour

- **`libturi` propagates the JIT to embedders.** Under `-DTUR_JIT=ON` the
  library gets `tur_mir` on its link line and `TUR_HAVE_JIT=1` in its
  *public* preprocessor interface
  ([src/CMakeLists.txt:535](../../src/CMakeLists.txt)) -- explicitly so "an
  embedding host gets MIR on its link line ... so a host can write one `#ifdef`
  and fall back to the tree-walking interpreter when the library was built
  without a JIT." The GDExtension is exactly that host.
- **There is a supported embedding API.** `tur_jit_compile_image` /
  `tur_jit_image_sym` / `tur_jit_image_free`, with `tests/turi/jit-embed.c` as
  a worked example that cross-checks `turi_eval` against a JIT'd image
  ([docs/guides/jit-guide.md](../guides/jit-guide.md), "Embedding").
- **The shapes line up.** `tur_jit_image_sym` is the natural replacement for
  `dlsym` in `aot_image.cpp`, which already models an image as "a handle plus a
  table of resolved symbol pointers" -- see `AotImage` in
  `../turmeric-godot/src/aot/aot_image.h`. The dispatch layer above it
  (`aot_dispatch`) should not need to care which produced the pointer.

## Open questions the spike must answer

1. **Does the GDExtension still link and load with a JIT-enabled `libturi`?**
   Cheapest possible first step: rebuild `libturi` with `-DTUR_JIT=ON`, relink
   the shim, load it in Godot. Answers "does MIR survive being inside a Godot
   plugin" before any code is written.
2. **W^X inside a host process.** A JIT allocating RWX pages inside an
   application the JIT does not control is a different proposition from doing it
   in `tur`. On macOS this is the MAP_JIT/hardened-runtime question (the plan
   notes the arm64 macOS MAP_JIT gate is closed for `tur` --
   [jit-engine-plan.md:70](../upcoming/jit-engine-plan.md) -- but Godot ships
   with its own entitlements and codesigning). On Windows it is the EDR /
   antivirus question. Neither is answered by `tur`'s own JIT working.
3. **`constructor` attribute.** c2mir discards it, so the embedding path must
   call `__tur_static_init` explicitly (the guide says so outright). Confirm the
   shim's init ordering has somewhere sensible to do that.
4. **What happens to the cache?** If compiles are in-process and fast, the
   on-disk cache may become unnecessary -- the REPL's JIT path already takes
   this position: "Every load compiles fresh, which is the point -- there is no
   cached artifact to go stale" ([jit-guide.md:310](../guides/jit-guide.md)).
   Deleting `aot_cache.cpp`'s staging/hashing/subprocess machinery would remove
   a great deal of surface area, including all of its platform-specific parts.
   Measure compile time for a representative script before assuming this.
5. **Is this a replacement or a second path?** A shipped game wants JIT (no
   toolchain); an editor session may still want AOT (debuggable artifacts, and
   the JIT is opt-in at build time so a `libturi` without it must still work).
   Two paths mean two things to keep correct -- decide deliberately, and note
   that `TUR_HAVE_JIT` was designed for exactly this fallback shape.
6. **Does the JIT reach the same fixture-level correctness?** `tests/run-jit.sh`
   is the whole-corpus JIT run; the archived findings list real JIT-only
   divergences (reactor fixtures aborting under MIR, GC/RC/weak fixtures on
   macOS). Scripts in Godot are not fixtures, but the failure classes carry.

## The Windows intersection

On Linux and macOS this spike buys convenience. On Windows it is close to
load-bearing: requiring MSYS2 to run a Godot game is not a shippable story, and
the AOT path there depends on the most fragile platform code in the shim
(`cmd.exe` quoting, `std::system` exit-code decoding, `.dll` cache naming) --
all of which the JIT path would delete rather than fix.

But it compounds risk rather than avoiding it: it needs a Windows JIT to exist
at all. Sequence [jit-windows-support-spike.md](../archive/jit-windows-support-spike.md)
first, or at minimum read its question 1 before planning anything Windows-shaped
here. That spike's questions 3 and 4 (emitted `__asm__`, executable memory) hit
this path unchanged.

## Method

- Do question 1 on Linux or macOS first, where the JIT is known to work. It is a
  rebuild and a relink, and it either loads or it does not.
- Only then attempt a single script end-to-end through
  `tur_jit_compile_image` + `tur_jit_image_sym`, bypassing `aot_cache` entirely
  -- a scratch branch in `../turmeric-godot`, not a refactor of the real path.
- Record findings; do not land a dual-path design out of the spike.

## Exit criteria

A verdict on whether in-process JIT compilation is (a) viable in a Godot host,
(b) fast enough to drop the on-disk cache, and (c) worth maintaining alongside
or instead of the AOT path. Plus a measured script compile time, since question
4 turns entirely on it.

## Related

- [jit-windows-support-spike.md](../archive/jit-windows-support-spike.md) -- sequence that
  one first for anything Windows-shaped.
- [docs/guides/jit-guide.md](../guides/jit-guide.md) -- "Embedding" section.
- [docs/upcoming/windows-remaining-plan.md](../upcoming/windows-remaining-plan.md) -- WIN2.
