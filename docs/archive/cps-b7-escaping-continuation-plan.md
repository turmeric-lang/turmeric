# B7 -- escaping / multishot continuation via `set!` (DK lowering plan)

**Fixture:** `tests/fixtures/effect-capture-k` (expected `0` then `10`).
**Status:** COMPLETE (commit 5098a0a). `effect-capture-k` DK-lowers to perform=0,
output 0/10, ASan-clean, suite 2203/0. The by-reference heap-cell capture +
copy-on-store described below is implemented and gated on g_opt_cps_tramp_resume;
the fold_stmt_is_risky escaping-mutable exclusion is lifted.

> **CONFIRMED DONE (2026-07-19).** Fixture still present; the
> `cps-tramp-resume` experiment has since GRADUATED (2026-07-19, always-on) and
> the fiber effect runtime has been deleted (Stage G) with this fixture riding
> the DK path. Nothing here remains open -- ready to archive.

## The program

```turmeric
(defeffect Ask [] : int)
(defn compute [] : int (let [x (perform (Ask))] (* x 2)))

(let [^mut k-store 0]                          ; a mutable in the enclosing scope
  (let [first-result (handle (compute)
                       (Ask [] k) (do (set! k-store k) 0))]   ; STORE k, return 0
    (println first-result)                      ; => 0
    (let [second-result (resume k-store 5)]     ; RESUME the stored k, AFTER the handle
      (println second-result))))                ; => 10  (5 * 2)
```

The handler does not resume inline; it stores its continuation `k` into the
enclosing `^mut k-store` and returns `0`. After the handle form has fully
returned, top-level code resumes the stored continuation with `5`. The
continuation escapes the handler's dynamic extent.

## Emit-level diagnosis (flag-on, colored-fn variant)

Move the body into a colored `run` fn (so `main` isn't the blocker) and
`emit-c --enable=cps-tramp-resume` produces structurally-close-but-BROKEN C
(`'k_hystore_1282' undeclared`). The three concrete defects, from the emitted
lifted bodies:

1. **The `^mut` is not captured into the handler-case env.** `run_hc0_0` (the
   `Ask` case) emits `k_hystore_1282 = k_1283;` -- assigning a bare name that is a
   local of `run__cps`, out of scope in the lifted case fn (its `env` arg is `0`).
   Root: the capture walkers (`collect_caps_case` / `collect_caps_rec`) treat
   `(set! k-store k)` as a value-read of the RHS only (`case EX_SET: REC(value)`
   at emit_cps_ir.c) -- a mutable *written* in a lifted body is never recorded as
   a capture at all.

2. **By-value capture cannot carry a write back.** The continuation frame
   `run_hk0` reads `k_hystore` from `__cap->f0`, a by-VALUE snapshot taken at
   handler-install time (`__ce_run_hk0->f0 = k_hystore_1282` == 0). Even if defect
   1 were fixed, a by-value case capture would write a private copy; the
   continuation would still read `0`. The mutable must be a SHARED cell captured
   BY REFERENCE into both lifted bodies.

3. **No copy-on-store; the stored chain dangles.** The case stores the raw
   `subk` (`k_1283 = subk`). `dk_perform` frees the captured chain once the
   handler returns, so a later `dk_invoke((DK*)k_store, 5)` is a use-after-free.
   The stored continuation must be cloned at the store (`dk_copy` /
   `tur_cloneable_cont_alloc`) so it outlives the handler.

(There is also a PRE-EXISTING direct/fiber gap: the colored-fn variant miscompiles
flag-OFF too with the same `k_hystore undeclared`, so escaping-continuation-in-a-fn
has never worked outside top-level `main`. The shipping fixture is top-level, where
main's inline fiber emission happens to scope the mutable correctly.)

## Fix design

A `^mut` that is (a) written inside a lifted handler-case body AND (b) read/resumed
in a sibling lifted body (the handle continuation, or a later top-level resume)
must be lowered to a **by-reference heap cell**:

1. **Detect the by-ref mutable.** Extend the capture analysis: a binding that is
   `is_mut` and is the target of an `EX_SET` inside a lifted case body (or crosses
   a handler boundary written-here / read-there) is a by-reference capture, not a
   by-value one. Record it in `CapSet` with a new `by_ref` flag.

2. **Allocate the cell once** in the enclosing `__cps` fn: `int64_t *k_store_cell
   = malloc(sizeof(int64_t)); *k_store_cell = <init>;` (leak-reaped like the other
   DK-lifetime allocations, `__dk_reap_ptr`).

3. **Capture the POINTER** (not the value) into every lifted body that touches it:
   the handler case env AND the continuation frame env both carry
   `int64_t *k_store_cell`. `emit_cont_env` / `emit_lifted` learn the by-ref env
   field spelling (`T *fN`), and reads/writes in the lifted body go through `*fN`.

4. **`set!` on a by-ref mutable** emits `*k_store_cell = <v>` (emit path for
   EX_SET / the CT lowering of the store). For a stored CONTINUATION value
   specifically, copy-on-store: `*k_store_cell = (int64_t)dk_copy((DK*)k)` so the
   chain survives `dk_perform`'s free.

5. **`resume` of a stored continuation** (`(resume k-store 5)` where `k-store` is
   the by-ref cell, not the case's own `k`): read `*k_store_cell`, `dk_invoke` the
   cloned chain. Reuse the existing `emit_resume` path but source `k` from the
   dereferenced cell.

## Substrate to reuse

- `dk_copy` / `tur_cloneable_cont_alloc` -- existing reified-continuation clone.
- The handler-case env plumbing already exists (`collect_caps_case`,
  `cenvs[ci] = emit_cont_env(...)`); this feature adds a by-REFERENCE flavor to it
  rather than inventing case captures from scratch.
- The multishot substrate notes in
  `cps-backend-multishot-continuations-owning-capture-plan.md` (Track A/B) and
  `cps-dk-multishot-user-effects-plan.md` (Phases A-C landed: `resumable_payload`,
  cloneable-cont handler wrap).

## Scope

Distinct from B4-B6: this threads a NEW capture flavor (by-reference cell) through
the delicate capture machinery (`CapSet`, `collect_caps*`, `has_capture*`,
`emit_lifted`, `emit_cont_env`) plus the `set!`/`resume` emit and the copy-on-store
lifetime rule. It is the multi-slice capstone the endgame plan flagged as
"FIXABLE, hardest." Recommend a dedicated implementation pass gated on
`g_opt_cps_tramp_resume`, verified against `effect-capture-k` (output `0`/`10`,
ASan-clean) with the by-value-capture path left byte-identical for every other
handle fixture.
