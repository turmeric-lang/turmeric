#pragma once
/* globals.h — extern declarations for global compiler configuration variables.
 * Definitions are in globals.c. Include this wherever these globals are used. */
#include <stdbool.h>
#include <stdint.h>

/* Phase U5: unsafe linting configuration */
extern uint32_t g_unsafe_max_lines;
extern bool g_unsafe_warn_nested;
extern bool g_unsafe_require_safety;
extern bool g_unsafe_stats_enabled;
extern bool g_lint_unsafe_enabled;
/* U6: warn on inline-C outside #{Unsafe}-annotated functions */
extern bool g_lint_inline_c_unsafe;

/* Phase R5: panic strategy */
extern bool g_panic_abort;

/* Phase R6: result/panic linting */
extern bool g_warn_unused_result;
extern bool g_lint_panic;

/* Phase C2: --no-contracts (strip contract checks at elaboration) */
extern bool g_no_contracts;

/* Debugger Phase 4: --debug emits `#line N "file.tur"` directives into the
 * generated C (so gdb/lldb step through .tur source) and switches the single-
 * file `tur build` C-compile to `-g -O0`.  Off by default so ordinary builds
 * and `emit-c` snapshots are unchanged. */
extern bool g_emit_debug_lines;

/* Phase B5: backtrack depth + clone plan dump */
extern int64_t g_backtrack_depth;
extern bool g_dump_clone_plan;

/* CPS1: --dump-cps-coloring flag — print colored/uncolored partition after CPS1 */
extern bool g_dump_cps_coloring;

/* CPS3: --cps-path flag — emit CPS wrappers for colored functions */
extern bool g_cps_path;

/* Phase U5: unsafe linting statistics */
extern uint32_t g_unsafe_block_count;
extern uint32_t g_unsafe_total_lines;

/* Phase P3: HAMT lowering */
extern bool g_needs_hamt;

/* jit-ffi-c2mir-plan: the program elaborated a dlopen/dlsym/dlclose builtin
 * (or a call-ptr), so the emitted C needs <dlfcn.h> and the link needs -ldl
 * (a no-op on glibc >= 2.34 / macOS, required on older glibc).  Same
 * lifecycle as g_needs_hamt: set during elaboration, read by the preamble. */
extern bool g_needs_dlfcn;

/* AR8: Variadic rest parameters -- set when any variadic defn is compiled */
extern bool g_has_variadics;

/* prelude-macros (Defect B / F3): set when the user-callable `cons` runtime
 * list constructor is referenced. */
extern bool g_uses_cons;

/* Phase G1: -Xgadt flag — enable defgadt syntax */
extern bool g_gadt_enabled;

/* DL0 (data-literals-plan): -Xdata-literals flag — enable map/vec/set data
 * literal syntax (#map{...}, #set{...}, and [...] in expression position). */
extern bool g_data_literals_enabled;

/* JR0 (json-reader-macro-plan): -Xjson-reader flag — enable the #json(...)
 * compile-time reader macro. */
extern bool g_json_reader_enabled;

/* RD (return-type-dispatch-and-schema plan): -Xschema-reader flag — enable the
 * #json-str<T>(...) family of typed-decode reader macros.  Implies
 * -Xjson-reader and additionally auto-loads schema.tur. */
extern bool g_schema_reader_enabled;

/* Phase SZ4: -Xsized-types flag — enable sized types (implies -Xgadt) */
extern bool g_sized_types_enabled;

/* Phase SZ8: --dump-sizes flag — print inferred size index per sized-GADT
 * constructor application during elaboration (requires -Xsized-types) */
extern bool g_dump_sizes;
/* SX8a: --dump-refine=json -- one JSON record per refinement obligation.
 * A diagnostic/dump surface, so explicitly outside the EXPERIMENTS[]
 * regime per the experimental-features rule. */
extern bool g_dump_refine_json;

/* ER1: --strict-effects flag — warn/check unannotated effectful functions */
extern bool g_strict_effects;

/* ER6: --dump-effects flag — print inferred effect row for each top-level defn */
extern bool g_dump_effects;

/* G1 (docs/archive/mutable-globals-plan.md): --dump-write-frames flag — print
 * the WF2 verdict for every DECLARED `#writes` frame, plus the global-write
 * answer that can downgrade it.  A diagnostic knob, not an experiment: it
 * reports what the checker decided and changes nothing.  Without it the only
 * observable difference between VERIFIED and UNVERIFIED is the absence of a
 * diagnostic, which is not something a fixture can assert on. */
extern bool g_dump_write_frames;

/* R4 slice 2 (trusted-refinement-claims-plan): --dump-read-frames flag --
 * print the read-frame verification verdict for every `#reads`-annotated
 * function (VERIFIED / EXCEEDED / UNVERIFIED).  Same character as
 * --dump-write-frames: a diagnostic knob, not an experiment -- it reports
 * what rf_resolve_read_frames decided and changes nothing.  The pass itself
 * runs unconditionally; this flag only makes its verdicts visible. */
extern bool g_dump_read_frames;

/* CPS2 (cps-transform-plan): --dump-cps flag — print the ANF/CPS IR for each
 * colored user-level top-level defn */
extern bool g_dump_cps;

/* Phase I: --emit-abi-trace flag — print the resolved ABI path (concrete-clone,
 * carrier, dictionary, polymorphic-wrapper) for each call site during emit-c */
extern bool g_emit_abi_trace;

/* LT0: -Xlinear flag — enable linear type checking */
extern bool g_linear_enabled;

/* UT0: -Xunique-types flag -- enable uniqueness type checking */
extern bool g_unique_enabled;

/* ST0: -Xsubstructural flag — enable substructural type checking (implies -Xlinear) */
extern bool g_substructural_enabled;

/* IT0: -Xunion-types flag — enable union type syntax and checking */
extern bool g_union_types_enabled;

/* IT2: -Xintersection-types flag — enable intersection type syntax */
extern bool g_intersection_types_enabled;

/* ET4: -Xeffect-types enables full effect typing (TY_HANDLER, handler typing, ET4 checks) */
extern bool g_effect_types_enabled;

/* CT3: Contract checking configuration */
extern bool g_contracts_enabled;          /* -Xcontracts: enable contract syntax (always on in debug) */
extern bool g_keep_contracts_in_release;  /* --keep-contracts: retain checks in release builds */

/* SS0a: -Xsessions flag — enable session type syntax and checking (implies -Xsubstructural) */
extern bool g_sessions_enabled;

/* DV0: -Xdynamic-vars flag — enable dynamic var syntax and checking */
extern bool g_dynvar_enabled;

/* CF4 / call-cc-completion CC5: vestigial.  -Xcallcc once gated the unsound
 * call/cc / escape stub; call/cc / escape are now real, sound, and enabled by
 * default on the CPS substrate, and the flag is a deprecated no-op.  No longer
 * read by the elaborator; kept for one release to avoid churn. */
extern bool g_callcc_enabled;

/* SYM0 (runtime-symbols-plan): -Xsymbols flag — enable first-class runtime
 * symbol values.  When off, a keyword in expression position is a hard error
 * (its only legal uses are syntactic: annotations, :refer, field selectors).
 * When on, `:foo` elaborates to a :Sym literal. */
extern bool g_symbols_enabled;

/* INT-2: --interpret mode flag — set by cmd_eval before elaboration. */
extern bool g_interpret_mode;

/* `tur expand`: print each macro expansion (outside stdlib load) to stdout
 * as it happens during elaboration.  Set by cmd_expand in main.c; read at
 * the expansion site in elab_call.c. */
extern bool g_dump_expansion;

/* Stage 3 (macro-system-direction-plan): --macro-caps=io grants the
 * macro-time env TURI_CAP_IO for the rare legitimately-effectful macro
 * (embed-file style).  Default deny; io is the only grantable capability
 * -- FFI/Unsafe/inline-C/async at macro time are never offered.  Set by
 * the global flag parser in main.c; read at macro-env creation in
 * src/turi/macro_env.c. */
extern bool g_macro_caps_io;

/* security-audit-plan WP3: --no-proc-macros.  When set, defining, calling, or
 * `:for-macros`-importing into a procedural macro (defmacro*) is a diagnostic
 * and no macro-time code runs; template defmacro still expands, since it runs
 * nothing.  rust-analyzer's `procMacro.enable = false`, for pointing `tur
 * check` at a tree you have not read.  Read in src/turi/macro_env.c. */
extern bool g_no_proc_macros;

/* interp-stdlib-class-method-shadows-user-defn: true while an interpreter
 * entry point (--interpret / REPL / WASM) is preloading stdlib modules via
 * `(load ...)` turns.  Those turns run with stdlib_prefix == 0, so
 * `e->in_stdlib_load` never brackets them and every typeclass they register
 * would read as USER-defined -- flipping the documented "user defn overrides
 * a stdlib method" resolution (prefer_method_dispatch in elab_call.c) and
 * the TUR-W0039 clash warning.  Typeclass registration ORs this in for
 * `tc->from_stdlib`.  Deliberately NOT applied to binding-level
 * `is_from_stdlib`: interpreter fixtures legitimately redefine stdlib defns
 * (the benchmark head/tail stub pattern), and marking those would turn the
 * MF3 collision error on under --interpret. */
extern bool g_turi_stdlib_preload;

/* F4 (cross-plan-followups): --Werror=deprecated flag — promotes
 * ^deprecated use-site warnings to errors. */
extern bool g_werror_deprecated;

/* Phase C: --Werror=inline-c-narrow-params — promote narrow-type-in-inline-C
 * warnings to errors so a strict build can gate against unannotated narrow
 * parameters reaching inline-C bodies. */
extern bool g_werror_inline_c_narrow_params;

/* -Werror=strict-effects: promote the --strict-effects lints (TUR-W0030,
 * TUR-W0032) to errors, so a build can gate on effect annotations.  Implies
 * --strict-effects, the way gcc's -Werror=<x> implies -W<x>. */
extern bool g_werror_strict_effects;

/* forall-kinds GRADUATED 2026-07-06 -- explicit kind annotations on
 * forall/exists bound variables ((f :: * -> *)) are always accepted; the enable
 * bit and its elab_types.c gate are gone.  forall-constraints and hkt-hrt
 * graduated the same day (see docs/archive/history/constrained-hkt-forall-plan.md). */

/* forall-dict-pass GRADUATED 2026-07-06 -- always-on.  A genuinely polymorphic
 * constrained function passed as a rank-2 argument is compiled to dispatch its
 * class methods through a runtime dictionary threaded via the poly carrier (a
 * dict-clone of the function + one leading dict argument per constraint,
 * resolved at each invocation).  The enable bit is gone.  See
 * docs/archive/history/forall-dict-pass-multi-constraint-hkt-plan.md. */

/* hrt-curried-result GRADUATED 2026-07-06 -- a rank-2 poly fn whose forall body
 * RESULT is itself a function type (e.g. `forall a. a -> (a -> a)`) always
 * instantiates that result to a concrete callable closure, so `(l x)` yields a
 * closure `((l x) y)` can apply (van Laarhoven optic composition).  The enable
 * bit and its two elab_call.c gates are gone.  See
 * docs/archive/history/constrained-hkt-forall-mode-b-plan.md. */

/* vl-wide-functor GRADUATED 2026-07-04 (VBM4).  A van Laarhoven lens may focus
 * through a WIDE by-value aggregate functor (a `:copy` struct / single-variant
 * flat-product ADT wider than the one-int64 carrier word) unconditionally -- no
 * flag, TUR-E0309 retired.  Path A boxes the aggregate into the carrier at each
 * lens crossing (the dict-dispatched `fmap`, the fat-boxed functor-wrapping `g`,
 * and the lens result) and unboxes it back on the way out, mirroring the
 * direct-shape MB2.5 bridge; the wide-ness test (non-opaque, non-:heap
 * flat-product) still gates it, so carrier-compatible functors are untouched.
 * The former `g_opt_vl_wide_functor` bool is gone; its guards are now
 * unconditional. */

/* VBM1-CM4 (docs/archive/history/van-laarhoven-monomorphization-plan.md +
 * van-laarhoven-consumer-mono-plan.md): the by-value HKT monomorphization path
 * (Path B) GRADUATED 2026-07-05 -- the former `vl-wide-mono` experiment is
 * retired and its registration/redirect/clone emit are unconditional.
 * elab_poly_call registers a spec key for each van Laarhoven lens site whose
 * pinned functor `f` is a WIDE by-value aggregate (see mono_specs.c); the
 * poly-call emit redirects every SIMPLE lens call site that resolves uniquely
 * to a by-value mono body -- no `(f S)`/`(f A)` heap box on that path.  COMPOSED
 * lenses (lens_is_simple_for_pathb == false) fall back to the (unconditional)
 * Path A carrier bridge, which also backs runtime-selected sites.
 * `g_dump_mono_specs` (from `--dump-mono-specs`) prints the registry after
 * elaboration. */
extern bool g_dump_mono_specs;

/* `g_dump_cps_mono` (from `--dump-cps-mono`) prints, for each colored-generic
 * MONOMORPH the direct emitter specializes, whether that monomorph's body +
 * concrete signature would land in the CPS-backend subset (its generic template
 * sig-rejects on the tyvar TY_APP, so the template is never a candidate).  See
 * docs/archive/cps-backend-generic-monomorph-classification-plan.md (G1).
 * Analysis only -- it changes no emitted code. */
extern bool g_dump_cps_mono;

/* g_opt_cps_effects RETIRED 2026-07-12 -- the `cps-effects` experiment graduated
 * and `handle-shallow` is now unconditionally accepted (see experiments.c). */

/* g_opt_backtrackable_state RETIRED 2026-08-29 -- the `backtrackable-state`
 * experiment graduated and stdlib/trail.tur is autoloaded unconditionally
 * (see experiments.c and stdlib_autoload.c). */
/* TUR_ADT_SLAB=1: bump-allocate never-freed multi-variant ADT boxes.
 * A measurement seam for docs/reported/multi-variant-adts-always-heap-allocate.md,
 * not a shipping default.  SHELVED 2026-08-25 -- kept reproducible, not
 * headed anywhere; the decision record in that report says why, and why
 * reclamation rather than a slab is the thing to build. */
extern bool g_adt_slab;
/* SR1 (docs/archive/sum-representation-plan.md): flow a non-recursive,
 * non-parametric, non-heap MULTI-VARIANT sum by value (tag + union aggregate)
 * instead of the int64 heap carrier.
 *
 * ON by default since 2026-08-26.  It is the fix for both halves of
 * docs/archive/multi-variant-adts-always-heap-allocate.md: such a sum is no
 * longer malloc'd on every construction, and a value that is never boxed has
 * nothing to leak.  A 2e6-construction loop over a two-variant `:copy` sum
 * went from 62.6 MB peak RSS to 1.2 MB.
 *
 * `TUR_SR1_SUM_BYVALUE=0` restores the int64 carrier -- an escape hatch for
 * bisecting a suspected representation bug, not a supported mode.  Recursive
 * sums are unaffected either way; they are SR4's population and still ride the
 * carrier (see AdtDef.is_self_recursive for why, and what blocks them). */
extern bool g_sr1_sum_byvalue;
/* SX1: set by tur_stdlib_prepend_forms when stdlib/trail.tur was actually read
 * into THIS compile.  Not the same question as "is the trail available": the
 * autoload can be suppressed (--no-auto-stdlib), and the emitted serial prelude
 * must only reference `tur_trail_level` when trail.tur's autolink marker is
 * also in the output pulling src/runtime/trail.c into the link.  Emitting the
 * guard off any looser signal is an undefined symbol at cc time. */
extern bool g_trail_autoloaded;
/* saffron-lang-plan S6 / r7rs-lang-plan R1: the ENTRY file's language
 * autoloads a prelude -- `LangTraits.prelude`, a stdlib-relative tail such as
 * "saffron/prelude.tur" -- so that tail joins the stdlib autoload list.  NULL
 * for a language with no prelude (Turmeric).  Was the bool
 * `g_saffron_prelude`; it became the path when a second language with a
 * prelude arrived, so the autoloaders read the trait instead of a name.
 *
 * Set by every path that detects the entry file's dialect, and set on EVERY
 * such call (a path or NULL) rather than only when non-NULL -- the REPL and
 * the harnesses run several compiles in one process, and a sticky value would
 * let a Saffron file license the prelude for the next Turmeric one.  Same
 * hazard `g_trail_autoloaded` records above, handled by being self-resetting
 * rather than by a separate clear.
 *
 * The prelude is scoped to the ENTRY file on purpose: a Saffron file IMPORTED
 * by a Turmeric program does not drag it in.  That keeps the prelude's names
 * out of a program that never asked for the dialect, and matches how the
 * `#lang` line already scopes the reader and the semantic layers. */
extern const char *g_lang_prelude;
/* saffron-lang-plan S8: `tur repl --lang saffron` -- start the interactive
 * session in Saffron instead of making the user type `#lang saffron` as their
 * first line.  Read once at REPL startup; `#lang` at the prompt is the other
 * route to the same env state. */
extern const char *g_repl_start_lang;
/* r7rs-lang-plan R9: in a synthetic `<...>` source (the interpreter's `<eval>`
 * blob), the first line that is USER input rather than the pinned stdlib
 * preload, or 0 when nothing is pinned.  The REPL compiles the pinned preload
 * and the prompt's input as one `<eval>` text, so a `#lang r7rs` session needs
 * the Scheme renames on the second part and none on the first (the preload's
 * native stubs are Turmeric).  Read by scheme_lower.c prelude_span; set around
 * each interpreter eval (turi_eval_with_sink). */
extern uint32_t g_synthetic_user_from_line;
/* SR3 slice B (the Option niche -- default since 2026-09-03, TUR_OPTION_NICHE=0
 * restores the tagged form; docs/archive/sr3-option-niche-plan.md):
 * an `(Option P)` whose payload is a NON-NULLABLE pointer is carried AS that
 * pointer -- 16 bytes down to 8, `(none)` as NULL, no tag word anywhere.
 *
 * Behind an experiment rather than on, because the soundness condition ("P's
 * valid values exclude 0") is not something the type system records: it is a
 * hand-maintained allowlist in `sr3_payload_is_nonnull_pointer` (types.c), and
 * an entry added in error makes `(some x)` and `(none)` the same value.  The
 * polarity is deliberate -- an unrecognised payload merely misses the
 * optimisation.
 *
 * Layered on g_sr2_app_sum_byvalue below: sr3_option_niche (types.c) reads this
 * bit only after that one, because a niche narrows a BY-VALUE Option and is
 * meaningless -- and, measured, crashing -- over the int64 carrier.  Clearing
 * SR2 therefore clears the niche too; clearing the niche leaves SR2 alone. */
extern bool g_opt_option_niche;
/* RM3 regions (docs/archive/regions-plan.md): declared lifetimes over the
 * arena that already ships, for values with no unique owner -- the per-node
 * spine box of a persistent recursive structure, which RM1's scope-exit rule
 * cannot reach (the nodes escape their constructor by construction) and RM2
 * cannot own (sharing makes "is this the last reference?" a runtime fact).
 *
 * Off by default and gated behind `--enable=regions`: this is user-visible
 * surface, and it is the reclamation phase most able to produce a silent wrong
 * answer if it rewinds a generation something still points into.  The plan's
 * safety rule is that a region which cannot PROVE every escaping value
 * relocatable does not rewind at all, so a missed shape costs a saving rather
 * than correctness -- the same discipline turi's value-pool promotion walk
 * already runs on.
 *
 * GRADUATED 2026-09-05: default true.  TUR_REGIONS=0 (main.c) is the bisection
 * hatch; tests/run-regions-seam.sh keeps that off path green. */
extern bool g_opt_regions;

/* "This build contains a DYNAMICALLY TYPED translation unit."  Set by
 * lang_dialect_apply when the reader takes a `#lang` line whose language's
 * trait row says `dynamic` (Saffron today; r7rs-lang-plan's `#lang r7rs`
 * next); never by a user-facing flag.
 *
 * GRADUATED 2026-09-10, at 0.46.0, as `g_opt_saffron`.  This WAS the
 * `saffron` experiment's enable bit, flipped by `--enable=saffron` /
 * `:experiments` / the `#lang` line (D9); the experiment is gone and
 * `--enable=saffron` is a TUR-W0063 no-op, but the bit stays because the
 * emitter reads it for a reason unrelated to gating: it decides whether to
 * emit the `any` type registry, the instance registry and the
 * dynamic-dispatch panic (emit_module.c).  A plain Turmeric program's emitted
 * C is byte-for-byte what it was before Saffron existed, and that is what
 * this bit buys.  It is NOT an on/off switch for a dialect -- the per-file
 * `SourceFile.lang` is (lang_span_is_dynamic).  Renamed in r7rs-lang-plan R0
 * because the fact it records is "the `any` machinery is needed", which is a
 * trait shared by every dynamic language, not Saffron's identity. */
extern bool g_opt_dynamic_any;
extern bool g_opt_saffron_gc;
/* r7rs (docs/archive/r7rs-lang-plan.md): does this BUILD contain an r7rs TU?
 * Never set by a flag a user has to write -- lang_dialect_apply sets it the
 * moment a `#lang r7rs` file is read, because the directive is itself the
 * enable (D11).
 *
 * It SURVIVED the dialect's graduation at 0.57.0 and keeps its name, the same
 * call saffron's graduation made for g_opt_saffron/g_opt_dynamic_any and for
 * the same reason: it was never only an enable bit.  emit_module.c's
 * r7rs_gc_active reads it to pick which collector opt-out governs the
 * program -- g_opt_r7rs_gc (TUR_R7RS_GC=0 / --no-r7rs-gc) when an r7rs TU is
 * present, else Saffron's g_opt_saffron_gc -- and both dialects set
 * g_opt_dynamic_any, so that bit cannot tell them apart.  `--enable=r7rs` is
 * a TUR-W0063 no-op now, so the directive is the only writer.
 *
 * The dialect's SEMANTICS ride none of this: they are LangTraits.dynamic
 * (g_opt_dynamic_any), LangTraits.scheme_truthiness and
 * SourceFile.reader_type == READER_R7RS, all per-FILE, where this is
 * per-build. */
extern bool g_opt_r7rs;
/* reflected-measures (docs/upcoming/reflected-measures-plan.md, RF0): the
 * `--enable=reflected-measures` bit.  When on, a `^reflect` defn is
 * registered for the deferred totality pass (elab_reflect.c) and, once it
 * passes, its defining equation is admitted to the refinement encoder by
 * bounded ground unfolding (refine_collect.c).  Off, `^reflect` is inert
 * (warned, never silently enforced). */
extern bool g_opt_reflected_measures;
/* RF5: --dump-reflect -- one line per `^reflect` site: verdict, decreasing
 * argument position, rejection reason.  A diagnostic knob, not an
 * experiment (same footing as --dump-write-frames). */
extern bool g_dump_reflect;
/* aot-compiled-repl-plan C1: `--enable=repl-jit-inline-c`.  An inline-C defn
 * the interpreter cannot run is compiled in process on its first call, on a
 * host that registered a compiler (turi/inline_c_jit.h; TUR_HAVE_JIT `tur`). */
extern bool g_opt_repl_jit_inline_c;
extern bool g_opt_r7rs_gc;
/* SR2a: a MULTI-VARIANT parametric sum monomorph -- `(Opt2 int)`, `(PRes
 * cstr)`, and above all `(Option int)` / `(Result int cstr)` -- flows by value
 * instead of riding the int64 heap-pointer carrier.  The parametric sibling of
 * g_sr1_sum_byvalue above, and the prerequisite SR2b's stdlib conversion was
 * built on: the carrier's monomorph ctors malloc per construction and never
 * free (measured: 16,000 bytes leaked in 1,000 constructions; by value is 3.6x
 * faster at 71x less peak RSS on the same loop).  Same exclusions as SR1:
 * non-GADT, non-heap, not self-recursive, and never a fixpoint partner's
 * functor app (adt_is_fixpoint_partner_of).
 *
 * ON by default since 2026-08-27 -- GRADUATED out of
 * `--enable=parametric-sum-byvalue` once SR2b (Option/Result as real sums) and
 * the spice-side layout migration had both landed, which is exactly what the
 * experiment's soak was waiting for.  `TUR_SR2_APP_SUM_BYVALUE=0` restores the
 * int64 carrier -- an escape hatch for bisecting a suspected representation
 * bug, not a supported mode, and the same two-way shape SR1 kept.  Consumed by
 * sr2_app_sum_byvalue (types.c).  See docs/archive/sr2-gate-results.md. */
extern bool g_sr2_app_sum_byvalue;

/* g_opt_cps_tramp_resume RETIRED 2026-08-22 -- the `cps-tramp-resume`
 * experiment graduated 2026-07-19 and E7's trampolined tail-resume is the sole
 * effect lowering.  The bit outlived the row by seven minor lines, unwritable
 * the whole time (its EXPERIMENTS[] row was gone, so nothing could set it), so
 * its 64 read sites were constant tests with an unreachable arm.  Folded to
 * true and the dead arms deleted; the bit is gone.  See
 * docs/archive/cps-dk-sole-effect-lowering-plan.md. */

/* g_opt_owning_cloneable_capture RETIRED 2026-08-22 -- the
 * `owning-cloneable-capture` experiment graduated 2026-07-20 and admitting an
 * owning value captured into a multi-shot cloneable continuation (with the
 * per-frame env clone/drop teardown) is unconditional.  Same story as the bit
 * above: unwritable since graduation, five reads folded to true.  See
 * docs/archive/history/cps-backend-owning-env-teardown-e3-plan.md. */

/* CG5/CG8 cycle-gc GRADUATED 2026-08-17 -- `(gc-auto!)` is an ordinary call
 * form; the g_opt_cycle_gc enable bit and the elab_gc_auto gate are gone.
 * GC_AUTO itself is still opt-in (you must CALL `(gc-auto!)`) and is never a
 * default.  See docs/archive/gc-cycle-collection-followup-plan.md. */

/* J1-J3 jit GRADUATED 2026-08-17 -- `tur jit` needs only the -DTUR_JIT=ON
 * build-time gate; the g_opt_jit enable bit is gone and the REPL's in-process
 * JIT loader hangs off engine selection instead.  See
 * docs/archive/jit-engine-plan.md. */

/* closure-drop-glue GRADUATED 2026-07-22 -- the Model R drop-glue header ABI
 * (env[-1] -> drop_glue_env_N, released via TUR_CLOSURE_DROP) is now
 * unconditional; the g_opt_closure_drop_glue enable bit and its codegen gates are
 * gone.  See docs/archive/closure-drop-glue-plan.md. */

/* RT0 refined GRADUATED 2026-08-01 -- static discharge of `#refine{...}`
 * predicates is unconditional; the g_opt_refined enable bit and its
 * elaboration gates are gone.  See
 * docs/archive/refined-graduation-plan.md. */

/* sealed-opaque GRADUATED 2026-08-17 -- the `:sealed` defopaque attribute's
 * ENFORCEMENT is unconditional; the g_opt_sealed_opaque enable bit and the
 * ascribe_check_sealed gate are gone.  See
 * docs/archive/sealed-opaque-plan.md. */

/* write-frames GRADUATED 2026-08-20 -- WF2 checks every declared `#writes`
 * frame and WF3 may widen on a checked callee frame, both unconditionally; the
 * g_opt_write_frames enable bit and its gates are gone.  See
 * docs/archive/checked-write-frames-plan.md. */

/* checked-reads GRADUATED 2026-08-20 -- positive evidence that a `#reads`
 * measure's body reads mutable state the frame omits REFUSES the congruence
 * override (the crossing gets the ordinary TUR-W0372) rather than merely
 * warning; the g_opt_checked_reads enable bit and its gates are gone.  Refusal
 * still keys on "saw a read", never on "could not see" -- an inline-C body
 * yields no evidence and keeps the trusted grant.  See
 * docs/archive/trusted-refinement-claims-plan.md (R2). */

/* jit-ffi GRADUATED 2026-08-21 -- `(unsafe (call-ptr p [T1 T2 -> R] args...))`
 * (F3) and `(unsafe (callback-ptr f [sig]))` (F5) are ordinary `unsafe` forms;
 * the g_opt_jit_ffi enable bit and its two elaboration gates are gone.  The
 * BUILD-TIME gate is unaffected: the turi routing still needs `-DTUR_JIT=ON`
 * and reports a clean diagnostic without it, and `unsafe` is still required.
 * See docs/archive/jit-ffi-c2mir-plan.md. */

/* --strict-refine: a diagnostic-strictness knob (NOT an experiment).  Upgrades
 * TUR-E0371 / TUR-W0372 from "keep the runtime check" to a hard compile error,
 * for users who want a fully-discharged build with no silent runtime
 * fallbacks. */
extern bool g_strict_refine;


/* ---------------------------------------------------------------------------
 * Interpreter-native return-type signatures
 * (untyped-native-registration-blocks-curated-facades fix)
 *
 * `turi_register_default_native` / `turi_env_register_native` register an
 * embedder native by (name, fn, ud) with no type information, so the
 * elaborator's eval-mode fallback used to default every native call to :int
 * (with a hand-rolled allow-list for `error?`/`error-message`).  A defn that
 * wraps a native and declares its honest non-:int return type then failed
 * elaboration (TUR-E0707/E0708) because the body was typed :int.
 *
 * This process-global registry lets the typed registration API record the
 * Turmeric type a native's TuriValue result carries at runtime.  The
 * elaborator consults it (replacing the allow-list) so both a direct call and
 * a curated typed wrapper see the right return type.  The enum lives here --
 * in the neutral runtime layer shared by the compiler and the embedder API --
 * so neither side has to include the other's headers.  See
 * docs/archive/history/untyped-native-registration-blocks-curated-facades.md.
 * --------------------------------------------------------------------------- */
typedef enum TurNativeRetType {
    TUR_NRT_INT = 0,   /* default; matches the historical untyped behavior */
    TUR_NRT_FLOAT,
    TUR_NRT_BOOL,
    TUR_NRT_CSTR,
    TUR_NRT_VOID,
    TUR_NRT_PTR,       /* opaque handle -> ptr<void> */
    TUR_NRT_SYNTAX,    /* syntax object (TURI_SYNTAX) -> the Syntax type */
} TurNativeRetType;

/* Register (or replace) the return-type signature for native `name`.  `name`
 * is copied; last write wins.  Registering TUR_NRT_INT is a no-op-equivalent
 * (the elaborator default), but is still recorded so a later override is
 * visible. */
void tur_native_sig_register(const char *name, TurNativeRetType ret);
/* Look up `name`; on hit writes *ret and returns true, else returns false. */
bool tur_native_sig_lookup(const char *name, TurNativeRetType *ret);
/* Drop every registered signature (mirrors turi_clear_default_natives). */
void tur_native_sig_clear(void);
