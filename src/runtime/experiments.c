/* experiments.c -- the experimental-feature-flag registry.
 *
 * Successor to the retired `-X<name>` surface.  See experiments.h and
 * docs/archive/history/experimental-flag-mechanism-plan.md.
 *
 * The EXPERIMENTS[] table below is the single source of truth: `tur
 * experiments`, `tur --help`, the docs site, and the release-cut script all
 * read it, and nothing restates the list.  To add a feature, append a row with
 * all seven fields populated and point `opt_global` at a `g_opt_<name>` bool the
 * feature's elaboration reads (keep the trailing `{ 0 }` sentinel last).
 *
 * Shape of a future entry (do not uncomment -- illustrative only):
 *
 *   static const ExperimentDescriptor EXPERIMENTS[] = {
 *     { "fancy-rows",
 *       "extensible row-typed records",
 *       "docs/upcoming/v1/fancy-rows-plan.md",
 *       "0.25.0",                  // introduced
 *       "0.28.0",                  // expires_at (soft deadline; on that version's
 *                                  //   cut, review the row and graduate, shelve, or bump)
 *       XF_LIFECYCLE_PROTOTYPE,
 *       &g_opt_fancy_rows },
 *   };
 */
#include "experiments.h"
#include "globals.h"   /* g_opt_<name> enable bits */

#include <stdio.h>
#include <stdlib.h>   /* getenv, exit, malloc, free */
#include <string.h>

/* The registry. */
static const ExperimentDescriptor EXPERIMENTS[] = {
    /* `backtrackable-state` GRADUATED 2026-08-29 -- stdlib/trail.tur is
     * autoloaded unconditionally and the row is gone (see stdlib_autoload.c).
     * It was held at prototype by one open question, plan 3.5's multi-shot
     * re-entry, which is now DECIDED: the checked error is permanent and
     * snapshotting the live trail segment is declined.  Do not re-add a gate
     * for it. */
    /* option-niche GRADUATED 2026-09-03 -- an `(Option P)` over a non-nullable
     * pointer payload is carried AS that pointer unconditionally (16 bytes to 8,
     * `(none)` as NULL), and a Vec stores such an element as that word (CE2).
     * The prototype's open question -- can non-nullness be DECLARED rather than
     * listed -- was answered 2026-08-28 (`:non-null` on a defopaque, TUR-E0303
     * at elaboration, the Some ctor and the carrier crossing at runtime); the
     * container parity that removed the urgency was broken by CE1/CE2
     * 2026-09-03.  The gate lives in sr3_option_niche (types.c);
     * TUR_OPTION_NICHE=0 restores the tagged 16-byte monomorph for bisection.
     * The name moves to GRADUATED[] below (a lingering --enable is a
     * TUR-W0063 no-op).  See docs/archive/sr3-option-niche-plan.md. */
    /* defstruct-as-defadt GRADUATED 2026-06-28 -- a `defstruct` now lowers to a
     * single-variant record `defadt` unconditionally (always-on; the former
     * per-shape gate defstruct_lowers_to_adt is gone, elab_defstruct in
     * elab_structs.c rewrites every form).  See
     * docs/archive/defstruct-as-defadt-plan.md. */
    /* B4 byvalue-recursive-carrier GRADUATED 2026-06-25 -- the recursive carrier
     * wrappers (Re/Expr, and wider products carrying an (F Self) field) now flow
     * by value through the fat-closure ABI unconditionally; the gate lives in
     * adt_is_byvalue_product_d (types.c).  See
     * docs/archive/history/b4-fat-closure-byvalue-adt-abi-plan.md. */
    /* forall-kinds GRADUATED 2026-07-06 -- explicit kind annotations on
     * forall/exists bound variables (e.g. (f :: * -> *)) are always accepted;
     * the pre-elaboration gate at elab_types.c is removed.  See
     * docs/archive/history/constrained-hkt-forall-plan.md. */
    /* forall-constraints GRADUATED 2026-07-06 -- typeclass constraint vectors
     * on forall types (e.g. (forall [a] [(Show a)] (-> a cstr))) are always
     * parsed and always enforced at each rank-2 instantiation site; the gate in
     * elab_types.c is removed.  See docs/archive/history/constrained-hkt-forall-plan.md. */
    /* hkt-hrt GRADUATED 2026-07-06 -- a rank-2 forall over a higher-kinded
     * variable (e.g. (forall [(f :: * -> *)] (-> (f int) int))) is always
     * validated at instantiation sites; the gate in elab_call.c is removed.
     * See docs/archive/history/constrained-hkt-forall-plan.md. */
    /* forall-dict-pass GRADUATED 2026-07-06 -- runtime dictionary passing for a
     * polymorphic constrained function used as a rank-2 argument (mode B) is
     * always-on.  Deficit 1 (dict-clone method return-type threading) and
     * Deficit 2 (multi-constraint + HKT-receiver dicts) both landed; a
     * constrained rank-2 forall carries one dictionary per constraint through
     * the poly carrier, and each class-method call in the dict-clone body
     * dispatches through the dict for its own class.  The one residual shape --
     * a constraint method dispatched from inside a nested lambda -- is rejected
     * with TUR-E0311 (never miscompiled) and tracked in
     * docs/archive/history/forall-dict-pass-nested-lambda-method.md.  See
     * docs/archive/history/forall-dict-pass-multi-constraint-hkt-plan.md. */
    /* hrt-curried-result GRADUATED 2026-07-06 -- a curried rank-2 poly fn whose
     * forall body result is itself a function (e.g. (forall [a] (-> a (-> a a))))
     * always instantiates the result to a concrete callable closure, so (l x)
     * yields a callable; the two gates in elab_call.c are removed.  See
     * docs/archive/history/constrained-hkt-forall-mode-b-plan.md. */
    /* vl-wide-functor GRADUATED 2026-07-04 (VBM4 of van-laarhoven-monomorphization-
     * plan) -- a van Laarhoven lens now focuses through a WIDE by-value functor
     * (a :copy struct / flat-product ADT wider than the one-int64 carrier)
     * unconditionally; TUR-E0309 is retired and the Path A carrier box/unbox
     * bridge is always-on (gated only by the wide-ness test, not a flag).  The
     * zero-overhead Path B redirect layers on top via --enable=vl-wide-mono. */
    /* vl-wide-mono GRADUATED 2026-07-05 (CM4 of van-laarhoven-consumer-mono-plan)
     * -- by-value HKT monomorphization across the van Laarhoven lens boundary
     * (Path B) is now unconditional: SIMPLE lenses (direct `fmap` tail) redirect
     * to a by-value mono body with no carrier box.  COMPOSED lenses fell back to
     * the always-on Path A carrier bridge at graduation; that residual was closed
     * the same day by CB1-CB5
     * (docs/archive/history/van-laarhoven-composed-byvalue-plan.md), so composed
     * lenses now thread `(f a)` by value too and Path A is only the CB5 backstop
     * for shapes that cannot be lowered.  The `g_opt_vl_wide_mono` bit is
     * retired. */
    /* panic-return-signal + stackless-catch-unwind GRADUATED 2026-07-07 -- the
     * compiled backend now always lowers panic propagation via the thread-local
     * `tur_panicking` return-path signal (no setjmp/longjmp on the catch-unwind
     * boundary), and a catch-crossing recursive function is always emitted as a
     * flat-stack heap-continuation trampoline.  The gates in emit_expr.c /
     * emit_fns.c / emit_module.c are removed; the feature is unconditional.  See
     * docs/archive/history/catch-unwind-graduation-plan.md. */
    /* cps-effects GRADUATED 2026-07-12 (F2 of
     * docs/archive/compiled-shallow-handlers-plan.md) -- the source-level
     * shallow effect handler (`handle-shallow`) is now unconditionally accepted;
     * the elab_handle gate and g_opt_cps_effects are removed.  handle-shallow
     * lowers to dk_handler_shallow on the CPS path and to the shallow_consumed
     * bubble-up on the fiber path, with compiled == interp on all shapes.  A
     * The name aged out of GRADUATED[] 2026-08-22; a lingering
     * --enable=cps-effects is now TUR-E0310. */
    /* cps-async GRADUATED 2026-07-19 -- the heap-continuation representation for
     * `async`/`await` is now the unconditional CPS-path lowering: a function
     * containing `await` is always CPS-colored, and each `await` lowers to a
     * `dk_shift` against the entry prompt (capturing the rest of the async body as
     * a heap continuation) instead of the fiber `tur_await_future`/swapcontext.
     * F3 closed every admissibility gap (await-as-shift, deferred pending drive,
     * bounded multi-await; docs/archive/compiled-async-heap-continuations-plan.md);
     * a recursive await evicting to the direct emitter is by-design (F4 declined;
     * docs/archive/history/compiled-stackless-recursive-await-plan.md).  The 2026-07-19
     * graduation attempt surfaced two fiber-interop gaps, both now closed so the
     * heap path is a strict superset of the fiber path:
     *   1. A pending future backed by a runnable scheduler fiber (manual spawn /
     *      TaskGroup) is now driven by __tur_await_body (it drains the scheduler
     *      run-queue before parking), so `taskgroup-async` resolves.
     *   2. An `await` nested in a handler-case body now delegates to the fiber
     *      path (build_letraw, guarded by b->in_handler_case in cps_ir.c) instead
     *      of building a CT_AWAIT the handler-case admission cannot host, so
     *      `async-effect-spawn` colors natively instead of evicting.
     * The gate did its job; the row is retired and the name moved to GRADUATED[]
     * below (a lingering --enable is a TUR-W0063 no-op).  See
     * docs/archive/history/cps-async-graduation-plan.md. */
    /* cps-tramp-resume GRADUATED 2026-07-19 -- the CPS/DK trampolined tail-resume
     * path is now the DEFAULT and SOLE lowering for effectful colored code
     * (g_opt_cps_tramp_resume defaults true).  The full corpus DK-lowers every
     * effect (zero tur_effect_perform call sites), so the row is retired and the
     * name moved to GRADUATED[] below (a lingering --enable is a TUR-W0063 no-op).
     * See docs/archive/cps-dk-sole-effect-lowering-plan.md. */
    /* owning-cloneable-capture GRADUATED 2026-07-20 -- admitting an owning value
     * (rc handle, :heap carrier handle, by-value aggregate) captured into a
     * multi-shot cloneable continuation, with the per-frame env clone/drop
     * teardown, is unconditional -- the admission predicates in elab_effects.c /
     * cps_ir.c / emit_cps_ir.c no longer test a bit at all (the
     * g_opt_owning_cloneable_capture enable bit was retired 2026-08-22; see
     * globals.h).  Every capture channel and owning shape that is leak-clean in
     * the base language is covered; the remaining rejections are bounded by the
     * base drop's shallowness, not this gate.  The name aged out of GRADUATED[]
     * 2026-08-22 (a lingering --enable is now TUR-E0310).  See
     * docs/archive/history/cps-backend-owning-env-teardown-e3-plan.md. */
    /* closure-drop-glue GRADUATED 2026-07-22 -- Model R (runtime drop-glue header
     * on every heap fat-closure env, walked on release via TUR_CLOSURE_DROP) is
     * now unconditional: the header ABI is emitted in every build and every
     * fat-handle free (scope-exit, catch-unwind, effect/shift reap, struct
     * fn-field drop, httpd/reactor teardown) routes through it.  The whole corpus
     * is crash- and leak-clean under the always-on ABI (forced-on suite verified
     * before graduation).  The name moves to GRADUATED[] below (a lingering
     * --enable is a TUR-W0063 no-op).  See docs/archive/closure-drop-glue-plan.md. */
    /* CG5/CG8 cycle-gc GRADUATED 2026-08-17 -- `(gc-auto!)` is now an ordinary
     * call form, available without `--enable`.  Read the scope narrowly, the
     * way CG8 decided it: what graduated is the CALL FORM, not a default.
     * `GC_AUTO` remains strictly opt-in -- a program that never calls
     * `(gc-auto!)` still gets the pure-RC path with no collector overhead, and
     * automatic collection is never going to be the default in this language
     * (CG8 (2), rejected permanently, before or after v1).  Ungating costs a
     * non-calling program exactly nothing, because the gate was on the call
     * form while the `rc_cb_alloc_kinded` payload zeroing is conditional on
     * GC_AUTO mode at RUN time.  Baked from 0.30.8 through the whole 0.31-0.33
     * lines; pause time measured and fixed (PT1/PT2), the alloc-path cost
     * measured (~10%, fixed overhead, AUTO-only), and the real-shape workload
     * re-measured after the `set!` refcount leak it surfaced was fixed
     * (steady-state residue ~60 blocks).  The name moves to GRADUATED[] below
     * (a lingering --enable is a TUR-W0063 no-op).  See
     * docs/archive/gc-cycle-collection-followup-plan.md. */
    /* RT0 refined GRADUATED 2026-08-01 -- static discharge of `#refine{...}`
     * predicates is now unconditional.  The runtime contract half was always
     * on; what became unconditional is the STATIC half, so the one
     * user-visible consequence is TUR-E0371 on a refinement violated on every
     * execution reaching it.  Preconditions were measured, not assumed: the
     * in-tree blast radius was one fixture (repurposed, not deleted), and cost
     * on a real ~5400-line program was 1.004x with zero TUR-E0371.  The name
     * moves to GRADUATED[] below, so a lingering --enable is a TUR-W0063
     * no-op.  (It had a twin in the `#lang` layer table, which is why
     * `#lang turmeric refined` warned rather than failing for a while; both
     * the twin and the layer axis itself are gone now.)  See
     * docs/archive/refined-graduation-plan.md. */
    /* J1-J3 jit GRADUATED 2026-08-17 -- `tur jit <file>` no longer needs
     * `--enable=jit`.  The gate existed to hold a third execution engine until
     * its parity sweep landed; J3 landed 2026-07-30 and closed every defect it
     * surfaced (three latent product bugs, all fixed), the harness denylist is
     * empty, and tests/run-jit.sh runs the whole corpus through the engine on
     * both hosts.  The remaining gate is the BUILD-TIME one and it stays:
     * TUR_JIT (ON by default since 2026-10-02, MIR vendored under
     * external/mir), and `tur jit` in a build without it still says so.  Engine
     * SELECTION is likewise unchanged and is not a default flip -- `cc` is
     * still what you get unless `--engine jit` / `TUR_ENGINE=jit` /
     * `:engine "jit"` says otherwise, and the REPL's in-process JIT loader now
     * hangs off that same selector rather than off this row.  The name moves to
     * GRADUATED[] below (a lingering --enable is a TUR-W0063 no-op).  See
     * docs/archive/jit-engine-plan.md. */
    /* sealed-opaque GRADUATED 2026-08-17 -- `(defopaque H :int :sealed)` now
     * enforces unconditionally: outside H's declaring module, `::` refuses both
     * the unwrap and the fabricate direction (TUR-E0302).  Both of the plan's
     * graduation criteria were met -- the ECS spice shipped on it with no
     * in-module pattern needing an escape, and the moduleless-top-level
     * limitation was accepted (not closed) in opaques-guide.md 2026-08-13 --
     * and the two-direction rule the gate existed to question survived that
     * adoption, so it graduates as designed rather than narrowed to
     * fabrication-only.  Unusually low-risk for a graduation: with the gate off
     * `:sealed` already parsed and imposed nothing, so becoming unconditional
     * reaches only code that deliberately wrote `:sealed`.  The name moves to
     * GRADUATED[] below (a lingering --enable is a TUR-W0063 no-op).  See
     * docs/archive/sealed-opaque-plan.md. */
    /* write-frames GRADUATED 2026-08-20 -- `#writes w` / `#writes [a b]` is now
     * CHECKED unconditionally (WF2's three verdicts, TUR-E0382 on EXCEEDED),
     * and WF3's borrow widening may consult a checked callee frame without an
     * opt-in.  The gate existed for two reasons and both are settled:
     *
     *   1. "The checking can reject a body that compiles today."  It can, but
     *      only a body that DECLARED a frame -- an annotation nobody writes by
     *      accident -- and only when the body demonstrably exceeds it.  The
     *      unverifiable case was designed as a silent downgrade (UNVERIFIED)
     *      precisely so adopting a frame never cascades into a caller's
     *      callees, so the reachable blast radius is "you wrote `#writes` and
     *      the body writes something else", which is the diagnostic's point.
     *   2. "WF4 elides a runtime check on the strength of the frame."  WF4 is
     *      RETIRED -- the check it proposed to elide does not exist and never
     *      did (`rt_inject_param_checks` already skips entry-check injection
     *      for any `#reads`-mentioning refinement).  Nothing elides on the
     *      strength of a frame, so the half of the gate that guarded an
     *      unasked-for optimization has no subject.
     *
     * What remains is a checker that reports a broken promise, which is the
     * ordinary tier.  G1's narrowing survives graduation unchanged: a frame
     * speaks about PARAMETERS, so a body that writes a mutable global is
     * downgraded to UNVERIFIED rather than stamped with a fact a consumer may
     * act on -- silently, because a global is outside the frame's vocabulary,
     * not outside the declared frame.  The name moves to GRADUATED[] below (a
     * lingering --enable is a TUR-W0063 no-op).  See
     * docs/archive/checked-write-frames-plan.md. */
    /* global-state GRADUATED 2026-08-18 (G5b of mutable-globals-plan) -- a
     * `#writes` frame may name a mutable global, an exported global is
     * read-only outside its defining module, and `^atomic` / `^thread-local`
     * are ordinary global annotations.  All unconditional; the name moves to
     * GRADUATED[] below (a lingering --enable is a TUR-W0063 no-op). */
    /* checked-reads GRADUATED 2026-08-20 (R2 of
     * docs/archive/trusted-refinement-claims-plan.md) -- positive evidence
     * that a `#reads` measure's body reads mutable state the frame omits now
     * REFUSES the congruence override unconditionally, instead of merely
     * warning.  The crossing it used to decide becomes the ordinary TUR-W0372
     * an unframed impure measure gets.
     *
     * The gate existed because this can only ever turn a currently-proving
     * program into a diagnostic.  That is still true, and it is exactly why
     * it should be the default: `#reads` is the ONE trusted claim the
     * refinement solver believes, its consumer GRANTS congruence on the
     * strength of it, and at the crossing it enables there is no runtime
     * fallback -- so a frame that omits state the body reads buys a proof it
     * has not earned.  A program that "stops proving" here was never entitled
     * to the proof; TUR-W0383 has been reporting the broken promise
     * gatelessly since 0.34, so the finding is not new, only the consequence.
     *
     * Refusal keys on "saw a read", never on "could not see".  An inline-C
     * measure -- essentially every measure that predates mutable globals --
     * yields no evidence and keeps the trusted grant, which is what kept the
     * nine strict-refine `#reads` fixtures byte-identical under the flag and
     * is what bounds the graduation's reach to frames the checker can read.
     * The name moves to GRADUATED[] below (a lingering --enable is a
     * TUR-W0063 no-op). */
    /* jit-ffi GRADUATED 2026-08-21 -- `(unsafe (call-ptr p [T1 T2 -> R]
     * args...))` and `(unsafe (callback-ptr f [sig]))` are now ordinary
     * `unsafe` forms, accepted without `--enable=jit-ffi`.  Graduated at its
     * own `expires_at` (0.38.0), which is the review the row asked for.
     *
     * The gate had exactly one stated job: hold the signature vocabulary
     * still enough that it "should be able to move without breaking early
     * adopters."  That vocabulary is now settled, and settled in the
     * direction that dissolves the question rather than merely answering it:
     * F4 (2026-08-18) encodes a layout INLINE in the sig string -- exact-width
     * member codes, nesting allowed -- so a sig is self-describing and the
     * struct registry section 4 flagged as the piece most likely to grow was
     * never built.  F5 landed callbacks the same day on F3's node.
     *
     * The 2026-08-21 follow-on batch then closed four of the five items F5
     * left open, and two of them were live wrong-answer bugs rather than the
     * cosmetic gaps the plan had described: the x86-64 verification that had
     * never been run (which found a nested-aggregate miscall present on every
     * architecture), inbound callback aggregates, extern-c aggregate slots,
     * and scalar width fidelity (a C callee returning `int` left the upper
     * half of the return register unspecified, so `neg_int(1234)` read back
     * as 4294966062).  A gate is worth keeping when the surface underneath it
     * is still moving; this one is now more measured than most of what ships
     * ungated, so it is withholding a form for a reason that no longer has a
     * subject.
     *
     * What graduation does NOT touch, because none of it was ever this row:
     *
     *   - The BUILD-TIME gate stays.  Under `--interpret` these forms route
     *     through the c2mir thunk provider, so they need `-DTUR_JIT=ON`; a
     *     default build still reports the clean non-JIT diagnostic rather than
     *     miscompiling.  The AOT path is a pure cast-and-call and needs no JIT.
     *   - `unsafe` stays required, exactly like `c-call`.  This is a raw
     *     function pointer invoked against a signature the CALLER asserts;
     *     graduating the flag does not make that checkable, and the whole
     *     surface remains behind the block that says so.
     *   - The plan's F1/F2 plumbing (runtime spice-export thunks,
     *     thunk-backed extern-c under --interpret) was never behind this flag.
     *
     * One substantive boundary used to survive graduation: under `--interpret`
     * on aarch64, an aggregate whose fields are all the same float type (an
     * AAPCS64 HFA) was REFUSED in both directions, because MIR had no HFA
     * class and would pass it in x0..x7 where a natively compiled callee reads
     * v0..v7.  That is FIXED as of 2026-08-26 -- the pinned MIR fork
     * implements the HFA rule, the refusal is gone, and no aggregate shape is
     * carved out any more.  See
     * docs/archive/mir-aarch64-fp-aggregate-abi.md.  The name moves to
     * GRADUATED[] below (a lingering --enable is a TUR-W0063 no-op).  See
     * docs/archive/jit-ffi-c2mir-plan.md. */
    /* parametric-sum-byvalue GRADUATED 2026-08-27 -- a multi-variant PARAMETRIC
     * sum monomorph ((Opt2 int), and above all (Option int) / (Result int
     * cstr)) now flows by value unconditionally; the gate lives in
     * sr2_app_sum_byvalue (types.c), reading the default-true
     * g_sr2_app_sum_byvalue with TUR_SR2_APP_SUM_BYVALUE=0 as the bisection
     * escape hatch.  The row's soak had exactly one job -- do not fix the ABI
     * before SR2b, its heaviest client, exists -- and SR2b landed in-tree and
     * across the spices before the expiry was anywhere near.  See
     * docs/archive/sr2-gate-results.md. */
    /* saffron GRADUATED 2026-09-10, at 0.46.0 -- `#lang saffron` is an ordinary
     * base dialect now, on the same footing as `#lang turmeric`.  It needs no
     * enable, prints no lifecycle warning, and a project manifest that scopes
     * `:experiments` can no longer turn it off.
     *
     * `g_opt_saffron` SURVIVED the graduation (renamed `g_opt_dynamic_any`
     * by r7rs-lang-plan R0, since the fact it records is a trait every
     * dynamic language shares), because it
     * was never only an enable bit: the emitter reads it to decide whether to
     * emit the `any` type/instance registries and the dynamic-dispatch panic
     * (emit_module.c), and gating them is what keeps a plain Turmeric program's
     * emitted C byte-for-byte unchanged.  `lang_dialect_apply` now sets it
     * directly when it reads a `#lang saffron` file -- the same moment
     * `experiment_enable` used to be what flipped it -- so the emission
     * decision is identical either side of this change.  Do NOT re-add a row
     * for it; the bit is a "this build contains a Saffron TU" fact, not a gate.
     * See docs/archive/saffron-lang-plan.md (complete; S9's two remaining
     * dynamic-dispatch limits are docs/reported/saffron-dyn-*.md). */
    /* class-superclasses GRADUATED 2026-09-25 (SC7 of
     * docs/archive/typeclass-superclasses-plan.md) -- the `defclass`
     * constraint preamble `(defclass Monoid [a] [(Semigroup a)] ...)`, the
     * entailment it licenses (a `[^Monoid A]` body may call `combine`) and the
     * instance obligation that makes the entailment sound (`Monoid [int]`
     * requires `Semigroup [int]`) are now unconditional.  Elaboration only; no
     * codegen, and no fixture snapshot moved at graduation.
     *
     * NO BISECTION HATCH, deliberately (plan SC7).  The guide's warning is
     * about a graduation that flips a REPRESENTATION default, where both paths
     * compiled and the old one silently loses its cover
     * (docs/archive/sr2-carrier-seam-rotted.md).  This is purely additive
     * syntax: before graduation the preamble did not parse at all, so there is
     * no old path to keep covered and no `TUR_CLASS_SUPERCLASSES=0` worth
     * carrying.  `g_opt_class_superclasses` is retired with the row.
     *
     * What graduation does NOT do: the stdlib's own classes stay flat.
     * Retrofitting a preamble onto an existing `Monoid` retroactively obliges
     * every existing instance -- including downstream spices -- to carry a
     * `Semigroup` instance (plan 4.1).  That audit is SC8 and is still open;
     * graduation deferred it, it did not remove it.  The name moves to
     * GRADUATED[] below (a lingering --enable is a TUR-W0063 no-op). */
    /* r7rs GRADUATED 2026-10-01, at 0.57.0 (introduced 0.52.0; prototype
     * until 2026-09-27, beta for the 0.56->0.57 soak, graduated on the
     * advisory expires_at rather than past it).  `#lang r7rs` is an ordinary
     * base dialect now, on the same footing as `#lang turmeric` and
     * `#lang saffron`: no row here, no enable, no TUR-W0061 on every compile,
     * and no way for a project manifest to refuse a directive the file itself
     * carries.  `tur dialects` reports all ten bases as `stable` and the
     * playground drops the chip, both off `LangBaseDescriptor.experiment`
     * going NULL again (lang_dialects.c).
     *
     * `g_opt_r7rs` SURVIVES and keeps its name -- the same call saffron's
     * graduation made for `g_opt_saffron`, and for the same reason: it was
     * never only an enable bit.  emit_module.c's `r7rs_gc_active` reads it to
     * pick which collector opt-out governs the program (`--no-r7rs-gc` versus
     * Saffron's `--no-saffron-gc`), and both dialects set `g_opt_dynamic_any`,
     * so that bit cannot tell them apart.  What moves is the WRITER:
     * `lang_dialect_apply` sets it off the new `LangTraits.scheme_runtime`
     * bit instead of through `experiment_enable`, at exactly the moment the
     * gate used to -- the reading of the `#lang` line -- so the emitted C is
     * unchanged either side of this change, measured on both arms of both
     * dynamic dialects (see the graduation commit).  The name moves to
     * GRADUATED[] below, so `--enable=r7rs` is a TUR-W0063 no-op and the
     * directive is the only writer there has ever been in practice.
     *
     * What graduation claims: the checklist the beta note named -- a
     * conforming program getting a wrong answer or failing to build -- is
     * empty.  The four reports it listed closed 2026-09-27, and
     * r7rs-reentrant-callcc-wrong-with-eval, the one of that kind still open
     * at beta, closed 2026-10-01 (`7c90e00b8`; see
     * docs/archive/r7rs-reentrant-callcc-wrong-with-eval.md).  R10's exit
     * criterion holds: `tur_r7rs_conformance` runs chibi-scheme's R7RS suite
     * and 1223 test invocations pass on BOTH back ends, 2 are settled as
     * differences kept on purpose (T7) and none fail.  The four `#lang r7rs`
     * reports still open are none of them a wrong answer --
     * r7rs-callcc-memory-never-freed (the interpreter retains re-entrant
     * stack images for the life of the process, by design elsewhere too),
     * r7rs-conformance-program-emits-megabytes-of-c (build cost; nothing is
     * miscompiled), r7rs-library-file-shape-and-export-rename (a decided
     * design question held as a report) and
     * r7rs-prelude-split-wrong-symbols-on-windows (fix landed; the split
     * stays off on Windows pending one run on a Windows host).  The plan is
     * archived at docs/archive/r7rs-lang-plan.md. */
    /* reflected-measures -- `^reflect` on a defn opts a pure, structurally
     * recursive, exhaustively matching function INTO the refinement logic: its
     * defining equation is admitted by bounded ground unfolding, so
     * `(> (len (Cons 1 (Nil))) 0)` proves instead of falling to TUR-W0372.
     * The gate exists because the totality check (TUR-E0384) is the first
     * checker of its kind in the language and the unfolding lives in the
     * encoder, where both historical refinement soundness bugs were.  The
     * decision the plan's RF0 required is recorded in its header: a
     * REFLECTED function must be shown total; program termination in general
     * stays out of scope.  See docs/upcoming/reflected-measures-plan.md. */
    { "reflected-measures",
      "`^reflect` admits a total measure's defining equation to the refinement solver (bounded unfolding)",
      "docs/upcoming/reflected-measures-plan.md",
      "0.57.0",                  /* introduced */
      "0.64.0",                  /* expires_at -- advisory; never blocks a release */
      XF_LIFECYCLE_PROTOTYPE,
      &g_opt_reflected_measures },
    /* loop-invariants GRADUATED 2026-10-03, in the 0.61 line (introduced
     * 0.57.0, prototype, advisory expires_at 0.58.0).  `(while c :invariant p
     * ...)` is always on: the runtime checks, the initiation/preservation
     * obligations that elide them, and the post-loop fact.  Graduated straight
     * from prototype -- the decline list a beta would have frozen was closed
     * out, and the last trust gap (a `#reads` body that writes, and a pure
     * callee returning an alias into a writer) closed with this change.  No
     * bisection hatch is kept, so the gate-off fixture retires rather than
     * inverts.  The name moves to GRADUATED[] below. */
    /* repl-jit-inline-c (aot-compiled-repl-plan C1) -- the interpreter
     * compiles an inline-C defn it cannot run, through the real emitter and
     * the in-process MIR engine, on that defn's first call; `tur repl` and
     * `tur --interpret` alike.  Needs a TUR_JIT build (the default on 64-bit
     * x86-64/arm64); without one the hook is absent and today's "inline-C not
     * supported" error stands.  A prototype: what a compiled defn may reach
     * (only itself, scalar signatures) is the part expected to move, and C2
     * (whole compiled turns) may subsume it. */
    { "repl-jit-inline-c",
      "the interpreter JIT-compiles an inline-C defn it cannot run, on its first call (needs a TUR_JIT build)",
      "docs/upcoming/aot-compiled-repl-plan.md",
      "0.59.0",                  /* introduced */
      "0.62.0",                  /* expires_at -- advisory; never blocks a release */
      XF_LIFECYCLE_PROTOTYPE,
      &g_opt_repl_jit_inline_c },
    { 0 }, /* sentinel so the array is never zero-length (C forbids that);
            * experiment_count() subtracts it off. */
};

/* Graduated experiments: names that WERE gated behind `--enable=` and are now
 * unconditionally on.  A CLI / manifest / user-config reference to one is
 * accepted as a no-op (TUR-W0063) rather than the hard TUR-E0310 an unknown
 * name gets, so a downstream build.tur or experiments.tur that opted in keeps
 * compiling across the graduation boundary.  This mirrors the retired -X<name>
 * accept-and-warn no-ops (drop-x-flags-plan).  Entries can age out one minor
 * line after graduation, once downstream configs have dropped the flag. */
static const char *const GRADUATED[] = {
    /* A graduated name is added here so a lingering
     * --enable/build.tur/experiments.tur reference to it is accepted as a no-op
     * (TUR-W0063) for one minor line, rather than the hard TUR-E0310 an unknown
     * name gets.  cps-backend graduated 2026-07-11 and its shim was retired once
     * no config referenced it.
     *
     * AGED OUT 2026-08-22, at 0.37.0 -- the five CPS/closure backend names,
     * every one of them at least seven minor lines past its graduation:
     * cps-effects (0.28 line), cps-tramp-resume (0.29.0), cps-async (0.29.1),
     * owning-cloneable-capture (0.29 line), closure-drop-glue (0.30.2).
     *
     * These five aged out together and ahead of the user-facing names that are
     * equally eligible by the calendar, because the population that could have
     * named them is different in kind.  Each gated a BACKEND LOWERING STRATEGY
     * -- which continuation representation the emitter picks, whether an owning
     * capture is admitted, whether a drop-glue header rides on a closure env --
     * with no source syntax to adopt and no behaviour to opt into.  There was
     * never a reason to write one in a build.tur, and a corpus-wide grep at
     * removal found no `flags` file, manifest, or experiments.tur naming any of
     * them (only prose in comments).  A name nobody had reason to enable needs
     * no migration window.
     *
     * AGED OUT 2026-08-22, at 0.38.0 -- the seven user-facing names, in a
     * second round taken as a deliberate decision rather than a cleanup:
     * refined (0.33.0), cycle-gc / jit / sealed-opaque (0.34.0), global-state
     * (0.35.0), write-frames / checked-reads (0.37.0).
     *
     * These are the opposite case from the backend five above.  Each gated
     * SOURCE SYNTAX someone had to write into a file and then enable to use, so
     * a lingering enable is exactly what a real adopter's config looks like and
     * retiring it turns their build red.  That is the point of the window, not
     * an argument against closing it: the shim is a migration window, not a
     * permanent alias, and every one of these is at least a full minor line
     * past its graduation.  `--enable=<any of them>` is now the hard TUR-E0310
     * an unknown name gets, pinned by errors/experiment-retired-name.
     *
     * `refined` also aged out of the `#lang` layer shim in the same change,
     * so `#lang turmeric refined` became TUR-E0330; the two shims were always
     * a pair and had to go together.  The layer axis has since been
     * decommissioned outright, so that token is now simply a trailing one.
     *
     * Only `jit-ffi` stays.  It graduated in 0.38.0 -- this line -- so its
     * window has not opened yet; it becomes eligible at 0.39.0. */
    "jit-ffi",       /* graduated 2026-08-21; call-ptr / callback-ptr are ordinary `unsafe` forms (-DTUR_JIT=ON still gates the interpreter path) */
    /* r7rs-gc GRADUATED 2026-09-25 (introduced 0.52.0, graduated in the same
     * line) -- the conservative collector is the allocator of every compiled
     * single-unit `#lang r7rs` program on Linux and macOS (g_opt_r7rs_gc
     * defaults true; emit_module.c's r7rs_gc_active).  A program that starts
     * threads opts out with TUR_R7RS_GC=0 or --no-r7rs-gc.  See
     * docs/archive/r7rs-gc-plan.md.  Eligible to age out at 0.54.0. */
    "r7rs-gc",       /* graduated 2026-09-25; a compiled #lang r7rs program's allocator is the collector (TUR_R7RS_GC=0 / --no-r7rs-gc opt out) */
    /* graduated 2026-08-27, in the 0.39 line.  A representation change with no
     * syntax, so the population that could have written it into a build.tur is
     * the narrow one the backend names above describe -- but it shipped as a
     * user-facing `--enable` for a full release, so it keeps the window. */
    "parametric-sum-byvalue",
    "option-niche",  /* graduated 2026-09-03; TUR_OPTION_NICHE=0 restores the tagged monomorph */
    "regions",       /* graduated 2026-09-05; TUR_REGIONS=0 restores malloc and unbracketed calls */
    /* graduated 2026-09-10, in the 0.46 line.  Unlike the names above, nobody
     * had to write this one anywhere: `#lang saffron` was always its own enable
     * (D9), so a hand-written `--enable=saffron` only ever duplicated the
     * directive.  It keeps the window anyway -- it shipped as a listed
     * `tur experiments` row for a full release, which is enough for a
     * build.tur somewhere to name it. */
    "saffron",
    /* graduated 2026-10-01, at 0.57.0 (introduced 0.52.0).  Same case as
     * `saffron` directly above and kept for the same reason: the `#lang r7rs`
     * line was always its own enable (D11), so a hand-written
     * `--enable=r7rs` only ever duplicated the directive -- but it shipped as
     * a listed `tur experiments` row for five minor lines, which is enough
     * for a build.tur or an experiments.tur somewhere to name it.  Eligible
     * to age out at 0.58.0.  See docs/archive/r7rs-lang-plan.md. */
    "r7rs",
    /* graduated 2026-09-25, in the 0.53 line (introduced 0.49.0, carrying an
     * advisory expires_at of 0.55.0 -- graduating early is routine).  Source
     * syntax someone had to write into a file and then enable, so a lingering
     * enable is exactly what a real adopter's config looks like: it keeps the
     * full migration window.  Eligible to age out at 0.54.0. */
    "class-superclasses",
    /* graduated 2026-10-03, in the 0.61 line.  Source syntax a user had to
     * write and enable, so it keeps the full window.  Eligible to age out at
     * 0.62.0.  See docs/archive/loop-invariants-plan.md. */
    "loop-invariants",
    NULL,
};

static bool experiment_is_graduated(const char *name) {
    if (!name) return false;
    for (size_t i = 0; GRADUATED[i]; i++)
        if (strcmp(GRADUATED[i], name) == 0) return true;
    return false;
}

/* Number of real entries (excluding the trailing { 0 } sentinel). */
size_t experiment_count(void) {
    return (sizeof(EXPERIMENTS) / sizeof(EXPERIMENTS[0])) - 1;
}

const ExperimentDescriptor *experiment_at(size_t i) {
    if (i >= experiment_count()) return NULL;
    return &EXPERIMENTS[i];
}

const ExperimentDescriptor *experiment_lookup(const char *name) {
    if (!name) return NULL;
    size_t n = experiment_count();
    for (size_t i = 0; i < n; i++) {
        if (strcmp(EXPERIMENTS[i].name, name) == 0) return &EXPERIMENTS[i];
    }
    return NULL;
}

/* Per-index enable source + once-per-compile warning dedup.  Sized to a fixed
 * cap that comfortably exceeds any sane table size (the mechanism is designed
 * so no more than ~2 flags live here at once). */
#define XF_MAX 64
static ExperimentSource g_src[XF_MAX];   /* XF_SRC_NONE = disabled */
static bool             g_warned[XF_MAX]; /* TUR-W006x emitted this compile */

static long experiment_index(const char *name) {
    if (!name) return -1;
    size_t n = experiment_count();
    for (size_t i = 0; i < n; i++) {
        if (strcmp(EXPERIMENTS[i].name, name) == 0) return (long)i;
    }
    return -1;
}

/* Once-per-process dedup for the graduated-experiment notice.  A CLI enable is
 * applied in both the parent and each worker, so warn only the first time. */
static bool g_grad_warned;

bool experiment_enable(const char *name, ExperimentSource src) {
    long idx = experiment_index(name);
    if (idx < 0) {
        /* Not a live experiment.  If it graduated, accept it as an always-on
         * no-op with a one-time deprecation notice; otherwise the caller
         * reports TUR-E0310 for an unknown name. */
        if (experiment_is_graduated(name)) {
            if (!g_grad_warned) {
                g_grad_warned = true;
                fprintf(stderr,
                        "warning [TUR-W0063]: experiment '%s' graduated and is "
                        "now on by default; --enable is no longer needed\n",
                        name);
            }
            return true;
        }
        return false;
    }
    if (idx >= XF_MAX) return false;
    const ExperimentDescriptor *d = &EXPERIMENTS[idx];
    if (d->opt_global) *d->opt_global = true;
    /* Higher-numbered source wins.  CLI beats manifest beats user-config
     * beats not-yet-set (XF_SRC_NONE); a lower-precedence enable never
     * downgrades a higher one that already ran. */
    if (src > g_src[idx]) g_src[idx] = src;
    return true;
}

bool experiment_is_enabled(const char *name) {
    long idx = experiment_index(name);
    if (idx < 0 || idx >= XF_MAX) return false;
    return g_src[idx] != XF_SRC_NONE;
}

ExperimentSource experiment_source_at(size_t i) {
    if (i >= experiment_count() || i >= XF_MAX) return XF_SRC_NONE;
    return g_src[i];
}

/* UC-4 (user-config-experiments-plan): the TUR-W006x lifecycle warnings now
 * fire unconditionally.  Enabling an experiment (via --enable=<name>,
 * build.tur, or ~/.config/turmeric/experiments.tur) is itself the
 * acknowledgment; there is no longer a --allow-experimental gate to suppress
 * them. */
void experiment_warn_if_used(const char *name) {
    long idx = experiment_index(name);
    if (idx < 0 || idx >= XF_MAX) return;
    if (g_src[idx] == XF_SRC_NONE) return;   /* not enabled -> nothing to warn */
    if (g_warned[idx]) return;               /* once per compile */
    g_warned[idx] = true;
    const ExperimentDescriptor *d = &EXPERIMENTS[idx];
    if (d->lifecycle == XF_LIFECYCLE_BETA) {
        fprintf(stderr,
                "warning [TUR-W0061]: experimental feature '%s' (beta) -- "
                "graduates in %s; see %s\n",
                d->name, d->expires_at, d->plan_path);
    } else {
        fprintf(stderr,
                "warning [TUR-W0060]: experimental feature '%s' (prototype) -- "
                "breaking changes likely; see %s\n",
                d->name, d->plan_path);
    }
}

void experiment_reset_warnings(void) {
    memset(g_warned, 0, sizeof(g_warned));
}

void experiment_mark_warned(const char *name) {
    long idx = experiment_index(name);
    if (idx >= 0 && idx < XF_MAX) g_warned[idx] = true;
}

/* ------------------------------------------------------------------------- *
 * UC-2: user-level experiments file reader.
 *
 * The file's grammar is a tiny subset of turmeric syntax -- a handful of
 * `:key [atom ...]` pairs, with `;` line comments and `#| |#` block comments.
 * A dedicated hand reader (no manifest reader, no `.tur` evaluation) keeps the
 * surface understandable and free of project-manifest assumptions; see the
 * plan's "The reader" section (option B).
 * ------------------------------------------------------------------------- */

/* Resolve the platform path to the user-level experiments file into `buf`.
 * Returns true (and fills `buf`) when a config directory is known -- the file
 * itself may or may not exist -- and false when no home/config dir can be
 * determined. */
static bool uc_config_path(char *buf, size_t buflen) {
#ifdef _WIN32
    const char *appdata = getenv("APPDATA");
    if (appdata && appdata[0]) {
        int n = snprintf(buf, buflen, "%s\\turmeric\\experiments.tur", appdata);
        return n > 0 && (size_t)n < buflen;
    }
    return false;
#else
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0]) {
        int n = snprintf(buf, buflen, "%s/turmeric/experiments.tur", xdg);
        return n > 0 && (size_t)n < buflen;
    }
    const char *home = getenv("HOME");
    if (home && home[0]) {
        int n = snprintf(buf, buflen, "%s/.config/turmeric/experiments.tur", home);
        return n > 0 && (size_t)n < buflen;
    }
    return false;
#endif
}

/* Slurp a whole file into a NUL-terminated malloc'd buffer.  Returns NULL if
 * the file cannot be opened (absent / unreadable) or on allocation failure.
 * The caller frees. */
static char *uc_slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

typedef enum { UC_EOF, UC_LBRACK, UC_RBRACK, UC_ATOM } UcTok;

static bool uc_is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

/* Advance `*pp` past whitespace, `;' line comments, and `#| |#' block
 * comments (non-nesting, matching the manifest reader's convention). */
static void uc_skip_ws(const char **pp) {
    const char *p = *pp;
    for (;;) {
        while (uc_is_space(*p)) p++;
        if (*p == ';') {                         /* line comment to EOL */
            while (*p && *p != '\n') p++;
            continue;
        }
        if (p[0] == '#' && p[1] == '|') {        /* block comment */
            p += 2;
            while (*p && !(p[0] == '|' && p[1] == '#')) p++;
            if (*p) p += 2;
            continue;
        }
        break;
    }
    *pp = p;
}

/* Lex one token.  For UC_ATOM, [*ts, *te) delimits the atom text (a leading
 * ':' is kept, marking a keyword).  Brackets carry no text. */
static UcTok uc_next(const char **pp, const char **ts, const char **te) {
    uc_skip_ws(pp);
    const char *p = *pp;
    if (*p == '\0') return UC_EOF;
    if (*p == '[') { *pp = p + 1; return UC_LBRACK; }
    if (*p == ']') { *pp = p + 1; return UC_RBRACK; }
    const char *start = p;
    while (*p && !uc_is_space(*p) && *p != '[' && *p != ']' && *p != ';') p++;
    *ts = start;
    *te = p;
    *pp = p;
    return UC_ATOM;
}

/* Copy the atom [ts,te) into `name` (truncating to fit) and enable it at
 * XF_SRC_USER_CONFIG.  Returns experiment_enable's result; on false, `name`
 * holds the offending token for the caller's error message. */
static bool uc_try_enable(const char *ts, const char *te,
                          char *name, size_t namesz) {
    size_t len = (size_t)(te - ts);
    if (len >= namesz) len = namesz - 1;
    memcpy(name, ts, len);
    name[len] = '\0';
    return experiment_enable(name, XF_SRC_USER_CONFIG);
}

bool experiments_read_user_config(void) {
    char path[4096];
    if (!uc_config_path(path, sizeof(path))) return false;
    char *src = uc_slurp(path);
    if (!src) return false;   /* absent / unreadable -> no-op */

    const char *p = src;
    const char *ts, *te;
    UcTok t;
    while ((t = uc_next(&p, &ts, &te)) != UC_EOF) {
        if (t != UC_ATOM || ts[0] != ':') continue;  /* expect a :key; skip stray tokens */

        /* Copy the key name (without the leading ':'). */
        char key[128];
        size_t klen = (size_t)(te - ts) - 1;
        if (klen >= sizeof(key)) klen = sizeof(key) - 1;
        memcpy(key, ts + 1, klen);
        key[klen] = '\0';

        if (strcmp(key, "enable") == 0) {
            const char *save = p;
            t = uc_next(&p, &ts, &te);
            if (t == UC_LBRACK) {
                char name[128];
                while ((t = uc_next(&p, &ts, &te)) == UC_ATOM) {
                    if (!uc_try_enable(ts, te, name, sizeof(name))) {
                        fprintf(stderr,
                                "error [TUR-E0310]: unknown experiment '%s' in "
                                "%s :enable; run 'tur experiments' for the list\n",
                                name, path);
                        free(src);
                        exit(2);
                    }
                }
                /* t is UC_RBRACK or UC_EOF -- either way the list is done. */
            } else if (t == UC_ATOM) {
                /* Bare `:enable name` (no vector) -- accept the single name. */
                char name[128];
                if (!uc_try_enable(ts, te, name, sizeof(name))) {
                    fprintf(stderr,
                            "error [TUR-E0310]: unknown experiment '%s' in "
                            "%s :enable; run 'tur experiments' for the list\n",
                            name, path);
                    free(src);
                    exit(2);
                }
            } else {
                p = save;  /* nothing followed -- let the outer loop resync */
            }
        } else {
            /* Unknown key: warn (TUR-W0062) and skip its value so we resync. */
            fprintf(stderr,
                    "warning [TUR-W0062]: unknown key ':%s' in %s\n", key, path);
            const char *save = p;
            t = uc_next(&p, &ts, &te);
            if (t == UC_LBRACK) {
                int depth = 1;
                while (depth > 0) {
                    t = uc_next(&p, &ts, &te);
                    if (t == UC_EOF) break;
                    if (t == UC_LBRACK) depth++;
                    else if (t == UC_RBRACK) depth--;
                }
            } else if (t == UC_RBRACK || t == UC_EOF) {
                p = save;  /* not our value -- resync at the outer loop */
            }
            /* else: a single-atom value, already consumed. */
        }
    }
    free(src);
    return true;
}
