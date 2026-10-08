# Compile the runtime preamble once on the cc path

**Status: steps 1-5 done on Linux and Windows. The split is the DEFAULT there
as of 2026-10-02. macOS is still opt-in, and that is the one step left.** See
"Step 5: the flip" below for the re-measured win, a binary-size regression the
flip would have shipped, and the preconditions a default build checks.

History: steps 1-4 landed 2026-09-06/07 (PR #838). `tur build --runtime=split` /
`TUR_RUNTIME=split` emits the decls region and links `libturt_preamble.a`
(a3e63b9d7, 966f2157b). The 20 failures in "Where it actually got to" below were
all fixed on 2026-09-07 -- 16 were duplicated thread-locals (c527e0494), plus the
frame-helper inlining (1b8e71050) and the discarded project includes
(9b72ae63e). CI's `split` and `windows-split` jobs then asserted `0 failed` until
the flip retired them (c1a04d0ec; paper trail in
`docs/archive/cc-path-split-windows-and-hamt-findings.md`). The swap declines
for `#lang r7rs` programs, which got their own prelude split on 2026-09-28.
Step 1's `weak` fix was replaced by the `TUR_RT_SPLIT_HOSTED` guard (a weak
definition broke the PE/COFF link).

**Win: ~10-11% of suite wall-clock, measured.** The ~17% projected below did not
survive measurement, and the ~45% first estimated before that was compile-*only*
and taken while the suite was running.

## Step 5: the flip (2026-10-02)

### What the win actually is

| where | whole preamble | split | |
| --- | --- | --- | --- |
| Linux, 4 cores, no ccache, `TUR_TEST_SHARD=1/10` (349 fixtures), 2 runs each | 81s, 86s | 73s, 73s | -10..-15% |
| Windows CI, sum of the 3 suite shards, mean of 4 green runs | 41.1 min | 36.8 min | -10% |
| one-line program, `tur build` end to end (Linux) | 0.50s | 0.37s | -26% |
| the `cc` part of that build | ~0.23s | ~0.11s | -50% |

The Windows CI rows come from runs 36974523476, 36969943779, 36978132289 and
36969199979. Each pair ran the same commit on the `windows` and `windows-split`
shards. They are noisy (one pair differed by 0.6 min, another by 7.6), which is
why the table shows the mean. Linux CI cannot give a clean comparison: `test`
and the old `split` job warm separate ccaches, and with a ccache hit the cc call
costs nothing on either path.

A full suite under the new default, run as two `TUR_TEST_SHARD` halves to stay
inside the 12-minute cap, came back `1745 passed, 0 failed` and `1744 passed, 0
failed`.

The `cc` call really does halve. The suite moves much less because the rest of
each fixture stays put: `tur` itself (~0.27s of emit for a one-line program on a
Debug build), the run, and the harness.

### The binary-size regression, and the fix

A one-line program's executable grew from 23 KB to 93 KB stripped under the
split (60 KB to 394 KB unstripped on a Debug toolchain). "Linked
all-or-nothing" is literal. `tur_rt_split.c` is ONE object, so the first
reference pulls in the whole preamble. The inline preamble had let `-O2` drop
every `static` function the program never called.

Fixed by building `libturt_preamble.a` with `-ffunction-sections
-fdata-sections` and linking split programs with `-Wl,--gc-sections`. The same
program is now 19 KB stripped, a little under the inline build. ELF only for
now. MinGW's `ld` has the flag, but no Windows run has checked a PE link with
it, so Windows split binaries still carry the whole preamble's code.

### When a default build takes the split

`preamble_split_auto_applies()` in `src/main.c`. Each condition is a case where
the split would build something other than what was asked for. The whole
preamble is the correct answer then, not a degraded one, so declining is quiet.
`--runtime=split` still insists and says so when it declines.

- `libturt_preamble.a` beside the lean `libturt_runtime.a`, and the program's
  own preamble in archive posture (`emit_rcgc_from_archive()`). A
  `--target tur` build has neither archive.
- no `-fsanitize` in `TUR_CC_FLAGS`. The archive is not instrumented, so ASan
  would stop seeing the preamble's heap traffic and TSan its atomics. This also
  keeps `tests/run.sh` under `TUR_TSAN=1` on the whole preamble.
- not `--debug`, not `--target wasm`, and not on top of an r7rs prelude split.
- Linux or Windows. Elsewhere `TUR_PREAMBLE_SPLIT=1` opts in.
- `TUR_PREAMBLE_SPLIT=0` turns it off everywhere.
- and the hash guard in `jit_try_split_preamble` must pass, as before.

How often it engages: 205 of a 227-fixture sample (every 12th) took the split.
The 22 that did not are all correct declines. Most are `#lang r7rs` (its own
prelude split) and saffron programs. Two embed the r7rs-gc collector
(`map-get-miss-on-any-value-is-nil`, `list-helpers-wide-head-element`), which
really is a different preamble. One, `jit-ffi-call-ptr`, adds a `<dlfcn.h>` gate
the canonical emission does not force. Forcing that gate in
`emit_rt_split_source` would win those programs back. It also moves the hash,
so it needs a regenerate.

`libturt_preamble.a` now installs (`cmake --install`) and ships in the release
archives next to `libturt_runtime.a`. Without that, every released toolchain
would have quietly kept the whole preamble.

### CI after the flip

- `test` (Linux) and every `windows` shard probe that a DEFAULT build links
  `-lturt_preamble`, because the split fails closed. A tree that lost it would
  otherwise go green having run the old path. Both suites now run the split.
- `split` became `whole-preamble`: the suite under `TUR_PREAMBLE_SPLIT=0`, with
  the mirror-image probe. That path is still live (`--target tur` builds,
  sanitizer and `--debug` builds, installs without the archive), and nothing
  else compiles it on Linux/gcc.
- `windows-split` is gone. `windows` runs the same configuration by default.

### Left

- **macOS. The run has now happened -- see "macOS, measured" below.** What is
  left is three edits and a comment.
- **`--gc-sections` on MinGW**, to give Windows the size fix too.
- **Version skew between `tur` and the archive.** The hash guard checks `tur`'s
  own preamble against the artifact `tur` was built with. It does not check the
  `libturt_preamble.a` it finds on disk. A stale archive beside a newer `tur`
  would link or misbehave rather than decline. `libturt_runtime.a` has the same
  exposure today. A symbol named after `tur_rt_split_hash`, defined in the
  archive and referenced from the decls region, would turn that into a link
  error.

### macOS, measured (2026-10-02)

Box: macOS 27.0 / Command Line Tools 27.0 / Apple clang 21.0.0, arm64 (M-series),
8 cores. Debug + ASan `tur`, all CMake targets, `libturt_preamble.a` present.

**It is green.** The whole corpus under the split, twice:

| run | summary |
| --- | --- |
| `TUR_PREAMBLE_SPLIT=1 bash tests/run.sh` | `3489 passed, 0 failed` |
| the same plus `-Wl,-dead_strip` on split links | `3489 passed, 0 failed` |

Plus the PR's eleven ctest targets under `TUR_PREAMBLE_SPLIT=1` --
`tur_leak_check`, `tur_leak_gate`, `tur_closure_env_leak`, `tur_fat_shim_leak`,
`tur_r7rs_prelude_split`, `tur_r7rs_eval_link`, `tur_build_project`,
`tur_build_shared`, `tur_install_tests`, `tur_repl_spice_linklibs`,
`tur_gc_runtime_copy_parity` -- 11/11 pass.

Both suite runs happened to share the box with another checkout's full suite
(load average ~140). That contaminates every *timing* number below, but it
makes the green *stronger*, not weaker: contention produces spurious timeouts,
not spurious passes.

Engagement and the decline paths behave exactly as on Linux. 204 of a
225-fixture sample (every 12th) took the split, against Linux's 205 of 227, and
the 21 that declined are the same set: `#lang r7rs`, saffron, the two r7rs-gc
embedders, `jit-ffi-call-ptr`. Checked by hand and all correct:
`TUR_PREAMBLE_SPLIT=0`, no env at all, `-fsanitize` in `TUR_CC_FLAGS`,
`--debug`, `--runtime=source`, and an r7rs program each keep the whole preamble.

**The constructor argument holds, and it is now checked rather than reasoned.**
`otool -l build/src/libturt_preamble.a` has no `__mod_init_func` section and the
generated `tur_rt_split.c` carries zero `__attribute__((constructor))`, so the
Mach-O initializer-order trap from
[r7rs-prelude-split-gc-seam-on-macos](../archive/r7rs-prelude-split-gc-seam-on-macos.md)
genuinely has nothing to bite. `nm -m` also shows no weak definitions in the
Mach-O archive.

**`-Wl,-dead_strip` is not optional on macOS.** The binary-size regression
reproduces at the same magnitude the ELF arm had, and the macOS flag fixes it
the same way:

| one-line program | file | `__text` |
| --- | --- | --- |
| whole preamble | 58,800 B | 10,380 B |
| split, no dead-strip | 171,896 B | 55,296 B |
| split + `-Wl,-dead_strip` | 53,312 B | 1,776 B |

As on ELF, the stripped split build ends up a little *under* the inline one.
Mach-O strips at atom granularity, so the archive's existing
`-ffunction-sections -fdata-sections` is enough; nothing in CMake has to change.

**The timing is NOT established on macOS.** The mechanism does measure: 12
interleaved, randomly ordered `cc` invocations per arm, same emitted-TU
snapshots, the suite's own `TUR_CC_FLAGS`, gave a median of **0.122s inline vs
0.085s split (-30%)**, with the inline arm's whole range above the split arm's.
Interleaving is why that ratio survives a loaded box, and a separate
non-interleaved end-to-end `tur build` pass agreed (0.186s -> 0.140s). What
could not be measured here is the number that matters for CI -- suite wall clock
-- because the only box available was running another checkout's suite
throughout. Two attempts produced 112s/216s for one arm and 142s/147s for the
other on the same shard, which is noise, not a result. Take the suite number
from the macOS `test` leg once the flip lands; do not quote a local A/B for it.

**The stamp cache used to bite when repeating any of this** -- fixed
2026-10-03. `tests/run.sh`'s `stamp_key` did not include `TUR_PREAMBLE_SPLIT`,
so running one mode and then the other PASS-skipped the entire corpus and
reported a full green for a run that never happened -- 7:06 for the real run,
1:14 for the no-op, both `3489 passed, 0 failed`. The key now carries the
resolved preamble mode (and `CC`, and every `TUR_*` knob), and the run's first
line says which path it took: `run.sh: preamble=split ...`. Check that line on
both halves of an A/B.
[run-sh-stamp-cache-ignores-the-preamble-split-mode](https://github.com/rjungemann/turmeric/blob/main/docs/archive/run-sh-stamp-cache-ignores-the-preamble-split-mode.md).

### What the macOS flip still needs

Three edits, a comment, and nothing else:

1. `preamble_split_auto_applies` (`src/main.c:977`): add `__APPLE__` to the
   `#if`, and correct the "macOS never has" prose above it (`src/main.c:963`).
2. **Done** (`src/main.c:3706`): `-Wl,-dead_strip` for `__APPLE__`, beside the
   ELF `--gc-sections`. Landed ahead of the flip because the split is still
   opt-in on macOS, so it only improves the path `TUR_PREAMBLE_SPLIT=1` and
   `--runtime=split` already take.
3. `.github/workflows/ci.yml:337`: the engage probe is
   `runner.os == 'Linux' && matrix.part == 'fixtures'`. The split fails closed,
   so a macOS flip without widening this has no check that it engaged.
4. `tvm/tvm.sh:407`: `__tvm_build_from_source` copies `libturi.a` and
   `libturt_runtime.a` and not `libturt_preamble.a`, so a `tvm install --build`
   toolchain silently never takes the split -- the same miss `cmake --install`
   and the release archives already had fixed. Platform-independent, but it is
   the install path macOS users reach for. (Downloaded releases are fine:
   `macos-arm64` ships the archive and `__tvm_normalize_layout` globs `*.a`.)

The comment: whether macOS also wants a `whole-preamble` counterpart leg.
`ci.yml:947` says "Linux only: macOS still defaults to the whole preamble, so
its `test` legs cover it there", and a flip retires that sentence. **Measured:
nothing is lost, so this is a note to write rather than a leg to add.** Three
checks, because the plausible-sounding version of this concern is wrong:

- **The preamble body stays fully covered.** The split removes 1,679 distinct
  lines from what `cc` receives (8,434 -> 6,618 for a one-line program). ALL
  1,679 are still compiled, under the suite's own
  `-Wall -Wfloat-conversion -Werror=implicit-function-declaration`, by
  `saffron-cons-list` alone -- 100%, not the ~3% an earlier pass here claimed.
  That number came from diffing whole emissions, which counts a one-line
  program's own `main` as preamble; diff the inline TU against the split TU
  instead and the figure is exact.
- **`-w` on the archive does not hide the dangerous class.** `-w` suppresses
  warnings, not errors, and AppleClang 21 makes `-Wint-conversion` and
  `-Wimplicit-function-declaration` errors by DEFAULT -- verified, both still
  fail a `cc -std=c99 -w -c`, exit 1. So the `libturt_preamble.a` build itself
  still catches them. Of `run.sh`'s four ratchet patterns, only
  `-Wincompatible-pointer-types` and `-Wfloat-conversion` are warnings `-w`
  silences, and those are the two the declining fixtures still cover.
- **`tur emit-c` is byte-identical in both modes** (9,738 lines either way), so
  `check-emitted-float-conversions.py` / `tur_emitted_float_conversions` reads
  the whole preamble no matter what `tur build` links. That corpus is at zero
  and stays load-bearing.

There is also a structural reason not to expect trouble: the preamble is FIXED
and byte-identical across every fixture, so it cannot be the source of a *new*
per-fixture warning. What the ratchet actually hunts -- warnings in
program-specific emitted code -- the split does not touch.

The one thing worth recording: that coverage is **incidental, not designed.**
It rests on r7rs and saffron continuing to decline the split. The plan already
contemplates winning declining programs back (forcing `jit-ffi-call-ptr`'s
`<dlfcn.h>` gate in `emit_rt_split_source`); doing the same for saffron would
silently remove the last macOS job that compiles the preamble with warnings on.
Note the dependency at `ci.yml:947` when the flip retires that comment, so a
later change has to notice.

## The waste

Every fixture's emitted TU carries the fixed runtime preamble, and `tur build`
hands the whole thing to `cc`:

```
emitted C for a ONE-LINE program:  8310 lines
  of which fixed runtime preamble: 4360 lines   (byte-identical every time)
```

2816 fixtures each recompile those 4360 lines. Nothing caches it: a single `cc`
call that compiles *and* links is uncacheable, which
[tur-link-and-build-split-plan](../archive/tur-link-and-build-split-plan.md)
already measured as "48/48 Uncacheable, 0 hits". That plan split compile from
link and added `--runtime=lib`, but `--runtime=lib` swaps the *autolinked
runtime sources*; the preamble is still inline in every program TU.

## Measured, idle box, gcc 16 / UCRT64

| TU handed to cc | lines | compile+link |
| --- | --- | --- |
| full (today) | 8310 | 1.31s |
| decls + program, prebuilt runtime | 5504 | 0.94s |

**29% off the `cc` call.** A fixture costs ~2.2s end to end (`tur` 0.5s, `cc`
1.3s, run + harness ~0.4s), so this is **~17% of the suite** -- roughly 30 min
to 25 on the Windows CI leg, and proportionally everywhere else. (A projection.
Step 5 measured ~10%, on CI and on Linux.)

That is worth having and is not a transformation. If the goal is a big cut, the
next-largest item is `tur` itself at 0.5s per fixture for a one-line program,
which is stdlib elaboration and a separate investigation.

## Proven

```sh
# runtime, compiled ONCE
gcc -O2 -c -o rt_split.o src/runtime/generated/tur_rt_split.c -Isrc -Isrc/runtime
gcc -c -o sjlj.o src/async/tur_sjlj_x64_win.S            # Windows only

# program half + prebuilt runtime
gcc -O2 -o t.exe split.c rt_split.o sjlj.o -lturt_runtime -L... -lm -lpthread ...
./t.exe   # -> correct output
```

The S2 split already produces exactly the text needed: `jit_try_split_preamble`
swaps the emitted preamble for `tur_rt_split_decls`, and the same swap applied
to `tur build`'s buffer is the whole change on the emitter side.

## The three obstacles, and what each needs

**1. `tur_closure_headers_enabled` is defined in both halves.** The generator
externs `static` globals in the decls half (`gen-runtime-split.py`, the
`extern {head_no_init}` branch) but this one is *non-static*, so it falls to
"keep verbatim in both" and collides at link.

Do **not** fix it by externing it in the decls half. `jit_sync_config_globals`
(`src/jit_engine.c`) locates it as a MIR **data item** in the program module and
copies its value onto the host's weak copy; with no definition there is no data
item and that handshake silently stops working. The JIT would keep building and
quietly use the wrong value.

Fix instead in the *impl* half: emit non-static global definitions with
`__attribute__((weak))`. The program half keeps its strong definition, so it
wins on the cc path and the JIT is untouched.

**2. `tur_sjlj_set` / `tur_sjlj_jump` are unresolved.** On the JIT path they
come from `JIT_SHIMS`; a cc-path link needs `src/async/tur_sjlj_x64_win.S` in
the archive (Windows only).

**3. The archive cannot simply gain `tur_rt_split.o`.** Adding it to
`libturt_runtime.a` is *not* inert: today's programs define those symbols
themselves, so any link that pulls the member for one symbol collides on the
rest. It needs its own archive (`libturt_preamble.a`), linked only in split
mode.

## Where it actually got to (2026-09-06)

`tur build --runtime=split` / `TUR_RUNTIME=split` is **wired and works for
simple and effectful programs, and is NOT correct for the suite.** Full run:
**2798 passed, 20 failed** against 1 failed in default mode. It is opt-in and
off by default, so nothing else is affected, but it must not be defaulted until
these are understood.

Landed and sound:

- generator emits the guarded `TUR_RT_SPLIT_HOSTED` forms (below), hash
  unchanged, JIT verified unaffected
- `libturt_preamble.a` -- its own archive, linked all-or-nothing
- `--runtime=split` + `TUR_RUNTIME=split`, reusing the JIT's own
  `jit_try_split_preamble` so the paths cannot drift, and declining LOUDLY
  (falling back to the full preamble) if the hash no longer matches
- the swap runs BEFORE `hoist_tur_include_directives`, because hoisting lands
  includes inside the preamble region and the swap was deleting them

### The 20, and what they say

| fixture class | symptom |
| --- | --- |
| `async-echo-server`, `async-file`, `async-timer-basic` | `no scheduler` |
| `fiber-local` | no output at all |
| `panic-catch-panic-of`, `panic-with-catch-of`, `catch-unwind-branch-result-return` | reaches a `should-not-reach` branch |
| `gc-registry-growth`, `schema-reader-json-*` | stdout mismatch |
| `hamt-lowering-basic` | build failed |

This is **not** a declarations problem -- `no scheduler` and a panic taking a
`should-not-reach` branch are runtime behaviour.

**Two hypotheses tested and RULED OUT**, so nobody re-runs them:

- *Duplicate state across the seam.* `nm`-diffing an async fixture's program
  object against `libturt_preamble.a` for defined data symbols returns
  **zero**. (The same diff on a trivial program is what found
  `tur_closure_headers_enabled`, so the method works.)
- *Two runtime vintages.* `tur_scheduler` appears in both
  `libturt_preamble.a` and `libturi.a`, which looks like the seam-3 hazard the
  S2 notes describe -- but `tur_rt_split.c` is a source of BOTH, so they are
  the same vintage, not a mix.

So the state is single-instance and single-vintage, and it is still not
initialized. That points at **initialization order or reachability**: something
the full preamble did on the way in is not happening when those definitions
arrive from an archive instead. `__tur_static_init` is the obvious suspect --
the generator deliberately keeps it `static` in the program half and drops it
from the runtime TU ("which neither defines nor calls it") -- so the next step
is to trace, on one async fixture, who is expected to create the scheduler and
whether that code runs at all under split.

Do that before touching the build wiring again; the wiring is not the problem.

## Implementation order

1. Generator: weak non-static global definitions in the impl half. Regenerate
   `src/runtime/generated/*`.
2. Confirm `tur_rt_split_hash` is **unchanged** -- it covers
   `emit_rt_split_source()`, the canonical preamble emission, not the generated
   artifacts, so a decls/impl change must not move it. Verify with
   `TUR_JIT_SPLIT_DEBUG=1` (prints probe vs committed) and re-run the JIT
   corpus. A silently-disengaged split is a documented failure mode:
   [jit-s2-split-disengages-on-hoisted-inline-c-include](../archive/jit-s2-split-disengages-on-hoisted-inline-c-include.md).
3. New `libturt_preamble.a` from `tur_rt_split.c` + the sjlj asm.
4. `tur build` mode that swaps the preamble for the decls region and links the
   new archive. Keep `tur emit-c` emitting the full self-contained TU, so the
   ~2816 `expected.c` snapshots do not move and the user-facing artifact stays
   standalone-compilable.
5. Full suite under the new mode, then flip the default.

## Watch for

`hoist_tur_include_directives` (a JIT post-pass) injects `#include "hamt.h"`,
whose real prototypes conflict with the emitted loose `extern void
*tur_hamt_new();`. That is why a JIT dump does not drop straight into `cc` --
it is an artifact of that pass, not of the split, and the cc path does not have
it. Do not "fix" the loose prototypes chasing this.
