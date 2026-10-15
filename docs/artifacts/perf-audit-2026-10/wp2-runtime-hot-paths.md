# WP2 -- emitted-program runtime hot paths

[performance-audit-plan](../../upcoming/performance-audit-plan.md) WP2. Box,
toolchain and method are the WP0 ones ([README.md](README.md),
[wp0-baseline.md](wp0-baseline.md)); every number here was taken on the
NUC 9 Pro under WSL2 with a Release `-DTUR_JIT=ON` `tur` built from this
branch. Programs named `sized/...` are WP0's sized fixture copies under
`benchmarks/audit/sized/` ([#1187](https://github.com/turmeric-lang/turmeric/pull/1187)).

## R-1 census -- how much is CPS-colored, and why

**The question** (the plan's lead item, decided 2026-10-11 to be a cost, not a
contract): any call through a function value colors the enclosing function
CPS, and coloring spreads to its callers. How many functions does that rule
color that no effect would?

**The instrument.** `TUR_CPS_CENSUS=1` (this branch, `src/passes/cps.c`)
makes `cps_color_program` run its fixpoint a second time with the
conservative rule switched off -- an unresolved call no longer counts as
reaching a control op -- and print one line per function node:

```
cps-census <colored|uncolored> <cause> <ind|-> <module|-> <file> <name>
```

`cause` is `control` (the body holds a perform / handle / resume / shift /
await itself), `callee` (a resolved callee reaches one, or an effectful
fn-value argument force-colored it), or `indirect`: colored in the real run
and **not** in the effect-only run, so colored by the conservative rule and
nothing else. Codegen is byte-identical with the variable set.
`benchmarks/audit/cps-census.py` runs `emit-c` with it over the tree and
aggregates; a function defined under `stdlib/` counts as stdlib (the
auto-loaded prelude is top-level, not a module, so the module column cannot
tell it from user code).

**The sweep**: every `tests/fixtures/*/` program (not `errors/`), every
`examples/**/*.tur` and `benchmarks/**/*.tur` -- 3035 programs, 3015 compiled
with user functions (13 failed: interpreter-only `-turi` fixtures, a
debugger fixture, and `bind-option` / `fmap-vec`, which main still cannot
build; WP0 fixes them). Raw: [wp2-census/](wp2-census/) (`programs.csv` one
row per program, `stdlib.csv` one row per stdlib function seen colored).

| | functions | share |
| --- | ---: | ---: |
| user functions | 19,767 | |
| colored | 7,065 | 36% of all |
| ... by their own control op | 1,299 | 18% of colored |
| ... through a callee that reaches one | 676 | 10% of colored |
| ... **by the indirect-call rule alone** | **5,090** | **72% of colored** |

| | programs |
| --- | ---: |
| with any colored user function | 1,191 (39%) |
| with at least one indirect-only colored function | 725 |
| **whose every colored function is indirect-only** | **606 (20%)** |

By dialect (user functions only):

| dialect | programs | user fns | control | callee | indirect-only | programs all indirect-only |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| turmeric | 2,769 | 14,810 | 1,275 | 482 | **1,968** | **481** |
| r7rs | 137 | 4,235 | 0 | 180 | 2,977 | 89 |
| saffron | 109 | 722 | 24 | 14 | 145 | 36 |

In plain Turmeric -- not Scheme, where every procedure call goes through a
value -- more than half of the colored functions (1,968 of 3,725) hold no
effect at all, and one program in six is colored for no effect anywhere in it.

**The stdlib.** 4,406 distinct stdlib functions appear across the sweep;
1,186 are colored in some program, and **1,163 of those (98%) only by the
indirect rule** (14 by their own control op, 9 through a callee). Every
program pays for about 21 of them, because they are auto-loaded and take a
function argument: the Option/Result `Functor` / `Applicative` / `Monad`
instances, the `Eq` instances of `Map` and `MutableMap`, `option-map`,
`option-eq?`, `result-map`, `list-eq?`, `__cons-fmap`, `map-eq-loop`,
`consume`, `bt-scope`, `with-untrailed` -- and `with-region`, so every region
bracket's thunk call runs on the CPS path. Each of these takes its function
argument at a type with no effect row (`with-region [^fat body : (fn [] A)]`).

### Pricing the lowering: a spike

The plan asks to "price a lowering where a fn-value call whose callee has an
empty effect row stays direct-style". The spike -- a local patch, **not in
this branch** -- does exactly that: in `cps_collect_calls`'s unresolved-call
arm, a call through a value whose static type is `TY_FN` with an empty effect
row no longer sets `has_indirect`:

```c
const Type *ct = e->as.call_.fn_binding ? &e->as.call_.fn_binding->type
               : (e->as.call_.fn_expr ? &e->as.call_.fn_expr->type : NULL);
bool pure = ct && ct->kind == TY_FN
         && effect_row_is_empty((EffectRow *)ct->as.fn.effect_row);
if (e->as.call_.ctor == NULL && !callee_inline_c && !pure)
    self->has_indirect = true;
```

**Radius.** Census on the spike: colored user functions 7,065 -> 5,432
(-23%), indirect-only 5,090 -> 3,511, programs with any colored function
1,191 -> 832, programs colored for no effect at all 606 -> 254; stdlib
functions colored somewhere 1,186 -> 935 (`with-region`, `bt-scope`,
`with-untrailed`, `option-map`, `list-eq?`, `consume`, `map-eq-loop` and the
Option instances among the ones that go; `__cons-fmap` stays, because its
`^fat f` parameter is untyped and so not a `TY_FN`).
r7rs is untouched -- its procedures are `Any`-typed values, not `TY_FN`.

**It is unsound.** An empty effect row on a function type does not mean
"pure": the type checker lets an effectful lambda flow into a slot typed
without a row. The spike stops 7 fixtures compiling that `main` compiles,
each with "this effect operation has no lowering here":
`cps-capturing-letrec-loop-deep`, `cps-capturing-local-lambda-called-in-place`,
`cps-effectful-callback-aggregate-args`, `cps-join-self-call-unoptimized`,
`cps-local-lambda-called-from-another-closure`, `cps-wide-adt-param-perform`,
`generic-hof-unannotated-fat-param-mixed`. Two shapes: a local lambda called
through its binding (the binding's `TY_FN` carries no inferred row), and an
effectful lambda passed to a generic HOF's unannotated function parameter.

Run-time check (contention-proof: answers, not times): every fixture whose
emitted C differs between `main` and the spike, built and run with both.
That is all 2,857 eligible fixtures -- the stdlib's own coloring changes, so
every unit's C does. 2,850 print the same answer; 6 are the compile refusals
above (the seventh, `cps-join-self-call-unoptimized`, is only exercised by its
`hook.sh`); and **one compiles and then aborts**:
`cps-local-fn-alias-called-in-place` prints `tur: unhandled effect (tag 2)`
and exits on SIGABRT, because a local alias's call was left direct-style and
the handler was never threaded to the `perform`. (A full `tests/run.sh` on
the spike's Debug build was attempted and overran its 12-minute timeout
while another session was compiling on the same box -- not a result.)

**The price**, on programs both builds compile and agree on (user-space
instructions from `perf stat -e instructions:u`, one run of 5; heap
allocations from valgrind; `main` -> spike):

| program | colored | instructions | heap allocs |
| --- | --- | --- | --- |
| `tests/fixtures/parsec-guide-patterns` | 4 -> 0 | 508,202 -> 276,716 (**-45.5%**) | 868 -> 502 |
| `tests/fixtures/httpd-mw-fold-many` | 2 -> 0 | 360,968 -> 208,038 (**-42.4%**) | 506 -> 212 |
| `sized/region-rounds-100k` | 3 -> 0 | -1.2% | 700,054 -> 300,042 (-4 per bracket) |
| `sized/fn-value-join-1000x` | 14 -> 14 | 0.0% | unchanged |
| `sized/effect-nontail-ticks-1m` | 9 -> 9 | 0.0% | unchanged |
| `sized/cps-tramp-resume-8m` | 3 -> 3 | 0.0% | unchanged |
| `triangle/fib`, `bench-parsec-json` | 0 -> 0 | 0.0% | unchanged |

**And it is not always a saving.** `closure-let-in-self-tail-loop`'s `loop`
calls a closure it made itself, then recurses -- the shape R-1's sized copy
measures:

| iterations | `main` instructions | spike instructions | allocs (both) |
| ---: | ---: | ---: | ---: |
| 10,000 | 5,198,210 | 6,002,535 (+15%) | 13,032 / 13,020 |
| 100,000 | 39,398,247 | 48,037,474 (+22%) | 103,032 / 103,020 |
| 1,000,000 | 381,398,303 | **segfault** (8 MB stack) | 1,003,032 / -- |

Uncolored, `loop` is emitted direct-style, and its self-call is **not a tail
call**: `TUR_CLOSURE_DROP(f)` and a `tur_panicking` check follow it, so the C
stack grows one frame per iteration (with `ulimit -s unlimited` the spike
prints the right answer). Colored, the same loop runs on the CPS trampoline:
constant stack, and cheaper. The coloring the plan calls incidental is, for
this shape, what makes the loop a loop -- and there is no per-call frame
traffic to save (one allocation per iteration either way: the closure).

**What R-1's fix needs**, then, ranked:

1. **Effect rows that mean something on function values.** Either infer a
   lambda's row and check it at the HOF parameter it flows into (so
   `(fn [A] B)` with no row really is pure), or a parameter-level analysis
   like the existing `may_hold_awaiter` -- "some call site hands this
   parameter a colored function" -- that makes `has_indirect` a property of
   what actually flows in. Without one, any narrowing of the indirect rule
   is unsound (the 7 fixtures above).
2. **Direct-path tail calls with drop glue after them.** Drop a dead closure
   before the self-call (it is dead once `(f 1)` has run), so the call is in
   tail position; otherwise uncoloring a loop trades a trampoline for stack
   growth (R-8's gap, from the other side).
3. With both: the 42-45% on combinator-style code above, 4 allocations per
   region bracket, and ~250 stdlib functions off the CPS path.

## R-2 and R-7 -- build modes: the preamble split, -O3, -march=native, LTO

**The question.** R-2: rc, region and other runtime helpers are out-of-line
calls into static archives, and nothing passes `-flto`. R-7: emitted programs
are built `-O2` with no `-O3`, `-march` or LTO option. Both are questions about
how the program's C is compiled, so one sweep answers both.

**What a default Linux build links.** `tur build` compiles the program with
`-DTUR_RT_SPLIT_HOSTED` and links `-lturt_preamble -lturt_runtime`
(cc-path-preamble-split-plan, default on Linux and Windows since the split
landed): the fixed runtime preamble -- 4,360 lines the program used to carry
inline -- is a prebuilt archive, which took 29% off the cc call. Everything in
it is now an out-of-line call that gcc cannot see into.

**Six modes**, each a `TUR_CC_FLAGS` / environment setting on the same `tur`
(no code change):

| mode | how |
| --- | --- |
| default | today: split preamble, `-O2 -std=c99 -Wall -fno-strict-aliasing` (+ `-fwrapv`) |
| nosplit | `TUR_PREAMBLE_SPLIT=0`: the preamble compiled into the program's own TU |
| O3 | `-O3` instead of `-O2` |
| native | `-O2 -march=native` (Coffee Lake, AVX2: this box only) |
| lto | `TUR_PREAMBLE_SPLIT=0 TUR_RUNTIME=source` + `-flto`: preamble inline AND the runtime sources (rc, gc, hamt, region, ...) in one LTO link |
| lto-O3 | the same at `-O3` |

over the 24 corpus programs that run long enough to measure (fixtures built
with `--debug` left out, since their `-Og` would be overridden). Every mode's
output matched the default's (the three self-timing programs excepted from the
comparison). Metric: **user-space instructions** (`perf stat -e
instructions:u,cycles:u -r 3`), which this box's interference bursts do not
move; cycles are given in parentheses and are **provisional** -- the sweep's
first programs ran while another session's test suite was loading the
machine, which inflates cycles (not instructions) through hyperthread
contention. Raw: [wp2-build-modes.csv](wp2-build-modes.csv).

| program | nosplit | O3 | native | lto | lto-O3 |
| --- | ---: | ---: | ---: | ---: | ---: |
| triangle/fib | **-98.7%** | -20.1% | 0.0% | -98.8% | -98.8% |
| triangle/mandel | -3.8% | +0.2% | -14.8% | -3.9% | -7.7% |
| bench-backtrack-n-queens (inline C) | 0.0% | -24.8% | -1.6% | 0.0% | -24.8% |
| bench-dfs-queens | -8.8% | -1.8% | -0.3% | -21.2% | -26.1% |
| bench-logic-subst | **-49.1%** | -0.3% | -3.6% | -51.7% | -52.0% |
| bench-regions-subst | -12.1% | -0.3% | -3.4% | -15.1% | -15.9% |
| hamt-bench-insert / lookup / transient | 0.0% | 0.0% | 0.0% | -0.5% | -1.2% |
| cps-tramp-resume-deep-1m | -17.8% | -1.4% | +0.1% | -17.8% | -22.0% |
| cps-capturing-letrec-loop-deep | -18.0% | -1.5% | 0.0% | -18.0% | -22.0% |
| effect-nontail-resume-under-outer | -12.2% | -0.9% | +0.1% | -12.2% | -17.0% |
| handle-in-nontail-branch | -12.2% | 0.0% | -0.1% | -12.2% | -17.4% |
| rc-free-queue-deep-cascade | -0.4% | 0.0% | 0.0% | -14.2% | -14.5% |
| sized/cps-tramp-resume-8m | -17.8% (-36.4% cyc) | -1.4% | +0.1% | -17.8% | -22.0% |
| sized/closure-let-loop-10m | **-30.5%** (-56.8% cyc) | 0.0% | 0.0% | -28.9% | -34.2% |
| sized/fn-value-join-1000x | -16.9% (-32.0% cyc) | 0.0% | -0.1% | -16.9% | -23.4% |
| sized/effect-nontail-ticks-1m | -12.3% (-19.2% cyc) | -0.9% | +0.1% | -12.3% | -17.1% |
| sized/mut-map-reassign-1m | 0.0% | 0.0% | 0.0% | -2.2% | -5.7% |
| **geomean cycles ratio** (provisional) | **0.749** | 0.950 | 1.044 | 0.747 | 0.691 |

(`bench-clone-overhead`, `bench-parsec-json`, `bench-saffron-dyn-tail`,
`stackless-catch-unwind-deep`, `sized/region-rounds-100k` move by under 6% in
every mode; the CSV has them. `region-rounds-100k` reads +73% *cycles* at
equal instructions in all three inline modes -- a memory-side effect of layout,
not chased.)

**What it says.**

- **The preamble split is the R-2 cost, and it is large**: compiling the
  preamble inline is worth ~25% of cycles across these programs and 12-50% of
  instructions on anything that calls through CPS, effects or regions. LTO
  across the separately compiled runtime sources (rc, hamt, region) adds
  almost nothing on top -- 0.749 vs 0.747 -- except where rc frees dominate
  (`rc-free-queue-deep-cascade`, -14%) or a search allocates in a loop
  (`bench-dfs-queens`).
- **The mechanism is one accessor.** Under `TUR_RT_SPLIT_HOSTED` the panic
  flag the emitter checks after every panic-capable call is
  `#define tur_panicking (*tur_tls_panicking_ptr())`
  (`src/runtime/generated/tur_rt_split_decls.h:709`) -- an out-of-line call to
  a function in `tur_tls.c` that returns the address of a `static __thread`.
  In `fib` that call is 17.6% of the profile by itself, and because it is
  opaque gcc can no longer transform the recursion at all: inline, the same
  program runs 98.7% fewer instructions. The accessor exists so a JIT or
  shared-library build can share the thread-local; a statically linked cc
  build does not need it.
- **`-O3` is worth ~5%**, almost all of it on inline-C-heavy code
  (`bench-backtrack-n-queens` -25% instructions). On emitted Turmeric it moves
  nothing measurable.
- **`-march=native` buys nothing** on this corpus (the one -15% is `mandel`'s
  float loop), and the plan already keeps it out of every default.

**Fix directions** (ranked):

1. **Make the hosted split's panic flag a plain extern thread-local** (or, at
   least, declare the accessor `__attribute__((const))` so gcc can hoist it
   out of the call sequence). Same treatment for the other `*_ptr()` TLS
   accessors the split routes through (`tur_handler_chain`, ...). This should
   recover most of the 25% while keeping the split's compile-time saving.
   Caveat to check first: a fiber that migrates between OS threads must not
   keep a cached TLS address across a suspension point -- the same question
   applies to the inline preamble's `__thread` today.
2. **`--release` = `-O3` plus the inline preamble** (or (1)), per the plan's
   decision 2. LTO is not worth its link time here.
3. Leave `-march=native` out, as decided.

## Where the cycles go, by subsystem

`benchmarks/audit/subsys-profile.py`: `tur build` (default flags), `perf
record -F 4999` (self time, user + kernel), every sampled symbol bucketed with
profiling-plan 4.3's prefix table plus three this audit needed -- `region`
(`tur_region_`, `reg_`, `arena_`), `tls` (the split preamble's
`tur_tls_*_ptr` accessors, R-2) and `kernel` (page faults, `brk`/`mmap`, exit
teardown). `user` and `stdlib` are told apart by name through
`tur demangle` and the program's own `TUR_CPS_CENSUS` file column. Percent of
samples; buckets under 1% omitted. Raw: [wp2-micro/subsys.csv](wp2-micro/subsys.csv).

| program | biggest buckets |
| --- | --- |
| triangle/fib | user 75, kernel 12, **tls 8**, libc 5 |
| bench-logic-subst | stdlib 59, **tls 13**, user 12, kernel 10 |
| bench-regions-subst | stdlib 57, user 11, **tls 10**, region 6, libc 6 |
| bench-dfs-queens | libc 26, kernel 20, native 19, trail 10, region 7, user 7, tls 6 |
| bench-saffron-dyn-tail | **gc 51**, user 22, native 21 |
| hamt-bench-insert | **hamt 69**, libc 13, native 12 |
| sized/cps-tramp-resume-8m | libc 40, **cps 36**, native 15, tls 9 |
| sized/closure-let-loop-10m | **cps 45**, libc 27, tls 16, native 11 |
| sized/fn-value-join-1000x | libc 49, **cps 33**, tls 8 |
| sized/effect-nontail-ticks-1m | kernel 48, libc 25, cps 17 |
| sized/region-rounds-100k | **region 73**, kernel 19 |
| sized/mut-map-reassign-1m | hamt 30, kernel 30, native 29, libc 11 |
| stackless-catch-unwind-deep | kernel 58, libc 33 |
| tailcall-carrier-let-deep | libc 53, kernel 28 |
| rc-free-queue 2M (R-4 below) | libc 47, kernel 27, rc 10, gc 4 |
| vec-push 10M (R-6 below) | kernel 55, user 35, region 8 |
| async spawn+await 2M (R-10 below) | libc 56, cps 20, native 15, tls 8 |

Read across: **`malloc`/`free` is the largest runtime bucket** wherever CPS,
effects, async or rc are in play (25-56%) -- DK frames, continuation copies
and fiber closures are individually heap-allocated; **`kernel` is large
exactly where memory grows** (M-2's held continuations, the carrier-let
retention, catch-unwind depth -- page faults, not syscalls); and **`tls` --
one accessor -- is 2-16% of nearly everything** (R-2). `native` is symbols the
table cannot place (monomorph `__spec__` clones whose base name the census
does not list, and the Saffron collector's helpers); it is never the top
bucket.

## R-4 -- rc allocation and the disabled collector

Every `rc` allocation registers with the cycle collector's registry and every
free unregisters, even with the collector off (the default): `rc_cb_alloc_kinded`
-> `gc_register_block` (`src/runtime/gc.c:342`, an append to a growable array
after an allocation checkpoint that returns at once unless `GC_AUTO`), and
`gc_unregister_block` + `gc_remove_suspect` on the way out. Priced on
`rc-free-queue-deep-cascade` built 20x deeper (2M linked `rc` blocks, built and
cascaded free, 267 ms): `gc_unregister_block` is 2.2% of samples and
`gc_register_block` does not reach the top 14; `malloc` / `free` and glibc's
consolidation are ~45%. **Not a problem**: the registry costs a few percent of
an rc-heavy program, and the allocator is the cost.

## R-5 -- HAMT transients path-copy

100k inserts (WP0's inline-C drivers, same keys, same order):

| | wall | instructions | heap allocs | heap bytes |
| --- | ---: | ---: | ---: | ---: |
| persistent `tur_hamt_set` | 177 ms | 1,720,879,185 | 866,177 | 93.4 MB |
| transient `tur_hamt_transient_set` | 173 ms | 1,701,872,284 | 766,179 | 87.0 MB |

The transient saves exactly one allocation per insert (100k: the map shell)
and 7% of the bytes; the rest of the work is the same. Its profile says why:
**56.9% in `tur_hamt_node_release`, 11.5% in `bitmap_node_copy`** -- it copies
the path and then releases the old one, which is a persistent insert with the
result handle reused. The hamt guide's "mutate in place" (D-3) is not what the
code does. An in-place transient (copy a node only the first time the
transient touches it, mutate it after, no release) would skip both of those
lines: on this profile, about two thirds of the time.

## R-6 -- `Vec` growth

`vec-push!` grows by `malloc` + element copy loop + `free`
(`stdlib/vec.tur:136-141`). Priced with a 10M-push program (fill, then sum
back; same output both ways) against a copy of `stdlib/` whose growth is a
single `realloc` (`TUR_STDLIB_DIR`):

| growth | wall | instructions |
| --- | ---: | ---: |
| malloc + copy + free (today) | 112 ms | 454,077,714 |
| realloc | **61 ms** | 360,153,953 |

**-46% wall, -21% instructions.** The plan guessed "likely small"; it is not,
because `realloc` of a large block extends it in place (`mremap`) instead of
touching every element again, and the copy loop also faults in a fresh buffer
each time. The same profile shows **`tur_region_note_escape` at 8.3%** of a
push loop with no region open: the store hook every `vec-push!` carries
(CLAUDE.md, "Region Store Hooks") is an out-of-line call into the runtime
archive, and it returns at its first test (`g_live_n <= 0`). An inline
"is any region open" check before the call would make the hook free when it
cannot matter. (Unrelated, noticed on the way: `vec-len` returns
`(int)vec->len`, a 32-bit truncation under its `: int`; harmless below 2^31
elements.)

## R-8 -- tail calls gcc does not guarantee

Every tail call the emitter wants guaranteed is written
`TUR_MUSTTAIL return f(...)`, and `TUR_MUSTTAIL` is `__attribute__((musttail))`
only where the compiler honours it (clang on x86-64/aarch64); under gcc it
expands to nothing (`# define TUR_MUSTTAIL`), so constant stack rests on
gcc's `-O2` sibling-call optimization. Over the fixture tree: **2,973 of 2,975
programs carry at least one such site, 43,559 sites in all** -- most from the
prelude (the `Eq` instance methods' self-calls appear in every unit), so the
gap is not a corner: on gcc, every program's guaranteed tail calls are hints.
Only 2 fixtures carry `requires.musttail`. R-1's spike shows one concrete
failure shape: a self-call followed by drop glue is not in tail position at
all, so no compiler can make it a jump (the closure loop above).

## R-10 -- fibers

Two fixtures sized up: `await-fresh-spawn-future-freed` at 2M spawn+await
iterations, `async-repeated-park-frees-each-turn` at 1M park/resume turns.

| | wall | per operation |
| --- | ---: | ---: |
| spawn + await, 2M | 273 ms | **~137 ns** |
| park + resume, 1M turns | 813 ms | **~813 ns** |

**Which switch path runs**: compiled programs switch fibers through libc
`swapcontext` (the emitted glue, `src/compiler/emit_module.c:16271-16390`;
the spawn binary links `swapcontext`, `_setjmp` and `longjmp`). On Linux,
glibc's `swapcontext` saves and restores the signal mask -- an
`rt_sigprocmask` system call per switch -- the likely reason a park/resume
turn costs ~0.8 us (not yet confirmed with a syscall count). The hand-written
`src/async/fiber_ctx_x64.S` / `fiber_ctx_arm64.S` are compiled into `tur`
itself, not into emitted programs; Windows gets its own register-snapshot
shim (`emit_module.c:13174`). Spawn+await is allocation-bound instead (malloc
/ calloc / free 52% of its profile: the DK frames and the fiber closure). Fix
direction: emit the asm switch (or the Windows-style register snapshot) on
Linux and macOS too; the signal mask is not something a Turmeric fiber
changes.

## R-11 -- dictionary dispatch after monomorphization

From the WP0 fixture sweep (an `emit-c` census of `((void**)__dict_N)[k]`
dispatch sites): **63 plain fixtures still dispatch through a dictionary after
monomorphization**, and none of them does it in a loop (all run in 5 ms or
less), so there is nothing to time; `hkt-generic-calls-generic` (15 sites in
17 `__dict_N` clones) is the densest. Every such clone is CPS-colored, through
R-1's indirect rule -- dispatch is a call through a function value -- so R-11's
cost is R-1's. A loop-bound dispatch microbenchmark is a gap the fixture tree
does not fill.

## R-3 -- retired region generations

**The claim** (plan): `tur_region_owns` walks every live and retired generation
on every node free, retired generations accumulate for the life of the
process, so a free gets slower the longer a program runs.

**What the profile says.** `sized/region-rounds-100k` (WP0's sized copy of
`region-with-region`: 100k rounds of an 8-node `with-region` bracket) spends
**71.4% of its cycles in `reg_remove`** (`perf record`, Release, cc -O2). The
cause is one step past the plan's: none of those brackets ever rewinds.

```
TUR_REGION_STATS=1   rounds=1000     4 ms   pushes=1003    rewinds=0 retires=1003
                     rounds=10000   40 ms   pushes=10003   rewinds=0 retires=10003
                     rounds=100000 1118 ms  pushes=100003  rewinds=0 retires=100003
```

Every generation retires, so every round keeps its slab (~4 KiB; 413 MB peak
at 100k rounds) and adds an arena to the process-wide registry `g_reg` that
nothing ever takes out again (`retire_top`, `src/runtime/region.c:266`). Three
costs follow:

1. **Shutdown is quadratic.** `tur_region_shutdown` frees the retired arenas
   with one `reg_remove` each (`region.c:397`), and `reg_remove` scans `g_reg`
   from index 0 for its argument (`region.c:163-174`) -- R x R/2 compares for R
   retirements. That is the 71%: it happens after `main` returns.
2. **A free is linear in retirements** -- the plan's R-3 as written:
   `tur_region_owns` walks live and retired generations (`region.c:337-344`).
3. **Memory grows by a slab per bracket**, by design once a generation
   retires (something may point in, so it can never be freed).

**Why nothing rewinds here.** The program builds its chain through
`(:: (Link n acc) :int)` and reads it in inline C through an erased `:int`
parameter. Both are hooked escape notes (CLAUDE.md, "Region Store Hooks":
every erasing ascription, and the implicit typed-node -> `:int` argument of an
inline-C callee), so each generation is flagged escaped and the pop retires it
-- the safe direction, no corruption, and no saving. A sweep of every program
that opens a region, under `TUR_REGION_STATS=1`:

| program | pushes | rewinds | retires |
| --- | ---: | ---: | ---: |
| `benchmarks/bench-regions-subst` | 9039 | **9039** | 0 |
| `tests/fixtures/region-with-region` | 103 | **0** | 103 |
| `tests/fixtures/region-scope-value-survives` | 103 | **0** | 103 |
| `tests/fixtures/region-scope-void-body` | 3 | 0 | 3 |
| 17 other fixtures | 1-12 each | 15 of 21 programs rewind at least once | |

Regions work where the chain stays typed: the benchmark the regions plan
named rewinds every bracket. But the two fixtures whose comments say the pop
rewinds ("the pop REWINDS (the static walk clears it)") do not rewind under
the current hooks. They check only the printed answer, so they still pass.
Filed: [region-rewind-fixtures-never-rewind](../../reported/region-rewind-fixtures-never-rewind.md)
and [region-retired-generations-make-shutdown-quadratic](../../reported/region-retired-generations-make-shutdown-quadratic.md).

## Not a problem (measured, fine)

- **R-4**, the collector registry on every rc allocation: ~2-3% of an
  rc-heavy program; the allocator is the cost.
- **`-march=native`** (R-7): nothing on this corpus.
- **LTO across the runtime archive** (R-2) on top of an inline preamble:
  0.749 -> 0.747 geomean cycles; worth it only for rc-free-heavy code.
- **Spawn + await** (R-10): ~137 ns, allocation-bound, no switch on the
  measured path.

## Ranked: change / buys / effort

| # | change | buys | effort |
| --- | --- | --- | --- |
| 1 | **Split preamble: read the panic flag (and the other `tur_tls_*_ptr` thread-locals) directly** -- an `extern __thread` in the hosted split, or at least `__attribute__((const))` on the accessor (R-2) | ~25% of cycles across the measured corpus, up to 57% on CPS loops and 98% of instructions on `fib`, without giving back the split's 25-40% cc saving (WP1) | small-medium (check fibers that migrate threads first) |
| 2 | **`realloc` in `vec-push!` growth** (R-6), and an inline "region open?" test in front of `TUR_REGION_NOTE` | -46% wall on a push loop; -8% of a push loop's samples | small |
| 3 | **In-place HAMT transients** (R-5): copy a node once per transient, mutate after, no path release | ~2/3 of a batch insert's time on this profile; D-3's guide sentence becomes true | medium |
| 4 | **Retired regions: O(1) registry removal and a cheap shutdown** (R-3): scan `reg_remove` from the end, drop this thread's registry entries in one pass at exit, index retired slabs for `tur_region_owns` | the 71% of `region-rounds-100k`; frees that stay cheap in long runs | small (1-2), medium (3) |
| 5 | **Fiber switch without `swapcontext`** on Linux/macOS (R-10): emit the asm switch or the Windows-style register snapshot | the ~0.8 us park/resume turn, most of it the signal-mask syscall | medium |
| 6 | **R-1, properly**: effect rows that hold for function values (inference at lambdas, checking at HOF parameters) or a `may_hold_awaiter`-style parameter analysis, **plus** drop-before-call so a self-call with closure drop glue is a real tail call | 42-45% of instructions on combinator-style code, 4 allocations per region bracket, ~250 stdlib functions off the CPS path -- and no closure loop overflowing its stack when it leaves the trampoline | large |
| 7 | `--release` = `-O3` with the inline preamble (or #1) (R-7, decision 2) | ~5% from `-O3`, more on inline-C code; LTO not worth its link time | small |
| 8 | Region rewind fixtures that assert a rewind (`TUR_REGION_STATS` pinned) | the rewind-corruption tests test a rewind again | small |

## Not done here

- **R-2's third arm** (rc and region hot functions as `static inline` in a
  header): the build-mode sweep answers the question it was for -- the cost is
  the split preamble's TLS accessor, not the runtime archive's call boundary
  -- so the header rewrite is not worth prototyping before fix #1.
- **C ceilings for R-3..R-6** in the style of `adt-alloc/ceiling.c`: each was
  priced against the alternative it would be replaced by (a realloc copy of
  `stdlib/`, the transient's own profile, the registry's profile share), which
  answers "what would the fix buy" more directly than a ceiling would.
- **Saffron's run time** (`bench-saffron-dyn-tail`: `gc` 51%) belongs to the
  Saffron collector and is outside this plan's corpus focus.
