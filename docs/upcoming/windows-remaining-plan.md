# Windows Support -- Remaining Work

> **Status:** WIN0 (compiler/runtime), WIN1 (generated-code portability), the
> hard core of WIN3 (async I/O + fiber context switches) and WIN2's milestone
> (the turmeric-godot GDExtension loads in a stock Godot on Windows,
> 2026-08-04) are done and on `main`. Checked 2026-10-18:
>
> - **The `windows-bringup` branch is gone.** It was squash-merged once as
>   `7a16ef1de` ("Windows Bringup (#682)"), merged up from `main` twice, and
>   merged into `main` for good on 2026-09-04 (PR #805), carrying the JIT
>   bring-up, the `__builtin_setjmp` fiber fix, the `-ldl` autolink fix and
>   the CI change below. Windows work happens on `main`; the `windows` job in
>   `.github/workflows/ci.yml` runs the fixture suite.
> - **WIN2's AOT path has run.** The "never run" note under WIN2 predates
>   2026-09-10, when Windows was where AOT was first driven end to end
>   (turmeric-godot#4 and `88197f74f`; see
>   [godot-binding-refresh-plan](../archive/godot-binding-refresh-plan.md)).
>   The AOT gaps left are not Windows-specific and are tracked in
>   [godot-aot-staged-build-lacks-godot-natives](../reported/godot-aot-staged-build-lacks-godot-natives.md).
> - **Open:** the shim-toolchain question under WIN2; `-Werror` is still off
>   for Windows builds (`src/CMakeLists.txt`); the pipe-fd reactor fixtures
>   stay skipped behind `requires.posix-apis`, since Windows `select()` cannot
>   poll pipes; WIN4 is deferred; the `spices/lmdb` Windows assessment
>   ([below](#lmdb-spice----windows-assessment)) has not been run.

`tur.exe` builds under MSYS2/UCRT64, and `tur build` compiles and runs real
programs. Measured on `windows-bringup` with gcc 16.1.0, Debug:

```
TUR=./build-win/tur.exe bash tests/run.sh
# summary: 2781 passed, 0 failed
```

**Do not trust a green local run until it is green on a machine that has never
run the suite.** That number was first measured on a box that happened to have
`C:	mp` left over from earlier runs; the same commit came back `2769 passed, 12
failed` on a clean CI runner whose workspace is on `D:`. A dozen fixtures (and
stdlib's `fs/tmpfile`) spell `/tmp/...` literally, and a NATIVE Windows binary
resolves that against the current drive root, not the MSYS shell's `/tmp`.
`tests/run.sh` now provisions `<drive>:	mp`; the real fix is tracked in
[windows-hardcoded-tmp-resolves-to-drive-root](../archive/windows-hardcoded-tmp-resolves-to-drive-root.md).

Run the suite against a **Debug** build. A Release `tur` compiles out contract
checks (`rt_contracts_emitted` is `#ifdef NDEBUG`), so every fixture pinning a
contract panic fails against it with the wrong runtime error -- which looks
exactly like a product regression and is not one.

The remaining known-bad fixtures PASS-skip behind markers rather than sitting
red, so a new failure is unambiguous:

| marker | what it covers |
| --- | --- |
| `requires.posix-apis` | POSIX-only APIs (11 fixtures) -- pipe-fd reactor family, `childhandle`, `term-raw-cooked` |

**WIN0 regressed between the merge and 2026-07-31 and had to be re-fixed.** Five
independent breaks accumulated, three of them within five days, because nothing
guarded Windows in CI. The compiler did not build at all. See
`docs/reported/windows-*.md` and the `fix(windows): restore the Windows build on
main` commit.

**That guard now exists, and as of this branch it runs the fixture suite, not
just the build.** `.github/workflows/ci.yml` has a `windows` job (MSYS2/UCRT64).
It was build-only by deliberate choice while ~54 fixtures were failing -- a
permanently-red job teaches people to ignore it -- with the stated exit
condition "turn the suite on here once those classes are closed". They are
closed, so it is on.

Build-only was not sufficient, and the gap was not hypothetical: `-ldl` (Windows
has no libdl) reached main in three new `jit-ffi` fixtures and the job stayed
green through it, because that failure is a LINK error when building a FIXTURE,
not when building `tur.exe`.

Still missing: `src/CMakeLists.txt` disables `-Werror` on Windows pending a
warning-clean port, so the job runs with warnings unpromoted (as of 2026-09-26
a Debug build still warns, e.g. `signal_shim` defined but unused in
`src/async/reactor.c`). `release.yml` now has a Windows job; the earlier "ships
no Windows artifact" is no longer true.

This plan tracks what is left.

The original end-to-end plan (toolchain rationale, the WIN0/WIN1 blockers, the
Wine loop, the full WIN3 investigation) is archived at
[docs/archive/windows-support-plan.md](../archive/windows-support-plan.md).

**Toolchain:** MinGW-w64 / UCRT64 via MSYS2 (`C:\msys64`), not Clang-cl. Build
with `just build-windows`; run the suite with
`TUR=./build-win/tur.exe bash tests/run.sh` (it derives the libturi `-L` from
`$TUR` and exports `TUR_BIND_LOOPBACK=1` so socket fixtures don't trip the
firewall).

---

## WIN2 -- `turmeric-godot` GDExtension Windows build (THE north star -- LOADS)

**Goal:** build the shim from `../turmeric-godot/` as a Windows `.dll` and load
it in a stock Godot 4 binary. This is the actual point of Windows support -- the
compiler/runtime work exists to serve it.

**Reached 2026-08-04.** The shim builds as
`libturmeric-godot.windows.template_debug.x86_64.dll` and initializes in a stock
Godot 4.3.stable:

```
$ scons platform=windows arch=x86_64 target=template_debug use_mingw=yes
$ Godot_v4.3-stable_win64.exe --headless --editor --path examples/spike --quit
[turmeric-godot] initialize(level=2)
[turmeric-godot] ctor called
[turmeric-godot] registered Turmeric script language + resource format
[turmeric-godot] initialize(level=3)
```

Startup and shutdown are both clean (exit 0, full uninitialize sequence). The
link used `-Wl,--no-undefined`, so nothing is left unresolved.

Three notes for whoever picks this up:

- **Use `--editor`, not plain `--headless --path`.** Without it Godot goes into
  run mode, bails with "no main scene defined", and never loads the extension at
  all -- which reads exactly like a load failure. The spike project's own header
  comment still recommends the plain form.
- **Headless Godot 4.3 hangs at teardown on this box**, with
  `Pages in use exist at exit in PagedAllocator`. Verified as NOT ours: an empty
  project with no GDExtension hangs identically. Wrap runs in a `timeout`.
- **One error remains**, and it is an ordering problem rather than a load
  failure: `[turmeric-godot] TurmericEditorSyntaxHighlighter class not
  registered; is the GDExtension loaded?` is pushed *before* `initialize(level=2)`.
  The editor plugin in `addons/turmeric-godot-editor` runs ahead of the
  extension's EDITOR-level registration. Not known to be Windows-specific --
  untested on Linux/macOS.

What is NOT yet demonstrated: **the AOT path has never run.** Loading the
extension does not compile a `.tur` script, so the Windows-specific work in
`aot_cache.cpp` (cmd.exe quoting, `std::system` exit decoding, `.dll` cache
naming) is still unexercised. That needs a project that actually attaches a
Turmeric script.

### Prerequisite in this repo: shared-library output naming -- DONE

`tur build --shared` emits `<name>.dll` (no `lib` prefix) on Windows.
`TUR_SHLIB_PREFIX` / `TUR_SHLIB_EXT` in `src/platform_fs.h` are the single
source of truth; `src/driver/project.c` (default output path) and
`src/turi/spice_loader.c` (the `.tur-repl-cache/` image) both build their names
from them. macOS deliberately keeps `lib<name>.so` -- see the header comment.

`tests/run-build-shared.sh` passes 11/11 against `build-win/tur.exe`: the .dll
links, exports `smokelib__add42`, dlopens through `src/platform_dl.h`, calls,
and writes `exports.manifest`. Three fixes were needed to get there:

- **Multi-TU collision in the emitted ucontext shim.** The WIN3-C register-
  snapshot shim goes into *every* generated TU with `.globl __tur_uctx_swap` /
  `__tur_uctx_tramp`, so any build with more than one TU died with "multiple
  definition". Not `--shared`-specific: every multi-module `tur build <dir>`
  hits it; `--shared` just always has a second TU (`tur_runtime.c`). The
  definitions now sit in COMDAT (`.text$<name>` + `.linkonce discard`), the
  mechanism a C++ inline function uses. Two traps: `.scl 3` alone is not enough
  for `__tur_uctx_swap` (the C code calls it, so GCC re-externalises it), and
  the asm block **must** end by switching back to `.text` -- otherwise later
  compiler output lands in the discardable section and every generated program
  segfaults in `main()`.
- **`drv_basename_of()` split on `/` only.** Windows `realpath()` returns the
  backslash spelling, so `tur build --shared .` used the whole absolute path as
  the artifact name and ld rejected it. Now splits on `\` too, under `_WIN32`.
- **`dlfcn.h` in the smoke harness.** MinGW has none; it now includes the
  repo's existing `src/platform_dl.h`.

Nine harnesses also hardcoded `TUR="./build/tur"` rather than honouring a `TUR`
override, so they could not be pointed at `build-win/tur.exe` at all. They now
read `${TUR:-./build/tur}` like the other 35.

Still open: confirm the shim's "compile script on demand" subprocess invokes
`tur.exe` (with the suffix). `tur_settle_exe_output` already handles the
executable case.

### Scope (mostly in `../turmeric-godot/`)

- Add `windows` arch targets to the `SConstruct`.
- Statically link the WIN0/WIN1 `libturi` artifact (`build-win/src/libturi.a`),
  and its Windows link deps: `ws2_32`, `shlwapi`, `pthread` (winpthreads).
- Update `turmeric-godot.gdextension` with `windows.debug.x86_64` /
  `windows.release.x86_64` entries.
- Cache directory: prefer the in-project `<project>/.godot/turmeric-cache/`
  (cross-platform, Godot already creates `.godot/`).

### Testing

- Build `examples/spike` on a `windows-latest` runner (Godot ships a headless
  Windows binary). Confirm `[turmeric-godot] initialize(level=...)` in the log.
- Run the paddle-pong demo headless in CI as a smoke test once it lands.

### Open question -- shim toolchain on the user's box

When `tur build --shared` runs inside the GDExtension, which C compiler does it
invoke? Godot on Windows is MSVC-built, but GDExtension is a C ABI and godot-cpp
supports `use_mingw=yes`, so a MinGW-built shim + MinGW `tur` is expected to
work. Bundling a known-good toolchain vs. requiring MSYS2 on the user's machine
is the main UX decision. (This is why the port stayed on MinGW rather than
Clang-cl -- see the archived plan.)

---

## WIN3 tail -- the async fixtures still failing

None of these block WIN2; they are the long tail of the async subset.

### Pipe-fd fixtures: do not build, and would not poll either (9 fixtures)

`reactor-fd-*`, `reactor-fibers-cancel-on-free`, `reactor-fibers-park-fd`,
`reactor-stop-from-callback`, `reactor-wake-cross-thread`, `scheduler-io-park`
create a `pipe()` and register the pipe fds with the reactor.

**Correction (2026-07-31): these fail at `cc`, not at runtime.** MinGW does not
declare `pipe()` -- it ships `_pipe`, with a different signature -- and
`-Wimplicit-function-declaration` is a hard error on gcc >= 14. No reactor code
is reached. See `docs/archive/windows-pipe-reactor-fixtures-do-not-build.md`.

The runtime limitation below is still real and still applies the moment they do
compile, so the two causes compound rather than compete: **Windows `select()` is
socket-only** -- it cannot poll pipe or file fds at all, so the select-based
backend (`src/async/io_iocp.c`) cannot service them.

Options, in rough order of effort:
1. Accept as a platform limit (Godot scripts don't poll pipes). Mark the fixtures
   `requires.*`-skip on Windows.
2. Route `pipe()` through a loopback socket pair AND `read`/`write` through
   `recv`/`send` for those fds -- intricate, collides with file I/O, fragile.
   Only worth it if a real turmeric-godot use case needs pipe-style async.

Recommendation: option 1 unless a use case forces option 2.

### Concurrency stdout mismatches (2 fixtures) -- no longer reproduce

`fiber-effect` and `p19-8-fiber-effect-chain` used to build and run but produce
diverging output. **Both pass on Windows as of 2026-09-26** (Windows 11,
MSYS2/UCRT64, gcc 16.1, Debug; `run.sh` in three shards, 3208 passed). They were
never diagnosed separately. The likeliest fix is the `__builtin_setjmp` fiber
work ([windows-longjmp-across-fiber-stack-kills-effects](../archive/windows-longjmp-across-fiber-stack-kills-effects.md)),
since both run an effect on a fiber.

(An earlier revision named `httpd-h4-keepalive`, `httpd-h6-routing` and
`taskgroup-async` here. All three now pass -- the first two were collateral of
the Winsock `setsockopt` build failure, not a scheduler difference.)

---

## Winsock / POSIX gaps in the stdlib

Found 2026-07-31. Not regressions: these predate the bring-up or were missed by
it.

- **`setsockopt`/`getsockopt` against Winsock (~40 fixtures) -- RESOLVED.** The
  whole `httpd-*` / `httpd-async-*` family failed to build: Winsock declares the
  option value as `char *` rather than `void *`. Fixed in the emitter's Winsock
  compat shim (`emit_winsock_compat_shim`), which already remapped
  `socket`/`fcntl`/`recv`/`send`/`accept`/`connect`/`close` and was simply
  missing these two -- so every current and future POSIX-shaped call in any
  inline-C is covered, not just the three sites in `stdlib/httpd.tur`. Two
  subtleties handled there rather than per call site: `SO_RCVTIMEO`/`SO_SNDTIMEO`
  take a DWORD of milliseconds, not a `struct timeval` (a plain cast sets a
  garbage timeout), and `SO_REUSEADDR` is *dropped* on Windows because its
  meaning inverts -- Winsock's permits binding over a LIVE socket. See
  [docs/archive/windows-httpd-setsockopt-winsock.md](../archive/windows-httpd-setsockopt-winsock.md).
- **POSIX-only inline-C (5 fixtures).** `_mkdir` (3), `ioctl`/`struct winsize`
  (1), `fork`/`getppid` (1). The `_mkdir` case is **not** a source bug --
  `stdlib/fs.tur` is correctly written with `#ifdef _WIN32` /
  `#include <direct.h>`. The include hoister
  (`tur_hoist_top_includes_scan`, `src/compiler/emit_core.c:3005`) consumes only
  blank lines, `//` comments, `#include` and object-like `#define` at the top of
  an inline-C body and stops at anything else -- including `/* */` comments and
  `#ifdef`. So a platform-conditional include is never lifted to file scope, and
  because an in-body include is block-scoped AND the header's include guard makes
  the *second* function's copy expand to nothing, the next function that needs it
  sees an implicit declaration. That is precisely the class the hoister exists to
  fix (see its own `#define`-hoisting comment), just an unhandled gap in it.
  Fixing it generally -- lift a top-of-body conditional block verbatim when it
  contains only includes/defines/comments -- also unblocks the natural port of
  `term/width`/`term/height`, which would otherwise break the same way the moment
  their `#include <sys/ioctl.h>` is wrapped in an `#ifdef`. See
  [docs/archive/windows-posix-inline-c-gaps.md](../archive/windows-posix-inline-c-gaps.md).

## Subprocess and shared-library layers (not fixture-visible) -- RESOLVED

**Resolved 2026-09-26.** `tur install`, `tur fetch`, `tur new`, REPL spice
loading and `tur build --shared` all work on Windows: the shell-string sites
quote and redirect through `platform_proc.h` / `pkg_cmd_arg`, the lockfile
hash is in-process, `--shared` emits a `.dll`, and the REPL JIT module graph
hard-links or copies where it used to hit the `symlink` `ENOSYS` stub. See
[docs/archive/windows-subprocess-and-shared-lib-gaps.md](../archive/windows-subprocess-and-shared-lib-gaps.md).

---

## Codegen defects the port surfaced (NOT Windows-specific)

- **Carrier<->pointer straddles (5 fixtures).** Generated C trips
  `-Wint-conversion` / `-Wincompatible-pointer-types`, hard errors on gcc >= 14
  and Apple clang >= 15. **Correction (2026-07-31): the `-Wno-error=`
  workaround this plan previously cited is gone** -- both downgrades were
  deliberately removed (`src/driver/project.c:cmd_build_multi_files`) on the grounds that every
  straddle was bridged at emit time. Five remained under gcc 16.1.0, so that
  claim did not hold; do not re-add the downgrades, the removal is what exposed
  them.

  **Update (2026-08-01): four of the five are fixed and archived** --
  [docs/archive/macos-int-conversion-carrier-pointer-straddles.md](../archive/macos-int-conversion-carrier-pointer-straddles.md)
  (not macOS-specific despite the filename -- it is "any toolchain new enough";
  verified on Apple clang 21, which promotes `-Wint-conversion` the same way
  gcc >= 14 does). The fifth, `data-literal-nested`, was never a straddle but a
  wrong-monomorph selection that gcc's promoted `-Wincompatible-pointer-types`
  catches and clang's does not; re-filed as
  [docs/archive/vec-empty-like-monomorph-selects-int-element.md](../archive/vec-empty-like-monomorph-selects-int-element.md)
  and **also fixed** (2026-08-01), so all five of this class should now build
  under gcc >= 14 -- unverified on Windows, since the fixes were made and
  measured on Apple clang 21. The older gcc-14 reports are resolved and archived
  under `docs/archive/history/`.
- **`-O0` link failure -- RESOLVED.** Generated C referenced
  `tur_get_contract_handler` / `tur_set_contract_handler` with no definition.
  Both are now defined in `src/runtime/contract_handler.c:21,30`.

---

## WIN4 -- standalone CLI polish (deferred)

Not required for turmeric-godot; do only if a standalone Windows CLI workflow is
wanted.

- **Coloured diagnostics:** `src/compiler/diag.c` -> `_isatty(_fileno(stderr))`
  plus `SetConsoleMode(ENABLE_VIRTUAL_TERMINAL_PROCESSING)` on Win10+.
- **REPL:** MSYS2 ships wineditline, so line editing already works; nothing to do
  unless targeting a non-MSYS2 build.
- **Release packaging:** add `windows-x86_64` to `.github/workflows/release.yml`,
  producing a `.zip` (the other targets ship `.tar.gz`). The binary links two
  non-system DLLs -- `/ucrt64/bin/libwinpthread-1.dll` and `/ucrt64/bin/edit.dll`
  (libedit, and hence the REPL's line editing) -- so either bundle them in the
  zip or link statically; a bare `tur.exe` will not start on a machine without
  MSYS2. Confirm by unzipping with MSYS2 off `PATH` and running `tur --version`.
  Make sure the runner installs `mingw-w64-ucrt-x86_64-libedit`, or the build
  quietly succeeds and ships a REPL with no line editing.

## lmdb spice -- Windows assessment

Moved here when [lmdb-spice-plan](../archive/lmdb-spice-plan.md) was archived
(2026-10-29); it is the one item of that plan still open. LMDB's Win32 path
differs from the POSIX one in how the data file is sized, how locks work and
which libraries it links, so the spice has only been run on POSIX hosts so far.
The procedure is that plan's
[section 7](../archive/lmdb-spice-plan.md#7-windows-assessment-run-by-hand-before-l0-closes):
a self-contained LMDB probe (A1-A6, no `tur` needed) and `tur test` in
`spices/lmdb` (A7), run under MSYS2 UCRT64 on an NTFS volume. Fill in the
Windows column of its results table, and apply the answers as it says: A1 sets
`:link-libs`, A2 sets the Windows default map size, A3/A4 confirm growing the
map, A5/A6 decide whether the thread test and temp-directory cleanup need
Windows handling.

---

## Out of scope

ARM64 Windows (no free CI runner), WSL (runs the Linux build already), WASM
(separate Emscripten build), in-engine REPL on Windows.
