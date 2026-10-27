# Effect performed inside an inner handle does not pass through to an outer handler

**STATUS: RESOLVED (verified 2026-07-18).** The minimal repro (`mixer` handles
`A` locally, performs `B`, expects `B` to reach `main`'s handler) now prints
`12` on BOTH the fiber path (`tur run`, no flag) and the DK path
(`--enable=cps-tramp-resume`), exit 0. Effect pass-through through an inner
handler for a different effect works. Archived from `docs/reported/`.

**Severity:** high (soundness/expressiveness -- a fundamental algebraic-effects
property is broken; both the compiled path AND the turi interpreter abort).

## Summary

A `perform` that appears **lexically inside** a `handle` for a *different*
effect should pass through that inner handler to the nearest enclosing handler
for its own effect. Instead it aborts at runtime with
`tur: unhandled effect (tag N)`. This reproduces in **both** the compiled binary
and the `tur run` interpreter, so it is not a codegen-only defect -- the
reference semantics disagree with the expected result too.

## Minimal repro

```turmeric
(defeffect A [] : int)
(defeffect B [] : int)

;; mixer handles A locally; it performs B, which must reach main's B handler.
(defn mixer [] #fx{B} : int
  (handle (+ (perform (A)) (perform (B)))
    (A [] k) (resume k 5)))

(defn main [] : int
  (println (handle (mixer) (B [] k) (resume k 7)))   ; expect 5 + 7 = 12
  0)
```

Expected: `12`. Actual: `tur: unhandled effect (tag 3)` (tag 3 = B), both via
`tur build` and `tur run`.

Even the degenerate pass-through -- an inner handler for `A` whose body performs
only `B` -- fails:

```turmeric
(defn mixer [] #fx{B} : int
  (handle (perform (B))                 ; TUR-W0033 "handler clause for 'A' unreachable"
    (A [] k) (resume k 5)))
```

This one also emits `TUR-W0033` (handler clause for `A` unreachable: the body
does not perform `A`) and then aborts `unhandled effect` at runtime.

## What works (for contrast)

`tests/fixtures/effect-nested` passes: there the outer-handled `perform` is
lexically **outside** the inner handle. The break is specific to a perform of a
not-locally-handled effect sitting **inside** an inner handler's body, which must
tunnel out through it.

## Root cause direction (not yet pinned)

Unclear which layer drops the effect. Because `tur run` (turi) fails identically,
the defect is at or below the effect-dispatch model shared by both, not in the
CPS/DK codegen alone. Two candidates to probe first:
- the handler-installation / effect-dispatch walk (does an inner handler frame
  for `A` shadow or truncate the search for `B` rather than delegating upward?);
- the `TUR-W0033` "unreachable clause" analysis may be conflated with dispatch --
  the same "body does not perform this effect" reasoning that warns may also be
  what stops `B` from tunnelling out.

## Not caused by, but adjacent to

Found while validating cps-runtime-finish Slice PF (self-contained handle
delegation). Slice PF's guards deliberately keep these functions on the native
path (a handle whose body performs a non-locally-handled effect is not
whole-body-delegated), so PF neither introduces nor masks this bug.
