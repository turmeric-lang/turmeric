# turmeric-godot AOT: the staged build has no `godot-*` natives, so it cannot compile any real script

> **INVESTIGATED 2026-09-07. Fix direction 3 below is WRONG and should not be
> acted on.** It claims the JIT makes the problem "disappear rather than being
> solved" because the natives are already in the host's address space. They are
> -- and it does not help, because the failure happens at **elaboration**, long
> before any symbol is resolved.
>
> Measured, not reasoned. Same three-line program, one undeclared native:
>
> ```
> tur build  -> error: unknown function or operator 'godot-export'
> tur jit    -> error: unknown function or operator 'godot-export'   (identical)
> tur --interpret -> warning TUR-W0040 ... will runtime-dispatch     (defers)
> ```
>
> The reason is in [src/compiler/elab_call.c:3726](../../src/compiler/elab_call.c):
> the runtime-dispatch fallback lives in a branch the source itself labels
> `eval mode`. Compiled mode takes the other branch and hard-errors. And
> `g_interpret_mode` is set by exactly `cmd_eval_h`, `cmd_eval_expr` and
> `cmd_repl` -- **not** by `cmd_jit`. The JIT elaborates in compiled mode, so it
> hits this identically. `src/main.c:4420` says as much in passing: "Same
> emission posture as cmd_jit -- which means COMPILED-mode elaboration".
>
> **So declarations are required on every route.** In-process compilation
> changes where symbols resolve at run time; it does not teach the elaborator a
> name. This narrows the JIT's advantage here to "no external toolchain and no
> link step" -- still real, but it is no longer an argument for skipping the
> work below.
>
> ### The declaration route works, and the shape is confirmed
>
> `extern-c` accepts a hyphenated name and mangles `-` to `_`:
>
> ```turmeric
> (extern-c godot-export [name :cstr ty :cstr dflt :float] :void)
> ```
>
> emits exactly
>
> ```c
> extern void godot_export(const char *, const char *, double);
> ...
> godot_export("vel-x", "float", 240.0);
> ```
>
> A plain, typed C call -- which also means the staged project gets *real types*
> rather than the interpreter path's `:int`-shaped dynamic dispatch.
>
> ### The cost that is not in "just declare them"
>
> The natives cannot be linked as they stand. Each is
> `static TuriValue tg_native_export(TuriEnv *, TuriValue *, uint32_t, void *)`
> ([src/turmeric_language.cpp:326](https://github.com/turmeric-lang/turmeric-godot/blob/main/src/turmeric_language.cpp))
> -- the *interpreter's* ABI, file-local, and nothing compiled code can call. So
> the real work is ~90 exported C entry points with legal names and concrete
> signatures, each marshalling to the existing implementation. That is the same
> work whichever route resolves the symbols, and it is what should be costed.
>
> Then, and only then, the narrower choice: link the staged library against the
> extension, bind at `dlopen` (fix direction 2, and `AotImage` already carries a
> `mangled` C-symbol field for the export table), or compile in-process and
> resolve via `dlsym(RTLD_DEFAULT)`, which now works on Windows.
>
> ### The three routes are NOT equally hard on Windows (2026-09-08)
>
> That last sentence understates the difference, and the gap is a platform
> property rather than a preference.
>
> **AOT/cc route.** A PE DLL cannot be linked with unresolved symbols at all:
>
> ```
> ld.exe: undefined reference to `godot_export'
> collect2.exe: error: ld returned 1 exit status
> ```
>
> So "leave the natives undefined and bind them at `dlopen`" -- fix direction 2
> -- works on Linux and macOS and **cannot work on Windows**. The staged build
> would need something for `ld` to resolve the extension's symbols against.
> (An earlier revision of this note said no import library is produced. That
> was measured on a tree that had never been built on Windows -- godot-cpp's
> own SConstruct does pass `-Wl,--out-implib`, yielding a doubled-prefix
> `liblib<name>.a` beside the DLL. It is simply never consumed. And it is not
> needed: MinGW's `ld` links against the DLL itself, synthesising the import
> stubs from its export table.) Producing one, finding it from the staged
> project, and
> keeping the staged link line pointed at it is real plumbing, on the most
> fragile platform surface the shim has.
>
> **JIT route.** No link step exists to fail. `MIR_link` resolves undefined
> imports through `jit_import_resolver`
> ([src/jit_engine.c:424](../../src/jit_engine.c)), which falls back to
> `dlsym(RTLD_DEFAULT, name)` -- `jit_engine.c:8` states the model outright,
> "symbols resolved against THIS process". The same generated `extern-c`
> declarations therefore work unchanged, and the import-library problem simply
> does not arise. **On Windows this is the whole difference between the two
> routes.**
>
> What the JIT route still needs is EXPORT, not import, and that is a much
> smaller ask. `dlsym(RTLD_DEFAULT)` on Windows walks the main module then every
> loaded module via `EnumProcessModules`, and `GetProcAddress` sees only
> **exported** symbols ([src/platform_dl.h:54](../../src/platform_dl.h) records
> this caveat). godot-cpp defaults `symbols_visibility` to `hidden`
> (`tools/godotcpp.py:316`), so the entry points need an explicit export
> attribute or an `--export-all-symbols` link flag. One flag, not an import
> library.
>
> ### The JIT's dynamic FFI is also the answer to the variadic problem
>
> Separately, and this is the part the AOT route has no equivalent for:
> `src/turi/jit_ffi.c` is a c2mir-backed dynamic FFI that synthesizes a call
> thunk from a signature string at run time, cached per signature, explicitly
> "so there is no `--max-arity` ceiling and no shape-table regeneration".
>
> Nine of the 89 natives are variadic, and they are the busiest names in the
> surface -- `godot-call-v` alone has 1116 call sites across 13 distinct
> arities. A fixed `extern-c` declaration cannot describe them, so the AOT route
> needs either ~42 monomorphic entry points or a pack-based calling convention.
> A dynamic FFI needs neither. That capability is JIT-only.
>
> ### What the interpreter does and does not need
>
> "turi has no FFI" is too strong: `dlsym` and `call-ptr` exist, gated behind
> `(unsafe ...)`, and the known interpreter divergence
> ([jit-ffi-interp-refuses-parametric-record-field](../archive/jit-ffi-interp-refuses-parametric-record-field.md), since resolved)
> is one record shape, not the whole surface. It also does not matter here --
> the interpreter reaches the natives through the env by name and never needs an
> FFI for them, which is why the interpreter path has worked all along while
> both compiled paths failed.
>
> **Net:** the JIT route avoids the import library AND the variadic problem, on
> the platform where both are worst. That is a materially stronger argument for
> it than "no toolchain on the player's machine", and it does not contradict the
> J1/J2 correction above: declarations and exported entry points are still
> required on every route. What changes is only what happens after them.

> ### PROGRESS 2026-09-09 -- the declaration route is BUILT and a real script
> ### now AOT-compiles and runs. This report stays OPEN; see "What remains".
>
> Reproduced first, on macOS, with a fresh `tur` (v0.46.0) and Godot 4.3
> against `turmeric-godot` at `origin/main` (which carries the `#mode`
> stripping fix, so AOT engages). Exactly the reported text:
>
> ```
> .../turmeric-cache/0a180eab577dd51f/src/ball.tur:10:2: error: unknown function or operator 'godot-export'
> 10 | (godot-export "vel-x"   "float" 240.0)
>    |  ^^^^^^^^^^^^
> ```
>
> plus one the report does not mention: `unknown function or operator
> 'node/self'`. The *prelude* is missing from the staged project too, not just
> the natives -- and the prelude is what every curated `node/...` call in a
> real script goes through.
>
> **Fix direction 2 (resolve at load) needed no compiler change.** `tur build
> --shared` already passes `-undefined dynamic_lookup` on macOS
> ([src/main.c:6245](../../src/main.c)), so the staged `.so` links with
> `godot_vec2` and friends undefined and `dlopen` binds them to the running
> extension. Verified by `nm -u` on the staged library. The whole fix is
> therefore in `turmeric-godot`; nothing in this repo changed.
>
> **Two hard constraints shaped the design, and both rule out "just paste the
> declarations at the top of the script":**
>
> - `import is only allowed inside defmodule` -- a script written as bare
>   top-level forms cannot reach a separate declarations module at all.
> - `defmodule must be the first form in the file` -- so declarations cannot be
>   prepended to a script that already has one, and `(defmodule ...)` is the
>   *only* shape whose defns reach `exports.manifest` and are therefore
>   AOT-dispatchable at all.
>
> The stager now rewrites the script instead: a bare-forms script is wrapped in
> `(defmodule <name> (import tg-godot) (export <its defns>) ... )`, spliced onto
> the front of the first form's line so no line moves and diagnostics keep the
> user's line numbers. Its top-level *calls* are blanked in place -- inside a
> `defmodule` they are TUR-E0711 ("expression at defmodule top level is never
> evaluated"), and they have already run in the interpreter pass, which is what
> populates the inspector exports and the signal list.
>
> ### What works now, and how it was checked
>
> `examples/paddle-pong-tur`, macOS arm64, Godot 4.3 headless:
>
> ```sh
> # scripts/ball.tur and scripts/paddle.tur each carry `#mode aot`
> TUR_BIN=<fresh tur> Godot --headless --path . --script scripts/pong_driver.gd
> ```
>
> ```
> [turmeric res://scripts/ball.tur AOT] loaded 110 exports from .../libtg_script_....so (built)
> [turmeric res://scripts/paddle.tur AOT] loaded 110 exports from .../libtg_script_....so (built)
> [turmeric-godot AOT] first dispatch routed via AOT: ball/_process (mangled=ball___unprocess)
> [turmeric-godot AOT] first dispatch routed via AOT: paddle/_process (mangled=paddle___unprocess)
> [pong] all assertions passed: ball=(281.1, 359.0) vy=-240.00  p1.y=-61.9  p2.y=541.9
> ```
>
> `pong_driver.gd` is a behavioural test, not a smoke test: it drives 200
> frames and asserts the ball advanced, stayed inside its bounds, and flipped
> `vel-y` off the bottom wall. The interpreter baseline gives the same answers
> to within frame jitter.
>
> `examples/aot-bench` also works for the first time (its `_ready` calls
> `godot-println`, so it hit this bug too): **298.4 ns/call interpreted ->
> 215.9 ns/call AOT** over 1e6 calls.
>
> Two bugs on the AOT side had to be fixed to get there, both invisible until
> the staged build got far enough to expose them:
>
> - `cb_has_method` consulted only the interpreter env. Godot asks
>   `has_method("_process")` to decide whether to put a node on the process
>   list, and the A4 fast path returns from `_reload` *before* the interpreter
>   eval -- so on a cache hit the env was empty, `has_method` said no, and every
>   lifecycle hook was silently dead. This is why the fix appeared to work on a
>   cold cache and do nothing on a warm one.
> - The AOT dispatch path set neither `g_current_instance` nor a Variant arena
>   frame. Compiled code calls the same natives, so `godot-self` /
>   `godot-prop-get` had no instance and every `godot-vec2` handle leaked.
>
> ### What remains
>
> - **Variadic natives have no C entry point.** `godot-call{,-v,-f,-b,-c}`,
>   `godot-signal`, `emit-signal`. `extern-c` has no variadic form
>   (`elab_extern_c` hardcodes `is_variadic = false`) and calling a C variadic
>   through a fixed prototype is ABI-invalid on Apple arm64. AOT scripts use the
>   arity- and type-specialised `godot-callx-*` family instead (25 entry points:
>   5 return types x {no extra arg, one :int/:float/:bool/:cstr arg}). A script
>   calling `godot-call` directly still fails -- but now with a note naming the
>   substitute rather than a bare "unknown function or operator".
>   `examples/paddle-pong-tur/scripts/score.tur` is the live example.
> - **The generated facade is not staged.** `TG_GENERATED_FACADE_SOURCE` is 2234
>   per-class wrappers (`label/set-text`, `node2d/...`), all built on
>   `godot-call*` at arities 0..N. Staging it means teaching
>   `tools/gen_godot_facade.py` to pick the arg-class-suffixed spelling, and
>   growing the `godot-callx-*` family past one extra argument (two-arg shapes
>   are 5 x 16 = 80 more entry points). Mechanical, but a generator change plus
>   ~5300 more lines of Turmeric in every staged compile.
> - **`godot-connect-typed` cannot cross.** It takes a Turmeric closure --
>   `TuriClosure*` interpreted, a fat pointer compiled. The prelude's
>   `timer/one-shot` and `after` are marked `;;#aot-skip` for the same reason.
> - **`(godot-export ...)` inside a hand-written `defmodule` is TUR-E0711.** The
>   stager blanks top-level calls only when it does the wrapping; a user who
>   writes their own `defmodule` and puts `godot-export` in it gets the error.
>   Same for `defgodot-script`'s `:exports` block.
> - **Compile cost.** A staged build is ~0.50 s wall (669 lines of Turmeric ->
>   10,364 lines of C, `tur` + `cc`), of which the `tg-godot` module is the bulk.
>   Cached per source-hash, so it is a cold-start cost, but it scales with
>   script count.

**Summary:** The AOT path stages a script into a transient project and compiles
it with standalone `tur`. But every `godot-*` name is a C++ native the
GDExtension registers into the *interpreter* env at run time, and standalone
`tur` has never heard of any of them -- so the staged build fails at the first
one with `unknown function or operator 'godot-export'`. Any script that touches
the Godot API, which is every useful script, cannot be AOT-compiled.

**Severity:** High for AOT. The interpreter path is unaffected and works.

**Platform:** Not platform-specific. Found on Windows only because that is where
the AOT path was first driven end to end; standalone `tur` has no `godot-*`
natives on any host, so this fails identically on Linux and macOS.

## Repro

```sh
cd ../turmeric-godot/examples/paddle-pong-tur
# force AOT for one script
sed -i '1i #mode aot' scripts/ball.tur
TUR_BIN=/path/to/tur Godot --headless --path . --quit
cat .godot/turmeric-cache/*/build.log
```

```
.../src/ball.tur:10:2: error: unknown function or operator 'godot-export'
10 | (godot-export "vel-x"   "float" 240.0)
   |  ^^^^^^^^^^^^
```

## What works, and why that matters

Everything around the compile is fine -- this is not a plumbing bug. The cache
tree is created and populated correctly:

```
.godot/turmeric-cache/<hash>/
  build.tur   src/ball.tur   build/{bin,lib,obj}   build.log
```

`tur build --shared` is invoked, and its non-zero exit is captured and surfaced
through the diag sink. So staging, quoting, subprocess spawn and exit handling
all behave. The failure is purely that the staged project is compiled in a world
where the Godot bridge does not exist.

## Root cause

Two different name-resolution worlds:

- **Interpreter path.** `TurmericLanguage` registers ~90 natives
  (`godot-export`, `godot-call`, `godot-vec2`, ...) with
  `turi_register_default_native_typed`, so every per-script `TuriEnv` has them
  and `turi_eval` resolves them. This is why the interpreter path works.
- **AOT path.** `aot_cache.cpp` writes the source to disk and shells out to
  `tur build --shared`. That is an ordinary compile of an ordinary project. The
  natives are C++ functions living inside the GDExtension `.dll`/`.so` -- there
  is no declaration for `tur` to resolve against, and nothing to link.

The prelude and generated facade do not help: they are *Turmeric* wrappers whose
bodies bottom out in the same `godot-*` natives, and they are evaluated into the
interpreter env, not staged.

## Fix directions

None of these is small; pick deliberately.

1. **Emit an `extern-c` declaration header into the staged project.** Generate a
   `.tur` file declaring every `godot-*` native's C signature, stage it
   alongside the source, and link the built `.so`/`.dll` against the
   GDExtension's exported symbols. Requires the extension to export them
   (it currently builds with `-fvisibility=hidden`) and requires the staged link
   line to reference the extension binary.
2. **Resolve at load instead of link.** Compile the staged library with the
   natives left undefined and bind them at `dlopen` time, the way the export
   table is already handled in `aot_image.cpp`. Trades a link-time dependency
   for a startup fixup pass.
3. **Drop AOT for the JIT.** The JIT compiles in-process, so the natives are
   already resolvable in the host's address space -- the whole problem
   disappears rather than being solved. See
   [jit-godot-embedding-spike.md](jit-godot-embedding-spike.md); this report is
   a concrete argument in that spike's favour, and worth weighing before
   investing in options 1 or 2.

   **Spike result (2026-08-05), and it strengthens this option.** A
   `-DTUR_JIT=ON` Release configure + build was attempted on Windows
   (MSYS2/UCRT64, gcc 16.1):

   - **MIR fetches and compiles cleanly under MinGW.** The feared answer from
     [jit-windows-support-spike.md](../archive/jit-windows-support-spike.md) question 1 --
     "MIR-gen only implements the SysV ABI, so Windows is a back-end port" --
     did **not** materialise at build time. Nothing in `_deps/mir-src` failed.
   - **The only thing that fails to compile is our own
     [src/jit_engine.c](../../src/jit_engine.c)** (711 lines), in three small
     and specific ways:
     - `#include <dlfcn.h>` -- MinGW has none. Fixed here by including the
       tree's existing `platform_dl.h`, the same remedy already used in
       `spice_loader.c` and the Godot shim's AOT image loader.
     - `#include <sys/resource.h>` plus `getrusage` / `RUSAGE_SELF`, inside the
       block the file itself labels "Temporary: measures the tier so I0's
       go/no-go gate has numbers. Remove wholesale if I0 comes back negative."
       Guarding or deleting that block costs no shipped functionality.
     - `RTLD_DEFAULT` is undeclared -- `platform_dl.h` defines `RTLD_NOW`,
       `RTLD_LAZY`, `RTLD_LOCAL` and `RTLD_GLOBAL` but not this one. It means
       "search the main program's symbols", whose Win32 spelling is
       `GetModuleHandle(NULL)`. This is a genuine gap in the shim, not a
       jit_engine quirk, and fixing it there benefits every caller.

   So the Windows JIT looks like **a bounded port of one file**, not a MIR
   project. Two of the three are already-solved problems applied to one more
   site; the third is a one-symbol addition to `platform_dl.h`.

   **Update, later the same session:** the build now completes and the engine
   *runs* -- it engages c2mir, and on failure falls back to the cc path
   cleanly. The new wall is that c2mir cannot digest the MinGW system headers
   (`vadefs.h` `#error`, `x86intrin.h`, `#pragma pack(push, MACRO)`), which
   the emitted C drags in via `#include <winsock2.h>` under `_WIN32`. The
   recommended route is the S2 split-runtime path with a windows-header-free
   TU -- see the findings block at the top of
   [jit-windows-support-spike.md](../archive/jit-windows-support-spike.md). Still true:
   no JIT-generated code has executed on Windows, so the MS x64 ABI and
   executable-memory questions remain open, and "MIR compiles" is still not
   "the JIT works".

## Note on `#mode`

Forcing AOT per-script requires the `#mode aot` directive, which until recently
made the reader fail with a parse error (the shim parsed the directive but never
stripped it before eval). Fixed in turmeric-godot as
"Strip the `#mode` directive before the source reaches a reader". Without that
fix this report is not reproducible, because AOT never engages at all.

## Related

- [godot-baked-in-prelude-fails-to-eval.md](../archive/godot-baked-in-prelude-fails-to-eval.md)
  -- the three bugs that had to be cleared before the AOT path could be reached.
- [docs/upcoming/v1/windows-remaining-plan.md](../upcoming/v1/windows-remaining-plan.md) -- WIN2.
