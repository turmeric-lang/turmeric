# A handler case that references a captured by-value struct's owning field through an owning-value op hard-fails to compile

**Severity:** low (rare shape; hard compile error, not a miscompile). Surfaced
while assessing E3 (owning-env teardown).

**Status: RESOLVED (cheapest fix landed).** `collect_handle_captures`
(`src/compiler/emit_core.c`) now descends into the owning-value op nodes, so a
handler case that references a local only through such an op captures it into
`__env` -- turning the hard compile error into the correct direct-path fallback.
Regression fixture: `tests/fixtures/cps-backend-owning-struct-field-op-capture-direct/`.

## Resolution

The direct emitter's handler-case free-variable walk
(`collect_handle_captures`) descended into ordinary operands (calls, lets,
field reads, ...) but NOT into the eleven owning-value op nodes -- `EX_RC_OF`,
`EX_RC_CLONE`, `EX_RC_DROP`, `EX_RC_PTR`, `EX_RC_COUNT`, `EX_RC_FROM_REF`,
`EX_REF_FROM_RC`, `EX_WEAK`, `EX_WEAK_UPGRADE`, `EX_WEAK_PRED`, `EX_REF_PRED`.
Each wraps a single operand (`{ Expr *expr; ... }` leading layout), so a local
referenced only through one -- e.g. `o` in `(rc/strong-count (.r o))` or
`(rc/drop (.r o))` -- was left uncaptured and the emitted `__effect_handler_*`
named it undeclared. The fix adds those eleven kinds to the walk (they share the
common initial `Expr *expr`, so `rc_of_.expr` reads the operand for every one),
mirroring the capture completeness the walk already has for every other
expression form.

The regression fixture uses a BORROW through the op (`rc/strong-count (.r o)`,
which reads but does not consume) so it is leak-clean: `o.r` starts at strong
count 1, is read inside the case, and is dropped exactly once by `f`'s
scope-exit auto-drop. Verified compiling (was `'o' undeclared` before) and
LeakSanitizer-clean.

## The consuming variant -- now a hard error (TUR-E0107)

A handler case that genuinely CONSUMES the captured field (`(rc/drop (.r o))`)
briefly compiled after the capture fix above, but double-dropped `o.r` (the case
decrements it once, `f`'s scope-exit auto-drop decrements it again ->
use-after-free). That is now a **hard error, TUR-E0107**
(`TUR_E0107_CAPTURED_FIELD_CONSUMED_IN_HANDLER`): `elab_let` detects, via
`is_field_consumed_in_handler` (`src/compiler/elab_core.c`), that a handler case
in the body drops an owning field of a captured by-value local and rejects it
before injecting the per-field auto-drop. Error fixture:
`tests/fixtures/errors/handler-case-consumes-captured-owning-field/`.

Why an error and not a fix: a handler case runs 0..N times, so no local
drop-suppression balances the counts (under-drop when the case never runs,
over-drop when it runs more than once), and the enclosing scope owns the
aggregate. Unlike a bare-`rc` consuming capture -- which E1's incref-on-read-out
DOES balance (see
`docs/archive/cps-handler-case-consumes-owning-capture-evicts.md`) -- a struct
field has no per-field incref-on-read-out, so the only correct admission is the
Option B env teardown (not built). Rejecting is the right end-state; it also
matches what the N6.5 fallback deletion would produce wholesale.

The related STRAIGHT-LINE double-drop (`(rc/drop (.r o))` with no handler) is a
separate, now-FIXED gap (`is_field_consumed` suppresses that field's auto-drop):
`docs/archive/explicit-field-drop-plus-scope-autodrop-double-drops.md`.

```turmeric
(defstruct Own [r : rc<int> tag : int])
(defn f [] : int                               ; straight-line: now single-drop
  (let [o (make-struct Own :r (rc/of 7) :tag 9)]
    (do (rc/drop (.r o)) (.tag o))))
```

## Original repro (the hard-fail -- now a clean TUR-E0107 instead)

This consuming repro no longer emits the raw `'o' undeclared` C error; it is now
rejected up front with TUR-E0107 (see "The consuming variant" above).

```turmeric
(defstruct Own [r : rc<int> tag : int])
(defeffect E [] : int)
(defn g [] : int (let [a (perform (E))] (let [b (perform (E))] (+ a b))))
(defn f [] : int
  (let [o (make-struct Own :r (rc/of 7) :tag 9)]
    (handle (g) (E [] k) (do (rc/drop (.r o)) (resume k 0)))))   ; case consumes o.r
(defn main [] : int (println (f)) 0)
```

Before any of this, `tur build` failed:

```
error: 'o_1285' undeclared (first use in this function); did you mean 'k_1286'?
   rc_strong_decrement((RcControlBlock *)(o_1285).r);
```

The generated `__effect_handler_187` referenced the captured local `o_1285`, but
the direct-path handler-literal capture did not thread `o` into the handler's
`__env`, so `o` was undeclared in the handler function.

## Root cause (as found)

Two layers:

1. **CPS side (correct, unchanged):** `collect_caps_case`
   (`src/compiler/emit_cps_ir.c`) admits a borrow-only owning capture as a bare
   alias (E-borrow) and admits a *consuming* `rc` capture via
   incref-on-read-out (balanced by the case's drop), but a *consuming AGGREGATE*
   capture has no scalar incref glue, so it evicts (`owning_cap_borrow_only`
   false + non-`TY_RC` -> `cs->ok = false`). This is the intended conservative
   behavior.

2. **Direct/fallback side (the actual failure, now fixed):** the direct
   emitter's handler-case free-variable walk (`collect_handle_captures` in
   `emit_core.c`) did not descend into owning-value ops, so `o` was used but not
   captured into `__env` -> undeclared. Fixed by adding the eleven owning-value
   op kinds to the walk.

## Full-admission direction (NOT taken; only if a real case appears)

Admitting the consuming aggregate capture on the CPS path needs the env to own
the aggregate (clone all owning fields on `dk_copy_node`, drop them once on
region teardown) -- the Option B env teardown that
`docs/archive/cps-backend-owning-env-teardown-e3-plan.md` describes. Given the
shape is rare and the direct-path hard-fail is now resolved, this substrate is
not currently worth building.

## Not affected (verified leak-clean, CPS-emitted)

- borrow-only rc / aggregate capture (case reads a scalar / field);
- consuming rc capture (case drops the rc), single- and multi-shot;
- an owning value crossing a single-shot `handle` (auto-drop lowered by P2).
