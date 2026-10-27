# CPS backend: a fallback intermediary between a CPS handler and CPS performer breaks the DK chain (crash)

> **RESOLVED (2026-07-12):** Fixed via **Fix direction 1 (call-path taint)** in
> `src/compiler/emit_cps_ir.c`. `ensure_S` now builds the colored call graph
> (`SEnt.edges`/`edges_all`, collected by the extended `expr_collect_effects_acc`
> raw-Expr walk) and splits each function's effect set into what it PERFORMS
> (`perf_lo/hi`) vs HANDLES (`hand_lo/hi`). A new fixpoint rule (Rule C) taints an
> effect E whenever any call-path node between a colored handler of E and a colored
> performer of E (forward-reachable from the handler AND backward-reachable to the
> performer) is a genuine fallback (`!in_s && !mono_template`), co-evicting the
> handler+performer so the whole region falls back to the fiber machine coherently.
> Regression fixture: `tests/fixtures/cps-backend-fallback-intermediary/`. The
> minimal repro below now prints `106` on the default (CPS) path.

> **Graduation status (2026-07-12):** The `cps-backend` experiment is **fully
> graduated** -- it went always-on in #657 (2026-07-11) and the
> `--enable=cps-backend` flag was removed in #658. `tur experiments` now lists
> nothing and passing `--enable=cps-backend` hard-errors (TUR-E0310); the
> CPS-IR-to-C backend (`src/compiler/emit_cps_ir.c`) is the default lowering.
> This changes how the report below must be read:
> - The "**gated** / off by default / default suite unaffected" caveat and the
>   "**graduation blocker**" framing are **stale**. This shape is now on the
>   **default** path, so re-assess it as an always-on crash, not a gated one.
> - In the repro, drop `--enable=cps-backend` (it now errors) -- plain `tur run`
>   IS the CPS path.
> - Graduation shipped with the fallback-eviction gate hardened in #657, so
>   whether this exact intermediary-fallback shape still reproduces post-#658
>   should be **re-verified** before picking it up; it may be fixed, still live,
>   or moved. (Not re-checked as part of this status annotation.)

**Severity:** high (miscompile -> abort), but **gated**: only reachable under
`--enable=cps-backend` (off by default), so the default suite is unaffected. It
is a graduation blocker -- the `cps-backend` graduation gate requires no
miscompiles with the fallback removed.

## Summary

The perform/handle co-classification guard (`ensure_S` in
`src/compiler/emit_cps_ir.c`, Rule B; see
`docs/archive/history/cps-backend-perform-handle-machine-split.md`) keeps a `perform E`
and its `handle E` on the same machine by tying together every colored function
that **syntactically performs or handles** E. It does not account for an
**intermediary** function that neither performs nor handles E but sits on the
call path *between* a CPS handler and a CPS performer.

When that intermediary falls back (fiber/direct emission) while its caller
(handler) and callee (performer) are CPS-emitted, the DK continuation chain is
severed: the handler's `dk_handler` prompt is installed in the caller's DK
chain, but the fallback intermediary is invoked as a plain C call
(`/* cps->direct */`), so the performer's `dk_perform` runs with a chain that
does not contain the handler. Result: `tur: unhandled effect (tag N)` / abort.

## Minimal repro

An intermediary `f` forced to fall back by a **capturing** non-tail cps->cps
call (`(+ a (g))` captures `a`), between CPS performer `g` and CPS handler `run`:

```turmeric
(defeffect Ask [] : int)
(defn g [] : int (+ 1 (perform (Ask))))     ; performer -> CPS-emitted (dk_perform)
(defn f [a : int] : int (+ a (g)))          ; intermediary, capturing join -> falls back
(defn run [] : int
  (handle (f 100)
    (Ask [] k) (resume k 5)))               ; handler -> CPS-emitted (dk_handler)
(defn main [] : int (println (run)) 0)
```

```
$ tur run repro.tur                          # direct-style
106
$ tur run repro.tur --enable=cps-backend     # CPS backend
tur: unhandled effect (tag 2)
Aborted
```

`emit-c ... --enable=cps-backend` shows `g__cps` and `run__cps` emitted but no
`f__cps` (f fell back). `run__cps` installs `dk_handler(2, ...)` then calls
`f(INT64_C(100)); /* cps->direct */` -- a plain C call that does not thread the
handler chain -- so `g__cps`'s `dk_perform(2, ...)` never sees it.

Any reason the intermediary falls back triggers this: a capturing heap join
(above), a non-Tier value flowing through it, a `>16`-arity signature, an
unsupported body form, etc. The **non-capturing** direct-body heap join is now
CPS-emitted (see `tests/fixtures/cps-backend-heap-join/`), so that particular
variant no longer falls back -- but the general hole remains.

## Root cause

`expr_collect_effects` (the raw-Expr effect walk backing Rule B) records the
effects a function **performs or handles**, not the effects that flow *through*
it via calls. The co-classification fixpoint therefore never places a
pure-conduit function in an effect's group, so an un-CPS-able conduit does not
taint the effect and does not co-evict the performer/handler pair. The true
invariant is stronger than "performer and handler agree": **every colored
function on a call path from a CPS handler of E to a CPS performer of E must be
CPS-emitted**, because each such frame is a link in the DK chain the
`dk_perform` walks.

## Fix directions

1. **Call-path taint.** Extend the `ensure_S` fixpoint: if a colored performer
   of E is in S and a colored handler of E is in S, then every colored function
   on a call path between them must be in S; if any such intermediary is evicted,
   taint E (co-evict performer + handler, coherently falling the whole region
   back). Needs call-graph reachability restricted to colored functions.
2. **Conservative over-approximation.** Taint E whenever *any* colored function
   that (transitively) calls a performer of E, or is (transitively) called from
   a handler of E, is not in S. Simpler than exact path reachability, more
   over-broad.
3. **Thread the handler chain through fallback calls.** Make a `/* cps->direct */`
   call from CPS code to a fallback intermediary carry/reinstall the current DK
   chain so a downstream CPS performer can still find the handler. Couples the
   two machines; likely undesirable (same objection as the runtime-bridge option
   in the perform/handle report).

Option 1 is the most precise sound step and matches the existing whole-program
fixpoint. Until then, the crash is avoidable only by not writing an effect that
routes through a fallback-forced intermediary under `--enable=cps-backend`.

## Relationship to non-tail heap joins

The non-tail cps->cps heap join landed for the **zero-capture direct-body** form
(`let x = g(args) in jbody`, jbody not capturing other locals): the join is
lifted into a DK value-transform frame chained onto the enclosing continuation,
so the intermediary stays CPS and the chain is unbroken
(`tests/fixtures/cps-backend-heap-join/`). This report is exactly the residual:
the *capturing* heap join (and any other fallback trigger on an intermediary)
still severs the chain. This finding is not a regression from the heap-join work
-- before it, *every* non-tail cps->cps call fell back, so this crash was
strictly more common; the heap-join change reduced its incidence.

## Related

- `docs/archive/history/cps-backend-perform-handle-machine-split.md` -- the
  direct performer<->handler split, resolved by the co-classification guard this
  report extends.
- `docs/reported/cps-coloring-ascription-hides-control-op.md` -- a coloring
  coverage gap that can force fallbacks.
- Parent plans: `docs/upcoming/v1/cps-ir-to-c-backend-plan.md`,
  `docs/upcoming/v1/cps-backend-non-scalar-values-plan.md`.
