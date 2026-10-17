# `(cont? k)` on a `call/cc` continuation fails to link

**RESOLVED 2026-10-17** -- see [Resolution](#resolution-2026-10-17). Pinned by
`cont-pred-every-flavor`.

**Severity:** low-medium. A documented predicate on a documented continuation
fails the build with a linker error rather than a diagnostic.

## Repro

```turmeric
(defn probe [] : int
  (call/cc (fn [k] (if (cont? k) 1 2))))

(defn main [] : int
  (println (probe))
  0)
```

`tur run`:

```
warning: implicit declaration of function 'tur_cont_consumed'
undefined reference to `tur_cont_consumed'
tur: cc invocation failed (status 256)
```

`tur --interpret` prints `2`, i.e. it reports the not-yet-invoked escape
continuation as dead. No doc says which answer is intended for an escape
continuation. The same predicate on a `cloneable-shift` continuation works on
the compiled path.

## Root cause

`src/compiler/emit_effects.c:emit_effects_cont_pred` emits
`!tur_cont_consumed((tur_cont *)k)` for every `TY_CONT` whose flavor is not
`CONT_EFFECT`. That covers `CONT_ESCAPE` (call/cc, escape) and probably
`CONT_SERIAL`, which is unverified. No emitted preamble defines
`tur_cont_consumed`. It belonged to the fiber-era `tur_cont` runtime in
`src/runtime/runtime.c`, which no target builds. The `CONT_EFFECT` arm was
moved to `__dk_cont_live_word` when the fiber runtime was deleted
(2026-07-19). The other flavors were left behind.

## Fix directions

- For `CONT_ESCAPE`: read the `tur_escape_cont`'s liveness. The escape runtime
  (`emit_dk_runtime.c:emit_cps_callcc_prelude`) already tracks `valid` and the
  per-thread live set that `tur_escape_resume` checks.
- For `CONT_CLONEABLE` / `CONT_SERIAL`: route to the same wrapper test
  `__dk_cont_live_word` uses (a wrapper is always live).
- Decide and document what `cont?` means for an escape continuation, and make
  `turi` agree. Add a fixture for each flavor.

## Resolution (2026-10-17)

`cont?` asks whether `k` can still be invoked, and both engines now answer
that for every flavor:

- **call/cc / escape** (`CONT_ESCAPE`): true while its prompt is on the stack,
  the same test `tur_escape_resume` makes before it longjmps. Compiled, through
  a new `tur_escape_is_live` in the DK prelude (`emit_dk_runtime.c`). The live
  set belongs to the runtime half under the preamble split, so the program
  cannot read it directly; the split artifacts were regenerated. Interpreted,
  through `escape_is_live`.
- **cloneable / serial**: always true. They are multi-shot and never consumed.
- **effect** (`CONT_EFFECT`): unchanged, `__dk_cont_live_word`.

`emit_effects_cont_pred` no longer emits the deleted `tur_cont_consumed`. The
serial flavor, which the report left unverified, failed to link the same way.
The interpreter used to answer false for all three.

Pinned by `cont-pred-every-flavor` (`1`, `3`, `5`), which passes compiled
(split and whole preamble), under `tur jit` and under `--interpret`.
