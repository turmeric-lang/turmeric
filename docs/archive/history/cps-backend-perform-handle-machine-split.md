# CPS backend can split a perform/handle pair across the two effect machines (crash)

**Status: RESOLVED** (co-classification guard in `ensure_S`,
`src/compiler/emit_cps_ir.c`). Kept for the paper trail; see "Resolution" below.

**Severity:** high (miscompile -> abort), but **gated**: only reachable under
`--enable=cps-backend` (off by default), so the default suite was unaffected. It
was a graduation blocker -- the `cps-backend` graduation gate requires no
miscompiles with the fallback removed.

## Summary

The CPS-IR-to-C backend classifies each colored function for CPS emission
**independently** (`ensure_S` in `src/compiler/emit_cps_ir.c`: per-function
`fn_sig_ok` + `term_core_ok`). Algebraic effects, however, couple two functions
at runtime: a `perform` must be caught by a `handle` **on the same machine**.

- A CPS-emitted performer lowers `perform` to `dk_perform(tag, ..., k)`, which
  searches the **DK continuation chain** for a `dk_handler`.
- A CPS-emitted handler installs that `dk_handler`.
- A fallback (fiber) performer/handler uses `tur_effect_perform` + the fiber
  `__eff_frame` handler stack instead.

Nothing forces a perform/handle pair to be classified the same way. When the
performer is admitted to the CPS set but its handler falls back (or vice versa),
the two machines do not interoperate: the `dk_perform` finds no DK handler in
the chain (the fiber handler is invisible to it), and the program aborts with
`tur: unhandled effect (tag N)`.

## Minimal repro

Handler forced onto the fallback path by a capture in its case body, performer
stays CPS-emittable:

```turmeric
(defeffect E [] : int)
(defn f [] : int (perform (E)))            ; simple -> CPS-emitted (dk_perform)
(defn run [x : int] : int
  (handle (f)
    (E [] k) (resume k x)))                ; case captures `x` -> run falls back
(defn main [] : int (println (run 5)) 0)
```

```
$ tur run repro.tur                       # direct-style
5
$ tur run repro.tur --enable=cps-backend  # CPS backend
tur: unhandled effect (tag 2)
Aborted
```

`tur emit-c ... --enable=cps-backend` shows `f__cps` emitted but no `run__cps`
(`run` fell back). `f__cps` performs via `dk_perform(2, ...)`; `run`'s fallback
installs a fiber `__eff_frame` handler, which `dk_perform` cannot see.

Any reason a handler falls back while its performer does not (or the reverse)
triggers this: a captured var in the case, `>1` handler case, a non-Tier-value
in the handler's own body, etc. The `(:: N :uint64)` reinterpret gap was one
such trigger (now fixed -- see below), but the hole is general.

## Root cause

`ensure_S` (`src/compiler/emit_cps_ir.c`) admits a function to the CPS set on a
purely **intra-function** predicate. There is no whole-program **effect
coherence** constraint tying a `perform E` site to the `handle E` that
dynamically encloses it. Because effect handling is dynamically scoped, the
enclosing handler is not syntactically visible at the perform site, so a naive
per-function fixpoint cannot see the coupling.

## Fix directions

The safe invariant: **for a given effect, every colored function that performs
or handles it must be classified the same way** (all CPS, or all fallback).
Options, roughly in increasing precision:

1. **Conservative co-classification.** Build an effect -> {colored functions
   that perform or handle it} map during `ensure_S`. In the fixpoint, if any
   member is not CPS-admissible, evict *all* members of that effect's group from
   the CPS set (they fall back together, coherently). Simple, sound, slightly
   over-broad (one un-analyzable handler taints the whole effect's fast path).
2. **Handler-rooted admission.** Only admit a performer of `E` to the CPS set
   once at least one CPS-admissible handler of `E` exists and the fixpoint has
   proven every reachable handler of `E` is CPS. Requires call-graph reachability
   from performer to handler.
3. **Runtime bridge.** Make `dk_perform` fall through to the fiber handler stack
   when no DK handler is found (and the fiber `perform` consult the DK chain).
   Removes the crash without co-classification, but couples the two runtimes and
   muddies the "one machine per colored region" model; likely undesirable.

Option 1 is the smallest sound step and fits the existing whole-program fixpoint
in `ensure_S`.

## Resolution

Implemented **Option 1, strengthened to be sound regardless of coloring
completeness** (`ensure_S`, `src/compiler/emit_cps_ir.c`):

1. **Every** colored top-level function now enters the classification table --
   candidates start in S; main / exported / fell-back ones are kept with
   `in_s=false` so their effects still participate.
2. Each function's effect set (the tags it performs or handles) is collected by
   `expr_collect_effects`, a **raw-Expr** walk -- not a CTerm walk. This is the
   crucial part: an effect op buried under a form the CPS translator does not
   lower (`match`, `for`, a closure body, ...) is invisible in the CTerm (it
   collapses to `CT_UNSUPPORTED`) yet still emits a *fiber* op, and an UNCOLORED
   performer (whose only control op is hidden from the coloring pass, so it
   never becomes a candidate) is invisible to a CTerm view entirely. The raw
   walk sees both. Effects reached only by never-CPS code (uncolored functions,
   top-level `def` initializers) are folded into a `base_taint`.
3. The `ensure_S` fixpoint gained Rule B: an effect is *tainted* if touched by
   `base_taint` or by any function not in S; every in-S function touching a
   tainted effect is evicted. Monotone (removals only), so it converges
   alongside the existing needs-heap-join rule (Rule A).

The result: a `perform` and its `handle` are never split across the DK and fiber
machines. If any function touching an effect cannot be CPS-emitted, every
function touching that effect falls back together, coherently.

Regression fixtures: `tests/fixtures/cps-backend-handler-fallback/` (a colored
handler forced to fall back by a captured param -- the performer co-evicts) and
`tests/fixtures/cps-backend-effect-under-match/` (an uncolored performer whose
`perform` hides under `match`, coupled to a colored handler only dynamically
through an intermediary -- the raw-Expr walk + base-taint catch it). Both assert
direct-vs-CPS value equality; before the guard the CPS build aborted with
"unhandled effect". Full suite: 1998 passed, 0 failed.

**Residual:** the raw-Expr walk covers every composite `Expr` kind (audited
against the `EX_*` enum); only genuine leaves and type/def declarations are
skipped. If a new effect-bearing `Expr` kind is added later, its case must be
added to `expr_collect_effects` or the split could reappear for that form.

The earlier `(:: N :uint64)` / `(:: N :int64)` reinterpret trigger was also
fixed independently: `src/passes/cps_ir.c` peels a same-size Tier A
`EX_REINTERPRET` (`is_tierA_reinterp` / `tierA_scalar_kind`), so a 64-bit
ascribed literal no longer forces a fallback in the first place.

## Related

- `docs/reported/cps-coloring-ascription-hides-control-op.md` -- the coloring
  pass not descending into ascriptions (and, as this report found, `match` and
  other forms). That is a *coverage* gap on its own; the co-classification guard
  here is what keeps it from becoming a *crash* under the CPS backend.
- Parent plans: `docs/upcoming/v1/cps-ir-to-c-backend-plan.md`,
  `docs/upcoming/v1/cps-backend-non-scalar-values-plan.md`.
