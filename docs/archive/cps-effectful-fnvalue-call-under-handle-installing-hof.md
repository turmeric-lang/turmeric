# Effectful fn-value call inside a handle-installing HOF ("E2 pending")

**STATUS: RESOLVED (both cases DK-lower).** BOTH shapes now CPS-emit on the DK under
`--enable=cps-tramp-resume`:
- CONTINUATION-side (`cps-backend-fn-param-effectful` / `use-writer`): fixed via E2c
  (capture the `via_registry` callee as an int64 fn-ptr scalar in the lifted frame env).
- HANDLE-BODY (`handle-effectful-fn-param-same-fn` / `run-with`): fixed in commit ad31ec5.
  `ptc_walk` now classifies a call to the fn-value param in the handle BODY as a threadable
  tail call (it threads the delim's `cur_k` -- the prompt carrying this handler -- not the
  bare outer `__kont`); and the E2a registry registration now covers an address-taken NAMED
  effectful fn (`my-eff`), not only lifted lambdas, so `__tur_cps_lookup` resolves the named
  arg. `handle-effectful-fn-param-same-fn` DK-lowers to perform=0, output 5/5, ASan-clean;
  suite 2203/0.

NOTE: a DISTINCT shape -- an effectful callback passed to a HOF whose fn-value parameter is
represented as a FAT closure (`tur_poly_fn_t`), e.g. `apply-cb [f : (-> int int)]` -- still
miscompiles (pre-existing, reproduces flag-off too); tracked separately in
`docs/reported/effectful-fnvalue-param-miscompile.md`. It is orthogonal to the handle-body
threading resolved here.

**What landed (E2c, `--enable=cps-tramp-resume`, always-on capture machinery):** a non-tail
effectful fn-value call in the LIFTED continuation frame of a handle the HOF installs
(`use-writer`: `f "hi"` in the continuation after `(handle (g) ...)`, `f` performs Write
escaping to main) is now threaded. The frame CAPTURES the `via_registry` callee `f` as a
bare int64 direct-entry fn-ptr scalar (`cap_add_fn_scalar` + `cap_ctype` TY_FN -> int64 + the
`collect_caps_rec`/`has_capture_rec` CT_TAILCALL cases), so `__tur_cps_lookup((intptr_t)f)`
reads `f` from the env instead of an out-of-scope param. The `param_thread_class`
`expr_has_handle -> PT_E1` guard is removed; a FAT (is_poly_fn) `f` still evicts (the capture
bails, `needs_heap_join` catches it) until the fn_cps channel lands. Verified: default suite
2194/0 (flag-off byte-identical); `use-writer` -> `hi`/`5` on the DK. Regression fixture
`cps-tramp-resume-e2c-effectful-fnvalue-nontail`.

---

**Severity (original):** medium (blocked `handle-effectful-fn-param-same-fn` and
`cps-backend-fn-param-effectful` from the CPS/DK backend under `--enable=cps-tramp-resume`).
Correctness is fine -- both run correctly on the fiber (`5`/`5` and `hi`/`5`). These are REAL
fiber-live fixtures (an effectful callback is invoked), so they are genuine migration targets.

## The two fixtures

Both are a colored higher-order function that takes an EFFECTFUL fn-value parameter `f`,
CALLS it, and ALSO installs a `handle` in the same function. They differ in where `f`'s
effect is handled:

```turmeric
;; handle-effectful-fn-param-same-fn: f's effect is handled by the SAME fn's handle
(defn run-with [f : (fn [] #fx{E} int)] : int
  (handle (f)                         ; f called in the handle's DELIMITED body; f performs E
    (E [] k) (resume k 5)))           ; caught here

;; cps-backend-fn-param-effectful: f's effect ESCAPES to an OUTER handler (in main)
(defn use-writer [f : (fn [cstr] #fx{Write} nil)] #fx{Write} : int
  (let [n (handle (g) (E [] k) (resume k 5))]    ; handles a DIFFERENT effect (E) internally
    (do (f "hi") n)))                            ; f "hi" performs Write -> escapes to main
```

## Exact eviction reason (pinned)

`TUR_TRACE_EVICT=1 tur emit-c --enable=cps-tramp-resume ...`:

```
BODY-UNSUPPORTED  run-with    effectful fn-value call (E2 pending)
BODY-UNSUPPORTED  use-writer  effectful fn-value call (E2 pending)
```

(the lambda/named-fn arguments -- `__fn_NNNN`, `my-eff`, `g` -- are SIG-TAINT downstream:
they perform the same effect the evicted HOF taints.)

The message is emitted in `src/passes/cps_ir.c` (~2468 in `cps_tail`, ~2835 in `cps_bind`):

```c
if (g_opt_cps_tramp_resume && call_is_effectful_fnvalue(e)) {
    /* E2a: a tier-`now` thread-param call THREADS the DK via the registry. */
    if (pf && cps_ir_thread_param_has(pf) && call_args_atomic(e)) { ...thread via_registry... }
    CTerm *t = new_term(b, CT_UNSUPPORTED);
    t->as.unsupported.why = "effectful fn-value call (E2 pending)";   /* <-- here */
    return t;
}
```

So an effectful fn-value call is threaded onto the DK ONLY when `pf` is a REGISTERED
thread-param (`cps_ir_thread_param_has`). For these two, `f` is NOT a thread-param.

## Why `f` is not a thread-param (the guard)

`param_thread_class` (src/compiler/emit_cps_ir.c ~3062) tiers `f`:

```c
if (ntc > 0) {   /* a non-tail fn-value call */
    /* A non-tail fn-value call inside a HOF that ALSO installs a `handle` sits in the
     * handle's LIFTED continuation frame, which does not capture the fn-value param --
     * the threaded `__tur_cps_lookup(f)` call would reference an out-of-scope `f`. */
    if (g_opt_cps_tramp_resume && expr_has_handle(fd->body)) return PT_E1;   /* <-- guard */
    return PT_NONTAIL;
}
```

Both HOFs install a `handle`, so `f` tiers **PT_E1** (or PT_NONE when the call is buried in
the handle body where `ptc_walk` does not descend), which keeps it out of `g_thread_params`
-> the call hits the `CT_UNSUPPORTED` branch above.

## What IS landed vs what these need

- **E2a (landed):** an effectful fn-value call THROUGH a thread-param, in a HOF that does
  NOT install a handle (the call is in the HOF's direct body, where the param is in scope),
  is threaded onto the DK via the registry (`via_registry` `CT_TAILCALL`, tier PT_NOW). Tier
  PT_NONTAIL (a non-tail call, handle-free HOF) also lands via a heap-join frame. Regression
  fixtures: `cps-tramp-resume-e2a-fnvalue*`, `-nontail`.
- **These two (blocked):** the fn-value call sits in the LIFTED continuation frame of a
  `handle` the HOF installs. That frame's env does NOT carry the fn-value param `f`, so the
  threaded `__tur_cps_lookup((intptr_t)f)` would reference an out-of-scope `f` -- a C compile
  error. The guard tiers them PT_E1 to keep them (soundly) on the fiber.

## Why the fix is not bounded (the capture gate)

Threading `f` here means carrying the effectful fn-value PARAM on the handle's lifted
continuation frame's ENV (so `f` is in scope when the threaded call fires). That runs into the
capture gate (documented in docs/upcoming/v2/cps-dk-sole-effect-lowering-plan.md, the
tier-nontail "REMAINING refinement" / capture-gate finding):

- A captureless effectful fn-value param is carried as a bare `int64` direct-entry fn-ptr,
  NOT a `tur_poly_fn_t`, so `is_poly_fn` is false; `cap_add` falls to
  `cap_ty_ok(TY_FN, ...) = slot_ty(TY_FN) || slot_box_ty(...)`, and `slot_ty(TY_FN)` is false
  -> the capture FAILS. Carrying `f` on a lifted frame therefore requires WIDENING the
  capture gate to admit a bare-int64 fn-ptr scalar carrier for a captureless `TY_FN` param
  (`cap_ty_ok` / `cap_add` / `cap_ctype` env-field type = `int64_t`), and teaching
  `collect_caps_rec` / `has_capture_rec` to treat a `via_registry` tailcall's callee as a
  capture, and `emit_heap_join` to read `f` from the env (capture-aware name) instead of the
  raw `f`. That touches the capture machinery used by EVERY lifted continuation -- not a
  ~2-fixture-sized change.
- A FAT (is_poly_fn) effectful `f` additionally needs the `tur_poly_fn_t.fn_cps` channel
  (E2b-effectful, kill-probe-proven, 0 in the current corpus).

## Recommendation / fix direction

Fold into the E2b / capture-gate slice (the plan already routes it there):

1. Widen the capture gate for a captureless-effectful `TY_FN` param (bare-int64 fn-ptr
   scalar carrier): `cap_ty_ok`/`cap_add` accept it, `cap_ctype` -> `int64_t`.
2. Make `collect_caps_rec` / `has_capture_rec` carry a `via_registry` tailcall's callee as a
   capture, and `emit_heap_join`'s `via_registry` branch read the callee from the env
   (capture-aware `name_for_binding`) rather than the raw name.
3. Then remove the `expr_has_handle` PT_E1 guard in `param_thread_class` for the captureless
   case; the two fixtures should thread `f` into the lifted frame and CPS-emit.
4. For a fat effectful `f`, the `fn_cps` channel is the separate follow-on.

Distinguish the two fixtures during bring-up: `run-with` handles `f`'s effect with its OWN
handle (delimited, self-contained); `use-writer` lets `f`'s effect ESCAPE to an outer handler
(main). The threaded perform must reach the correct prompt in each -- `run-with`'s own prompt,
`use-writer`'s caller chain.

## Remaining after E2c: run-with (the handle-BODY case)

`use-writer` is landed; `run-with` is NOT, and it is a DIFFERENT mechanism from the E2c
capture fix. In `run-with`, the effectful fn-value call IS the handle's DELIMITED BODY
(`(handle (f) ...)`), not a call in the continuation. Findings from the E2c attempt:

- `ptc_walk` deliberately treats a fn-value occurrence inside an `EX_HANDLE` as a value-use
  ("runs under the HOF's own prompt"), so `f` classifies PT_NONE and is never a thread-param.
  Adding an `EX_HANDLE` case that walks the handle BODY makes `f` classify PT_NOW and register
  as a thread-param (verified: val=0, tc=1) -- but `run-with` STILL evicts `E2 pending`.
- So the blocker is DEEPER than classification: the handle-BODY lowering does not route the
  fn-value call `(f)` through the `cps_tail` `via_registry` EX_CALL branch (which threads to
  the handle's prompt). The handle's delimited body is lowered by the `EX_HANDLE` transform
  path, which emits `CT_UNSUPPORTED` for an effectful fn-value body rather than threading it
  to `__h0`. Threading `(handle (f) ...)` needs that path to emit
  `__tur_cps_lookup(f)(__h0)` (the handle prompt as the fn-value's kont), with `f` in scope
  (it is the `__cps` param -- likely no capture needed, unlike use-writer).
- The `ptc_walk` EX_HANDLE descent was REVERTED in the E2c commit (it made `f` a thread-param
  but the transform still evicted, and it broadened classification with no payoff); reinstate
  it as part of the handle-body-threading work, together with the `EX_HANDLE` transform
  change.

## Verification

- Repro: `TUR_TRACE_EVICT=1 tur emit-c --enable=cps-tramp-resume ...` -> the two
  `BODY-UNSUPPORTED effectful fn-value call (E2 pending)` lines. Target: zero `eff=1`
  evictions, `run-with`/`use-writer` emit `..._cps`.
- Correctness after the fix: `handle-effectful-fn-param-same-fn` prints `5`/`5`;
  `cps-backend-fn-param-effectful` prints `hi`/`5`. Both must be IDENTICAL to the current
  fiber output (direct-vs-CPS equivalence).
- Guardrail: the EFFECT must still reach the right handler -- `use-writer`'s Write escapes to
  main (do NOT swallow it under use-writer's own E prompt). Keep the effectful-vs-pure
  distinction (E2b's pure fat-closure fixtures must be unchanged).
- Gate: default suite (`bash tests/run.sh`, 12-min timeout) green + flag-off byte-identical
  (this is flag-gated E2 surface); full flag-on build sweep (known `-lturi`/turi false-
  positives only).

## Context

These are the two BODY-UNSUPPORTED "E2 pending" roots in
docs/upcoming/v2/cps-dk-sole-effect-lowering-plan.md, folded into the E2b / capture-gate
follow-on. Landed precedent: E2a (registry-threaded effectful fn-value, handle-free) and the
tier-nontail heap-join frame; both are the same machinery this ticket extends into the
handle-installing-HOF case.
