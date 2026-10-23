# `fuzz_manifest` died on a deadly signal (Sentry `turmeric-ci` 7786308509)

**Resolved 2026-10-23 -- a deadly signal reproduced locally and fixed; not confirmed to be the Sentry event's input.** The Sentry attachment was not available, so step 1 was replaced by fuzzing: `tests/fuzz/run-fuzzers.sh build-fuzz 600` with `TUR_FUZZ_TARGETS=fuzz_manifest` (clang 18, ASan/UBSan) hit `deadly signal` at ~1.2M executions on the 23-byte input `S#use-reader-macros"."/`. It is SIGABRT from `oom` (`src/runtime/arena.c:102`) under `reader_macros_load_file` (`src/compiler/reader.c`): `fopen` succeeds on a directory, `ftell` on it is `LONG_MAX` on glibc, and `(size_t)sz + 1` asked the arena for 2^63 bytes. Like the earlier findings it is in the reader path the manifest shares with ordinary sources, and it was not untrusted-input-only: a plain `#use-reader-macros "."` or `(load ".")` in a program aborted `tur` the same way. Fixed by refusing a non-regular file (fstat + S_ISREG) in `reader_macros_load_file`, `elab_read_file` (the `load` / module path) and `pkg_manifest_read_status` (a directory where a manifest should be is now MALFORMED, not an abort). A second 480 s run (1.2M executions) after the fix was clean. Pinned by `tests/fuzz/seeds/fuzz_manifest/use-reader-macros-directory-oom.bin` (replayed by every smoke run) and `tests/fixtures/errors/load-a-directory-is-an-error` / `use-reader-macros-directory-is-an-error`. If the Sentry event's own input surfaces later and is something else, file it fresh.

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
