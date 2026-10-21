/* pipeline.c -- the compile pipeline: the core passes, the three compile
 * entry points (compile_to_c, compile_to_h, compile_to_implementation), the
 * stdlib and project prelude, the split-build switches, and the LSP's two
 * entry points (tur_check_only, tur_collect_symbols).
 *
 * Part of the compiler driver (src/driver/); driver.h says how the files
 * share declarations. */

#include "driver.h"

#include "arena.h"
#include "borrow_check.h"  /* Phase 14 */
#include "cps.h"          /* Phase 18: CPS transformation */
#include "srfi_prune.h"   /* r7rs-srfi-plan S3: unreached SRFI definitions */
#include "cps_ir.h"       /* CPS2: ANF/CPS IR (--dump-cps) */
#include "effect_check.h" /* Phase P19-2: effect-row inference */
#include "effect.h"       /* built-in effect registration */
#include "kind_check.h"   /* Phase HKT H0: kind inference pass */
#include "compiler/emit_split.h" /* r7rs-programs-compile-slowly */
#include "effect_lower.h" /* Phase 19: Effect lowering */
#include "forms.h"
#include "pass.h"         /* Phase P19-1: pass scheduling */
#include "prof.h"         /* profiling-plan C0+C1: --timings */
#include "reader.h"
#include "reader_macros.h"
#include "refine_discharge.h" /* RT3: per-compile refinement stats reset */
#include "span_audit.h"  /* debugger Phase 1: breakpoint-span coverage audit */
#include "symbols.h"
#include "globals.h"
#include "mono_specs.h"   /* VBM1: --dump-mono-specs registry dump */
#include "lsp/lsp_sym.h"
#include "lsp/lsp_collect.h"
#include "lsp/lsp_scope.h"
#include "stdlib_autoload.h"

static Form **load_project_prelude_impl(Arena *arena, SymbolTable *st,
                                         Form **user_forms, uint32_t nuser,
                                         uint32_t *stdlib_count_out);

/* Phase HKT-P6: --dump-kinds flag: print kind annotations after kind-check */
bool g_dump_kinds = false;

/* debugger Phase 1: `tur audit-spans` mode.  When set, run_core_passes stops
 * after PASS_ELABORATE and audits the program for breakpoint-eligible nodes
 * lacking a usable source span; the hole count lands in g_audit_span_holes. */
bool g_audit_spans = false;
int  g_audit_span_holes = 0;

/* Ordered list of compiler passes executed for every compilation unit.
 * A new pass is named in four places:
 *   1. its PassKind enumerator in pass.h;
 *   2. its entry here, in pipeline order;
 *   3. its arm in pass_zone() below (plus a ProfZone in prof.h if the pass
 *      should be timed on its own) -- a missing arm is a -Wswitch error;
 *   4. its arm in run_core_pass() below, which does the work.              */
static const PassKind core_passes[] = {
    PASS_ELABORATE,
    PASS_KIND_CHECK,
    PASS_EFFECT_LOWER,
    PASS_EFFECT_ROW_INFER,  /* effect_check_pass: effect-row inference and checking */
    PASS_CPS,
    PASS_BORROW_CHECK,
};
static const int n_core_passes = (int)(sizeof(core_passes) / sizeof(core_passes[0]));

/* --timings (profiling-plan 3.3): the zone each core pass is charged to.  No
 * default arm on purpose -- a new PassKind without a zone is a -Wswitch error
 * here, not a pass whose time silently lands in whatever zone encloses it. */
static ProfZone pass_zone(PassKind k) {
    switch (k) {
    case PASS_ELABORATE:        return PROF_ELAB;
    case PASS_KIND_CHECK:       return PROF_KIND;
    case PASS_EFFECT_LOWER:     return PROF_EFFECT_LOWER;
    case PASS_EFFECT_ROW_INFER: return PROF_EFFECT_ROW;
    case PASS_CPS:              return PROF_CPS;
    case PASS_BORROW_CHECK:     return PROF_BORROW;
    }
    return PROF_OTHER;
}

/* One core pass.  Returns 0 to continue, 1 on failure, 2 to stop the
 * pipeline successfully (audit-spans mode, after elaboration). */
static int run_core_pass(PassContext *ctx, PassKind kind) {
        switch (kind) {
        case PASS_ELABORATE:
            ctx->prog = elaborate_program(ctx->arena, ctx->st,
                                          ctx->forms, ctx->nforms,
                                          ctx->stdlib_prefix,
                                          ctx->module_base_dir,
                                          ctx->separate_compilation,
                                          /*sandboxed=*/false,
                                          &ctx->tc_env,
                                          ctx->include_dirs,
                                          ctx->n_include_dirs,
                                          NULL,
                                          ctx->reader_macros);
            if (!ctx->prog || diag_had_error()) return 1;
#ifndef NDEBUG
            /* Phase HKT-P6: verify kind info is preserved after elaboration */
            assert(kind_verify_program(ctx->prog) && "Kind info cleared after PASS_ELABORATE");
#endif
            /* VBM1/VBM2 (van-laarhoven-monomorphization-plan): resolve each
             * abstract lens spec discovered during elaboration to the concrete
             * lens passed at its enclosing fn's top-level invocations (VBM2's
             * cross-procedural collapse -- plan OQ #1/#2), then --dump-mono-specs
             * prints both the abstract and resolved-concrete registries.
             * Resolves abstract lens specs to concrete ones; the emitter
             * redirects to the by-value `<lens>__mono_<hash>` bodies (see
             * mono_specs.h).  Graduated (vl-wide-mono, 2026-07-05):
             * the resolve pass is unconditional; it no-ops when no wide-functor
             * lens pin populated the registry during elaboration. */
            prof_enter(PROF_MONO);
            mono_specs_resolve_program(ctx->prog);
            prof_exit(PROF_MONO);
            if (g_dump_mono_specs) {
                mono_specs_dump(stdout);
                /* Harmless: cmd_run also flushes before system(), so a dump
                 * precedes the program's output. */
                fflush(stdout);
            }
            /* debugger Phase 1: audit breakpoint-span coverage on the freshly
             * elaborated tree, before any transform pass introduces synthetic
             * (legitimately span-less) nodes.  Stop the pipeline afterwards --
             * audit mode never emits. */
            if (g_audit_spans) {
                g_audit_span_holes = span_audit_program(ctx->prog, stdout);
                return 2;
            }
            break;
        case PASS_KIND_CHECK:
            /* Phase HKT H0: kind inference and validation pass. */
            if (kind_check_pass(ctx->arena, ctx->prog) != 0)
                return 1;
            /* Phase HKT-P6: optionally dump kind annotations for debugging. */
            if (g_dump_kinds)
                kind_dump_program(ctx->prog, stdout);
#ifndef NDEBUG
            /* Phase HKT-P6: verify kind info is preserved after kind-check */
            assert(kind_verify_program(ctx->prog) && "Kind info cleared after PASS_KIND_CHECK");
#endif
            break;
        case PASS_EFFECT_LOWER:
            /* Builds the EffectEnv (builtin Unsafe and the capabilities) that
             * later passes read.  effect_lower is an identity walk -- the
             * CPS/DK backend lowers effects. */
            ctx->effect_env = effect_env_new(ctx->arena);
            effect_env_register_builtin_unsafe(
                ctx->effect_env, ctx->arena,
                symtab_intern(ctx->st, strslice(EFFECT_NAME_UNSAFE, 6)));
            effect_env_register_builtin_capabilities(ctx->effect_env, ctx->arena,
                                                     ctx->st);
            ctx->prog = effect_lower(ctx->arena, ctx->st,
                                     ctx->prog, ctx->effect_env);
            if (!ctx->prog || diag_had_error()) return 1;
#ifndef NDEBUG
            /* Phase HKT-P6: verify kind info preserved after effect-lower */
            assert(kind_verify_program(ctx->prog) && "Kind info cleared after PASS_EFFECT_LOWER");
#endif
            break;
        case PASS_EFFECT_ROW_INFER:
            /* P19-2: effect-row inference and validation pass. */
            effect_check_set_separate_compilation(ctx->separate_compilation);
            if (effect_check_pass(ctx->arena, ctx->prog, ctx->effect_env) != 0)
                return 1;
            /* ER6: --dump-effects: print inferred effect row for each defn. */
            if (g_dump_effects)
                effect_check_dump_effects(ctx->prog, stdout);
#ifndef NDEBUG
            /* Phase HKT-P6: verify kind info preserved after effect-row-infer */
            assert(kind_verify_program(ctx->prog) && "Kind info cleared after PASS_EFFECT_ROW_INFER");
#endif
            break;
        case PASS_CPS:
            /* CPS1 (cps-transform-plan): --dump-cps-coloring runs the
             * whole-program may-capture coloring on the pre-transform tree and
             * prints the colored/uncolored partition. */
            if (g_dump_cps_coloring)
                cps_dump_coloring(ctx->arena, ctx->prog, stdout);
            /* CPS2 (cps-transform-plan): --dump-cps prints the CT-IR the CPS
             * backend emits from, for every colored function. */
            if (g_dump_cps)
                cps_ir_dump_program(ctx->arena, ctx->prog, stdout);
            /* Phase 18: CPS transformation for shift/reset. */
            ctx->prog = cps_transform(ctx->arena, ctx->prog, &ctx->tc_env);
            if (!ctx->prog || diag_had_error()) return 1;
            /* Phase B5: --dump-clone-plan: print cloneable capture plan after CPS */
            if (g_dump_clone_plan) cps_dump_clone_plan(ctx->prog, stderr);
            /* CPS1: --dump-cps-coloring: print colored/uncolored partition */
            if (g_dump_cps_coloring) cps_dump_cps_coloring(ctx->prog, stderr);
#ifndef NDEBUG
            /* Phase HKT-P6: verify kind info preserved after CPS */
            assert(kind_verify_program(ctx->prog) && "Kind info cleared after PASS_CPS");
#endif
            break;
        case PASS_BORROW_CHECK:
            /* Phase 14: ownership, move, and borrow analysis. */
            if (!borrow_check_program(ctx->prog)) return 1;
            /* Phase P19-7: Always-on check that handler case bodies do not
             * capture borrow-typed variables from the enclosing scope. */
            if (!borrow_check_effect_handler_captures(ctx->prog)) return 1;
            /* TY4: Always-on lifetime pass -- elision + outlives cycle check. */
            if (!lifetime_check_program(ctx->prog)) return 1;
#ifndef NDEBUG
            /* Phase HKT-P6: verify kind info preserved after borrow-check */
            assert(kind_verify_program(ctx->prog) && "Kind info cleared after PASS_BORROW_CHECK");
#endif
            break;
        }
    return 0;
}

/* Run all core compiler passes in order.  ctx->forms/nforms must be set
 * before calling; ctx->prog and ctx->effect_env are populated by the passes.
 * Returns 0 on success, 1 on the first pass failure.                        */
static int run_core_passes(PassContext *ctx) {
    for (int i = 0; i < n_core_passes; i++) {
        ProfZone z = pass_zone(core_passes[i]);
        prof_enter(z);
        int rc = run_core_pass(ctx, core_passes[i]);
        prof_exit(z);
        if (rc == 1) return 1;
        if (rc == 2) return 0;
    }
    return 0;
}

/* J2: when non-NULL, compile_to_c also appends the exports manifest of the
 * compiled program here (set only by the REPL's in-process spice build). */
Buf *g_manifest_sink = NULL;

/* r7rs-programs-compile-slowly (src/compiler/emit_split.h): when non-NULL and
 * the program is a `#lang r7rs` one, compile_to_c writes it as TWO units --
 * the library unit (the runtime preamble and the auto-loaded stdlib, the
 * prelude among them) here, and the program unit into its usual output.  Left
 * empty when the split declines; the output is then one unit as always.  Set
 * only by cmd_build, which compiles the library unit once and caches it. */
Buf *g_split_lib_sink = NULL;
/* r7rs-imported-libraries-recompiled-every-build: when non-NULL as well, a
 * program that imports a library from the stdlib -- `(srfi 13)`, `(scheme
 * eval)` -- is written as THREE units, the imported libraries' definitions
 * here (emit_split.h).  Left empty when the program imports none, or when
 * that unit declines; the build is then the two units above. */
Buf *g_split_imp_sink = NULL;

/* Does a split build write the imported-library unit?  Wherever the split
 * applies; TUR_IMPORT_SPLIT=0 turns it off (the imported libraries are then
 * the program's, as they were). */
bool g_import_split_failed = false;   /* cmd_build: this build's three units did not link */
/* How many names the imported-library unit exports (compile_to_c), for
 * import_split_pieces. */
static size_t g_split_imp_exports = 0;

/* compile_to_c: did this compile's library unit come from the cache? */
bool g_split_lib_from_cache = false;

/* The imported-library unit is compiled in pieces (prelude_pieces) only when
 * it is big.  Every piece compiles the whole runtime preamble the unit
 * carries, ~0.5 s of cc at -O2 whatever the unit defines, so for a small SRFI
 * the pieces buy ~0.5 s of wall time for ~1.4 s more CPU -- the wrong trade
 * for a cold fixture suite, which runs a build per core.  SRFI 13's unit
 * (~300 names) and (scheme eval)'s (~175, compiled with ASan under a Debug
 * `tur`: 8.5 s whole, 6.9 s in four) gain from them; SRFI 1's (~120) and
 * (scheme read)'s (~75) do not. */
#define IMPORT_SPLIT_PIECES_MIN_EXPORTS 150
int import_split_pieces(void) {
    return g_split_imp_exports >= IMPORT_SPLIT_PIECES_MIN_EXPORTS ? prelude_pieces() : 1;
}
static bool import_split_applies(void) {
    const char *e = getenv("TUR_IMPORT_SPLIT");
    return !(e && strcmp(e, "0") == 0) && !g_import_split_failed;
}

/* Does this compile get a split build?  `#lang r7rs` only: that is where the
 * prelude dominates the build (a one-line program reaches ~160 of its
 * functions through the uncaught-error printer alone).  64-bit hosts only --
 * the library unit's fat boxes are pointer words (ensure_fatbox_keep).  Not
 * under --debug, whose `#line` spans would point into a unit that was
 * compiled for another program.  TUR_PRELUDE_SPLIT=0 turns it off.
 *
 * On by default on Linux and macOS; elsewhere TUR_PRELUDE_SPLIT=1 opts in.  On
 * Windows the two units link, but the keyword records they share (SYM2,
 * `__attribute__((weak)) const`) come out pointing at the wrong bytes and
 * quoted symbols print as other strings
 * (docs/reported/r7rs-prelude-split-wrong-symbols-on-windows.md).
 *
 * macOS was off for the same kind of reason until 2026-09-28: a Scheme value
 * kept only in a Turmeric map was collected under it, because Mach-O runs
 * initializers in link order and ignores the collector's `constructor(101)`
 * across object files, so the program unit allocated archive memory before the
 * library unit's copy installed the collector's allocator hook.  The hook is
 * now installed from __tur_static_init as well, which is ahead of every band
 * on every platform (docs/archive/r7rs-prelude-split-gc-seam-on-macos.md). */
static bool prelude_split_applies(void) {
    const char *e = getenv("TUR_PRELUDE_SPLIT");
    if (e && strcmp(e, "0") == 0) return false;
#if !defined(__linux__) && !defined(__APPLE__)
    if (!e || strcmp(e, "1") != 0) return false;
#endif
    if (sizeof(void *) != 8 || g_emit_debug_lines || g_manifest_sink) return false;
    return g_lang_prelude && strcmp(g_lang_prelude, "r7rs/prelude.tur") == 0;
}

/* DEDUP-4b (docs/archive/gc-cycle-collection-plan.md): resolve whether the emitted preamble
 * should DECLARE the rc<T>/GC runtime -- because libturt_runtime.a will supply
 * it -- instead of defining its own hand-written replica, and tell the emitter
 * before codegen runs.
 *
 * ON by default, but ONLY when `tur` is the one doing the linking.  A preamble
 * that declares without defining is not self-contained C, and bare
 * `tur emit-c` exists precisely to hand someone a translation unit they will
 * build themselves -- so that path keeps the definitions (and its snapshots
 * keep matching).  `tur build` / project builds control their own link line, so
 * they take the archive.  g_emit_for_link is what distinguishes the two.
 *
 * Also requires an actual archive: without one the program would link nothing
 * at all, so a failed probe silently keeps the emitted definitions.  That is
 * what makes this safe to default -- a toolchain with no archive still builds,
 * it just keeps running the replica.
 *
 * TUR_RCGC_FROM_ARCHIVE=0 forces the replica back (escape hatch if an archive
 * link ever misbehaves); =1 forces the archive even for bare `emit-c`, for
 * someone who links libturt_runtime.a from their own build system.
 *
 * Safe to call before the link step decides anything: locate_runtime_lib is a
 * pure filesystem probe with no side effects. */
void resolve_rcgc_from_archive(void) {
    const char *opt = getenv("TUR_RCGC_FROM_ARCHIVE");
    bool forced_on = opt && strcmp(opt, "1") == 0;

    if (opt && strcmp(opt, "0") == 0)     { emit_set_rcgc_from_archive(false); return; }
    if (g_runtime_mode == TUR_RT_SOURCE)  { emit_set_rcgc_from_archive(false); return; }
    /* wasm-build-takes-native-archive-posture: a locatable archive is a
     * NATIVE libturt_runtime.a, which emcc cannot link and is never given, so
     * a wasm build keeps the replica -- TUR_RCGC_FROM_ARCHIVE=1 included. */
    if (g_emit_for_wasm)                  { emit_set_rcgc_from_archive(false); return; }
    if (!g_emit_for_link && !forced_on)   { emit_set_rcgc_from_archive(false); return; }

    char libdir[4096], libname[128];
    int found = locate_runtime_lib(libdir, sizeof(libdir), libname, sizeof(libname));
    emit_set_rcgc_from_archive(found && strcmp(libname, "turt_runtime") == 0);
}

/* cc-path-preamble-split-plan step 5: does a DEFAULT build (--runtime=auto)
 * swap the fixed runtime preamble for the decls region and link
 * libturt_preamble.a?  Measured at 81-86s -> 73s on a tenth of the suite
 * (Linux, no ccache) and ~10% on the Windows CI shards: the cc call for a
 * one-line program halves, and nothing else moves.
 *
 * Every precondition here is a case where the split would build something
 * other than what was asked for, so each one keeps the whole preamble instead:
 *
 *   - both archives side by side.  The decls region declares the rc<T>/GC
 *     runtime in its archive posture, so the lean libturt_runtime.a has to be
 *     on the line as well as libturt_preamble.a -- and a `--target tur` build
 *     produces neither.  emit_rcgc_from_archive() is the same decision for
 *     this program's own preamble; if that kept the replica, so does this.
 *   - no sanitizer in TUR_CC_FLAGS.  The archive is compiled without one, so
 *     ASan would stop seeing the preamble's heap traffic and TSan its
 *     atomics -- a silent loss of exactly the coverage that build asked for.
 *   - not --debug, whose `-g -Og` build should step through the preamble the
 *     user can see, not an -O2 archive.  Same reason prelude_split_applies
 *     stays out of it.
 *   - Linux and Windows only, where CI ran the whole suite under the split for
 *     weeks before it became the default (the retired `split` and
 *     `windows-split` jobs; `test` and `windows` run it now).  macOS never
 *     has; TUR_PREAMBLE_SPLIT=1 opts in there, as TUR_RUNTIME=split always
 *     could.
 *
 * TUR_PREAMBLE_SPLIT=0 turns it off.  The swap itself can still decline (the
 * hash guard in jit_try_split_preamble, or a `#lang r7rs` program, whose
 * preamble differs); under AUTO that is quiet, because the whole preamble is
 * the correct answer then, not a degraded one. */
bool preamble_split_auto_applies(void) {
    const char *e = getenv("TUR_PREAMBLE_SPLIT");
    if (e && strcmp(e, "0") == 0) return false;
#if !defined(__linux__) && !defined(_WIN32)
    if (!e || strcmp(e, "1") != 0) return false;
#endif
    if (g_emit_debug_lines) return false;
    const char *f = getenv("TUR_CC_FLAGS");
    if (f && strstr(f, "-fsanitize")) return false;
    if (!emit_rcgc_from_archive()) return false;
    char libdir[4096], libname[128];
    if (!locate_runtime_lib(libdir, sizeof(libdir), libname, sizeof(libname)) ||
        strcmp(libname, "turt_runtime") != 0)
        return false;
    char probe[4200];
    struct stat st;
    snprintf(probe, sizeof(probe), "%s/libturt_preamble.a", libdir);
    return stat(probe, &st) == 0 && S_ISREG(st.st_mode);
}

/* -------------------------------------------------------------------------
 * LSP symbol collection (LD1)
 * --------------------------------------------------------------------- */

/* The real path of the document compile_to_c is analysing from a scratch copy
 * (tur_collect_symbols), or NULL.  Resolution that is relative to the ENTRY
 * FILE's directory -- a sibling `(import mod)`, a `#use-reader-macros "x.tur"`
 * -- has to start from the document, not from the temp directory the scratch
 * copy sits in.  `load` is cwd-relative and is unaffected either way. */
static const char *g_logical_entry_path = NULL;

int tur_collect_symbols(const char *path, const char *logical_path,
                        LspSymbol *out, int cap, int *count_out) {
    lsp_collect_begin(out, cap, count_out);

    /* Resolve the spice from where the buffer LIVES, not from where it was
     * written.
     *
     * The LSP writes the editor's text to a scratch file and analyses that,
     * so walk-up discovery from the scratch path finds no build.tur -- and a
     * module inside a spice came back with every `(import sibling)`
     * unresolved, no symbol index at all, and therefore no completion, no
     * go-to-definition, and a rename that refused because it could not see a
     * definition. `tur check` on the same file has always worked, because it
     * walks up from the real one. This is the difference.
     *
     * NULL means the path IS the real one (the MCP server, the stdlib prime
     * on an empty scratch file), which is the pre-existing behaviour. */
    const char *anchor = (logical_path && *logical_path) ? logical_path : path;

    char **inc = NULL;
    int    n_inc = 0;
    char **owned = NULL;
    int    n_owned = 0;
    Ls2ResolverCtx ls2;
    auto_append_spice_includes(anchor, &inc, &n_inc, &owned, &n_owned, &ls2);

    Buf discard;
    buf_init(&discard);
    int rm_n = 0;
    char **rm_p = discover_manifest_reader_macros(anchor, &rm_n);
    ls2_resolver_ctx_set(&ls2);
    /* lsp-sibling-import-resolves-against-scratch-dir: spice includes were
     * already anchored on the real path (above); the entry file's own
     * directory has to be too. */
    g_logical_entry_path = (logical_path && *logical_path) ? logical_path : NULL;
    int rc = compile_to_c(path, &discard, (const char **)inc, n_inc,
                          (const char **)rm_p, rm_n);
    g_logical_entry_path = NULL;
    ls2_resolver_ctx_set(NULL);
    ls2_resolver_ctx_dispose(&ls2);
    free_reader_macro_paths(rm_p, rm_n);
    buf_free(&discard);

    for (int i = 0; i < n_owned; i++) free(owned[i]);
    free(owned);
    free(inc);
    lsp_collect_end();
    return rc;
}

/* RP1: keep only the exports.manifest lines appended since `from` whose C
 * symbol the emitted unit `c_src` defines.  emit_exports_manifest lists every
 * exported defn, but a generic one with no symbol of its own (a phantom
 * `[n]` length, a `[^&cols]` row: only its monomorphs are emitted, under
 * their own names) left the loader a line it could not dlsym -- and the
 * loader rejects the whole library as "stale exports.manifest", so no export
 * of frame or linalg could be bound at all.  An unlisted export stays the
 * host's to interpret. */
static void manifest_keep_defined(Buf *manifest, size_t from, const Buf *c_src) {
    if (!manifest || !c_src || !c_src->data || from >= manifest->len) return;
    Buf kept;
    buf_init(&kept);
    const char *p = manifest->data + from, *end = manifest->data + manifest->len;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *le = nl ? nl + 1 : end;
        const char *arrow = strstr(p, " -> ");
        const char *colons = arrow ? strstr(arrow, " :: ") : NULL;
        bool keep = true;
        if (arrow && colons && colons < le) {
            const char *m = arrow + 4;
            size_t ml = (size_t)(colons - m);
            keep = false;
            for (const char *q = c_src->data; (q = strstr(q, "(")) != NULL; q++) {
                if ((size_t)(q - c_src->data) < ml) continue;
                const char *s0 = q - ml;
                if (memcmp(s0, m, ml) != 0) continue;
                if (s0 > c_src->data && (isalnum((unsigned char)s0[-1]) || s0[-1] == '_'))
                    continue;
                keep = true;
                break;
            }
        }
        if (keep) buf_write(&kept, p, (size_t)(le - p));
        p = le;
    }
    manifest->len = from;
    if (kept.len) buf_write(manifest, kept.data, kept.len);
    buf_free(&kept);
}

/* Reads a .tur file and emits its C source into `out_c`. Returns 0 on success,
 * nonzero on error (diagnostics already emitted).
 * include_dirs/n_include_dirs: additional module search paths for (import ...).
 * reader_macro_paths/n_reader_macro_paths: RM4 — absolute paths to
 * `(reader-macros/define ...)` definition files that are preloaded into
 * the reader's macro registry before the entry file is parsed. Typically
 * derived from the project's `build.tur :reader-macros [...]` entry. */
/* Thin adapter over the shared prepend in compiler/stdlib_autoload.c.
 * The list, the read loop, and the form splice moved there so the WASM
 * playground's in-process analyzer prepends exactly the same stdlib this
 * does; what stays here is the CLI's own two answers -- where the stdlib
 * lives (resolve_stdlib_root, an exe-relative walk-up that means nothing in
 * a browser) and whether --no-auto-stdlib was passed. */
static uint32_t prepend_stdlib_forms(Arena *arena, SymbolTable *st,
                                     const char *entry_path,
                                     Form ***forms_in_out,
                                     uint32_t *nforms_in_out,
                                     uint8_t *file_id_in_out) {
    /* --timings: the auto-loaded stdlib is module loading; its parse inside
     * is "read" and its elaboration later is "elaborate". */
    prof_enter(PROF_MODULE_LOAD);
    uint32_t n = tur_stdlib_prepend_forms(arena, st, resolve_stdlib_root(),
                                          entry_path, g_no_auto_stdlib,
                                          forms_in_out, nforms_in_out,
                                          file_id_in_out);
    prof_exit(PROF_MODULE_LOAD);
    /* The auto-loaded files took ids [1, *file_id_in_out); an import or a
     * `(load ...)` must be numbered past them (elab_core.c). */
    if (file_id_in_out) diag_note_autoload_file_ids(*file_id_in_out);
    return n;
}

/* Start a compilation unit: compile_to_c, compile_to_h and
 * compile_to_implementation each call this first.  Clear the global diagnostic
 * state (the `had_error_` flag and the file registry) before registering the
 * unit's file so a prior failed compile in the same process cannot poison this
 * one.  Without this, batch drivers that loop compile_to_c in-process
 * (`tur test <dir>`, `tur check <dir>`) would mark every alphabetically-later
 * file as FAIL once any earlier file failed to compile -- the stale
 * `had_error_` short-circuits the `diag_had_error()` gate.  A new per-unit
 * state is reset here, once, for all three. */
static void begin_compile_unit(void) {
    diag_reset();
    experiment_reset_warnings();  /* XF2: once-per-compile TUR-W006x dedup */
    pkg_tur_version_reassert();   /* :tur-version floor survives the reset above */
    pkg_manifest_reassert();      /* a broken build.tur likewise: error: + exit 0 is not an error */
    refine_discharge_reset();     /* RT3: once-per-compile refinement stats */
}

int compile_to_c(const char *path, Buf *out_c,
                  const char **include_dirs, int n_include_dirs,
                  const char **reader_macro_paths,
                  int n_reader_macro_paths) {
    char  *src = NULL;
    size_t len = 0;
    if (drv_read_entire_file(path, &src, &len) != 0) return 2;

    /* Detect language and adjust source for #lang directive */
    const char *src_adj = src;
    size_t len_adj = len;
    LangDialect lang_dialect = LANG_TURMERIC;
    ReaderType reader_type = detect_and_adjust_lang(path, src, len, &src_adj, &len_adj, &lang_dialect);

    /* Each compile_to_c call is a self-contained compilation unit. */
    begin_compile_unit();

    /* The directory entry-relative paths resolve against: the scratch copy's
     * own directory is meaningless (see g_logical_entry_path). */
    char base_dir[4096];
    drv_dir_of_path(g_logical_entry_path ? g_logical_entry_path : path,
                    base_dir, sizeof(base_dir));

    SourceFile file = {0};
    file.path = path;
    file.base_dir = g_logical_entry_path ? base_dir : NULL;
    file.src = src_adj;
    file.len = len_adj;
    /* How much of the file `src_adj` skipped -- the `#lang` line. Span
     * offsets index src_adj; anything that seeks into the file on disk (the
     * LSP's scope table) has to add this back. */
    file.head_offset = (size_t)(src_adj - src);
    file.file_id = 0;
    file.reader_type = reader_type;
    file.lang        = lang_dialect;
    diag_register_file(&file);

    Arena arena;
    arena_init(&arena, 0);
    SymbolTable st;
    symtab_init(&st, &arena);

    /* RM4: preload spice-manifest `:reader-macros [...]` into a registry
     * the entry-file reader will see. */
    ReaderMacroRegistry reader_macros_reg;
    reader_macros_init(&reader_macros_reg, &arena);
    /* Transitive-RM decision #2: batch compile is strict -- duplicate
     * `(reader-macros/define ...)` is a hard error. REPL leaves this
     * flag false on env->reader_macros for iterative-redefinition UX. */
    reader_macros_reg.strict = true;
    for (int i = 0; i < n_reader_macro_paths; ++i) {
        if (reader_macros_load_file(&arena, &st,
                                    reader_macro_paths[i],
                                    &reader_macros_reg) != 0) {
            arena_free(&arena);
            symtab_free(&st);
            free(src);
            return 1;
        }
    }

    uint32_t nforms = 0;
    Form **forms = read_all_with_registry(&arena, &st, &file,
                                          &reader_macros_reg, &nforms);

    /* Phase 7: prepend stdlib autoload forms.  Shared with compile_to_h via
     * prepend_stdlib_forms so project-mode builds see the same stdlib API
     * (Cons / Option / Result / typeclass stubs / etc.) that single-file
     * builds do.  See docs/archive/history/project-mode-no-stdlib-autoload.md. */
    uint8_t file_id = 1;
    uint32_t total_stdlib_forms = prepend_stdlib_forms(&arena, &st, path,
                                                       &forms, &nforms,
                                                       &file_id);

    int rc = 0;
    if (!forms || diag_had_error()) {
        rc = 1;
    } else {
        PassContext ctx = {0};
        ctx.arena = &arena;
        ctx.st    = &st;
        ctx.forms  = forms;
        ctx.nforms = nforms;
        ctx.stdlib_prefix = total_stdlib_forms;
        ctx.module_base_dir = base_dir;
        ctx.include_dirs    = include_dirs;
        ctx.n_include_dirs  = n_include_dirs;
        ctx.reader_macros   = &reader_macros_reg;
        rc = run_core_passes(&ctx);
        /* Collect symbols whether or not later passes failed -- elaboration
         * may have succeeded even when borrow-check reports errors. */
        if (lsp_collect_active() && ctx.prog)
            lsp_collect_program(ctx.prog);
        /* Locals ride the same hook as globals, for the reason lsp_collect.h
         * gives about there being exactly one walk: a second traversal is a
         * second thing to keep in agreement with the elaborator. */
        if (lsp_scope_active() && ctx.prog)
            lsp_scope_program(ctx.prog);
        if (g_audit_spans) {
            /* Audit-only mode: never emit.  A clean audit is rc 0; remaining
             * holes surface as rc 3 (distinct from rc 1 = elaboration failure,
             * rc 2 = file error) so harnesses can tell "audited, found holes"
             * apart from "could not audit". */
            if (rc == 0 && g_audit_span_holes > 0) rc = 3;
        } else if (rc == 0) {
            /* --timings: everything from here to the end of the block is
             * producing C -- the SRFI prune, the split units, the manifest. */
            prof_enter(PROF_EMIT);
            /* DEDUP-4b: decide before emitting -- the preamble's text depends
             * on whether the archive will supply the rc<T>/GC runtime. */
            resolve_rcgc_from_archive();
            /* r7rs-srfi-plan S3: the whole program is here now, so an SRFI
             * definition nothing reaches can go before it costs a C function
             * (and a startup fat box).  Exports stay when a manifest of them
             * is being written.
             *
             * r7rs-imported-libraries-recompiled-every-build: before the
             * split, too, although that makes an imported SRFI's unit (below)
             * this program's rather than the library's -- cached for every
             * rebuild of the program, but not shared with another that
             * reaches other SRFI procedures.  Keeping all of SRFI 13 for the
             * sake of sharing cost a one-line program ~1.9 s more of Debug
             * `tur` emission per build than its pruned unit: more than a
             * cold compile of that unit, paid on every build instead of once.
             * A library that is not pruned ((scheme eval), (scheme read)) has
             * one unit for every program that imports it. */
            srfi_prune_program(ctx.arena, ctx.prog, g_manifest_sink != NULL);
            const bool split_try = g_split_lib_sink && prelude_split_applies();
            bool use_imp = split_try && g_split_imp_sink && import_split_applies() &&
                           emit_split_program_imports(ctx.prog);
            bool split_done = false;
            if (split_try) {
                /* The library unit first, then the imported-library unit:
                 * each records what the units after it declare instead of
                 * defining. */
                Buf lib; buf_init(&lib);
                Buf imp; buf_init(&imp);
                Buf cli; buf_init(&cli);
                SplitLibCache slc;
                split_lib_cache_init(&slc, ctx.prog, path);
                for (;;) {
                    emit_split_set_imp(use_imp);
                    emit_split_set_mode(EMIT_SPLIT_LIB);
                    /* The library unit's text is renamed by its own exports
                     * alone, so it is the same text whatever is imported --
                     * and the same for every program (SplitLibCache). */
                    Buf lib_r; buf_init(&lib_r);
                    const bool cached = split_lib_cache_restore(&slc, ctx.prog, &lib_r);
                    bool ok = cached;
                    if (!cached) {
                        ok = emit_program(&lib, ctx.prog) == 0 && !emit_split_refusal();
                        if (ok) emit_split_rename(lib.data, lib.len, &lib_r);
                        if (ok) split_lib_cache_record(&slc, ctx.prog, &lib_r);
                    }
                    size_t lib_exports = emit_split_exports_count();
                    if (ok && use_imp) {
                        emit_split_set_mode(EMIT_SPLIT_IMP);
                        ok = emit_program(&imp, ctx.prog) == 0 && !emit_split_refusal();
                        g_split_imp_exports = emit_split_exports_count() - lib_exports;
                        if (ok && getenv("TUR_SHOW_CC"))
                            fprintf(stderr, "tur: imported-library unit: %zu names, %d piece(s)\n",
                                    g_split_imp_exports, import_split_pieces());
                    }
                    if (ok) {
                        emit_split_set_mode(EMIT_SPLIT_CLIENT);
                        ok = emit_program(&cli, ctx.prog) == 0 && !emit_split_refusal();
                    }
                    const char *why = emit_split_refusal();
                    if (!ok && cached && emit_split_refused_on_sigs() && !diag_had_error()) {
                        /* The cached unit is not the one this program's
                         * stdlib would emit: emit it, as with no cache. */
                        if (getenv("TUR_SHOW_CC"))
                            fprintf(stderr, "tur: the cached library unit %016llx does not fit; emitting it\n",
                                    (unsigned long long)slc.key);
                        slc.no_restore = true;
                        emit_split_clear_defined(ctx.prog);
                        buf_free(&lib_r);
                        emit_split_reset();
                        lib.len = imp.len = cli.len = 0;
                        continue;
                    }
                    if (!ok && getenv("TUR_SHOW_CC"))
                        fprintf(stderr, "tur: %s split declined: %s\n",
                                use_imp ? "imported-library" : "prelude",
                                why ? why : "emission failed");
                    if (ok) {
                        /* Every name a library unit exports moves to a
                         * private prefix, in every unit (EMIT_SPLIT_PREFIX). */
                        buf_write(g_split_lib_sink, lib_r.data, lib_r.len);
                        if (use_imp) emit_split_rename(imp.data, imp.len, g_split_imp_sink);
                        emit_split_rename(cli.data, cli.len, out_c);
                        split_done = true;
                        g_split_lib_from_cache = cached;
                    }
                    buf_free(&lib_r);
                    emit_split_reset();
                    lib.len = imp.len = cli.len = 0;
                    if (ok || !use_imp || diag_had_error()) break;
                    /* Two units then, as before: the imported libraries are
                     * the program's. */
                    use_imp = false;
                }
                buf_free(&lib);
                buf_free(&imp);
                buf_free(&cli);
                if (diag_had_error()) rc = 1;
            }
            if (rc == 0 && !split_done && emit_program(out_c, ctx.prog) != 0) rc = 1;
            /* J2: the REPL's in-process spice build wants the exports
             * manifest from this same single-TU compile (the sink is set
             * only around that call; every other caller leaves it NULL). */
            if (rc == 0 && g_manifest_sink) {
                size_t mfrom = g_manifest_sink->len;
                if (emit_exports_manifest(g_manifest_sink, ctx.prog) != 0) rc = 1;
                else manifest_keep_defined(g_manifest_sink, mfrom, out_c);
            }
            prof_exit(PROF_EMIT);
        }
    }

    /* Phase U5: Print unsafe statistics if enabled */
    if (g_unsafe_stats_enabled) {
        fprintf(stderr, "unsafe stats: %u blocks, %u total forms\n",
                g_unsafe_block_count, g_unsafe_total_lines);
    }

    symtab_free(&st);
    arena_free(&arena);
    free(src);
    return rc;
}

/* --timings: as prepend_stdlib_forms -- loading the prelude is module-load,
 * its parse "read". */
static Form **load_project_prelude(Arena *arena, SymbolTable *st,
                                    Form **user_forms, uint32_t nuser,
                                    uint32_t *stdlib_count_out) {
    prof_enter(PROF_MODULE_LOAD);
    Form **r = load_project_prelude_impl(arena, st, user_forms, nuser,
                                         stdlib_count_out);
    prof_exit(PROF_MODULE_LOAD);
    return r;
}

/* Prepend the project-mode prelude to user_forms, for compile_to_h and
 * compile_to_implementation.  Returns a new forms array with the prelude forms
 * ahead of user_forms and sets *stdlib_count_out to how many were prepended.
 * When nothing loads, the returned array is user_forms unchanged and
 * *stdlib_count_out = 0. */
static Form **load_project_prelude_impl(Arena *arena, SymbolTable *st,
                                         Form **user_forms, uint32_t nuser,
                                         uint32_t *stdlib_count_out) {
    *stdlib_count_out = 0;

    /* The prelude IS the project-mode stdlib auto-load: every file the
     * single-file compile_to_c path loads (tur_stdlib_autoload_files()) is
     * preloaded here in the same order, so `(Cons A)` / `tnil?` /
     * `Option`/`Result` etc. are visible inside spice defmodule bodies
     * built via `tur build .` without explicit imports.  See
     * docs/archive/history/project-mode-no-stdlib-autoload.md.
     *
     * Codegen note: bindings created during this prelude window are
     * marked `is_from_stdlib`. emit_implementation skips non-exported
     * stdlib bindings; exported ones (`ok`, `ok-val`,
     * `make-struct Result ...`) are static-or-inline-C wrappers so the
     * per-TU duplication mirrors `emit_closure_fat_runtime`. */
    const char *const *prelude_files = tur_stdlib_autoload_files();

    uint32_t total = 0;
    Form **all = NULL;
    uint8_t fid = 1;

    for (int i = 0; prelude_files[i] != NULL; i++) {
        char path_buf[4096];
        tur_stdlib_path(prelude_files[i], path_buf, sizeof(path_buf));
        char *stdlib_src = NULL;
        size_t stdlib_len = 0;
        if (drv_read_entire_file_quiet(path_buf, &stdlib_src, &stdlib_len) != 0)
            continue;

        char *src_copy = (char *)arena_alloc(arena, stdlib_len);
        memcpy(src_copy, stdlib_src, stdlib_len);
        char *path_copy = (char *)arena_alloc(arena, strlen(path_buf) + 1);
        memcpy(path_copy, path_buf, strlen(path_buf) + 1);
        free(stdlib_src);

        SourceFile *sf = (SourceFile *)arena_alloc(arena, sizeof(SourceFile));
        *sf = (SourceFile){0};
        sf->path       = path_copy;
        sf->src        = src_copy;
        sf->len        = stdlib_len;
        sf->file_id    = fid++;
        sf->reader_type = READER_TURMERIC;
        /* saffron-lang-plan S8: honour a `#lang` line in an autoloaded stdlib
         * file.  Without this a scaffolded Saffron LIBRARY died on the
         * prelude's own first line (`unexpected character '#'`) while the
         * BINARY scaffold built fine, because that path goes through
         * compile_to_c and the other loader.  Shared with
         * tur_stdlib_prepend_forms -- the two had identical copies, and this
         * was the fourth loader found missing the detection entirely; see the
         * helper's comment. */
        tur_source_file_apply_lang_header(sf, src_copy, stdlib_len);
        diag_register_file(sf);

        uint32_t n = 0;
        Form **f = read_all(arena, st, sf, &n);

        if (f && n > 0) {
            Form **merged = (Form **)arena_alloc(arena, (total + n) * sizeof(Form *));
            for (uint32_t j = 0; j < total; j++) merged[j] = all[j];
            for (uint32_t j = 0; j < n;     j++) merged[total + j] = f[j];
            all   = merged;
            total += n;
        }
    }

    if (total == 0)
        return user_forms;

    Form **combined = (Form **)arena_alloc(arena, (total + nuser) * sizeof(Form *));
    for (uint32_t i = 0; i < total; i++) combined[i]         = all[i];
    for (uint32_t i = 0; i < nuser;  i++) combined[total + i] = user_forms[i];

    *stdlib_count_out = total;
    return combined;
}

/* Compile a .tur file to a C header (.h). Returns 0 on success.
 * J5/J6: forced/n_forced inject ABI clone decls for cross-module borrow specs. */
int compile_to_h(const char *path, Buf *out_h, const char *module_name,
                 const char **include_dirs, int n_include_dirs,
                 const char **reader_macro_paths,
                 int n_reader_macro_paths,
                 const ForcedAbiSpec *forced, uint32_t n_forced) {
    char  *src = NULL;
    size_t len = 0;
    if (drv_read_entire_file(path, &src, &len) != 0) return 2;

    /* Detect language and adjust source for #lang directive */
    const char *src_adj = src;
    size_t len_adj = len;
    LangDialect lang_dialect = LANG_TURMERIC;
    ReaderType reader_type = detect_and_adjust_lang(path, src, len, &src_adj, &len_adj, &lang_dialect);

    /* Fresh diagnostic slate per compilation unit -- see begin_compile_unit.
     * The project-mode dir build loops compile_to_h / compile_to_implementation
     * in-process, so a stale `had_error_` from an earlier module would
     * otherwise short-circuit every later module's `diag_had_error()` gate. */
    begin_compile_unit();

    SourceFile file = {0};
    file.path = path;
    file.src = src_adj;
    file.len = len_adj;
    /* How much of the file `src_adj` skipped -- the `#lang` line. Span
     * offsets index src_adj; anything that seeks into the file on disk (the
     * LSP's scope table) has to add this back. */
    file.head_offset = (size_t)(src_adj - src);
    file.file_id = 0;
    file.reader_type = reader_type;
    file.lang        = lang_dialect;
    diag_register_file(&file);

    Arena arena;
    arena_init(&arena, 0);
    SymbolTable st;
    symtab_init(&st, &arena);

    /* RM4: preload spice-manifest reader-macros into the reader registry. */
    ReaderMacroRegistry reader_macros_reg;
    reader_macros_init(&reader_macros_reg, &arena);
    /* Transitive-RM decision #2: batch compile is strict -- duplicate
     * `(reader-macros/define ...)` is a hard error. REPL leaves this
     * flag false on env->reader_macros for iterative-redefinition UX. */
    reader_macros_reg.strict = true;
    for (int i = 0; i < n_reader_macro_paths; ++i) {
        if (reader_macros_load_file(&arena, &st,
                                    reader_macro_paths[i],
                                    &reader_macros_reg) != 0) {
            symtab_free(&st); arena_free(&arena); free(src);
            return 1;
        }
    }

    uint32_t nforms = 0;
    Form **forms = read_all_with_registry(&arena, &st, &file,
                                          &reader_macros_reg, &nforms);

    /* F1: prepend the stdlib auto-load list (the same files compile_to_c gets). */
    uint32_t stdlib_prefix = 0;
    forms = load_project_prelude(&arena, &st, forms, nforms, &stdlib_prefix);
    nforms += stdlib_prefix;

    int rc = 0;
    if (!forms || diag_had_error()) {
        rc = 1;
    } else {
        char base_dir[4096];
        drv_dir_of_path(path, base_dir, sizeof(base_dir));
        PassContext ctx = {0};
        ctx.arena  = &arena;
        ctx.st     = &st;
        ctx.forms  = forms;
        ctx.nforms = nforms;
        ctx.stdlib_prefix    = stdlib_prefix;
        ctx.module_base_dir = base_dir;
        ctx.include_dirs    = include_dirs;
        ctx.n_include_dirs  = n_include_dirs;
        ctx.separate_compilation = true;
        ctx.reader_macros   = &reader_macros_reg;
        rc = run_core_passes(&ctx);
        prof_enter(PROF_EMIT);
        if (rc == 0 && emit_header(out_h, module_name, ctx.prog, true,
                                   forced, n_forced) != 0) {
            rc = 1;
        }
        prof_exit(PROF_EMIT);
    }

    symtab_free(&st);
    arena_free(&arena);
    free(src);
    return rc;
}
/* compile_to_implementation follows. */

/* Compile a .tur file to a C implementation (.c). Returns 0 on success.
 * RP1: when `out_manifest` is non-NULL, append one exports.manifest line
 * per exported defn (see emit.h:emit_exports_manifest). Pass NULL when
 * the caller only wants the .c output.
 * J5/J6: forced/n_forced inject forced ABI clone bodies; out_borrow_specs
 * receives borrow spec info for the cross-module cache (pass NULL to skip). */
int compile_to_implementation(const char *path, Buf *out_c, const char *module_name,
                              const char **include_dirs, int n_include_dirs,
                              const char **reader_macro_paths,
                              int n_reader_macro_paths,
                              Buf *out_manifest,
                              const ForcedAbiSpec *forced, uint32_t n_forced,
                              BorrowSpecInfo **out_borrow_specs, uint32_t *out_n_borrow_specs) {
    char  *src = NULL;
    size_t len = 0;
    if (drv_read_entire_file(path, &src, &len) != 0) return 2;

    /* Detect language and adjust source for #lang directive */
    const char *src_adj = src;
    size_t len_adj = len;
    LangDialect lang_dialect = LANG_TURMERIC;
    ReaderType reader_type = detect_and_adjust_lang(path, src, len, &src_adj, &len_adj, &lang_dialect);

    /* Fresh diagnostic slate per compilation unit -- see begin_compile_unit.
     * The project-mode dir build loops compile_to_h / compile_to_implementation
     * in-process, so a stale `had_error_` from an earlier module would
     * otherwise short-circuit every later module's `diag_had_error()` gate. */
    begin_compile_unit();

    SourceFile file = {0};
    file.path = path;
    file.src = src_adj;
    file.len = len_adj;
    /* How much of the file `src_adj` skipped -- the `#lang` line. Span
     * offsets index src_adj; anything that seeks into the file on disk (the
     * LSP's scope table) has to add this back. */
    file.head_offset = (size_t)(src_adj - src);
    file.file_id = 0;
    file.reader_type = reader_type;
    file.lang        = lang_dialect;
    diag_register_file(&file);

    Arena arena;
    arena_init(&arena, 0);
    SymbolTable st;
    symtab_init(&st, &arena);

    /* RM4: preload spice-manifest reader-macros into the reader registry. */
    ReaderMacroRegistry reader_macros_reg;
    reader_macros_init(&reader_macros_reg, &arena);
    /* Transitive-RM decision #2: batch compile is strict -- duplicate
     * `(reader-macros/define ...)` is a hard error. REPL leaves this
     * flag false on env->reader_macros for iterative-redefinition UX. */
    reader_macros_reg.strict = true;
    for (int i = 0; i < n_reader_macro_paths; ++i) {
        if (reader_macros_load_file(&arena, &st,
                                    reader_macro_paths[i],
                                    &reader_macros_reg) != 0) {
            symtab_free(&st); arena_free(&arena); free(src);
            return 1;
        }
    }

    uint32_t nforms = 0;
    Form **forms = read_all_with_registry(&arena, &st, &file,
                                          &reader_macros_reg, &nforms);

    /* F1: prepend the stdlib auto-load list (the same files compile_to_c gets). */
    uint32_t stdlib_prefix = 0;
    forms = load_project_prelude(&arena, &st, forms, nforms, &stdlib_prefix);
    nforms += stdlib_prefix;

    int rc = 0;
    if (!forms || diag_had_error()) {
        rc = 1;
    } else {
        char base_dir[4096];
        drv_dir_of_path(path, base_dir, sizeof(base_dir));
        PassContext ctx = {0};
        ctx.arena  = &arena;
        ctx.st     = &st;
        ctx.forms  = forms;
        ctx.nforms = nforms;
        ctx.stdlib_prefix    = stdlib_prefix;
        ctx.module_base_dir = base_dir;
        ctx.include_dirs    = include_dirs;
        ctx.n_include_dirs  = n_include_dirs;
        ctx.separate_compilation = true;
        ctx.reader_macros   = &reader_macros_reg;
        rc = run_core_passes(&ctx);
        prof_enter(PROF_EMIT);
        if (rc == 0 && emit_implementation(out_c, module_name, ctx.prog, true,
                                           forced, n_forced,
                                           out_borrow_specs, out_n_borrow_specs) != 0) {
            rc = 1;
        }
        /* RP1: emit_exports_manifest only inspects bindings on `ctx.prog`
         * and is safe to run after emit_implementation (which doesn't
         * mutate the program). Skip on prior error to avoid surfacing
         * incomplete manifest entries. */
        if (rc == 0 && out_manifest) {
            size_t mfrom = out_manifest->len;
            if (emit_exports_manifest(out_manifest, ctx.prog) != 0) rc = 1;
            else manifest_keep_defined(out_manifest, mfrom, out_c);
        }
        prof_exit(PROF_EMIT);
    }

    symtab_free(&st);
    arena_free(&arena);
    free(src);
    return rc;
}

/* Run type-check only on `path`; discard generated C.
 * Used by the LSP server. Must be called with diag_lsp_begin active.
 *
 * SC6: applies the same SC4+SC5 auto-discovery that `tur check` uses,
 * so editor diagnostics in spice files don't show bogus "module not
 * found" errors for intra-spice and cross-spice imports.  --no-auto-spice
 * is honored if the user passed it at the top level. */
int tur_check_only(const char *path) {
    /* LSP/MCP diagnostics run experiment-hot: enable every registered
     * in-flight feature (kind annotations, constrained forall, HKT rank-2,
     * dictionary passing, curried rank-2 results, ...) so the editor sees
     * the same "this parses" answer a developer would get with the matching
     * --enable= flags on the CLI.  The TUR-W006x lifecycle warnings write to
     * stderr, not the JSON diagnostics stream, so they never surface as
     * editor diagnostics. */
    for (size_t xi = 0, xn = experiment_count(); xi < xn; xi++) {
        const ExperimentDescriptor *d = experiment_at(xi);
        if (d) experiment_enable(d->name, XF_SRC_CLI);
    }
    /* UC-3: everything is already on at XF_SRC_CLI above, so the user-level
     * experiments file would be a redundant no-op -- and a malformed one
     * must not crash an editor keystroke.  Mark it consulted so the later
     * discover_manifest_reader_macros call skips the read entirely (and the
     * source column stays 'cli'). */
    g_user_config_experiments_done = true;

    char **inc = NULL;
    int    n_inc = 0;
    char **owned = NULL;
    int    n_owned = 0;
    Ls2ResolverCtx ls2;
    auto_append_spice_includes(path, &inc, &n_inc, &owned, &n_owned, &ls2);

    Buf discard;
    buf_init(&discard);
    int rm_n = 0;
    char **rm_p = discover_manifest_reader_macros(path, &rm_n);
    ls2_resolver_ctx_set(&ls2);
    int rc = compile_to_c(path, &discard, (const char **)inc, n_inc,
                          (const char **)rm_p, rm_n);
    ls2_resolver_ctx_set(NULL);
    ls2_resolver_ctx_dispose(&ls2);
    free_reader_macro_paths(rm_p, rm_n);
    buf_free(&discard);

    for (int i = 0; i < n_owned; i++) free(owned[i]);
    free(owned);
    free(inc);
    return rc;
}
