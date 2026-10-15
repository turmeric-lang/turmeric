# Performance audit -- Turmeric as it stands at v0.64.1

> **Status: PROPOSED; the author's five decisions are in (2026-10-11), see
> section 8.** Baseline is the author's local Intel NUC 9 Pro (i7-9850H)
> running Windows 11 with WSL2 (revised 2026-10-13 from an AWS `m7i.xlarge`;
> the setup was confirmed in WP0 on 2026-10-14); `--release` will mean `-O3` over
> an unchanged `-O2` default; the CPS coloring of fn-value calls (R-1) is
> incidental, so it is WP2's lead item and D-1 is settled now; CI tracking
> stays on hosted runners; attention is split between compile latency and
> runtime, leaning runtime when in doubt. Written 2026-10-11 against `main`
> @ 9a03eb48 (v0.64.1) from a half-day survey of the tree: the benchmark harnesses, the
> four performance guides, the open and archived reports, CI, and the
> runtime/codegen sources. Section 2 lists what the survey turned up, so the
> audit starts from a map rather than from zero; every row there is a
> *candidate* until the work package that owns it measures it. Rows marked
> **verified** were read in the source during the survey; the rest are
> doc-derived or unmeasured and still need a number.
> **Type:** Performance / measurement / tooling
> **Depends on:** nothing that is not already in the tree. `tur demangle`
> (profiling-plan P0, landed), `benchmarks/run-triangle.sh`,
> `benchmarks/run-benchmarks.sh`, the memory-usage guide's `measure.sh`, and the `ci-metrics` timing pipeline
> (`tools/ci/collect-suite-timings.py`, `publish-timings.sh`, the `/ci` page)
> are the instruments this plan builds on.
> **Related:** [profiling-plan](profiling-plan.md) (the `--timings` /
> `--profile` tooling WP1 and WP2 want; this plan does not build it twice),
> [docs/notes/r7rs-performance-audit](../notes/r7rs-performance-audit.md)
> (the template: one dialect, fully costed, 2026-10-05 -- this plan is the
> same exercise for the rest of the system),
> [performance-guide](../guides/performance-guide.md),
> [memory-usage-guide](../guides/memory-usage-guide.md),
> [gc-guide](../guides/gc-guide.md), [jit-guide](../guides/jit-guide.md),
> [suite-timing-trends-plan](../archive/suite-timing-trends-plan.md) (the
> CI timing pipeline WP6 extends),
> [post-jit-benchmark-resurrection-plan](../archive/post-jit-benchmark-resurrection-plan.md)
> (the cross-language suite's last full sweep, 2026-09-01),
> [aws-bench-box-design](../design/aws-bench-box-design.md) (the AWS
> baseline this plan named until 2026-10-13, kept for reference).

## 0. Summary

Turmeric has a lot of performance *work* behind it and very little
performance *measurement* in front of it. Every big runtime change of the
last three months (regions, by-value sums, the HAMT node fix, DK frame
reclamation, the r7rs GC rewrite) was measured once, by hand, on one machine,
and the number went into a report. Nothing re-measures it. Concretely, as of
this survey:

- **Two benchmark trees, both run by hand, neither in CI.** `benchmarks/`
  holds a dozen one-off experiment harnesses whose committed results date
  from May to September; `performance-comparison/` is a 21-program,
  nine-language suite whose raw results are gitignored and whose last full
  sweep was 2026-09-01 on an Apple M2 (out of scope here; see below).
  `tests/benchmarks/` (three programs,
  `tests/run-bench.sh`) is registered with nothing and run by nothing.
- **CI measures how long the test suites take, and nothing else.** The
  `ci-metrics` branch and the `/ci` page track suite wall-clock per
  (os, cc, nproc). No job times an emitted program, a compile, or a binary.
  The nightly arm64 timings are collected but never plotted.
- **The compiler has no phase timer.** `tur jit --timing-json` and
  `TUR_JIT_TIMING=1` exist for the JIT path only. "Why is `tur build` slow"
  is answered today by `callgrind` on a Release `tur`, which the r7rs audit
  did once.
- **The guides disagree with each other and with the code** in at least five
  places (section 2, rows D-1 to D-5), and two of the disagreements are about
  the cost model a user would design against (CPS coloring, async stacks).

The audit therefore has two jobs, in this order: **first make the system
measurable** (a locked baseline, a compiler self-timer, a regression row in
CI), **then measure it** across five surfaces -- emitted-program runtime,
memory and allocation, compile latency, the interpreter and JIT, and the
web/WASM build -- and turn what is found into ranked, costed fix directions in
the style of the r7rs audit's section 5. The audit itself lands no
optimizations beyond "exact rewrites found while profiling" (the r7rs audit's
precedent: two front-end fixes, byte-identical `emit-c` output on the whole
corpus). Everything else it finds is filed under `docs/reported/` or ranked in
section 2 for the v1 track to pick from.

What this plan is deliberately **not**:

- **Not an optimization plan.** It produces a ranked list; the ranking says
  what to build next, this plan does not build it.
- **Not a cross-language comparison.** `performance-comparison/` (the C /
  Rust / Haskell / Racket / Python / Clojure suite) is not a priority for this
  work and is not in the audit corpus. The audit measures Turmeric against
  itself: its own engines, its own C baselines where a `-baseline.c` or a
  ceiling harness exists, and its own earlier numbers. The suite stays in the
  tree for whoever wants the external comparison later; nothing here
  depends on it.
- **Not a profiler plan.** The `--timings` and `--profile` flags belong to
  [profiling-plan](profiling-plan.md). WP1 builds the minimal `--timings`
  (profiling-plan C0+C1) because the compile-time half of the audit cannot be
  done honestly without it; it does not build P1-P6.
- **Not another r7rs audit.** `#lang r7rs` was costed on 2026-10-05 and its
  open items (recommendations 1, 2, 4 and 5 of that note) stand as written;
  [r7rs-imported-libraries-recompiled-every-build](../archive/r7rs-imported-libraries-recompiled-every-build.md),
  the other one, was resolved 2026-10-12. This plan covers plain Turmeric,
  Saffron where cheap, and the engines; it cites the r7rs note rather than
  re-measuring it.

## 1. What is already measured, and what is not

| Surface | Instrument in the tree | Results kept where | Re-measured by | Gap |
| --- | --- | --- | --- | --- |
| Emitted-program runtime, Turmeric-only | `benchmarks/run-benchmarks.sh` (wall time, optional `-baseline.c`, optional `.time` tripwire) | `benchmarks/benchmark-results.md` (2026-05-13: two rows) | nobody | hard-codes `./build/tur` (a Debug/ASan build); 2 of ~12 programs have a bound or baseline; `bench-backtrack-n-queens` names a baseline C file that does not exist |
| Emitted-program runtime, cross-language | `performance-comparison/scripts/run_all.sh` + `aggregate_results.py` + `check_reproducibility.py` (CV > 10% flagged) | `results/raw/`, gitignored; golden stdout committed | nobody since 2026-09-01 | **out of scope for this plan**; noted for completeness. No stored history; `docs/methodology.md` names an M3/macOS 14.5 box, the setup note an M2 |
| Three-engine latency | `benchmarks/run-triangle.sh` (interp / jit / cc build+run, best of N, refuses on output disagreement) | pasted into performance-guide.md | by hand when the guide is edited | snapshot only |
| Per-feature heap cost | memory-usage-guide `measure.sh` (valgrind memcheck + massif, N=10000 vs N=0) | the guide's cost table (2026-10-05) | by hand | the script is prose in a guide, not a file; nothing diffs the table against a rebuild |
| Leaks in emitted code | `tests/run-leak-check.sh` (`requires.leak-check`), `docs/artifacts/leak-sweep-*` | report rows | per PR for marked fixtures | byte residue (1766 B corpus-wide) tracked by hand in `carrier-sum-option-boxes-have-no-owner` |
| Allocation census | `benchmarks/sum-census/`, `benchmarks/adt-alloc/ceiling.c`, `TUR_REPR_CENSUS` | RESULTS.md files | nobody | decision data for closed plans |
| Compile time | none general; `tur jit --timing-json`, `TUR_JIT_TIMING=1` (JIT only) | -- | -- | **no `--timings`**; the r7rs audit used callgrind by hand |
| Test-suite wall clock | ctest `--output-junit` -> `collect-suite-timings.py` -> `ci-metrics` branch -> `/ci` | append-only JSONL, per (build_type, os, cc, nproc, jit) | every CI run | suites only; nightly arm64 rows uploaded but not plotted |
| Binary / code size | `benchmarks/option-niche-size/binary-size.sh` (one experiment) | its RESULTS.md | nobody | nothing tracks emitted binary size, `libturi.a` (115 MB sanitized), or the WASM download |
| Web / WASM | `tools/ci/collect-playwright-timings.py` (test durations) | ci-metrics | every CI run | no download-size or time-to-first-eval number anywhere |
| Interpreter | `tests/check-turi-frame-reclaim.py`, `TUR_TURI_FRAME_RECLAIM`, `TUR_IC_TRACE` | pinned thresholds | per PR | correctness of reclamation, not speed |

Runtime counters that already exist and the audit should read rather than
rebuild: `TUR_REGION_STATS=1`, `TUR_GC_TRACE=1`, `TUR_GC_STATS=1`,
`TUR_REFINE_STATS`, `TUR_REPR_CENSUS`, `TUR_TRACE_EVICT=1`, `TUR_SHOW_CC=1`.

## 2. Candidate findings already in hand

Each row is a question for a work package, not a conclusion. **verified**
means read in the source on 2026-10-11; **doc** means the claim comes from a
guide or report and was not re-checked; **unmeasured** means nobody has a
number. IDs: R = emitted-program runtime, M = memory, C = compile time,
E = engines, W = web, D = documentation disagreement.

### Emitted-program runtime (WP2)

| ID | Claim | Evidence | Status |
| --- | --- | --- | --- |
| R-1 | **Any call through a function value colors the enclosing function CPS, and coloring spreads to its callers.** A CPS function pays ~2 allocations / ~200 B of continuation frames per fn-value call. Constructors, inline-C and `extern-c` callees are the only exceptions. The blast radius over a real program is unknown. **Author (2026-10-11): the rule is incidental, likely vestigial from the trampoline work, not a soundness contract.** So this is WP2's lead item: measure the radius, then price a lowering where a fn-value call whose callee has an empty effect row stays direct-style. | `src/passes/cps.c:1051-1086`, `2335-2340`; memory-usage-guide "Calls through function values, and effects" | verified (mechanism); unmeasured (radius); **decided: a cost, not a contract** |
| R-2 | **`rc` increment/decrement, `tur_region_note_escape` and `tur_region_alloc_or_malloc` are out-of-line calls into a static archive, and nothing passes `-flto`.** Every `vec-push!`/`vec-set!` makes one such call when regions are on. | `src/runtime/rc.h:247`, `region.c:222`, `region.c:332`; `src/main.c:3851` default flags | verified (shape); unmeasured (cost) |
| R-3 | **`tur_region_owns` walks every live AND retired generation and each one's slab list on every node free.** Retired generations accumulate for the life of the process, so a free gets slower the longer a program runs. | `src/runtime/region.c:337-344` | verified; unmeasured |
| R-4 | **Every `rc` allocation registers with the cycle collector's global registry even when the collector is off** (the default). | `src/runtime/rc.c:170` -> `gc.c` `gc_register_block` | verified; unmeasured |
| R-5 | **HAMT transients are not in place.** `tur_hamt_transient_set` runs `node_has` then the persistent `node_insert`, so it path-copies and traverses twice; only the shell allocation is saved. The guide says "mutate in place". | `src/runtime/hamt.c:1895-1923`; hamt-guide.md:264 | verified; unmeasured |
| R-6 | `Vec` growth is `malloc` + copy loop + `free`, not `realloc`. | `stdlib/vec.tur:136-141` | verified; unmeasured (likely small) |
| R-7 | **Emitted programs are built at `-O2 -fno-strict-aliasing -fwrapv` with no `-O3`, `-march` or LTO option, and `tur run --release` is parsed and ignored.** **Author (2026-10-11): the default stays `-O2`; `--release` adds `-O3`; `-march=native` is never on by default** (it produces binaries that will not run elsewhere). WP2's sweep decides whether `-O3` buys anything here. | `src/main.c:195`, `:3851`, `:6011` (`(void)release;`) | verified; **policy decided** |
| R-8 | `musttail` is honoured only by clang on x86-64/aarch64; gcc and the JIT get no guarantee for non-self, non-fused tail calls, and a CPS self-loop survives only through gcc's `-O2` sibling-call optimization. | `emit_module.c:10218-10232`; [cps-self-call-through-join-grows-the-stack](../reported/cps-self-call-through-join-grows-the-stack.md) | doc / reported |
| R-9 | `string/concat` copies both inputs; a loop that concatenates is quadratic (~5 KB traffic per appended character at 10k appends). Documented, with `StringBuilder` as the answer; worth a census of how often stdlib and examples do it anyway. | memory-usage-guide "Strings" | doc |
| R-10 | Async tasks each `malloc` a 1 MB fiber stack and switch through `swapcontext`-shaped glue; hand-written `fiber_ctx_{x64,arm64}.S` exist and are compiled in. Which path a task actually takes, and what a switch costs, is unmeasured. | `emit_module.c:15727-15750`, `src/async/fiber.c:21`, `src/CMakeLists.txt:399-401` | partly verified; unmeasured |
| R-11 | Dictionary dispatch through a function value inside a function also triggers R-1's coloring; monomorphization removes it only where `type_has_concrete_codegen_layout()` succeeds. How much dictionary dispatch survives in the stdlib after monomorphization is unknown. | typeclass-internals-guide, monomorphization-abi-guide | doc; unmeasured |

### Memory and allocation (WP3)

| ID | Claim | Evidence | Status |
| --- | --- | --- | --- |
| M-1 | The per-feature cost table (Vec ~19 B, MutableMap ~78 B, HAMT ~66 B live / ~720 B traffic per insert, fn-value call ~200 B, non-tail resume ~630 B) is a 2026-10-05 snapshot with no harness checked in to re-measure it. | memory-usage-guide "How the numbers were measured" | doc |
| M-2 | A non-tail or repeated `resume` holds its continuation copies until the outermost direct-style call returns. | [effect-nontail-resume-copies-held-until-outer-entry](../reported/effect-nontail-resume-copies-held-until-outer-entry.md) | reported |
| M-3 | Leak residue over the leak-checked corpus is 1766 B and byte-stable; the only row that grows with a running program is a user-written HKT-constrained generic called from a concrete caller (~40-72 B a call). | [carrier-sum-option-boxes-have-no-owner](../reported/carrier-sum-option-boxes-have-no-owner.md) | reported |
| M-4 | `Vec` of an element wider than 8 B boxes every element (~53 B peak for a 24 B struct); the guide's answer is structure-of-arrays or a region. How often the stdlib and examples hit this is unknown. | memory-usage-guide cost table | doc |
| M-5 | `tur build` peak RSS is 175-254 MB under ASan, flat across r7rs fixtures; the plain-Turmeric and Release numbers are not recorded. | r7rs audit "Not a problem" | doc |

### Compile time (WP1)

| ID | Claim | Evidence | Status |
| --- | --- | --- | --- |
| C-1 | **34 stdlib files (~9,950 lines) are auto-loaded, parsed, elaborated and emitted as `static` C into every plain-Turmeric build; cc's dead-code elimination then drops most of them.** Only r7rs has a cached library unit. | `src/compiler/stdlib_autoload.c:18-62`; `jit_prune.h:1-10` (~380 statics, ~160 KB of C for `(println 42)`) | verified |
| C-2 | **The two published front-end numbers disagree by ~5x.** The r7rs audit measured plain `emit-c` at 0.026 s (Release); the performance guide's triangle attributes ~200 ms of every engine's run to "elaboration and codegen shared by all three legs" (interpreter `fib` is 137 ms end to end). One of them is counting something the other is not. | r7rs audit section 1; performance-guide "Execution engines" | doc (contradiction) |
| C-3 | Of the r7rs front end's ~1 s, the audit left open: refinement path-condition walks (~9.5% incl.), the reader (~9%), `strcmp` on interned names (~3%). Plain Turmeric shares all three. | r7rs audit section 4 | doc |
| C-4 | Two solver reports are still open: [solver-hot-structures-linear-scans](../archive/solver-hot-structures-linear-scans.md), [refine-chain-expands-the-same-dnf-four-times](../archive/refine-chain-expands-the-same-dnf-four-times.md) (filed under archive but marked OPEN). | the reports | doc |
| C-5 | The preamble split (`libturt_preamble.a`) is default on Linux/Windows and opt-in on macOS, so macOS builds compile ~4,360 lines of runtime per program. | [cc-path-preamble-split-plan](cc-path-preamble-split-plan.md); `main.c:957-1010` | doc |
| C-6 | The ABI cache (`.tur-abi-cache/`) covers borrow-spec ownership for multi-module builds only; there is no `tur run` output cache, so an unchanged program rebuilds. | `main.c:7231`, `1861`, `11541` | verified |

### Engines (WP4)

| ID | Claim | Evidence | Status |
| --- | --- | --- | --- |
| E-1 | `eval_lookup` was 24.5% of interpreter instructions before the 2026-10-05 pointer-compare fast path; what it is now is unmeasured. A module-global lookup `snprintf`s `"M/name"` and probes a hash table, and a miss goes through `turi_errorf`. | r7rs audit section 2; `src/turi/eval.c:1501`, `env.c:936` | partly verified; unmeasured |
| E-2 | `tur jit` hands c2mir the pruned unit on every run; MIR has a binary format (`MIR_write`/`MIR_read`) and nothing caches a library's MIR. | r7rs audit section 3, recommendation 5 | doc |
| E-3 | JIT'd loop bodies run at ~1-2x of `cc -O2` (`r7rs-tail-calls-stress`: 11.4 s vs 2.6 s native), attributed to MIR code quality. | r7rs audit; performance-guide | doc |
| E-4 | The triangle numbers in the guide were measured once on x86-64 Linux; the interpreter column predates frame reclamation (30-40% faster on r7rs programs since). | performance-guide; r7rs audit follow-up | doc (stale) |

### Web / WASM (WP5)

| ID | Claim | Evidence | Status |
| --- | --- | --- | --- |
| W-1 | The WASM build embeds the whole 3.5 MB `stdlib/` directory, including the 604 KB generated `docstrings.tur`, into the download. No download size or time-to-first-eval is recorded anywhere. | `src/CMakeLists.txt:1981-2060` (`--embed-file stdlib@/stdlib`) | verified (mechanism); unmeasured |
| W-2 | Startup preloads macros, collections and typeclasses into the interpreter, then snapshots the prelude. | `src/web/wasm_glue.c:95-127` | verified; unmeasured |

### Documentation disagreements (WP7)

| ID | Claim | Where |
| --- | --- | --- |
| D-1 | effects-system-guide says hot direct-style code "pays no trampoline or allocation cost"; memory-usage-guide and `cps.c` say any fn-value call colors the function. **Settled 2026-10-11 toward the memory guide:** the coloring is real and incidental (R-1), so the effects guide's sentence is wrong today and should say so, with a pointer to R-1's fix when it lands. Can be fixed before WP2 measures anything. | effects-system-guide.md:540 vs memory-usage-guide.md:232-262 |
| D-2 | async-await-guide says fibers have no separate OS stack; the emitter `malloc`s 1 MB per `FiberBlock`. | async-await-guide.md:56 vs emit_module.c:15727 |
| D-3 | hamt-guide says transients mutate in place; they path-copy (R-5). | hamt-guide.md:264 |
| D-4 | performance-guide says mutual recursion is not turned into a loop (line ~530) and documents T5 fusion of mutual groups (lines 158-176). | performance-guide.md |
| D-5 | performance-guide says "Turmeric is refcounted, not tracing"; r7rs/saffron run under a mark-sweep collector. Its 2026-08-20 note says `run_all.sh` does not exist; it does. Stale `types.c` line numbers in type-erasure-guide. | performance-guide.md, type-erasure-guide.md |

## 3. The five surfaces and the questions each must answer

**A. Emitted-program runtime.** Where does a compiled Turmeric program spend
its cycles, by subsystem (user code, rc, regions, CPS/DK frames, HAMT, Vec,
strings, dictionary dispatch, async)? Where a C ceiling exists in the tree
(`bind-option-baseline.c`, `fmap-vec-baseline.c`, `adt-alloc/ceiling.c`),
what is the gap to it, and how much of it is the runtime (R-2..R-5) versus
the lowering (R-1, R-11) versus the C flags (R-7)?

**B. Memory and allocation.** Per feature, what does a value cost in traffic,
peak and residue, and does the cost table still hold? Which shapes allocate
per iteration where a user would expect none (R-1, M-2, M-4)? What does a
long-running program's RSS curve look like under regions (R-3)?

**C. Compile latency.** For a representative set of programs, how do
`tur build` and `tur run` split across read, elaborate, passes, emit, cc and
link? How much of that is the stdlib prelude (C-1) and is it cacheable the
way the r7rs library unit is? Which of C-2's two numbers is right?

**D. Engines.** For the same programs, what are interpreter, `tur jit` and cc
time-to-first-result and steady-state throughput today, and where does each
engine's fixed cost go (E-1, E-2)?

**E. Web / WASM and CI.** What does Try Turmeric download and how long until
the first evaluation (W-1, W-2)? Which of the above can CI watch without
drowning in runner noise (WP6)?

## 4. Work packages

Every package lands as its own PR from `main` (no stacks). A package's
measurements go in `docs/artifacts/perf-audit-2026-10/<wp>-*.md` with raw
CSV/JSONL beside them, the way `benchmarks/*-results.md` and
`docs/artifacts/leak-sweep-*` do today; a finding that outlives the audit is
filed under `docs/reported/` with a README row. Exact rewrites found while
profiling may land in the package's PR if `emit-c` output is byte-identical
across the fixture corpus (the r7rs audit's bar); anything else is a
direction, not a change.

### WP0 -- Baseline and environment lock (prerequisite for everything)

**Status: done 2026-10-15.** The baseline, the corpus (70 programs), the
harness fixes and the first readings handed to WP1-WP4 are in
[docs/artifacts/perf-audit-2026-10/wp0-baseline.md](../artifacts/perf-audit-2026-10/wp0-baseline.md);
the box and its checklist answers are in that directory's README. One
method change for every later package: on this box a single sweep can take
a multi-second burst that slows even sub-millisecond launches 10-250x, so
timings are three sweeps merged by `benchmarks/audit/merge-sweeps.py`, not
one.

**Goal.** One machine, one toolchain, one Release build, one corpus, one set
of numbers everything later is compared against.

- The box is the author's own machine (section 8, decision 1), described in
  **The baseline machine** below. Its setup is not yet confirmed, so WP0
  starts by working through that subsection's checklist and writing the
  answers into the artifact README. Every later table in this plan carries
  the header that checklist produces, or it is not a result.
- Define the **audit corpus** and commit the list. Turmeric programs only;
  `performance-comparison/` is not in it:
  - `benchmarks/*.tur` (the experiment harnesses, with their two C
    baselines) and `benchmarks/triangle/`;
  - `tests/benchmarks/` (3 programs);
  - the larger `examples/` programs that `tests/check-examples.sh` already
    builds and runs (19 of them go through `cc`, per the suite-timing
    readout), as the closest thing the tree has to real programs;
  - ~30 fixtures picked from `tests/fixtures/` to cover shapes the benchmarks
    do not: effects with non-tail resume, async, STM, HAMT-heavy, string
    builder, region-bracketed, dictionary-dispatch-heavy, Saffron, and the
    `*-deep`/`*-stress` constant-stack checks sized up. Pick them by
    `TUR_REPR_CENSUS` / `--dump-cps` output, not by name.
- Fix the harness defects that would otherwise poison every number:
  `run-benchmarks.sh` must take `TUR` like the other runners and refuse an
  ASan binary (the triangle runner's probe is the model); the missing
  `bench-backtrack-n-queens-baseline.c` is either written or the reference
  removed; `tests/run-bench.sh` is either registered (not in the PR gate) or
  folded into `benchmarks/`. Every corpus program's stdout is pinned (the
  fixtures already have `expected.stdout`; the benchmarks and examples get a
  checked-in `expected.stdout` beside them), so a later experiment cannot
  look fast by doing less.
- Run everything once, serially, and commit: wall time, user+sys CPU, peak
  RSS (`wait4` rusage, as the r7rs audit did), emitted C size, binary size,
  and for the cc path the build/run split. This is the **baseline**.

**Deliverable.** `docs/artifacts/perf-audit-2026-10/wp0-baseline.md` + CSV;
the corpus list as a file the later runners read. **Effort:** small-medium;
mostly machine time.

#### The baseline machine

**Confirmed on the box 2026-10-14** (the full header, toolchain and checklist
answers are in [docs/artifacts/perf-audit-2026-10/README.md](../artifacts/perf-audit-2026-10/README.md)).
The first draft named a NUC8i7HVK; the machine is a NUC 9 Pro.

| | |
| --- | --- |
| Machine | Intel NUC 9 Pro (`NUC9V7QNX`, "Quartz Canyon"), the author's own |
| CPU | Core i7-9850H: Coffee Lake-H, 6 cores / 12 threads, AVX2, no AVX-512 |
| Memory | 16 GB host (2 x 8 GB DDR4-2400); the WSL2 VM gets 12 GB (item 2 below) |
| Storage | 256 GB internal NVMe (the WSL disk); a 1 TB external USB SSD is attached and not used |
| Host OS | Windows 11 Home 10.0.26200 |
| Linux | WSL 2.6.1, kernel 6.6.87.2-microsoft-standard-WSL2, Ubuntu 24.04.5 |
| Hypervisor | working; the VM exposes the PMU to `perf` (item 3 below) |

What follows from the choice, stated once:

- **It is not the CI runners' shape** (`ubuntu-latest` is 4 vCPU / 16 GiB
  of a server CPU; this is 12 logical CPUs of a 2019 laptop-class part inside
  a VM with its own memory cap). Its numbers are compared with each other and with nothing on
  `/ci`.
- **A Claude Code cloud session cannot reach it.** The timing work of WP0-WP4
  runs in a local session (Claude Code inside WSL2) or by hand, and lands as
  commits like anything else. A cloud session can still do the reading, the
  harness code, the R-1 census (a count from `--dump-cps`, not a timing) and
  WP7.
- **`-march=native` in the R-7 sweep means Coffee Lake (AVX2).** That number
  belongs to this host; it is not a portable result (decision 2 already
  keeps it out of every default).

**To investigate before the first number.** The author believes the
hypervisor is set up; nothing in this plan has checked it, and WSL2 brings a
few traps a bare-metal Linux box does not. Each item ends with what goes in
the artifact header.

1. **WSL2, not WSL1, on a working hypervisor.** On Windows, `wsl -l -v`
   (VERSION column reads 2), `wsl --version`, and `systeminfo` (its Hyper-V
   section should read "A hypervisor has been detected"; if it lists
   requirements instead, virtualization is off in firmware or the Virtual
   Machine Platform feature is not enabled). Inside, `uname -r` ends in
   `-microsoft-standard-WSL2` and `systemd-detect-virt` prints `microsoft`.
   Prefer Ubuntu 24.04, what `ubuntu-latest` runs. *Record:* Windows build,
   WSL version, kernel, distro.
2. **What the VM is given.** By default WSL2 caps the VM at a fraction of
   host memory (half on current releases, so ~8 GB here) and hands it every
   logical processor, unless `%UserProfile%\.wslconfig` says otherwise. An
   interpreter fixture alone peaks at ~3.5 GiB (CLAUDE.md), so set
   `[wsl2] memory=` explicitly (e.g. `12GB`, leaving Windows 4 GB) and decide
   `processors=` and `swap=`. Turn off, or at least record, any
   `[experimental]` setting such as `autoMemoryReclaim`, which can drop the
   page cache under a running sweep. *Record:* the `.wslconfig`, `nproc`,
   `free -g`.
3. **Hardware performance counters.** Whether the WSL2 VM exposes the PMU is
   the main unknown: `perf stat -e cycles,instructions true` prints real
   counts if it does and `<not supported>` if it does not. The plan does not
   depend on it -- without counters, `perf record -e cpu-clock` still
   attributes time by sampling and callgrind (which needs no PMU) carries the
   instruction counts -- but WP2 loses its IPC and cache columns. Getting
   `perf` at all is its own step: Ubuntu's `linux-tools-*` packages are built
   for Ubuntu's kernels, not Microsoft's, so either build `tools/perf` from
   the matching WSL2-Linux-Kernel tag or call another version's binary
   directly (`/usr/lib/linux-tools/<ver>/perf`). *Record:* counters yes/no,
   the perf version, `kernel.perf_event_paranoid`.
4. **Where the tree lives.** Clone and build in the distro's own ext4 disk
   (`~/turmeric`), never under `/mnt/c`: the Windows drives are mounted over
   9P, which makes a file-heavy build and the suite many times slower and puts
   Defender's real-time scan in the path. The WSL virtual disk grows on the
   512 GB SSD; Debug + Release builds, ccache and fixture outputs want about
   80 GiB free. *Record:* the checkout path, free space.
5. **Clock speed and noise come from Windows.** Turbo and frequency follow
   the Windows power plan, not a Linux governor (`cpupower` has nothing to
   drive inside WSL2): pick one plan (High or Ultimate performance) and use it
   for every run. The NUC is a compact chassis, so run one long corpus
   program twice back to back; if the second run is slower, that is thermal
   throttling, and sweeps get a cool-down between programs. Windows
   background work (Update, Defender scans, search indexing, a browser) is
   the real threat to section 6's idle-box rule: pause updates and close what
   can be closed for a sweep, and let the CV > 10% reproducibility rule (WP6)
   mark the rows that stayed noisy anyway. *Record:* power plan, whether
   throttling was seen.
6. **Pinning is to vCPUs.** `taskset` inside WSL2 pins to the VM's virtual
   processors, which Hyper-V may run on any physical core. Use it for
   consistency, but record it as "vCPU-pinned", not as core isolation.

### WP1 -- Compile latency

**Goal.** Answer "why is `tur build` slow" for plain Turmeric the way the r7rs
audit answered it for r7rs, and settle C-2.

- Build profiling-plan **C0+C1** (`--timings`: zone timers for read, macro
  expand, elaborate, each core pass, emit, cc, link; exclusive time via a
  zone stack; a table to stderr and JSON to a path). Absorb
  `--timing-json` as that plan specifies. This is the one piece of tooling
  the audit builds, because every number in this package depends on it.
  **Landed 2026-10-14** (profiling-plan C0+C1; `--timing-json` left as is,
  its absorption being that plan's C4).
- Run `--timings` over the corpus on the Release `tur`; callgrind the top
  three programs per zone. Expect C-3's three open hot spots to reappear.
- Cost the prelude (C-1): time and bytes of the 34 auto-loaded files per
  build; how many of the ~380 statics survive DCE per program; what a
  content-hashed cached library unit for plain Turmeric would save
  (prototype with `TUR_SHOW_CC=1` and a hand-split, the way the r7rs split
  was prototyped, do not build the cache in this package).
- Cost the cc leg: emitted-C size per program, `cc -O2` time, link time, with
  and without the preamble split (C-5), under gcc and clang.
- Settle C-2: measure `emit-c` of the triangle's `fib.tur` and of
  `(println 42)` the two ways the two documents did, and say which number
  the guide should carry.

**Deliverable.** `--timings` (with a fixture and a `tur explain` entry if it
has a diagnostic); `wp1-compile-latency.md` with a ranked table of where the
front end's time goes and what each direction buys. **Effort:** medium (the
timer is ~a day; the measurement is machine time plus reading).

### WP2 -- Emitted-program runtime hot paths

**Goal.** Attribute cycles to subsystems across the corpus and price R-1
through R-11.

- Profile: `perf record -g` (Linux) on the Release-built corpus, piped
  through `tur demangle` (profiling-plan P0, landed), plus callgrind on the
  programs where `perf`'s sampling is too coarse. Group symbols into the
  profiling plan's subsystem buckets by hand (the P0a table) and report
  percentage per bucket per program.
- **R-1 census.** Over the corpus and the stdlib: how many functions are
  CPS-colored, how many only because of an indirect call (not an effect),
  and what the per-call frame traffic costs in the three most-colored
  programs. This is the single most consequential unknown in the cost model
  and the D-1 disagreement hinges on it.
- **Runtime call-boundary experiment (R-2).** Build the corpus three ways:
  as today; with `-flto` added to `TUR_CC_FLAGS` and the runtime archive;
  with the rc and region hot functions moved to `static inline` in a header.
  Report the delta per program. This prices a whole class of fixes at once.
- **R-3, R-4, R-5, R-6**: one microbenchmark each in `benchmarks/`, with a
  C ceiling the way `adt-alloc/ceiling.c` does it (region free after N
  retired generations; `rc/of` with the registry insert compiled out; HAMT
  transient batch insert vs persistent; Vec growth via `realloc`). Each
  states what fraction of the corpus could benefit before anyone builds it.
- **R-7 flags sweep.** `-O2` (today) vs `-O3` vs `-O2 -march=native` vs
  `-O2 -flto` on the corpus, gcc and clang, with the output-parity check the
  triangle runner already does. The policy is decided (section 8: `-O2`
  stays the default, `--release` adds `-O3`, `-march=native` never by
  default); the sweep says what `-O3` is worth on this codebase and whether
  LTO belongs in `--release` too. Wiring `--release` to the flags is a small
  change this package may land once the sweep has the number.
- **R-8**: count the fixtures and corpus functions whose tail calls fall into
  the gcc-no-guarantee gap, under `^tailcall`.
- **R-10**: measure a fiber switch and a task spawn; confirm which switch
  path runs on each platform.

**Deliverable.** `wp2-runtime-hot-paths.md`: a per-program subsystem table,
the R-1 census, and a ranked fix table in the r7rs audit's section-5 shape
(change / buys / effort). New microbenchmarks under `benchmarks/` with
`-results.md` files. **Effort:** medium-large; the profiling is mechanical,
the reading is not.

### WP3 -- Memory and allocation

**Goal.** Turn the memory-usage guide's snapshot into a harness, and find the
per-iteration allocations a user would not expect.

- Check in the guide's `measure.sh` as `benchmarks/memory/measure.sh` with
  one probe per cost-table row, so the table is regenerated, not retyped.
  Re-run it; diff against the 2026-10-05 table; update the guide.
- Add probes for the shapes the table does not have: a CPS-colored loop
  (R-1), a non-tail resume loop (M-2), `Vec` of a wide struct (M-4), an async
  task spawn (R-10), a region bracket rewound in a loop (R-3's steady state).
- Long-run RSS curves: three corpus programs run for 10x and 100x their
  benchmark size, RSS sampled per second. This is where R-3 and M-2 show up
  if they are real.
- `tur build` peak RSS for the plain corpus on Release and Debug (M-5), so
  the next CI timeout discussion has the number.
- Confirm M-3's residue number on the current tree with
  `tests/run-leak-check.sh`, and record it beside the baseline so drift is
  visible.

**Deliverable.** The harness; `wp3-memory.md` with the regenerated table and
the curves; guide update. **Effort:** small-medium.

### WP4 -- Interpreter and JIT

**Goal.** Refresh the engine triangle on the baseline box and attribute each
engine's fixed cost.

- Re-run `benchmarks/run-triangle.sh` on the baseline box, and the corpus
  under all three engines (E-4). Replace the guide's table.
- Interpreter: callgrind the three slowest corpus programs under
  `--interpret`; report `eval_lookup`'s share after the fast path (E-1), the
  module-global lookup path, and the `turi_errorf`-on-miss question.
- JIT: `TUR_JIT_TIMING=1` over the corpus for the c2mir / gen / run split;
  measure what `MIR_write`/`MIR_read` of the pruned prelude would save (E-2)
  with a one-off spike in `tools/jit-spike/`, which already exists for this
  kind of question.
- E-3: pick two loop bodies where the JIT is >1.5x off native and read the
  MIR; say whether it is MIR codegen or a lowering the cc path gets from gcc
  (sibling calls, R-8).

**Deliverable.** `wp4-engines.md`; refreshed guide table. **Effort:** medium.

### WP5 -- Web / WASM

**Goal.** Put a number on what Try Turmeric costs the visitor.

- Measure the production bundle: `turmeric.wasm` + `.js` + embedded stdlib
  bytes, compressed and uncompressed; time from page load to first
  evaluation result, and per-keystroke LSP latency, with a Playwright test
  that reports through `collect-playwright-timings.py` (already wired to
  ci-metrics).
- Cost W-1: what the embed loses with `docstrings.tur` served lazily and
  with the stdlib tree pre-parsed or snapshotted at build time instead of at
  startup (W-2).

**Deliverable.** `wp5-web.md`; one Playwright timing test. **Effort:** small.

### WP6 -- Regression tracking in CI

**Goal.** Make the baseline a trend, not a snapshot, without pretending a
hosted runner is a quiet machine.

- Pick at most **ten** programs from the corpus whose run time is >1 s on
  the baseline box (so runner jitter is a small fraction) and whose output
  the harness checks. Run them in the existing **nightly** arm64 job (not
  the PR gate), serially, best of 3, and emit rows in the ci-metrics JSONL
  shape: `(program, engine, metric, value, os, cc, nproc)`. Add compile
  time (from WP1's `--timings`) and emitted binary size as two more metrics
  on the same programs.
  Hosted runners are the only runners (section 8, decision 4); the
  baseline machine is the author's desktop, not a runner, so the plan
  never assumes a quiet dedicated machine in CI.
- Plot them on `/ci` as a third panel beside suite timings and line count.
  Plot the nightly arm64 suite rows that are already uploaded and never
  shown.
- Tripwires, not gates: a `.time`-style bound per program at 1.5x that
  program's own trailing nightly median (the WP0 baseline is different
  hardware, so it cannot anchor a CI bound), surfaced as a `::warning::` in
  the nightly, never a failed PR.
  CLAUDE.md's "a red suite does not block landing" applies here with extra
  force.
- Give `check_reproducibility.py`'s CV > 10% rule a home in the nightly so a
  noisy row is marked noisy instead of read as a regression.

**Deliverable.** Workflow step, collector rows, `/ci` panel, a short section
in performance-guide.md "Benchmarking methodology" saying what is tracked
and how to read it. **Effort:** medium; reuses the whole timing pipeline.

### WP7 -- Documentation reconciliation

**Goal.** Make the guides agree with the code and with each other.

- Resolve D-1 to D-5. All five are decided or mechanical and can land as
  soon as WP0 does; D-1 states the coloring as a current cost with a pointer
  to R-1, and is reworded again when R-1's fix lands.
- performance-guide.md: refresh the triangle (WP4), add a "Profiling" section
  that documents `perf | tur demangle`, `--timings` (WP1) and the runtime
  counters in section 1, and replace the "refcounted, not tracing" sentence
  with a pointer to gc-guide's four layers.
- memory-usage-guide.md: regenerate the cost table from WP3's harness and
  say so.
- Cross-link every artifact this audit produced from the guides that quote
  its numbers.

**Deliverable.** Guide PRs. **Effort:** small.

## 5. Sequencing

```
WP0 baseline ---+--> WP1 compile latency (--timings) --+
                +--> WP2 runtime hot paths -------------+--> WP6 CI tracking --> WP7 docs
                +--> WP3 memory ------------------------+
                +--> WP4 engines -----------------------+
                +--> WP5 web ---------------------------+
```

WP0 first; it is cheap and every other package reads its numbers. WP1-WP5
are independent of one another and can run in parallel sessions. The
author's priority (section 8, decision 5) is a balance of compile latency
and runtime, leaning runtime when in doubt: so WP2 and WP1 start together
after WP0, WP2's R-1 census is the first measurement taken, and when one
session has to choose, it picks WP2. WP6 wants WP1's `--timings` for its
compile-time row but can start its runtime rows from WP0 alone. WP7's D-1 to
D-5 can all land early. Nothing here blocks the v1 track; each package is a
PR from `main` that stands alone.

## 6. Method and tooling

- **Release `tur` only** for every timing number (`cmake -DCMAKE_BUILD_TYPE=Release
  -DTUR_JIT=ON`). The Debug build carries ASan+UBSan (~4x on the front end,
  per the r7rs audit) and its numbers are not comparable to anything. Memory
  *residue* numbers come from valgrind on Release binaries or LSan on the
  leak-check harness, never from ASan RSS.
- **Serial runs, idle box.** No concurrent suite, build or sweep. CLAUDE.md's
  overlap trap produces fake regressions as readily as fake failures. On the
  baseline machine "idle" covers the Windows host too (WP0, item 5). Pin to
  a vCPU set if you like and record whether you did; under WSL2 that is not
  core isolation (WP0, item 6).
- **Best-of-N for latency, trimmed mean for throughput**, as the two existing
  harnesses already do; report N. Every table carries commit, box and
  toolchain, or it is not a result.
- **Output parity before timing.** The triangle runner refuses to time a
  program whose engines disagree; every WP2 experiment (flags, LTO, inlined
  runtime) inherits that rule through the corpus's pinned `expected.stdout`
  (WP0).
- **Instruments:** `perf` + `tur demangle`, callgrind (instructions, for
  attribution), massif and memcheck (bytes), `wait4` rusage (RSS), the
  runtime counters in section 1, `TUR_SHOW_CC=1` for the cc leg,
  `--dump-cps` for the R-1 census. No new profiler beyond WP1's zone timer.
- **Float probes** follow CLAUDE.md: a literal with a fractional part first.
- **Every suite run** the audit triggers carries the 12-minute timeout.
- **Reporting shape:** the r7rs audit's. A "where the time goes" table per
  surface, a "not a problem" section (what was measured and found fine, so
  nobody re-measures it), and a ranked `change / buys / effort` table. A
  reader should be able to pick the next optimization off the last table
  without reading the rest.

## 7. Definition of done

- WP0's baseline artifact exists with the corpus list, the box header and
  every metric column filled for every corpus program on all three engines.
- `tur build --timings` prints a zone table and writes JSON; the guide
  documents it.
- Each of R-1 through R-11, M-1 through M-5, C-1 through C-6, E-1 through
  E-4 and W-1/W-2 has either a number and a ranked row, a "not a problem"
  entry, or a `docs/reported/` report with a README row. None is left as
  "candidate".
- D-1 through D-5 are resolved in the guides.
- The nightly publishes runtime, compile-time and binary-size rows for the
  tracked programs and `/ci` shows them.
- performance-guide.md's triangle and memory-usage-guide.md's cost table are
  regenerated from checked-in harnesses, dated, and say which.

## 8. Decisions (2026-10-11)

The five questions the first draft put to the author, and the answers. Each
is folded into the section it affects above; this is the record.

1. **Baseline machine: the author's local Intel NUC, Windows 11 with
   WSL2** (16 GB RAM). Named a NUC8i7HVK here at first; WP0 found a NUC 9
   Pro (i7-9850H, 6C/12T, 256 GB internal NVMe) on 2026-10-14, and section 4
   carries the confirmed table. *Revised 2026-10-13*; the first answer
   was an AWS `m7i.xlarge` bench box, and its notes are now in
   [aws-bench-box-design](../design/aws-bench-box-design.md) for reference.
   The author believes the hypervisor is set up; that, and the rest of the
   WSL2 setup, is the first thing WP0 confirms (WP0, "The baseline
   machine"). The machine does not match the CI runners' shape, so its
   numbers are compared only with each other. The nightly's arm64 can be a
   column; it is not the reference.
2. **`tur run --release`:** the default stays `-O2`; `--release` adds `-O3`;
   `-march=native` is never on by default. WP2's flag sweep (R-7) decides
   whether `-O3` is worth anything here and whether LTO joins it.
3. **CPS coloring of fn-value calls (R-1) is incidental**, likely vestigial
   from the trampoline work, not a soundness contract. It is WP2's lead item
   and D-1 is settled toward the memory guide now.
4. **CI performance tracking stays on hosted runners.** There is no budget
   for a standing dedicated runner, and the baseline machine is the
   author's desktop, not a runner. WP6's shape (nightly, serial, programs over one
   second, warnings not gates) stands, and per-PR tracking is not planned.
5. **Compile latency vs runtime throughput: a balance, leaning runtime when
   in doubt.** WP1 and WP2 start together after WP0; WP2 wins a tie.

Nothing remains open that this plan needs the author for before WP0 starts,
except access to the baseline machine: WP0's investigation (the six items
under "The baseline machine") runs on it, locally, and its answers may raise
a setup change only the author can make (a firmware switch, a Windows
feature, `.wslconfig`). Any new question a work package raises goes in that
package's artifact.
