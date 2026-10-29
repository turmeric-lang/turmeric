# repl-turn results

## 2026-10-29, Try Turmeric's module under node, the preamble cache

The browser JIT module (`build-wasm/wasm-jit`, `external/mir/` at `a43e5b2`),
driven through `turi_wasm_eval` as the page's prompt drives it: the same
sessions (N `defn`s, then 30 expression turns), one per `---`-separated turn,
`node tools/wasm-jit/run.js --compiled-turns --time`, with
`TUR_JIT_PREAMBLE_CACHE=0` and then `=1`:

| preamble cache | defs | median turn | p90 | first turn |
| --- | --- | --- | --- | --- |
| off | 1 | 104 ms | 148 ms | 263 ms |
| on | 1 | 82 ms | 109 ms | 441 ms |
| off | 50 | 122 ms | 153 ms | 246 ms |
| on | 50 | 77 ms | 96 ms | 413 ms |
| off | 200 | 142 ms | 182 ms | 233 ms |
| on | 200 | 84 ms | 86 ms | 468 ms |

c2mir goes from ~75 to ~36 ms a turn (`TUR_JIT_TIMING=1`, 50 definitions); the
rest is the front end in wasm, ~40 ms. The first turn fills the cache, ~190 ms
once a session. Node's wasm engine is V8, as in Chrome; other browsers will
differ.

## 2026-10-29, x86-64 Linux, 4 cores, Release, the preamble cache

`python3 benchmarks/repl-turn/run.py --tur build-pc/tur` (30 expression
turns), a Release build against the MIR fork's `c2mir-macro-dump` branch
(`-DTUR_MIR_SOURCE_DIR`; merged as turmeric-lang/mir#19 and vendored since),
with `TUR_JIT_PREAMBLE_CACHE=0` and then the default (on in a compiled REPL
session):

| preamble cache | defs | median turn | p90 | front | prep | jit | run | peak RSS |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| off | 1 | 134 ms | 162 ms | 18 | 3 | 86 | 0 | 55 MB |
| on | 1 | 60 ms | 63 ms | 17 | 3 | 28 | 0 | 44 MB |
| off | 50 | 167 ms | 197 ms | 23 | 3 | 112 | 0 | 61 MB |
| on | 50 | 65 ms | 80 ms | 19 | 3 | 30 | 0 | 49 MB |
| off | 200 | 165 ms | 204 ms | 24 | 3 | 104 | 0 | 82 MB |
| on | 200 | 86 ms | 109 ms | 25 | 4 | 39 | 0 | 65 MB |

The "off" rows run slower than the same configuration below did an hour
earlier (134 against 123 ms at one definition): the box is noisy, so compare
rows of one run. The cache takes c2mir's phase from ~85-110 ms to ~30-40 ms:
what it no longer does is preprocess the system headers in every turn
(aot-compiled-repl-plan C2, "The preamble, preprocessed once").

## 2026-10-29, x86-64 Linux, 4 cores, Release, `623de6329`

`python3 benchmarks/repl-turn/run.py --interp` (30 expression turns):

| engine | defs | median turn | p90 | front | prep | jit | run | peak RSS |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| compiled | 1 | 123 ms | 143 ms | 17 | 3 | 81 | 0 | 55 MB |
| interpreted | 1 | 0.1 ms | 0.1 ms | | | | | 16 MB |
| compiled | 50 | 119 ms | 127 ms | 17 | 3 | 79 | 0 | 61 MB |
| interpreted | 50 | 0.1 ms | 0.1 ms | | | | | 16 MB |
| compiled | 200 | 138 ms | 158 ms | 21 | 3 | 87 | 0 | 83 MB |
| interpreted | 200 | 0.1 ms | 0.1 ms | | | | | 18 MB |

The gate (200 defs, median at most 200 ms) is met: 138 ms.

**Where the time goes.** `jit` -- c2mir -- is ~80 ms of every turn, and the
front end ~17-21 ms. Timed with MIR's own `c2m` on a turn's dumped C
(`TUR_JIT_DUMP_FULL`): 69 ms to read it, 18 ms for the same text already
preprocessed, and 55 ms to preprocess the system headers alone
(`<stdio.h>`, `<pthread.h>`, `<ucontext.h>`, ...). So about two thirds of a
turn is c2mir re-preprocessing the same headers, and keeping the front end
resident (C4 item 1) could save at most ~20 ms.
