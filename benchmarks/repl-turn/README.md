# repl-turn -- what a compiled REPL turn costs

The latency gate of [aot-compiled-repl-plan](../../docs/upcoming/aot-compiled-repl-plan.md)
C2: a turn in a session of up to 200 definitions takes at most 200 ms at the
median.

```sh
python3 benchmarks/repl-turn/run.py --tur build-release/tur [--defs 1,50,200] [--turns 30] [--interp]
```

For each session size it types N one-line `defn`s into
`tur repl --enable=compiled-repl`, then T expression turns calling them, and
prints one markdown row: the median and p90 wall time of an expression turn
(between consecutive `=>` lines), the median of each compile phase from
`TUR_REPL_TURN_TIMING=1` (`src/driver/jit.c`), and the REPL's peak RSS.

| phase | what |
| --- | --- |
| front | `compile_to_c`: read, elaborate and emit the turn's whole program |
| prep | the S2 preamble split, the pruner, the autolink scan |
| jit | c2mir, MIR link and generation (`tur_jit_last_timings`) |
| run | the program itself |

The rest of a turn's wall time is the REPL's own elaboration of the turn,
before the compile. Measure a Release build, alone on the machine.

Results: [RESULTS.md](RESULTS.md).
