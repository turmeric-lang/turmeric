# Owning-field borrow in a handler case evicts the HOF from the CPS backend

**STATUS: RESOLVED.** `expr_is_pure_borrow_of` (`src/compiler/emit_cps_ir.c`) now
peels a field-read chain to the root aggregate in its `EX_RC_COUNT` /
`EX_RC_PTR` / `EX_WEAK` arms (the same peel the `EX_GET_FIELD` arm already did),
so a borrow THROUGH an owning field read -- `(rc/strong-count (.r o))` -- is
recognized as a pure borrow of the captured aggregate `o`.
`owning_cap_borrow_only` then holds, `collect_caps_case` demotes the capture to
a leak-clean shallow alias, and the HOF CPS-emits on the DK instead of evicting
BODY-STRUCT-OR-TAINT. Verified: `f__cps` emitted, zero `eff=1` evictions, output
`10` (unchanged); default suite 2195/0 (flag-off byte-identical); flag-on build
sweep clean. Regression fixture:
`tests/fixtures/cps-tramp-resume-owning-field-borrow-handler-case/` (flag-on
CPS-emit) alongside the existing
`tests/fixtures/cps-backend-owning-struct-field-op-capture-direct/` (direct
fallback). This was the last owning-by-value-aggregate capture in the
BODY-STRUCT-OR-TAINT eviction set.

---

**Severity:** low (bounded admission gap; correctness is fine -- the fixture runs
on the direct emitter, output `10`). This is the LAST owning-capture eviction
root: a colored fn whose handler CASE borrows a captured by-value struct's OWNING
field through an owning-value op (`rc/strong-count (.r o)`) evicts
**BODY-STRUCT-OR-TAINT** under `--enable=cps-tramp-resume` instead of CPS-emitting.
Unlike the session-effects (permanent inline-C/pthread) and while-loop (missing
native transform) roots, this one is a small, provably-safe widening of an
EXISTING borrow-recognition helper.

## The fixture

`tests/fixtures/cps-backend-owning-struct-field-op-capture-direct/` (already in
the tree as the DIRECT-emitter capture-completeness regression -- see
docs/archive/cps-consuming-aggregate-capture-hardfails.md; this report is about
the ORTHOGONAL CPS-backend admission gap the same shape exposes).

```turmeric
(defstruct Own [r : rc<int> tag : int])
(defeffect E [] : int)
(defn g [] : int (perform (E)))
(defn f [] : int
  (let [o (make-struct Own :r (rc/of 7) :tag 9)]
    (let [v (handle (g) (E [] k) (resume k (rc/strong-count (.r o))))]  ; <-- borrows o.r
      (+ v (.tag o)))))
(defn main [] : int (println (f)) 0)      ; => 10   (strong-count 1 + tag 9)
```

`o` is a by-value `Own` struct with an OWNING `rc<int>` field `r`. The `E` case
captures `o` and reads `o.r`'s strong count -- a BORROW (`rc/strong-count`
reads, does not consume). The capture is leak-clean: `o.r` starts at strong
count 1, is read (not dropped) in the case, and is dropped exactly once by `f`'s
scope-exit auto-drop.

## Exact eviction reason (pinned)

`TUR_TRACE_EVICT=1 tur emit-c --enable=cps-tramp-resume ...`:

```
BODY-STRUCT-OR-TAINT   eff=1 f     (flag ON)
BODY-STRUCT-OR-TAINT   eff=1 g     (g performs E; SIG-TAINT downstream of f)
SIG-MAIN               eff=0 main
```

(flag OFF both are plain SIG-TAINT; runs correct on the fiber either way.)

The CPS IR for `f` builds fine (`--dump-cps` shows the handle + case), so this is
NOT a missing transform -- it is the CAPTURE GATE rejecting the case's `o`
capture. `f` evicts because `collect_caps_case` (src/compiler/emit_cps_ir.c
~1280) cannot admit the owning by-value aggregate `o`:

```c
if (cs->ok)
    for (int i = 0; i < cs->n; i++) {
        if (!cs->owning[i] || !cs->b[i]) continue;
        if (owning_cap_borrow_only(body, cs->b[i]->id))
            cs->owning[i] = false;              /* borrow -> shallow alias, leak-clean */
        else if (cs->ty[i] != TY_RC)
            cs->ok = false;                     /* <-- f lands here: o is a struct, not TY_RC */
    }
```

`o` is captured `owning=true` (it is an `owning_byvalue_aggregate` -- has an `rc`
field). Admission then hinges on `owning_cap_borrow_only(case_body, o.id)`
proving every reference to `o` is a pure borrow. It returns **false** here, and
`o`'s type is a struct (not `TY_RC`), so `cs->ok = false` -> collect fails -> the
handle is unemittable -> BODY-STRUCT-OR-TAINT.

## Why `owning_cap_borrow_only` returns false (the real blocker)

The case body's only reference to `o` is `(rc/strong-count (.r o))`, an
`EX_RC_COUNT` wrapping an `EX_GET_FIELD` read of the OWNING field `r`. In CPS IR
that owning-value op is a `CT_LETRAW`, so `owning_cap_borrow_only` (line ~1258)
runs:

```c
case CT_LETRAW:
    if (expr_refs_bid(t->as.letraw.e, bid)
        && !expr_is_pure_borrow_of(t->as.letraw.e, bid))   /* <-- returns false for o */
        return false;
```

`expr_refs_bid` is true (`o` appears). The verdict rides on
`expr_is_pure_borrow_of((rc/strong-count (.r o)), o)` (line ~1191):

```c
case EX_RC_COUNT: inner = e->as.rc_count_.expr; break;   /* inner = (.r o), a GET_FIELD */
case EX_RC_PTR:   inner = e->as.rc_ptr_.expr;   break;
case EX_WEAK:     inner = e->as.weak_.expr;     break;
case EX_GET_FIELD: ... /* peels a chain of field reads to the root aggregate */ break;
...
return inner && inner->kind == EX_VAR && inner->as.var.binding
    && inner->as.var.binding->id == bid;                 /* requires a BARE var */
```

The `EX_RC_COUNT`/`EX_RC_PTR`/`EX_WEAK` arms bind `inner` to their immediate
operand and then demand it be a bare `EX_VAR` -- they recognize
`(rc/strong-count o)` (direct var) but NOT `(rc/strong-count (.r o))` (the op's
operand is a `.r`-field read, an `EX_GET_FIELD`, not a var). Only the
`EX_GET_FIELD` arm peels a field-read chain to the root aggregate; the rc/weak
arms do not. So an owning op APPLIED TO a field read of the captured aggregate
falls through to `false`, and the borrow is conservatively rejected.

## Confirming probes

- **Capture the NON-owning field only** (`(resume k (.tag o))`): a plain
  `EX_GET_FIELD` of a non-owning field -> `expr_is_pure_borrow_of` peels it,
  borrow-only holds -> `f` CPS-emits (no eff=1 eviction). So the by-value struct
  capture itself is fine; the OWNING-field borrow is the sole blocker.
- **Bare rc local, direct op** (`(let [r (rc/of 7)] ... (resume k (rc/strong-count r)))`):
  operand is a bare `EX_VAR` -> recognized -> `f` CPS-emits. So the rc-count
  borrow is recognized when its operand is a var, just not when it is a field
  read.

Both isolate the gap to exactly: **an owning-value op (`rc/strong-count` /
`rc/ptr` / `weak`) whose operand is a field-read of the captured by-value
aggregate.**

## Fix direction (bounded)

Teach the `EX_RC_COUNT` / `EX_RC_PTR` / `EX_WEAK` arms of `expr_is_pure_borrow_of`
to peel a field-read chain from their operand to the root aggregate, the same
peel the `EX_GET_FIELD` arm already performs, before the final `EX_VAR` identity
check. Then `(rc/strong-count (.r o))` recognizes `o` as a pure borrow ->
`owning_cap_borrow_only` returns true -> `collect_caps_case` demotes the capture
to a shallow non-owning alias (`owning=false`) -> `f` CPS-emits.

Guardrails:

- Keep it a BORROW-ONLY widening. `rc/strong-count`, `rc/ptr`, and `weak`(-of) all
  READ, so peeling the field read stays leak-clean -- the enclosing fn still owns
  and drops the aggregate exactly once. Do NOT extend this to a CONSUMING op
  (`rc/drop (.r o)`): consuming an owning field of a captured by-value aggregate
  in a 0..N-shot handler case is already the hard error TUR-E0107
  (docs/archive/cps-consuming-aggregate-capture-hardfails.md, "The consuming
  variant"), and that end-state is correct -- no per-field incref-on-read-out
  balances a case that runs zero or many times.
- Only peel when the field read yields a scalar/handle the op consumes structurally
  (an `rc<T>` field for rc-ops, a `weak<T>` for weak-ops). The field being read is
  itself owning, but the OP borrows it; the aggregate `o` is untouched.

## Verification

- Repro: `TUR_TRACE_EVICT=1 tur emit-c --enable=cps-tramp-resume
  tests/fixtures/cps-backend-owning-struct-field-op-capture-direct/input.tur
  2>&1 >/dev/null | grep eff=1` -> `BODY-STRUCT-OR-TAINT f`, `... g`. Target: `f`
  emits `f__cps`, zero `eff=1` evictions.
- Correctness after the fix: still prints `10` (byte-identical program output;
  direct-vs-CPS equivalence). The existing fixture already asserts `10` on the
  direct path -- it should keep asserting `10` on the DK; add a
  `cps-tramp-resume-...` sibling that asserts `f__cps` is emitted.
- Leak: LeakSanitizer-clean (the borrow-demoted alias adds no incref/drop; `o.r`
  is dropped once by `f`'s auto-drop). Run the compiled binary under ASan/LSan.
- Gate: default suite (`bash tests/run.sh`, 12-min timeout) green + flag-off
  byte-identical (this widens a flag-gated admission path only). Full flag-on
  build sweep (known `-lturi`/turi false-positives only).

## Relationship to the archived DIRECT-emitter fix

docs/archive/cps-consuming-aggregate-capture-hardfails.md resolved the DIRECT
emitter's side: `collect_handle_captures` (src/compiler/emit_core.c) now descends
into the eleven owning-value op nodes, so when this shape EVICTS to the direct
emitter it captures `o` correctly (was `'o' undeclared`). That is the fallback
path and is DONE. THIS report is the complementary CPS-backend admission gap:
teaching `expr_is_pure_borrow_of` to see through the field read so the shape does
not need the fallback at all. Landing it removes the last owning-by-value-aggregate
capture from the BODY-STRUCT-OR-TAINT eviction set.

## Context

The final owning-capture root in
docs/upcoming/v2/cps-dk-sole-effect-lowering-plan.md. Related landed work: the
E-borrow demotion in `collect_caps_case` (owning capture -> shallow alias when
borrow-only) and the P6 heap-join-over-recursion STRUCT-OR-TAINT keystone; this
report just widens the borrow recognizer those rely on to cover an owning-field
read operand.
