# WP4 -- interpreter and JIT

[performance-audit-plan](../../archive/performance-audit-plan.md) WP4: refresh
the engine triangle (E-4), say where each engine's fixed cost goes, and answer
E-1 (the interpreter's lookups), E-2 (would a cached MIR image beat c2mir) and
E-3 (why some JIT'd loops run far behind cc). Box and method are WP0's
([README.md](README.md), [wp0-baseline.md](wp0-baseline.md)): NUC 9 Pro, WSL2
Ubuntu 24.04, gcc 13.3, a Release `-DTUR_JIT=ON` `tur` built from `main` @
`1076c7fa7` (#1190 and #1191, merged since, touched no code), host idle for
every timed run. Instruction counts (callgrind, `perf stat instructions:u`)
come from a second Release build with `-g` added, which does not change gcc's
code.

## E-4: the triangle, refreshed

`TUR=./build-release/tur REPS=11 bash benchmarks/run-triangle.sh`, two sweeps,
each cell the faster sweep. End-to-end wall time, the guide's columns:

| program | interpreter | tur jit | cc build | cc run | cc total |
| --- | ---: | ---: | ---: | ---: | ---: |
| fib (fib 27, call-heavy) | 165 ms | 105 ms | 170 ms | 3 ms | 173 ms |
| loop-sum (5M-iteration loop) | 1489 ms | 107 ms | 160 ms | 2 ms | 162 ms |
| mandel (float inner loop) | 647 ms | 109 ms | 178 ms | 4 ms | 182 ms |

The two sweeps agree within 3 ms everywhere but `cc build`, which spreads up
to 10% (gcc's own run-to-run noise). Against the guide's old snapshot
(another machine, so only the shape compares): the JIT leg fell from
127-142 ms to 105-109 ms, the cc leg and the interpreter's two loops moved
less than 10%, fib's interpreter cell reads 165 ms against 137 ms (WP0 has
162 ms for it on this box), and `tur jit` now beats the cc round trip by
34-40% end to end, not 25-30%.
WP0 has every corpus program on all three engines
([wp0-baseline.csv](wp0-baseline.csv)).

### Where a `tur jit` run goes

`benchmarks/audit/jit-phases.py` runs the 69 jit-engine corpus programs under
`--timings` and `TUR_JIT_TIMING=1`, best of 3. Raw:
[wp4-jit-phases.csv](wp4-jit-phases.csv).

| phase | median program | share of the corpus's summed wall time |
| --- | ---: | ---: |
| front end (read .. emit, prune; everything outside the engine) | 19.7 ms | 8.0% |
| **c2mir** | **72.1 ms** | **26.1%** |
| gen_init + load + link | 1.9 ms | 1.2% |
| run (the program, including lazy code generation) | 10.8 ms | 59.3% |
| wall | 130.4 ms | |

For the 42 programs whose run phase is under 20 ms -- what a run-edit-run
loop looks like -- the median `tur jit` is **110 ms: 19.5 ms front end,
72.2 ms c2mir, 4.7 ms run**; c2mir is 65% of the wall clock. The triangle's
JIT runs are 3.2 / 4.3 / 4.9 ms against cc's 3 / 2 / 4.

**C-2, as it reads on this engine**: the shared front end is ~20 ms, not
"roughly 200ms of each cell" (WP1 settled the cc leg the same way). The
guide's sentence is rewritten in this change. `tests/benchmarks/workstealing-deque-bench`
reports no run phase (its work runs on threads the engine's timer does not
see), so its 187 ms "front end" is mostly its run; it is the only such row.

## E-2: a cached MIR image

**The question.** Every `tur jit` hands c2mir the pruned unit. MIR can write
and read a binary image (`MIR_write`/`MIR_read`). What would a cache hit save?

**The spike.** `tools/jit-spike/mir-cache-spike.{c,sh}`: for each program,
dump the exact unit `tur jit` compiles (`TUR_JIT_DUMP_C`), pruned and with
`TUR_JIT_NO_PRUNE=1`, then time -- best of 5 in one process, and its first
(cold) repetition -- c2mir of prelude + unit, `MIR_write` of the result,
`MIR_read` of the image, and read + load + gen_init + link: the point where
`tur jit` starts the program. The harness links the same `libtur_mir.a`, and
extracts the engine's `JIT_PRELUDE` from `src/jit_engine.c` on every run so
the two cannot drift. 70 programs (WP0's 69 plus an empty one). Raw:
[wp4-mir-cache-spike.tsv](wp4-mir-cache-spike.tsv).

| median over 70 programs | pruned unit (today) | unpruned unit |
| --- | ---: | ---: |
| unit text | 129 KB (119-398) | 290 KB |
| MIR image | **8.6 KB** (4.8-117) | 64 KB |
| functions | 52 (39-476) | 448 |
| c2mir, `tur`'s own `TUR_JIT_TIMING` | **75.5 ms** (68.7-142.8) | 135 ms |
| c2mir in the harness, best / first repetition | 46.7 / 60.7 ms | 77.7 / 120 ms |
| `MIR_write` (once, to fill the cache) | 1.0 ms | 6.8 ms |
| `MIR_read` | 0.6 ms | 4.0 ms |
| **read + load + link: ready to run** | **2.0 ms** (1.1-29) | 21.2 ms |

- **A cache hit saves a median 72.6 ms per run** (41.7-132.3) against `tur`'s
  own c2mir figure, or 46 ms against the harness's warmest one. For the 42
  quick programs above, that turns a 110 ms `tur jit` into ~40 ms.
- The largest units are where the saving shrinks in ratio, not in size: the
  476-function `docs-introducing-saffron-examples` still goes from 143 ms of
  c2mir to 29 ms of read + link.
- **A whole-library image is not the shape.** Reading and linking the
  unpruned unit's 448 functions costs 21 ms, ten times a per-program hit;
  pruning is worth keeping (it also cuts c2mir 40%).
- `tur`'s own c2mir time is ~15 ms above the harness's cold repetition on the
  same text. Not chased; the likeliest difference is the heap the front end
  leaves behind (see E-3's second program).
- A key built from the unit text still pays the ~20 ms front end and the
  prune to compute it; one built from the sources and flags would skip that
  too, which is C-6's missing run cache.
- Lazy generation still happens at first call, inside the run phase; an image
  holds MIR, not machine code.

## E-1: the interpreter

Callgrind on the three slowest corpus programs under `--interpret`, outputs
checked against their pins. Raw tops: [wp4-callgrind/](wp4-callgrind/).

| | loop-sum | mandel | bench-saffron-dyn-tail |
| --- | ---: | ---: | ---: |
| instructions | 19.1 G | 7.7 G | 42.8 G |
| `eval_lookup`, inclusive | **10.6%** (35.0 M calls) | **11.7%** (11.5 M) | **4.8%** (50.0 M) |
| `turi_env_get` (the global table) | 2.2% (5.0 M) | 0.05% | 0.0% |
| `turi_errorf` calls | 99 | 99 | 99 |
| `strcmp`, self | 3.2% (30.2 M calls) | 7.5% (25.5 M) | 3.2% (50.2 M) |
| `builtin_lookup`, inclusive | -- | -- | **9.1%** (30.0 M) |
| `malloc` calls | 20.0 M | 9.1 M | 10.0 M |

- **`eval_lookup` after the fast path is 11-12% inclusive on Turmeric loops
  and 5% on Saffron**, against the 24.5% the r7rs audit measured before it.
  Of what remains, the frame-chain walk is most of it (`eval.c:1581-1599`).
- **The module-global path and `turi_errorf`-on-miss cost nothing in a
  single-file program**: 99 `turi_errorf` calls per run, all at startup, and
  one to seven `"M/name"` `snprintf`s. `env->current_module` is set only
  while a closure defined inside a `defmodule` runs.
- **Inside a module, it is the biggest single cost.** The same loop, flat and
  inside `(defmodule m (export run step) ...)`:

  | | wall | instructions |
  | --- | ---: | ---: |
  | flat | 0.82 s | 9.80 G |
  | in a module, `step` exported | **1.30 s** | **14.73 G** (1.50x) |
  | in a module, `step` private | 1.20 s | 13.61 G |

  Every reference to a non-local name builds the `"m/name"` key with
  `snprintf` (`eval.c:1609`), and when that key is not a private binding,
  `turi_env_get` builds an "unbound variable" error value with
  `turi_errorf` (`env.c:942`) that the caller throws away before probing the
  flat name. In the module run `vsnprintf` alone is 25.7% of all
  instructions and `eval_lookup` is 40.7%. A private callee hits on the first
  probe; the exported self-call still misses. 10 of the 148 stdlib files
  declare a `defmodule`, so this lands on module-structured user and spice
  code more than on the stdlib.
- **The `strcmp` calls are not lookups.** On loop-sum, 30 M of them are
  `is_cont_resume_builtin` (`eval.c:4405`) comparing every called builtin's C
  name against two continuation builtins -- twice per builtin call. On
  Saffron, every dynamic operator compares its name to `"and"`, `"or"` and the
  truthiness op (`eval.c:14315`, `:14404`): all 50 M of its calls.
- **Saffron's dynamic operators re-resolve their builtin every time.**
  `builtin_lookup` (`builtins.c:289`) scans the whole builtin table, with a
  `type_eq` per name match, on each execution of `(+ acc 1)`; with
  `type_simple` and `type_eq` it is ~12% of the run.
- The rest is the tree-walker's shape, not a hot spot: `eval_drive_ex` is
  ~40% self on the Turmeric loops, and the top self lines on every program
  are `eval_expr_impl`'s entry and exit (`eval.c:12385`, `:14941`).

## E-3: two programs where the JIT is >1.5x behind cc

Run phase under `tur jit` (`TUR_JIT_TIMING`) against WP0's native run, for
corpus programs whose native run is over 50 ms:

| program | jit run | cc run | ratio |
| --- | ---: | ---: | ---: |
| `bench-saffron-dyn-tail` | 3302 ms | 903 ms | **3.66x** |
| `tailcall-dyn-deep` (cc at `-O0`, its `flags`) | 1121 ms | 417 ms | 2.69x |
| `tailcall-mutual-deep` (cc at `-O0`) | 248 ms | 120 ms | 2.07x |
| `bench-clone-overhead` | 255 ms | 133 ms | **1.91x** |
| `tailcall-carrier-let-deep` (cc at `-O0`) | 411 ms | 271 ms | 1.52x |
| `fn-value-join-1000x` | 628 ms | 422 ms | 1.49x |
| 14 more | | | 0.84-1.43x |

The two read are the two whose cc leg is `-O2`. **Neither gap is MIR's
arithmetic codegen.** The first is a lowering gcc does and c2mir does not;
the second is the process the program runs in.

### `bench-saffron-dyn-tail`: every struct copy is a call to `memcpy`

The JIT profile is 35% `__memmove_avx_unaligned_erms` (glibc's `memcpy`).
An `LD_PRELOAD` counter (`tools/jit-spike/memcount-preload.c`) finds
**550 million `memcpy` calls under `tur jit`, every one of 16 bytes, all
from generated code; the native binary makes 2.** c2mir's `block_move`
(`external/mir/c2mir/c2mir.c:12431-12441`) emits a call to libc `memcpy` for
any struct copy over 5 bytes, and a Saffron value is a 16-byte struct passed
by value. The MIR for `run` (`tools/jit-spike/mir-dump-fn.c` shows a
function's MIR and MIR-gen's final code) opens with three of them, one per
parameter:

```
call  memcpy_p, memcpy, I_0, fp, I0_f, 16
call  memcpy_p, memcpy, I_2, I_1, I0_n, 16
call  memcpy_p, memcpy, I_4, I_3, I0_acc, 16
```

MIR-gen keeps them as calls; gcc turns each into two moves. **A spike**
(14 lines in `block_move`, in a scratch checkout, never the vendored tree:
a copy of at most 32 bytes in whole words becomes word loads, then word
stores) gives:

| | memcpy call (today) | inlined | |
| --- | ---: | ---: | ---: |
| `bench-saffron-dyn-tail` run | 3225 ms | 2298 ms | **-29%** |
| `tailcall-dyn-deep` run | 1127 ms | 816 ms | **-28%** |
| `fn-value-join-1000x` run | 628 ms | 612 ms | -2% |
| `mandel` run | 4.5 ms | 4.5 ms | 0 |

Output parity over all 69 jit corpus programs: 65 byte-identical, 4 differ
only in the self-timing columns they also differ in between two runs of the
unpatched build, no exit-code change. Nine corpus programs make more than a
million such calls ([wp4-jit-memcpy.tsv](wp4-jit-memcpy.tsv): Saffron, the
dynamic and mutual tail-call fixtures, `fn-value-join`, the logic/regions
substitution benchmarks; its run times were taken with the counter loaded,
so read only the counts).

What is left of Saffron's gap after the fix is mostly allocation:
`calloc`/`free` are ~33% of the patched profile, where the native binary
allocates through the collector (`tur_gc_malloc` and its map, ~60% of the
native profile) -- the JIT switches that collector off (`TUR_JIT_ENGINE`, the
`src/jit_engine.c` prelude). The out-of-line TLS accessors
(`tur_tls_tb_desc_ptr` and siblings, WP2's R-2) are ~3%.

### `bench-clone-overhead`: the heap, not the code

All inline C: 1000 rounds of building and freeing a 10,000-cell list, so both
engines spend the run in `malloc`/`free`. Under `tur jit` (less an empty
program's cost) the run executes **+17% instructions but 2.1x the cycles and
~65x the cache misses** (1.28 M against 19.7 K) of the native binary. The
cells come from the `tur` process's heap after the compiler has used it, not
a fresh one. It is not the entry thread's malloc arena: `MALLOC_ARENA_MAX=1`
makes it no faster (270 ms against 250). Heap locality is the remaining
explanation, not confirmed further.

## R-10, corrected: compiled `async` never switches stacks

WP2 attributed the ~813 ns of an `async` park/resume turn to glibc
`swapcontext` and its `rt_sigprocmask`, from the symbols the binary links.
**Linking is not calling.** An `LD_PRELOAD` counter
(`benchmarks/audit/ctxcount-preload.c`) over the two fixtures WP2 sized:

| fixture | `swapcontext` | `getcontext` |
| --- | ---: | ---: |
| `async-repeated-park-frees-each-turn` | 0 | 0 |
| `await-fresh-spawn-future-freed` | 0 | 0 |
| `fiber-basic` (the explicit fiber API, control) | 2 | 1 |

Compiled `async`/`await` parks a copy of its CPS continuation instead (WP7,
#1191, found the same by probing the emitted C, and the async guide now says
so). A 1M-turn copy of the park fixture runs in 0.81 s -- WP2's number --
and its profile is **~69% `malloc`/`calloc`/`free`** (including
consolidation), 6% `memset` (`calloc`'s zeroing), 7% `dk_copy_node` and 4% the
reap list. The turn is allocation-bound; the fix is to pool the continuation
nodes, not to emit an assembly switch, which would help only `fiber-new` and
`task-group-spawn`. WP2's R-10 section and its ranked row 5 are corrected to
point here.

## Not a problem (measured, fine)

- **gen_init + load + link**: 1.9 ms median, 1.2% of the corpus's wall time.
- **Lazy generation** stays inside the run phase and is not visible in the
  triangle programs' 3-5 ms runs.
- **The interpreter's global table** (`ht_find`): 1.9% on the loop with the
  most global references.
- **`turi_errorf` in a single-file program**: 99 calls per run, at startup.
- **The JIT on plain Turmeric loops**: mandel's JIT run is 4.9 ms against
  cc's 4-5 ms; the >1.5x cases above all trace to struct copies or the heap.

## Ranked: change / buys / effort

| # | change | buys | effort |
| --- | --- | --- | --- |
| 1 | **Cache the MIR image of a program's pruned unit** (E-2), keyed on the unit (or, with C-6, on sources and flags) | ~72 ms of a ~110 ms quick `tur jit` run: c2mir is 65% of it | medium |
| 2 | **c2mir: inline small struct copies** in `block_move` (E-3) -- one MIR fork change, batched per the one-PR rule | -29% on Saffron's dynamic-call JIT runs, -28% on `tailcall-dyn-deep`; anything that passes 16-byte values by value | small |
| 3 | **Interpreter: resolve a module reference's qualified key once, and probe with `turi_env_find_binding`** instead of `turi_env_get` (E-1) | the 1.5x instructions module code pays per non-local reference | small |
| 4 | **Interpreter: cache a dynamic operator's builtin per call site; compare names by interned pointer**; flag continuation builtins on `BuiltinSpec` | ~15% of interpreted Saffron, ~3% of the Turmeric loops | small |
| 5 | **Pool async continuation nodes; skip `calloc`'s zeroing** (R-10, corrected) | most of the ~813 ns park/resume turn | medium |

## Not done here

- **Sibling calls (R-8) as an E-3 cause.** The two mutual/dynamic tail-call
  fixtures build their cc leg at `-O0` (`flags` = `--debug`), so their ratios
  are not against gcc's `-O2` sibling-call optimization; no `-O2` corpus
  program isolates it.
- The ~15 ms between `tur`'s c2mir time and the harness's (E-2), and
  `bench-clone-overhead`'s heap-locality explanation, are measured gaps, not
  explained ones.
- An `-O2` comparison for Saffron's allocation path (the JIT's `calloc`
  against the native collector) after the copy fix.
