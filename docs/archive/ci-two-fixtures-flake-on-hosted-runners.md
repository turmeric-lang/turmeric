# Two fixtures flake on hosted runners: `httpd-mw-rate-limit` and `rp7-reload-self-heal`

**RESOLVED 2026-09-11 -- both halves.** The `rp7-reload-self-heal` half was
root-caused and fixed 2026-09-09 (see the update below: the harness polls for
the failed startup load instead of sleeping a fixed second).  The
`httpd-mw-rate-limit` half is the middleware's `Retry-After`, which is
`window - age` in whole seconds: it reads `60` only when the third request
lands in the same second as the first, and a one-second stall on a loaded
runner prints `59` -- a stdout mismatch that looked like an answer and was a
clock.  The fixture now asserts the header is a positive integer no larger
than the window (`r3-retry-after=ok`) and its client retries `connect()`
briefly rather than assuming the server is up after a fixed 30 ms sleep.
Passes under `run.sh`; the `run-jit.sh` leg (where it flaked) was not
verifiable on the fixing box, which carried no JIT engine -- the only new
libc call in the client is `strtol`, which c2mir already links.

**Summary (at filing):** Both failed on a **documentation-only** PR and both passed on a
re-run of the *identical tree* with no change whatsoever. They are flaky, not
broken. Recorded because the cost is not the re-run -- it is the half hour spent
deciding whether a red CI belongs to you.

**Severity:** Low individually; medium as a pair. Neither points at a product
defect. But a suite that goes red for reasons unrelated to the commit under test
trains people to skim past red, which is the expensive failure mode.

**Platform:** GitHub hosted runners. `httpd-mw-rate-limit` seen on
`JIT engine (macos-latest)`; `rp7-reload-self-heal` on `Test (ubuntu-latest)`.
Neither reproduced locally.

## Evidence that it is flake, not breakage

Observed on [PR #836](https://github.com/turmeric-lang/turmeric/pull/836), whose
entire diff is three markdown files -- no code, no tests, no build files -- and
whose merge-base *is* `origin/main` (`2da89e84f`), the same commit whose own CI
run was green.

| | first attempt | re-run, same tree |
| --- | --- | --- |
| `JIT engine (macos-latest)` | FAIL | **pass** (12m24s) |
| `Test (ubuntu-latest)` | FAIL | **pass** (15m54s) |

Nothing was pushed between the two. `gh run rerun --failed` on the same run id.

**A method note, because the wrong approach looked reasonable.** The first thing
tried was scanning recent runs for a pattern, on the theory that a recurring
failure is a flake. That *disconfirmed* the hypothesis and was misleading:
`JIT engine (macos-latest)` had indeed failed in three of the four preceding
failing runs -- but on `j2-load-in-process`, `cps-mixed-coloring` and
`van-laarhoven-lens-*`, which were real defects on a branch that had real
defects. Same job name, unrelated cause. History tells you a job is *fragile*;
only a re-run of the same tree tells you a failure is *spurious*.

## What each looked like

### 1. `httpd-mw-rate-limit` -- stdout mismatch

```
85: FAIL httpd-mw-rate-limit -- stdout mismatch
85: jit fixture summary: 2718 passed, 1 failed, 59 skipped
```

One fixture out of 2718. The fixture exercises a **rate limiter**, so it is
timing-dependent by construction, and a shared runner is exactly where a
wall-clock assumption gets violated.

Note also the guidance in CLAUDE.md that the harnesses used to diff stdout
before reporting a timeout, so a killed fixture surfaced as `stdout mismatch` --
a claim about the answer when the truth was about the clock. Worth confirming
which this is before treating the mismatch as meaningful.

Related but distinct, and not the same bug:
[windows-httpd-async-limit-hangs-on-ci](windows-httpd-async-limit-hangs-on-ci.md)
is a different fixture on a different platform, and
`docs/archive/httpd-mw-rate-limit-state-leak.md` is closed.

### 2. `rp7-reload-self-heal` -- failure plus a LeakSanitizer report

```
FAIL rp7-reload-self-heal -- <U+2597>            <U+2598>
==127266==ERROR: LeakSanitizer: detected memory leaks
repl-spice-errors: 4 passed, 1 failed
```

(The two `<U+....>` above are quarter-block glyphs, transcribed rather than
pasted so this file stays ASCII.)

Two things worth noticing. The assertion text arrived carrying **spinner
artifacts** -- the recorded expectation is competing with a progress indicator,
which is an output-capture race rather than a behavioural difference. And the
LeakSanitizer report is almost certainly a *consequence* of the failure path
rather than an independent leak, since the same binary is leak-clean on the
re-run; do not chase it as a separate defect without confirming that.

`repl-spice-errors` drives a REPL that shells out to real subprocess builds, so
it carries more timing surface than most.

## Suggested next steps

Cheap, in order:

1. **Make the timing assumption explicit** in `httpd-mw-rate-limit` -- assert on
   ordering or on counts rather than on a wall-clock-sensitive transcript, or
   widen the window. First confirm whether the mismatch is a genuine diff or a
   truncated-by-timeout transcript.
2. **Stop the spinner from reaching captured output** in the repl-spice harness
   when stdout is not a TTY. That removes a whole class of false failure and
   makes the real assertion legible when it does fail.
3. Only then consider whether either deserves a retry wrapper. Retries hide
   flakes rather than fixing them, and both of the above are small.

## What this is not

- Not a regression from PR #836, which changes only markdown.
- Not evidence that `main` is broken -- `main`'s own runs are green.
- Not the same as the archived `httpd-mw-rate-limit-state-leak`, which was a
  real state-leak defect and is fixed.

## Update 2026-09-09: the `rp7-reload-self-heal` half is root-caused and fixed

It was not an output-capture race. The harness slept a fixed second after
starting the REPL and then rewrote `src/lib.tur` with the fixed source. On a
loaded runner (the auxiliary suites run under `-j`, and on
[#846](https://github.com/turmeric-lang/turmeric/pull/846) the `test` job was
also warming a fresh Emscripten) the REPL had not yet performed its failing
startup load when the fix landed, so the STARTUP load succeeded
(`Loaded spice from ... (1 export)`), `(reload)` reported `no changes`, and
the assertion on `(reload) loaded 1 export` failed. The spinner glyphs were
a red herring: they are the banner. `tests/turi/repl-spice-errors.sh` now
polls `out.log` for the startup diagnostic (`fix the error above`, the same
line scenario 2 asserts) before rewriting the source, then still waits one
second so the rewrite's mtime moves past the failed build's. The LSan
report in that failure (13 bytes in `tur_ffi_install_spice_bindings`) is
real but does not decide the test; it is the interpreter's process-lifetime
binding, on the path where the spice loads at startup and `(reload)` has
nothing to do.

`httpd-mw-rate-limit` (macOS JIT) is unchanged and this report stays open
for it.
