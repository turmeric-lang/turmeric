# `rp6-watch-with-help` fails ~1 run in 300: SIGPIPE + `pipefail`, not a parser bug

**Severity:** low (no product defect) -- but it is the third distinct cause to
produce this exact failure text, and the failure message actively lies about
what happened.

**Status (2026-08-01): FIXED** -- `tests/turi/repl-spice-watch.sh` scenarios 4
and 5 now capture output before grepping.

## Symptom

`tur_repl_spice_watch` fails in CI on a single assertion:

```
PASS rp6-watch-no-changes
PASS rp6-watch-edit-auto-reload
PASS rp6-no-watch-skips-auto-reload
FAIL rp6-watch-with-help -- expected help output to mention 'tur repl'
PASS rp6-watch-unknown-flag

repl-spice-watch: 4 passed, 1 failed
```

Observed on
[run 30715409287](https://github.com/turmeric-lang/turmeric/actions/runs/30715409287/job/91410036267)
(main @ `30ca0f89`, ubuntu-latest, Debug + ASAN). The message is false: the
help output does mention `tur repl`, every time.

## Root cause

Scenarios 4 and 5 were the only two assertions in the file that piped `tur`
straight into `grep`:

```sh
if "$TUR" repl --watch --help 2>&1 | grep -q 'tur repl'; then
```

Three facts combine:

1. `grep -q` exits the instant it matches, closing the read end of the pipe.
   The match is on line 2 of a ~900-byte usage block.
2. `usage_repl()` (`src/main.c:8157`) writes via `fprintf(stderr, ...)`.
   stderr is unbuffered, and glibc emits an unbuffered stream in **several**
   `write()` calls -- so `tur` is typically still writing when `grep` leaves,
   and takes SIGPIPE. Exit status 141.
3. The harness sets `set -uo pipefail` (line 19). With `pipefail` the
   pipeline's status is the last non-zero member, so the pipeline reports
   **141 even though `grep` matched and exited 0**.

The `if` therefore takes the else branch and calls `fail` with a message that
describes a condition that never occurred.

Measured directly (Debug + ASAN, 4-core box), instrumenting `PIPESTATUS`:

```
round 1: 0 / 300
round 2: 1 / 300  [i=74 tur=141 grep=0]
round 3: 0 / 300
```

`tur=141 grep=0` is the whole diagnosis in one line: the text matched; the
producer was killed for writing to a pipe nobody was reading.

This is why only *one* of the two assertions failed in CI. They race
independently, at roughly 1-in-300 each, so the common outcome is exactly one
of them tripping.

## Why it kept getting misdiagnosed

`docs/archive/history/ctest-parallel-contention-false-failures.md` attributes
this same message to the compiler binary being relinked mid-run, and
`docs/archive/history/repl-spice-watch-flake-plan.md` attributes the target's
flakiness to wall-clock races in scenarios 2/3. Both were real and both were
fixed. Neither is this.

The binary-moved theory is also cleanly falsifiable here: a non-executable
`./build/tur` fails scenarios 4 **and** 5 together, and CI showed 4 alone.
And the CI job cannot have been contended -- it runs `ctest` with no `-j`,
serially, after the build step has completed.

Three causes, one message. The message was the problem: `fail` was reporting
the *hypothesis* the assertion was written to test rather than what was
observed.

## Fix

Capture first, then grep -- `$(...)` reads to EOF, so the producer never sees
a closed pipe and SIGPIPE cannot occur:

```sh
out=$("$TUR" repl --watch --help 2>&1)
if echo "$out" | grep -q 'tur repl'; then
```

Both failure messages now append `got: $out`, matching scenarios 1-3, so a
future failure shows the actual output instead of asserting a conclusion
about it.

A repo-wide scan for the same shape (`set -o pipefail` plus a non-builtin
piped into `grep -q`) found these two lines and nothing else, so the fix is
complete rather than representative.

## Verification

| | before fix | after fix |
|---|---|---|
| scenario 4 + 5 assertions, tight loop | 3 / 300 and 1 / 300 | **0 / 2400** |
| full harness | 5 passed intermittently | 5 passed, 3/3 runs |
| `ctest -R tur_repl_spice_watch --repeat until-fail:3` | -- | Passed |

## Residual

`grep -q` is a fine thing to write; `grep -q` as a pipeline consumer under
`pipefail` is a footgun, because a matched pattern and a dead producer are
indistinguishable in the exit status. The general rule for this repo's
harnesses: **under `pipefail`, do not put a process under test on the left of
`| grep -q`.** Capture it.
