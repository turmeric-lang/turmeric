/* emit_dk_runtime.c -- DK delimited-control runtime prelude emitters
 * (cps-backend-unification U7, step 1: "relocate the runtime").
 *
 * Relocated verbatim out of emit_cps.c so the load-bearing DK runtime is no
 * longer entangled with the direct-style lowering functions that U7 deletes.
 * Both the direct emitter (emit_cps.c) and the native CT-IR backend
 * (emit_cps_ir.c) generate calls into the C emitted here. The emitted text is
 * byte-identical to the pre-relocation emitters -- this is a pure code move.
 *
 * See emit_dk_runtime.h for the model and
 * docs/archive/cps-backend-unification-u7-readiness-plan.md for the cut
 * sequence this is the first step of.
 */

#include "emit_dk_runtime.h"
#include "globals.h"  /* compiler config globals */

/* ---- (call/cc f) / (escape f): undelimited escape-continuation runtime ---- */

void emit_cps_callcc_prelude(Buf *out) {
    buf_puts(out,
"/* call-cc-completion: undelimited escape-continuation runtime.\n"
" * (call/cc f)/(escape f) set up a landing here; the continuation handle f\n"
" * receives is &tur_escape_cont. Invoking it (tur_escape_resume) is a one-shot\n"
" * upward escape: it longjmps back to the call/cc site delivering the value.\n"
" * Capture is O(1) and unbounded -- no TUR_CONT_MAX_CAPTURED_FRAMES ceiling. */\n"
"typedef struct tur_escape_cont {\n"
"    tur_jmp_buf buf;\n"
"    int64_t result;  /* value delivered by tur_escape_resume */\n"
"    bool    valid;   /* false once the call/cc prompt has returned */\n"
"} tur_escape_cont;\n"
"/* r7rs-lang-plan R6 (D7): the prompts currently on the stack.  `valid` lives\n"
" * in the prompt's own frame, so once that frame has returned the flag is a\n"
" * dead stack word and reads as whatever ran there since -- a stale (k v)\n"
" * then longjmp'd into garbage instead of reporting.  A continuation is live\n"
" * iff its prompt is in this set; a landing truncates the set to itself, since\n"
" * everything pushed above it was escaped over. */\n"
"#if defined(__GNUC__) || defined(__clang__)\n"
"static TUR_THREAD_LOCAL tur_escape_cont **tur_escape_live;\n"
"static TUR_THREAD_LOCAL int tur_escape_live_n, tur_escape_live_cap;\n"
"#else\n"
"/* A front end without thread-locals (c2mir, `tur jit`): the host keeps a\n"
" * real per-thread slot each (src/runtime/tur_tls.c), as emit_rt_tls does. */\n"
"extern void **tur_tls_escape_live_ptr(void);\n"
"extern int *tur_tls_escape_live_n_ptr(void);\n"
"extern int *tur_tls_escape_live_cap_ptr(void);\n"
"#define tur_escape_live (*(tur_escape_cont ***)tur_tls_escape_live_ptr())\n"
"#define tur_escape_live_n (*tur_tls_escape_live_n_ptr())\n"
"#define tur_escape_live_cap (*tur_tls_escape_live_cap_ptr())\n"
"#endif\n"
"/* A fiber's escapes move with it (tur_fiber_block_resume), so they are read\n"
" * afresh on its side of a yield (TUR_TLS_FRESH). */\n"
"#if defined(TUR_TLS_FRESH) && !defined(tur_escape_live)\n"
"TUR_TLS_FRESH(tur_escape_cont **, tur_escape_live, tur_escape_live__at);\n"
"TUR_TLS_FRESH(int, tur_escape_live_n, tur_escape_live_n__at);\n"
"TUR_TLS_FRESH(int, tur_escape_live_cap, tur_escape_live_cap__at);\n"
"#define tur_escape_live (*tur_escape_live__at())\n"
"#define tur_escape_live_n (*tur_escape_live_n__at())\n"
"#define tur_escape_live_cap (*tur_escape_live_cap__at())\n"
"#endif\n");
    buf_puts(out,
"/* #lang r7rs's dynamic environment (stdlib/r7rs/prelude.tur, r7rs-dyn-ref__):\n"
" * the dynamic-wind stack, the exception-handler stack, parameterize's\n"
" * bindings, and the value a re-entered continuation delivers.  Per thread:\n"
" * as process globals, one thread's raise called another thread's handler and\n"
" * a pop on one removed another's\n"
" * (docs/archive/r7rs-dynamic-environment-shared-across-threads.md).  And per\n"
" * fiber, since it belongs to the code that is running: tur_fiber_block_resume\n"
" * swaps it like the escapes.  All zero on a new thread or fiber, which the\n"
" * prelude reads as the empty list. */\n"
"typedef struct { tur_tagged_t w[4]; } tur_r7rs_dynenv;\n"
"#if defined(__GNUC__) || defined(__clang__)\n"
"static TUR_THREAD_LOCAL tur_r7rs_dynenv tur_r7rs_dyn;\n"
"#else\n"
"extern void **tur_tls_r7rs_dyn_ptr(void);\n"
"#define tur_r7rs_dyn (*(tur_r7rs_dynenv *)tur_tls_r7rs_dyn_ptr())\n"
"#endif\n"
"#if defined(TUR_TLS_FRESH) && !defined(tur_r7rs_dyn)\n"
"TUR_TLS_FRESH(tur_r7rs_dynenv, tur_r7rs_dyn, tur_r7rs_dyn__at);\n"
"#define tur_r7rs_dyn (*tur_r7rs_dyn__at())\n"
"#endif\n");
    buf_puts(out,
"static void tur_escape_live_push(tur_escape_cont *cc) {\n"
"    if (tur_escape_live_n == tur_escape_live_cap) {\n"
"        tur_escape_live_cap = tur_escape_live_cap ? tur_escape_live_cap * 2 : 16;\n"
"        tur_escape_live = (tur_escape_cont **)realloc(tur_escape_live, (size_t)tur_escape_live_cap * sizeof *tur_escape_live);\n"
"        if (!tur_escape_live) { fprintf(stderr, \"tur: oom\\n\"); abort(); }\n"
"    }\n"
"    tur_escape_live[tur_escape_live_n++] = cc;\n"
"}\n"
"static void tur_escape_live_pop(tur_escape_cont *cc) {\n"
"    for (int i = tur_escape_live_n; i > 0; i--)\n"
"        if (tur_escape_live[i - 1] == cc) { tur_escape_live_n = i - 1; return; }\n"
"}\n"
"static int64_t tur_escape_resume(int64_t k, int64_t v) {\n"
"    tur_escape_cont *cc = (tur_escape_cont *)(intptr_t)k;\n"
"    int live = 0;\n"
"    for (int i = 0; i < tur_escape_live_n && cc; i++) if (tur_escape_live[i] == cc) { live = 1; break; }\n"
"    if (!live || !cc->valid) {\n"
"        fflush(stdout);\n"
"        fprintf(stderr, \"tur: continuation invoked after its call/cc prompt returned\\n\");\n"
"        abort();\n"
"    }\n"
"    cc->result = v;\n"
"    TUR_LONGJMP(cc->buf);\n"
"    return 0; /* unreachable */\n"
"}\n"
"/* An `any`-typed call/cc: the resume value is a 16-byte box, and `result`\n"
" * carries one word -- so (k v) hands over a heap copy's address and the\n"
" * landing reads and frees it (emit_callcc).  One malloc per escape. */\n"
"static int64_t __tur_escape_box_any(tur_tagged_t v) {\n"
"    tur_tagged_t *b = (tur_tagged_t *)malloc(sizeof *b);\n"
"    if (!b) { fprintf(stderr, \"tur: oom\\n\"); abort(); }\n"
"    *b = v;\n"
"    return (int64_t)(intptr_t)b;\n"
"}\n"
"\n");
}

/* Bridge: a cloneable continuation whose captured context is a DK chain.
 * Reuses the emitted tur_cloneable_cont struct -- the DK chain rides in `env`,
 * resume dispatches to dk_invoke (multi-shot), clone copies the chain, drop
 * frees it. Emitted after both the cloneable runtime and the DK machine. */
void emit_cps_cloneable_bridge_prelude(Buf *out) {
    buf_puts(out,
"/* cps-transform-plan (CPS9): cloneable continuation <-> DK bridge.\n"
" * The reified delimited context (a DK chain) rides in the cloneable cont's\n"
" * env; resume = dk_invoke (multi-shot, copies internally), clone = chain\n"
" * copy, drop = dk_free. Reuses the existing tur_cloneable_cont machinery. */\n"
"static int64_t __dk_cont_fn(void *env, int64_t value) {\n"
"    return (int64_t)dk_invoke((DK *)env, (intptr_t)value);\n"
"}\n"
"static void *__dk_env_clone(const void *env) {\n"
"    return (void *)dk_copy_range((const DK *)env, NULL);\n"
"}\n"
"static void __dk_env_drop(void *env) { dk_free((DK *)env); }\n"
"\n");
}

/* Serial marshaling runtime: the fixed tagged context frames + the resume /
 * serialize / deserialize builtins. Emitted after the DK machine prelude,
 * gated on preamble_uses_serial (emit_module.c). */
void emit_cps_serial_runtime_prelude(Buf *out) {
    buf_puts(out,
"/* cps-transform-plan (CPS10 / CPS5.4): serializable continuations.\n"
" * The captured continuation is a DK chain of tagged context frames; it is\n"
" * marshaled by writing each frame's stable tag + env (no code addresses), and\n"
" * rebuilt by mapping the tag back to a frame fn. Buffer layout matches stdlib\n"
" * `bytes` (int64 length prefix, then payload). */\n"
"enum { SK_TAG_ADD = 1, SK_TAG_MUL = 2, SK_TAG_SUBR = 3, SK_TAG_SUBL = 4,\n"
"       SK_TAG_DIVR = 5, SK_TAG_DIVL = 6 };\n"
"static intptr_t __sk_add (intptr_t env, intptr_t v) { return env + v; }\n"
"static intptr_t __sk_mul (intptr_t env, intptr_t v) { return env * v; }\n"
"static intptr_t __sk_subr(intptr_t env, intptr_t v) { return env - v; } /* other - hole */\n"
"static intptr_t __sk_subl(intptr_t env, intptr_t v) { return v - env; } /* hole - other */\n"
"static intptr_t __sk_divr(intptr_t env, intptr_t v) {  /* other / hole */\n"
"    return (v) ? (env / v) : (fprintf(stderr, \"division by zero\\n\"), abort(), 0);\n"
"}\n"
"static intptr_t __sk_divl(intptr_t env, intptr_t v) {  /* hole / other */\n"
"    return (env) ? (v / env) : (fprintf(stderr, \"division by zero\\n\"), abort(), 0);\n"
"}\n"
"static DKFrame __sk_frame_for_tag(int tag) {\n"
"    switch (tag) {\n"
"        case SK_TAG_ADD:  return __sk_add;\n"
"        case SK_TAG_MUL:  return __sk_mul;\n"
"        case SK_TAG_SUBR: return __sk_subr;\n"
"        case SK_TAG_SUBL: return __sk_subl;\n"
"        case SK_TAG_DIVR: return __sk_divr;\n"
"        case SK_TAG_DIVL: return __sk_divl;\n"
"        default: return NULL;\n"
"    }\n"
"}\n"
"static int __sk_tag_for_frame(DKFrame f) {\n"
"    if (f == __sk_add)  return SK_TAG_ADD;\n"
"    if (f == __sk_mul)  return SK_TAG_MUL;\n"
"    if (f == __sk_subr) return SK_TAG_SUBR;\n"
"    if (f == __sk_subl) return SK_TAG_SUBL;\n"
"    if (f == __sk_divr) return SK_TAG_DIVR;\n"
"    if (f == __sk_divl) return SK_TAG_DIVL;\n"
"    return 0;  /* not an arithmetic frame -- a call frame (see the registry) */\n"
"}\n"
"/* Call-frame registry: each per-site call frame self-registers (via a C\n"
" * constructor) a stable name <-> DKFrame mapping, so a call frame marshals as\n"
" * its name rather than a code address -- the same name-keyed scheme the\n"
" * interpreter's serial.c uses, here with the emitted C function name. */\n"
"/* env_kind: 0 = int (inline int64), 1 = cstr (length-prefixed bytes),\n"
" * 2 = serializable (marshaled via the env type's Serializable instance). For a\n"
" * serializable env, ser/deser point at __inst_Serializable_{serialize,\n"
" * deserialize}_<T>: ser(env) -> a `bytes` {int64 len; data} buffer; deser(bytes)\n"
" * -> the env value. */\n"
"typedef struct SkReg {\n"
"    const char *name; DKFrame fn; int env_kind;\n"
"    void *(*ser)(int64_t); int64_t (*deser)(void *);\n"
"    struct SkReg *next;\n"
"} SkReg;\n"
"static SkReg *__sk_registry = NULL;\n"
"static void __sk_register(SkReg *r) { r->next = __sk_registry; __sk_registry = r; }\n"
"static DKFrame __sk_call_for_name(const char *name) {\n"
"    for (SkReg *r = __sk_registry; r; r = r->next)\n"
"        if (strcmp(r->name, name) == 0) return r->fn;\n"
"    return NULL;\n"
"}\n"
"static SkReg *__sk_reg_by_name(const char *name) {\n"
"    for (SkReg *r = __sk_registry; r; r = r->next)\n"
"        if (strcmp(r->name, name) == 0) return r;\n"
"    return NULL;\n"
"}\n"
"static SkReg *__sk_reg_for_frame(DKFrame f) {\n"
"    for (SkReg *r = __sk_registry; r; r = r->next)\n"
"        if (r->fn == f) return r;\n"
"    return NULL;\n"
"}\n"
"#define SK_TAG_CALL 100\n"
"#define SK_ENV_INT  0\n"
"#define SK_ENV_CSTR 1\n"
"#define SK_ENV_SER  2\n");
    buf_puts(out,
"/* Resume a (possibly deserialized) serial continuation on a value. */\n"
"static int64_t tur_serial_cont_resume(int64_t k, int64_t v) {\n"
"    return (int64_t)dk_invoke((DK *)(intptr_t)k, (intptr_t)v);\n"
"}\n"
"/* Marshal to a length-prefixed buffer ([int64 payload_len][int64 n][records]).\n"
" * Each record is self-describing. An arithmetic frame is [tag(1..6)][int64 env].\n"
" * A call frame is [SK_TAG_CALL][name_len][name][env_kind][env]: an int env is an\n"
" * inline int64; a cstr env is [int64 len][bytes] -- so a non-int (cstr) env is\n"
" * marshaled by value, not as a code/heap address. All scalars are memcpy'd. */\n"
"static int64_t tur_serial_cont_serialize(int64_t k) {\n");
    /* SX1 (solver-extension-plan 3.5): refuse to serialize inside a trail
     * scope.  Emitted ONLY when stdlib/trail.tur was autoloaded into this
     * compile -- that is also what puts trail.tur's `__tur_autolink__` marker in
     * the output, which is what pulls src/runtime/trail.c into the link.  Gating
     * on anything looser (the experiment bit, or nothing at all) emits a call to
     * `tur_trail_level` that cc cannot resolve. */
    if (g_trail_autoloaded) {
        buf_puts(out,
"    /* A serialized continuation carries CONTROL.  Trailed writes made under an\n"
"     * open bt-scope are not in it, and the undo information that would put them\n"
"     * back is process-local -- so a blob taken here deserializes into a world\n"
"     * where those writes either never happened or can never be unwound.  Both\n"
"     * are silent wrong answers, so this refuses instead.  See\n"
"     * docs/archive/solver-extension-plan.md 3.5. */\n"
"    /* Declared here because this prelude is emitted well ahead of trail.tur's\n"
"     * own extern-c block.  Same return type, so the two declarations are\n"
"     * compatible and whichever lands first wins. */\n"
"    extern int64_t tur_trail_level_i64(void);\n"
"    extern int64_t tur_trail_depth_i64(void);\n"
"    {\n"
"        int64_t __tur_lvl = tur_trail_level_i64();\n"
"        if (__tur_lvl != 0) {\n"
"            fprintf(stderr,\n"
"                    \"tur: cannot serialize a continuation inside a trail scope \"\n"
"                    \"(bt-scope depth %lld, %lld trailed write(s) outstanding)\\n\"\n"
"                    \"  the trail's undo information is process-local and does \"\n"
"                    \"not travel with the blob\\n\"\n"
"                    \"  serialize outside the enclosing bt-scope, or commit the \"\n"
"                    \"level first with bt-commit-to!\\n\",\n"
"                    (long long)__tur_lvl, (long long)tur_trail_depth_i64());\n"
"            abort();\n"
"        }\n"
"    }\n");
    }
    buf_puts(out,
"    DK *p = (DK *)(intptr_t)k;\n"
"    int64_t n = 0, sz = 8;  /* 8 bytes for the frame count */\n"
"    for (DK *q = p; q && q->kind == DKK_FRAME; q = q->next) {\n"
"        n++;\n"
"        if (__sk_tag_for_frame(q->fn)) { sz += 16; continue; }\n"
"        SkReg *r = __sk_reg_for_frame(q->fn);\n"
"        const char *nm = r ? r->name : \"\";\n"
"        int64_t L = (int64_t)strlen(nm);\n"
"        sz += 8 + 8 + L + 8;  /* tag + name_len + name + env_kind */\n"
"        if (r && r->env_kind == SK_ENV_CSTR) {\n"
"            const char *s = (const char *)(intptr_t)q->env;\n"
"            sz += 8 + (int64_t)(s ? strlen(s) : 0);  /* str_len + bytes */\n"
"        } else if (r && r->env_kind == SK_ENV_SER && r->ser) {\n"
"            void *b = r->ser((int64_t)q->env);       /* instance bytes {len;data} */\n"
"            int64_t bl = b ? ((int64_t *)b)[0] : 0;\n"
"            sz += 8 + bl; free(b);                    /* len + data */\n"
"        } else { sz += 8; }                            /* inline int64 */\n"
"    }\n"
"    uint8_t *buf = (uint8_t *)malloc((size_t)(8 + sz));\n"
"    if (!buf) abort();\n"
"    memcpy(buf, &sz, 8);\n"
"    uint8_t *c = buf + 8;\n"
"    memcpy(c, &n, 8); c += 8;\n"
"    for (DK *q = p; q && q->kind == DKK_FRAME; q = q->next) {\n"
"        int t = __sk_tag_for_frame(q->fn);\n"
"        if (t) {\n"
"            int64_t tag = t, env = (int64_t)q->env;\n"
"            memcpy(c, &tag, 8); c += 8; memcpy(c, &env, 8); c += 8;\n"
"            continue;\n"
"        }\n"
"        SkReg *r = __sk_reg_for_frame(q->fn);\n"
"        const char *nm = r ? r->name : \"\";\n"
"        int64_t tag = SK_TAG_CALL, L = (int64_t)strlen(nm);\n"
"        int64_t ek = r ? r->env_kind : SK_ENV_INT;\n"
"        memcpy(c, &tag, 8); c += 8;\n"
"        memcpy(c, &L, 8); c += 8;\n"
"        if (L) { memcpy(c, nm, (size_t)L); c += L; }\n"
"        memcpy(c, &ek, 8); c += 8;\n"
"        if (ek == SK_ENV_CSTR) {\n"
"            const char *s = (const char *)(intptr_t)q->env;\n"
"            int64_t sl = (int64_t)(s ? strlen(s) : 0);\n"
"            memcpy(c, &sl, 8); c += 8;\n"
"            if (sl) { memcpy(c, s, (size_t)sl); c += sl; }\n"
"        } else if (ek == SK_ENV_SER && r && r->ser) {\n"
"            void *b = r->ser((int64_t)q->env);\n"
"            int64_t bl = b ? ((int64_t *)b)[0] : 0;\n"
"            memcpy(c, &bl, 8); c += 8;\n"
"            if (bl) { memcpy(c, (int64_t *)b + 1, (size_t)bl); c += bl; }\n"
"            free(b);\n"
"        } else {\n"
"            int64_t env = (int64_t)q->env;\n"
"            memcpy(c, &env, 8); c += 8;\n"
"        }\n"
"    }\n"
"    return (int64_t)(intptr_t)buf;\n"
"}\n");
    /* security-audit-plan M-1: the deserializer used to trust every length,
     * count, tag and env kind in the buffer, and only bytes->serial-cont
     * validated first -- resume-cont! and image/blob-resume! went straight in.
     * The check now lives here, inside the one function every entry point
     * calls, so there is no unvalidated route.  It is also exposed on its own
     * (tur_serial_cont_check) so bytes->serial-cont can turn a bad buffer into
     * an Err instead of a panic.  The codes index tur_serial_cont_errstr; the
     * stdlib maps each to its own "bytes->serial-cont: ..." text. */
    buf_puts(out,
"/* Validate a marshaled buffer without touching the runtime: every record must\n"
" * fit, the frame count must be possible for the bytes present, every tag must\n"
" * be known, every call frame must name a frame THIS program registered, and\n"
" * the record's env kind must be the kind that frame was registered with -- a\n"
" * cstr frame handed an inline int would dereference it.  Returns 0 when the\n"
" * buffer is safe to rebuild, else an SK_ERR_* code. */\n"
"enum { SK_ERR_NULL = 1, SK_ERR_SHORT, SK_ERR_COUNT, SK_ERR_TRUNC_FRAME,\n"
"       SK_ERR_NAME_LONG, SK_ERR_UNKNOWN, SK_ERR_TRUNC_ENV, SK_ERR_ENV_KIND,\n"
"       SK_ERR_TAG, SK_ERR_ENV_MISMATCH };\n"
"static const char *tur_serial_cont_errstr(int code) {\n"
"    switch (code) {\n"
"        case SK_ERR_NULL:         return \"null bytes\";\n"
"        case SK_ERR_SHORT:        return \"short buffer\";\n"
"        case SK_ERR_COUNT:        return \"bad frame count\";\n"
"        case SK_ERR_TRUNC_FRAME:  return \"truncated frame\";\n"
"        case SK_ERR_NAME_LONG:    return \"frame name too long\";\n"
"        case SK_ERR_UNKNOWN:      return \"unknown frame (written by a different program?)\";\n"
"        case SK_ERR_TRUNC_ENV:    return \"truncated env\";\n"
"        case SK_ERR_ENV_KIND:     return \"bad env kind\";\n"
"        case SK_ERR_TAG:          return \"bad frame tag\";\n"
"        case SK_ERR_ENV_MISMATCH: return \"env kind does not match the frame\";\n"
"        default:                  return \"invalid continuation bytes\";\n"
"    }\n"
"}\n"
"static int tur_serial_cont_check(int64_t bytes) {\n"
"    const int64_t *in = (const int64_t *)(intptr_t)bytes;\n"
"    if (!in) return SK_ERR_NULL;\n"
"    int64_t len = in[0];\n"
"    if (len < 8) return SK_ERR_SHORT;\n"
"    const uint8_t *c = (const uint8_t *)(in + 1), *end = c + len;\n"
"    int64_t n; memcpy(&n, c, 8); c += 8;\n"
"    /* No upper bound needed here: every record consumes at least 8 bytes or\n"
"     * fails the walk below, so a buffer that passes holds all n records and\n"
"     * the rebuild's two n-sized arrays are bounded by the input. */\n"
"    if (n < 0) return SK_ERR_COUNT;\n"
"    for (int64_t i = 0; i < n; i++) {\n"
"        int64_t tag;\n"
"        if (end - c < 8) return SK_ERR_TRUNC_FRAME;\n"
"        memcpy(&tag, c, 8); c += 8;\n"
"        if (tag == SK_TAG_CALL) {\n"
"            int64_t L, ek;\n"
"            if (end - c < 8) return SK_ERR_TRUNC_FRAME;\n"
"            memcpy(&L, c, 8); c += 8;\n"
"            if (L < 0 || end - c < L) return SK_ERR_TRUNC_FRAME;\n"
"            char nm[256];\n"
"            if (L >= (int64_t)sizeof nm) return SK_ERR_NAME_LONG;\n"
"            memcpy(nm, c, (size_t)L); nm[L] = 0; c += L;\n"
"            SkReg *reg = __sk_reg_by_name(nm);\n"
"            if (!reg) return SK_ERR_UNKNOWN;\n"
"            if (end - c < 8) return SK_ERR_TRUNC_FRAME;\n"
"            memcpy(&ek, c, 8); c += 8;\n"
"            if (ek != SK_ENV_INT && ek != SK_ENV_CSTR && ek != SK_ENV_SER)\n"
"                return SK_ERR_ENV_KIND;\n"
"            if (ek != reg->env_kind) return SK_ERR_ENV_MISMATCH;\n"
"            if (ek == SK_ENV_SER && !reg->deser) return SK_ERR_ENV_MISMATCH;\n"
"            if (ek == SK_ENV_INT) {\n"
"                if (end - c < 8) return SK_ERR_TRUNC_ENV;\n"
"                c += 8;\n"
"            } else {\n"
"                int64_t sl;\n"
"                if (end - c < 8) return SK_ERR_TRUNC_ENV;\n"
"                memcpy(&sl, c, 8); c += 8;\n"
"                if (sl < 0 || end - c < sl) return SK_ERR_TRUNC_ENV;\n"
"                c += sl;\n"
"            }\n"
"        } else if (tag >= SK_TAG_ADD && tag <= SK_TAG_DIVL) {\n"
"            if (end - c < 8) return SK_ERR_TRUNC_ENV;\n"
"            c += 8;\n"
"        } else {\n"
"            return SK_ERR_TAG;\n"
"        }\n"
"    }\n"
"    return 0;\n"
"}\n");
    buf_puts(out,
"/* Rebuild a runnable chain [frames..., prompt, done] from a marshaled buffer.\n"
" * Records are parsed forward, then prepended in reverse so the first-written\n"
" * frame stays at the front of the chain. A cstr env is rematerialized as a\n"
" * fresh heap string.  The buffer is validated first: a bad one panics (the\n"
" * T2 promise -- a panic, never a wild read), and if a handler catches that\n"
" * panic the caller gets the identity continuation, which is safe to resume. */\n"
"static int64_t tur_serial_cont_deserialize(int64_t bytes) {\n"
"    int err = tur_serial_cont_check(bytes);\n"
"    if (err) {\n"
"        char msg[128];\n"
"        snprintf(msg, sizeof msg, \"cannot rebuild a serialized continuation: %s\",\n"
"                 tur_serial_cont_errstr(err));\n"
"        tur_panic(msg);\n"
"        return (int64_t)(intptr_t)dk_prompt(1, dk_done());\n"
"    }\n"
"    uint8_t *c = (uint8_t *)(intptr_t)bytes + 8;  /* skip the length prefix */\n"
"    int64_t n; memcpy(&n, c, 8); c += 8;\n"
"    DKFrame *fns = (DKFrame *)malloc(sizeof(DKFrame) * (size_t)(n > 0 ? n : 1));\n"
"    intptr_t *envs = (intptr_t *)malloc(sizeof(intptr_t) * (size_t)(n > 0 ? n : 1));\n"
"    if (!fns || !envs) abort();\n"
"    for (int64_t i = 0; i < n; i++) {\n"
"        int64_t tag; memcpy(&tag, c, 8); c += 8;\n"
"        if (tag == SK_TAG_CALL) {\n"
"            int64_t L; memcpy(&L, c, 8); c += 8;\n"
"            char *nm = (char *)malloc((size_t)L + 1);\n"
"            if (!nm) abort();\n"
"            if (L) memcpy(nm, c, (size_t)L);\n"
"            nm[L] = 0; c += L;\n"
"            fns[i] = __sk_call_for_name(nm);\n"
"            SkReg *reg = __sk_reg_by_name(nm); free(nm);\n"
"            int64_t ek; memcpy(&ek, c, 8); c += 8;\n"
"            if (ek == SK_ENV_CSTR) {\n"
"                int64_t sl; memcpy(&sl, c, 8); c += 8;\n"
"                char *s = (char *)malloc((size_t)sl + 1);\n"
"                if (!s) abort();\n"
"                if (sl) memcpy(s, c, (size_t)sl);\n"
"                s[sl] = 0; c += sl;\n"
"                envs[i] = (intptr_t)s;\n"
"            } else if (ek == SK_ENV_SER) {\n"
"                int64_t bl; memcpy(&bl, c, 8); c += 8;\n"
"                int64_t *bb = (int64_t *)malloc(sizeof(int64_t) + (size_t)bl);\n"
"                if (!bb) abort();\n"
"                bb[0] = bl;\n"
"                if (bl) memcpy(bb + 1, c, (size_t)bl);\n"
"                c += bl;\n"
"                envs[i] = (intptr_t)reg->deser(bb);  /* checked non-NULL */\n"
"                free(bb);\n"
"            } else {\n"
"                int64_t env; memcpy(&env, c, 8); c += 8;\n"
"                envs[i] = (intptr_t)env;\n"
"            }\n"
"        } else {\n"
"            fns[i] = __sk_frame_for_tag((int)tag);\n"
"            int64_t env; memcpy(&env, c, 8); c += 8;\n"
"            envs[i] = (intptr_t)env;\n"
"        }\n"
"    }\n"
"    DK *chain = dk_prompt(1, dk_done());\n"
"    for (int64_t i = n - 1; i >= 0; i--) chain = dk_frame(fns[i], envs[i], chain);\n"
"    free(fns); free(envs);\n"
"    return (int64_t)(intptr_t)chain;\n"
"}\n"
"\n");
}

/* ---- runtime prelude: a faithful C port of src/runtime/cps_prompt.c ----- */


/* The landing pad every emitted setjmp/longjmp pair uses.  Emitted
 * UNCONDITIONALLY and early, because its consumers are not all gated the
 * same way: the DK trampoline, the call/cc escape, the shift/reset and
 * handler-node landings, per-fiber panic recovery and cancellation each
 * appear on their own conditions, and several can appear with no
 * delimited control in the program at all.
 */
void emit_tur_jmp_buf_prelude(Buf *out) {
    /* The tail-resume trampoline's landing pad.  Everywhere but Windows this is
     * plain setjmp/longjmp.
     *
     * On win64 it cannot be: `longjmp` there is a genuine SEH unwind
     * (`_setjmpex` + `RtlUnwindEx`), and RtlUnwindEx validates each frame's RSP
     * against the *thread's* stack bounds as recorded in the TEB.  A fiber
     * stack is malloc'd and the TEB knows nothing about it, so every frame on
     * it is out of bounds and the unwind raises STATUS_BAD_STACK (0xc0000028),
     * killing the process before any output.  This is not a cross-stack mistake
     * that bookkeeping could avoid: a setjmp/longjmp pair BOTH on the fiber
     * stack fails identically, which is exactly what a fiber body does -- its
     * direct->cps entry installs its own landing there.
     *
     * GCC's __builtin_setjmp/__builtin_longjmp are a plain SP/FP/PC
     * save-restore with no unwinder and no TEB check, which is all this
     * trampoline ever wanted.  Measured on a fiber stack: 200 re-armed hops,
     * throws 30-40 frames deep, a nested scoped landing that restores the outer
     * one, and a throw after a swapcontext round trip.  The buffer is 5 words
     * by GCC's definition.
     *
     * The `#if` is on __GNUC__ because the builtins are GCC/clang-only, but the
     * OUTER choice is made HERE, at emission, and baked in as a literal -- it
     * must not be a preprocessor test the two halves of an S2 split program can
     * answer differently.  c2mir has no __builtin_setjmp (verified: no such
     * identifier anywhere in c2mir), and it does not define __GNUC__, so a
     * program half compiled by c2mir would take the plain-setjmp branch while
     * the host-compiled runtime half took the builtin one.  setjmp/longjmp must
     * PAIR, so that mismatch is silent: the program exits 0 having printed
     * nothing.
     *
     * The split used to answer this by keeping plain setjmp on both sides.  That
     * was not, as recorded at the time, "the cc path's fiber-stack defect, left
     * in place on the JIT path": win64 SEH broke the JIT path for a SECOND and
     * independent reason.  RtlUnwindEx calls RtlLookupFunctionEntry on every
     * frame between the longjmp and the setjmp, MIR emits no .pdata/.xdata, and a
     * JIT-generated frame in between therefore raises STATUS_BAD_FUNCTION_TABLE
     * (0xC00000FF) -- not STATUS_BAD_STACK (0xC0000028), and with no fiber
     * involved.  The tell was that fiber+effect fixtures PASSED on the JIT path
     * while fiber-free effect fixtures failed.
     *
     * So the split now calls tur_sjlj_set/_jump (src/async/tur_sjlj_x64_win.S),
     * which restore registers and jump without unwinding anything.  The two halves
     * agree on a SYMBOL rather than on a builtin -- an ordinary extern call is
     * something both a real toolchain and c2mir can emit -- so the "must not be a
     * preprocessor test the halves answer differently" rule above is satisfied
     * without deciding anything at emission time.  See
     * docs/archive/jit-windows-support-spike.md, "Resolution: the JIT longjmp",
     * and docs/archive/windows-longjmp-across-fiber-stack-kills-effects.md for the
     * cc path's separate (fiber-stack) defect. */
    if (rt_split_canonical_emission()) {
        /* The two halves must agree, and c2mir has neither __builtin_setjmp nor
         * __GNUC__ -- so the choice cannot be spelled as a test on the compiler.
         * It CAN be spelled as a test on the platform: c2mir predefines _WIN32
         * (mirc_x86_64_win.h), so both halves answer this identically, and the
         * emitted text is byte-for-byte the same on every host -- which is what
         * keeps the split hash stable and the split engaged.  (An emission-time
         * choice here would make the text platform-dependent and silently
         * disengage S2 on whichever host did not generate the artifacts.)
         *
         * tur_sjlj_set/_jump are an ordinary extern call, so neither half needs a
         * builtin: the host-compiled runtime half links async/tur_sjlj_x64_win.S
         * directly, and the c2mir-compiled program half resolves the symbols
         * through jit_engine.c's JIT_SHIMS table. */
        buf_puts(out,
"/* S2 split emission: one mechanism both halves can name.  See\n"
"   src/async/tur_sjlj_x64_win.S -- win64 longjmp is an SEH unwind that dies\n"
"   on a fiber stack, and c2mir has no __builtin_setjmp. */\n"
"#ifdef _WIN32\n"
"typedef void *tur_jmp_buf[30];\n"
"extern int  tur_sjlj_set(void *);\n"
"extern void tur_sjlj_jump(void *);\n"
"#define TUR_SETJMP(b)  tur_sjlj_set(b)\n"
"#define TUR_LONGJMP(b) tur_sjlj_jump(b)\n"
"#else\n"
"typedef jmp_buf tur_jmp_buf;\n"
"#define TUR_SETJMP(b)  setjmp(b)\n"
"#define TUR_LONGJMP(b) longjmp((b), 1)\n"
"#endif\n");
    } else {
        /* One TU and one compiler here, so the halves always agree -- but the
         * JIT's whole-preamble fallback compiles this TU with c2mir, which has
         * no __GNUC__ and would otherwise take plain setjmp: on Windows that
         * is MinGW's SEH longjmp, which cannot unwind MIR's frames (no
         * .pdata).  The middle arm is that case, and uses the split's
         * no-unwind pair, resolved through jit_engine.c's JIT_SHIMS. */
        buf_puts(out,
"#if defined(_WIN32) && defined(__GNUC__)\n"
"typedef void *tur_jmp_buf[5];\n"
"#define TUR_SETJMP(b)  __builtin_setjmp(b)\n"
"#define TUR_LONGJMP(b) __builtin_longjmp((b), 1)\n"
"#elif defined(_WIN32)\n"
"typedef void *tur_jmp_buf[30];\n"
"extern int  tur_sjlj_set(void *);\n"
"extern void tur_sjlj_jump(void *);\n"
"#define TUR_SETJMP(b)  tur_sjlj_set(b)\n"
"#define TUR_LONGJMP(b) tur_sjlj_jump(b)\n"
"#else\n"
"typedef jmp_buf tur_jmp_buf;\n"
"#define TUR_SETJMP(b)  setjmp(b)\n"
"#define TUR_LONGJMP(b) longjmp((b), 1)\n"
"#endif\n");
    }
}

void emit_cps_runtime_prelude(Buf *out) {
    buf_puts(out,
"/* CPS substrate (cps-transform-plan): multi-prompt delimited-control machine.\n"
" * Heap-reified continuation chains (DK); a reset is a prompt, a shift slices\n"
" * the chain up to the nearest prompt. Faithful port of src/runtime/cps_prompt.c.\n"
" * Capture is O(depth-of-slice) and unbounded -- no 16-frame ceiling. */\n"
"#define DK_ROOT_TAG 0\n"
"typedef struct DK DK;\n"
"typedef intptr_t (*DKFrame)(intptr_t env, intptr_t value);\n"
"typedef intptr_t (*DKBody)(intptr_t env, DK *subk);\n"
"typedef intptr_t (*DKHandler)(intptr_t env, intptr_t arg, DK *subk);\n"
/* A RESUME frame (multi-suspension continuation lowering, Track A): unlike a
 * plain DKK_FRAME value-transform, it receives its run-time downstream chain
 * `rest` (this node's k->next as spliced by dk_perform -- the reinstalled
 * handler tail) and CONSUMES it (dk_run_impl returns rfn's result rather than
 * continuing the loop).  That is what lets a nested control op inside a lifted
 * continuation thread the correct enclosing handler and deliver exactly once. */
"typedef intptr_t (*DKResumeFrame)(intptr_t env, intptr_t value, DK *rest);\n"
/* E3a (cps-backend-owning-env-teardown): owning-env clone/drop glue.  A frame
 * whose captured env holds an owning value carries this pair so a multi-shot
 * resume gets its own refcounted copy instead of a shared shallow alias; both
 * default NULL, in which case dk_copy_node keeps the shallow env-pointer copy
 * and dk_free never touches the env -- byte-identical to the pre-E3a path. */
"typedef intptr_t (*DKEnvClone)(intptr_t env);\n"
"typedef void (*DKEnvDrop)(intptr_t env);\n"
"typedef enum { DKK_DONE, DKK_FRAME, DKK_PROMPT, DKK_SHIFT, DKK_SHIFT0, DKK_HANDLER, DKK_RESUME_FRAME } DKKind;\n"
"struct DK {\n"
"    DKKind kind; DKFrame fn; intptr_t env; int tag;\n"
"    DKBody body; intptr_t body_env;\n"
"    DKHandler handler; intptr_t handler_env; bool shallow;\n"
"    DKResumeFrame rfn; DKEnvClone env_clone; DKEnvDrop env_drop; DK *next;\n"
/* Spine unification (cps-multishot-nontail-resume-inner-handle-drops-clause-
 * rest): a handle-continuation frame's `next` is the ACTUAL enclosing chain --
 * one spine serves dk_perform's search, dk_copy_range's boundary, and the
 * H->next delivery alike -- but that chain has its own reap owner, so this
 * node's free must not walk into it.  borrow_next marks exactly that: dk_free
 * frees this node and stops.  Copies never inherit it (dk_copy_node leaves it
 * false): a dk_copy_range copy OWNS every node it copied, including the ones
 * past the borrow point. */
"    bool borrow_next;  /* ->next is borrowed (another chain owns it): dk_free stops here */\n"
/* Re-opening delivery protocol (cps-case-reopen-marker-kont-truncates-capture):
 * a handler whose case RE-OPENS an outer effect runs with the REAL enclosing
 * chain as its continuation and delivers its own value through it
 * (dk_run(__kont, v) on every exit).  dk_perform's inline branch must then
 * return the case's result verbatim instead of delivering H->next a second
 * time -- the measured failure of the naive real-chain conversion was exactly
 * that double delivery.  The flag is a property of the case FN's protocol, so
 * dk_copy_node carries it (a marker copy's case still delivers for itself). */
"    bool case_delivers;  /* case fn delivers through the chain itself: dk_perform returns its result as-is */\n");
    buf_puts(out,
"    bool tail_resume;  /* E7: this handler tail-resumes -> dk_perform yields to driver */\n"
"    int hgroup;        /* re-opening: same-handle sibling group id (0 = ungrouped);\n"
"                        * distinguishes this handle's cases from an enclosing\n"
"                        * handle's handlers once a re-install flattens the chain */\n"
"    bool consumed;     /* (cont? k): a handler-case continuation is unconsumed until\n"
"                        * the program `resume`s it; set at the user resume site so\n"
"                        * `cont?` can read `!k->consumed` (matches the fiber path). */\n"
/* fn-value-call-cps-frames-held-until-outer-entry: `ncopy` counts the times
 * dk_copy_node has copied this node (saturating at 255), so a node at 0 has no
 * copy anywhere -- nothing shares its env.  `join_once` marks a heap-join frame
 * (dk_frame_join / dk_frame_resume_join): the original runs at most once, and
 * when it does without ever having been copied, dk_run_impl hands it back
 * (__dk_join_release_node) instead of holding it until the outermost entry
 * returns.  Neither is copied by dk_copy_node.
 *
 * async-repeated-park-holds-frames-until-settle: `env_size` is the size of a
 * frame's env struct when the emitter knows it (dk_env_sized: heap joins and
 * await continuations); `env_owned` marks a copy that holds a private byte
 * copy of that env, freed with the node.  A parked continuation is such a copy
 * (dk_copy_range_owned), and so is every copy made from one, so nothing it
 * leaves behind shares an env with it.  `orphaned` / `orphan_env` are set on
 * the ORIGINALS an await's shift ran only as copies (__dk_await_release): the
 * node is unreachable, and with `orphan_env` -- it parked, and the shift's was
 * the node's only copy -- so is its env.  A park's hand-off frees both
 * (__dk_reap_seg_take).  All of it fits the struct's tail padding. */
"    uint8_t ncopy;     /* times dk_copy_node copied this node (255 = many) */\n"
"    bool join_once;    /* heap-join frame: may be reclaimed when it runs uncopied */\n"
"    bool orphaned;     /* an original an await's shift ran only as copies */\n"
"    bool orphan_env;   /* ...and the shift that parked was its only copy */\n"
"    bool env_owned;    /* this copy owns a private copy of the env */\n"
"    uint16_t env_size; /* sizeof the env struct, 0 when unknown */\n");
    buf_puts(out,
"};\n"
"static DK *dk_new(DKKind kind, DK *next) {\n"
"    DK *k = (DK *)calloc(1, sizeof(DK)); k->kind = kind; k->next = next; return k;\n"
"}\n"
"static DK *dk_done(void) { return dk_new(DKK_DONE, NULL); }\n"
"static DK *dk_frame(DKFrame fn, intptr_t env, DK *next) {\n"
"    DK *k = dk_new(DKK_FRAME, next); k->fn = fn; k->env = env; return k;\n"
"}\n"
"/* E3a: a plain frame whose env is owning -- carries the clone/drop pair fired\n"
" * by dk_copy_node / dk_free.  NULL for both is exactly dk_frame. */\n"
"__attribute__((unused))\n"
"static DK *dk_frame_owning(DKFrame fn, intptr_t env,\n"
"                           DKEnvClone env_clone, DKEnvDrop env_drop, DK *next) {\n"
"    DK *k = dk_new(DKK_FRAME, next); k->fn = fn; k->env = env;\n"
"    k->env_clone = env_clone; k->env_drop = env_drop; return k;\n"
"}\n"
"static DK *dk_frame_resume(DKResumeFrame fn, intptr_t env, DK *next) {\n"
"    DK *k = dk_new(DKK_RESUME_FRAME, next); k->rfn = fn; k->env = env; return k;\n"
"}\n"
"/* A heap-join frame (emit_heap_join): the continuation of one non-tail\n"
" * cps->cps call, spliced onto the caller's chain and registered for a\n"
" * single-node reap.  See __dk_join_release_node. */\n"
"__attribute__((unused))\n"
"static DK *dk_frame_join(DKFrame fn, intptr_t env, DK *next) {\n"
"    DK *k = dk_frame(fn, env, next); k->join_once = true; return k;\n"
"}\n"
"__attribute__((unused))\n"
"static DK *dk_frame_resume_join(DKResumeFrame fn, intptr_t env, DK *next) {\n"
"    DK *k = dk_frame_resume(fn, env, next); k->join_once = true; return k;\n"
"}\n"
"/* A handle-continuation resume-frame whose `next` is the ACTUAL enclosing\n"
" * chain, borrowed.  The one spine then serves every consumer: dk_perform's\n"
" * handler search walks straight into the real enclosing handlers, its capture\n"
" * boundary (dk_copy_range(k, H)) stops at the real H, and its delivery\n"
" * (dk_run_impl(H->next, r)) runs the real rest of the program -- exactly once,\n"
" * however many times the handler case resumed its sub-continuation.  The frame\n"
" * fn is a DKResumeFrame, so a COPY of this node threads the COPY's own next\n"
" * (its reified, marker-terminated tail) rather than a baked env pointer to the\n"
" * original chain -- which is what confined a resumed continuation to its\n"
" * delimiter.  borrow_next keeps dk_free out of the enclosing chain (it has its\n"
" * own reap owner). */\n"
"__attribute__((unused))\n"
"static DK *dk_frame_resume_borrow(DKResumeFrame fn, intptr_t env, DK *next) {\n"
"    DK *k = dk_frame_resume(fn, env, next); k->borrow_next = true; return k;\n"
"}\n"
"static DK *dk_prompt(int tag, DK *next) {\n"
"    DK *k = dk_new(DKK_PROMPT, next); k->tag = tag; return k;\n"
"}\n"
"static DK *dk_shift_impl(DKKind kind, int tag, DKBody body, intptr_t env, DK *next) {\n"
"    DK *k = dk_new(kind, next); k->tag = tag; k->body = body; k->body_env = env; return k;\n"
"}\n"
"static DK *dk_shift(int tag, DKBody body, intptr_t env, DK *next) {\n"
"    return dk_shift_impl(DKK_SHIFT, tag, body, env, next);\n"
"}\n"
"static DK *dk_shift0(int tag, DKBody body, intptr_t env, DK *next) {\n"
"    return dk_shift_impl(DKK_SHIFT0, tag, body, env, next);\n"
"}\n"
"static DK *dk_handler_impl(int tag, DKHandler fn, intptr_t env, bool shallow, DK *next) {\n"
"    DK *k = dk_new(DKK_HANDLER, next); k->tag = tag; k->handler = fn; k->handler_env = env; k->shallow = shallow; return k;\n"
"}\n"
"static DK *dk_handler(int tag, DKHandler fn, intptr_t env, DK *next) {\n"
"    return dk_handler_impl(tag, fn, env, false, next);\n"
"}\n"
"static DK *dk_handler_shallow(int tag, DKHandler fn, intptr_t env, DK *next) {\n"
"    return dk_handler_impl(tag, fn, env, true, next);\n"
"}\n"
"/* Mark a handler whose case delivers its own value through the real chain\n"
" * (a RE-OPENING case -- see the case_delivers field).  Composes with any of\n"
" * the dk_handler ctors: dk_case_delivers(dk_handler(...)). */\n"
"__attribute__((unused))\n"
"static DK *dk_case_delivers(DK *k) { k->case_delivers = true; return k; }\n");
    buf_puts(out,
"/* E7: a deep handler whose case tail-resumes -- dk_perform yields to the entry\n"
" * driver instead of resuming inline, keeping deep effectful recursion flat. */\n"
"static DK *dk_handler_tail(int tag, DKHandler fn, intptr_t env, DK *next) {\n"
"    DK *k = dk_handler_impl(tag, fn, env, false, next); k->tail_resume = true; return k;\n"
"}\n"
"/* Re-opening: stamp the maximal run of consecutive DKK_HANDLER nodes starting at\n"
" * `head` (exactly ONE handle's sibling cases -- the run ends at this handle's\n"
" * continuation frame) with a fresh, shared group id.  dk_case_enclosing_real and\n"
" * dk_perform's re-install then skip only same-group handlers, so an enclosing\n"
" * handle's handlers that become ADJACENT after a chain-flattening re-install are\n"
" * no longer mistaken for this handle's siblings (they carry a different id).\n"
" * The counter is process-wide and bumped from every thread, so it is atomic:\n"
" * a plain ++ lost updates, and a counter that goes BACKWARDS can hand one\n"
" * thread the same id twice -- two handles' cases read as siblings (TSan on\n"
" * threads-effects-tail-resume, security-audit-plan WP5).  Not per-thread: a\n"
" * fiber's chain can pick up ids on more than one worker.  Relaxed, because\n"
" * only uniqueness matters. */\n"
"static volatile uint64_t g_dk_hgroup_ctr = 0;\n"
"static DK *dk_hgroup(DK *head) {\n"
"    int g = (int)TUR_ATOMIC_ADD_FETCH_U64(&g_dk_hgroup_ctr, 1, __ATOMIC_RELAXED);\n"
"    for (DK *p = head; p && p->kind == DKK_HANDLER; p = p->next) p->hgroup = g;\n"
"    return head;\n"
"}\n");
    buf_puts(out,
"/* B3: install a DK handler group from a runtime first-class handler table -- the\n"
" * DK-side analogue of running a with-handler body under a dynamic handler value.\n"
" * Each entry contributes a dk_handler(dk_tag, dk_fn, env, ...) node, chained\n"
" * h1-outer (entry 0 outermost, matching tur_handler_table_concat order and the\n"
" * static handle chain) over `base` (the with-handler continuation frame), then\n"
" * stamped as one hgroup.  The DK case fns + tags were emitted at the handler\n"
" * literal's creation site (colored context). */\n"
"__attribute__((unused))\n"
"static DK *dk_hgroup_from_table(const tur_handler_table_t *t, DK *base) {\n"
"    DK *head = base;\n"
"    if (t) for (int i = t->n_entries - 1; i >= 0; i--) {\n"
"        tur_handler_entry_t *e = &t->entries[i];\n"
"        if (!e->dk_fn) continue;  /* case not DK-emittable -> leave unhandled, don't crash */\n"
"        /* The emitted DK case fns always end in a tail `return dk_tail_resume`,\n"
"         * so install as a tail handler (dk_perform yields the resumed chain to\n"
"         * the entry driver), matching the static handle path. */\n"
"        head = dk_handler_tail(e->dk_tag, (DKHandler)e->dk_fn, (intptr_t)e->env, head);\n"
"    }\n"
"    return dk_hgroup(head);\n"
"}\n");
    /* async-repeated-park-holds-frames-until-settle: the byte copy an
     * owning copy holds.  A frame env is a struct of captured words written
     * once at construction and only read after, so a copy of it reads the
     * same as the original -- an owning capture keeps the env's single +1
     * either way (the read-out increfs per run). */
    buf_puts(out,
"static intptr_t __dk_env_dup(intptr_t env, size_t size) {\n"
"    void *c = malloc(size);\n"
"    memcpy(c, (const void *)env, size);\n"
"    return (intptr_t)c;\n"
"}\n"
"/* Record the size of a frame's env struct, so a parked copy can own a copy\n"
" * of it.  Not for an E3a owning frame, whose clone glue already copies it. */\n"
"__attribute__((unused)) static DK *dk_env_sized(DK *k, size_t size) {\n"
"    if (k->env && !k->env_clone && size <= 0xFFFF) k->env_size = (uint16_t)size;\n"
"    return k;\n"
"}\n");
    buf_puts(out,
"static DK *dk_copy_node(const DK *n);\n");
    buf_puts(out,
"static DK *dk_copy_node(const DK *n) {\n"
"    if (n->ncopy != 255) ((DK *)n)->ncopy++;\n"
"    DK *c = dk_new(n->kind, NULL); c->fn = n->fn; c->tag = n->tag;\n"
/* E3a: an owning frame gets an OWNED copy of its env (rc incref / aggregate
 * deep-copy) instead of a shared shallow alias; NULL env_clone keeps the shallow
 * copy.  The clone/drop glue rides along so the copy frees its env symmetrically. */
"    c->env = n->env_clone ? n->env_clone(n->env) : n->env;\n"
"    c->env_clone = n->env_clone; c->env_drop = n->env_drop;\n"
"    c->env_size = n->env_size;\n"
"    if (n->env_owned) { c->env = __dk_env_dup(n->env, n->env_size); c->env_owned = true; }\n"
"    c->body = n->body; c->body_env = n->body_env;\n"
"    c->handler = n->handler; c->handler_env = n->handler_env; c->shallow = n->shallow;\n");
    buf_puts(out,
"    c->tail_resume = n->tail_resume;\n"
"    c->hgroup = n->hgroup;\n");
    buf_puts(out,
/* borrow_next is deliberately NOT copied: dk_copy_range crosses a borrow link
 * like any other ->next, and the copy OWNS everything it copied -- its dk_free
 * must walk the whole copied chain, not stop where the ORIGINAL's ownership
 * boundary happened to sit.  case_delivers IS copied: it describes the case
 * fn's delivery protocol, which a re-installed marker copy shares. */
"    c->case_delivers = n->case_delivers;\n"
"    c->rfn = n->rfn; return c;\n"
"}\n"
"static DK *dk_copy_enclosing_handlers(const DK *from) {\n"
"    DK *head = NULL, *tail = NULL;\n"
"    for (const DK *p = from; p && p->kind != DKK_DONE; p = p->next) {\n"
"        if (p->kind != DKK_HANDLER) continue;\n"
"        DK *c = dk_copy_node(p);\n"
"        if (!head) head = tail = c; else { tail->next = c; tail = c; }\n"
"    }\n"
"    DK *done = dk_done();\n"
"    if (!head) return done;\n"
"    tail->next = done; return head;\n"
"}\n"
"/* Effect re-opening: the REAL enclosing chain in effect at the dynamic point a\n"
" * handler case body runs -- i.e. everything past this handle's sibling group,\n"
" * BORROWED.  A re-opening case takes this as its `__kont`: a `perform` in the\n"
" * case dispatches into the real enclosing handlers, its capture crosses into\n"
" * the real intermediate frames (so a multishot outer resume re-runs them per\n"
" * resume), and the case delivers its own value through the same chain\n"
" * (dk_run(__kont, v) on every exit -- the case_delivers protocol; dk_perform\n"
" * then returns the case's result without a second H->next delivery).  This\n"
" * replaced a done-terminated marker COPY, which truncated the capture at the\n"
" * marker and left the pending delivery as a C-stack frame that a tail-resume\n"
" * longjmp silently discarded (cps-case-reopen-marker-kont-truncates-capture).\n"
" * For a deep handler we skip the whole dk_handler sibling group starting at H\n"
" * (its case shares the handle's continuation context, so a sibling effect\n"
" * propagates OUTWARD, matching dk_perform's own `ge` walk); a shallow handler\n"
" * skips only H itself.  The returned chain is borrowed -- the case must only\n"
" * read, thread, or copy it, never free it. */\n"
"__attribute__((unused))\n"
"static DK *dk_case_enclosing_real(const DK *H) {\n"
"    if (!H) return NULL;\n"
"    const DK *ge = H;\n"
"    if (H->shallow) ge = H->next;\n");
    /* Deep skip: a flattening re-install can make an enclosing handle's handlers
     * ADJACENT to H, so "consecutive DKK_HANDLER" over-skips them.  Skip only H's
     * own sibling GROUP (same hgroup).  Unconditional since cps-tramp-resume
     * graduated (2026-07-19); the historical consecutive-HANDLER walk it used to
     * fall back to is gone. */
    buf_puts(out,
"    else while (ge && ge->kind == DKK_HANDLER && ge->hgroup == H->hgroup) ge = ge->next;\n");
    buf_puts(out,
"    return (DK *)ge;\n"
"}\n"
"static DK *dk_copy_range(const DK *from, const DK *stop) {\n"
"    DK *head = NULL, *tail = NULL;\n"
"    for (const DK *p = from; p && p != stop; p = p->next) {\n"
"        DK *c = dk_copy_node(p);\n"
"        if (!head) head = tail = c; else { tail->next = c; tail = c; }\n"
"    }\n"
"    return head;\n"
"}\n"
"/* A copy whose sized frames own their envs (async-repeated-park-holds-\n"
" * frames-until-settle): a parked continuation, which outlives the entry\n"
" * that built the originals. */\n"
"__attribute__((unused)) static DK *dk_copy_range_owned(const DK *from, const DK *stop) {\n"
"    DK *head = dk_copy_range(from, stop);\n"
"    for (DK *c = head; c; c = c->next)\n"
"        if (c->env_size && !c->env_owned) { c->env = __dk_env_dup(c->env, c->env_size); c->env_owned = true; }\n"
"    return head;\n"
"}\n"
"static DK *dk_append(DK *a, DK *b) {\n"
"    if (!a) return b;\n"
"    DK *p = a;\n"
"    while (p->next) p = p->next;\n"
"    p->next = b;\n"
"    return a;\n"
"}\n"
/* E3a: drop each frame's owning env (if any) before freeing the node.
 * A borrow_next node ends the walk: its ->next belongs to another chain (with
 * its own reap entry), so following it would double-free -- and by reap time
 * the borrowed tail may already be gone, so it must not even be read. */
/* r7rs-lang-plan T5: once a re-entrant continuation exists (the R7RS
 * prelude's r7rs-cont-capture__ sets tur_dk_pinned), a copy of the C stack may
 * hold any live DK node, and re-entering it after its CPS entry returned must
 * find the node intact -- so from then on DK memory is never reclaimed, the
 * interpreter's process-lifetime policy.  Nothing else sets the flag, so every
 * other program frees exactly as before. */
"static int tur_dk_pinned = 0;\n"
/* dynamic-returned-closure-env-is-never-freed: tells code emitted later in the
 * TU (__tur_any_closure_drop) that the flag above is in scope. */
"#define TUR_DK_PIN 1\n"
"static void dk_free(DK *k) { if (tur_dk_pinned) return; while (k) { DK *n = k->borrow_next ? NULL : k->next; if (k->env_drop) k->env_drop(k->env); else if (k->env_owned) free((void *)k->env); free(k); k = n; } }\n");
    buf_puts(out,
"/* Free a single spliced node without following ->next -- used to reclaim the\n"
" * one-off shift/perform node whose ->next points into an enclosing continuation\n"
" * (dk_free would walk into that continuation and risk a double free).  See\n"
" * docs/archive/cps-delimited-dk-node-leak.md. */\n"
"__attribute__((unused)) static void dk_free_node(DK *k) { if (tur_dk_pinned) return; if (k && k->env_drop) k->env_drop(k->env); else if (k && k->env_owned) free((void *)k->env); free(k); }\n");
    buf_puts(out,
"/* E2a: direct-entry -> CPS-entry registry (probes/e2a-registry-probe.c). */\n"
"typedef intptr_t (*__tur_cps_fn)();\n"
"static struct { intptr_t direct; __tur_cps_fn cps; } __tur_cps_reg[256];\n"
"static int __tur_cps_reg_n = 0;\n"
"__attribute__((unused)) static void __tur_cps_register(intptr_t direct, __tur_cps_fn cps) {\n"
"    if (__tur_cps_reg_n < 256) { __tur_cps_reg[__tur_cps_reg_n].direct = direct;\n"
"        __tur_cps_reg[__tur_cps_reg_n].cps = cps; __tur_cps_reg_n++; }\n"
"}\n"
"__attribute__((unused)) static __tur_cps_fn __tur_cps_lookup(intptr_t direct) {\n"
"    for (int i = 0; i < __tur_cps_reg_n; i++)\n"
"        if (__tur_cps_reg[i].direct == direct) return __tur_cps_reg[i].cps;\n"
"    return (__tur_cps_fn)0;\n"
"}\n"
"/* A registry MISS used to be called straight through -- i.e. a call to NULL,\n"
"   which lands as a bare SIGSEGV with nothing pointing at the cause.  Only a\n"
"   value whose CPS entry was registered at startup can thread the handler\n"
"   chain, so a miss is a compiler bug, not a user error; say so and stop. */\n"
"__attribute__((unused)) static __tur_cps_fn __tur_cps_lookup_checked(intptr_t direct,\n"
"                                                                     const char *who) {\n"
"    __tur_cps_fn f = __tur_cps_lookup(direct);\n"
"    if (!f) {\n"
"        fprintf(stderr, \"tur: internal error: no CPS entry registered for \"\n"
"                        \"effectful fn-value '%s' -- it cannot thread the \"\n"
"                        \"effect handler chain\\n\", who ? who : \"?\");\n"
"        abort();\n"
"    }\n"
"    return f;\n"
"}\n");
    buf_puts(out,
"/* Structural-chain reaping (docs/archive/cps-delimited-dk-node-leak.md).\n"
" * reset/handle install a prompt/handler chain as the current continuation and\n"
" * thread it through the delimited body in TAIL position, so the install site\n"
" * never regains control to free it.  Each chain is self-contained (its tail is\n"
" * dk_done; its frame envs carry the enclosing k as an intptr, not via ->next),\n"
" * so once the outermost dk_run has fully settled every registered chain is dead\n"
" * and dk_free-able without aliasing another.  We register each at construction\n"
" * and reap at the outermost direct->cps entry boundary.  The same registry\n"
" * reclaims per-continuation env structs (plain malloc, freed one-by-one).\n"
" *\n"
" * dk-reap-list-shared-across-threads: the registry, the entry depth and the\n"
" * trampoline's state below are per-thread.  Shared, a CPS entry on one thread\n"
" * pushed onto the list another thread was reallocating, a worker's exit that\n"
" * took the depth to 0 freed every thread's live chains, and a worker's entry\n"
" * overwrote the landing another thread's tail-resume longjmps to.  A fiber\n"
" * carries its own registry and depth across its switches (FiberBlock,\n"
" * tur_fiber_block_resume), since it can resume on another thread. */\n"
"#if defined(__GNUC__) || defined(__clang__)\n"
"static TUR_THREAD_LOCAL void **__dk_reap_v;\n"
"static TUR_THREAD_LOCAL unsigned char *__dk_reap_kind;  /* 1 = DK chain (dk_free), 0 = plain (free) */\n"
"static TUR_THREAD_LOCAL size_t __dk_reap_n;\n"
"static TUR_THREAD_LOCAL size_t __dk_reap_cap;\n"
"static TUR_THREAD_LOCAL int __dk_entry_depth;\n"
"/* The current entry-driver landing; NULL runs a tail resume inline. */\n"
"static TUR_THREAD_LOCAL tur_jmp_buf *g_dk_driver;\n"
"static TUR_THREAD_LOCAL DK *g_dk_resume_chain;\n"
"static TUR_THREAD_LOCAL intptr_t g_dk_resume_val;\n"
"static TUR_THREAD_LOCAL DK **g_dk_meta;\n"
"static TUR_THREAD_LOCAL size_t g_dk_meta_n;\n"
"static TUR_THREAD_LOCAL size_t g_dk_meta_cap;\n"
"#else\n"
"/* A front end without thread-locals (c2mir, `tur jit`): the host keeps a\n"
" * real per-thread slot each (src/runtime/tur_tls.c), as emit_rt_tls does. */\n"
"extern void **tur_tls_dk_reap_v_ptr(void);\n"
"extern void **tur_tls_dk_reap_kind_ptr(void);\n"
"extern size_t *tur_tls_dk_reap_n_ptr(void);\n"
"extern size_t *tur_tls_dk_reap_cap_ptr(void);\n"
"extern int *tur_tls_dk_entry_depth_ptr(void);\n"
"extern void **tur_tls_dk_driver_ptr(void);\n"
"extern void **tur_tls_dk_resume_chain_ptr(void);\n"
"extern intptr_t *tur_tls_dk_resume_val_ptr(void);\n"
"extern void **tur_tls_dk_meta_ptr(void);\n"
"extern size_t *tur_tls_dk_meta_n_ptr(void);\n"
"extern size_t *tur_tls_dk_meta_cap_ptr(void);\n"
"#define __dk_reap_v (*(void ***)tur_tls_dk_reap_v_ptr())\n"
"#define __dk_reap_kind (*(unsigned char **)tur_tls_dk_reap_kind_ptr())\n"
"#define __dk_reap_n (*tur_tls_dk_reap_n_ptr())\n"
"#define __dk_reap_cap (*tur_tls_dk_reap_cap_ptr())\n"
"#define __dk_entry_depth (*tur_tls_dk_entry_depth_ptr())\n"
"#define g_dk_driver (*(tur_jmp_buf **)tur_tls_dk_driver_ptr())\n"
"#define g_dk_resume_chain (*(DK **)tur_tls_dk_resume_chain_ptr())\n"
"#define g_dk_resume_val (*tur_tls_dk_resume_val_ptr())\n"
"#define g_dk_meta (*(DK ***)tur_tls_dk_meta_ptr())\n"
"#define g_dk_meta_n (*tur_tls_dk_meta_n_ptr())\n"
"#define g_dk_meta_cap (*tur_tls_dk_meta_cap_ptr())\n"
"#endif\n");
    buf_puts(out,
"/* The fiber's DK state follows it from thread to thread, so a CPS entry on a\n"
" * fiber reads it afresh after the body, which may have yielded and resumed\n"
" * elsewhere (TUR_TLS_FRESH). */\n"
"#if defined(TUR_TLS_FRESH) && !defined(__dk_reap_v)\n"
"TUR_TLS_FRESH(void **, __dk_reap_v, __dk_reap_v__at);\n"
"TUR_TLS_FRESH(unsigned char *, __dk_reap_kind, __dk_reap_kind__at);\n"
"TUR_TLS_FRESH(size_t, __dk_reap_n, __dk_reap_n__at);\n"
"TUR_TLS_FRESH(size_t, __dk_reap_cap, __dk_reap_cap__at);\n"
"TUR_TLS_FRESH(int, __dk_entry_depth, __dk_entry_depth__at);\n"
"TUR_TLS_FRESH(tur_jmp_buf *, g_dk_driver, g_dk_driver__at);\n"
"TUR_TLS_FRESH(DK *, g_dk_resume_chain, g_dk_resume_chain__at);\n"
"TUR_TLS_FRESH(intptr_t, g_dk_resume_val, g_dk_resume_val__at);\n"
"TUR_TLS_FRESH(DK **, g_dk_meta, g_dk_meta__at);\n"
"TUR_TLS_FRESH(size_t, g_dk_meta_n, g_dk_meta_n__at);\n"
"TUR_TLS_FRESH(size_t, g_dk_meta_cap, g_dk_meta_cap__at);\n"
"#define __dk_reap_v (*__dk_reap_v__at())\n"
"#define __dk_reap_kind (*__dk_reap_kind__at())\n"
"#define __dk_reap_n (*__dk_reap_n__at())\n"
"#define __dk_reap_cap (*__dk_reap_cap__at())\n"
"#define __dk_entry_depth (*__dk_entry_depth__at())\n"
"#define g_dk_driver (*g_dk_driver__at())\n"
"#define g_dk_resume_chain (*g_dk_resume_chain__at())\n"
"#define g_dk_resume_val (*g_dk_resume_val__at())\n"
"#define g_dk_meta (*g_dk_meta__at())\n"
"#define g_dk_meta_n (*g_dk_meta_n__at())\n"
"#define g_dk_meta_cap (*g_dk_meta_cap__at())\n"
"#endif\n");
    buf_puts(out,
"static void __dk_reap_push(void *p, unsigned char kind) {\n"
"    if (__dk_reap_n == __dk_reap_cap) {\n"
"        __dk_reap_cap = __dk_reap_cap ? __dk_reap_cap * 2 : 16;\n"
"        __dk_reap_v = (void **)realloc(__dk_reap_v, __dk_reap_cap * sizeof(void *));\n"
"        __dk_reap_kind = (unsigned char *)realloc(__dk_reap_kind, __dk_reap_cap);\n"
"    }\n"
"    __dk_reap_v[__dk_reap_n] = p; __dk_reap_kind[__dk_reap_n] = kind; __dk_reap_n++;\n"
"}\n"
"__attribute__((unused)) static DK *__dk_reap_keep(DK *k) { __dk_reap_push(k, 1); return k; }\n"
"__attribute__((unused)) static intptr_t __dk_reap_ptr(intptr_t p) { __dk_reap_push((void *)p, 0); return p; }\n"
"/* Register a single spliced node (->next points into an enclosing k) for a\n"
" * single-node free at reap -- dk_free would walk into the enclosing chain.\n"
" * Kind 3, freed like kind 0, but known to be a node: a park's hand-off reads\n"
" * its `orphaned` flag (__dk_reap_seg_take). */\n"
"__attribute__((unused)) static DK *__dk_reap_node(DK *k) { __dk_reap_push(k, 3); return k; }\n");
    /* closure-drop-glue: a boundary-reaped closure env is headered (env[-1] holds
     * its drop-glue), so a bare free of the past-header pointer would be an
     * interior free (corruption).  Reap kind 2 = "headered closure", released
     * through TUR_CLOSURE_DROP (recovers the header, walks owning captures, frees
     * the base). */
    buf_puts(out,
"__attribute__((unused)) static intptr_t __dk_reap_closure(intptr_t p) { __dk_reap_push((void *)p, 2); return p; }\n"
/* closure-let-in-self-tail-loop-leaks: a CPS self-tail loop's backedge frees
 * the closures that turn registered, instead of holding one per turn until the
 * outermost entry returns.  The entry comes OFF the list (order kept, so no
 * entry's mark moves) before the drop, so the boundary never frees it twice.
 * Pinned (a kept continuation may still reach it) or not found (the collector
 * already forgot it), it is left alone. */
"__attribute__((unused)) static void __dk_reap_closure_now(intptr_t p) {\n"
"    if (tur_dk_pinned) return;\n"
"    for (size_t i = __dk_reap_n; i-- > 0; ) {\n"
"        if (__dk_reap_v[i] == (void *)p && __dk_reap_kind[i] == 2) {\n"
"            memmove(&__dk_reap_v[i], &__dk_reap_v[i + 1], (__dk_reap_n - i - 1) * sizeof(void *));\n"
"            memmove(&__dk_reap_kind[i], &__dk_reap_kind[i + 1], __dk_reap_n - i - 1);\n"
"            __dk_reap_n--;\n"
"            TUR_CLOSURE_DROP(p);\n"
"            return;\n"
"        }\n"
"    }\n"
"}\n"
"static void __dk_reap_run(void) {\n"
"    for (size_t i = 0; i < __dk_reap_n && !tur_dk_pinned; i++) {\n"
"        if (__dk_reap_kind[i] == 1) dk_free((DK *)__dk_reap_v[i]);\n"
"        else if (__dk_reap_kind[i] == 2) TUR_CLOSURE_DROP(__dk_reap_v[i]);\n"
"        else free(__dk_reap_v[i]);\n"
"    }\n"
"    free(__dk_reap_v); free(__dk_reap_kind);\n"
"    __dk_reap_v = NULL; __dk_reap_kind = NULL; __dk_reap_n = __dk_reap_cap = 0;\n"
"}\n"
/* r7rs-callcc-memory-never-freed: a NESTED entry's exit.  Only the outermost
 * exit may free what the list holds -- a registered chain can still be in use
 * until the outermost dk_run settles -- so a program whose loop runs inside
 * one CPS entry (a `guard`, a call/cc escape, anything that reaches Turmeric's
 * call/cc) kept every inner entry's registrations until the program ended:
 * 2,000,000 entries for a `guard` in a loop run 1,000,000 times, and every
 * box and chain they name live with them.  Under the collector (`TUR_GC_ON`,
 * a compiled `#lang r7rs` program) nothing needs freeing by hand: forgetting
 * the entry's own registrations is enough, and the collector reclaims
 * whatever nothing else still reaches -- a stack image of a re-entrant
 * continuation included, which is scanned while it is live.  The slots are
 * cleared, since the list's array is itself scanned.  Without a collector
 * this does nothing, and the outermost exit frees everything, as before.
 * The list and the mark are the calling thread's (or fiber's), so this holds
 * with any number of threads. */
"__attribute__((unused)) static void __dk_reap_drop_to(size_t mark) {\n"
"#if defined(TUR_GC_ON) && TUR_GC_ON\n"
"    /* A re-entered continuation can bring back an entry whose mark is past\n"
"     * the list's end: nothing of its is left to drop. */\n"
"    if (mark >= __dk_reap_n) return;\n"
"    for (size_t i = mark; i < __dk_reap_n; i++) __dk_reap_v[i] = NULL;\n"
"    __dk_reap_n = mark;\n"
"#else\n"
"    (void)mark;\n"
"#endif\n"
"}\n");
    /* async-parked-body-chains-never-reaped: a parked async body's share of
     * the reap list.  When a body parks on a pending await, what its entry
     * registered is still needed -- the parked copy shares those frames' envs,
     * and a RESET_CONT frame's env can hold the entry's root -- but the entry
     * has returned, so nothing would ever reap it.  Its entry wrapper used to
     * leave the list and its own depth count as they were, which switched the
     * reaper off for the rest of the thread: the depth never got back to 0.
     *
     * Instead the owning entry MOVES the registrations past its mark into the
     * park record (__dk_reap_seg_take), their order kept, and leaves normally.
     * The resume holds the record's share aside while it runs the parked copy
     * -- off the list, so no last-entry release can take one of them while a
     * copy shares it -- then either puts it back on the list to be reaped
     * with what the run registered (__dk_reap_seg_give, once, when the body
     * settles) or passes it on whole to the next park (__dk_reap_seg_move),
     * which appends only that turn's registrations.  A body that parks on
     * every turn of a long loop so costs amortized O(1) per park, not a copy
     * of everything it holds.  The arrays are malloc'd, so under the collector
     * (TUR_GC_ON) the park record, which a future reaches, keeps them
     * scanned. */
    buf_puts(out,
"typedef struct { void **v; unsigned char *kind; size_t n, cap; } __dk_reap_seg;\n"
"__attribute__((unused)) static void __dk_reap_seg_reserve(__dk_reap_seg *s, size_t k) {\n"
"    if (s->n + k <= s->cap) return;\n"
"    size_t c = s->cap ? s->cap : 16;\n"
"    while (c < s->n + k) c *= 2;\n"
"    s->v = (void **)realloc(s->v, c * sizeof(void *));\n"
"    s->kind = (unsigned char *)realloc(s->kind, c);\n"
"    s->cap = c;\n"
"}\n"
"__attribute__((unused)) static void __dk_reap_seg_add(__dk_reap_seg *s, void *p, unsigned char kind) {\n"
"    __dk_reap_seg_reserve(s, 1);\n"
"    s->v[s->n] = p; s->kind[s->n] = kind; s->n++;\n"
"}\n"
"/* async-repeated-park-holds-frames-until-settle: the originals the parked\n"
" * await's shift left behind are freed here instead of moving: an `orphaned`\n"
" * node, and its env too when `orphan_env` says nothing else shared it then\n"
" * and `ncopy` says nothing copied it since (the entry's code may run on\n"
" * after a park -- a handler case carrying on with its placeholder).  The env\n"
" * is registered right before its node (emit_cont_env, then the frame), so it\n"
" * is the entry just taken; one registered elsewhere stays. */\n"
"__attribute__((unused)) static void __dk_reap_seg_take(__dk_reap_seg *s, size_t mark) {\n"
"    if (mark >= __dk_reap_n) return;\n"
"    size_t k = __dk_reap_n - mark;\n"
"    __dk_reap_seg_reserve(s, k);\n"
"    for (size_t i = mark; i < __dk_reap_n; i++) {\n"
"        void *p = __dk_reap_v[i]; unsigned char kd = __dk_reap_kind[i];\n"
"        __dk_reap_v[i] = NULL;\n"
"        if (kd == 3 && !tur_dk_pinned && ((DK *)p)->orphaned) {\n"
"            DK *d = (DK *)p;\n"
"            if (d->orphan_env && d->ncopy == 1 && s->n && s->v[s->n - 1] == (void *)d->env && s->kind[s->n - 1] == 0) {\n"
"                s->n--;\n"
"                free((void *)d->env);\n"
"            }\n"
"            free(d);\n"
"            continue;\n"
"        }\n"
"        s->v[s->n] = p; s->kind[s->n] = kd; s->n++;\n"
"    }\n"
"    __dk_reap_n = mark;\n"
"}\n"
"/* `from`'s entries go first in `to` (they are the older); `from` is emptied. */\n"
"__attribute__((unused)) static void __dk_reap_seg_move(__dk_reap_seg *to, __dk_reap_seg *from) {\n"
"    if (!to->n) { free(to->v); free(to->kind); *to = *from; }\n"
"    else {\n"
"        __dk_reap_seg_reserve(from, to->n);\n"
"        memcpy(from->v + from->n, to->v, to->n * sizeof(void *));\n"
"        memcpy(from->kind + from->n, to->kind, to->n);\n"
"        from->n += to->n;\n"
"        free(to->v); free(to->kind); *to = *from;\n"
"    }\n"
"    from->v = NULL; from->kind = NULL; from->n = from->cap = 0;\n"
"}\n"
"__attribute__((unused)) static void __dk_reap_seg_give(__dk_reap_seg *s) {\n"
"    for (size_t i = 0; i < s->n; i++) __dk_reap_push(s->v[i], s->kind[i]);\n"
"    free(s->v); free(s->kind);\n"
"    s->v = NULL; s->kind = NULL; s->n = s->cap = 0;\n"
"}\n");
    /* fn-value-call-cps-frames-held-until-outer-entry: a heap-join frame and
     * its env are registered back to back (__dk_reap_ptr(env), then
     * __dk_reap_node(frame)) and are dead once the frame has run: the join is
     * one non-tail call's continuation, delivered exactly once.  Holding them
     * until the outermost entry returns made a self-tail-recursive loop that
     * calls through a function value grow by both per iteration.
     *
     * The release is taken only when it is provably the last reference:
     *   - the node was never copied (`copied` unset), so no captured
     *     continuation, delivery or async park shares its env or will run it;
     *   - it is the LAST entry on the reap list, so nothing registered after it
     *     -- a handle chain, a perform's sub, another frame spliced onto it --
     *     is still outstanding (a released join above it came off the list
     *     with its own release);
     *   - nothing pinned DK memory (r7rs call/cc).
     * Otherwise the boundary reap frees it, as before.  The env goes second,
     * from inside the frame's function once it has read its captures: with the
     * node off the list, the env is the last entry exactly when this release
     * happened, and never when a copy is running (a copied node stays
     * registered above its env).  The slots are cleared because the collector
     * scans the list's array (TUR_GC_ON).
     *
     * An await's continuation (emit_await) releases its env the same way.  On
     * the fast path -- the future already fulfilled -- the frame function is
     * called in place right after the env is registered, so the env is the
     * last entry.  On the shift path every run is a copy's: one inside the
     * shift has the frame and shift nodes registered above the env, and a
     * parked one runs on resume, while the park holds the env off the list
     * (async-parked-body-chains-never-reaped). */
    buf_puts(out,
"__attribute__((unused)) static bool __dk_join_release_node(DK *k) {\n"
"    size_t n = __dk_reap_n;\n"
"    if (tur_dk_pinned || k->ncopy || !n || __dk_reap_v[n - 1] != (void *)k\n"
"        || __dk_reap_kind[n - 1] != 3) return false;\n"
"    __dk_reap_v[n - 1] = NULL; __dk_reap_n = n - 1;\n"
"    free(k); return true;\n"
"}\n"
"/* async-parked-body-chains-never-reaped: an await's shift node, and the\n"
" * frame it shifts over, once dk_run has returned from the shift: the shift\n"
" * arm ran (or parked) a COPY of the frame, so neither original is reachable.\n"
" * Taken back while each is the list's last entry, as a join is.  NULL frame:\n"
" * the shift's next is the caller's continuation, not the await's to free.\n"
" *\n"
" * async-repeated-park-holds-frames-until-settle: the same is true of every\n"
" * original between the shift and the prompt it reached -- only copies of\n"
" * them run from here on -- so each is marked `orphaned`, for the park's\n"
" * hand-off to free.  `parked` (the awaited future is still pending, which\n"
" * only the shift's own park leaves it) means the one copy taken, the shift\n"
" * arm's, was copied on into the park, which owns its envs; an original that\n"
" * no other copy was ever taken of (ncopy 1) then shares its env with\n"
" * nothing: `orphan_env`.  The frame's env goes back here when it is the\n"
" * list's last entry. */\n"
"__attribute__((unused)) static void __dk_await_release(DK *s, DK *f, int parked) {\n"
"    if (tur_dk_pinned) return;\n"
"    s->orphaned = true;\n"
"    for (DK *p = s->next; p && !(p->kind == DKK_PROMPT && p->tag == s->tag) && p->kind != DKK_DONE; p = p->next) {\n"
"        p->orphaned = true;\n"
"        if (parked && p->ncopy == 1 && p->env_size && !p->env_owned) p->orphan_env = true;\n"
"    }\n"
"    size_t n = __dk_reap_n;\n"
"    if (!n || __dk_reap_v[n - 1] != (void *)s || __dk_reap_kind[n - 1] != 3) return;\n"
"    __dk_reap_v[n - 1] = NULL; __dk_reap_n = --n;\n"
"    free(s);\n"
"    if (!f || !n || __dk_reap_v[n - 1] != (void *)f || __dk_reap_kind[n - 1] != 3) return;\n"
"    __dk_reap_v[n - 1] = NULL; __dk_reap_n = --n;\n"
"    intptr_t fe = f->env; bool fe_free = f->orphan_env;\n"
"    free(f);\n"
"    if (!fe_free || !n || __dk_reap_v[n - 1] != (void *)fe || __dk_reap_kind[n - 1] != 0) return;\n"
"    __dk_reap_v[n - 1] = NULL; __dk_reap_n = n - 1;\n"
"    free((void *)fe);\n"
"}\n"
"__attribute__((unused)) static void __dk_join_release_env(intptr_t env) {\n"
"    size_t n = __dk_reap_n;\n"
"    if (tur_dk_pinned || !env || !n || __dk_reap_v[n - 1] != (void *)env\n"
"        || __dk_reap_kind[n - 1] != 0) return;\n"
"    __dk_reap_v[n - 1] = NULL; __dk_reap_n = n - 1;\n"
"    free((void *)env);\n"
"}\n");
    buf_puts(out,
"static intptr_t dk_run_impl(DK *k, intptr_t v, bool root) {\n"
"    while (k) {\n"
"        switch (k->kind) {\n"
"            case DKK_DONE: return v;\n"
"            case DKK_PROMPT: case DKK_HANDLER: k = k->next; break;\n"
"            /* cps-body-panic-not-propagated: a frame whose body panicked under a\n"
"             * handler returned by signal; the rest of the chain is the rest of the\n"
"             * program past the panic and must not run.  Its nodes are reap-owned. */\n"
"            case DKK_FRAME: {\n"
"                DK *self = k;\n"
"                v = k->fn(k->env, v); if (tur_panicking) return 0; k = k->next;\n"
"                if (self->join_once) __dk_join_release_node(self);\n"
"                break;\n"
"            }\n"
"            case DKK_RESUME_FRAME:\n"
"                if (k->join_once) {\n"
"                    DKResumeFrame rf = k->rfn; intptr_t renv = k->env; DK *rest = k->next;\n"
"                    if (__dk_join_release_node(k)) return rf(renv, v, rest);\n"
"                }\n"
"                return k->rfn(k->env, v, k->next);\n"
"            case DKK_SHIFT:\n"
"            case DKK_SHIFT0: {\n"
"                DK *P = k->next;\n"
"                while (P && !(P->kind == DKK_PROMPT && P->tag == k->tag)\n"
"                         && P->kind != DKK_DONE) P = P->next;\n"
"                bool to_root = (!P || P->kind == DKK_DONE);\n"
"                bool reinstall = (k->kind == DKK_SHIFT);\n"
"                DK *sub = dk_copy_range(k->next, P);\n"
"                DK *tail = reinstall\n"
"                    ? dk_prompt(to_root ? DK_ROOT_TAG : k->tag, dk_done()) : dk_done();\n"
"                sub = dk_append(sub, tail);\n"
"                intptr_t bodyval = k->body(k->body_env, sub);\n"
"                dk_free(sub);\n"
"                if (to_root || tur_panicking) return bodyval;\n"
"                k = P->next; v = bodyval; break;\n"
"            }\n"
"        }\n"
"    }\n"
"    (void)root; return v;\n"
"}\n"
"static intptr_t dk_run(DK *k, intptr_t v)      { return dk_run_impl(k, v, false); }\n"
"static intptr_t dk_run_root(DK *k, intptr_t v) { return dk_run_impl(k, v, true); }\n");
    buf_puts(out,
"/* Forward decl of the bounded driver (defined with the E7 runtime below):\n"
" * dk_invoke consults g_dk_driver (defined with the reap registry above) to\n"
" * know whether running the invoked chain might tail-resume out. */\n"
"static intptr_t __dk_drive_bounded(DK *first, intptr_t firstv, size_t floor);\n"
"static intptr_t dk_invoke(DK *sub, intptr_t w) {\n"
"    DK *c = dk_copy_range(sub, NULL);\n"
"    /* A tail-resume inside the invoked chain longjmps to whichever landing\n"
"     * g_dk_driver names.  That landing must be THIS ONE, not the program entry:\n"
"     * dk_invoke is how a NON-tail `resume` runs, so its caller is a handler case\n"
"     * with more clause left to run (`(+ (resume k 1) (resume k 10))`).  Letting\n"
"     * the yield escape to the entry driver unwinds the case, delivering the\n"
"     * first resume's value as the whole handle's value and discarding the rest\n"
"     * -- a silent wrong answer, `2` where the answer is `22`.  See\n"
"     * docs/archive/cps-multishot-nontail-resume-inner-handle-drops-clause-rest.md.\n"
"     *\n"
"     * So install a landing scoped to this invoke and run the trampoline bounded\n"
"     * by the meta-stack watermark: deliveries queued during THIS run drain here,\n"
"     * anything an outer level queued stays for that level.  The E7 fast path is\n"
"     * untouched -- a tail resume reached without an intervening dk_invoke still\n"
"     * yields all the way to the entry driver, so deep effectful recursion stays\n"
"     * flat.\n"
"     *\n"
"     * `c` is reaped rather than freed on every path in the bounded loop: a\n"
"     * pending delivery may still reference it (the reason __dk_drive_after\n"
"     * stopped eagerly freeing a yielded chain -- see\n"
"     * docs/archive/effect-rec-nested-handler-nonterminates.md), and reaping only\n"
"     * ever defers a free to the outermost boundary, never double-frees.\n"
"     * With no driver a longjmp is impossible -- free eagerly as before. */\n"
"    if (g_dk_driver) return __dk_drive_bounded(c, w, g_dk_meta_n);\n"
"    intptr_t r = dk_run_impl(c, w, false);\n"
"    dk_free(c); return r;\n"
"}\n");
    buf_puts(out,
"/* ---- E7: trampolined tail-resume (cps-tramp-resume) -------------------- *\n"
" * A tail-resume handler does not resume inline (which nests ~160 B of C stack\n"
" * per resumed perform -> O(N)); instead dk_perform yields the resumed chain to\n"
" * the entry driver, which re-enters it from the top. Pending handle-continuation\n"
" * deliveries (what dk_run_impl(H->next,r) would run) ride a heap meta-stack in\n"
" * nesting (LIFO) order; a delivery of only HANDLER/DONE nodes is a no-op and is\n"
" * elided, so the meta-stack stays O(nesting), not O(N). Validated end-to-end at\n"
" * N=1e6 by docs/artifacts/probes/e7-fidelity-probe.c.  The driver landing,\n"
" * the resume chain and value, and the meta-stack are per-thread, with the\n"
" * reap registry above. */\n"
"static void __dk_meta_push(DK *d) {\n"
"    if (g_dk_meta_n == g_dk_meta_cap) {\n"
"        g_dk_meta_cap = g_dk_meta_cap ? g_dk_meta_cap * 2 : 16;\n"
"        g_dk_meta = (DK **)realloc(g_dk_meta, g_dk_meta_cap * sizeof(DK *));\n"
"    }\n"
"    g_dk_meta[g_dk_meta_n++] = d;\n"
"}\n"
"static bool __dk_delivery_noop(const DK *d) {   /* only HANDLER/DONE -> identity */\n"
"    for (const DK *p = d; p; p = p->next)\n"
"        if (p->kind != DKK_HANDLER && p->kind != DKK_DONE) return false;\n"
"    return true;\n"
"}\n"
"/* tail-resume: yield the resumed chain to the driver (never returns).  With no\n"
" * active driver (dk_perform did NOT take its tail-resume yield branch, so no\n"
" * delivery was queued), fall back to the inline dk_invoke resume -- byte-identical\n"
" * to the non-trampolined path, keeping the two sides consistent. */\n"
"static intptr_t dk_tail_resume(DK *sub, intptr_t v) {\n"
"    if (!g_dk_driver) return dk_invoke(sub, v);\n"
"    g_dk_resume_chain = sub; g_dk_resume_val = v;\n"
"    TUR_LONGJMP(*g_dk_driver);\n"
"    return 0; /* unreachable */\n"
"}\n"
"");
    buf_puts(out,
"/* Run `first` to completion, absorbing any tail-resume yields it makes, and\n"
" * return its value.  Same trampoline as __dk_drive_after but SCOPED: it drains\n"
" * the meta-stack only down to `floor` (the depth at entry), and restores the\n"
" * previous landing on the way out, so a nested run cannot consume an outer\n"
" * level's pending deliveries or steal its yields.  dk_invoke uses it to keep a\n"
" * non-tail resume's tail-resume from unwinding the handler case that called it.\n"
" *\n"
" * Locals are re-read from the resume globals at the top of each iteration (and\n"
" * setjmp is re-armed there) rather than carried across the longjmp, which is\n"
" * what makes them well-defined on the yield path -- the same structure\n"
" * __dk_drive_after uses. */\n"
"static intptr_t __dk_drive_bounded(DK *first, intptr_t firstv, size_t floor) {\n"
"    tur_jmp_buf jb; tur_jmp_buf *saved = g_dk_driver;\n"
"    g_dk_driver = &jb;\n"
"    g_dk_resume_chain = first; g_dk_resume_val = firstv;\n"
"    intptr_t r;\n"
"    for (;;) {\n"
"        DK *ch = g_dk_resume_chain; intptr_t rv = g_dk_resume_val;\n"
"        if (TUR_SETJMP(jb) == 0) {\n"
"            r = dk_run_impl(ch, rv, false);\n"
"            __dk_reap_keep(ch);\n"
"            /* cps-body-panic-not-propagated: a panic signalled out of the chain\n"
"             * abandons this level's pending deliveries (reap-owned, freed at the\n"
"             * entry boundary) and returns so the wrapper's caller sees the flag. */\n"
"            if (tur_panicking) { while (g_dk_meta_n > floor) __dk_reap_keep(g_dk_meta[--g_dk_meta_n]); break; }\n"
"            if (g_dk_meta_n <= floor) break;\n"
"            g_dk_resume_chain = g_dk_meta[--g_dk_meta_n];\n"
"            g_dk_resume_val = r;\n"
"        } else {\n"
"            __dk_reap_keep(ch);   /* a pending delivery may still reference it */\n"
"        }\n"
"    }\n"
"    g_dk_driver = saved;\n"
"    return r;\n"
"}\n"
"/* Run the meta-stack trampoline to completion after a tail-resume longjmp landed\n"
" * in the entry wrapper. Owns its own jmp_buf so further yields land here. */\n"
"static intptr_t __dk_drive_after(void) {\n"
"    tur_jmp_buf jb; g_dk_driver = &jb;\n"
"    intptr_t r;\n"
"    for (;;) {\n"
"        DK *ch = g_dk_resume_chain; intptr_t rv = g_dk_resume_val;\n"
"        if (TUR_SETJMP(jb) == 0) {\n"
"            r = dk_run_impl(ch, rv, false);\n"
"            dk_free(ch);\n"
"            if (tur_panicking) { while (g_dk_meta_n > 0) dk_free(g_dk_meta[--g_dk_meta_n]); return r; }\n"
"            if (g_dk_meta_n == 0) return r;\n"
"            g_dk_resume_chain = g_dk_meta[--g_dk_meta_n];\n"
"            g_dk_resume_val = r;\n"
"        } else {\n"
"            /* Yielded mid-run: `ch` tail-resumed again from deep inside its own\n"
"             * execution.  With nested handlers the pending meta-stack delivery\n"
"             * queued by that interior perform re-enters the machine and reifies\n"
"             * continuations that still point into `ch`, so eagerly freeing it\n"
"             * here is a use-after-free (an inner `perform` under an outer\n"
"             * handler resumed across it -> dk_run_impl walks freed nodes and\n"
"             * spins forever).  Hand `ch` a boundary owner instead -- the same\n"
"             * treatment dk_invoke gives a chain that may tail-resume out -- so it\n"
"             * is freed exactly once at the outermost entry (__dk_reap_run) after\n"
"             * every delivery that references it has drained.  A single-handler\n"
"             * deep loop is unaffected in correctness; it only defers these frees\n"
"             * to the entry boundary.  See\n"
"             * docs/archive/effect-rec-nested-handler-nonterminates.md. */\n"
"            __dk_reap_keep(ch);   /* was dk_free(ch): premature under nesting */\n"
"        }\n"
"    }\n"
"}\n");
    buf_puts(out,
"/* Effect re-opening: the handler node whose case is currently running, set just\n"
" * before dk_perform calls the case.  A re-opening case reads it (into a local, at\n"
" * entry, before any interior perform can overwrite it) to recover its real\n"
" * enclosing chain via dk_case_enclosing_real. */\n"
"static const DK *g_dk_case_reopen_hnode = NULL;\n"
"static intptr_t dk_perform(int tag, intptr_t arg, DK *k) {\n"
"    DK *H = k;\n"
"    while (H && !(H->kind == DKK_HANDLER && H->tag == tag) && H->kind != DKK_DONE) H = H->next;\n"
"    if (!H || H->kind == DKK_DONE) { fprintf(stderr, \"tur: unhandled effect (tag %d)\\n\", tag); abort(); }\n"
"    DK *sub = dk_copy_range(k, H);\n"
"    DK *tail;\n"
"    if (H->shallow) {\n"
"        tail = dk_copy_enclosing_handlers(H->next);\n"
"    } else {\n"
"        /* Deep: re-install H AND its consecutive SIBLING handlers -- the rest of\n"
"         * this handle's dk_handler group (a multi-effect handle emits one\n"
"         * dk_handler node per case, chained: HANDLER(A)->HANDLER(B)->cont-frame).\n"
"         * dk_copy_range(k,H) already copied the siblings BEFORE H; the siblings\n"
"         * AFTER H live in H->next and would otherwise be lost, so a re-perform of\n"
"         * a sibling effect (e.g. perform B in the continuation resumed by the A\n"
"         * case) escaped -> `unhandled effect`.  Re-install the maximal run of\n"
"         * DKK_HANDLER nodes starting at H (it ends at the first non-handler node,\n"
"         * the handle's continuation frame), so the full group is present in the\n"
"         * resumed sub-continuation.  A single-case handle copies just H (identical\n"
"         * to the old dk_handler(tag,...) re-install). */\n"
"        const DK *ge = H;\n");
    /* Same sibling-group boundary fix as dk_case_enclosing_real: skip only H's
     * own hgroup, so a prior re-install that flattened an enclosing handle
     * adjacent to H does not fold it into H's re-installed group. */
    buf_puts(out,
"        while (ge && ge->kind == DKK_HANDLER && ge->hgroup == H->hgroup) ge = ge->next;\n");
    buf_puts(out,
"        /* Terminate the re-installed group with the ENCLOSING handler markers, not\n"
"         * dk_done(): a deep handler leaves the outer handlers in place, so an\n"
"         * effect the group does NOT handle, performed in the resumed continuation,\n"
"         * propagates outward (e.g. inner handles Write, its body also performs Log\n"
"         * which must reach the enclosing Log handler).  dk_done() cut that off ->\n"
"         * `unhandled effect`.  dk_copy_enclosing_handlers(ge) copies the outer\n"
"         * HANDLER markers past this handle's continuation frame; with no enclosing\n"
"         * handler it is [done], i.e. unchanged from before. */\n"
"        tail = dk_append(dk_copy_range(H, ge), dk_copy_enclosing_handlers(ge));\n"
"    }\n"
"    sub = dk_append(sub, tail);\n");
    buf_puts(out,
"    /* E7: a tail-resume handler under an active driver yields the resumed chain\n"
"     * rather than resuming inline; queue its H->next delivery (unless a no-op) so\n"
"     * it runs after the resumed chain settles, in nesting order. */\n"
"    if (H->tail_resume && g_dk_driver) {\n"
"        DK *__deliv = dk_copy_range(H->next, NULL);\n"
"        if (__dk_delivery_noop(__deliv)) dk_free(__deliv); else __dk_meta_push(__deliv);\n"
"        g_dk_case_reopen_hnode = H;  /* re-opening: case reads its enclosing markers */\n"
"        return H->handler(H->handler_env, arg, sub);  /* ends in dk_tail_resume -> longjmp */\n"
"    }\n");
    buf_puts(out,
"    g_dk_case_reopen_hnode = H;  /* re-opening: case reads its enclosing markers */\n");
    buf_puts(out,
"    /* A non-tail deep case that RE-OPENS an outer effect ends its body in that\n"
"     * interior perform; if the outer effect is tail-resumed, dk_tail_resume\n"
"     * longjmps to the entry driver and this dk_perform frame is unwound -- so the\n"
"     * `dk_free(sub)` below never runs and `sub` leaks once per re-opened perform\n"
"     * (O(N), docs/archive/cps-reopen-perform-onode-leak.md).  Give `sub` an owner\n"
"     * that survives the longjmp: register it for a boundary dk_free at the\n"
"     * outermost entry (__dk_reap_run), mirroring how every other per-perform node\n"
"     * on this path is owned.  This is safe against double-free -- only a TAIL case\n"
"     * hands its `sub` to the driver (the E7 branch above, which the driver frees),\n"
"     * and a case body only ever reaps COPIES of `subk`, never `subk`/`sub` itself\n"
"     * -- so reaping is `sub`'s sole disposal on both the return and longjmp paths.\n"
"     * We reap BEFORE the handler call so a longjmp cannot skip the registration. */\n"
"    __dk_reap_keep(sub);\n"
"    intptr_t r = H->handler(H->handler_env, arg, sub);\n"
/* case_delivers: a re-opening case already delivered its value through the
 * real chain (its __kont = dk_case_enclosing_real covers H->next); `r` is the
 * value that bubbled back from that delivery, so deliver it AGAIN and the rest
 * of the program runs twice (the measured spurious `1000` in
 * cps-case-reopen-marker-kont-truncates-capture).  Return it verbatim. */
"    return H->case_delivers ? r : dk_run_impl(H->next, r, false);\n"
"}\n");
    /* NOTE: the retired `tramp == false` arm also emitted an `__dk_abort_body`
     * helper here.  It went with the arm rather than being hoisted: nothing in
     * the emitter references it, and it appears in zero expected.c snapshots --
     * i.e. it has not been emitted since cps-tramp-resume graduated.  The
     * arm's trailing blank line went with it for the same reason. */
}
