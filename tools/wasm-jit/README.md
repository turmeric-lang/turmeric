# tools/wasm-jit -- the browser JIT module

Builds and drives `turmeric-jit.{js,wasm}`: Turmeric compiled the way
`tur jit` compiles it -- the driver's front half emits C, c2mir compiles it to
MIR, MIR's interpreter runs it -- inside a WebAssembly module, so inline C runs
in the browser with no server.  The design and its status are in
[docs/upcoming/wasm-jit-plan.md](../../docs/upcoming/wasm-jit-plan.md).

## Build

```sh
cmake -S . -B build-wasm -DTUR_WASM=ON      # emcc on PATH (the emsdk environment)
cmake --build build-wasm --target tur_wasm_jit
```

The output is `build-wasm/wasm-jit/turmeric-jit.{js,wasm}`, copied to
`web/public/` beside the playground's `turmeric.{js,wasm}`.  It is a second
module, not a replacement: the page loads it only for the Compile engine
(`web/public/jit-worker.js`).  `-DTUR_WASM_JIT=OFF` leaves it out.

Neither module embeds the stdlib.  Both link the loader of one package,
`stdlib.data` (CMake's `tur_stdlib_pkg`, wasm-jit-plan 5.11), which fetches it
from beside the module's script and mounts it at `/stdlib`.  The build copies
it into `build-wasm/wasm-jit/` too, so `run.js` finds it there under node.

| File | Role |
|---|---|
| `build-module.sh` | what the target runs: objects through a generated Makefile (`-MMD`, so a rebuild recompiles only what changed), the import table, the embedded headers, the link |
| `gen-symtab.py` | the import table that stands in for `dlsym` (plan 5.3): the runtime's declarations region, `hamt.h`, the TLS accessors and a libc list at their real types, and the rest of the resident runtime at the wasm signatures its objects record; with `--stubs`, one exact-signature call stub per wasm signature the runtime defines plus every small libc shape (plan 5.5), which `tur_jit_execute` registers with MIR so a call into the runtime skips the JS trampoline |
| `headers.txt` | the system headers c2mir may open, embedded at `/sysinclude` as their closure inside Emscripten's musl (plan 5.4) |
| `stdatomic.h` | C11 atomics for c2mir, embedded beside them: musl has none, and on this single-threaded engine each operation is a plain access (its header says why that is sound here only) |
| `run.js` | runs one program through the module under node, as the page will; `--dump-c FILE` writes the text c2mir was handed; `--turns` runs the file as prompt turns (chunks between `---` lines) in one session, as the Compile engine's prompt does |
| `check-examples.sh` | P3's gate: every Try Turmeric example compiles or falls back, and none differs from `tur interpret` |
| `smoke/` | four programs from P1: inline C (`cmix`), effects (`eff`, which runs since P5's `setjmp`), a loop and hello; and `turns.tur`, prompt turns whose inline-C defns C1 compiles (`run.js --turns`, P6) |

The sources that are not scripts live in `src/`: `src/web/wasm_jit_glue.c`
(`turi_wasm_jit_run`, the page's entry point) and `src/web/wasm_jit_stubs.c`
(MIR's generator and ucontext, which the module links and never reaches).

### `setjmp` and call stubs (P5)

emcc refuses any indirect use of `setjmp`, so the import table cannot carry
it.  `jit_import_resolver` resolves it to a stand-in instead, MIR replaces
that with its own marker, and the interpreter calls `setjmp` by name in the
frame of the function that asked (turmeric-lang/mir, `wasm-tests/README.md`
"setjmp").  `longjmp` is an ordinary import.  The effect and panic landings and
the DK root entry need nothing else.

A call stub turns a call into the runtime from a JS round trip into a direct
`call_indirect`, so it is strict where the trampoline was lenient: a program
whose C declares a function at a different type than it has (an undeclared
`sqrt`, implicitly `int sqrt()`) traps with `function signature mismatch`
rather than computing garbage.  Natively the same program is already wrong.

### The JSPI build (`TUR_WASM_JIT_JSPI=1`)

The raylib playground's module (raylib-web-playground-plan, J4): the same
script with `TUR_WASM_JIT_JSPI=1` in the environment links `-sJSPI
-sJSPI_EXPORTS=turi_wasm_jit_run` and puts `emscripten_sleep` in the import
table.  `turi_wasm_jit_run` then returns a Promise, and a compiled program
that sleeps -- raylib's frame loop through `WindowShouldClose`, or inline C
calling `emscripten_sleep` -- suspends the wasm stack and resumes when the
timer fires, so a blocking `while` loop yields to the page between frames.
The wasm is unchanged (JSPI instruments nothing; Asyncify would have grown
it by half), and so are depth and speed.  Two constraints: the sleep must
have only wasm frames above it (this build's wasm-native `setjmp` sees to
that for calls out of `eval`; a sleep inside a comparator or a frame
callback entered through the table's JS closure would fail with
`SuspendError`), and the browser must have JSPI (`'Suspending' in
WebAssembly`: Chrome 137+ today).  That is why it is not the default: the
Compile engine must load in every browser.  Build it into a directory of
its own, never over the default module's (`tools/wasm-jit-spike/jspi/build.sh`
does that), and drive it from a browser -- node 22's V8 has no JSPI.

## Run a program

```sh
node tools/wasm-jit/run.js tools/wasm-jit/smoke/cmix.tur
```

The program's stdout and stderr pass through; the module's answer -- which
engine ran it and why -- is the last line on stderr:

```
tur-wasm-jit: {"engine":"compiled","exit":0,"front_ms":130.9,"compile_ms":120.7,"run_ms":1.3}
```

`engine` is `compiled` (the MIR engine ran it), `interpreted` (the engine
refused the program -- threads, an import it lacks, a c2mir rejection -- and
the interpreter ran it instead, `reason` says why) or `error` (the program
does not compile).  `TUR_JIT_WARNINGS=1` in the module's environment shows
c2mir's warnings, which the page hides.  A `<tur-jit>:LINE` diagnostic is a
line of the text `--dump-c` writes.

## The corpus

`bash tests/run-jit-wasm.sh` runs the fixtures through the module and checks
them against `tests/jit-wasm-baseline.txt`; its header says how.
