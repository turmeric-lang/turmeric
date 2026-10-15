# Profiling -- where does the time actually go

> **Status: P0 LANDED 2026-08-25; C0+C1 LANDED 2026-10-14 (`--timings`,
> performance-audit-plan WP1); everything else PROPOSED.** Written
> 2026-08-25 in response to "is there a way to output performance information,
> percentages of time spent in what sub-system, possibly
> `--profile <profile-json>`". The answer was no; this is the plan for yes.
> `tur demangle` (section 5.1) is built and tested -- `perf` and Instruments
> read cleanly against a Turmeric binary today. Open questions 2 and 3 were
> answered by the author on 2026-08-25 and are folded into sections 2 and 4.3.
> **C0+C1 (2026-10-14):** `src/compiler/prof.{h,c}`, the zones of section
> 1.2 plus `jit`, `run` and `other`, `--timings[=<path>]` on any compiling
> command, the `kind: "compiler"` document, `tests/run-timings.sh` (ctest
> `tur_timings_tests`), and a "Compiler timings" subsection in
> `performance-guide.md`; what was decided differently is under C0+C1 in
> section 5. `tur jit --timing-json` is unchanged and unabsorbed (C4).
> Remaining, none started or declined: P0a, P1a, P1, P2, P3, C2/P4, P5, P6, C4,
> and the rest of the section 6 guide (the `--profile` half). Open questions
> 1, 4, 5 stand.
> The `src/` file:line anchors below have drifted (e.g. `--timing-json` is now
> `src/main.c:4634`) -- locate by symbol, not line.
> **Type:** Tooling / runtime / compiler driver
> **Depends on:** `tur_demangle` (`src/compiler/mangle.c:172`), the `#line`
> emission behind `--debug` (`src/compiler/emit_core.c:461`), and the
> `static_init_register` bands in the emitted `main`
> (`src/compiler/emit_module.c:13620`). All three already exist.
> **Related:** [`post-jit-benchmark-resurrection-plan.md`](../archive/post-jit-benchmark-resurrection-plan.md)
> (owns `tur jit --timing-json`, which this plan absorbs without breaking),
> [`suite-timing-trends-plan.md`](../archive/suite-timing-trends-plan.md) (suite-level
> wall clock; complementary, not overlapping),
> [`docs/guides/performance-guide.md`](../guides/performance-guide.md) section
> "Benchmarking methodology" (which currently has no profiling section to
> point at -- this plan gives it one).

## 0. Summary

Two different questions get asked with the same words:

1. **"Why is my program slow?"** -- where a *running Turmeric program* spends
   its time, split across user functions and runtime subsystems (rc, GC, HAMT,
   STM, CPS/scheduler, FFI, string).
2. **"Why is `tur build` slow?"** -- where the *compiler* spends its time,
   split across read, elaborate, typecheck, refine discharge, CPS lowering,
   codegen, and the `cc` subprocess.

Both are real, both are unanswerable today, and both want the same output
shape: a table of percentages plus a machine-readable JSON document. This plan
builds one JSON schema (`tur-profile/1`), one reporting front-end
(`tur profile report`), and two producers behind two flags:

| Flag | Answers | Mechanism |
| --- | --- | --- |
| `--profile [<path>]` on `run` / `build` / `jit` / `test` | question 1 | SIGPROF sampling inside the emitted binary |
| `--timings [<path>]` on any compiling command | question 2 | explicit zone timers inside `tur` |

With no path, each prints a percentage table to stderr. With a path, each
writes JSON *and* prints the table.

### 0.1 The answer available today (LANDED)

`perf`, Instruments, `samply`, and `nm` already work on both binaries. The only
thing that made their output unreadable was that every Turmeric function
appeared under its mangled C name. **`tur demangle` fixes that and is shipped:**

```sh
perf record -g ./build/bin/myprog && perf script | tur demangle | perf report -i -
nm -g ./build/bin/myprog | tur demangle
tur demangle geom__vector__add2          # -> geom/vector/add2
```

That is most of the value of question 1 for a fraction of the work, and it
stands on its own regardless of whether the rest of this plan is ever built.
See section 5.1 for what the measurement turned up -- the decode was the easy
half, and the recognizer needed real data to get right.

### 0.2 What this is deliberately NOT

- **Not a tracing profiler.** No `-finstrument-functions`, no emitted
  enter/exit hooks. Section 4.1 argues the case: instrumentation costs 2-10x,
  and a profiler that changes the shape of what it measures is worse than no
  profiler. Exact call *counts* are a separate feature; if they are wanted
  later they belong behind a distinct `--call-counts`, not smuggled in here.
- **Not a memory profiler.** Allocation-site attribution is a different data
  model (bytes and lifetimes, not samples) and a different plan. `TUR_GC_TRACE`
  and `tests/run-leak-check.sh` already cover part of that ground.
- **Not a new experiment.** Per [`CLAUDE.md`](../../CLAUDE.md), the
  `--enable=<name>` gate is for in-flight *language/compiler semantics*;
  diagnostic and codegen knobs (`--dump-*`, `--emit-abi-trace`,
  `--strict-effects`) are explicitly exempt. `--profile` and `--timings` change
  no program's meaning. **Do not add an `EXPERIMENTS[]` row for this**, and do
  not let a reviewer talk you into one.
- **Not portable on day one.** POSIX (Linux, macOS) in P1-P5; Windows in P6;
  `--target wasm` is a hard error from the start (section 7.3).

## 1. The shared schema: `tur-profile/1`

One envelope, two `kind`s. The envelope keys are identical so
`tur profile report` and any downstream chart reads either without branching.

```json
{
  "schema": "tur-profile/1",
  "kind": "program",
  "tur_version": "0.37.0",
  "command": "tur run examples/solver.tur -- --n 12",
  "started_utc": "2026-08-25T17:02:11Z",
  "wall_ms": 4210.7,
  "notes": []
}
```

`notes` carries every honesty caveat the run knows about -- sanitized build,
dropped samples, missing frame pointers, cc fallback -- as plain strings that
the report front-end prints above the table. A profile that had to compromise
says so on its face rather than in a footnote nobody reads.

### 1.1 `kind: "program"`

```json
{
  "sample_hz": 199,
  "samples": 4123,
  "dropped_samples": 0,
  "subsystems": [
    {"name": "user",   "self_ms": 2101.4, "self_pct": 49.9, "samples": 2058},
    {"name": "stdlib", "self_ms":  738.7, "self_pct": 17.5, "samples":  723},
    {"name": "hamt",   "self_ms":  702.3, "self_pct": 16.7, "samples":  687},
    {"name": "rc",     "self_ms":  310.0, "self_pct":  7.4, "samples":  303},
    {"name": "libc",   "self_ms":  237.8, "self_pct":  5.6, "samples":  233},
    {"name": "gc",     "self_ms":  120.5, "self_pct":  2.9, "samples":  118}
  ],
  "functions": [
    {"name": "solver/step", "subsystem": "user",
     "file": "src/solver.tur", "line": 88,
     "self_ms": 1204.0, "self_pct": 28.6,
     "total_ms": 3900.2, "total_pct": 92.6, "samples": 1179}
  ],
  "threads": [
    {"id": 0, "label": "main", "samples": 3990},
    {"id": 1, "label": "sched-worker", "samples": 133}
  ]
}
```

`self_*` is exclusive (the sample's leaf frame); `total_*` is inclusive
(anywhere on the stack). `subsystems[].self_pct` sums to 100 by construction.
`functions[].total_pct` does not and must not -- the report labels the column
so nobody reads it as a partition.

### 1.2 `kind: "compiler"`

```json
{
  "phases": [
    {"name": "read",          "self_ms":  60.1, "self_pct":  3.3},
    {"name": "sweet",         "self_ms":  11.0, "self_pct":  0.6},
    {"name": "module-load",   "self_ms": 190.4, "self_pct": 10.4},
    {"name": "macro-expand",  "self_ms":  70.2, "self_pct":  3.8},
    {"name": "elaborate",     "self_ms": 640.0, "self_pct": 35.0},
    {"name": "typeclass",     "self_ms": 210.5, "self_pct": 11.5},
    {"name": "kind-check",    "self_ms":   8.0, "self_pct":  0.4},
    {"name": "effect-lower",  "self_ms":  22.0, "self_pct":  1.2},
    {"name": "effect-row",    "self_ms":  14.0, "self_pct":  0.8},
    {"name": "cps",           "self_ms":  55.0, "self_pct":  3.0},
    {"name": "borrow-check",  "self_ms":  31.0, "self_pct":  1.7},
    {"name": "refine",        "self_ms":  12.0, "self_pct":  0.7},
    {"name": "mono",          "self_ms":   6.0, "self_pct":  0.3},
    {"name": "emit",          "self_ms": 200.0, "self_pct": 10.9},
    {"name": "cc",            "self_ms": 300.0, "self_pct": 16.4}
  ],
  "modules": [
    {"name": "stdlib/hamt", "self_ms": 120.0, "self_pct": 6.6, "forms": 412}
  ]
}
```

`phases[].self_ms` is **exclusive** and sums to `wall_ms` within measurement
noise -- section 3.2 explains why that invariant is worth the small amount of
extra bookkeeping, and section 8 makes it a test.

## 2. CLI surface

```
tur run   <file> --profile[=<path>] [-- args...]
tur build <dir>  --profile[=<path>]      # instruments; does not run
tur jit   <file> --profile[=<path>]
tur test         --profile[=<path>]

tur build   <dir>  --timings[=<path>]
tur check   <file> --timings[=<path>]
tur emit-c  <file> --timings[=<path>]

tur profile report <file.json>           # pretty table from either kind
tur profile diff   <a.json> <b.json>     # percentage-point deltas
tur demangle                             # stdin -> stdout name filter (P0)
```

Both flags accept `--flag <path>` and `--flag=<path>`, matching the existing
`--timing-json` parser at `src/main.c:3642`.

**`--profile` on `build` arms the sampler; it never bakes in an output path**
(resolving open question 2). The armed binary writes a profile only when
`TUR_PROFILE_OUT` names a destination at run time, and is otherwise inert apart
from a one-line stderr banner saying it is armed. A profiled binary must never
be silently profiled, and a binary that writes files to a baked-in path because
of how it was compiled months ago is worse.

`tur run --profile <path>` and `tur jit --profile <path>` set `TUR_PROFILE_OUT`
for the child they spawn, so the common interactive case is still one step and
the env var only surfaces when you deliberately profile a pre-built artifact.

### 2.1 Driver plumbing

- New `bool g_emit_profile` / `const char *g_profile_out` and
  `bool g_timings` / `const char *g_timings_out` alongside the existing
  `g_dump_*` globals in `src/compiler/emit_internal.h:46`.
- Add rows to `wk_apply_flags` (`src/main.c:7503`) for both flags. The worker
  processes re-apply compiler flags from a string; without a row there, a
  parallel or project-mode build profiles the parent and not the work. This is
  the single easiest thing to forget and it fails silently.
- `--profile` implies the `#line` emission that `--debug` turns on
  (`g_emit_debug_lines`), but **not** its `-Og`. Section 4.4.

## 3. Compiler self-profile (`--timings`)

### 3.1 The zone API

New `src/compiler/prof.h` + `prof.c`, roughly 120 lines:

```c
typedef enum ProfZone {
    PROF_READ, PROF_SWEET, PROF_MODULE_LOAD, PROF_MACRO_EXPAND,
    PROF_ELAB, PROF_TYPECLASS, PROF_KIND, PROF_EFFECT_LOWER,
    PROF_EFFECT_ROW, PROF_CPS, PROF_BORROW, PROF_REFINE,
    PROF_MONO, PROF_EMIT, PROF_CC,
    PROF_ZONE_N
} ProfZone;

void prof_enter(ProfZone z);
void prof_exit(ProfZone z);
void prof_module_begin(const char *name);   /* the second axis, C2 */
void prof_module_end(void);
int  prof_write_json(const char *path);     /* 0 on success */
void prof_print_table(FILE *out);
```

`prof_enter`/`prof_exit` open with `if (!g_timings) return;` so an unprofiled
build pays one predictable branch at a handful of coarse boundaries. No
`#ifdef`, no build variant -- a profiler you have to rebuild to use is a
profiler nobody uses.

### 3.2 Exclusive time via a zone stack

Zones nest and re-enter: `PROF_ELAB` recursively enters `PROF_MODULE_LOAD`
which enters `PROF_READ` which enters `PROF_ELAB` again. Naive per-zone
start/stop timestamps double-count and produce a table summing to 300%.

Instead keep an explicit stack and charge **exclusive** time on every
transition:

```c
static ProfZone stack[64];
static int      sp;
static uint64_t last_ns;

void prof_enter(ProfZone z) {
    if (!g_timings) return;
    uint64_t now = prof_now_ns();
    if (sp > 0) self_ns[stack[sp - 1]] += now - last_ns;
    if (sp < 64) stack[sp++] = z;
    last_ns = now;
}
```

Two clock reads per boundary, boundaries are coarse (per pass, per module, per
`cc` invocation), so total overhead is well under a millisecond on a
multi-second build. The payoff is the invariant in section 1.2: the phase
column is a genuine partition of wall clock, which is what makes the
percentages trustworthy. `prof_now_ns` reuses the `clock_gettime(CLOCK_MONOTONIC)`
idiom already in `src/jit_engine.c:51`.

### 3.3 Hook points (C0)

| Zone | Site |
| --- | --- |
| `PROF_READ`, `PROF_SWEET` | `read_all` at `src/main.c:895` and its peers |
| `PROF_ELAB` .. `PROF_BORROW` | wrap each `case` in `run_core_passes`, `src/main.c:438-520` |
| `PROF_EMIT` | the `emit_*` call after `run_core_passes` in `compile_to_c`, `src/main.c:567` / `:710` |
| `PROF_CC` | around `system(cmd.data)` in `link_command_run`, `src/main.c:2278` |
| `PROF_REFINE` | `refine_discharge_*`, which already has `TUR_REFINE_STATS` counters to cross-check against |

`run_core_passes` being a flat ordered switch over `core_passes[]` is what
makes C0 a small change. The pass list in `src/runtime/pass.h` and the zone
enum should be kept adjacent in review -- a new `PassKind` without a zone
lands in whatever zone encloses it and quietly inflates a neighbor.

### 3.4 Why C1 is not optional

C0 alone will report something close to `elaborate: 78%`, which is true and
useless -- `PASS_ELABORATE` is a monolith spanning module loading, macro
expansion, typeclass resolution, and unification. **C1 subdivides it** with
`PROF_MODULE_LOAD`, `PROF_MACRO_EXPAND`, and `PROF_TYPECLASS` zones placed
inside `src/compiler/elab_module.c`, `elab_macros.c`, and `elab_typeclasses.c`.
Plan C0 and C1 as one deliverable; shipping C0 by itself invites the
conclusion "elaboration is slow", which nobody can act on.

### 3.5 Absorbing `--timing-json`

`tur jit --timing-json` (`src/main.c:3642`, owned by
[`post-jit-benchmark-resurrection-plan.md`](../archive/post-jit-benchmark-resurrection-plan.md))
keeps working byte-for-byte -- the benchmark harness parses `compile_ms`,
`run_ms`, and the `"engine": "cc-fallback"` marker. C4 additionally makes
`tur jit --timings` emit a full `kind: "compiler"` document whose `phases`
include the engine's own c2mir and link stages from `tur_jit_last_timings`.
Do not "unify" these by changing the old keys.

## 4. Program profile (`--profile`)

### 4.1 Sampling, not instrumentation

| | Instrumentation | Sampling |
| --- | --- | --- |
| Overhead | 2-10x | 1-3% |
| Codegen change | every function body | none |
| Exact call counts | yes | no |
| Distorts the thing measured | severely | negligibly |

Instrumentation is additionally hostile to this codebase specifically: the
single-file build leans on the optimizer's dead-code elimination to drop
preloaded stdlib defns (see the `-Og` comment at `src/main.c:2394`), and
per-function hooks anchor exactly those bodies; TCO'd self-calls
(`src/compiler/emit_fns.c` `tco_*`) become back-edges with no call to hook;
and the CPS path splits one Turmeric function across many C functions, so
"calls" stops meaning what a reader expects. **Sample.**

### 4.2 Mechanism

New `src/runtime/prof_sample.c` / `.h`, compiled into `libturi` and reachable
from the emitted preamble.

- **Arming.** `--profile` makes the emitter register `__tur_prof_init` via
  `static_init_register(..., STATIC_INIT_KEYS)` -- the earliest band, next to
  `__tur_fatbox_init` at `src/compiler/emit_module.c:13665` -- so the timer is
  live before any user code, and an `atexit` hook that writes the JSON.
- **Timer.** `setitimer(ITIMER_PROF, ...)` at a default **199 Hz**. A prime,
  non-round rate deliberately: 100 Hz beats against 10 ms scheduler quanta and
  against any user loop that happens to tick on a round millisecond, producing
  confidently wrong attributions.
- **Handler.** The SIGPROF handler is async-signal-safe by construction: it
  writes a fixed-size record into a preallocated per-thread ring buffer and
  returns. No `malloc`, no stdio, no locks. On overflow it increments
  `dropped_samples`, which surfaces in the JSON and in `notes`.
- **Symbolization.** Deferred entirely to `atexit`, where `backtrace_symbols`
  (or `dladdr`) is legal to call. Each symbol goes through the demangler; each
  frame is classified by section 4.3's table.

### 4.3 Subsystem attribution

The runtime's public symbols are already cleanly prefixed, which is what makes
this work without any annotation burden:

| Symbol prefix | Subsystem |
| --- | --- |
| `tur_hamt_`, `tur_map_` | `hamt` |
| `tur_rc_`, `tur_ref_`, `__tur_rc_` | `rc` |
| `gc_`, `tur_gc_` | `gc` |
| `tur_stm_`, `tur_tvar_` | `stm` |
| `tur_kont_`, `tur_step_`, `__tur_cps_`, `dk_` | `cps` |
| `tur_ffi_call_` | `ffi` |
| `tur_string_`, `tur_sb_`, `tur_slice_` | `string` |
| `tur_trail_`, `tur_uf_`, `tur_bt_` | `trail` |
| `tur_arena_` | `arena` |
| a Turmeric definition from a `stdlib/` module | `stdlib` |
| any other Turmeric definition | `user` |
| `malloc`, `free`, `mem*`, `str*` | `libc` |
| anything else | `native` |

**`stdlib` is its own subsystem, split out from `user`** (resolving open
question 3). A reader staring at `user: 67%` wants to know how much of that is
`stdlib/vec` before they go looking at their own code. The split is free at
symbol-classification time: a demangled name carries its module path, so
anything resolving to a `stdlib/` module is `stdlib` and everything else is
`user`. Once P1a's symbol side-table exists the classification is exact rather
than path-shaped guessing.

The `functions[]` table still carries the full module for both, so the two
subsystems stay drillable rather than opaque.

The table lives in exactly one place, `src/runtime/prof_subsys.c`. **Its
invariant is the interesting part of this section:** a CI check
(`tools/ci/check-prof-subsys-table.py`) enumerates every exported `tur_*`
symbol in `libturi.a` via `nm` and fails if any matches zero rows or more than
one. Without that check, a new subsystem lands silently in `native` and the
profile quietly under-reports it forever -- the exact failure mode that makes
people distrust profilers.

### 4.4 Getting back to `.tur` file:line

`--profile` turns on the existing `#line` emission
(`emit_line_directive`, `src/compiler/emit_core.c:461`) and adds `-g` to
`cc_flags` (`src/main.c:2388`), so DWARF maps generated C straight back to
`.tur` source and `functions[].file`/`.line` are real Turmeric locations.

It does **not** adopt `--debug`'s `-Og`. Profiling a `-Og` binary answers a
question about a build nobody ships. `--profile` keeps the normal optimization
level and accepts that inlining blurs some attribution; `notes` says so.

`-fno-omit-frame-pointer` is added too, but only P3 needs it -- see the
staging in 4.5.

### 4.5 Leaf-only first

Full call trees need reliable stack unwinding, which needs frame pointers,
which is where the portability and accuracy problems live. **Leaf-only
attribution -- record just the interrupted PC, no stack walk -- needs none of
that and already answers the question as asked:** percentages per subsystem.

So P1 ships leaf-only (`subsystems[]` complete, `functions[].self_*`
complete, `total_*` absent), and P3 adds stack capture for inclusive time and
caller attribution. If P3 slips, P1 is still a shipped, useful profiler.

## 5. Phases

### P0 -- demangle filter (LANDED 2026-08-25)

`tur demangle` reads stdin, rewrites recognizable mangled names via
`tur_demangle`, writes stdout; with arguments it decodes them one per line
(c++filt's shape). `--strict` and `--annotate` as described in section 2.
Immediately makes `perf`, `samply`, Instruments, `nm`, and `gdb` output
readable against both binaries.

Shipped in `src/cli/demangle.c`, dispatched from `src/main.c`, with unit tests
in `tests/mangle_test.c` (`tur_mangle_unit`) and a CLI harness at
`tests/run-demangle.sh` (`tur_demangle_tests`).

**Two deviations from this plan as originally written, both deliberate:**

**1. `tur_demangle` was NOT moved to `src/runtime/demangle.c`.** The filter
runs inside `tur`, which already links `compiler/mangle.c`, so the move buys
nothing yet -- and it is not free: the forward table (`sigil_mnemonic`) and its
inverse (`mnemonic_byte`) are two functions in one file today, and splitting
them across a compiler/runtime boundary makes silent drift possible. Nothing
caught that drift: `mangle_test.c`'s oracle is hand-written and only covered
the mnemonics someone thought to list.

So P0 landed the safety net instead of the move: an **exhaustive round-trip
over all 255 non-NUL source bytes** (mangle each, demangle it back, require
the original byte). That closes the gap today and makes the move a mechanical
change whenever P1 actually needs the decoder inside an emitted binary.

**2. The recognizer needed real work, and the measurement changed the design.**
The plan assumed decoding was the hard part. It is not -- `tur_demangle`
already exists and is exact. The hard part is deciding *which* tokens in an
arbitrary text stream are Turmeric names, and that is undecidable from text:
the mangling is injective, so `foo_lt_bar` is simultaneously an ordinary C
symbol and a valid encoding of `foo<|r`. Round-tripping cannot discriminate
between them, because both directions are correct.

Leaning on `tur_demangle`'s own strictness is not enough either. Measured over
the 14418 C symbols in `nm ./build/tur` -- a binary containing no Turmeric code,
so every rewrite is by definition wrong -- strictness alone accepted **149**,
including `analyze_expr` -> `analyze!pr`, `tur_string_cmp` -> `tur*ring,p`, and
`pthread_create` -> `pthread^eate`. Those are exactly the hot symbols a profile
shows, so ~1% corruption of the symbol table is far worse than 1% noise.

The fix was to score every mnemonic on real data -- recall as the number of
stdlib `defn` names containing that sigil, cost as C symbols in
`nm ./build/tur` carrying that escape -- and keep only the profitable ones:

| escape | sigil | recall | cost | verdict |
| --- | --- | --- | --- | --- |
| `_hy` | `-` | 1178 | 1 | keep (kebab-case is the dominant convention) |
| `_sl` | `/` | 351 | 47 | keep (`hamt/get`, `json/decode`) |
| `_gt` `_lt` | `>` `<` | 26 | 0 | keep |
| `_qu` **at end** | `?` | 137 | 0 | keep (every `?` name ends in `?`) |
| `_ex` **at end** | `!` | 74 | 4 | keep |
| `_ex` anywhere | `!` | 74 | 112 | reject |
| `_un` | `_` | 6 | 85 | reject |
| `_eq` | `=` | 1 | 83 | reject |
| `_st` | `*` | 0 | 253 | reject |

The two interesting rows are `?` and `!`: anchoring them to the end of the
token -- which is exactly Turmeric's `name?` / `name!` convention -- collapses
their cost to nearly nothing while keeping all of their recall. Plus a
leading-`__` rejection (a decoded leading `/` is not a name; those tokens are
`__fn_5`, `__stdinp`, `__func__`, all emitted verbatim anyway), which alone
removed 30 of the 149.

Result: **8 false positives of 14418 (0.055%)**, all one `_sl` + "ice"/"ot"
shape (`n_slice`, `find_slot`) that it would be over-fitting to special-case;
and **1374 of 1374** non-trivially-mangled stdlib names recovered exactly, with
zero mis-decodes. `tests/run-demangle.sh` re-runs the precision measurement as
a ratchet, so adding a noisy mnemonic fails a test instead of quietly degrading
every profile.

**What this implies for P1** (see also section 5.1a): the heuristic is good but
it is still a heuristic, and it exists only because the filter reads a text
stream with no idea which binary produced it. The compiler knows the answer
exactly.

### P0a -- subsystem table

`src/runtime/prof_subsys.c` and its CI check (section 4.3) are independently
verifiable before any sampler exists, but were not part of the demangle work
and are not built.

### P1a -- emit a symbol side-table (promoted out of P0's findings)

`raw_name_for_binding` (`src/compiler/emit_core.c:1792`) already computes the
mangled spelling of every Turmeric definition. Writing that mapping out beside
the artifact -- `<binary>.tur-symbols`, one `mangled<TAB>source` line per
definition -- turns demangling from a guess into a lookup: 100% precision and
100% recall, including the `foo_bar` and `tur_slice_eq` cases no heuristic can
ever settle.

`tur demangle --symbols <file>` then consults the table first and falls back to
the heuristic for anything absent. The in-process sampler (P1) should read the
same table rather than re-deriving names from `backtrace_symbols`.

This is cheap, it subsumes the hardest part of P0, and it should land before
anyone tries to tighten `is_signal_mnemonic()` further.

### C0+C1 -- compiler timings (LANDED 2026-10-14)

`prof.h` zone API, the `run_core_passes` / read / emit / cc hooks, the
elaboration subdivision, `--timings`, JSON + stderr table. Answers question 2
end to end.

As built, and where it differs from sections 2-3:

- **Three zones beyond 1.2's list.** `jit` (c2mir, the MIR link and the
  context teardown), `run` (the program's own execution under `tur run` /
  `tur jit`, so the partition holds there too -- the report says the phase is
  not compile time) and `other` (the stack's bottom: what no zone claims).
  `pass_zone()` in `main.c` maps every `PassKind` to a zone in a switch with
  no default, so a new pass without a zone is a `-Wswitch` error rather than
  a review note.
- **The stack is run-length encoded**, and `elab_form` re-enters
  `elaborate`.  The C1 zones (module-load, macro-expand, typeclass, refine)
  each elaborate forms of their own; `elab_form` pushes `elaborate` when
  something else is on top, so that work goes back where it belongs.  Pushing
  the zone already on top bumps a count and reads no clock, so the cost is
  per zone *change*, and the report counts them (`clock_reads`, ~1,200 for a
  one-line program: tens of microseconds).
- **`typeclass` includes instance resolution** (`typeclass_env_lookup_instance`)
  wherever it is asked for, the CPS pass included.  **`mono`** is the
  by-value spec resolution pass only; monomorphs instantiated inline are
  charged to `elaborate` or `emit`, wherever that happens.
- **`--timings <path>` takes the next argument only when it ends in `.json`
  or is a `/dev/` node**, since otherwise `--timings f.tur` is ambiguous;
  `--timings=<path>` takes anything, and `-` / `/dev/stdout` reuse the
  process's stdout.  Parsed and stripped before anything else, so the r7rs
  split cache key (which hashes the command line) does not see it.
- **No worker processes in `tur build`.** Single-file and project builds run
  in one process; their only children are toolchain subprocesses, charged to
  `cc` as the wall time the driver waits (parallel prelude pieces overlap).
  `wk_apply_flags` has the 2.1 row anyway, for the `tur worker` fixture
  evaluator: each forked fixture child times its own compile and writes its
  report before `_exit`; nothing is merged.
- An extra envelope key, `clock_reads`, records the timer's own cost.

### P1 -- leaf sampling

`prof_sample.c`, arming via `static_init_register`, `--profile`, JSON + table,
POSIX only, leaf-only. **Answers question 1 as asked.**

### P2 -- source attribution

`#line`/DWARF wiring, `functions[]` populated with `.tur` file and line,
top-N ranking in the report.

### P3 -- stacks

Frame-pointer stack capture, `total_ms`/`total_pct`, caller rollups.

### C2/P4 -- second axes

Per-module timings on the compiler side; per-thread rollups on the program
side, with scheduler-worker labeling from `src/runtime/tur_tls.c`.

### P5 -- report and diff

`tur profile report` and `tur profile diff` (percentage-point deltas, for
regression gating). Wire into `benchmarks/run-benchmarks.sh` and the trend
work in [`suite-timing-trends-plan.md`](../archive/suite-timing-trends-plan.md).

### P6 -- Windows

`CreateTimerQueueTimer` + `SuspendThread`/`RtlCaptureStackBackTrace`. Until
then, `--profile` under `TUR_HOST_WINDOWS=1` prints a clear diagnostic and
exits non-zero; fixtures carry `requires.posix-apis`.

## 6. Guide and docs

- New section "Profiling" in
  [`docs/guides/performance-guide.md`](../guides/performance-guide.md), sited
  next to "Benchmarking methodology", covering both flags, how to read the
  table, and the overhead caveats.
- The guide states current behavior only -- no phase history, per
  [`CLAUDE.md`](../../CLAUDE.md) conventions -- and links to this plan by
  GitHub URL, not a relative path, since `docs/upcoming/` is not published.

## 7. Traps

### 7.1 Sanitized builds

The Debug build carries `-fsanitize=address,undefined`. Profiling it measures
ASan's redzone bookkeeping and reports it as `libc` and `native`, badly
skewing every percentage. `__tur_prof_init` detects the sanitizer
(`__has_feature(address_sanitizer)` / `__SANITIZE_ADDRESS__`), pushes a `notes`
entry, and prints a `TUR-W` at startup. **Profile a Release build.**

### 7.2 Overlapping runs

The wall-clock contention documented in [`CLAUDE.md`](../../CLAUDE.md) applies
doubly here: a profile taken while another build or suite is competing for
cores reports contention as time in whatever subsystem happened to be
scheduled. The report front-end prints a load-average line from the capture
and warns above a threshold.

### 7.3 wasm

No `setitimer`, no signals. `--profile` with `--target wasm` is a hard error
from P1 onward, not a silent no-op that hands back an empty profile.

### 7.4 Interpreter

`tur --interpret` runs the tree-walking interpreter, where every sample lands
in `interp.c` and the subsystem table says `native: 98%`. P1 rejects
`--profile` with `--interpret` and points at the compiled path. A useful
interpreter profile means attributing to the *Turmeric* frame the interpreter
is evaluating, which is a distinct feature and out of scope here.

## 8. Testing

Fixtures under `tests/fixtures/prof-*/`, `requires.posix-apis`.

**Assert shape, never numbers.** Percentages are machine-, kernel-, and
load-dependent; a fixture asserting `hamt: 16.7%` is a flake generator. The
assertions that hold:

- A deliberately HAMT-heavy fixture: `hamt` is the top **non-`user`**
  subsystem, and its share exceeds a loose floor (say 5%).
- A pure-arithmetic fixture: `user` exceeds 80%, and `gc` is under 1%.
- Schema: every required key present, `subsystems[].self_pct` sums to
  100 +/- 0.5.
- Compiler side: `tur build --timings /dev/stdout` on a known fixture yields
  every phase name, and `sum(phases[].self_ms)` is within 2% of `wall_ms` --
  the section 3.2 invariant, enforced.
- Overhead: profiled versus unprofiled wall clock on the same fixture stays
  within 5%. This one is a soft assertion with a generous margin, because the
  alternative -- an unmeasured profiler that silently grows to 30% overhead --
  is how profilers stop being used.
- `tools/ci/check-prof-subsys-table.py` in CI (section 4.3).

## 9. Open questions

1. **Default sample rate.** 199 Hz is a guess informed by convention. A
   30-second benchmark gives ~6000 samples, plenty; a 50 ms fixture gives 10,
   which is noise. Should `--profile` auto-raise the rate for short runs, or
   just refuse to report percentages below a sample-count floor and say so in
   `notes`? Leaning toward the latter -- inventing precision is worse than
   declining to.
2. ~~**Does `--profile` on `build` bake in a path, or only arm the
   sampler?**~~ **ANSWERED 2026-08-25: arm only, path from `TUR_PROFILE_OUT`
   at run time.** Folded into section 2, with `tur run`/`tur jit` setting the
   variable for the child so the interactive case stays one step.
3. ~~**`user` versus stdlib.**~~ **ANSWERED 2026-08-25: `stdlib` is its own
   subsystem.** Folded into section 4.3 and the schema in 1.1.
4. **Inlining.** At normal optimization levels an inlined stdlib helper is
   attributed to its caller. Accept it and note it, or add `-fno-inline` behind
   a separate `--profile-no-inline` for when precision matters more than
   realism? Leaning accept-and-note.
5. **Does C2's per-module axis want to be a full cross-product** (phase x
   module) rather than two flat lists? The cross-product is what actually
   answers "which module is slow to typecheck", but it is O(zones x modules)
   rows and a much bigger report. Defer until C0/C1 shows whether the flat
   version leaves the question open.
