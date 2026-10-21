#ifndef TUR_EMIT_H
#define TUR_EMIT_H

#include "buf.h"
#include "expr.h"
#include "types.h"

/* Emit a complete C99 program from a typed Expr program (EX_PROGRAM). Returns
 * 0 on success, -1 on error (diagnostics already emitted). */
int emit_program(Buf *out, const Expr *program);

/* project-mode-rc-runtime-preamble-missing (owner-TU design): emit the shared
 * inline C runtime as an include-guarded header (`tur_runtime.h`).  Every module
 * TU of a separate-compilation (`--shared`) build includes this; the single TU
 * that #defines TUR_RT_OWNER before including it (the generated tur_runtime.c)
 * supplies the one definition of each runtime global, while the runtime
 * functions are file-local (static) replicas operating on that shared state. */
void emit_shared_runtime_header(Buf *out);

/* S2 (jit-engine-plan, findings 19.4): the feature-complete SINGLE-FILE
 * runtime preamble -- every program-gated block forced on, single-file
 * linkage -- ending at the preamble marker.  Consumed by the split-generation
 * tool (via `tur emit-rt-split`) to produce the committed runtime-TU +
 * declarations artifacts, and by cmd_jit to hash the current emitter's
 * preamble against the artifact's recorded hash.  Deliberately reflects
 * current process knob state (backtrack depth, archive mode, experiment
 * flags): knob drift must change the text and fail the hash compare. */
void emit_rt_split_source(Buf *out);

/* r7rs-programs-compile-slowly (emit_split.h): write the library unit
 * (EMIT_SPLIT_LIB), the imported-library unit (EMIT_SPLIT_IMP) or the client
 * unit (EMIT_SPLIT_CLIENT) of a split build with the next emit_program, or an
 * ordinary single unit (EMIT_SPLIT_NONE).  Emit them in that order: each
 * records what the units after it declare instead of defining (the fat boxes
 * it owns and its functions' signatures).
 * emit_split_refusal is NULL while both units can be built, else why not --
 * the caller then emits the program as one unit.  emit_split_reset clears
 * the mode and what the library unit recorded. */
void emit_split_set_mode(int mode);
const char *emit_split_refusal(void);
void emit_split_reset(void);
/* r7rs-imported-libraries-recompiled-every-build: does this split build
 * write an imported-library unit (EMIT_SPLIT_IMP)?  Set before the library
 * unit is emitted and kept for all three; with it off, an imported library's
 * items are the program's, as they always were.  emit_split_reset clears it.
 * emit_split_program_imports says whether a program has any such items. */
void emit_split_set_imp(bool on);
bool emit_split_program_imports(const Expr *program);

/* r7rs-split-units-each-compile-the-runtime: what the library unit's emission
 * leaves for the units after it -- the names it exports, the fat boxes and
 * keyword records it defines, its functions' signatures and which of the
 * program's stdlib bindings it defines -- written right after that emission
 * (emit_split_carry_save, appending a text record to `out`) and put back by
 * emit_split_carry_load in place of the emission, in EMIT_SPLIT_LIB mode, for
 * the same elaborated stdlib (the driver keys the cache).  Load returns false,
 * having put nothing back, when the record is malformed or names a binding
 * this program does not have; the caller then emits the unit.
 * emit_split_clear_defined forgets the bindings a load (or an emission)
 * marked. */
void emit_split_carry_save(Buf *out, const Expr *program);
bool emit_split_carry_load(const char *rec, size_t len, const Expr *program);
void emit_split_clear_defined(const Expr *program);
/* The first item the library unit would write that does not come from one of
 * the stdlib's files (an `extern-c` of the program's, which every unit
 * declares), or NULL: only then is the unit the stdlib's alone, so a cached
 * one can stand in for it.  Marks files[id] for the file of every item it
 * would write (ids below n_files). */
const Expr *emit_split_lib_files(const Expr *program, bool *files, size_t n_files);
/* Was this split refused because a unit after the library unit declared one
 * of its functions with another signature?  With a cached library unit, that
 * is the cache's fault, not the program's. */
bool emit_split_refused_on_sigs(void);

/* J2: when true, emit_program appends the per-export `<mangled>__ffi` shims
 * (the --shared path's interpreter-arbitrary-arity-ffi emission) to the
 * single-file TU.  Set only by the REPL's in-process spice build; leave
 * false everywhere else so ordinary emission stays byte-identical. */
extern bool g_emit_ffi_export_shims;

/* DEDUP-4b (docs/archive/gc-cycle-collection-plan.md): declare, rather than define, the
 * rc<T>/GC runtime in the emitted preamble, because the program will link
 * libturt_runtime.a (which carries src/runtime/{rc,gc,rc_free_queue}.c).
 * Without this a compiled program runs a hand-written replica of the collector
 * while the maintained implementation sits unused in the archive.
 *
 * Must be set BEFORE emission -- the generated text depends on it.  The
 * driver resolves it from the same archive probe the link step uses; the probe is a
 * pure filesystem check, so it is safe to run ahead of codegen. */
void emit_set_rcgc_from_archive(bool from_archive);
/* The posture as last set -- true when the emitted preamble declared the
 * archive-owned runtime (rc<T>/GC, regions) without defining it, so the
 * caller's link line must carry libturt_runtime.a. */
bool emit_rcgc_from_archive(void);

/* J5/J6: A forced ABI specialization -- a clone that a borrower module needs
 * emitted by the owner module (even when the owner has no local call sites).
 * Passed from cmd_build_multi / cmd_emit_c_to_dir to emit_header and
 * emit_implementation after the cross-module borrow-spec scan. */
typedef struct {
    const char *clone_name;          /* unique mangled clone name */
    const char *fn_symbol;           /* binding name to look up in owner module */
    TypeKind    result_kind;
    TypeKind    arg_kinds[MAX_FN_ARITY];
    uint8_t     n_args;
    /* forced-spec-loses-aggregate-type: the ADT name at a TY_ADT position, so
     * the owner rebuilds the full type (a by-value struct) rather than the
     * carrier its kind alone gives.  NULL elsewhere.  Borrowed, like the
     * names above. */
    const char *result_adt;
    const char *arg_adts[MAX_FN_ARITY];
} ForcedAbiSpec;

/* J6: Borrow spec info output from emit_implementation.  Each entry describes
 * a clone this module borrowed (call-site rewrite, no body).  The owner is
 * identified by fn_binding->defining_module_name stored in owning_module.
 * All three string fields are malloc'd; caller must free them. */
typedef struct {
    char       *clone_name;          /* malloc'd */
    char       *owning_module;       /* malloc'd (strdup of binding->defining_module_name) */
    char       *fn_symbol;           /* malloc'd (strdup of binding->name->name) */
    TypeKind    result_kind;
    TypeKind    arg_kinds[MAX_FN_ARITY];
    uint8_t     n_args;
    /* ForcedAbiSpec's result_adt / arg_adts; malloc'd, NULL when the position
     * is not a TY_ADT. */
    char       *result_adt;
    char       *arg_adts[MAX_FN_ARITY];
} BorrowSpecInfo;

/* Phase 2: Multi-file support - emit separate .h and .c for a module.
 * Returns 0 on success, -1 on error.
 * `separate_compilation` (Phase M3): when true, the header exports only
 * exported symbols and the implementation #includes imported modules' headers
 * rather than inlining their code.
 * J5/J6: `forced`/`n_forced` inject specialization clones that borrower
 * modules need emitted here; pass NULL/0 for the first-pass compilation.
 * `out_borrow_specs`/`out_n` (emit_implementation only): if non-NULL, receives
 * a malloc'd array of BorrowSpecInfo describing clones this module borrowed. */
/* definstance-not-dispatchable-across-modules: see the definition in
 * emit_module.c -- true when an instance method may take external linkage. */
bool emit_inst_method_wants_external(const struct FnDef *fd);
/* loaded-user-definstance-duplicated-across-tus: the linkage prefix for such a
 * method's definition -- "__attribute__((weak)) " when it arrived by a
 * `(load ...)` of a user file (every loading TU defines it), else "". */
const char *emit_inst_method_linkage_attr(const struct FnDef *fd);

int emit_header(Buf *out, const char *module_name, const Expr *program,
                bool separate_compilation,
                const ForcedAbiSpec *forced, uint32_t n_forced);
int emit_implementation(Buf *out, const char *module_name, const Expr *program,
                        bool separate_compilation,
                        const ForcedAbiSpec *forced, uint32_t n_forced,
                        BorrowSpecInfo **out_borrow_specs, uint32_t *out_n_borrow_specs);

/* RP1: append one manifest line per exported defn of the form
 *   <module>/<defn> -> <mangled-c-symbol> :: (:arg-tag ...) -> :ret-tag
 * to `out`. Tags use the Turmeric type DSL (:int, :cstr, :float, :bool,
 * :void, :ptr, :any). Variadic defns get a `& :rest-tag` after the fixed
 * args. Returns 0 on success, -1 on error. Used by `tur build --shared`
 * to produce exports.manifest for the dlopen host (REPL, FFI dispatcher). */
int emit_exports_manifest(Buf *out, const Expr *program);

#endif
