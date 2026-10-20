# The diagnostic-code tables drift apart, and `tur explain` shows it

**Severity:** low. Several real codes cannot be explained, and nothing tests
that the tables agree.

## Repro

```
$ tur explain TUR-E0020
tur: no explanation for 'TUR-E0020'
$ tur explain TUR-I0379
<explain>:1:1: error [TUR-E0003]: unbound symbol 'TUR-I0379'
```

## What is out of sync

`src/compiler/diag.c` keeps the `DiagCode` enum's names in three hand-kept
tables: code to string, string to code, and the explain text. According to
the investigation:

- E0020, W0037, W0038 and W0039 are missing from `diag_code_from_string`, so
  `tur explain` cannot find them.
- E0400 and E0401 are missing from `diag_code_to_string`.
- About 30 codes have no explanation text.
- `diag_looks_like_code` rejects the `I` (info) prefix, so `tur explain` treats
  `TUR-I....` as source code to evaluate.

## Fix directions

- Add a ctest that walks the enum and requires each member to round-trip
  through both string tables and to have explain text (or to be on an
  explicit allowlist).
- Accept the `I` prefix in `diag_looks_like_code`.
- See [diagnostics-internals-guide](../guides/diagnostics-internals-guide.md)
  for the "adding a code" checklist.

## Resolution (2026-10-20)

Both fix directions, plus the table gaps the check found. Re-checked against
`main` before fixing: the `I`-prefix claim was already fixed
(`diag_looks_like_code` accepts it and `TUR-I0379` explains), and `E0020` had
a `diag_code_to_string` case by then; the rest of the report still held.

- **The check.** `tests/check-diag-code-tables.py` (ctest
  `tur_diag_code_tables`) reads the enum and the three tables from the source
  text, so it needs no build and runs in well under a second. It requires a
  `diag_code_to_string` case whose string is the one the member's name spells,
  a `diag_code_from_string` row that parses it back, and a
  `diag_explanations_[]` entry or a line in `tests/diag-explain-missing.txt`.
  Against the pre-fix tree it reported 38 problems: E0400 and E0401 missing
  from `diag_code_to_string`; E0020, W0037, W0038, W0039, E0400 and E0401
  missing from `diag_code_from_string`; 27 codes with no explanation; and the
  `default:` arm.
- **The tables.** The two `diag_code_to_string` cases and the six
  `diag_code_from_string` rows are added, and `diag_code_to_string` has no
  `default:` arm any more (`case DIAG_CODE_NONE` instead), so `-Wswitch`
  flags the next member somebody forgets.
- **The explanations.** The report counted "about 30" codes without text.
  Twenty-two now have it (E0006, E0020, E0022, E0105, E0106, E0375, E0621,
  E0622, E0702, E0703, E0705, E0711, E0712, E0713, W0037, W0038, W0039,
  W0040, W0041, W0623, W0624, W0706), each written from its emit site's
  message and checked against the compiler where it carries an example. The
  other seven are never emitted by anything in `src/` (E0700 and E0701 are
  retired with the `-Xcallcc` gate; E0370, E0376, E0400, E0401 and E0704 are
  unused), so `tests/diag-explain-missing.txt` lists them and can only shrink:
  a code that gains text, or leaves the enum, fails the check until its line
  is deleted.
- **User-visible half.** `tests/run-flags.sh` now asserts that `tur explain`
  prints TUR-E0006, E0020, W0037 and W0041 and looks up `TUR-I0379` as a
  code rather than evaluating it as source.
- [diagnostics-internals-guide](../guides/diagnostics-internals-guide.md)
  drops the table of gaps, describes the check, and points step 3 to 5 of the
  "adding a code" checklist at it.
