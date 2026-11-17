# `tur_turi_frame_reclaim`'s call/cc case dies silently in the Linux aux leg

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
  [turi-callcc-reclaim-depends-on-type-size](turi-callcc-reclaim-depends-on-type-size.md):
  that sweep marks conservatively from a copied C stack, which is exactly the
  sort of thing a runner-image change (kernel, ASLR, libasan) moves.
