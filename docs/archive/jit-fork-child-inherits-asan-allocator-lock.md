# A child forked under the sanitized `tur jit` can hang on ASan's allocator lock

**RESOLVED 2026-10-04** -- the closure condition below is met; see "Confirmed
in CI".

**Severity: low (CI flake on the JIT legs; no product impact).** Only the Debug
(`-fsanitize=address`) `tur jit` is affected, because only there does the
program run on ASan's allocator: the compiled build and a Release `tur jit`
use glibc's `malloc`, which is fork-safe. The Debug + JIT configuration is
exactly what CI's `JIT engine` legs run, so there it shows up as an
intermittent failure.

Found 2026-09-29 while validating the JIT prune
([jit-suite-pays-for-the-whole-prelude](jit-suite-pays-for-the-whole-prelude.md));
it reproduces identically with `TUR_JIT_NO_PRUNE=1`, so it is not caused by
that change.

**Measured in CI, 2026-10-03.** This is the single largest contributor to
`JIT engine (ubuntu-latest)`'s redness. That leg fails on 55 of 306 commits
(18%) and 46 of those 55 runs report exactly one failing fixture; of the 18
newest, 9 are this one and the other 9 are a since-fixed fixture. So the
~1-in-3 local rate above shows up as ~18% of Linux commits, and because the
leg is `continue-on-error` none of it is visible. That raises the value of the
fixture-side skip below without changing this report's severity -- it is still
a CI flake with no product impact. Details and method in
[jit-linux-leg-failures-absorbed](jit-linux-leg-failures-absorbed.md).

## Repro

```sh
for i in 1 2 3 4 5 6; do
  ./build-jit/tur jit tests/fixtures/r7rs-threads-lifecycle/input.tur 2>/dev/null |
    grep fork-failures
done
# (fork-failures 0) most runs; (fork-failures 1) about 1 run in 3 locally
```

`build-jit` is the CI configuration: `-DCMAKE_BUILD_TYPE=Debug -DTUR_JIT=ON`
(GCC 13.3's libsanitizer here). The failing child dies of its own `alarm(5)`
(`WIFSIGNALED`, `SIGALRM`) at a random iteration of the 60 forks.

## Root cause

gdb on a hung child (the fork check in
`tests/fixtures/r7rs-threads-lifecycle/life.tur`, `life-forks`):

```
#0  __sanitizer::FutexWait
#2  __sanitizer::Mutex::Lock
#4  SizeClassAllocator64<__asan::AP64<...>>::GetFromAllocator (class_id=3)
#8  __asan::Allocator::Allocate
#10 ___interceptor_malloc (size=32)
#11 tur_region_alloc_or_malloc (n=32)   src/runtime/region.c:334
#12 ?? (JIT-compiled Scheme code: the child's first allocation)
```

The child blocks on ASan's per-size-class region mutex. The burner thread held
it, mid-`malloc`, at the moment of the fork, and a forked child inherits a held
mutex with no thread to release it. glibc's `malloc` takes its arena locks in
its own fork handlers for exactly this reason. This libsanitizer's allocator
does not, so any child of a multithreaded sanitized process that allocates can
hang.

This is the same shape as the archived
[jit-fork-child-hangs-with-threads](jit-fork-child-hangs-with-threads.md)
(the JIT's `g_gen_lock`, fixed with `pthread_atfork` in `src/jit_engine.c`).
That fix is in place and holds. This lock is ASan's, which `tur` cannot take.

## Direction 1 implemented 2026-10-03 -- CONFIRMED in CI 2026-10-04

The host now tells the program: `src/jit_engine.c`'s `JIT_PRELUDE` carries
`#define TUR_JIT_HOST_ASAN 1` when, and only when, `tur` itself was built with
ASan (`__SANITIZE_ADDRESS__`, or `__has_feature(address_sanitizer)` on clang),
and `life-forks` returns 0 without forking under it. The skip is narrower than
the pre-`g_gen_lock` one it replaces: that was on `TUR_JIT_ENGINE` and so
covered every JIT run, where this leaves the Release JIT and the compiled path
still exercising the real fork path. No product coverage is lost -- the
scenario the skip removes is a sanitizer artifact, not a property `tur` claims.

**Not verified here, and that is why this stays open.** The deadlock is a
Linux/GCC-libsanitizer phenomenon and reproducing it needs a
`-DCMAKE_BUILD_TYPE=Debug -DTUR_JIT=ON` build plus the ~1-in-3 flake; neither
is available on this host. What *was* verified: the conditional define appears
in the prelude string under `-fsanitize=address` and is absent without it (a
standalone compile of the same `#if`/`#elif`/`__has_feature` structure, both
arms), and `life-forks`'s body compiles warning-clean on both arms of the new
`#ifdef` with the skip arm returning 0 without forking.

**Closure condition.** `JIT engine (ubuntu-latest)` failed 55 of 306 commits
(18%) before this. At that rate the chance of 15 consecutive clean commits by
luck is 0.82^15, about 5%, so **~15 consecutive green Linux JIT legs** is the
evidence that the skip worked. Until then this is a fix in flight, not a fixed
bug, and the leg's `continue-on-error` should stay as it is -- flipping it on
an unconfirmed fix is how an unverified change becomes everyone's problem.

**Reproduced locally, 2026-10-03 (rjungemann/turmeric#1049).** On a Linux
`-DCMAKE_BUILD_TYPE=Debug -DTUR_JIT=ON` build, the same prelude define plus
fixture skip measured `(fork-failures 1)` in 3 of 8 runs of the Debug
`tur jit` before and 0 of 8 after, and `tests/run-jit.sh` passed the fixture
three times out of three. That is the local half of the verification above;
the CI closure condition still stands. Two notes from that work:

- **Why a skip and not a narrower change.** Pausing the burner around each
  fork, or giving the child a non-allocating body, does not help: on an ASan
  host even the child's first LAZILY GENERATED function takes the allocator
  lock, because MIR's generator runs in the host and allocates through it.
- **Coverage traded.** This check was the only one exercising the
  `g_gen_lock` `pthread_atfork` fix
  ([jit-fork-child-hangs-with-threads](jit-fork-child-hangs-with-threads.md))
  under the JIT, and every CI JIT leg that can fork is a Debug (ASan) build,
  so that fix now runs unexercised in CI. A Release `tur jit` leg, or fix
  direction 2 below, would restore it -- delete the `#ifdef` in `life-forks`
  then.

## Confirmed in CI, 2026-10-04

Over `origin/ci-metrics`, `tur_jit_fixture_tests` on `JIT engine
(ubuntu-latest)` since #1050 merged (`5d250958`): **25 of 26 runs green, the
last 17 in a row** -- past the ~15-consecutive closure condition above, against
18% red before. No run since has printed `(fork-failures 1)`.

The one red run in that window (`37149022036`, `dde72b1fd`, 2026-10-03 20:24Z)
did fail on this fixture, but with a different symptom: `exited 1 (expected
0)`, not the `stdout mismatch` a fork failure produces (the fork check prints a
count; it does not change the exit status). That is a separate finding, filed
as [jit-r7rs-threads-lifecycle-exited-1-once](../reported/jit-r7rs-threads-lifecycle-exited-1-once.md).
40 local runs of the CI configuration (Debug + `TUR_JIT`, `detect_leaks=0`,
4 at a time) were all clean.

The coverage note above still stands: the `g_gen_lock` `pthread_atfork` fix
runs unexercised on CI's sanitized JIT legs.

## Fix directions

- **In the fixture:** skip the fork check when the program runs on a sanitizer
  allocator. **(Implemented -- see above.)** That means under `TUR_JIT_ENGINE` in a `__SANITIZE_ADDRESS__` /
  `__has_feature(address_sanitizer)` host. It was skipped under
  `TUR_JIT_ENGINE` before the `g_gen_lock` fix, so this narrows that skip
  rather than restoring it. The inline-C body sees the program's macros, not
  the host's, so the host would need to pass the fact in (e.g. a
  `TUR_JIT_HOST_ASAN` define in `JIT_PRELUDE`).
- **In the host:** recent compiler-rt locks the allocator around `fork` itself
  (an `InstallAtForkHandler` hook; not verified here which LLVM release, or
  which GCC libsanitizer merge, first carries it). A toolchain whose ASan
  runtime does that closes this with no change in the repo. Check the runner's
  toolchain before relying on it.
- Not a fix: raising the child's `alarm` -- the lock is never released.
