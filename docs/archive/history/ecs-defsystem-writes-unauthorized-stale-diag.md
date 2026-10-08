---
title: `tests/fixtures/errors/ecs-defsystem-writes-unauthorized` has a stale `expected.diag` (spices/ecs drift)
category: Test fixtures / spices drift
severity: Low. A `requires.spices` error-fixture whose expected diagnostic no
  longer matches what the current `../turmeric-spices/spices/ecs` emits. Only
  surfaces when the sibling `turmeric-spices` checkout is present (the fixture
  auto-skips otherwise), so it does not affect the default `bash tests/run.sh`
  count in a checkout-less environment.
status: RESOLVED 2026-06-20 (uncovered 2026-06-18 while cloning `turmeric-spices`
  to complete the Phase 2.1 HKT measurement; not caused by any compiler change
  that session, reproduced identically on then-HEAD).
---

## RESOLUTION (2026-06-20)

The `expected.diag` was never stale -- the compiler simply was not reaching the
form that produces it. The fixture's expected `TUR-E0003 unbound symbol
'Vel-write-cap'` lives in the `defsystem rogue` body. `(defmodule ...)` body
elaboration bailed out (`return NULL`) on the FIRST erroring form, and an
imported `ecs/world` module errors first (`no instance binding for associated
type 'Storage'`), so the main module's `defsystem` body was never elaborated and
the expected diagnostic never appeared.

Fixed by adding error recovery to the module-body loop
(`elab_module.c` `elab_defmodule` Pass 2 now keeps going past a NULL form,
mirroring the top-level driver in `elab_toplevel.c`) plus a NULL-field-name
guard in `elab_method_call` (`elab_typeclasses.c`) that prevents a SEGV when
recovery reaches a `.method`/field lookup on an incompletely-elaborated struct.
The fixture now emits its expected diagnostic and PASSES with
`../turmeric-spices/` present; full suite `1690 passed, 0 failed`. Full
write-up: `docs/archive/tourist-captures-reads-back-as-int-carrier-collapse.md`.

---

# Stale `expected.diag` in `ecs-defsystem-writes-unauthorized`

## Observed vs expected

`tests/fixtures/errors/ecs-defsystem-writes-unauthorized/expected.diag`:

```
TUR-E0003
unbound symbol 'Vel-write-cap'
```

Actual diagnostic from the current ecs spice
(`../turmeric-spices/spices/ecs/src/ecs/world.tur:170:24`):

```
error: no instance binding for associated type 'Storage' at this type
```

The fixture expects an `unbound symbol 'Vel-write-cap'` capability-authorization
error, but compilation now fails earlier inside the ecs `world.tur` macro
expansion (`(Storage ~(first comps))`) with a `Storage` associated-type
resolution error. Either the ecs spice's `world`/`Storage` machinery changed
since the fixture was captured, or a stdlib typeclass/associated-type change
altered the diagnostic.

## Repro

```sh
git clone --depth 1 https://github.com/turmeric-lang/turmeric-spices/ ../turmeric-spices
bash tests/run.sh 2>&1 | grep ecs-defsystem-writes-unauthorized
# => FAIL ... diagnostic mismatch
```

Reproduces on unmodified HEAD (verified by reverting all in-session changes), so
it is pre-existing spices/stdlib drift, not a regression from the
monomorphization work.

## Fix directions

- Determine whether the `Storage` associated-type error is the intended
  behavior now (then update `expected.diag` to match), or a genuine ecs/stdlib
  regression (then fix the spice). Requires reading
  `../turmeric-spices/spices/ecs/src/ecs/world.tur` and the `Storage` /
  `defsystem` capability machinery -- out of scope for the monomorphization
  plan.
- Note: the spice itself lives in `rjungemann/turmeric-spices` (a separate
  repo); only the fixture marker + `expected.diag` are in this repo.

## Related

- The fixture: `tests/fixtures/errors/ecs-defsystem-writes-unauthorized/`
  (carries `requires.spices`).
