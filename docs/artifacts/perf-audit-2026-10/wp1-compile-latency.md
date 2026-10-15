# WP1 -- compile latency

[performance-audit-plan](../../upcoming/performance-audit-plan.md) WP1: answer
"why is `tur build` slow" for plain Turmeric, settle C-2, cost the prelude
(C-1), the cc leg (C-5) and the front end's hot spots (C-3). Box and method are
WP0's ([README.md](README.md), [wp0-baseline.md](wp0-baseline.md)): NUC 9 Pro,
WSL2 Ubuntu 24.04, gcc 13.3, a Release `-DTUR_JIT=ON` `tur` built from this
branch, best of 5 per program, host idle.

## The instrument: `tur --timings`

This branch adds profiling-plan C0+C1: `--timings[=<path>]` on any compiling
command, a zone timer inside `tur` that charges **exclusive** time on every
zone change, so the phases partition the invocation's wall clock (they sum
to `wall_ms` exactly). Eighteen zones: the plan's fifteen (read, sweet,
module-load, macro-expand, elaborate, typeclass, kind-check, effect-lower,
effect-row, cps, borrow-check, refine, mono, emit, cc) plus `jit`, `run` and
`other`, without which the partition would not close. JSON is the plan's
`tur-profile/1` `kind: "compiler"` document; with no path it prints a table to
stderr. Off, every hook is one branch on a global. The guide's "Compiler
timings" subsection has the details; `tests/run-timings.sh` (ctest
`tur_timings_tests`) pins the shape, the partition and that codegen is
unchanged.

`benchmarks/audit/timings-corpus.py` runs it over WP0's 70-program corpus
(`tur build` and `tur emit-c`, best of 5 each). Raw: [wp1-phases.csv](wp1-phases.csv).

## Where `tur build` spends its time

| phase | summed over 70 programs | share | median program | max (program) |
| --- | ---: | ---: | ---: | --- |
| **cc** | 15,493 ms | **90.9%** | 175.6 ms | 940 ms (`docs-introducing-saffron-examples`) |
| emit | 551 ms | 3.2% | 7.2 ms | 19 ms |
| refine | 316 ms | 1.9% | 0.2 ms | 301 ms (`examples/reflected-measures`, see below) |
| elaborate | 312 ms | 1.8% | 4.2 ms | 7.6 ms |
| read | 128 ms | 0.8% | 1.8 ms | 2.9 ms |
| other | 94 ms | 0.6% | 1.2 ms | 8.9 ms |
| typeclass | 59 ms | 0.3% | 0.8 ms | 2.1 ms |
| module-load | 38 ms | 0.2% | 0.5 ms | 0.9 ms |
| effect-row | 36 ms | 0.2% | 0.5 ms | 0.9 ms |
| cps, borrow-check, kind-check, macro-expand, effect-lower, mono | 19 ms | 0.1% | < 0.1 ms | 0.9 ms |

The median build is 193 ms. **The front end proper (read through mono) is a
median 8.3 ms**; with emit, ~15 ms. Everything else is the C compiler.

`tur emit-c` alone (median 15.2 ms): emit 44%, elaborate 27%, read 11%,
typeclass 5%, module-load 3%, effect-row 3%, refine 2%.

## C-2, settled

The r7rs audit measured plain `emit-c` at 0.026 s; the performance guide says
"roughly 200ms of each cell is elaboration and codegen shared by all three
legs". The first is right. Emit-c is ~15 ms; the ~175 ms beside it on the cc
leg is cc and the link, and on the jit leg ~85 ms is c2mir (WP0: a 0.5 ms
program takes ~100 ms end to end under `tur jit`). The interpreter leg has
neither -- a trivial program runs end to end in ~11 ms (WP0). WP7 rewrites the
guide's sentence.

## C-1: the prelude is most of every build

An empty program, `(defn main [] : int 0)`:

| command | wall | where |
| --- | ---: | --- |
| `tur build` | 150.8 ms | cc 134.6, emit 6.8, elaborate 4.3, read 1.8, typeclass 0.8, module-load 0.5, effect-row 0.5 |
| `tur emit-c` | 14.4 ms | emit 6.1, elaborate 4.0, read 1.7, typeclass 0.8, effect-row 0.5, module-load 0.5 |
| `tur check` | 14.7 ms | the same -- **emit 6.4 ms of it**: `check` emits the C and throws it away |

It emits **425,213 bytes / 10,675 lines of C**. Against the corpus median
build (front end 8.3 ms, emit 7.2 ms, cc 176 ms): the empty program's front
end is the same 8.3 ms, its emit is 94% of the median's, and its cc is 77% of
the median's. For the median program, the auto-loaded prelude is very nearly
the whole front end and three quarters of the C compile.

**What cc does with it.** `gcc -ftime-report` on the unit `tur build` hands it
(the split build, `-DTUR_RT_SPLIT_HOSTED`):

| unit | C compiled | parsing | optimize + generate | total |
| --- | ---: | ---: | ---: | ---: |
| `triangle/fib` | 279 KB | **0.14 s (87%)** | 0.02 s | 0.16 s |
| `docs-introducing-saffron-examples` | 563 KB | 0.23 s | **0.90 s** | 1.13 s |

For an ordinary program gcc spends almost all of its time *parsing* prelude
definitions it then discards (`static`, unreferenced, dropped by DCE). The JIT
path already solves exactly this: `jit_prune` hands c2mir the pruned unit
(~160 KB for `(println 42)`, per `jit_prune.h`). The cc path does not prune.

## The cc leg (C-5)

`cc` milliseconds, best of 3, under the split (today's Linux default) and with
the preamble inline (`TUR_PREAMBLE_SPLIT=0`), gcc and clang:

| program | split / gcc | inline / gcc | split / clang | inline / clang |
| --- | ---: | ---: | ---: | ---: |
| `triangle/fib` | 150 | 232 | 167 | 258 |
| `examples/datalog/blog` | 278 | 441 | 280 | 472 |
| `examples/minikanren` | 495 | 494 | 402 | 505 |
| `docs-introducing-saffron-examples` | 946 | 953 | 926 | 847 |
| `httpd-mw-fold-many` | 322 | 429 | 349 | 481 |

The split takes 25-40% off cc for ordinary programs, as its plan said; for
Saffron and minikanren, whose cost is optimizing live code, it does nothing.
clang is no faster than gcc. **The split's price is paid at run time**: WP2's
build-mode sweep finds the inline preamble ~25% faster in cycles, because
under the split the panic flag checked after every call is an out-of-line
`tur_tls_panicking_ptr()` call ([wp2-runtime-hot-paths](wp2-runtime-hot-paths.md),
R-2). macOS (where the split is opt-in) was not measured; this box has none.

**Saffron's C is expensive to optimize, not to parse**: 0.90 s of its 1.13 s is
gcc's optimization and code generation (0.74 s in "callgraph functions
expansion"), against 0.02 s for `fib`. Its units carry far more live code --
dynamic ops lowered inline -- and that is what the four Saffron programs'
524-908 ms builds are made of.

## C-3: the front end's hot spots

callgrind on the three slowest front ends (`tur emit-c`; self cost):

| | `reflected-measures` | `parsec-guide-patterns` | `httpd-mw-fold-many` |
| --- | ---: | ---: | ---: |
| `memcmp` + `strcmp` (libc) | 4.5% | 13.7% | 11.9% |
| `skip_ws_and_comments` (reader) | 4.8% | 3.8% | 5.0% |
| `ensure_S` (CPS candidate classification) | -- | 5.9% | 4.6% |
| `arena_alloc_aligned` | 2.7% | 2.1% | 2.2% |
| `printf` family (emit) | 3.8% | 3.9% | 3.5% |
| `rat_mul` / `rat_norm` / `la_unsat` (refinement solver) | 7.1% | -- | -- |

The r7rs audit's three open items reappear in plain Turmeric: string compares
on names (`strcmp` / `memcmp`, 5-14%), the reader (4-5%), and refinement
arithmetic where refinements are dense. `ensure_S` is new and tracks R-1: it
is heaviest on exactly the programs most colored by fn-value calls, so it
shrinks if R-1's fix lands. None of these is larger than ~1-2 ms on a real
build, because the whole front end is ~8 ms.

## Not a problem (measured, fine)

- **mono, effect-lower, kind-check, macro-expand, cps, borrow-check**: under
  1 ms on every corpus program; 0.1% of build time together.
- **module-load**: 0.5 ms median -- finding and splicing the prelude files is
  not where the prelude costs.
- **`--timings` itself**: ~1,160 clock reads on a `fib` build, tens of
  microseconds.

## Found on the way

- **`tur build <file>` and `tur emit-c <file>` compile `examples/reflected-measures`
  differently.** Its `build.tur` sets `:experiments ["reflected-measures"]`;
  `emit-c` (like `check` and `run`) walks up to the manifest and turns the
  experiment on, `tur build <file>` does not (single-file build does not
  auto-discover -- CLAUDE.md, "Per-file Commands Inside a Spice"). Built,
  `^reflect is ignored`, the measures stay opaque, the solver spends **394 ms**
  in `refine` and still returns unknown (TUR-W0372, runtime check kept);
  emitted, the same file discharges every obligation in 7 ms. Two
  consequences: WP0's baseline row for this program measures the build
  without its experiment (the corpus has no per-program flags for non-fixtures
  yet), and a refinement the solver cannot decide costs ~50x one it can.
- **`tur check` emits C** (6.4 of its 14.7 ms on an empty program) only to
  discard it.

## Ranked: change / buys / effort

| # | change | buys | effort |
| --- | --- | --- | --- |
| 1 | **Prune the cc path's unit the way `jit_prune` prunes the JIT's**: drop prelude statics nothing references before handing C to cc | gcc spends 87% of a small build's cc time parsing that C; an empty build is 151 ms, of which 135 ms is cc | medium (the pruner exists; the cc path's split units and `--gc-sections` need care) |
| 2 | **A cached prelude unit** for plain Turmeric (content-hashed, like the r7rs library unit): compile the prelude's C to an object once | the ~13 ms of front end + emit the prelude costs per build, and its cc share where (1) does not reach | medium-large |
| 3 | **Keep the split's compile saving without its run-time price**: make the hosted split's TLS flags direct `extern __thread` (WP2 R-2 fix 1) | the split's 25-40% cc saving and WP2's ~25% run-time cycles at once | small-medium |
| 4 | `tur check` stops at emission (no C) | ~40% of `check` on small programs | small |
| 5 | Front-end micro: interned-name compares by pointer where `strcmp` / `memcmp` run today; the reader's whitespace loop | 5-14% and 4-5% of an ~8 ms front end -- under 2 ms a build | small each, low priority |
| 6 | Make `tur build <file>` honor the enclosing manifest's `:experiments` (or say loudly that it does not) | consistent semantics between `build` and `run`/`check`/`emit-c`; this example's 394 ms | small |
