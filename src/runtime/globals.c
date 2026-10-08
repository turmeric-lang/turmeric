/* globals.c — Definitions for global configuration variables shared between
 * the tur compiler driver (main.c) and libturi (tur_eval_basic, REPL, etc.).
 * main.c modifies these after parsing CLI flags; all defaults are "off". */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "globals.h"  /* TurNativeRetType + native-sig registry prototypes */

/* Phase U5: Global configuration for unsafe linting */
uint32_t g_unsafe_max_lines = 20;
bool g_unsafe_warn_nested = false;
bool g_unsafe_require_safety = false;
bool g_unsafe_stats_enabled = false;
bool g_lint_unsafe_enabled = false;
/* U6: warn on inline-C outside #{Unsafe}-annotated functions */
bool g_lint_inline_c_unsafe = false;

/* Phase R5: Panic strategy configuration */
bool g_panic_abort = false;

/* Phase R6: Result/panic linting configuration */
bool g_warn_unused_result = false;
bool g_lint_panic = false;

/* Phase C2: --no-contracts strips contract checks (assert!/require!/ensure!/
 * invariant!) at elaboration time -- the predicate is never evaluated -- and
 * makes contract-enabled? fold to false. */
bool g_no_contracts = false;

/* Debugger Phase 4: --debug emits `#line` directives into the generated C and
 * builds single-file targets with `-g -O0`. */
bool g_emit_debug_lines = false;

/* Phase B5: --backtrack-depth N global flag (0 = unlimited) */
int64_t g_backtrack_depth = 0;

/* Phase B5: --dump-clone-plan flag */
bool g_dump_clone_plan = false;

/* CPS1: --dump-cps-coloring flag */
bool g_dump_cps_coloring = false;

/* CPS3: --cps-path flag */
bool g_cps_path = false;

/* Phase U5: Global statistics for unsafe linting */
uint32_t g_unsafe_block_count = 0;
uint32_t g_unsafe_total_lines = 0;

/* Phase P3: HAMT lowering - track if HAMT is needed for this compilation */
bool g_needs_hamt = false;
bool g_needs_dlfcn = false;   /* jit-ffi: program uses dlopen/dlsym/call-ptr */

/* stdlib/re.tur: track when any inline-C in this compilation references
 * <regex.h>. When true, the emitter hoists `#include <regex.h>` into the
 * preamble so multiple re_* functions can share the typedef. */
bool g_needs_regex_h = false;

/* WIN3-B: track when any inline-C references the BSD socket API (detected by
 * "AF_INET"). When true and targeting Windows, the emitter writes a Winsock
 * compatibility shim into the preamble so POSIX socket idioms
 * (fcntl(O_NONBLOCK), errno EWOULDBLOCK, close on a socket) compile and behave.
 * Gated because the shim remaps close/recv/send/etc., which must NOT happen in
 * a program that has no sockets. */
bool g_needs_winsock = false;

/* inline-c-function-scope-include-guards fix: deduped set of
 * `#include <...>` / `#include "..."` directives lifted from the top of
 * inline-C bodies during elaboration. Emitted at file scope by emit_program
 * (monolithic) and emit_header (separate-compilation) so include-guarded
 * headers (e.g. sqlite3.h) are visible to every inline-C function in the TU.
 * Per-function `#include` is otherwise skipped by the header's own include
 * guard after the first occurrence -- hiding the header's typedefs from
 * subsequent functions. */
char    **g_hoisted_includes = NULL;
uint32_t  g_n_hoisted_includes = 0;
uint32_t  g_cap_hoisted_includes = 0;
/* Parallel to g_hoisted_includes: did the stdlib hoist this entry?  A split
 * build's library unit writes only those (emit_hoisted_includes).  The scan's
 * caller says whose code it is scanning in g_hoist_origin_stdlib. */
bool     *g_hoisted_stdlib = NULL;
bool      g_hoist_origin_stdlib = false;

/* AR8: Variadic rest parameters -- track if any variadic defn is compiled */
bool g_has_variadics = false;

/* prelude-macros (Defect B / F3): set when the user-callable `cons` runtime
 * list constructor is referenced.  Gates emission of the `cons` cons-cell
 * helper in both single-file (emit_program) and project-mode
 * (emit_implementation) output so non-cons programs don't churn snapshots. */
bool g_uses_cons = false;

/* Phase G1: -Xgadt flag — enable defgadt syntax.
 * range-gadt-typeclass-migration-plan A1: graduated to default-on. defgadt and
 * GADT pattern matching now work without a flag; -Xgadt is a deprecated no-op
 * (still accepted, mirroring the -Xcallcc graduation). */
bool g_gadt_enabled = true;

/* drop-x-flags-plan (v0.24.0): -Xdata-literals is now an accept-and-warn
 * no-op; data-literal syntax (#map{...}, #set{...}, [...] in expression
 * position) is unconditionally on. */
bool g_data_literals_enabled = true;

/* drop-x-flags-plan (v0.24.0): -Xjson-reader is now an accept-and-warn
 * no-op; the #json(...) reader macro is unconditionally on. */
bool g_json_reader_enabled = true;

/* drop-x-flags-plan (v0.24.0): -Xschema-reader is now an accept-and-warn
 * no-op; #json-str<T>(...) typed-decode readers are unconditionally on. */
bool g_schema_reader_enabled = true;

/* Phase SZ4: -Xsized-types — was opt-in; now ON by default. Sized types are
 * mature: stdlib uses (Static n) (stdlib/sized*.tur), the tur-ecs spice
 * depends on sized storages, and the in-tree fixture coverage is broad. The
 * `-Xsized-types` CLI flag is now a deprecated no-op, mirroring -Xgadt. */
bool g_sized_types_enabled = true;

/* Phase SZ8: --dump-sizes flag */
bool g_dump_sizes = false;
bool g_dump_refine_json = false;
bool g_trail_autoloaded = false;
const char *g_lang_prelude = NULL;   /* saffron-lang-plan S6 / r7rs R1: LangTraits.prelude of the entry file */
uint32_t g_synthetic_user_from_line = 0; /* r7rs-lang-plan R9; see globals.h */
const char *g_repl_start_lang = NULL; /* saffron-lang-plan S8 / r7rs R9: `tur repl --lang <base>`, validated by main.c */
bool g_opt_option_niche = true;   /* graduated 2026-09-03; TUR_OPTION_NICHE=0 restores the tagged monomorph */
bool g_opt_regions = true;       /* graduated 2026-09-05; TUR_REGIONS=0 restores malloc + unbracketed calls */
bool g_opt_reflected_measures = false; /* reflected-measures RF0: --enable=reflected-measures */
bool g_dump_reflect = false;           /* reflected-measures RF5: --dump-reflect */
bool g_opt_r7rs = false;         /* r7rs (graduated 2026-10-01): this build contains an r7rs TU; set by the `#lang r7rs` line, never by a flag.  emit_module.c's r7rs_gc_active reads it to pick the collector opt-out (docs/archive/r7rs-lang-plan.md) */
bool g_opt_repl_jit_inline_c = false; /* aot-compiled-repl-plan C1: `--enable=repl-jit-inline-c` */
bool g_opt_r7rs_gc = true;       /* r7rs-gc (graduated 2026-09-25): a compiled `#lang r7rs` program's allocator is the collector; TUR_R7RS_GC=0 or --no-r7rs-gc opts out (docs/archive/r7rs-gc-plan.md) */
bool g_opt_saffron_gc = true;   /* any-widen-stored-in-an-adt-field-has-no-owner (2026-09-28): a compiled single-unit `#lang saffron` program's allocator is the r7rs-gc collector; TUR_SAFFRON_GC=0 or --no-saffron-gc opts out */
bool g_opt_dynamic_any = false;  /* graduated 2026-09-10 as g_opt_saffron; set by any dynamic-language `#lang` file, not by a flag */
bool g_adt_slab = false;
bool g_sr1_sum_byvalue = true;
bool g_sr2_app_sum_byvalue = true;

/* ER1: --strict-effects flag */
bool g_strict_effects = false;

/* ER6: --dump-effects flag */
bool g_dump_effects = false;

/* G1: --dump-write-frames flag */
bool g_dump_write_frames = false;

/* R4 slice 2: --dump-read-frames flag */
bool g_dump_read_frames = false;

/* CPS2: --dump-cps flag */
bool g_dump_cps = false;

/* Phase I: --emit-abi-trace flag */
bool g_emit_abi_trace = false;

bool g_linear_enabled = true;

bool g_unique_enabled = true;

bool g_substructural_enabled = true;

/* drop-x-flags-plan (v0.24.0): -Xunion-types is now an accept-and-warn
 * no-op; union type syntax and checking is unconditionally on. */
bool g_union_types_enabled = true;

/* drop-x-flags-plan (v0.24.0): -Xintersection-types is now an
 * accept-and-warn no-op; intersection type syntax is unconditionally on. */
bool g_intersection_types_enabled = true;

/* drop-x-flags-plan (v0.24.0): -Xeffect-types is now an accept-and-warn
 * no-op; full effect typing (ET0-ET4, LC0-LC3, MS0-MS4) is unconditionally
 * on.  Note: --strict-effects stays opt-in -- always-on effect types do
 * NOT imply always-on strict warnings. */
bool g_effect_types_enabled = true;

/* CT3: Contract checking configuration */
bool g_contracts_enabled = true;          /* contracts always active by default */
bool g_keep_contracts_in_release = false; /* --keep-contracts: retain in release builds */

bool g_sessions_enabled = true;

/* drop-x-flags-plan (v0.24.0): -Xdynamic-vars is now an accept-and-warn
 * no-op; dynamic var syntax and checking is unconditionally on. */
bool g_dynvar_enabled = true;

/* drop-x-flags-plan (v0.24.0): -Xcallcc is now an accept-and-warn no-op;
 * call/cc / escape are real and unconditionally on (CPS substrate). */
bool g_callcc_enabled = true;

/* drop-x-flags-plan (v0.24.0): -Xsymbols is now an accept-and-warn no-op;
 * first-class runtime symbol (:Sym) values are unconditionally on. */
bool g_symbols_enabled = true;

/* INT-2: --interpret mode — true when running tur --interpret. */
bool g_interpret_mode = false;

/* `tur expand`: dump macro expansions during elaboration. */
bool g_dump_expansion = false;

/* Stage 3: --macro-caps=io -- grant the macro-time env I/O. */
bool g_macro_caps_io = false;
/* security-audit-plan WP3: --no-proc-macros -- refuse every defmacro*. */
bool g_no_proc_macros = false;
bool g_turi_stdlib_preload = false;

/* F4 (cross-plan-followups): --Werror=deprecated promotes deprecation
 * warnings emitted by elab_lookup_sym to errors so a clean build can
 * gate against new uses of deprecated APIs. */
bool g_werror_deprecated = false;

/* Phase C: --Werror=inline-c-narrow-params promotes narrow-param-in-inline-C
 * warnings to errors. */
bool g_werror_inline_c_narrow_params = false;

/* -Werror=strict-effects promotes the --strict-effects lints to errors. */
bool g_werror_strict_effects = false;

/* forall-kinds / forall-constraints / hkt-hrt GRADUATED 2026-07-06 -- always-on;
 * their enable bits and elaboration gates are gone.  See
 * docs/archive/history/constrained-hkt-forall-plan.md. */

/* forall-dict-pass GRADUATED 2026-07-06 -- always-on; runtime dictionary
 * passing for a polymorphic constrained function used as a rank-2 argument
 * (mode B) no longer has an enable bit.  See
 * docs/archive/history/forall-dict-pass-multi-constraint-hkt-plan.md. */

/* hrt-curried-result GRADUATED 2026-07-06 -- always-on; the curried rank-2
 * result-is-a-function elaboration ((l x) yields a callable closure) no longer
 * needs an enable bit.  See docs/archive/history/constrained-hkt-forall-mode-b-plan.md. */

/* vl-wide-functor GRADUATED 2026-07-04 (VBM4 of van-laarhoven-monomorphization-
 * plan) -- a van Laarhoven lens may now focus through a WIDE by-value aggregate
 * functor unconditionally (no flag, TUR-E0309 retired).  The Path A carrier
 * box/unbox bridge is always-on; the wide-ness test (non-opaque, non-:heap
 * flat-product) still gates it, so carrier-compatible functors are untouched.
 * The zero-overhead Path B redirect (formerly --enable=vl-wide-mono) layers on
 * top unconditionally since its 2026-07-05 graduation (see below). */

/* vl-wide-mono GRADUATED 2026-07-05 (CM4 of van-laarhoven-consumer-mono-plan) --
 * the by-value HKT monomorphization (Path B) registration, unique-redirect, and
 * consumer-clone emit are now unconditional; the former `g_opt_vl_wide_mono`
 * enable bit is retired.  SIMPLE lenses (a direct `fmap` tail) redirect to a
 * by-value mono body with no `(f S)`/`(f A)` heap box; COMPOSED lenses fall back
 * to the Path A carrier bridge (mono_specs.c lens_is_simple_for_pathb).  Paired
 * with `--dump-mono-specs` (g_dump_mono_specs) to review the keying. */
bool g_dump_mono_specs = false;

/* --dump-cps-mono: report CPS-subset admissibility of colored-generic
 * monomorphs (G1 of the generic-monomorph-classification plan).  Analysis only. */
bool g_dump_cps_mono = false;

/* g_opt_cps_tramp_resume and g_opt_owning_cloneable_capture RETIRED 2026-08-22.
 * Both experiments graduated in July 2026 and both left their enable bit behind,
 * defined and initialized true.  Neither had an EXPERIMENTS[] row any more, so
 * no CLI, manifest, or user-config path could write either one -- the
 * initializer was the only assignment in the tree, and every read (64 and 5
 * respectively, across eight files) was a constant test with an unreachable
 * arm.  Both are folded to true and the dead arms deleted.  See globals.h. */

/* CG5/CG8 cycle-gc GRADUATED 2026-08-17 -- `(gc-auto!)` is an ordinary call
 * form; the enable bit and the elab_gc_auto gate are gone.  What did NOT change
 * is the default: a program that never calls `(gc-auto!)` still runs the
 * pure-RC path with no collector overhead.  See
 * docs/archive/gc-cycle-collection-followup-plan.md. */

/* J1-J3 jit GRADUATED 2026-08-17 -- `tur jit` needs only -DTUR_JIT=ON; the
 * enable bit is gone.  See docs/archive/jit-engine-plan.md. */

/* closure-drop-glue GRADUATED 2026-07-22 -- the Model R drop-glue header ABI is
 * unconditional; the enable bit and its codegen gates are gone.  See
 * docs/archive/closure-drop-glue-plan.md. */

/* RT0 refined GRADUATED 2026-08-01 -- static discharge of `#refine{...}` is
 * unconditional; the g_opt_refined enable bit and its elaboration gates are
 * gone.  See docs/archive/refined-graduation-plan.md. */

/* sealed-opaque GRADUATED 2026-08-17 -- `(defopaque H :int :sealed)` makes `::`
 * refuse to convert between H and its representation type outside the module
 * that declared H, unconditionally.  The enable bit and its gate are gone.  See
 * docs/archive/sealed-opaque-plan.md. */

/* write-frames GRADUATED 2026-08-20 -- `#writes w` / `#writes [a b]` declares
 * which of a function's arguments its body may write, and WF2 now CHECKS every
 * declared frame unconditionally (VERIFIED / EXCEEDED -> TUR-E0382 /
 * UNVERIFIED).  The enable bit and its gates are gone.  See
 * docs/archive/checked-write-frames-plan.md. */

/* checked-reads GRADUATED 2026-08-20 -- the #reads congruence override is
 * refused on positive broken-promise evidence, unconditionally.  The enable bit
 * and its gates are gone.  See docs/archive/trusted-refinement-claims-plan.md
 * (R2). */

/* jit-ffi GRADUATED 2026-08-21 -- call-ptr / callback-ptr are ordinary
 * `unsafe` forms.  The enable bit and its two elaboration gates are gone; the
 * `-DTUR_JIT=ON` build gate on the interpreter path is untouched.  See
 * docs/archive/jit-ffi-c2mir-plan.md. */

/* --strict-refine: hard-fail on any obligation the chain could not prove. */
bool g_strict_refine = false;


/* ---------------------------------------------------------------------------
 * Interpreter-native return-type signature registry (see globals.h).
 * --------------------------------------------------------------------------- */
typedef struct TurNativeSig {
    char            *name;   /* owned (strdup) */
    TurNativeRetType ret;
} TurNativeSig;

static TurNativeSig *g_native_sigs;
static size_t        g_native_sigs_count;
static size_t        g_native_sigs_cap;

void tur_native_sig_register(const char *name, TurNativeRetType ret) {
    if (!name) return;
    for (size_t i = 0; i < g_native_sigs_count; i++) {
        if (strcmp(g_native_sigs[i].name, name) == 0) {
            g_native_sigs[i].ret = ret;
            return;
        }
    }
    if (g_native_sigs_count == g_native_sigs_cap) {
        size_t nc = g_native_sigs_cap ? g_native_sigs_cap * 2u : 8u;
        TurNativeSig *grown =
            (TurNativeSig *)realloc(g_native_sigs, nc * sizeof(TurNativeSig));
        if (!grown) return;
        g_native_sigs     = grown;
        g_native_sigs_cap = nc;
    }
    TurNativeSig *e = &g_native_sigs[g_native_sigs_count++];
    e->name = strdup(name);
    e->ret  = ret;
}

bool tur_native_sig_lookup(const char *name, TurNativeRetType *ret) {
    if (!name) return false;
    for (size_t i = 0; i < g_native_sigs_count; i++) {
        if (strcmp(g_native_sigs[i].name, name) == 0) {
            if (ret) *ret = g_native_sigs[i].ret;
            return true;
        }
    }
    return false;
}

void tur_native_sig_clear(void) {
    for (size_t i = 0; i < g_native_sigs_count; i++) free(g_native_sigs[i].name);
    free(g_native_sigs);
    g_native_sigs       = NULL;
    g_native_sigs_count = 0;
    g_native_sigs_cap   = 0;
}
