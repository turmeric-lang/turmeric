# `fuzz_manifest` died on a deadly signal (Sentry `turmeric-ci` 7786308509)

**Severity: unknown until triaged, but it is on the untrusted-input surface**
(`surface=untrusted-input`): a crash in the manifest reader on a hostile
`build.tur` is worth treating as at least medium until the signal is known.
Filed from the Sentry issue alone; the signal, the stack and the reproducer were
not in the event as given.

## Report

| Field | Value |
| --- | --- |
| Sentry issue | `7786308509`, project `turmeric-ci` |
| Title | `deadly signal: fuzz_manifest: deadly signal` |
| Date | 2026-10-19 10:42:47 UTC |
| Release (head) | `901f2142750596f2ec080b4027cb06da6d5b9bf9` (merge of PR #1293, `claude/primitive-docs-1mmix5`) |
| Tags | `kind=fuzz-parser`, `target=fuzz_manifest`, `surface=untrusted-input`, `level=error` |
| Trace | `cf81d9b626924606bd25fa08b0f66e71` / span `cf81d9b626924606` |
| Stack | only `fuzzer::PrintStackTrace()` (libFuzzer's own handler); no frame in `tur` code |

The harness is `tests/fuzz/fuzz_manifest.c`, which exercises the manifest
reader (`pkg_manifest_read`, plain and sweet-exp readers). "Deadly signal" is
libFuzzer's message for SIGSEGV/SIGABRT/SIGBUS/SIGILL; which one is not recorded.

## To triage

1. Take the crashing input from the Sentry attachment (libFuzzer writes it as
   `crash-<sha1>`).
2. Reproduce on an ASan/UBSan build of the fuzzers:
   `cmake --build build-fuzz -j --target tur_fuzz_all`, then run
   `fuzz_manifest` on the file; `tests/fuzz/run-fuzzers.sh` is the replay
   driver. The sanitizer report gives the signal and the frames the Sentry stack
   lacks.
3. Pin the input under `tests/fuzz/seeds/fuzz_manifest/` (the existing seeds,
   such as `lang-trailing-token-symtab-leak.bin` and
   `neoteric-bracketapply-invalid-free.bin`, are the shape to follow), so every
   smoke run replays it.
4. Note that earlier `fuzz_manifest` findings were in the reader paths shared
   with the sweet-exp preprocessor (`#lang` handling, neoteric `bracket-apply`),
   so look there first. Do not assume it is the same defect: confirm against the
   archived reports before folding it in.

## Fix directions

Unknown until step 2.
