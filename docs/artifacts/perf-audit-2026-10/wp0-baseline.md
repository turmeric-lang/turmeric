# WP0 -- baseline and environment lock

[performance-audit-plan](../../archive/performance-audit-plan.md) WP0. The box
header, the checklist answers and the thermal check are in
[README.md](README.md); every number below was taken on that box under those
conditions.

| | |
| --- | --- |
| Commit | `1a2052bf7` (this branch, off `main` @ `6ba0fa1e6`, v0.65.1); clean tree |
| tur | Release, `-DTUR_JIT=ON`, gcc 13.3.0, `build-release/` |
| Box | NUC 9 Pro, i7-9850H, WSL2 Ubuntu 24.04, 12 vCPU / 12 GB (README) |
| Host | High performance plan + Best performance overlay; Steam and Razer Synapse closed; idle |
| Method | 3 sweeps x (1 discarded warm-up + best of 5), 3 s cool-down between programs, serial, not pinned |
| Sweeps | 2026-10-15 00:19:50Z, 00:29:53Z, 00:39:57Z UTC; 296 rows each, every row's output matched its pin |
| Result | 296 merged rows; **2 not reproduced** within 10% by a second sweep (both runs of a few ms) |

## Reproduce

Inside the WSL2 distro, on a Release `-DTUR_JIT=ON` build, with the host on
the High performance plan and idle, three times:

```sh
for i in 1 2 3; do
  python3 benchmarks/audit/run-corpus.py --tur ./build-release/tur --reps 5 --cooldown 3 \
    --out /srv/perf/runs/wp0-sweep$i
done
python3 benchmarks/audit/merge-sweeps.py /srv/perf/runs/wp0-merged /srv/perf/runs/wp0-sweep{1,2,3}
```

`run-corpus.py` refuses a sanitized `tur`, checks every run's stdout against
the program's pin before its number counts, and measures through
`benchmarks/audit/rusage-run.c` (fork, exec, `wait4`, `CLOCK_MONOTONIC`).
`merge-sweeps.py` keeps each row's best across sweeps and marks the row when
no second sweep reproduces that best within 10% (the plan's CV > 10% rule).
The merged rows are [wp0-baseline.csv](wp0-baseline.csv) (with every sweep's
best in `sweep_mins`); each sweep's raw rows and header are in
[wp0-sweeps/](wp0-sweeps/).

**Why three sweeps.** Each single sweep took one burst of several seconds in
which even sub-millisecond launches ran 10-250x slow (`triangle/mandel`'s cc
run read 68 ms best-of-5 against a steady 3.4 ms in one sweep;
`examples/datalog/datalog`'s read 150 ms against 0.6 ms in another), at a
different point each time. No host process was busy before or after, and
the cause is unknown; WSL2/Hyper-V scheduling is the obvious suspect. A
single sweep's best-of-5 cannot tell such a burst from a real number; the
best of three sweeps, with a reproduction check, can. Any later package
that times on this box should do the same.

Columns: wall times are the **best across 3 sweeps x 5 runs** in ms; `*`
marks a cell no second sweep reproduced within 10%. `emit-c` is `tur emit-c`
alone; `cc build` is `tur build -o` (front end + cc + link, the driver's
default `-O2 -std=c99 -Wall -fno-strict-aliasing -fwrapv`); `cc run` is the
binary; `jit` is `tur jit <file>` (front end + c2mir + run); `interp` is
`tur --interpret <file>` (front end + tree-walk). RSS is the peak
`ru_maxrss` over every run, in KiB; the launcher's own floor is ~1.5 MB.
An empty cell is an engine the corpus does not run that program on (see
`corpus.txt` for why: inline C, depth, or `run.args`).

| program | emit-c ms | C bytes | cc build ms | binary bytes | cc run ms | run RSS KiB | jit ms | interp ms |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| benchmarks/triangle/fib.tur | 16.71 | 425895 | 161.92 | 26536 | 2.0 | 1536 | 105.95 | 161.67 |
| benchmarks/triangle/loop-sum.tur | 16.12 | 425914 | 163.52 | 23328 | 0.6 | 1536 | 108.25 | 1526.37 |
| benchmarks/triangle/mandel.tur | 16.9 | 430017 | 181.13 | 26736 | 3.37 * | 1536 | 110.89 | 628.33 |
| benchmarks/bind-option.tur | 16.23 | 428124 | 160.89 | 23320 | 3.01 | 4736 | 104.41 |  |
| benchmarks/fmap-vec.tur | 15.68 | 427230 | 163.25 | 23392 | 4.47 | 9344 | 104.71 |  |
| benchmarks/bench-backtrack-n-queens.tur | 15.9 | 427136 | 176.18 | 23416 | 153.06 | 1664 | 286.84 |  |
| benchmarks/bench-clone-overhead.tur | 15.77 | 426817 | 158.21 | 23328 | 133.23 | 1664 | 339.4 |  |
| benchmarks/bench-dfs-queens.tur | 19.47 | 484278 | 315.77 | 56744 | 26.82 | 19840 | 173.03 |  |
| benchmarks/bench-logic-query.tur | 15.57 | 428287 | 159.17 | 23328 | 3.25 | 1536 | 104.26 |  |
| benchmarks/bench-logic-subst.tur | 21.85 | 506086 | 214.1 | 36584 | 15.27 | 5248 | 143.2 |  |
| benchmarks/bench-logic-subst-split.tur | 22.6 | 506951 | 217.51 | 31016 | 15.11 | 5248 | 143.6 |  |
| benchmarks/bench-parsec-json.tur | 16.03 | 428111 | 164.6 | 23328 | 7.97 | 1536 | 115.16 |  |
| benchmarks/bench-poly-specialize.tur | 16.05 | 429009 | 169.36 | 40160 | 0.59 | 1536 | 101.54 | 10.81 |
| benchmarks/bench-regions-subst.tur | 21.97 | 511481 | 232.52 | 46552 | 16.16 | 3968 | 147.07 |  |
| benchmarks/bench-saffron-dyn-tail.tur | 17.24 | 549946 | 657.61 | 68120 | 902.73 | 1664 | 3187.22 | 3214.57 |
| benchmarks/hamt-bench-insert.tur | 15.65 | 426369 | 159.93 | 48040 | 164.26 | 17664 | 271.95 |  |
| benchmarks/hamt-bench-lookup.tur | 15.45 | 426760 | 162.71 | 48040 | 169.27 | 17920 | 274.26 |  |
| benchmarks/hamt-bench-snapshot.tur | 15.46 | 426850 | 163.73 | 48040 | 1.29 | 1664 | 101.89 |  |
| benchmarks/hamt-bench-transient.tur | 15.54 | 426400 | 159.75 | 48040 | 159.7 | 17664 | 268.64 |  |
| tests/benchmarks/hkt-dict-pass.tur | 15.39 | 427502 | 163.17 | 26416 | 0.54 | 1152 | 100.32 |  |
| tests/benchmarks/scheduler-mt-vs-st.tur | 15.53 | 425992 | 156.75 | 23416 | 0.54 | 1152 | 97.41 |  |
| tests/benchmarks/workstealing-deque-bench.tur | 15.68 | 427240 | 163.41 | 23288 | 0.54 | 1152 | 398.11 |  |
| examples/cellular-automata.tur | 16.56 | 435237 | 209.59 | 35488 | 0.6 | 1408 | 106.39 |  |
| examples/cli_args_demo.tur | 16.84 | 433713 | 170.51 | 26720 | 0.61 | 1280 | 103.36 |  |
| examples/datalog/blog.tur | 19.03 | 477328 | 281.98 | 45928 | 0.61 | 1536 | 122.58 |  |
| examples/datalog/datalog.tur | 18.26 | 462038 | 253.94 | 35288 | 0.6 * | 1408 | 118.11 |  |
| examples/datalog/indexed.tur | 17.31 | 443231 | 203.4 | 30832 | 0.61 | 1536 |  |  |
| examples/datalog/minimal.tur | 17.48 | 451847 | 226.22 | 40624 | 0.61 | 1536 | 116.21 |  |
| examples/datalog/query.tur | 18.96 | 474063 | 269.21 | 44936 | 0.61 | 1536 | 122.69 |  |
| examples/minikanren/src/main.tur | 24.48 | 540306 | 483.48 | 62176 | 0.68 | 1664 | 174.0 |  |
| examples/reflected-measures/src/main.tur | 24.6 | 451997 | 511.92 | 31008 | 0.61 | 1536 | 114.77 | 307.91 |
| tests/fixtures/effect-nontail-resume-under-outer/input.tur | 17.09 | 469462 | 297.83 | 61608 | 7.48 | 4608 | 130.91 | 48.42 |
| tests/fixtures/effect-multishot-resume-in-loop/input.tur | 16.93 | 439530 | 196.35 | 45672 | 0.6 | 1536 | 105.89 | 10.78 |
| tests/fixtures/handle-in-nontail-branch/input.tur | 17.58 | 451840 | 233.38 | 50712 | 8.15 | 5504 | 125.05 | 29.12 |
| tests/fixtures/cps-tramp-resume-deep-1m/input.tur | 16.23 | 428749 | 169.41 | 45112 | 106.58 | 1536 | 216.78 |  |
| tests/fixtures/cps-tramp-resume-multicase-500k/input.tur | 16.55 | 429801 | 171.16 | 45192 | 110.16 | 1536 | 227.7 |  |
| tests/fixtures/cps-capturing-letrec-loop-deep/input.tur | 16.21 | 431898 | 182.51 | 45240 | 107.57 | 1536 | 227.12 |  |
| tests/fixtures/tailcall-mutual-deep/input.tur | 16.65 | 468098 | 225.07 | 66880 | 119.78 | 1536 | 337.15 |  |
| tests/fixtures/tailcall-drop-glue-deep/input.tur | 16.19 | 462476 | 220.75 | 82384 | 110.65 | 1536 | 236.03 |  |
| tests/fixtures/tailcall-carrier-let-deep/input.tur | 17.02 | 465909 | 226.87 | 71000 | 270.51 | 267264 | 506.95 |  |
| tests/fixtures/stackless-catch-unwind-deep/input.tur | 16.29 | 428661 | 172.05 | 26784 | 126.86 | 259328 | 274.62 |  |
| tests/fixtures/rc-free-queue-deep-cascade/input.tur | 16.07 | 429172 | 174.49 | 42904 | 13.96 | 13824 | 118.98 |  |
| tests/fixtures/await-fresh-spawn-future-freed/input.tur | 16.83 | 445877 | 237.98 | 59648 | 0.65 | 1536 | 113.82 |  |
| tests/fixtures/async-repeated-park-frees-each-turn/input.tur | 16.76 | 436975 | 183.28 | 46528 | 1.45 | 1536 | 106.43 |  |
| tests/fixtures/fiber-scheduler/input.tur | 15.89 | 429446 | 168.77 | 31712 | 0.65 | 1536 | 101.67 |  |
| tests/fixtures/taskgroup-linear/input.tur | 16.17 | 438729 | 163.68 | 26960 | 0.59 | 1280 | 100.54 |  |
| tests/fixtures/stm-tmvar-tchan/input.tur | 16.71 | 466607 | 234.3 | 39840 | 0.61 | 1536 | 114.59 |  |
| tests/fixtures/stm-stress/input.tur | 15.88 | 432155 | 175.26 | 35744 | 2.28 | 1664 | 96.99 |  |
| tests/fixtures/set-typed-basic/input.tur | 17.3 | 458430 | 214.79 | 59752 | 0.61 | 1536 | 115.65 | 11.15 |
| tests/fixtures/hamt-transient-basic/input.tur | 15.88 | 430381 | 163.25 | 52320 | 0.6 | 1536 | 104.8 |  |
| tests/fixtures/mut-map-reassign/input.tur | 16.41 | 441716 | 166.58 | 48144 | 0.6 | 1536 | 105.28 | 10.69 |
| tests/fixtures/string-builder/input.tur | 20.24 | 474214 | 172.17 | 61288 | 0.61 | 1536 | 105.71 |  |
| tests/fixtures/region-with-region/input.tur | 15.56 | 430112 | 174.76 | 40040 | 0.8 | 2048 | 101.6 |  |
| tests/fixtures/region-escape-via-main-loop/input.tur | 17.06 | 435087 | 185.52 | 40576 | 0.63 | 1536 | 108.02 |  |
| tests/fixtures/hkt-generic-calls-generic/input.tur | 21.67 | 495577 | 273.52 | 37752 | 0.6 | 1536 | 138.75 | 11.22 |
| tests/fixtures/stdlib-monad-entails-applicative/input.tur | 18.62 | 475255 | 240.01 | 36824 | 0.61 | 1536 | 125.72 | 10.75 |
| tests/fixtures/van-laarhoven-lens-show-deep/input.tur | 18.01 | 441027 | 196.61 | 41064 | 0.62 | 1536 | 111.06 |  |
| tests/fixtures/tailcall-dyn-deep/input.tur | 19.18 | 601734 | 523.54 | 240120 | 416.76 | 1664 | 1247.77 |  |
| tests/fixtures/docs-introducing-saffron-examples/input.tur | 20.76 | 614311 | 907.99 | 118456 | 0.73 | 1664 | 247.04 | 12.06 |
| tests/fixtures/saffron-higher-order/input.tur | 17.82 | 573315 | 607.09 | 89776 | 0.7 | 1664 | 200.14 | 10.97 |
| tests/fixtures/closure-let-in-self-tail-loop/input.tur | 16.28 | 437425 | 203.12 | 40792 | 1.14 | 1664 | 106.44 | 19.68 |
| tests/fixtures/parsec-guide-patterns/input.tur | 29.85 | 579194 | 478.92 | 96192 | 0.67 | 1536 | 170.22 |  |
| tests/fixtures/fn-value-call-join-reclaimed/input.tur | 18.73 | 457518 | 251.62 | 55248 | 1.23 | 1792 | 118.94 | 14.03 |
| tests/fixtures/httpd-mw-fold-many/input.tur | 24.44 | 652988 | 332.65 | 40928 | 0.63 | 1664 | 136.48 |  |
| benchmarks/audit/sized/effect-nontail-ticks-1m.tur | 17.6 | 469464 | 296.84 | 61600 | 336.28 | 157696 | 382.14 |  |
| benchmarks/audit/sized/cps-tramp-resume-8m.tur | 16.4 | 428749 | 169.77 | 45112 | 831.99 | 1536 | 1025.45 |  |
| benchmarks/audit/sized/closure-let-loop-10m.tur | 16.1 | 437428 | 205.64 | 40784 | 421.47 | 1664 | 527.6 |  |
| benchmarks/audit/sized/fn-value-join-1000x.tur | 18.68 | 457527 | 250.51 | 55248 | 421.78 | 41600 | 705.3 |  |
| benchmarks/audit/sized/region-rounds-100k.tur | 15.91 | 430115 | 174.72 | 40040 | 821.96 | 423424 | 1508.74 |  |
| benchmarks/audit/sized/mut-map-reassign-1m.tur | 16.36 | 441722 | 166.28 | 48152 | 1243.97 | 1177984 | 1608.99 |  |

## The corpus

[`benchmarks/audit/corpus.txt`](../../../benchmarks/audit/corpus.txt), read by
the runner; 70 programs, Turmeric only (`performance-comparison/` is not in
it):

| group | programs | what it is for |
| --- | ---: | --- |
| `benchmarks/triangle/` | 3 | pure Turmeric on all three engines; the guide's triangle |
| `benchmarks/*.tur` | 16 | the experiment harnesses; two with C ceilings (`bind-option`, `fmap-vec`) |
| `tests/benchmarks/` | 3 | one `fmap` per process -- startup cost, in effect |
| `examples/` | 9 | the closest thing to real programs; runs are milliseconds, so these are compile-latency rows |
| `tests/fixtures/` | 33 | shapes the benchmarks do not cover, picked from compiler output (below) |
| `benchmarks/audit/sized/` | 6 | fixture copies with one size literal raised, so the run is 0.4-2 s |

Left out, and why: `bench-capture-restore` (SX0(a)'s multi-minute sweep, owned
by `run-capture-curve.sh`), `examples/guestbook/src/main.tur` (an HTTP server),
`examples/snake/src/main.tur` (raylib and a display), the guestbook and snake
library modules (they build and print nothing), and every contract/refine
fixture (a Release `tur` compiles the checks away, so 14 of them print a
different answer -- correct for Release, useless as a pin).

Every program's answer is pinned: a fixture by its own `expected.stdout`,
everything else under `benchmarks/audit/expected/` (two runs, and every
engine, had to agree before a pin was written). Self-timing programs are
pinned through a mask (`cols=` / `keep-lines=`) that drops their nanosecond
fields. The three `tests/benchmarks/` programs print nothing, so their pins
check only a clean exit.

### How the fixtures were picked

All 2826 eligible fixtures (not `errors/`, no `hook.sh`, none of
`requires.{interp-only,tsan,spices,dedicated-runner,interp,musttail}`) were
run through `tur --dump-cps-coloring check`, `tur --dump-cps check` (user CPS
bodies, with their perform / handle / resume / reset / await terms counted),
and `emit-c` (a runtime-call census against a `(println 42)` baseline, `__cps`
symbols, and dictionary-dispatch sites `((void**)__dict_N)[k]` left after
monomorphization), then built and run once. Each pick's evidence:

| fixture | shape | evidence |
| --- | --- | --- |
| effect-nontail-resume-under-outer | non-tail and multishot resume (M-2) | 9 user fns colored; `let __t4 = resume k 1` then `(+ 100 __t4)`; 20 resume / 24 handle terms |
| effect-multishot-resume-in-loop | multishot non-tail resume inside `while` | `let __t8 = resume k i` then `(+ a __t8)` in the loop |
| handle-in-nontail-branch | handle in non-tail position | 11 colored; 9 handle / 8 resume terms |
| cps-tramp-resume-deep-1m | tail-resume loop, constant stack | `tailcall loop(1000000 0 <prompt>) ; cps->cps`; tail `resume k 3` |
| cps-tramp-resume-multicase-500k | the same with two effect cases | perform Add/Mul, two tail resumes |
| cps-capturing-letrec-loop-deep | capturing letrec loop under a handle | `count-up` colored; `tailcall count-up(2 1000000 <prompt>)` |
| tailcall-mutual-deep | mutual and self tail calls (`flags` = `--debug`) | 0 user fns colored: direct style |
| tailcall-drop-glue-deep | tail back-edge plus drop glue (`--debug`) | +8 `__tur_tailcall` back-edges over the prelude |
| tailcall-carrier-let-deep | tail back-edge plus carrier lets (`--debug`) | +12 `__tur_tailcall` back-edges |
| stackless-catch-unwind-deep | 500k nested catch-unwind | 0 colored |
| rc-free-queue-deep-cascade | deep rc chain freed iteratively | rc drop cascade |
| await-fresh-spawn-future-freed | async spawn + await (DK path) | 8 colored; 9 `await` terms; `tur_async_fiber` x3 |
| async-repeated-park-frees-each-turn | an await park every loop turn | 6 colored; await in `next-value` |
| fiber-scheduler | fiber yield/resume (R-10) | `tur_fiber_block_yield` x6, resume x2 |
| taskgroup-linear | task-group spawn/cancel on fibers | `task-group-spawn(-async)`, `tur_fiber_block_new` |
| stm-tmvar-tchan | STM TMVar/TChan, one thread | 23 tx new/commit, 24 tvar reads, 13 writes |
| stm-stress | STM, 8 threads x 500 | `tur_stm_*`, `tvar_swap` |
| set-typed-basic | HAMT-backed Set | 15 `set-add`, union/intersect/del, `tur_hamt_iter_*` |
| hamt-transient-basic | HAMT transients (R-5) | `hamt/transient-set!` x2, `persistent!` x1 |
| mut-map-reassign | Map assoc + `set!` in `while` | 6 `map_assoc` spec calls -> `tur_hamt_set_eq_o` |
| string-builder | StringBuilder push loop (R-9) | `builder/push-cstr!`, `tur_sb_*` |
| region-with-region | region bracket rewound in a loop (R-3) | `tur_region_push/pop_checked` in `pure-total` |
| region-escape-via-main-loop | region bracket in a CPS module fn | `with_region__spec__...__cps`, `tur_region_note_escape` |
| hkt-generic-calls-generic | dictionary dispatch surviving mono (R-11) | 15 dict-param dispatch sites in 17 `__dict_N` clones, all colored |
| stdlib-monad-entails-applicative | dispatch with superclass entailment | 18 dispatch sites, 9 colored clones |
| van-laarhoven-lens-show-deep | dispatch + deep lens composition | 3 dispatch sites; 3 colored `deep-lens__dict_N` |
| tailcall-dyn-deep | Saffron dyn tail calls (R-1; `--debug`) | `#lang saffron`; 12 colored, 0 control terms; +6 `__tur_tailcall` |
| docs-introducing-saffron-examples | Saffron broad (dyn, effects, HOFs) | 13+ colored; 5 resume terms |
| saffron-higher-order | Saffron HOFs (R-1) | 11 colored (`lmap`/`lfilter`/`vec-fold`...), 0 control terms |
| closure-let-in-self-tail-loop | R-1: fn-value call in a self-tail loop | 5 colored, 4 user `__cps` fns, 0 control terms |
| parsec-guide-patterns | R-1: parser combinators | 19 colored, 11 user `__cps`, 0 control terms |
| fn-value-call-join-reclaimed | R-1 loops via closure / value / effect | 11 colored |
| httpd-mw-fold-many | R-1: middleware fold through fn values | 4 colored, 3 `__cps` |

Shapes the fixture tree has **no** good program for, so WP2/WP3 need new
microbenchmarks rather than a bigger fixture: HAMT-heavy code at the Turmeric
level (every map/set fixture holds at most ~20 keys; transients appear only
with 2 inserts), `string/concat` in a loop (R-9), dictionary dispatch in a
loop (63 fixtures keep dispatch after mono, none loops), spawn- or
switch-heavy async (R-10; nothing spawns more than ~200 tasks), STM beyond 8
threads x 500, and Saffron with real run time (only `tailcall-dyn-deep`).

## What WP0 fixed

The plan named three harness defects; the survey found more. All are in this
package's PR.

- **Six of the 17 `benchmarks/*.tur` did not build** on `main`, and a seventh
  exited 10 every run: the three `hamt-bench-*` that still called the
  pre-rename `hamt_*` C API, `bind-option` and `fmap-vec` (the prelude now
  defines Functor/Applicative/Monad), `bench-logic-query` (inline C calling a
  Turmeric function by a guessed mangling), and `bench-logic-subst-split`
  (returned a keep-alive sum that is 10 by construction as its exit status).
- **`run-benchmarks.sh`** hard-coded the Debug/ASan `./build/tur` (so the
  2026-05-13 numbers in `benchmark-results.md` were sanitizer overhead), built
  with `emit-c` + a bare `cc` (which cannot link the runtime archives),
  declared a timeout it never applied, needed `bc`, and truncated the curated
  `benchmark-results.md` every run. It now takes `TUR`, refuses a sanitized
  binary, uses `tur build`, best of `REPS`, checks exact pins, and writes to
  `benchmarks/output/`.
- **`bind-option-baseline.c` never compiled** under `-std=c99` (no
  `<stdbool.h>`); the old runner sent the error to `/dev/null`, so no
  bind-option ratio was ever reported. It reads 1.25-1.50x now, over runs too
  short to trust (see the last table row below).
- **The sanitizer probe** is `nm`, not `grep`: a Release `tur` contains the
  string `__asan_init`, because the driver carries its own `nm` command line
  as a literal. `benchmarks/tur-probe.sh` holds it; `run-triangle.sh` (which
  only probed for the JIT) and `run-benchmarks.sh` source it.
- **Peak RSS through Python is wrong.** A child forked from Python inherits
  Python's high-water RSS into its own `ru_maxrss` at exec, so every row read
  >= ~16 MB. The runner measures through a C launcher instead (floor ~1.5 MB).
- **`bench-backtrack-n-queens`** cited a `-baseline.c` that was never written;
  the program is all inline C, so it is its own baseline, and says so.
- **`tests/run-bench.sh`** (registered with nothing, run by nothing) is
  folded into the corpus and removed; `hkt-guide.md` said its program ran
  `fmap` "in a tight loop" -- it runs one per process.
- **Seven build artifacts** under the gitignored `benchmarks/output/` and
  `tests/benchmarks/output/` had been committed on 2026-05-13, so every
  benchmark run dirtied the tree. Untracked.

Filed: [jit-string-literal-identity-differs-from-cc](../../reported/jit-string-literal-identity-differs-from-cc.md)
-- the output-parity check's first catch. `examples/datalog/indexed.tur`
answers its queries compiled and returns nothing under `tur jit`, because it
keys an index on string-literal addresses, which gcc merges and c2mir does
not. That program is cc-only in the corpus until it is fixed.

## First readings, handed to the packages that own them

What the baseline shows on its face. Every row stays a **candidate** until
its package measures it properly (WP1's `--timings`, WP2's profiles, WP3's
harness); they are here so those packages start from a number, not from
zero. Plan IDs where one fits; **new** where the survey found something the
plan did not list.

| ID | Reading | Owner |
| --- | --- | --- |
| C-1 | **Every program emits 426-653 KB of C** (median 439 KB). The 11-line `triangle/fib` emits 425,895 bytes, so the auto-loaded prelude is ~97% of the median unit. After the cc's DCE the binaries are 23-240 KB (median 42 KB). | WP1 |
| C-2 | **Settled toward the r7rs audit, pending `--timings`.** `emit-c` is 15.4-29.9 ms (median 16.7); the interpreter runs a trivial program end to end in 10.7-12 ms; a cc build is a median 191 ms, of which `emit-c` is a median 9% -- the rest is cc and link. The performance guide's "roughly 200 ms of each cell is elaboration and codegen shared by all three legs" is the cc leg's cc+link and the jit leg's ~85 ms of c2mir, and does not exist on the interpreter leg. | WP1, WP7 |
| new | **Saffron builds are 2.7-4.8x the corpus median.** The four Saffron programs' cc builds are 524-908 ms against the median 191 ms, with the same ~17-21 ms `emit-c`; their C is only ~1.3x larger (550-614 KB), so the cost is in how cc digests it. | WP1 (cc leg) |
| new | **`tur build` and `tur --interpret` of `examples/reflected-measures` peak at 345 MB and 343 MB**, with 144 / 87 ms of system time, where `emit-c` (23 MB, 25 ms) and `tur jit` (43 MB, 115 ms) of the same file do not. Something those two paths run and the other two skip costs ~300 ms and ~300 MB; the program is refinement-heavy, so verification is the first suspect. Every other build stays under 61 MB (median 33 MB). | WP1 (and M-5) |
| M-5 | `tur build` peak RSS on Release: median 33 MB, max 61 MB except the row above. | WP3 |
| E-2 | **The JIT's fixed cost is ~100 ms**: a 0.5 ms program takes 97-104 ms end to end under `tur jit`. One outlier: `tests/benchmarks/workstealing-deque-bench` takes 398 ms under the JIT for the same 0.5 ms run. | WP4 |
| E-3 | On loops that run 0.4 s or longer (the sized copies), `tur jit` is 1.08-1.82x the cc binary after subtracting the front end (c2mir not subtracted) -- the guide's "~1-2x" holds. Shorter runs read 1.5-2.7x because the ~100 ms fixed cost is in them. | WP4 |
| new | **Saffron under `tur jit` runs at interpreter speed.** `bench-saffron-dyn-tail`: jit 3187 ms, interp 3215 ms, cc 903 ms; `tailcall-dyn-deep`: jit 1248 ms against cc 417 ms. Worth asking whether the JIT path hands dyn-heavy Saffron something interpreter-shaped. | WP4 |
| E-4 | **The triangle, refreshed** (interp / jit / cc build+run, ms): fib 162 / 106 / 164, loop-sum 1526 / 108 / 164, mandel 628 / 111 / 185. The guide's snapshot was 137 / 127 / 180, 1567 / 137 / 183, 639 / 142 / 192: same shape, the JIT 15-25% faster here. | WP4, WP7 |
| R-1 | **Radius preview** (from the fixture sweep, a count, not a timing): 1055 of 2585 plain fixtures color a user function; in 701 of them (27% of all) no colored function holds a perform / handle / resume / reset / await term, so fn-value calls alone colored them; 416 of those 701 emit user `__cps` C functions. The census WP2 opens with starts here. | WP2 |
| R-3 | **A rewound region gets more expensive the longer a program runs.** `sized/region-rounds-100k` (100k rounds of an 8-node bracket) peaks at 413 MB and takes 822 ms; the fixture sweep saw 1e6 rounds take 84.5 s and 4.2 GB -- superlinear, as R-3 predicts if `tur_region_owns` walks every retired generation. The strongest candidate in hand. | WP2, WP3 |
| R-5 | `hamt-bench-transient` (100k inserts through a transient) takes 159.7 ms against `hamt-bench-insert`'s 164.3 ms persistent: transients buy 3%. Both drive the runtime from inline C. | WP2 |
| new | **`set!` of `map-assoc` keeps old versions.** `sized/mut-map-reassign-1m` (1M reassigning inserts) peaks at 1.12 GiB, ~1.2 KB per insert, against the memory guide's ~66 B live per HAMT insert. Check it under the leak harness before filing. | WP3 |
| M-2 | `sized/effect-nontail-ticks-1m` (1M non-tail resumes) peaks at 154 MiB, ~155-160 B per resume held until the outer call returns -- what the open report describes. | WP3 |
| new | `tailcall-carrier-let-deep` peaks at 261 MB in 271 ms: 5 of its 6 `^tailcall` loops keep 32-95 B per iteration (the fixture sweep's sizing). Constant stack, growing heap. The fixture has no `requires.leak-check` marker. | WP3 |
| -- | `stackless-catch-unwind-deep` peaks at 253 MB: ~520 B per nested level, by design. Recorded so nobody re-measures it. | -- |
| -- | The two C ceilings are too short to read: `bind-option` is 1.25-1.50x its baseline over runs of 4-6 ms, and `fmap-vec`'s 0.39x compares different work (its baseline grows a vector by push; the `.tur` fills an exact-size buffer). Both need sizing up, and `fmap-vec-baseline.c` needs the same allocation pattern. | WP2 |
