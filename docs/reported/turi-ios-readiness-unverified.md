# The interpreter is the only iOS engine, and nothing checks it can run there

**Severity: medium (platform readiness)** -- nothing is broken today, because
no iOS target exists. The risk is that one is planned
([godot-binding-ios-plan](../upcoming/hold/godot-binding-ios-plan.md):
"ship interpreter mode only"), the interpreter is the only engine that plan
can use, and since the JIT became the default build (v0.60.0, `923b803a`) no
CI leg builds the interpreter the way iOS would need it. Anything turi picks
up that assumes a JIT, a subprocess or `dlopen` reaches `main` unnoticed.

**Status: OPEN -- research.** Filed 2026-10-04 while reviewing whether the
default JIT build puts the tree-walking interpreter at risk of rot. Nothing
here has been compiled against an iOS SDK. Every claim about Apple's SDK below
is marked as unverified and is the first thing to check.

## Why iOS is turi-only

- **JIT:** [jit-binaries-design](../design/jit-binaries-design.md) (Platform
  policy row) says a JIT binary is "**impossible on iOS**", and the MIR
  interpreter tier is not a way around it: it still publishes shims through
  the `MAP_JIT` allocator ([jit-guide](../guides/jit-guide.md), "We do not ship
  the MIR interpreter").
- **cc / AOT:** the iOS plan rules out shipping per-script `.dylib`s. A
  static-link-at-export route exists only as a research follow-up.
- So `libturi.a`, the tree-walker in `src/turi/`, is the engine iOS gets. The
  web REPL (`libturi` under Emscripten) is the one other no-JIT target, and it
  runs `src/turi/` too.

## What would break or be unavailable, by area

Each item gives what the code does now, then what iOS (probably) needs.

### 1. Build configuration turns the JIT on for iOS

`CMakeLists.txt:153-157` sets `TUR_JIT` ON for any non-Emscripten 64-bit
`x86_64`/`aarch64`/`arm64` processor. An iOS cross-configure is arm64, so it
gets the JIT by default and pulls in `cmake/mir.cmake`, `jit_engine.c` and
`src/turi/jit_ffi.c`. That code would compile, but every use of it would fail
at runtime on a device.

**Fixed 2026-10-04:** `_tur_jit_default` now excludes
`CMAKE_SYSTEM_NAME` iOS/tvOS/watchOS/visionOS, the same way `EMSCRIPTEN` is
excluded, and `nightly-ios.yml` checks that an iOS configure leaves
`TUR_JIT:BOOL=OFF` in its cache.

### 2. Coroutines and effect handlers use `ucontext`

`src/turi/eval.c` (the includes near the top), `src/turi/env.h:22-27` and
`src/turi/fiber.h:48-53` include `<ucontext.h>` on every non-Windows,
non-Emscripten host, and use `getcontext`/`makecontext`/`swapcontext` for
fibers, generators and the effect-continuation paths that still use a
separate stack. On macOS these are already deprecated: `eval.c` silences
`-Wdeprecated-declarations` around the include.

**Unverified:** whether the iOS SDK declares these functions at all (or marks
them unavailable), and whether they work on a device if it does. Check this
first; it decides whether item 2 is a warning or a blocker.

**Fix direction:** turi already has two non-ucontext backends behind the same
names -- Win32 Fibers (`src/platform_ucontext_win.h`) and Emscripten fibers
(`src/turi/fiber.h:58-82`). A third over the hand-written arm64 context switch
the compiled runtime already ships (`src/async/fiber_ctx_arm64.S`,
`tur_ctx_swap`) needs no new platform API.

### 3. Spice loading builds a dylib in a subprocess and `dlopen`s it

`src/turi/spice_loader.c:366/374` runs `system()` to invoke
`tur build --shared`, then `dlopen`s the result (`spice_loader.c:788`). iOS
has no `tur` binary to call and does not allow loading a dylib built at
runtime. **Unverified:** whether `system()` is even declared in the iOS SDK
(it is believed to be marked unavailable).

**Fix direction:** on iOS, spices have to be linked statically into the app
and registered as natives when the app starts. That is the same shape as the
iOS plan's static-AOT follow-up, so the two should be designed together.
Until that exists, compile spice auto-discovery out on iOS rather than failing
at runtime.

### 4. User FFI: `dlopen`/`dlsym` and the shape table

- `BS_DLOPEN`/`BS_DLSYM` under `--interpret` call real `dlopen`
  (`src/turi/eval.c:4694-4699`). On iOS, only the main image and
  system frameworks can be opened. Calling through `dlsym(RTLD_DEFAULT, ...)`
  into symbols statically linked into the app should still work, but that is
  unverified.
- Without the JIT there is no c2mir thunk (`src/turi/jit_ffi.c`), so dynamic
  calls fall back to the generated dispatcher in `ffi_thunk.c`. It covers
  argument shapes up to its `--max-arity` (6 by default,
  `src/turi/ffi_thunk.c:275`); a wider shape fails with "no registered
  dispatcher for shape". That limit becomes the iOS FFI limit.

### 5. Inline-C bodies have no path

The interpreter cannot run a user inline-C body. Its only remedy,
`--enable=repl-jit-inline-c` (C1, `src/turi/inline_c_jit.c`), compiles the
body with MIR. On iOS an inline-C `defn` is therefore simply not callable.
`tests/run-turi.sh` already skips every fixture whose program contains user
inline-C, so the suite does not measure this gap -- see "Finding the turi
gaps" below.

### 6. `process/spawn` forks

`src/turi/interpreter_natives.c:5026-5028` implements `process/spawn` with
`fork()` + `execvp`. Sandboxed iOS apps cannot fork. The native is already
behind the `TURI_CAP_PROC` capability (`src/turi/native_caps.c:304`), so the
fix is to leave that capability out of an iOS build and make the native report
a clear "unsupported on this platform" error.

## What is not a problem

- Fiber stacks are allocated with `mmap` read/write only; nothing in
  `src/turi/` maps executable memory (`grep PROT_EXEC|MAP_JIT src/` finds only
  the Windows mman shim and the JIT engine's own header).
- The trampolined evaluator (work stack on the heap, `DK_*` frames) has no
  platform dependency beyond the item-2 fibers.

## Finding the turi gaps

The list above comes from reading the code. To get one that is complete:

1. **A no-JIT build in CI -- landed 2026-10-04.** `ci.yml`'s `interp-nojit`
   job (Linux, every PR) configures `-DTUR_JIT=OFF`, checks the engine really
   is absent, and runs `tests/run-turi.sh`. Its first local run, on `main` at
   `1846d17f`: `2581 passed, 0 failed, 930 skipped of 3511 discovered`.
2. **Track the turi skip count -- partly landed.** The `interp-nojit` job
   writes the summary line (with S) to its job summary. Nothing fails yet when
   S grows; a limit is the next step once a few runs give a baseline. Growth
   in S is how gaps (items 4-5) hide.
3. **Cross-compile smoke test -- landed 2026-10-04.** `nightly-ios.yml`
   configures for `CMAKE_SYSTEM_NAME=iOS` against the device SDK, compiles
   `libturi` with `make -k`, and lists every distinct compiler error in the
   job summary. This settles items 2, 3 and 6 from the compiler instead of
   from memory. It is its own workflow, not a job in `nightly-arm64.yml`, so
   that its expected early red does not hide a red representation suite.
   Expect it red until the items above are worked.
4. **Simulator run (later).** Embed `libturi` in a minimal app or XCTest
   target and run a few dozen `run-turi.sh` fixtures through `turi_eval`. The
   simulator does not enforce every device rule (code signing, W^X), so this
   comes after (3) and does not replace a TestFlight check.

## Related

- [godot-binding-ios-plan](../upcoming/hold/godot-binding-ios-plan.md) -- the
  consumer; its success criterion is an interpreter-mode script on TestFlight.
- [turi-parity-guide](../guides/turi-parity-guide.md) -- where the
  interpreter is known to differ from compiled output.
- [jit-binaries-design](../design/jit-binaries-design.md) -- why the JIT
  cannot be the answer on iOS.
