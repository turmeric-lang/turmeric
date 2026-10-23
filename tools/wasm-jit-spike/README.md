# wasm-jit-spike

The P1 pre-spike of [docs/upcoming/wasm-jit-plan.md](../../docs/upcoming/wasm-jit-plan.md):
the vendored MIR built for wasm32 with Emscripten, driving c2mir and the
MIR interpreter from node. `run.sh` builds and runs everything; the numbers
it produced on 2026-10-23 (emcc 6.0.13, node 22) are in the plan's section
7.1. It links nothing from `tur` itself -- that is P1 proper.

Files: `spike.c` (compile-link-interpret driver), `hdr/` (the two headers
the test programs include; on wasm the embedder serves libc headers),
`drive.js` (depth and cost measurements), `mini.js` (small calls against the
assertions build), `sjlj.c` (the host-owned setjmp landing, no MIR).

## `engine/` -- P1: the engine inside a copy of the REPL module

`engine/build.sh` compiles the stock `tur_wasm` source list plus the driver's
front half, `jit_engine.c`, `turi/jit_ffi.c`, MIR and a glue entry into one
module, generating the import table from the runtime's declarations region
(`engine/gen-symtab.py`). It needs a configured `build-wasm/` (its `tur_wasm`
command line is copied), emcc and node. `node engine/run.js engine/tests/cmix.tur`
runs the plan's inline-C program through it; flags are documented in
`engine/run.js`. MIR is patched in a scratch copy under `engine/build/mir`
with the fork fixes the plan's section 6 lists; `external/mir` is never
edited. The plan's P1 section has the numbers and what is left.
