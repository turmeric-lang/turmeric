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

The output is `build-wasm/wasm-jit/turmeric-jit.{js,wasm}`.  It is a second
module beside the playground's `turmeric.{js,wasm}`, not a replacement: the
page loads it only for the Compile engine.  `-DTUR_WASM_JIT=OFF` leaves it out.

| File | Role |
|---|---|
| `build-module.sh` | what the target runs: objects through a generated Makefile (`-MMD`, so a rebuild recompiles only what changed), the import table, the embedded headers, the link |
| `gen-symtab.py` | the import table that stands in for `dlsym` (plan 5.3): the runtime's declarations region, `hamt.h`, the TLS accessors and a libc list at their real types, and the rest of the resident runtime at the wasm signatures its objects record |
| `headers.txt` | the system headers c2mir may open, embedded at `/sysinclude` as their closure inside Emscripten's musl (plan 5.4) |
| `run.js` | runs one program through the module under node, as the page will |
| `smoke/` | four programs from P1: inline C (`cmix`), effects (`eff`, which still needs P5's `setjmp` landing), a loop and hello |

The sources that are not scripts live in `src/`: `src/web/wasm_jit_glue.c`
(`turi_wasm_jit_run`, the page's entry point) and `src/web/wasm_jit_stubs.c`
(MIR's generator and ucontext, which the module links and never reaches).

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
c2mir's warnings, which the page hides.

## The corpus

`bash tests/run-jit-wasm.sh` runs the fixtures through the module and checks
them against `tests/jit-wasm-baseline.txt`; its header says how.
