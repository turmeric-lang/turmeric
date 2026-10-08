# The msgpack spice still carries workarounds for four compiler defects that are now fixed

**Severity: low** -- nothing is wrong, nothing is slow, and nothing is
mistyped. This is dead scaffolding: code in `turmeric-spices/spices/msgpack`
whose only reason to exist was a compiler defect that no longer reproduces.
Left in place it teaches the next reader that the hazard is live, and it
carries duplicated error strings and hand-rolled inline C where a one-line
forward would do.

**Status: RESOLVED 2026-09-15.** Swept in
[turmeric-spices#71](https://github.com/turmeric-lang/turmeric-spices/pull/71).

Three of the four rows swept as filed. The fourth did not: restoring the
`DecodeMp` forwards turned up a SECOND, distinct defect that the fix for
`struct-instance-makes-generic-ok-val-see-a-byvalue-result` does not cover --
an instance method called from inside another instance body of the same class
is specialized with the enclosing instance's result type. That is now fixed and
archived as
[instance-method-call-inside-an-instance-body-takes-the-enclosing-result-type](instance-method-call-inside-an-instance-body-takes-the-enclosing-result-type.md),
and the row sweeps.

Filed 2026-09-14 alongside the fixes for the four reports below, which are
archived. The compiler half of each was done and pinned by a fixture; this was
the spice half, which could not be done in the same session --
`../turmeric-spices/` is a separate checkout and was not present.

## What to remove

Each archived report ends with a "what to remove when this is fixed" section
naming the exact lines. Collected here so the sweep is one pass:

| Archived report | Remove from `spices/msgpack` |
| --- | --- |
| [struct-instance-makes-generic-ok-val-see-a-byvalue-result](../archive/struct-instance-makes-generic-ok-val-see-a-byvalue-result.md) | **DONE**, but not by that fix alone -- see the status note above. `src/msgpack/encode.tur`: the four `DecodeMp` primitive instances were inline C building the Result with `tur_box_ok` / `tur_box_err` purely to stay carrier-shaped. Restored to one-line forwards to `mp-get-int` / `mp-get-str` / `mp-get-bool` / `mp-get-float`; the ~20-line comment and the duplicated error-message strings are gone |
| [generic-unwrap-specializes-by-the-enclosing-type-argument](../archive/generic-unwrap-specializes-by-the-enclosing-type-argument.md) | `src/msgpack/encode.tur`: `__mp-arr-len` and `__mp-arr-node` exist only to keep `unwrap` out of a generic body. Inline both back into their two call sites in `__mp-arr-decode` / `decode-mp-list` and delete the helpers and the ~10-line hazard comment above `__mp-arr-len` |
| [pinned-instance-dispatch-loses-an-opaque-return-type](../archive/pinned-instance-dispatch-loses-an-opaque-return-type.md) | `tests/container-round-trip.tur`: drop the outer `(:: ... Buf)` from all eight pinned `encode-mp @Cons ...` call sites, leaving the spelling the json spice's equivalent test uses |
| [int-literal-overflow-wraps-silently](../archive/int-literal-overflow-wraps-silently.md) | Nothing. `tests/decode-primitives.tur` already writes the boundary literals directly and should keep passing unchanged -- the visible confirmation is the two UBSan lines disappearing from that suite's output |

## How to verify

Note the verification below is **necessary but not sufficient**, and that is
how the fourth row read as sweepable when it was not:
`container-round-trip.tur` passes 20/20 with the forwards restored even with
the second defect present. `derive-round-trip.tur` and `decode-checked.tur` --
the two suites that exercise a DERIVED struct instance -- are what fail. Run
the whole `tur test tests` directory, not the one file.


`spices/msgpack/tests/container-round-trip.tur` covers `decode-mp-list` at
`(Cons int)`, `(Cons float)` and `(Cons cstr)`. That multi-instantiation
coverage is exactly what catches a regression in the first two rows -- a
regression re-breaks the build rather than passing quietly -- so running that
suite after the sweep is the whole check.

Build the spice against a `tur` at or after the four fixes; an older compiler
will reject the reverted code, which is the point.
