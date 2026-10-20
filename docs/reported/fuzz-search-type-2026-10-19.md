# Nightly fuzz search: the `type` harness found a defect (Sentry `turmeric-ci` 7769783525)

**Severity: unknown until triaged.** Filed from the Sentry issue alone; the
seed and the failing program were not in the event as given, so the defect is
not yet identified.

## Report

| Field | Value |
| --- | --- |
| Sentry issue | `7769783525`, project `turmeric-ci` |
| Title | `unknown: Fuzz search: type found a defect` |
| Date | 2026-10-19 10:21:09 UTC |
| Release (head) | `901f2142750596f2ec080b4027cb06da6d5b9bf9` (merge of PR #1293, `claude/primitive-docs-1mmix5`) |
| Tags | `kind=fuzz-search`, `target=type`, `transaction=type`, `ci=github-actions`, `level=error` |
| Trace | `d9c727b03b0e4633967e5359eb56d564` / span `d9c727b03b0e4633` |

Raised by the Sentry step of `.github/workflows/fuzz.yml` for the `type`
harness (`tests/type-fuzz-src.py`). The event's `--seed` and attached findings
carry the seed and the programs; the tags above do not.

## To triage

1. Read the seed and the attached findings from the Sentry issue.
2. Reproduce, alone (no overlapping build or suite):
   `python3 tests/type-fuzz-src.py --tur ./build/tur --n 400 --seed <seed>`.
   Run it with a `tur` whose banner does not say `fnsan: UNAVAILABLE`; a run
   with fnsan unavailable proves nothing about a type-harness finding (see the
   archived `generator-thunk-call-site-returns-void-ptr-not-carrier` for a
   finding that only showed with fnsan armed).
3. Check first whether it is a shape already known: the harness has a
   `known_bug_slug` table, and an unindexed repeat of a known shape drowns later
   runs.
4. Classify it, as the workflow's note says: a real defect gets its own report
   here, a row in the harness's `known_bug_slug`, a `KNOWN_PROBES` pin, and its
   seed appended to `tests/fuzz-seed-corpus.txt`; a `GEN_REJECT` the generator
   claims is legal is a generator bug, fixed in the generator.

## Fix directions

Unknown until step 1.
