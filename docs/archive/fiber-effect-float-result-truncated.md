# Direct-style (fiber) effect with a `:float` result truncates to an integer

**RESOLVED (2026-07-12):** Fixed in `src/compiler/emit_effects.c` alongside the
sibling arg-truncation report. The direct/fiber `resume` value is now stored
into the int64 slot via a `union { double d; int64_t i; }` bit-reinterpret (was
`(int64_t)v`, which truncated `7.1 -> 7`), and the fiber's final result is
loaded back into a genuine `double` local via the inverse reinterpret before it
flows on (was `(double)i`, a numeric conversion of the bit pattern). The shared
`eff_slot_store` / `eff_slot_load` helpers apply the same convention at the
body-fiber result store and the `perform`/`handle` result-out sites, so every
edge round-trips. Verified: `(handle (perform (Sample)) (Sample [] k) (resume k
7.1))` now prints `7.1` (was `7`) on the default path, matching what the CPS
backend produces. Regression fixture:
`tests/fixtures/direct-fiber-effect-float-result/`.

**Severity:** medium (silent wrong value for float-typed effects on the default
/ direct-style path). Not gated -- this is the normal compile path, independent
of `--enable=cps-backend`.

## Summary

An algebraic effect whose result type is `:float` (or `:float64` / `:float32`)
returns a value that is **truncated to an integer** on the direct-style / fiber
effect path. `perform`/`resume` thread the effect value through an `int64_t`-wide
slot with a plain cast rather than a bit-reinterpret, so the double's bit pattern
is read as an integer (or the double is converted to int on the way in), losing
the fractional part.

Discovered while implementing Tier B (float) support for the CPS-IR-to-C backend
(N2). The CPS backend reinterprets float bits correctly across the slot, so it
does **not** have this bug -- which means a float-effect program currently prints
a different (correct) value under `--enable=cps-backend` than on the default
path.

## Minimal repro

```turmeric
(defeffect Sample [] : float)
(defn run [] : float
  (handle (perform (Sample))
    (Sample [] k) (resume k 7.1)))
(defn main [] : int (println (run)) 0)
```

```
$ tur run repro.tur                       # direct-style / fiber
7                                         # WRONG -- should be 7.1
$ tur run repro.tur --enable=cps-backend  # CPS backend
7.1                                       # correct
```

For comparison, plain float arithmetic (no effect) is correct on both paths:
`(println (+ 7.1 1.5))` prints `8.6`. So float printing and arithmetic are fine;
only the effect value-passing truncates.

## Root cause (suspected)

The fiber effect runtime passes the effect result and the `resume` value through
a machine-word slot. Somewhere on that path a `double` is cast to/from an integer
type with a value cast (`(int64_t)d` / `(double)i`) instead of a bit reinterpret
(`union { double d; int64_t i; }`). The value cast truncates. The exact site is
in the effect/`resume` plumbing in the fiber runtime + its emission (the effect
handler frame's argument slot and the `resume` delivery), analogous to the six DK
slot boundaries the CPS backend fixed for Tier B.

## Fix direction

Mirror the Tier B slot convention on the fiber path: store/load a float effect
argument, a `resume` value, and an effect result through a `union { double d;
int64_t i; }` reinterpret (and a `uint32_t` reinterpret for `float32`) rather
than a numeric cast. The CPS backend's `slot_store` / `slot_load`
(`src/compiler/emit_cps_ir.c`) are the reference implementation.

## Related

- N2 (Tier B floats) of the CPS-IR-to-C backend, which handles this correctly:
  `docs/upcoming/v1/cps-backend-non-scalar-values-plan.md`.
- Fixture `tests/fixtures/cps-backend-float-effect/` exercises the CPS path (which
  is correct); it asserts `8.6`, the value the direct path gets wrong.
