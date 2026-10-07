# Aborting fixtures intermittently time out on the Linux JIT leg

**Severity: low (CI noise on a `continue-on-error` leg).** Fixtures whose
program ends in `abort()` -- a panic, a failed contract, a failed cast --
sometimes run past `run-jit.sh`'s 15 s budget on `JIT engine (ubuntu-latest)`,
though each takes about 0.5 s locally. The leg cannot turn red, so these
failures stay invisible unless someone reads the log, and a real JIT
regression on one of these fixtures would be lost among them.

## Evidence

Two of the three `main` runs checked on 2026-10-06:

| run | timeouts (`timed out (>15s under the JIT engine)`) |
| --- | --- |
| 37412067483 | none |
| 37484233418 | `any-cast-mismatch-panic`, `any-cast-wrong-instantiation`, `any-fn-wrong-signature-cast-panics`, `any-cast-wrong-type-panics`, `refine-match-impure-arm` |
| 37514742502 | the same four `any-cast-*`, then `contract-ensure-fail`, `contract-assert-fail`, `contract-require-fail`, `panic-no-unwind-abort`, `refine-int-real-literal-not-contradictory`, `refine-loop-invariant-byvalue-mutation-not-proved`, `refine-loop-invariant-shadowed-reads-not-trusted` |

All 16 end in SIGABRT under `tur jit` (exit 134 locally), and all pass
locally, in about 0.5 s each. They are not a random sample: 61 of the 91
`expected.exit` fixtures abort, out of ~3500 in the corpus. They also fail in
clusters of alphabetical neighbours with timestamps a second or two apart, and
`run-jit.sh` dispatches alphabetically across `nproc` workers. So they abort
**at the same time**.

(The `r7rs-eval` `stdout mismatch` in the same runs was a separate, real bug,
fixed as
[r7rs-eval-host-procedure-name-on-the-stack](../archive/r7rs-eval-host-procedure-name-on-the-stack.md).)

## Hypothesis (unverified)

Under the JIT the aborting process is the sanitized `tur` itself, not a small
unsanitized binary as under `run.sh`. Ubuntu pipes core dumps to a handler
(`kernel.core_pattern` is `|/usr/share/apport/apport ...` on stock images), and
the kernel ignores `RLIMIT_CORE` for a pipe, so ASan's `disable_coredump` does
not stop the dump. The dying process is not reaped until the handler has read
the core, and apport serializes on a lock. A core of a large ASan process,
taken several at a time, could cost seconds each and queue past 15 s.
Nothing here has been checked on a runner.

## Next steps

1. Confirm the mechanism on a runner: print `cat /proc/sys/kernel/core_pattern`
   in the JIT job, and time one aborting fixture alone and four together.
2. If confirmed, stop the dump for the harness's sanitized `tur`: either
   `ASAN_OPTIONS=...:handle_abort=1` in `run-jit.sh`, so ASan catches SIGABRT
   and `_exit`s with its `exitcode` (still `nonzero`, so `expected.exit`
   holds), or switch off apport/`core_pattern` in the workflow. The first keeps
   the fix in the harness and works on any host.
