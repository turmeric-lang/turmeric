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

## `engine/` -- P1, superseded

P1's `engine/` (the engine linked into a copy of the REPL module by hand) was
replaced in P3 by the real build: the `tur_wasm_jit` CMake target and
[`tools/wasm-jit/`](../wasm-jit/README.md).  Its four programs live on as
`tools/wasm-jit/smoke/`; the plan's P1 section keeps its measurements.

## `asyncify/` -- can a compiled program yield? (2026-10-29)

raylib-web-playground-plan's prerequisite 3. `link.sh` relinks a built
`tur_wasm_jit` with extra emcc flags (`-sASYNCIFY`) and `emscripten_sleep` in
the import table; `drive.js` runs a program through `turi_wasm_jit_run` with
ccall's async mode and counts event-loop ticks; `frames.tur` and `while.tur`
sleep 16 ms a frame from inline C. The results are in that plan's section 7,
prerequisite 3.

```sh
bash tools/wasm-jit-spike/asyncify/link.sh /tmp/async -sASYNCIFY
node tools/wasm-jit-spike/asyncify/drive.js /tmp/async tools/wasm-jit-spike/asyncify/while.tur
```

