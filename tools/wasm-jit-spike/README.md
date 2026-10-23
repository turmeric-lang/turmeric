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
