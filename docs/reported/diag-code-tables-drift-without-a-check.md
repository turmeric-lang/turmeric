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
