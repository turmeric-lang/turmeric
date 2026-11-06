# `tur --interpret`: a `match` on a `catch-unwind` result never matched

**Severity:** medium (engine disagreement; a correct program aborts under
`--interpret`). **Resolved 2026-11-06**, the day it was found.

Found while executing
[cps-catch-unwind-cannot-cross-an-effect-capture](../reported/cps-catch-unwind-cannot-cross-an-effect-capture.md).

## Repro

```turmeric
(defn main [] : int
  (println (match (catch-unwind (fn [] : int 5)) (Ok v) v (Err e) 0))
  0)
```

Compiled: `5`. `tur --interpret`: `tur: eval: match: no arm matched` -- for the
Ok case and the Err case alike, written in place or through a `let`, annotated
or not. `ok?` / `err?` / `ok-val` worked, which is why no fixture saw it: every
catch-unwind fixture read its result through them.

## Root cause

`turi_ok_result_box` / `turi_err_result_box` (`src/turi/eval.c`) built the
Result as the legacy `int64[3]` box `{is_ok, ok, err}`. `native_ok` /
`native_err` had moved to the ctor-named `Ok` / `Err` structs (SR2b), and the
interpreter's `match` tests a constructor pattern against a struct's name
only (`eval_match_resolve`). `result_field`, behind the accessors, reads both
shapes -- so the accessors kept working and the match never did.

## Fix

Both builders return the ctor-named struct now: `Ok` over the value, `Err`
over the `TuriPanicPayload` handle the `panic-*` accessors read. Pinned by
`tests/fixtures/catch-unwind-match-in-place` (both engines: Ok, a bare-panic
thunk, a panicking callee, an annotated let, `err?` beside a match, a cstr
payload).
