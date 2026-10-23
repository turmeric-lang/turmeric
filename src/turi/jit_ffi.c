/* jit_ffi.c -- the c2mir-backed dynamic-FFI provider (jit-ffi-c2mir-plan,
 * F1).  Compiled into tur_jit_obj (JIT builds only), installed from the
 * host's startup via tur_jit_ffi_install; tur_core consumers reach it only
 * through the TurJitFfiProvider hook in jit_ffi_hook.c.
 *
 * A thunk is a ~10-line C function rendered from the signature string --
 * the exact cast-and-call the generated per-export __ffi shims bake in at
 * spice-build time, but synthesized at runtime for the precise signature,
 * so there is no --max-arity ceiling and no shape-table regeneration.  It
 * is compiled through the engine's own image path (c2mir -> MIR_link ->
 * MIR_gen), cached per signature, and kept for the process lifetime --
 * compile cost is milliseconds per UNIQUE signature, amortized against
 * tree-walking dispatch overhead.
 *
 * c2mir compile errors surface as a NULL thunk with a message in the
 * caller's errbuf (the engine already printed specifics to stderr); they
 * never abort. */
#include "jit_ffi.h"

#ifdef _WIN32
/* MinGW ships no <dlfcn.h>.  Same guard jit_engine.c already carries -- see
 * the note there: host-symbol resolution goes through platform_dl.h so the
 * tree has one LoadLibrary shim rather than a second, subtly-different one.
 *
 * This file reached main unguarded because the Windows CI job configures
 * WITHOUT -DTUR_JIT=ON, so nothing on Windows compiles the JIT sources. */
#  include "platform_dl.h"
#else
#  include <dlfcn.h>
#endif
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jit_engine.h"
#include "runtime/buf.h"   /* TUR_PRINTF_FMT */

/* ------------------------------------------------------------------ */
/* signature -> C source                                               */
/* ------------------------------------------------------------------ */

/* Upper bound on thunked arity.  Not an ABI limit -- just a sanity cap on
 * the source buffer; raise freely if a real signature ever hits it. */
#define JIT_FFI_MAX_ARGS 256

static const char *class_c_type(char c) {
    switch (c) {
        case 'i': return "long long";
        /* Exact-width scalar classes (scalar-width-fidelity): the RETURN
         * width must be the callee's true C type or the register's upper
         * bits read back as garbage; the C cast to long long then extends
         * by the signedness the case carries. */
        case 'b': return "signed char";
        case 'B': return "unsigned char";
        case 'h': return "short";
        case 'H': return "unsigned short";
        case 'w': return "int";
        case 'W': return "unsigned int";
        case 'f': return "double";
        case 'F': return "float";
        case 'v': return "void";
        default:  return NULL;
    }
}

/* Integer-register scalar classes (everything class_c_type knows that is
 * neither float nor void nor aggregate). */
static int class_is_int(char c) {
    return c == 'i' || c == 'b' || c == 'B' || c == 'h' || c == 'H' ||
           c == 'w' || c == 'W';
}

/* F4: the exact-width member vocabulary (see jit_ffi.h).  Only the widths
 * matter to layout and classification, so one spelling per width. */
static const char *member_c_type(char c) {
    switch (c) {
        case 'b': return "signed char";
        case 'h': return "short";
        case 'w': return "int";
        case 'q': return "long long";
        case 'p': return "void *";
        case 'F': return "float";
        case 'f': return "double";
        default:  return NULL;
    }
}

/* ------------------------------------------------------------------ */
/* growable source buffer                                              */
/* ------------------------------------------------------------------ */

/* Struct typedefs make the source length hard to bound up front, so the
 * renderer appends rather than sizing a fixed buffer.  `bad` latches an
 * allocation failure so callers check once at the end. */
typedef struct { char *p; size_t len, cap; int bad; } SrcBuf;

static void sb_putn(SrcBuf *b, const char *s, size_t n) {
    if (b->bad) return;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 512;
        while (cap < b->len + n + 1) cap *= 2;
        char *np = (char *)realloc(b->p, cap);
        if (!np) { b->bad = 1; return; }
        b->p = np; b->cap = cap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

static void sb_puts(SrcBuf *b, const char *s) { sb_putn(b, s, strlen(s)); }

static void sb_printf(SrcBuf *b, const char *fmt, ...) TUR_PRINTF_FMT(2, 3);
static void sb_printf(SrcBuf *b, const char *fmt, ...) {
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) { b->bad = 1; return; }
    if ((size_t)n >= sizeof tmp) { b->bad = 1; return; }
    sb_putn(b, tmp, (size_t)n);
}

/* ------------------------------------------------------------------ */
/* parsed signature                                                    */
/* ------------------------------------------------------------------ */

/* One return or parameter slot.  `cls` is the scalar class, or '{' for an
 * aggregate, in which case sbeg/slen delimit the aggregate's sig text and
 * `tid` names the typedef the renderer emitted for it. */
typedef struct {
    char        cls;
    const char *sbeg;
    size_t      slen;
    int         tid;
} SigSlot;

typedef struct {
    SigSlot ret;
    SigSlot args[JIT_FFI_MAX_ARGS];
    int     n;
} ParsedSig;

/* Consume one slot at *pp.  `is_return` admits 'v'.  Returns 0 on success. */
static int slot_parse(const char **pp, SigSlot *out, int is_return) {
    const char *p = *pp;
    if (*p == '{') {
        const char *after =
            tur_jit_ffi_struct_layout(p, NULL, NULL, NULL, NULL, 0, NULL);
        if (!after) return -1;
        out->cls  = '{';
        out->sbeg = p;
        out->slen = (size_t)(after - p);
        out->tid  = -1;
        *pp = after;
        return 0;
    }
    if (!class_is_int(*p) && *p != 'f' && *p != 'F' &&
        !(is_return && *p == 'v'))
        return -1;
    out->cls  = *p;
    out->sbeg = NULL;
    out->slen = 0;
    out->tid  = -1;
    *pp = p + 1;
    return 0;
}

/* Validate "ret:args" into `out`.  Returns the arg count, or -1. */
static int sig_parse(const char *sig, ParsedSig *out) {
    if (!sig || !sig[0]) return -1;
    const char *p = sig;
    if (slot_parse(&p, &out->ret, 1) != 0) return -1;
    if (*p != ':') return -1;
    p++;
    out->n = 0;
    while (*p) {
        if (out->n >= JIT_FFI_MAX_ARGS) return -1;
        if (slot_parse(&p, &out->args[out->n], 0) != 0) return -1;
        out->n++;
    }
    return out->n;
}

/* Render the C declaration of the aggregate at `s` (points at '{') into `b`,
 * as `struct { <members> }`.  Returns a pointer just past the matching '}'.
 * Nested aggregates render inline, which is what makes a sig string a
 * complete, self-describing layout: nothing outside it has to be consulted,
 * so the thunk cache stays keyed on the sig alone with no registry to keep
 * in sync between the elaborator and turi. */
static const char *render_struct_c(SrcBuf *b, const char *s) {
    if (!s || *s != '{') { b->bad = 1; return NULL; }
    sb_puts(b, "struct { ");
    const char *p = s + 1;
    int i = 0;
    while (*p && *p != '}') {
        if (*p == '{') {
            const char *after = render_struct_c(b, p);
            if (!after) return NULL;
            sb_printf(b, " m%d; ", i++);
            p = after;
        } else {
            const char *ct = member_c_type(*p);
            if (!ct) { b->bad = 1; return NULL; }
            sb_printf(b, "%s m%d; ", ct, i++);
            p++;
        }
    }
    if (*p != '}') { b->bad = 1; return NULL; }
    sb_puts(b, "}");
    return p + 1;
}

/* Emit one typedef per DISTINCT aggregate in the signature and record its id
 * on every slot that uses it. */
static void render_struct_typedefs(SrcBuf *b, ParsedSig *ps) {
    SigSlot *slots[JIT_FFI_MAX_ARGS + 1];
    int n_slots = 0;
    if (ps->ret.cls == '{') slots[n_slots++] = &ps->ret;
    for (int i = 0; i < ps->n; i++)
        if (ps->args[i].cls == '{') slots[n_slots++] = &ps->args[i];

    int next_tid = 0;
    for (int i = 0; i < n_slots; i++) {
        for (int j = 0; j < i; j++) {
            if (slots[j]->slen == slots[i]->slen &&
                memcmp(slots[j]->sbeg, slots[i]->sbeg, slots[i]->slen) == 0) {
                slots[i]->tid = slots[j]->tid;
                break;
            }
        }
        if (slots[i]->tid >= 0) continue;
        slots[i]->tid = next_tid++;
        sb_puts(b, "typedef ");
        render_struct_c(b, slots[i]->sbeg);
        sb_printf(b, " T%d;\n", slots[i]->tid);
    }
}

static void render_slot_c_type(SrcBuf *b, const SigSlot *s) {
    if (s->cls == '{') sb_printf(b, "T%d", s->tid);
    else               sb_puts(b, class_c_type(s->cls));
}

/* Render the thunk TU for `sig`.  Caller frees. */
static char *render_thunk_source(ParsedSig *ps) {
    SrcBuf b = { NULL, 0, 0, 0 };
    int n = ps->n;

    render_struct_typedefs(&b, ps);

    sb_puts(&b,
        "void __tur_ffi_thunk(void *fn, const long long *iv,\n"
        "                     const double *fv, void *sv,\n"
        "                     long long *out_i, double *out_f, void *out_s)\n"
        "{\n"
        "  (void)sv; (void)out_s; (void)iv; (void)fv;\n"
        "  (void)out_i; (void)out_f;\n  ");
    /* Store expression prefix by return class.  A narrow integer return
     * extends through the C cast chain: the call expression has the exact
     * declared type, so (long long) sign- or zero-extends correctly.  An
     * aggregate return is stored through out_s, which the caller sized
     * from the same layout. */
    if (class_is_int(ps->ret.cls))
                                 sb_puts(&b, "*out_i = (long long)");
    else if (ps->ret.cls == 'f' || ps->ret.cls == 'F')
                                 sb_puts(&b, "*out_f = (double)");
    else if (ps->ret.cls == '{') sb_printf(&b, "*(T%d *)out_s = ", ps->ret.tid);
    /* The cast: ((RET (*)(A0, A1, ...))fn)(...) */
    sb_puts(&b, "((");
    render_slot_c_type(&b, &ps->ret);
    sb_puts(&b, " (*)(");
    if (n == 0) {
        sb_puts(&b, "void");
    } else {
        for (int i = 0; i < n; i++) {
            if (i) sb_puts(&b, ", ");
            render_slot_c_type(&b, &ps->args[i]);
        }
    }
    sb_puts(&b, "))fn)(");
    for (int i = 0; i < n; i++) {
        /* Position-indexed buffers: arg k reads iv[k], fv[k] or sv[k] by
         * class -- the same convention the interpreter's marshaller and the
         * emitted __ffi shims already share. */
        if (i) sb_puts(&b, ", ");
        switch (ps->args[i].cls) {
            case 'f': sb_printf(&b, "fv[%d]", i); break;
            case 'F': sb_printf(&b, "(float)fv[%d]", i); break;
            case '{': sb_printf(&b, "*(T%d *)(((void *const *)sv)[%d])",
                                ps->args[i].tid, i); break;
            default:  /* integer classes: cast down to the declared width */
                      sb_printf(&b, "(%s)iv[%d]",
                                class_c_type(ps->args[i].cls), i); break;
        }
    }
    sb_puts(&b, ");\n}\n");

    if (b.bad) { free(b.p); return NULL; }
    return b.p;
}

/* ------------------------------------------------------------------ */
/* signature cache                                                     */
/* ------------------------------------------------------------------ */

/* Chained hash keyed by the signature string.  Entries (and their images)
 * are process-lifetime by design, matching turi's closure policy; the
 * mutex is cheap insurance for the day the interpreter grows threads --
 * today's callers are single-threaded. */
typedef struct ThunkEntry {
    char              *sig;
    TurJitFfiThunkFn   thunk;   /* NULL = negative cache (compile failed) */
    struct ThunkEntry *next;
} ThunkEntry;

#define THUNK_BUCKETS 64
static ThunkEntry      *g_buckets[THUNK_BUCKETS];
static pthread_mutex_t  g_cache_lock = PTHREAD_MUTEX_INITIALIZER;

static unsigned sig_hash(const char *s) {
    unsigned h = 2166136261u;
    while (*s) h = (h ^ (unsigned char)*s++) * 16777619u;
    return h % THUNK_BUCKETS;
}

static TurJitFfiThunkFn thunk_for(const char *sig, char *errbuf,
                                  size_t errcap) {
    ParsedSig ps;
    int n = sig_parse(sig, &ps);
    if (n < 0) {
        if (errbuf && errcap)
            snprintf(errbuf, errcap, "malformed ffi signature '%s'",
                     sig ? sig : "(null)");
        return NULL;
    }

    /* F4: refuse an aggregate this host's thunk provider cannot pass
     * correctly, rather than compiling a thunk that miscalls it.  Checked
     * BEFORE the cache so the diagnostic carries the specific reason every
     * time instead of degrading to the generic negative-cache message. */
    {
        const SigSlot *bad = NULL;
        const char *why = NULL;
        if (ps.ret.cls == '{' &&
            !tur_jit_ffi_struct_supported(ps.ret.sbeg, &why))
            bad = &ps.ret;
        for (int i = 0; !bad && i < n; i++)
            if (ps.args[i].cls == '{' &&
                !tur_jit_ffi_struct_supported(ps.args[i].sbeg, &why))
                bad = &ps.args[i];
        if (bad) {
            if (errbuf && errcap)
                snprintf(errbuf, errcap, "%s",
                         why ? why : "unsupported aggregate signature");
            return NULL;
        }
    }

    pthread_mutex_lock(&g_cache_lock);
    unsigned b = sig_hash(sig);
    for (ThunkEntry *e = g_buckets[b]; e; e = e->next) {
        if (strcmp(e->sig, sig) == 0) {
            TurJitFfiThunkFn t = e->thunk;
            pthread_mutex_unlock(&g_cache_lock);
            if (!t && errbuf && errcap)
                snprintf(errbuf, errcap,
                         "ffi thunk for '%s' failed to compile earlier", sig);
            return t;
        }
    }

    /* Miss: render + compile while holding the lock (the engine's compile
     * path is single-threaded state; see jit_engine.c's gen lock notes). */
    TurJitFfiThunkFn thunk = NULL;
    char *src = render_thunk_source(&ps);
    if (src) {
        TurJitImage *img = NULL;
        int rc = tur_jit_compile_image(src, strlen(src), NULL, NULL, 0, &img);
        if (rc == TUR_JIT_OK && img) {
            thunk = (TurJitFfiThunkFn)tur_jit_image_sym(img,
                                                        "__tur_ffi_thunk");
            /* The image stays resident for the process lifetime -- the
             * thunk pointer lives inside it. */
        }
        free(src);
    }

    ThunkEntry *e = (ThunkEntry *)calloc(1, sizeof(*e));
    if (e) {
        e->sig   = strdup(sig);
        e->thunk = thunk;   /* NULL entries negative-cache the failure */
        if (e->sig) {
            e->next = g_buckets[b];
            g_buckets[b] = e;
        } else {
            free(e);
        }
    }
    pthread_mutex_unlock(&g_cache_lock);

    if (!thunk && errbuf && errcap)
        snprintf(errbuf, errcap,
                 "c2mir could not compile an ffi thunk for '%s' "
                 "(see stderr for the compile diagnostic)", sig);
    return thunk;
}

static void *resolve_sym(const char *name) {
    if (!name) return NULL;
#ifdef __EMSCRIPTEN__
    /* wasm-jit-plan 5.3: the same generated table jit_import_resolver uses. */
    extern void *tur_wasm_jit_symtab_lookup(const char *);
    return tur_wasm_jit_symtab_lookup(name);
#else
    return dlsym(RTLD_DEFAULT, name);
#endif
}

/* ------------------------------------------------------------------ */
/* F5: callbacks                                                       */
/* ------------------------------------------------------------------ */

/* A callback is bound to ONE context, so unlike a call thunk it cannot be
 * shared across signatures alone -- the cache is keyed on (sig, ctx).  The
 * pairing matters in a loop: without it, re-evaluating a callback-ptr form
 * would compile a fresh MIR image per iteration and never reclaim any of
 * them (callbacks are process-lifetime by design). */
typedef struct CbEntry {
    char           *sig;
    void           *ctx;
    void           *fnptr;
    struct CbEntry *next;
} CbEntry;

static CbEntry *g_cb_head = NULL;   /* few enough per program for a list */

/* Render the callback TU for `sig` with `ctx` baked in as an address
 * literal.  An aggregate parameter is taken by value and its address handed
 * through the sv channel (the by-value copy lives on the callback's own
 * frame for the duration of the dispatch); an aggregate return is stored
 * into a local through out_s and returned by value.  Caller frees. */
static char *render_callback_source(ParsedSig *ps, void *ctx) {
    SrcBuf b = { NULL, 0, 0, 0 };
    int n = ps->n;

    render_struct_typedefs(&b, ps);

    sb_puts(&b,
        "extern void tur_ffi_cb_dispatch(void *, const long long *,\n"
        "                                const double *,\n"
        "                                const void *const *, long long *,\n"
        "                                double *, void *);\n");
    render_slot_c_type(&b, &ps->ret);
    sb_puts(&b, " __tur_ffi_cb(");
    if (n == 0) {
        sb_puts(&b, "void");
    } else {
        for (int i = 0; i < n; i++) {
            if (i) sb_puts(&b, ", ");
            render_slot_c_type(&b, &ps->args[i]);
            sb_printf(&b, " a%d", i);
        }
    }
    sb_puts(&b, ") {\n");
    /* Sized for n or 1: a zero-length array is not C. */
    sb_printf(&b, "  long long iv[%d]; double fv[%d]; const void *sv[%d];\n",
              n ? n : 1, n ? n : 1, n ? n : 1);
    sb_puts(&b, "  long long oi = 0; double of = 0;\n");
    sb_puts(&b, "  (void)iv; (void)fv; (void)sv;\n");
    for (int i = 0; i < n; i++) {
        if (ps->args[i].cls == '{')
            sb_printf(&b, "  sv[%d] = &a%d;\n", i, i);
        else if (class_is_int(ps->args[i].cls))
            sb_printf(&b, "  iv[%d] = (long long)a%d;\n", i, i);
        else
            sb_printf(&b, "  fv[%d] = (double)a%d;\n", i, i);
    }
    /* Zero-initialized so a dispatch-side mismatch returns zeros, matching
     * the scalar error path. */
    if (ps->ret.cls == '{')
        sb_printf(&b, "  T%d outs = {0};\n", ps->ret.tid);
    /* The context rides as an unsigned literal -- c2mir sees a constant, so
     * there is no relocation and no symbol to resolve. */
    sb_printf(&b,
              "  tur_ffi_cb_dispatch((void *)(unsigned long long)%lluULL,\n"
              "                      iv, fv, (const void *const *)sv,\n"
              "                      &oi, &of, %s);\n",
              (unsigned long long)(uintptr_t)ctx,
              ps->ret.cls == '{' ? "(void *)&outs" : "(void *)0");
    if (ps->ret.cls == 'f')      sb_puts(&b, "  return of;\n");
    else if (ps->ret.cls == 'F') sb_puts(&b, "  return (float)of;\n");
    else if (ps->ret.cls == '{') sb_puts(&b, "  return outs;\n");
    else if (class_is_int(ps->ret.cls))
        sb_printf(&b, "  return (%s)oi;\n", class_c_type(ps->ret.cls));
    sb_puts(&b, "}\n");

    if (b.bad) { free(b.p); return NULL; }
    return b.p;
}

static void *callback_for(const char *sig, void *ctx, char *errbuf,
                          size_t errcap) {
    ParsedSig ps;
    int n = sig_parse(sig, &ps);
    if (n < 0) {
        if (errbuf && errcap)
            snprintf(errbuf, errcap, "malformed callback signature '%s'",
                     sig ? sig : "(null)");
        return NULL;
    }
    /* An aggregate the host's provider cannot pass correctly (an HFA on
     * aarch64) is just as wrong inbound as outbound: the native CALLER puts
     * it in v0..v7 and the c2mir-generated callback reads x0..x7.  Refuse,
     * mirroring thunk_for. */
    {
        const SigSlot *bad = NULL;
        const char *why = NULL;
        if (ps.ret.cls == '{' &&
            !tur_jit_ffi_struct_supported(ps.ret.sbeg, &why))
            bad = &ps.ret;
        for (int i = 0; !bad && i < n; i++)
            if (ps.args[i].cls == '{' &&
                !tur_jit_ffi_struct_supported(ps.args[i].sbeg, &why))
                bad = &ps.args[i];
        if (bad) {
            if (errbuf && errcap)
                snprintf(errbuf, errcap, "%s",
                         why ? why : "unsupported aggregate signature");
            return NULL;
        }
    }

    pthread_mutex_lock(&g_cache_lock);
    for (CbEntry *e = g_cb_head; e; e = e->next) {
        if (e->ctx == ctx && strcmp(e->sig, sig) == 0) {
            void *f = e->fnptr;
            pthread_mutex_unlock(&g_cache_lock);
            return f;
        }
    }

    void *fnptr = NULL;
    char *src = render_callback_source(&ps, ctx);
    if (src) {
        TurJitImage *img = NULL;
        int rc = tur_jit_compile_image(src, strlen(src), NULL, NULL, 0, &img);
        if (rc == TUR_JIT_OK && img)
            fnptr = tur_jit_image_sym(img, "__tur_ffi_cb");
        free(src);
    }
    if (fnptr) {
        CbEntry *e = (CbEntry *)calloc(1, sizeof(*e));
        if (e) {
            e->sig = strdup(sig);
            if (e->sig) {
                e->ctx = ctx; e->fnptr = fnptr;
                e->next = g_cb_head; g_cb_head = e;
            } else {
                free(e);
            }
        }
    }
    pthread_mutex_unlock(&g_cache_lock);

    if (!fnptr && errbuf && errcap)
        snprintf(errbuf, errcap,
                 "c2mir could not compile a callback for '%s' "
                 "(see stderr for the compile diagnostic)", sig);
    return fnptr;
}

static const TurJitFfiProvider g_provider = { thunk_for, resolve_sym,
                                              callback_for };

void tur_jit_ffi_install(void) {
    tur_jit_ffi_set_provider(&g_provider);
}
