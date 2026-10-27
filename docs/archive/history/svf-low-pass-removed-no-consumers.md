---
title: svf-low-pass removed in the signal rebuild; took a q parameter and silently discarded it
category: Reported
severity: low
description: Paper-trail entry. The pre-rebuild `tur-signal` spice exposed `svf-low-pass freq q` -- a name promising state-variable-filter resonance -- which under the hood just wrapped the 1-pole `low-pass alpha` and threw `q` away. That is the "half-stub primitive" anti-pattern the rebuild bans. Removed in the rebuild with no consumers in the spices tree. A real SVF with resonance is Tier 2 and tracked under the rebuild plan; this entry exists so the removal isn't silently lost.
---

# `svf-low-pass` removed in the rebuild; no consumers

## Summary

The pre-rebuild `tur-signal` spice's `src/signal/synth.tur` defined

```turmeric
(defn svf-low-pass [freq : float q : float]
  ;; ... promising a state-variable-filter low-pass with resonance ...
  ;; ... internally just (low-pass alpha-from-freq), q discarded.
  )
```

The name promises a state-variable filter with resonance, the
implementation provides a 1-pole IIR low-pass and silently throws
`q` away. That is exactly the "half-stub primitive" pattern the
rebuild plan bans: a function whose body doesn't do what its name
and docstring claim.

## Severity

Low. The defn no longer exists; this entry is the paper-trail entry
the rebuild plan asks for (`[[tur-signal-rebuild-plan]]` Bug reports
owed, item 3) so the removal isn't silently lost.

## Status

Resolved by the rebuild. `svf-low-pass` is gone from `src/signal/`.
No consumers existed in `../turmeric-spices/` (grepping the rest of
the spices tree finds zero references). `signal/filter` ships the
honest 1-pole shapes (`low-pass`, `high-pass`) under their true
names, with no fake `q` slot.

A genuine state-variable filter with resonance is Tier 2 under
`[[tur-signal-rebuild-plan]]` and lands behind a real consumer and
its own plan; the rebuild does not ship a placeholder for it.

## Related

- `[[tur-signal-rebuild-plan]]` -- Bug reports owed, item 3;
  Tier 2 deferred surface includes resonant filters.
