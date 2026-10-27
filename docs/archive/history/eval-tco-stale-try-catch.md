---
status: resolved
resolved: 2026-07-06
severity: low
discovered: 2026-07-06
discovered-by: turi-c-scoped-forms-heap-bounding Phase C1
fix: removed the stale tin test from tests/turi/eval-tco.{tur,sh}
---

# `tests/turi/eval-tco.tur` "tin" test referenced removed `try`/`catch`

Severity: low (stale test; blocked one audit harness, now removed)

## Summary

Test 17 ("tin", driver-coverage: control operator inside a `try` body) in
`tests/turi/eval-tco.tur` exercised the `try`/`catch` special form:

```turmeric
(reset (+ 1 (try (shift (fn [v : int] (tin (- n 1))) 0) (catch [e] 0))))
```

The `try`/`catch` special form no longer exists in the language (only
`try-with` remains -- `elab_core.c` interns `try-with`, and there is no
`EX_TRY_CATCH`/`DK_TRY_BODY` handling left; `EX_TRY_CATCH` survives only in a
stale comment). With `catch` demoted to an ordinary symbol, `(catch [e] 0)`
parses `[e]` as a **vec literal** (`(vec-of e)`) in expression position, so
`e` is a free variable reference:

```
error: unbound symbol 'e'
help: Did you mean 'n'?  (TUR-E0003)
```

This fires at elaboration in **both** the compiled (`tur run`) and interpret
(`--interpret`) paths, so it is not a runtime issue.

## Impact

`tests/turi/eval-tco.sh` drives the fixture through `:reload` in `tur repl`.
An elaboration error aborts the whole reload before any `(println ...)` runs,
so every probe in the file silently produced no output (the harness's
`grep -qF` checks only "passed" spuriously where the expected string was a
substring of a diagnostic, e.g. `"0"`). The audit harness was effectively
dead.

## Resolution

Removed the stale `tin` test and its `eval-tco.sh` check line (2026-07-06),
during Phase C1 of turi-c-scoped-forms-heap-bounding. The harness is green
again (19/19), which is what let the two new C1 probes (`cu-rec`,
`cu-catch-deep`) be validated.

## Fix directions (if reinstating try-body coverage)

Rewrite the probe against a still-supported control form (e.g. `try-with`,
or drop the `try` wrapper -- the shift-in-reset recursion it checked is
already covered by `nest-sh`). No language change needed.
