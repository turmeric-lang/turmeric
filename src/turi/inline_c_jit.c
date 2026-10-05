/* inline_c_jit.c -- see inline_c_jit.h.
 *
 * One call, start to finish:
 *
 *   1. The gate: experiment `repl-jit-inline-c` on, and a host hook present.
 *   2. The cache: one entry per FnDef, holding either the compiled shim or
 *      the reason it could not be had.  A redefinition elaborates a NEW FnDef,
 *      so it misses the cache and compiles afresh; the old entry, and the old
 *      image, stay -- a closure made earlier may still be holding the old
 *      FnDef, and freeing an image something may point into is the mistake
 *      the spice reload already learned not to make (ffi_thunk.c's retired
 *      list).  A refusal is cached too, so a body c2mir rejects costs one
 *      compile, not one per call.
 *   3. The signature check, BEFORE compiling: every parameter and the result
 *      must be a scalar the `__ffi` shim marshals (int-class, float, cstr,
 *      bool, nil).  Anything else is refused with the type named.
 *   4. The source: the defn's own text, sliced out of the file it was read
 *      from by the span elaboration recorded for it (Binding.defn_claim), or
 *      -- for a reader whose text the plain reader cannot read back -- the
 *      Form, printed.
 *   5. The compile: `(defmodule __repl-jit (export NAME) <defn>)` through the
 *      host, which runs the real emitter (hoisted #includes, the preamble's
 *      typed builders, the body-entry region note) and the MIR engine, and
 *      hands back the exports manifest with the function's C name.
 *   6. The call: through NAME__ffi, the uniform-signature shim the emitter
 *      writes beside every scalar export, which casts each slot to the real C
 *      parameter type -- after the value boundary (below) has checked that no
 *      interpreter memory rides into compiled code in a pointer-shaped slot.
 *
 * Only the defn itself is compiled.  A body that calls another Turmeric
 * definition therefore does not elaborate, and is refused with the compiler's
 * own diagnostic -- C1's documented limit (C2 compiles whole turns).  No type
 * definitions ride along either: step 3 admits only scalar signatures, so a
 * signature can never name one. */

#include "inline_c_jit.h"

#include "buf.h"
#include "diag.h"
#include "expr.h"
#include "forms.h"
#include "types.h"
#include "../runtime/experiments.h"
#include "../runtime/globals.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const TuriInlineCJitHook *g_hook = NULL;

void turi_set_inline_c_jit_hook(const TuriInlineCJitHook *hook) {
    g_hook = hook;
}

/* The emitter's NAME__ffi shim (emit_module.c, emit_ffi_export_shims). */
typedef void (*InlineCShim)(const int64_t *iv, const double *fv,
                            int64_t *out_i, double *out_f);

typedef struct JitEntry {
    const FnDef *fn;
    InlineCShim  shim;      /* NULL when refused */
    char        *refusal;   /* malloc'd, when refused */
} JitEntry;

/* Process-lifetime, like the images the shims point into.  Linear: an entry
 * exists only for an inline-C defn the interpreter could not run AND that was
 * called, which is a handful per session. */
static JitEntry *g_cache = NULL;
static uint32_t  g_n_cache = 0, g_cap_cache = 0;

static JitEntry *cache_find(const FnDef *fn) {
    for (uint32_t i = 0; i < g_n_cache; i++)
        if (g_cache[i].fn == fn) return &g_cache[i];
    return NULL;
}

static JitEntry *cache_add(const FnDef *fn) {
    if (g_n_cache == g_cap_cache) {
        uint32_t nc = g_cap_cache ? g_cap_cache * 2 : 8;
        JitEntry *na = (JitEntry *)realloc(g_cache, nc * sizeof(*na));
        if (!na) return NULL;
        g_cache = na;
        g_cap_cache = nc;
    }
    JitEntry *e = &g_cache[g_n_cache++];
    e->fn = fn;
    e->shim = NULL;
    e->refusal = NULL;
    return e;
}

static void entry_refuse(JitEntry *e, const char *fmt, ...)
    TUR_PRINTF_FMT(2, 3);
static void entry_refuse(JitEntry *e, const char *fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    e->refusal = strdup(msg);
}

/* The register class the __ffi shim reads a slot from.  Mirrors
 * ffi_shim_class_for_kind in emit_module.c, which decides whether the shim is
 * emitted at all: 'i' int-register, 'f' float, 'v' void return, '?' none. */
static char slot_class(TypeKind k, bool is_return) {
    switch (k) {
        case TY_NIL:      return is_return ? 'v' : '?';
        case TY_BOOL:
        case TY_INT:
        case TY_CSTR:
        case TY_PTR_VOID:
        case TY_INT8:  case TY_INT16:  case TY_INT32:  case TY_INT64:
        case TY_UINT8: case TY_UINT16: case TY_UINT32: case TY_UINT64:
            return 'i';
        case TY_FLOAT:
        case TY_FLOAT32:
        case TY_FLOAT64:
            return 'f';
        default:
            return '?';
    }
}

/* A slot type, named for a refusal message.  type_print spells every ADT
 * `<adt>`, which tells the user nothing about which parameter it is. */
static void slot_type_name(Buf *b, Type t) {
    if (t.kind == TY_ADT && t.as.adt_.def && t.as.adt_.def->name)
        buf_puts(b, t.as.adt_.def->name);
    else
        type_print(b, t);
    buf_putc(b, '\0');
}

/* The declared result.  fn->return_type keeps only the KIND of some results
 * (an opaque ADT reads as a plain ADT, see the inline-C re-tag in eval.c), so
 * prefer the binding's full fn type when it carries one. */
static Type result_type(const FnDef *fn) {
    if (fn->binding && fn->binding->type.kind == TY_FN &&
        fn->binding->type.as.fn.result_full_type)
        return *fn->binding->type.as.fn.result_full_type;
    return fn->return_type;
}

/* Step 4, preferred: the defn's text as written.  Only for a file the plain
 * reader can read back verbatim -- the default reader, or curly-infix, which
 * every dialect enables -- and only when the span really is a `(defn` form (a
 * macro expansion's claim carries its call site's span). */
static bool defn_source_slice(const FnDef *fn, Buf *out) {
    const Binding *b = fn->binding;
    const Span s = b->defn_claim;
    if (s.line == 0 || s.off_end <= s.off_start) return false;
    const SourceFile *sf = diag_source_file(s.file_id);
    if (!sf || !sf->src || s.off_end > sf->len) return false;
    if (sf->lang != LANG_TURMERIC) return false;
    if (sf->reader_type != READER_TURMERIC &&
        sf->reader_type != READER_CURLY_INFIX)
        return false;
    const char *p = sf->src + s.off_start;
    size_t n = (size_t)(s.off_end - s.off_start);
    if (n < 6 || memcmp(p, "(defn", 5) != 0) return false;
    buf_write(out, p, n);
    return true;
}

/* Step 4, fallback: the most recent top-level `(defn NAME ...)` Form the
 * session read, printed back as an s-expression.  Covers the neoteric and
 * sweet readers, whose text the plain reader cannot parse. */
static bool defn_source_form(const TuriEnv *env, const char *name, Buf *out) {
    for (uint32_t i = env->n_acc_forms; i-- > 0;) {
        const Form *f = env->acc_forms[i];
        if (!f || f->tag != F_LIST || f->as.list.len < 3) continue;
        const Form *h = f->as.list.items[0];
        if (h->tag != F_SYM || strcmp(h->as.sym->name, "defn") != 0) continue;
        const Form *nm = f->as.list.items[1];
        if (nm->tag != F_SYM || strcmp(nm->as.sym->name, name) != 0) continue;
        form_print(out, f);
        return true;
    }
    return false;
}

/* The manifest line for NAME is `<module>/NAME -> <cname> :: (...) -> R`. */
static char *manifest_cname(const char *manifest, const char *name) {
    size_t nl = strlen(name);
    for (const char *line = manifest; line && *line;) {
        const char *eol = strchr(line, '\n');
        const char *slash = memchr(line, '/', eol ? (size_t)(eol - line)
                                                  : strlen(line));
        if (slash && strncmp(slash + 1, name, nl) == 0 &&
            strncmp(slash + 1 + nl, " -> ", 4) == 0) {
            const char *c = slash + 1 + nl + 4;
            const char *ce = strstr(c, " :: ");
            if (ce && (!eol || ce < eol)) {
                char *r = (char *)malloc((size_t)(ce - c) + 1);
                if (!r) return NULL;
                memcpy(r, c, (size_t)(ce - c));
                r[ce - c] = '\0';
                return r;
            }
        }
        line = eol ? eol + 1 : NULL;
    }
    return NULL;
}

/* Steps 3-5: fill a fresh cache entry with a shim or a refusal. */
static void compile_entry(TuriEnv *env, FnDef *fn, uint32_t param_offset,
                          JitEntry *e) {
    const char *name = fn->binding->name->name;

    if (fn->is_variadic) {
        entry_refuse(e, "eval: inline-C defn '%s' is variadic; the REPL JIT "
                        "(repl-jit-inline-c) cannot call it yet", name);
        return;
    }
    for (uint32_t i = param_offset; i < fn->n_params; i++) {
        if (slot_class(fn->param_types[i].kind, false) == '?') {
            Buf t;
            buf_init(&t);
            /* param_types keeps only the kind of an ADT ("for codegen");
             * the parameter's own binding has the declared type. */
            slot_type_name(&t, fn->params[i] ? fn->params[i]->type
                                             : fn->param_types[i]);
            entry_refuse(e, "eval: inline-C defn '%s' takes a parameter of "
                            "type %s; the REPL JIT (repl-jit-inline-c) passes "
                            "only int-class, float, cstr and bool values",
                         name, t.data);
            buf_free(&t);
            return;
        }
    }
    Type rt = result_type(fn);
    if (slot_class(rt.kind, true) == '?') {
        Buf t;
        buf_init(&t);
        slot_type_name(&t, rt);
        entry_refuse(e, "eval: inline-C defn '%s' returns %s; the REPL JIT "
                        "(repl-jit-inline-c) returns only int-class, float, "
                        "cstr, bool and unit results", name, t.data);
        buf_free(&t);
        return;
    }

    Buf defn;
    buf_init(&defn);
    if (!defn_source_slice(fn, &defn) && !defn_source_form(env, name, &defn)) {
        buf_free(&defn);
        entry_refuse(e, "eval: inline-C defn '%s': the REPL JIT "
                        "(repl-jit-inline-c) could not find its source form "
                        "(a defn written by a macro is not supported)", name);
        return;
    }
    Buf prog;
    buf_init(&prog);
    buf_printf(&prog, "(defmodule __repl-jit\n  (export %s)\n", name);
    buf_write(&prog, defn.data, defn.len);
    buf_puts(&prog, ")\n");
    buf_free(&defn);

    void *image = NULL;
    char *manifest = NULL;
    int rc = g_hook->build(name, prog.data, prog.len, &image, &manifest);
    buf_free(&prog);
    if (rc != 0 || !image || !manifest) {
        free(manifest);
        entry_refuse(e, "eval: inline-C defn '%s' could not be compiled by the "
                        "REPL JIT (repl-jit-inline-c); see the diagnostics "
                        "above.  Only the defn itself is compiled, so a body "
                        "that calls another Turmeric definition is not "
                        "supported", name);
        return;
    }
    char *cname = manifest_cname(manifest, name);
    free(manifest);
    InlineCShim shim = NULL;
    if (cname) {
        size_t cl = strlen(cname);
        char *shim_name = (char *)malloc(cl + sizeof "__ffi");
        if (shim_name) {
            memcpy(shim_name, cname, cl);
            memcpy(shim_name + cl, "__ffi", sizeof "__ffi");
            /* Function pointer through void*: the same conversion dlsym's
             * every caller makes; the image API returns void*. */
            void *p = g_hook->sym(image, shim_name);
            memcpy(&shim, &p, sizeof shim);
            free(shim_name);
        }
        free(cname);
    }
    /* The image stays resident either way (see the header comment). */
    if (!shim) {
        entry_refuse(e, "eval: inline-C defn '%s' compiled, but the REPL JIT "
                        "(repl-jit-inline-c) found no call shim for it",
                     name);
        return;
    }
    e->shim = shim;
}

/* The value boundary.  The signature check above admits pointer-shaped slots
 * (ptr<void>, `:int`, an un-annotated parameter), and an interpreter value can
 * reach compiled code through one as a bare word: compiled code then reads it
 * with the compiled layout and frees or keeps memory the interpreter allocated
 * from its own arenas, which takes the REPL process down.  Two checks close
 * the cases the runtime can see; see the plan's C1 "Supported subset".
 *
 * 1. Words compiled code has handed back: every non-zero int-class result of
 *    a JIT'd call.  A pointer parameter accepts only these and nil -- a handle
 *    compiled code made is safe to hand back to compiled code.  Open
 *    addressing, process-lifetime, like the images the handles point into. */
static int64_t *g_origin = NULL;
static uint32_t g_n_origin = 0, g_cap_origin = 0;

static uint32_t origin_hash(int64_t w, uint32_t cap) {
    uint64_t h = (uint64_t)w * 0x9E3779B97F4A7C15ull;
    return (uint32_t)(h >> 32) & (cap - 1);
}

static bool origin_has(int64_t w) {
    if (!g_cap_origin) return false;
    for (uint32_t i = origin_hash(w, g_cap_origin);;
         i = (i + 1) & (g_cap_origin - 1)) {
        if (g_origin[i] == 0) return false;
        if (g_origin[i] == w) return true;
    }
}

static void origin_add(int64_t w) {
    if (w == 0 || origin_has(w)) return;
    if ((g_n_origin + 1) * 2 > g_cap_origin) {
        uint32_t nc = g_cap_origin ? g_cap_origin * 2 : 64;
        int64_t *na = (int64_t *)calloc(nc, sizeof *na);
        if (!na) return;
        for (uint32_t i = 0; i < g_cap_origin; i++) {
            if (!g_origin[i]) continue;
            uint32_t j = origin_hash(g_origin[i], nc);
            while (na[j]) j = (j + 1) & (nc - 1);
            na[j] = g_origin[i];
        }
        free(g_origin);
        g_origin = na;
        g_cap_origin = nc;
    }
    uint32_t j = origin_hash(w, g_cap_origin);
    while (g_origin[j]) j = (j + 1) & (g_cap_origin - 1);
    g_origin[j] = w;
    g_n_origin++;
}

/* A parameter declared as a pointer.  (An un-annotated parameter is one
 * untyped word too, but it carries a number at least as often as a handle, so
 * it gets only check 2.) */
static bool handle_param(const FnDef *fn, uint32_t i) {
    return fn->param_types[i].kind == TY_PTR_VOID;
}

/* 2. True when W is the address of memory the interpreter owns: its value
 *    arenas, or a collection it tracks.  Checked for every int-class slot. */
static bool interp_heap_word(const TuriEnv *env, int64_t w) {
    if (w == 0) return false;
    const void *p = (const void *)(intptr_t)w;
    if (arena_owns(&env->value_scratch, p) || arena_owns(&env->value_perm, p))
        return true;
    for (const TuriCollBuf *c = env->coll_bufs; c; c = c->next)
        if (c->box == p) return true;
    return false;
}

bool turi_inline_c_jit_try(TuriEnv *env, FnDef *fn, uint32_t param_offset,
                           TuriValue *args, uint32_t n_args, TuriValue *out) {
    if (!g_opt_repl_jit_inline_c || !g_hook || !fn || !fn->binding ||
        !fn->body || fn->body->kind != EX_INLINE_C)
        return false;

    static bool warned = false;
    if (!warned) {
        experiment_warn_if_used("repl-jit-inline-c");
        warned = true;
    }

    JitEntry *e = cache_find(fn);
    if (!e) {
        e = cache_add(fn);
        if (!e) {
            *out = turi_error("eval: out of memory");
            return true;
        }
        compile_entry(env, fn, param_offset, e);
    }
    if (!e->shim) {
        *out = turi_error(e->refusal ? e->refusal
                                     : "eval: inline-C defn could not be "
                                       "compiled by the REPL JIT");
        return true;
    }

    /* Step 6.  Arity was already checked by the caller (eval_apply_driven),
     * and slot classes by compile_entry, so only the VALUES can mismatch. */
    int64_t i_inl[8];
    double  f_inl[8];
    int64_t *iv = i_inl;
    double  *fv = f_inl;
    if (n_args > 8) {
        iv = (int64_t *)calloc(n_args, sizeof *iv);
        fv = (double *)calloc(n_args, sizeof *fv);
        if (!iv || !fv) {
            free(iv == i_inl ? NULL : iv);
            free(fv == f_inl ? NULL : fv);
            *out = turi_error("eval: out of memory");
            return true;
        }
    }
    const char *name = fn->binding->name->name;
    TuriValue result = turi_nil();
    bool ok = true;
    for (uint32_t k = 0; k < n_args && ok; k++) {
        const TuriValue *v = &args[k];
        iv[k] = 0;
        fv[k] = 0.0;
        if (slot_class(fn->param_types[param_offset + k].kind, false) == 'f') {
            if (v->tag == TURI_FLOAT)    fv[k] = v->as_float;
            else if (v->tag == TURI_INT) fv[k] = (double)v->as_int;
            else ok = false;
        } else {
            switch (v->tag) {
                case TURI_INT:
                    if (v->as_int != 0 && handle_param(fn, param_offset + k) &&
                        !origin_has(v->as_int)) {
                        result = turi_errorf(
                            "eval: inline-C defn '%s' argument %u is not a "
                            "handle compiled code returned; the REPL JIT "
                            "(repl-jit-inline-c) passes a pointer parameter "
                            "only a value an earlier JIT'd call returned, or "
                            "nil", name, (unsigned)k);
                        ok = false;
                        goto next_arg;
                    }
                    if (interp_heap_word(env, v->as_int)) {
                        result = turi_errorf(
                            "eval: inline-C defn '%s' argument %u is a value "
                            "the interpreter allocated; the REPL JIT "
                            "(repl-jit-inline-c) cannot hand it to compiled "
                            "code, which would read, free or keep it as if "
                            "compiled code had made it", name, (unsigned)k);
                        ok = false;
                        goto next_arg;
                    }
                    iv[k] = v->as_int;
                    break;
                case TURI_BOOL: iv[k] = v->as_bool ? 1 : 0; break;
                case TURI_CSTR: iv[k] = (int64_t)(intptr_t)v->as_cstr; break;
                case TURI_NIL:  iv[k] = 0; break;
                default:        ok = false; break;
            }
        }
        if (!ok)
            result = turi_errorf("eval: inline-C defn '%s' argument %u: the "
                                 "REPL JIT cannot pass this value as a C "
                                 "scalar", name, (unsigned)k);
    next_arg:;
    }
    if (ok) {
        int64_t out_i = 0;
        double  out_f = 0.0;
        e->shim(iv, fv, &out_i, &out_f);
        Type rt = result_type(fn);
        switch (rt.kind) {
            case TY_NIL:     result = turi_nil(); break;
            case TY_BOOL:    result = turi_bool(out_i != 0); break;
            case TY_CSTR:    result = turi_cstr((const char *)(intptr_t)out_i); break;
            case TY_FLOAT:
            case TY_FLOAT32:
            case TY_FLOAT64: result = turi_float(out_f); break;
            default:         result = turi_int(out_i); origin_add(out_i); break;
        }
    }
    if (iv != i_inl) free(iv);
    if (fv != f_inl) free(fv);
    *out = result;
    return true;
}
