# `tur_turi_frame_reclaim`'s call/cc case dies silently in the Linux aux leg

**RESOLVED 2026-11-24: a call/cc image taken outside every top-level form ran
over the interpreter thread's TLS.** The image now stops at the outermost
interpreter frame, and the check asserts that it does; see "Root cause and
fix" at the end.

**Severity:** medium (CI red on `main` and every PR). Since 2026-11-13 the
`Auxiliary suites (ubuntu-latest)` job fails on `tur_turi_frame_reclaim` in
nearly every run: 5 of the last 5 completed `main` runs, plus PR #1476 (a
`web/` icon-package bump that cannot affect it). Same SHA `3e035f73` passed
2026-11-12, failed 2026-11-13 and passed 2026-11-14, so it is intermittent,
not a code change in that window.

## What CI shows

The first five programs pass; the sixth (the re-entrant `call/cc` one, which
pins r7rs-callcc-memory-never-freed's capture epoch) fails identically every
time:

```
FAIL check-turi-frame-reclaim: call/cc program printed '' (status 256), want '1799970000\n60000\n1799970000\n60000\n(hits 2 v 1)\n'
```

Status 256 is exit code 1. Empty stdout means the process ended without its
stdio buffer being flushed -- the shape of a sanitizer report (ASan and LSan
`_exit(1)`) or a turi error before the first `display`. Until now the script
discarded stderr, so the cause was never printed.

## Repro (does not reproduce locally)

```sh
python3 tests/check-turi-frame-reclaim.py ./build/tur
```

Debug (ASan/UBSan) `tur` at `6112ce23`, gcc 13.3, 4 cores: the check passes,
the call/cc program alone passes 40/40, and 48/48 with six copies running in
parallel. It reads only stdlib files (strace: no writes), so a file race with
the tests co-scheduled under `ctest -j` (`tur_turi_callcc_reclaim`,
`tur_turi_pool_sweep`, `tur_shard_partition`) is ruled out. Leaving LSan on
(`detect_leaks=1`) does reproduce the shape exactly -- empty stdout, exit 1 --
from a 32-byte leak at `eval_drive_ex` (`src/turi/eval.c:11280`), but the
check sets `detect_leaks=0`, so that is a separate finding, not this one.

## Fix directions

- The check now prints the last 40 lines of a failing program's stderr under
  its FAIL line (`stderr_tail`). The next red run names the error; start there.
- If it is an ASan report in the call/cc stack-image path, compare with
  [turi-callcc-reclaim-depends-on-type-size](../reported/turi-callcc-reclaim-depends-on-type-size.md):
  that sweep marks conservatively from a copied C stack, which is exactly the
  sort of thing a runner-image change (kernel, ASLR, libasan) moves.

## Root cause and fix (2026-11-24)

**What the runner showed.** With stderr captured, the crash was inside ASan
itself: `free` -> `Allocator::QuarantineChunk` -> `StackDepotBase::Put`, a
read through a non-canonical address. ASan's one-line summary skips its own
frames, which is why the first red run named
`frame_pin_bare_tyvars_from_args` (`eval.c:8226`). A temporary bisection in
the check, run on the runner (AMD EPYC 7763, kernel 7.0.0-azure, Ubuntu
26.04.1, glibc 2.43, GCC 15.2; `ubuntu-latest` is 26.04 on AWS-hosted
runners), gave:

| the call/cc program, on the runner | result |
| --- | --- |
| as the check runs it, twice | failed, then passed |
| shadow stack off (`GLIBC_TUNABLES=glibc.cpu.x86_shstk=off`) | failed |
| pool sweep off, continuation sweep off, frame reclaim off (each) | failed |
| ASLR off (`setarch -R`), twice | passed, passed |

So not the sweeps, not CET: it depended on address layout. None of these
reproduced it locally (64 runs in all): gcc 13.3 / glibc 2.39; gcc 15.2 from
conda-forge; Ubuntu 26.04's own gcc 15.2 / glibc 2.43 in a chroot, with its
GCC 16 `libasan8` and the `amd64v3` variant packages the runner installs;
`vm.mmap_rnd_bits=32`.

**The cause.** `TUR_R7K_TRACE=1` (new, below) showed the program's capture
running under no prompt: `(define v (call/cc ...))` is a `define`, and
`r7rs-toplevel__` wraps only expression forms, so `g_r7k_form_base` was NULL
and `r7k_snapshot` took the image up to `r7k_stack_base()` -- the top of the
thread's stack block, from `pthread_getattr_np`. `tur` interprets on a
pthread (`tur_run_on_big_stack`), and glibc keeps a pthread's static TLS and
its control block at the top of that block: a thread-local's address printed
inside the image's range. Every re-entry `r7k_copy`'d the capture's stale copy
of the thread's TLS back -- glibc's, ASan's and the interpreter's own -- in an
uninstrumented function, so ASan never saw the write. Whether the stale bytes
mattered depended on what they held, hence on the layout: on the runners
ASan's allocator state went inconsistent with its stack depot and crashed it;
locally it never did.

**The fix.** An image taken outside every form now stops at the outermost
call into the interpreter on the thread: `turi_eval_with_sink` and
`turi_call` register their frame (`turi_cont_entry_enter`/`_leave`,
`src/turi/eval.h`), and `r7k_snapshot` bounds such a capture there, below the
embedder's frames and the TLS. The thread's stack base remains only for an
entry path that registers no frame. A restore of an entry-bounded image needs
that entry frame live and refuses otherwise ("captured in another call into
the interpreter"), rather than overwrite whatever frames sit there now. The
program's answer is unchanged: re-entering finishes the `define` and re-runs
the later forms, as before.

**The test.** The crash only happens where the stale bytes matter, so it is
no test. `tests/check-turi-frame-reclaim.py` now runs the call/cc program
with `TUR_R7K_TRACE=1` and fails if any capture's image reaches the thread's
stack base. Without the fix it fails on every machine; with it it passes.
Checked with the fix: the check, `check-turi-callcc-reclaim.py`,
`run-r7rs-conformance.sh` (1223 passed on each engine), `run-r7rs-gc.sh`
(270), `run-r7rs-sanitize.sh` (137), `run-r7rs-import.sh`,
`turi/repl-lang-r7rs.sh`, and `run-turi.sh` over the continuation, `eval`
and R7RS fixtures (311 passed, 0 failed).
