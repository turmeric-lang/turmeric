# effect-subtype-capability: a PURE fn in an effectful-typed capability field evicts

**RESOLVED** (see `docs/archive/history/cps-effect-subtype-capability-pure-fn-in-effectful-field.md`).
A call THROUGH a lowered `.field` capability access (`fn_expr` is an `EX_GET_FIELD`
carrying an `adt_ctor`) is now credited with the FIELD's PRECISE effect row -- mirroring
`effect_check`'s `collect_effects_in_expr` field path -- instead of the blunt indirect-call
`callee_overflow`.  So a handled body whose only interior call is a pure capability
invocation (a `#fx{}` value stored in an effectful-typed field, which the compiler already
proves -- it raises TUR-W0033) admits: `main` emits `main__cps` with zero `eff=1` evictions.
Gated on `--enable=cps-tramp-resume`; the shipping classifier is byte-identical (suite green,
no snapshot churn).  The effectful companion still evicts (SIG-TAINT) and runs on the fiber.

> **Root-cause note (this branch).** The ticket below hypothesized the eviction came from
> the fn-value-call effect-crediting block reading the field access's DECLARED row
> (`fn_expr->type.as.fn.effect_row`).  On this branch that row is actually NULL, and the
> `CtorField.effect_row` is `ERK_UNRESOLVED` (treated as empty by both passes -- which is
> why W0033 fires).  The real force-evict was the **`callee_overflow`** the ticket flagged
> as a secondary suspect ("An indirect field-call may ALSO set `callee_overflow`/`ov` --
> both must be handled"): the indirect `.run` call set overflow in `letraw_effect_free`, so
> the handled-body `CT_LETRAW` was rejected.  The fix is Approach A applied to that overflow
> path -- credit the field's precise row, suppress the overflow for a capability-field call.

---

**Severity:** medium (blocks `effect-subtype-capability` from the CPS/DK backend under
`--enable=cps-tramp-resume`; correctness is fine -- it runs on the fiber). Represents a
class: effect *subtyping* (a pure value `#fx{}` satisfies an effectful field `#fx{Write}`).

## The fixture

```turmeric
(defeffect Write [s : cstr] : nil)
(defstruct Action :copy [run : fn #fx{Write}])   ; capability field DECLARED effectful

(defn pure-greet [msg : cstr]                     ; but the value stored is PURE
  (println msg))                                  ; (inferred row = #fx{})

(defn main [] : int
  (let [act (make-struct Action pure-greet)]
    (handle
      (do (.run act "hello from pure") 0)         ; call the fn-value through the field
      (Write [s] k) (do (println s) (resume k nil)))))
```

`pure-greet`'s row #fx{} is a subset of the field's declared #fx{Write}, so the store
type-checks. At runtime the Write handler is NEVER triggered -- the compiler even proves
this and emits **TUR-W0033: handler clause for 'Write' is unreachable: the body does not
perform 'Write'**. So the effect INFERENCE already knows the truth: the handled body is
pure.

## Why it evicts (the CPS classifier over-approximates)

`main` evicts BODY-STRUCT-OR-TAINT. Its handled body lowers to a delegated
(`CT_LETRAW`) call `(.run act "hello")` followed by a `KK_PROMPT` deliver. The delegated
call is INDIRECT (a fn-value field), so `expr_collect_effects_acc`'s `EX_CALL` arm set
`callee_overflow` -> `letraw_effect_free` returned false -> the `CT_LETRAW` fell to
`term_core_ok`, which rejects the following `KK_PROMPT` deliver -> `main` evicts.

The mismatch: the classifier over-approximated the indirect call as "reaches every colored
peer"; the effect inference (W0033) computed that the actual call performs #fx{} (pure).

## Fix approaches (for the agent)

**A. Precise per-call effect (preferred if the info exists).** Credit the capability-field
call with the field's precise effect row (the `CtorField.effect_row` that `effect_check`
reads), and suppress the indirect-call overflow for it. Surgical; mirrors the row-variable
inferred-row win. [Chosen.]

**B. Dead-handle elimination (most general).** Replace `(handle pure-body cases...)` with
`pure-body` when the body performs none of the handled effects. General but a new pass, and
UNSAFE here because the field row is `ERK_UNRESOLVED` (both an effectful and a pure field
value read as "pure", so eliminating the handle would drop a genuinely-needed handler).

**C. Value-flow (do NOT start here).** Track `act.run == pure-greet`. Out of scope.

## Guardrails / how to verify

- The EFFECTFUL case must still evict: a field `fn #fx{Write}` holding a genuinely
  Write-performing value must stay on the fiber. Confirmed: its stored fn is address-taken
  -> a fiber source -> SIG-TAINT, independent of the local crediting.
- `handle-effectful-fn-param-same-fn` and `cps-backend-fn-param-effectful` unchanged.
- `main` emits `main__cps`, zero `eff=1` evictions (`TUR_TRACE_EVICT=1 tur emit-c
  --enable=cps-tramp-resume ...`), output `hello from pure`.
- Default suite green; flag-gated so flag-off is byte-identical (no snapshot churn).
- Flag-on regression fixtures added: `cps-tramp-resume-effect-subtype-capability` (locks the
  DK path) and `-effectful` (the fiber guardrail companion).

## Context

One of the compound BODY-STRUCT-OR-TAINT roots from
docs/upcoming/v2/cps-dk-sole-effect-lowering-plan.md. The struct-field-value analogue of
`callee_effect_free`'s inferred-row fallback (Approach A for a NAMED callee).
