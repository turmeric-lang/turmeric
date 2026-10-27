# Effectful fn-value passed to a fn-value-param function miscompiles

**STATUS: RESOLVED (2026-07-23).** All three defects are fixed and in-tree; the
minimal repro now prints `15` and runs clean (full suite 2272/0). Defect 1 (the
poly-wrap binder ABI) landed earlier; this pass closes defects 2+3, the E2
DK-threading of an effectful callback through a fat-closure (`tur_poly_fn_t`)
fn-value parameter. See the resolution section at the bottom.

Summary: passing an EFFECTFUL callback (a fn that `perform`s) as a `(-> int int)`
argument to a function whose signature takes a fn-value, then invoking it under a
`handle`, emits C that does not compile. Severity: medium (blocks the E2
effect-through-fn-value shape from source; pre-existing, orthogonal to E7).

## Minimal repro

```turmeric
(defeffect Ask [] : int)
(defn apply-cb [f : (-> int int) x : int] : int (f x))
(defn cb [x : int] : int (+ x (perform (Ask))))
(defn run [] : int
  (handle (apply-cb cb 10)
    (Ask [] k) (resume k 5)))
(defn main [] : int (println (run)) 0)
```

`tur build` (with OR without `--enable=cps-tramp-resume` -- identical failure, so
this is NOT E7-related) emits:

```
error: incompatible types when assigning to type 'void *' from type 'tur_poly_fn_t'
error: incompatible type for argument 1 of 'apply_hycb'
error: '__t155' undeclared (first use in this function)
```

A NON-effectful fn-value param works fine:

```turmeric
(defn apply-cb [f : (-> int int) x : int] : int (f x))
(defn dbl [x : int] : int (* x 2))
(defn main [] : int (println (apply-cb dbl 21)) 0)   ;; => 42, compiles + runs
```

So the trigger is the interaction of: (a) `apply-cb` SIG-REJECT (non-scalar
signature -- a fn-value parameter), (b) the callback `cb` performing an effect,
(c) the enclosing `handle`. The fiber emission of the fn-value-parameter call in
that combination produces a `tur_poly_fn_t` vs `void*` mismatch and an undeclared
temp.

## Root cause direction

`apply-cb` evicts SIG-REJECT (non-scalar signature) and is emitted by the direct/
fiber path; the fn-value-parameter carrier bridge in that path mis-handles a
`tur_poly_fn_t` value (assigns it to a `void*`) and drops a temp declaration when
the fn-value is effectful. This is exactly the non-scalar-signature carrier-ABI
crossing that v2 plan **E1 (Stage C)** rewrites, and the effect-through-fn-value
channel **E2 (Stage E)** builds on. Fixing it belongs to those stages; recorded
here so the shape is not forgotten.

## Confirmed emit trace (2026-07-19)

Compiling the repro pins the exact shape and shows a compile-only patch is NOT
enough -- it would trade the build error for a runtime miscompile:

```c
static int64_t apply_hycb__cps(tur_poly_fn_t f, int64_t x, DK *__kont) {
    __auto_type __ps_154 = (((int64_t (*)(void*, int64_t))f.fn)(f.env, x)); /* calls f.fn -- the DIRECT thunk */
    ...
}
static int64_t run__cps(DK *__kont) {
    void * __t1;                                                       /* (A) declared void*  */
    __t1 = (tur_poly_fn_t){ NULL, (int64_t(*)(void*,int64_t))__poly_1285 };  /* (B) assign tur_poly_fn_t -> void*  */
    return apply_hycb__cps((int64_t)(intptr_t)__t1, INT64_C(10), __h0);      /* (C) pass int64 into tur_poly_fn_t param */
}
static int64_t __poly_1285(void *env, int64_t x0) { return cb(x0); }   /* (D) poly thunk calls DIRECT cb, not cb__cps */
```

Three distinct defects, only the first two of which are the compile error:

1. **Binder C type (A/B/C).** The temp holding the `tur_poly_fn_t` literal is
   declared `void *` (its `binder_ctype_full` reads `TY_FN` -> `void*`), so the
   struct literal assignment and the `(int64_t)(intptr_t)` arg cast both mismatch
   the `tur_poly_fn_t` param. Declaring the temp `tur_poly_fn_t` and passing it
   unwrapped fixes the two `cc` errors.

2. **`fn_cps` slot never populated (D).** The `tur_poly_fn_t` literal sets only
   `{ env, fn=__poly_1285 }`; the third field `fn_cps` is left zero, and no
   `__poly_1285__cps` variant is emitted (though `cb__cps` DOES exist and `cb` is
   registered via `__tur_cps_register((intptr_t)cb, cb__cps)`).

3. **Callee dispatches the direct slot.** `apply_hycb__cps` invokes `f.fn`
   directly -- never `f.fn_cps` nor the `__tur_cps_register` registry -- so the
   callback's `(perform (Ask))` runs OUTSIDE the DK trampoline installed by the
   enclosing `handle`. Even with (1) fixed, the effect would perform on the wrong
   fiber.

So this is genuinely the **E2** feature: emit a `__poly_N__cps` variant for an
effectful poly-wrap thunk (or register `__poly_N`), populate the literal's
`fn_cps`, and have a fn-value-param `__cps` callee dispatch through `fn_cps` /
the registry when a DK is active. A fix that only corrects the binder type would
compile a program that then performs the effect off-trampoline -- a silent
miscompile, strictly worse than the current build error. Not point-fixable.

## Fix directions

- Short term: audit the fiber emission of a fn-value (`tur_poly_fn_t`) argument to
  a SIG-REJECT callee -- the `void*` assignment (`incompatible types ...
  tur_poly_fn_t`) and the missing temp declaration. **But do not ship this alone:
  without defects 2+3 it converts the build error into a runtime miscompile.**
- Structural: E1 carrier-ABI `__cps` emission for non-scalar signatures lets
  `apply-cb` CPS-emit under the carrier ABI, and E2's `__fn_cps` slot threads the
  DK through the effectful callback -- both remove this direct-path crossing.

## Progress (2026-07-20): defect 1 fixed in code; defects 2+3 scoped precisely

**Defect 1 (the compile miscompile) is FIXED and in-tree** (full suite 2202/0):
a poly-wrap value emits a `tur_poly_fn_t` fat struct but its EX_POLY_WRAP node is
typed `ptr<void>`, so the CT-IR binder was `void *` and the cps->cps call
int64-cast the aggregate. `emit_cps_ir.c` now declares a poly-wrap CT_LETRAW
binder `tur_poly_fn_t` (`letraw_emits_poly_fn`) and passes a `tur_poly_fn_t`
param bare (`cps_call_param_is_poly_fn`). The repro **now compiles** (previously
two hard C errors) and aborts cleanly with `unhandled effect` -- proving the
callback still runs on the DIRECT `f.fn` path.

**Defects 2+3 (the runtime effect-threading) are the E2 CPS feature**, and the
in-tree investigation pins exactly what they need:

- The poly-fn-param call `(f x)` inside `apply-cb` is lowered as a delegated
  indirect call (CT_LETRAW), emitted by the DIRECT emitter as
  `f.fn(f.env, x)` (emit_expr.c:3633), then its result is delivered to the DK via
  `dk_run(__kont, r)`. The direct emitter has no `__kont` in scope
  (`emit_letraw` at emit_cps_ir.c:5372 delegates via `emit_value` without passing
  any CPS-continuation state on `ctx`), so it cannot thread the effect.
- To make it work, an EFFECTFUL poly-fn-param tail call must become a
  continuation-threading dispatch `return f.fn_cps(f.env, x, __kont)` -- which
  needs three coordinated pieces:
  1. **A `ctx`-carried CPS-kont channel** (or a dedicated CT-IR node) so the
     poly-fn call site knows it is in a DK context and can emit the `fn_cps`
     dispatch instead of `f.fn`, and skip the `dk_run` re-wrap.
  2. **`tur_poly_fn_t.fn_cps` populated** at the poly-wrap literal
     (emit_expr.c:7701 / :7649) with a `__poly_N__cps` variant.
  3. **A `__poly_N__cps` thunk emitted** -- the poly-wrap thunk `__poly_1285`
     (which calls `cb`) needs a cps twin calling `cb__cps(x, __kont)`; today the
     thunk is not a CPS candidate (the perm-fiber-taint gate at emit_cps_ir.c:4165
     evicts it -- the gate whose comment already says "Cleared once E2 gives
     fn-values a DK-threading (__fn_cps) entry").

So defect 1 is a clean ABI fix (landed); defects 2+3 are the coordinated E2
DK-threading feature across the CT-IR translation, the poly-wrap emitter, and the
CPS-candidate machinery.

## Resolution (2026-07-23): defects 2+3 landed -- E2 fat-closure fn_cps threading

Defect 1 (the binder-ABI compile error) was already fixed. This pass closes
defects 2+3 -- the runtime effect-threading -- by giving a fat closure
(`tur_poly_fn_t`) a populated `fn_cps` DK-threading slot and dispatching through
it. The repro (and non-tail / multi-call variants) now thread `cb`'s
`(perform (Ask))` to the enclosing `handle` and print the correct result.

**The four coordinated pieces:**

1. **`tur_poly_fn_t.fn_cps` populated at the poly-wrap literal**
   (`emit_expr.c`, EX_POLY_WRAP wrapper-binding case). When the wrapped inner fn
   is EFFECTFUL (its `FnDef.inferred_effect_row` is non-empty) and has a plain
   `int`/`int64` arg + result, the literal emits a third field
   `__poly_N__cps` and calls `ensure_poly_wrap_cps_thunk`. A pure fn-value is
   unchanged (two-field literal, `fn_cps` stays NULL) -- no fixture churn.

2. **The `__poly_N__cps` twin** (`emit_module.c` `ensure_poly_wrap_cps_thunk`).
   Emitted into the `thunk_typedefs` prelude with the fat closure's `fn_cps` ABI
   `(void*, int64_t, DK*)`; it recovers the wrapped fn's CPS entry from the
   direct->CPS registry (`__tur_cps_lookup((intptr_t)cb)` -> `cb__cps`, the same
   channel E2a uses) and tail-calls it threading `__kont`. `cb` is already
   registered by its addr-taken CPS-registration constructor.

3. **A `via_fncps` CT-IR tail call** (`cps_ir.h` + `cps_ir.c`). A call through a
   fat-closure poly-fn PARAM (`fn->is_poly_fn`, a concrete `:fn` carrier, single
   `int`/`int64` arg) becomes a `CT_TAILCALL{via_fncps}` in both TAIL position
   (`cps_tail`) and BIND position (`cps_bind`, reifying the continuation as a
   heap join). Rank-2/3 forall poly params and poly-wrapped args are excluded
   (`fncps_param_call_ok`).

4. **The callee dispatch** (`emit_cps_ir.c` CT_TAILCALL + `emit_heap_join`).
   `if (f.fn_cps) return f.fn_cps(f.env, arg, <kont>)` threads an effectful
   fn-value onto the caller's trampoline; a NULL slot (pure fn-value) falls to
   the direct `f.fn` call delivered to the continuation, exactly as the old
   delegated CT_LETRAW path did. The non-tail heap-join reifies the continuation
   as a DK frame and captures the fat closure `f` on the frame env as a
   `tur_poly_fn_t` field (`collect_caps`/`has_capture`/`jbody_has_cps_tailcall`
   extended for `via_fncps`), so a lifted-body `f.fn_cps` resolves.

Regression fixture: `tests/fixtures/cps-tramp-resume-e2-fat-fnvalue-param`
(tail + non-tail `(+ 1 (f x))` + two-call `(+ (f x) (f (+ x 1)))`, output `252`).

**Residual (out of scope, no regression):** the fn_cps channel is the single
int-arg `tur_poly_fn_t.fn_cps` ABI, so a fat-closure fn-value with a wider arg
(cstr/ptr/float/multi-arg) or a non-`int` result stays on the delegated direct
path (correct as before -- the effect escapes only if it was already escaping);
widening the slot is a future extension.
