# Nightly fuzz search: the `type` harness found a defect (Sentry `turmeric-ci` 7769783525)

**Resolved 2026-10-23: two defects, both fixed.** The seed was in the run's
own log (`Fuzzing with seed 20261019`, run 39235287401, job "Fuzz search"),
and the two findings in its `type` group: case 164 `BUG_invalid_c` (tags
adt, class_nested, gbody, gid, scalar_float32) and case 172
`BUG_value_conversion` (bare, fn_field_eff, gid_let, scalar_float). Both
reproduced on `main` with
`python3 tests/type-fuzz-src.py --tur ./build/tur --seed 20261019 --first 164 --n 173`.

1. **Case 164 -- a cps->cps `let`'s join returned a by-value ADT raw.**
   Reduced: `(defn gb [A] [x : A] : A (let [r (app (fn [y : A] : A y) x)] r))`
   at a `defdata` value, `app` calling its fn parameter (so CPS-emitted). The
   lifted join (`LH_RESUME_CONT`) unboxes its value into a `tur_adt_W` local
   but never recorded that spelling, so `slot_store_reap`'s double-box guard
   found a stale `int64_t` record from another body and handed the aggregate
   back as the slot word -- "aggregate value used where an integer was
   expected". `emit_lifted` now records the value parameter's spelling for the
   helper's body and restores the previous one after, as it does for captures.
2. **Case 172 -- a float carrier word passed by value to a spec clone.** A
   float in a CPS-emitted function (main's handle continuation) handed to a
   generic: the translator atomized it as the int64 carrier for the base ABI,
   and the call routed to `l1g5__spec__double_double` passed that word to a
   `double` parameter, which C converts by value. `atoms_csv_call_clone` bridged
   a recorded-vs-parameter mismatch only for pointers; a float kind now crosses
   as its bits too (`emit_scalar_word_conv`). The float-conversion checker
   reports nothing on it now.

The whole seed (400 cases) has no BUG class left. Pinned by
`tests/fixtures/cps-join-generic-let-byvalue-adt` and
`cps-clone-float-arg-from-carrier`, and by two `type 20261019` lines in
`tests/fuzz-seed-corpus.txt` (cases 164 and 172).

## As filed

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
