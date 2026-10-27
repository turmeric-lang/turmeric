---
status: resolved
severity: high
discovered: 2026-07-24
resolved: 2026-07-24
area: runtime (DK-lowered effects / nested handler resume)
---

# Resuming an outer effect through an inner `handle` loop never terminates

## Resolution (2026-07-24)

Fixed in `src/compiler/emit_dk_runtime.c`: the E7 entry driver's yield-branch
`dk_free(ch)` (`__dk_drive_after`) was eagerly freeing a resumed chain that a
pending meta-stack delivery still referenced under nested handlers -- a
heap-use-after-free (ASan-confirmed: freed in the yield branch, read back in
`dk_run_impl`, allocated as a `dk_perform` `sub`). The chain now gets a boundary
owner via `__dk_reap_keep(ch)` -- the same treatment `dk_invoke` already gives a
chain that may tail-resume out -- so it is freed exactly once at the outermost
entry (`__dk_reap_run`), after every delivery that references it has drained.

Verified: `effect-rec` probe passes at 1,000,000 depth under `ulimit -s 256`;
the minimal repro (a single inner `perform` under a nested handler) returns the
correct value and is leak-clean under ASan/LSan; the full `bash tests/run.sh`
suite has no non-snapshot regressions (the 140 `expected.c` snapshots were
regenerated for the changed DK-runtime preamble). `effect-rec` was removed from
the `xfail` set in `tests/stackless-signoff-probes.sh`.

Tradeoff: because a yielded chain is now reap-owned rather than eagerly freed,
a deep effect loop retains those chains until the entry boundary -- ~3x peak
heap at 1,000,000 depth (single-handle: 175 MB -> 559 MB). This is a
constant-factor memory cost on deep loops, not a complexity change; a
memory-bounded refinement (free eagerly when the yielded chain is provably not
aliased by any pending delivery) is possible follow-up work.

## Summary

On the current DK-lowered effect runtime, resuming an **outer** effect whose
captured continuation re-enters an **inner** `handle` running its own
perform/resume loop **does not terminate** -- the program loops forever (verified
hanging even at loop depth `N = 10`, not merely slow at large `N`).

This is the bug the `effect-rec` sign-off probe
(`tests/probes/stackless-signoff/effect-rec.tur`) was written to guard, but the
probe as committed had no timeout, so instead of failing it hung the entire
`stackless_signoff_probes` ctest target -- which is what stalled the Ubuntu /
macOS CI job until GitHub canceled it (~32 min). The probe is now quarantined as
an `xfail` under a hard per-probe timeout (see `tests/stackless-signoff-probes.sh`);
this report tracks the underlying runtime defect. Remove `effect-rec` from the
`xfail` set in that script once the fix lands (it will XPASS and re-arm).

## Minimal repro

The full probe drives 1,000,000 iterations, but the non-termination reproduces
with any depth. Reduced to `N = 3`:

```turmeric
(defeffect Outer [] : int)
(defeffect Tick [] : int)

(defn tick-loop [n : int acc : int] : int
  (if (= n 0) acc (tick-loop (- n 1) (+ acc (perform (Tick))))))

(defn run-ticks [n : int] : int
  (handle (tick-loop n 0)
    (Tick [] k) (resume k 1)))

(defn main [] : int
  (println
    (handle (+ (perform (Outer)) (run-ticks 3))   ; expect 3
      (Outer [] k) (resume k 0)))
  0)
```

```sh
tur build repro.tur -o /tmp/repro && /tmp/repro   # hangs; never prints
```

## What isolates the bug -- all three elements are required

Each of these variants terminates correctly; only their combination hangs:

- **Tick loop alone** (`run-ticks 3`, no outer handler) -> prints `3`. OK.
- **Outer handle wrapping the Tick loop but no `(perform (Outer))`** -> `3`. OK.
- **Outer perform/resume around a plain value** (`(+ (perform (Outer)) 42)`,
  no inner `handle`) -> `42`. OK.
- **Outer perform/resume whose continuation contains the inner `handle` loop**
  -> **hangs.**

So the trigger is specifically: *resuming an outer effect whose delimited
continuation re-enters an inner `handle` that itself performs and resumes.*

## Root cause -- direction

Effects are DK/CPS-lowered (the fiber-based effect runtime the probe's header
comment describes is stale; `cps-effects` graduated -- see
`src/runtime/experiments.c:160`). The suspect is the reify/re-install path in
`dk_perform` (`src/runtime/cps_prompt.c:258`): for a deep handler it captures
`dk_copy_range(k, H)` and appends a re-installed handler
(`src/runtime/cps_prompt.c:283-287`). When the resumed outer continuation
carries an inner handler marker, the handler search
(`src/runtime/cps_prompt.c:260-262`) and/or the deep re-install appears to keep
re-delimiting under the same marker so the inner loop never makes progress.
Needs a walk of the DK chain at the outer `resume` to confirm whether the inner
handler marker is being duplicated/re-entered rather than advanced.

## Impact

Nested handlers (an outer effect resumed across an inner effectful computation)
are a normal, expected composition. Any program of that shape hangs. Fixing this
is v1 effect-runtime work; the probe stands as the regression guard.
