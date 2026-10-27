# A handler case that CONSUMES an owning capture (without a straight-line drop in the enclosing fn) evicts before CPS capture applies

**Severity:** low (missed coverage, not a correctness gap -- the fallback is
sound). Surfaced while landing E1 of
[cps-backend-multishot-continuations-owning-capture-plan.md](../upcoming/cps-backend-multishot-continuations-owning-capture-plan.md)
(Track B / E3, which owns the resolution).

**Status: RESOLVED for the bare-`rc` case (this note's repro).** After the owning
auto-drop lowering landed (O1-b P1/P2,
[cps-backend-owning-autodrop-lowering-plan.md](../archive/cps-backend-owning-autodrop-lowering-plan.md)),
`f`'s scope-exit `(defer (rc/drop r))` no longer stays an un-lowered `EX_DEFER`
-- it lowers into the CPS path, so `f` is CPS-emittable and no longer evicts. The
consuming case then rides E1's clone-on-read-out: the case increments the
captured `rc` on entry (`rc_strong_increment`) and its own `(rc/drop r)` balances
that, while the base is dropped once by the P2-lowered auto-drop -- net zero,
freed once, leak-clean (verified: emitted `f_hc0_0` does incref+decref, `f_hk0`
does the single base decref). No eviction, no double-drop.

The **aggregate** variant (a captured by-value struct whose owning *field* is
dropped in the case, `(rc/drop (.f o))`) is NOT resolved this way: there is no
per-field incref-on-read-out, so it still evicts and its fallback double-drops.
That variant is now a hard error -- **TUR-E0107** -- rather than a silent
miscompile
([../archive/cps-consuming-aggregate-capture-hardfails.md](../archive/cps-consuming-aggregate-capture-hardfails.md)).
So both shapes this note's "fix directions" worried about are now closed: bare-rc
works, aggregate is loudly rejected.

## Summary

E1's mechanism (clone-on-read-out of an owning `rc` capture into a multi-shot
handler CASE env) lands and is exercised by the *borrow-style* shape (the case
READS the captured rc; the enclosing fn drops it straight-line). The plan's
literal E1 narrative -- "a handler case body that captures one rc local ...
drops it once per resume", i.e. the case CONSUMES the capture and the enclosing
fn does NOT drop it -- is **unreachable**: it evicts to the whole-function
fallback before the capture machinery runs.

## Minimal repro

```turmeric
(defeffect E [] : int)
(defn g [] : int (perform (E)))
(defn f [] : int
  (let [r (rc/of 5)]
    (handle (g)
      (E [] k)
      (let [c (rc/strong-count r)]
        (rc/drop r)              ; case CONSUMES r ...
        (resume k c)))))         ; ... and f does NOT drop r straight-line
(defn main [] : int (println (f)) 0)
```

`TUR_TRACE_EVICT=1 tur emit-c` reports `[EVICT] BODY-UNSUPPORTED f
unsupported form: EX_DEFER`, and a `tur build` fails to compile (the
direct/fallback path's `collect_handle_captures` does not descend into rc ops,
so the emitted `__effect_handler_*` references `r` undeclared).

## Root cause

`is_binding_consumed` (`src/compiler/elab_core.c:1195`) recognizes a consuming
op (`rc/drop`, `drop!`, `ref/from-rc`) only when it can reach it by traversing
the expression tree -- and it has **no case for the `handle` expression**, so a
consume that lives *inside a handler case body* is invisible to it. The
let-form rc auto-drop injection (`src/compiler/elab_forms.c:1117-1178`) then
sees `is_binding_consumed(body, r) == false`, injects
`(defer (rc/drop r))`, and that `EX_DEFER` has no CT-IR lowering case
(`src/compiler/emit_cps_ir.c` term classification) -> the whole function
evicts. So the capture-admission gate (`collect_caps_case` / `cap_owning_ok`,
which E1 relaxed) never even runs.

The only way a consuming case reaches CPS today is if the enclosing fn ALSO
drops `r` on a straight-line path -- which suppresses the auto-defer -- but that
is a user double-consume (the ownership checker does not currently reject it;
see the double-drop probe in the plan discussion). E1's clone-on-read-out keeps
even that shape memory-safe (each case invocation owns its own +1, balanced by
the case drop; the straight-line drop consumes f's original).

## Fix directions

This is the O1-b / env-capture interaction the plan scopes out. Two independent
pieces, either of which unblocks the consuming-case shape:

1. Teach `is_binding_consumed` (and/or the auto-drop injection) that a consume
   inside a handler case satisfies the let-binding's drop obligation, so no
   `EX_DEFER` is injected. This is an ownership/drop-insertion change with broad
   blast radius (it changes drop placement on the direct path too) and needs its
   own careful design -- **not** a local CPS-backend tweak.
2. Land Option B (plan phase E3): a refcounted env with a real clone/drop pair,
   which is the leak-clean landing regardless.

Until then the borrow-style owning capture (landed in E1) is the covered shape;
the consuming-case shape remains on the sound whole-function fallback.
