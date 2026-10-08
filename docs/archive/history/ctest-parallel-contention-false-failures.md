# Timing-sensitive ctest targets report false failures under parallel/contended runs

**Severity:** low (no product defect) -- but it costs real investigation time
and, worse, teaches people that red is normal.

Reported by the agent working on
[Trowel](https://github.com/turmeric-lang/trowel) (`turi_fixture_tests` fails
under `ctest -j4`, passes in isolation). Hit independently here twice, on two
different targets, from a different cause -- so the pattern is broader than one
test.

## Status (2026-07-27): FIXED

With one correction to this report's own diagnosis, below.

`ctest -j4` over the four heaviest targets, before and after:

| | before | after |
|---|---|---|
| `tur_tests` | **FAILED** (307s) | Passed (289s) |
| `turi_fixture_tests` | **FAILED** (141s) | Passed (69s) |
| `tur_cli_tests`, `tur_fmt_tests` | Passed | Passed |
| overall | 2 failed of 4 | **0 failed of 4** |

Note the scope was larger than reported: `tur_tests` fails under `-j4` too,
and it is the target that reports `2399 passed, 0 failed` in isolation. Note
also that `turi_fixture_tests` runs in *half* the time serialized (141s -> 69s)
-- contending for cores was costing more than waiting for them.

### Correction: `tur_repl_spice_watch` was never a contention failure

This report grouped it under contention. That is wrong, and it took a
falsification test to see:

- Under 8 spin loops on a 4-core box -- heavier load than any `-j4` run --
  `tur_repl_spice_watch` passes **5/5**.
- With a mid-run relink window simulated (`chmod -x build/tur` for four
  seconds), it fails `rp6-watch-with-help` and `rp6-watch-unknown-flag`,
  **exactly** the two assertions and messages originally observed.

Both of the instances hit here were therefore the *same* cause as the
`refine-*` batch -- the binary changing underneath the run -- and only the
consumer-reported `turi_fixture_tests` was genuine contention. The table in
the Summary below is left as originally written, with this correction
attached, because the mistake is the instructive part: two failure modes with
completely different fixes produce failures that read identically.

## Summary

Several targets assume they have the machine. When they do not, they fail in
ways that look like genuine product bugs and cost a real investigation before
being dismissed.

Three observed instances:

| Target | Trigger | Symptom |
|---|---|---|
| `turi_fixture_tests` | `ctest -j4` | fails under `-j4`, passes alone (~162s alone) |
| `tur_repl_spice_watch` | a concurrent `tests/run.sh` | `rp6-watch-with-help`, `rp6-watch-unknown-flag` fail; both pass when run directly |
| 25 x `refine-*` fixtures | rebuilding `./build/tur` mid-run | `build failed`, with `actual.stderr` reading `timeout: failed to run command './build/tur': Permission denied` |

The third is not contention but the same class of problem: the suite reads the
binary it is testing straight out of `build/`, so anything that relinks while
it runs poisons an arbitrary subset of fixtures with an error message that
looks nothing like its cause.

## Repro

```sh
# In one shell:
bash tests/run.sh
# In another, while the above is running:
ctest --test-dir build -R tur_repl_spice_watch --output-on-failure
```

`rp6-watch-with-help` fails claiming the help output does not mention
`tur repl`. It does:

```sh
$ ./build/tur repl --help | head -2
usage:
  tur repl [--watch]   start the interactive REPL
```

## Why it misleads

The failure text never mentions timing. `expected help output to mention 'tur
repl'` reads as a broken argument parser, and `build failed` on 25 refine
fixtures reads as a compiler regression. Both cost a full diagnostic pass
before the environment turns out to be the culprit. `CLAUDE.md` already warns
that a slow full-suite run usually means CPU contention rather than a hang;
the same warning is worth extending to *failures*, not just wall-clock.

## What was done

All three fix directions, with the third narrowed by the correction above.

**`RUN_SERIAL` on `tur_tests` and `turi_fixture_tests`.** These are the only
two ctest harnesses that fan out across `nproc` internally, so `ctest -jN` runs
not N tests on the machine but `N x nproc` processes on it, and their
per-fixture timeouts (10s compiled, 15s interpreted) expire on work that would
otherwise finish easily. `RUN_SERIAL` gives them the machine, which is what
they already assumed. Not applied to `tur_repl_spice_watch`: it was never a
contention failure, and marking it serial would have "fixed" it by coincidence
while leaving the real cause live.

**A binary-stability guard in `tests/run.sh`.** The identity of `$TUR`
(`ls -ln`, portable where `stat -c`/`-f` are not) is captured at startup and
re-checked at the end. On a change it prints the before/after stamps and what
they mean, and -- if anything failed -- exits 2 rather than reporting a normal
result, on the same principle as the existing completeness guard: a run that
cannot be trusted is neither a pass nor an ordinary failure.

Demonstrated against the original symptom by holding `build/tur` non-executable
for six seconds mid-run:

```
timeout: failed to run command './build/tur': Permission denied
FAIL vec-typed-fat-closure-readback -- tur build failed
...
WARNING: ./build/tur changed while this run was in progress.
  before: -rwxr-xr-x 1 0 0 51736408 Jul 27 18:32 ./build/tur
  after:  -rw-r--r-- 1 0 0 51736408 Jul 27 18:32 ./build/tur
  Something rebuilt the compiler mid-run ...
  65 failure(s) recorded -- THIS IS NOT A VALID RUN.
exit code: 2
```

It then caught a real one unprompted: the `-j4` verification run was still
going in the background while those guard experiments ran, and it correctly
flagged that run's 772 failures as artifacts. Third occurrence of this mistake
in a single session, which is roughly the point.

**A `CLAUDE.md` section.** The existing "READ BEFORE ASSUMING A HANG" note
covers wall-clock only. The new one covers false *failures*, keeps both causes
separate with their distinguishing symptoms, and states the rule of thumb:
before diagnosing a failure, check whether anything else was building or
testing at the same time, and re-run alone before believing it.

## Residual

The guard is only in `tests/run.sh`. Other harnesses -- `repl-spice-watch.sh`
among them -- still fail opaquely if the binary moves under them, which is why
the `CLAUDE.md` note describes the *shape* ("assertions that pass when you run
them by hand were probably never really run") rather than relying on tooling.
A shared helper would be the tidier answer if this recurs.
