# repl-turn results

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
