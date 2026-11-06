# The Windows JIT corpus is not in CI, and would fail the fallback ratchet there

**Severity: low.** A coverage gap, not a defect in shipped behaviour. Every
fixture on the list below still gets the right answer on Windows, through the
cc fallback. But nothing in CI would notice a Windows JIT regression beyond
the smoke tests.

**RESOLVED 2026-11-06** (#1462): the `windows-jit-corpus` job is green --
4128 passed, 0 failed across its three shards on windows-latest, with the
77-name overlay and `jit-inline-c-thread-local` taking the cc fallback as on
Linux. A new Windows-only fallback now fails the job.

**Worked 2026-11-06: fix direction 1, and the corpus is in CI.**
`tests/run-jit.sh` reads `tests/jit-fallback-baseline-windows.txt` as an
overlay on the shared baseline when `MSYSTEM` is a Windows environment
(`TUR_JIT_FALLBACK_UPDATE=1` there rewrites only the overlay), and a sharded
run now prints the fallbacks it saw, so the union of the shards is the
host's whole set. `ci.yml`'s new `windows-jit-corpus` job runs the corpus in
three shards on every code PR. Its first run (on #1462) set the overlay
to what Windows measured: 77 names, up from this report's 47 as the corpus
grew. It also found two more gaps, both fixed in the same PR:

- **`run-jit.sh` gave fixtures no Windows scratch directory.** `run.sh`
  exports a `cygpath -m` `TUR_TEST_TMPDIR`; `run-jit.sh` exported none, so a
  native fixture binary fell back to `/tmp`, which it resolves to `C:\tmp`,
  absent on a fresh runner. Nine fixtures printed nothing (`fs-read-string`,
  `io-stdlib-roundtrip`, `serial-typed-surface`, the `image-*` round trips,
  ...). It exports the same variable now, under its results directory.
- **The engine compiled `_Thread_local` as one shared global on Windows.**
  The refusal of jit-inline-c-thread-local-collapses-to-global reads c2mir's
  warnings through `open_memstream`, which MinGW lacks, so it was POSIX-only
  and `jit-inline-c-thread-local` printed 99 there. Windows collects the
  messages in a delete-on-close temp file instead (`jit_msg_capture_open`,
  `src/jit_engine.c`), and the program falls back to cc as on Linux.

The report closes when the job is green.

**Status: open.** Split out 2026-09-26 from section 4 of
[jit-windows-support-spike](../archive/jit-windows-support-spike.md) when that
spike was archived.

## What CI runs today

The `windows-jit` job builds `-DTUR_JIT=ON` and smoke-tests:

- `hello` in all three generation tiers;
- the same `hello` on the whole-preamble path, plus a variadic inline-C
  definition on both paths, failing on any TUR-W0070 fallback (added
  2026-09-26);
- `tests/turi/repl-spice-jit.sh`.

It does not run `tests/run-jit.sh`. The Linux and macOS `jit` jobs do.

## What running the corpus shows

Measured 2026-09-26 on Windows 11 / MSYS2 UCRT64, gcc 16.1, Debug
`-DTUR_JIT=ON`, 12 cores, `TUR_TEST_SHARD` thirds:

| path | passed | failed | skipped | wall per shard |
| --- | --- | --- | --- | --- |
| split (default) | 3090 | 0 | 65 | 318-328 s |
| whole-preamble (`TUR_JIT_NO_SPLIT=1`) | 3090 | 0 | 65 | 342-349 s |

Every shard still exits non-zero. The fallback ratchet
(`tests/jit-fallback-baseline.txt`) fails on **47 fixtures that fall back to
cc on Windows only**. The baseline was generated on Linux, and nothing had run
`run-jit.sh` on Windows since the ratchet landed (2026-09-09); the last
Windows corpus run was 2026-09-05. The fallbacks are identical on both paths,
and they predate the 2026-09-26 JIT work. The httpd family, for example,
falls back the same way with that day's `stdlib/httpd.tur` reverted.

Grouped by the first error c2mir reports:

| count | first error | fixtures |
| --- | --- | --- |
| 33 | MinGW `<sys/types.h>:63`, syntax error on typedef | every `httpd-*` fixture on the list, `r7rs-eval`, `r7rs-file-ports`, `r7rs-system-libraries` |
| 3 | `vadefs.h:35: #error VARARGS not implemented for this compiler` | `recursive-linear-borrow-branch`, `tmpfile-linear`, `tmpfile-linear-borrow` |
| 1 | MinGW `<dirent.h>:67`, syntax error | `io-stdlib-roundtrip` |
| 5 | `<tur-jit>` "syntax error on identifier (expected ';')" | `image-globals-roundtrip`, `image-hooks-tracked`, `image-reload-hook`, `image-roundtrip`, `warn-image-global-unregistered` |
| 2 | the same, in the async-file shape | `async-echo-server`, `async-file` |
| 1 | the same | `term-string` |
| 2 | `<tur-jit>` "syntax error on ) (expected '<statement>')" | `rwlock-basic`, `with-lock-scoped` |

The first three rows are the header wall the spike described: an include
that `JIT_PRELUDE_WIN` does not guard-stuff, so c2mir opens the real MinGW
header and cannot parse it. The `<tur-jit>` rows are almost certainly the
other half of the same thing: a type or macro those headers would have
supplied (`off_t`, `struct stat`, `DIR`, `pthread_rwlock_t`, ...) that the
prelude does not declare. That is unconfirmed per fixture.

## Fix directions

Pick one before turning the corpus on in CI:

1. **A Windows baseline.** `tests/jit-fallback-baseline-windows.txt`, selected
   by `run-jit.sh` when `MSYSTEM` is a Windows environment. It is honest about
   today and ratchets from there, and costs one file plus a few lines in the
   harness.
2. **Close the fallbacks.** Guard-stuff `<sys/types.h>`, `<sys/stat.h>`,
   `<dirent.h>` and whatever reaches `vadefs.h`, and declare what they supply,
   with layouts measured on the host. `jit-win-prelude-and-libc-names-drift`'s
   check will insist every new name is in `libc_names`. This is more work, and
   it moves 47 programs from cc to the engine on Windows.

Either way, run it sharded like `windows-split`: three shards of about 5.5 min
each on a 12-core box, plus the MIR fetch and build per shard.
