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
rewinds ("the pop REWINDS (the static walk clears it)") have not rewound
since the erasure hooks landed. They check only the printed answer, so they
still pass.
