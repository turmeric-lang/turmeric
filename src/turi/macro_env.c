/* macro_env.c -- the per-compile macro-time interpreter env.
 *
 * Stage 1 of docs/archive/macro-system-direction-plan.md: the elaborator
 * gets a lazily-created, capability-denied, fuel-limited turi env hung off
 * its Elab/ElabSession, torn down with the session.  Stage 2's procedural
 * macros (`defmacro*`) evaluate their bodies in this env; nothing in the
 * compiler calls into it yet beyond creation/teardown.
 *
 * This file lives on the turi side of the tree because the include
 * direction in this codebase is turi -> compiler (src/turi/eval.c already
 * includes elab.h); the compiler sees only the two lean declarations in
 * elab.h with `struct TuriEnv` opaque.
 *
 * The two reentrancy hazards this file owns (the Stage 1 audit):
 *
 *  - `turi_env_new` unconditionally sets the process-global
 *    `g_interpret_mode = true` (env.c, libturi-embed-interpret-mode-flag)
 *    on the theory that anybody creating an env is an interpreter
 *    embedder.  Here the caller is the COMPILER, mid-elaboration, and
 *    leaving the flag flipped would change the enclosing compile's
 *    semantics: `#?(:tur ...)` reader-conds would pick the `:turi` arm and
 *    unknown call heads would demote from hard errors to TUR-W0040
 *    runtime dispatch.  So creation brackets the flag (the same
 *    save/set/call/restore the JIT REPL uses around compile_to_c).
 *
 *  - Each `turi_eval` against the env is already safe: turi_eval_with_sink
 *    (eval.c, "Gap 7") snapshots g_interpret_mode to the env's own
 *    interpret_mode bit for the duration of the call and restores it, and
 *    saves/restores the diagnostic sink and file registry around the
 *    nested elaboration.  Macro-time evaluation deliberately KEEPS
 *    env->interpret_mode = true: macro bodies run interpreted, so they see
 *    `#?(:turi ...)` arms and interpreter call semantics -- that is what
 *    "the macro-time language is the interpreted language" means.
 */
#include "turi/eval.h"
#include "source_literal.h"
#include "turi/env.h"
#include "turi/interpreter_natives.h"
#include "turi/collections_native.h"
#include "turi/preload.h"

#include "elab.h"
#include "elab_internal.h"
#include "globals.h"

/* Bracket for every nested evaluation the macro env runs mid-compile
 * (creation/preload, defmacro* definition, :for-macros import).  Each
 * nested eval resets the diagnostic slate, registers its own source at
 * file id 0 (replacing the compile's main file -- diag_files_restore alone
 * deliberately skips id 0, so entries are re-registered directly), and
 * re-stamps the global builtin table's name_sym pointers against the macro
 * env's own symbol table (builtins_init runs at every elaborate entry;
 * un-restored, every later pointer-keyed builtin lookup in the enclosing
 * compile misses -- "unknown function or operator 'println'").  The
 * `tur expand` trace is also silenced: the macro env's internal expansions
 * are not the user's program. */
typedef struct MacroEnvBracket {
    bool              saved_had;
    const SourceFile *saved_files[DIAG_MAX_FILES];
    size_t            n_saved;
    bool              saved_dump;
} MacroEnvBracket;

static void macro_env_bracket_enter(MacroEnvBracket *b) {
    b->saved_had = diag_had_error();
    b->n_saved = diag_files_save(b->saved_files,
                                 sizeof(b->saved_files) / sizeof(b->saved_files[0]));
    b->saved_dump = g_dump_expansion;
    g_dump_expansion = false;
}

static void macro_env_bracket_exit(MacroEnvBracket *b, Elab *e) {
    /* Replace, do not merge.  The nested evaluations registered the macro
     * env's own files (its stdlib preload, a defmacro* body's source), which
     * live in the macro env's eval arenas.  Merging back left them in the
     * registry, every later turn's save/restore carried them forward, and once
     * the macro env was freed with its session -- which a failed REPL turn
     * does -- the next bracket re-registered freed SourceFiles and aborted
     * with "too many source files" on their garbage file_id. */
    diag_files_replace(b->saved_files, b->n_saved);
    if (b->saved_had) diag_force_had_error();
    g_dump_expansion = b->saved_dump;
    /* A bare session (e.g. the ctest) has no symtab yet; its next
     * elaborate entry re-stamps anyway. */
    if (e->st) builtins_init(e->st);
}

/* ---------------------------------------------------------------------------
 * Stage 4 / R3: bounded type reflection -- syntax-struct-fields.
 *
 * R3 of docs/upcoming/row-types-followups-plan.md rules that if
 * type-level metaprogramming is ever built, it must be a bounded, TOTAL,
 * structurally-recursive accessor -- never a general Type value in an
 * evaluator.  This native is exactly that: given a Syntax symbol naming a
 * single-constructor record (a defstruct, post structdef-retirement a
 * record ADT), it returns the field names as a Syntax list of symbols.
 * No Type value ever crosses into the macro env; the accessor is a flat,
 * finite projection of the compile's ADT registry.
 *
 * The registry lives on the enclosing compile's Elab, which is a stack
 * local of elaborate_program_session -- its address is only meaningful
 * while an expansion is actually running, so elab_macro_env_call_proc
 * opens a reflection window around each turi_call (save/set/restore, so
 * nested expansions stay correct) and the native refuses to run outside
 * one.
 * ------------------------------------------------------------------------- */
static Elab *g_reflect_elab = NULL;

static TuriValue native_syntax_struct_fields(TuriEnv *e, TuriValue *a,
                                             uint32_t n, void *ud) {
    (void)ud;
    if (!g_reflect_elab)
        return turi_errorf("syntax-struct-fields: only available while a "
                           "macro expansion is running");
    if (n < 1 || a[0].tag != TURI_SYNTAX || !a[0].as_syntax ||
        a[0].as_syntax->tag != F_SYM)
        return turi_errorf("syntax-struct-fields: expected a syntax symbol "
                           "naming a struct");
    const Symbol *name = a[0].as_syntax->as.sym;

    Elab *ce = g_reflect_elab;
    const AdtDef *adt = NULL;
    for (uint32_t i = 0; i < ce->n_adt_defs; i++) {
        const AdtDef *d = ce->adt_defs[i];
        if (d->superseded) continue;
        if (strlen(d->name) == name->len &&
            memcmp(d->name, name->name, name->len) == 0) {
            adt = d;
            break;
        }
    }
    if (!adt)
        return turi_errorf("syntax-struct-fields: no struct or data type "
                           "named '%s' is defined at this point in the "
                           "compile", name->name);
    if (adt->is_opaque)
        return turi_errorf("syntax-struct-fields: '%s' is an opaque newtype "
                           "with no fields", name->name);
    if (adt->n_ctors != 1 || !adt->ctors[0]->is_record)
        return turi_errorf("syntax-struct-fields: '%s' is not a "
                           "single-constructor record (walk its variants "
                           "explicitly)", name->name);

    const CtorDef *ctor = adt->ctors[0];
    Form **items = (ctor->n_fields == 0) ? NULL
        : (Form **)arena_alloc(&e->sym_arena,
                               ctor->n_fields * sizeof(Form *));
    for (uint32_t i = 0; i < ctor->n_fields; i++) {
        const char *fname = ctor->fields[i].name;
        StrSlice sl = { fname, (uint32_t)strlen(fname) };
        const Symbol *fsym = symtab_intern(&e->st, sl);
        items[i] = form_sym(&e->sym_arena, SPAN_UNKNOWN, fsym);
    }
    return turi_syntax_val(form_list(&e->sym_arena, SPAN_UNKNOWN,
                                     items, ctor->n_fields));
}

struct TuriEnv *elab_macro_env_get(ElabSession *session) {
    Elab *e = (Elab *)session;
    if (!e) return NULL;
    if (e->macro_env) return e->macro_env;

    /* Creation runs several nested evaluations (the stdlib preload below);
     * bracket ALL of it -- creation is lazy, so it fires mid-compile at the
     * first defmacro*. */
    MacroEnvBracket br;
    macro_env_bracket_enter(&br);
    bool saved_interp = g_interpret_mode;

    TuriEnv *env = turi_env_new();   /* sets g_interpret_mode = true */
    g_interpret_mode = saved_interp;
    if (env) {
        /* Load the stdlib into the macro env with the REPL's exact sequence
         * (repl_preload_stdlib_and_natives), so defmacro* bodies get the
         * real language: core macros (cond/when/for/...), the carrier list
         * helpers, typed collections, typeclasses, and string functions.
         * This must run BEFORE capabilities are denied -- the preload's
         * `(load ...)` splices are gated on TURI_CAP_IMPORT -- and the
         * interpreter natives are re-registered AFTER it so the native
         * shims override the loaded inline-C stdlib bodies (the documented
         * preload ordering contract in preload.h / repl.c).
         *
         * stdlib root: the compile's own resolved stdlib dir; each nested
         * eval self-brackets g_interpret_mode (turi_eval_with_sink). */
        const char *stdlib_root = e->module_stdlib_dir;
        turi_env_preload_macros(env, stdlib_root);
        turi_env_preload_native_stubs(env);
        turi_env_preload_collections(env, stdlib_root);
        turi_env_preload_typeclasses(env, stdlib_root);
        /* String helpers are not in the shared collection preload set; load
         * them too -- name synthesis is the bread-and-butter of procedural
         * macros, and the typed defns kill the W0040 runtime-dispatch
         * warning on every defmacro* body that concatenates.  str-build.tur
         * carries str-concat + the StringBuilder; cstr.tur the byte-level
         * cstr ops.  (The interpreter-native shims re-registered below
         * override their inline-C bodies at runtime, per the preload
         * ordering contract.) */
        {
            char loadbuf[8600], eroot[8200];
            const char *root = (stdlib_root && *stdlib_root) ? stdlib_root
                                                             : "stdlib";
            /* generated SOURCE -- a Windows path needs escaping to survive
             * the string lexer.  See source_literal.h. */
            if (tur_source_literal_escape(root, eroot, sizeof eroot)) {
            snprintf(loadbuf, sizeof(loadbuf),
                     "(load \"%s/cstr.tur\")(load \"%s/str-build.tur\")",
                     eroot, eroot);
            turi_eval(env, loadbuf);
            }
        }
        turi_env_pin_prelude(env);
        turi_env_register_interpreter_natives(env);
        /* Re-assert collection natives (vec/set/map/hamt) after preload --
         * see the matching call in cmd_eval_h for why. */
        turi_register_collection_natives(env);
        /* Stage 4 / R3: bounded type reflection, macro-env only -- the
         * native reads the enclosing compile's ADT registry through the
         * reflection window call_proc opens around each expansion. */
        turi_env_register_native_typed(env, "syntax-struct-fields",
                                       native_syntax_struct_fields, NULL,
                                       TUR_NRT_SYNTAX);

        /* Macro expansion must be deterministic and effect-free: deny every
         * capability (no I/O, FFI, inline-C, async, unsafe, import) and
         * bound total work with step fuel so a runaway macro-time loop is a
         * diagnostic, not a hung compile.  Stage 3 grants TURI_CAP_IMPORT
         * transiently for `:for-macros` module loads; nothing else is ever
         * granted here. */
        turi_env_deny(env, TURI_CAP_ALL);
        turi_env_set_fuel(env, TURI_DEFAULT_SANDBOX_FUEL);
        /* Stage 3: --macro-caps=io re-grants exactly I/O for the rare
         * legitimately-effectful macro (an embed-file generator).  WP3 split
         * path-based file access out of TURI_CAP_IO into TURI_CAP_FS, so the
         * flag grants both to keep meaning what it documented.  Process,
         * environment, FFI, inline-C, unsafe and async are never granted. */
        if (g_macro_caps_io) turi_env_allow(env, TURI_CAP_IO | TURI_CAP_FS);
    }

    macro_env_bracket_exit(&br, e);

    e->macro_env = env;
    return env;
}

void elab_macro_env_dispose(struct TuriEnv *env) {
    if (env) turi_env_free(env);
}

/* ---------------------------------------------------------------------------
 * Stage 2: the procedural-macro bridge (defmacro*).
 * ------------------------------------------------------------------------- */

/* Deep-copy a macro-env-built Form into the compiler's arena, re-interning
 * every symbol into the compiler's symbol table.  Mandatory, not an
 * optimization: symbols built macro-side live in the macro env's own
 * SymbolTable, and the elaborator dispatches special forms by POINTER
 * identity (`head == e->sym_do`), so an expansion carrying env-interned
 * symbols would never match any special form or binding.  Also gives the
 * expansion compile-arena lifetime, decoupled from the macro env's. */
static Form *import_form(Elab *e, const Form *f, Span call_span) {
    if (!f) return NULL;
    /* Forms constructed macro-side (int->syntax, sym->syntax, ...) carry no
     * span; attribute them to the macro CALL SITE so diagnostics against
     * generated code point at the call the user wrote instead of `?:0:0`.
     * Forms that came from real source (read-string, or arguments passed
     * through) keep their own spans. */
    Span sp = (f->span.line == 0) ? call_span : f->span;
    Form *out = form_new(e->arena, f->tag, sp);
    out->lit_suffix = f->lit_suffix;
    out->fx_prov    = f->fx_prov;
    switch (f->tag) {
        case F_NIL:
            break;
        case F_BOOL:  out->as.b = f->as.b; break;
        case F_INT:   out->as.i = f->as.i; break;
        case F_FLOAT: out->as.f = f->as.f; break;
        case F_STR: {
            char *p = (char *)arena_alloc(e->arena, f->as.s.len + 1);
            memcpy(p, f->as.s.p, f->as.s.len);
            p[f->as.s.len] = '\0';
            out->as.s.p = p;
            out->as.s.len = f->as.s.len;
            break;
        }
        case F_CBLOCK: {
            char *p = (char *)arena_alloc(e->arena, f->as.cblock.len + 1);
            memcpy(p, f->as.cblock.p, f->as.cblock.len);
            p[f->as.cblock.len] = '\0';
            out->as.cblock.p = p;
            out->as.cblock.len = f->as.cblock.len;
            break;
        }
        case F_SYM:
        case F_KEYWORD:
            out->as.sym = symtab_intern(e->st,
                                        strslice(f->as.sym->name, f->as.sym->len));
            break;
        default: {
            /* Every remaining tag carries the FormList payload (lists,
             * vectors, maps, sets, literals, quote/quasiquote/unquote
             * wrappers, type annotations, contract types, reader conds,
             * range vars). */
            uint32_t len = f->as.list.len;
            Form **items = (len == 0) ? NULL
                : (Form **)arena_alloc(e->arena, len * sizeof(Form *));
            for (uint32_t i = 0; i < len; i++)
                items[i] = import_form(e, f->as.list.items[i], call_span);
            out->as.list.items = items;
            out->as.list.len   = len;
            break;
        }
    }
    return out;
}

/* security-audit-plan WP3: --no-proc-macros refuses each of the three ways
 * into macro-time evaluation before the env is even created. */
static bool proc_macros_refused(Span span, const char *what) {
    if (!g_no_proc_macros) return false;
    diag_emit(DIAG_ERROR, span,
              "%s: procedural macros are disabled (--no-proc-macros); no "
              "macro-time code was run", what);
    return true;
}

bool elab_macro_env_define_proc(Elab *e, const char *fn_name,
                                const Form *defn_form, Span err_span) {
    if (proc_macros_refused(err_span, "defmacro*")) return false;
    TuriEnv *env = elab_macro_env_get((ElabSession *)e);
    if (!env) {
        diag_emit(DIAG_ERROR, err_span,
                  "defmacro*: could not create the macro-time environment");
        return false;
    }

    /* The macro env has its own reader/symtab, so hand the synthesized defn
     * over as printed source -- form_print output is the same AST-faithful
     * serialization parse-check diffs on.  The path string is retained by
     * the env's SourceFile, so it must not be stack memory: allocate it
     * from the compile arena. */
    Buf src;
    buf_init(&src);
    form_print(&src, defn_form);
    buf_putc(&src, '\0');

    size_t path_len = strlen(fn_name) + 16;
    char *path = (char *)arena_alloc(e->arena, path_len);
    snprintf(path, path_len, "<defmacro* %s>", fn_name);

    MacroEnvBracket br;
    macro_env_bracket_enter(&br);
    TuriValue r = turi_eval_with_path(env, src.data, path);
    buf_free(&src);
    macro_env_bracket_exit(&br, e);

    if (turi_is_error(r)) {
        diag_emit(DIAG_ERROR, err_span,
                  "defmacro*: body failed at definition time: %s",
                  turi_error_message(r) ? turi_error_message(r) : "unknown error");
        return false;
    }
    TuriValue fn = turi_env_get(env, fn_name);
    if (fn.tag != TURI_CLOSURE) {
        diag_emit(DIAG_ERROR, err_span,
                  "defmacro*: body did not produce a callable macro function");
        return false;
    }
    return true;
}

Form *elab_macro_env_call_proc(Elab *e, MacroDef *macro,
                               Form **args, uint32_t n_args, Span call_span) {
    if (proc_macros_refused(call_span, macro->name->name)) return NULL;
    TuriEnv *env = elab_macro_env_get((ElabSession *)e);
    if (!env) {
        diag_emit(DIAG_ERROR, call_span,
                  "macro '%s': macro-time environment unavailable",
                  macro->name->name);
        return NULL;
    }

    /* Arity, mirroring the template path's messages. */
    if (macro->is_variadic ? (n_args < macro->n_params)
                           : (n_args != macro->n_params)) {
        diag_emit(DIAG_ERROR, call_span,
                  macro->is_variadic
                      ? "macro '%s' expects at least %u arguments, got %u"
                      : "macro '%s' expects %u arguments, got %u",
                  macro->name->name, macro->n_params, n_args);
        return NULL;
    }

    /* Marshal: each fixed arg is one Syntax; a variadic tail packs into one
     * list form (matching the single `rest : Syntax` param the definition
     * path declared). */
    uint32_t argc = macro->n_params + (macro->is_variadic ? 1 : 0);
    TuriValue argv_buf[16];
    TuriValue *argv = argc <= 16 ? argv_buf
        : (TuriValue *)arena_alloc(e->arena, argc * sizeof(TuriValue));
    for (uint32_t i = 0; i < macro->n_params; i++)
        argv[i] = turi_syntax_val(args[i]);
    if (macro->is_variadic) {
        uint32_t n_rest = n_args - macro->n_params;
        Form **rest = (n_rest == 0) ? NULL
            : (Form **)arena_alloc(e->arena, n_rest * sizeof(Form *));
        for (uint32_t i = 0; i < n_rest; i++) rest[i] = args[macro->n_params + i];
        Span rest_span = n_rest > 0 ? rest[0]->span : call_span;
        argv[macro->n_params] =
            turi_syntax_val(form_list(e->arena, rest_span, rest, n_rest));
    }

    TuriValue fnv = turi_env_get(env, macro->proc_fn_name);
    if (fnv.tag != TURI_CLOSURE) {
        diag_emit(DIAG_ERROR, call_span,
                  "macro '%s': internal error -- macro function '%s' is not "
                  "bound in the macro-time environment",
                  macro->name->name, macro->proc_fn_name);
        return NULL;
    }

    /* Each expansion gets a fresh fuel budget (the limit set at env
     * creation); runtime evaluation itself does not consult
     * g_interpret_mode, but bracket it anyway so a native that re-enters
     * elaboration (read-string) sees interpreter posture and the compile's
     * posture is restored regardless of how the call exits. */
    turi_env_set_fuel(env, env->step_fuel_limit);
    bool saved_interp = g_interpret_mode;
    g_interpret_mode = true;
    /* Stage 4 / R3: open the reflection window (save/restore, so nested
     * expansions stay correct) -- syntax-struct-fields reads the compile's
     * ADT registry only while an expansion is actually running. */
    Elab *saved_reflect = g_reflect_elab;
    g_reflect_elab = e;
    TuriValue r = turi_call(env, fnv, argv, argc);
    g_reflect_elab = saved_reflect;
    g_interpret_mode = saved_interp;

    if (turi_is_error(r)) {
        diag_emit(DIAG_ERROR, call_span,
                  "macro '%s' failed at expansion time: %s",
                  macro->name->name,
                  turi_error_message(r) ? turi_error_message(r) : "unknown error");
        return NULL;
    }
    if (r.tag != TURI_SYNTAX || !r.as_syntax) {
        diag_emit(DIAG_ERROR, call_span,
                  "macro '%s' did not return a syntax object (got %s); a "
                  "defmacro* body must return the expansion as Syntax "
                  "(e.g. via int->syntax / syntax-list / read-string)",
                  macro->name->name,
                  r.tag == TURI_NIL ? "nil" :
                  r.tag == TURI_INT ? "int" :
                  r.tag == TURI_CSTR ? "cstr" :
                  r.tag == TURI_BOOL ? "bool" :
                  r.tag == TURI_FLOAT ? "float" : "a non-syntax value");
        return NULL;
    }
    return import_form(e, r.as_syntax, call_span);
}

/* ---------------------------------------------------------------------------
 * Stage 3: `(import m :for-macros)` -- macro-time-only imports.
 * ------------------------------------------------------------------------- */

bool elab_macro_env_import(Elab *e, const Symbol *module_name,
                           const char *path, Span span) {
    if (proc_macros_refused(span, ":for-macros")) return false;
    TuriEnv *env = elab_macro_env_get((ElabSession *)e);
    if (!env) {
        diag_emit(DIAG_ERROR, span,
                  ":for-macros: could not create the macro-time environment");
        return false;
    }

    /* Repeat :for-macros of the same module in one compile must be a no-op
     * -- the load splice does NOT dedup across eval turns (measured: the
     * second import spliced the file again, which would trip "already
     * defined" for any macro the module defines).  The env's own global
     * binding table is the dedup set: no extra teardown plumbing. */
    {
        /* turi_env_set retains the binding NAME pointer -- the key handed to
         * it must outlive the env's hash table, so allocate it from the
         * compile arena (the lookup alone may use stack memory). */
        char dedup_key[4400];
        int klen = snprintf(dedup_key, sizeof(dedup_key),
                            "__mx_for_macros_loaded:%s", path);
        if (klen < 0 || (size_t)klen >= sizeof(dedup_key)) klen = (int)sizeof(dedup_key) - 1;
        TuriValue seen = turi_env_get(env, dedup_key);
        if (seen.tag == TURI_BOOL && seen.as_bool) return true;
        char *stable_key = (char *)arena_alloc(e->arena, (size_t)klen + 1);
        memcpy(stable_key, dedup_key, (size_t)klen + 1);
        turi_env_set(env, stable_key, turi_bool(true));
    }

    /* Evaluate the resolved module file into the macro env via the load
     * splice -- the same mechanism the stdlib preload uses.  The env is
     * capability-denied after creation, and (load ...) is gated on
     * TURI_CAP_IMPORT (via the sandboxed flag the nested elaboration
     * derives from it), so grant IMPORT for exactly this eval and take
     * back only that grant afterward -- a --macro-caps=io grant must
     * survive. */
    char src[4352], epath[8200];
    if (!tur_source_literal_escape(path, epath, sizeof epath)) epath[0] = '\0';
    int n = snprintf(src, sizeof(src), "(load \"%s\")", epath);
    if (n < 0 || (size_t)n >= sizeof(src)) {
        diag_emit(DIAG_ERROR, span,
                  ":for-macros module path too long for '%s'",
                  module_name->name);
        return false;
    }

    /* The path label is retained by the env's SourceFile -- arena, not
     * stack. */
    size_t label_len = module_name->len + 16;
    char *label = (char *)arena_alloc(e->arena, label_len);
    snprintf(label, label_len, "<for-macros %s>", module_name->name);

    MacroEnvBracket br;
    macro_env_bracket_enter(&br);
    turi_env_allow(env, TURI_CAP_IMPORT);
    TuriValue r = turi_eval_with_path(env, src, label);
    turi_env_deny(env, TURI_CAP_IMPORT);
    macro_env_bracket_exit(&br, e);

    if (turi_is_error(r)) {
        diag_emit(DIAG_ERROR, span,
                  ":for-macros import of '%s' failed at macro time: %s",
                  module_name->name,
                  turi_error_message(r) ? turi_error_message(r) : "unknown error");
        return false;
    }
    return true;
}
