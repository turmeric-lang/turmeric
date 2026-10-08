/* string_native.c -- interpreter natives for the owned String type.
 * See string_native.h; every native forwards to src/runtime/tur_string.c. */
#include "string_native.h"

#include <string.h>

#include "../runtime/tur_string.h"
#include "value.h"

/* A String / StringBuilder handle rides the int64 carrier.  A cstr argument may
 * arrive as TURI_CSTR (a literal) or as an int carrier (a computed pointer). */
static void *arg_ptr(TuriValue v) {
    return v.tag == TURI_CSTR ? (void *)(intptr_t)v.as_cstr
                              : (void *)(intptr_t)v.as_int;
}
static const char *arg_cstr(TuriValue v) {
    return v.tag == TURI_CSTR ? v.as_cstr : (const char *)(intptr_t)v.as_int;
}

#define S1 (n >= 1 ? arg_ptr(a[0]) : NULL)
#define S2 (n >= 2 ? arg_ptr(a[1]) : NULL)

static TuriValue n_from_cstr(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    return turi_int((int64_t)(intptr_t)tur_string_from_cstr(n >= 1 ? arg_cstr(a[0]) : ""));
}
static TuriValue n_from_bytes(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    const char *src = n >= 1 ? arg_cstr(a[0]) : "";
    int64_t len = n >= 2 ? a[1].as_int : 0;
    /* A cstr VALUE carries no length but its NUL, so a `len` past it read
     * the interpreter's own heap beyond the string -- an out-of-bounds read
     * any program, sandboxed or not, could aim (security audit WP5, M-5).
     * Cap it there.  A raw address (a TURI_INT) is a byte buffer that may hold
     * NULs, as on the compiled path, and keeps the caller's length. */
    if (n >= 1 && a[0].tag == TURI_CSTR && src && len > 0)
        len = (int64_t)strnlen(src, (size_t)len);
    /* S-5: in a provenance-tracked env the guard admitted the word only as a
     * C string (CSTR row) -- never as a sized buffer -- so it is capped at its
     * NUL too.  A NULL source copied `len` bytes from address 0. */
    if (!src) len = 0;
    else if (e->provenance_on && len > 0) len = (int64_t)strnlen(src, (size_t)len);
    return turi_int((int64_t)(intptr_t)tur_string_from_bytes(src, len));
}
static TuriValue n_cstr(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    /* S-5: the bytes belong to the String, which tur_string_release frees,
     * while a tagged cstr is trusted wherever it goes.  A provenance-tracked
     * env gets an env-pool copy, so the string it holds cannot dangle. */
    if (e->provenance_on) return turi_cstr(turi_val_strdup(e, tur_string_cstr(S1)));
    return turi_cstr(tur_string_cstr(S1));
}
static TuriValue n_adopt_cstr(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    /* Copy the bytes into an owned String but do NOT free the source cstr.
     *
     * The compiled path's tur_string_adopt_cstr() does from_bytes()+free(s):
     * the callee handed back a genuinely malloc'd buffer (strdup / a fresh
     * malloc in the colorizer / range / re inline-C), so adopting frees it.
     * Under --interpret those same inline-C bodies are reproduced by the
     * tree-walker via the env value-arena (turi_val_alloc), NOT raw malloc
     * (see native_extern_free's rationale in eval.c and the ic_exec_* string
     * builders).  Handing such an arena pointer to libc free() is a hard
     * bad-free / double-free abort -- the exact crash the term-string and
     * range-string fixtures hit.  Since the interpreter runs process-lifetime
     * and never frees its arena until env teardown, skip the free: from_bytes
     * already made an independent owned copy, and leaking the (arena) source
     * is consistent with the rest of the interpreter's allocation model. */
    const char *s = n >= 1 ? arg_cstr(a[0]) : "";
    return turi_int((int64_t)(intptr_t)tur_string_from_cstr(s));
}
static TuriValue n_from_int(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    return turi_int((int64_t)(intptr_t)tur_string_from_int(n >= 1 ? a[0].as_int : 0));
}
static TuriValue n_retain(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    return turi_int((int64_t)(intptr_t)tur_string_retain(S1));
}
static TuriValue n_release(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    tur_string_release(S1);
    return turi_nil();
}
static TuriValue n_len(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    return turi_int(tur_string_len(S1));
}
static TuriValue n_empty(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    return turi_bool(tur_string_empty(S1) != 0);
}
static TuriValue n_byte_at(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    return turi_int(tur_string_byte_at(S1, n >= 2 ? a[1].as_int : 0));
}
static TuriValue n_eq(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    return turi_bool(tur_string_eq(S1, S2) != 0);
}
static TuriValue n_cmp(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    return turi_int(tur_string_cmp(S1, S2));
}
static TuriValue n_hash(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    return turi_int(tur_string_hash(S1));
}
static TuriValue n_starts_with(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    return turi_bool(tur_string_starts_with(S1, S2) != 0);
}
static TuriValue n_ends_with(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    return turi_bool(tur_string_ends_with(S1, S2) != 0);
}
static TuriValue n_contains(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    return turi_bool(tur_string_contains(S1, S2) != 0);
}
static TuriValue n_concat(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    return turi_int((int64_t)(intptr_t)tur_string_concat(S1, S2));
}
static TuriValue n_substring(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    int64_t start = n >= 2 ? a[1].as_int : 0;
    int64_t len = n >= 3 ? a[2].as_int : 0;
    return turi_int((int64_t)(intptr_t)tur_string_substring(S1, start, len));
}
static TuriValue n_to_upper(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    return turi_int((int64_t)(intptr_t)tur_string_to_upper(S1));
}
static TuriValue n_to_lower(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    return turi_int((int64_t)(intptr_t)tur_string_to_lower(S1));
}
static TuriValue n_trim(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    return turi_int((int64_t)(intptr_t)tur_string_trim(S1));
}
static TuriValue n_box_key(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    return turi_int((int64_t)(intptr_t)tur_string_box_key(S1));
}
static TuriValue n_key_eq_addr(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)a; (void)n; (void)ud;
    return turi_int(tur_string_key_eq_addr());
}
static TuriValue n_sb_new(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)a; (void)n; (void)ud;
    return turi_int((int64_t)(intptr_t)tur_sb_new());
}
static TuriValue n_sb_push_cstr(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    tur_sb_push_cstr(S1, n >= 2 ? arg_cstr(a[1]) : "");
    return turi_nil();
}
static TuriValue n_sb_push_string(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    tur_sb_push_string(S1, S2);
    return turi_nil();
}
static TuriValue n_sb_push_byte(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    tur_sb_push_byte(S1, n >= 2 ? a[1].as_int : 0);
    return turi_nil();
}
static TuriValue n_sb_len(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    return turi_int(tur_sb_len(S1));
}
static TuriValue n_sb_finish(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    return turi_int((int64_t)(intptr_t)tur_sb_finish(S1));
}
static TuriValue n_slice(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    int64_t off = n >= 2 ? a[1].as_int : 0;
    int64_t len = n >= 3 ? a[2].as_int : 0;
    return turi_int((int64_t)(intptr_t)tur_string_slice(S1, off, len));
}
static TuriValue n_slice_cstr(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    const char *s = n >= 1 ? arg_cstr(a[0]) : "";
    int64_t off = n >= 2 ? a[1].as_int : 0;
    int64_t len = n >= 3 ? a[2].as_int : 0;
    return turi_int((int64_t)(intptr_t)tur_string_slice_cstr(s, off, len));
}
static TuriValue n_slice_retain(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    return turi_int((int64_t)(intptr_t)tur_slice_retain(S1));
}
static TuriValue n_slice_release(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    tur_slice_release(S1);
    return turi_nil();
}
static TuriValue n_slice_len(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    return turi_int(tur_slice_len(S1));
}
static TuriValue n_slice_empty(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    return turi_bool(tur_slice_empty(S1) != 0);
}
static TuriValue n_slice_byte_at(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    return turi_int(tur_slice_byte_at(S1, n >= 2 ? a[1].as_int : 0));
}
static TuriValue n_slice_sub(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    int64_t off = n >= 2 ? a[1].as_int : 0;
    int64_t len = n >= 3 ? a[2].as_int : 0;
    return turi_int((int64_t)(intptr_t)tur_slice_sub(S1, off, len));
}
static TuriValue n_slice_to_string(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    return turi_int((int64_t)(intptr_t)tur_slice_to_string(S1));
}
static TuriValue n_slice_to_cstr(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    return turi_cstr(tur_slice_to_cstr(S1));
}
static TuriValue n_slice_eq(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    return turi_bool(tur_slice_eq(S1, S2) != 0);
}
static TuriValue n_slice_cmp(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    return turi_int(tur_slice_cmp(S1, S2));
}
static TuriValue n_slice_hash(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    return turi_int(tur_slice_hash(S1));
}

void turi_register_string_natives(TuriEnv *env) {
    turi_env_register_native(env, "tur_string_from_cstr", n_from_cstr, NULL);
    turi_env_register_native(env, "tur_string_from_bytes", n_from_bytes, NULL);
    turi_env_register_native(env, "tur_string_cstr", n_cstr, NULL);
    turi_env_register_native(env, "tur_string_adopt_cstr", n_adopt_cstr, NULL);
    turi_env_register_native(env, "tur_string_from_int", n_from_int, NULL);
    turi_env_register_native(env, "tur_string_retain", n_retain, NULL);
    turi_env_register_native(env, "tur_string_release", n_release, NULL);
    turi_env_register_native(env, "tur_string_len", n_len, NULL);
    turi_env_register_native(env, "tur_string_empty", n_empty, NULL);
    turi_env_register_native(env, "tur_string_byte_at", n_byte_at, NULL);
    turi_env_register_native(env, "tur_string_eq", n_eq, NULL);
    turi_env_register_native(env, "tur_string_cmp", n_cmp, NULL);
    turi_env_register_native(env, "tur_string_hash", n_hash, NULL);
    turi_env_register_native(env, "tur_string_starts_with", n_starts_with, NULL);
    turi_env_register_native(env, "tur_string_ends_with", n_ends_with, NULL);
    turi_env_register_native(env, "tur_string_contains", n_contains, NULL);
    turi_env_register_native(env, "tur_string_concat", n_concat, NULL);
    turi_env_register_native(env, "tur_string_substring", n_substring, NULL);
    turi_env_register_native(env, "tur_string_to_upper", n_to_upper, NULL);
    turi_env_register_native(env, "tur_string_to_lower", n_to_lower, NULL);
    turi_env_register_native(env, "tur_string_trim", n_trim, NULL);
    turi_env_register_native(env, "tur_string_box_key", n_box_key, NULL);
    turi_env_register_native(env, "tur_string_key_eq_addr", n_key_eq_addr, NULL);
    turi_env_register_native(env, "tur_sb_new", n_sb_new, NULL);
    turi_env_register_native(env, "tur_sb_push_cstr", n_sb_push_cstr, NULL);
    turi_env_register_native(env, "tur_sb_push_string", n_sb_push_string, NULL);
    turi_env_register_native(env, "tur_sb_push_byte", n_sb_push_byte, NULL);
    turi_env_register_native(env, "tur_sb_len", n_sb_len, NULL);
    turi_env_register_native(env, "tur_sb_finish", n_sb_finish, NULL);
    turi_env_register_native(env, "tur_string_slice", n_slice, NULL);
    turi_env_register_native(env, "tur_string_slice_cstr", n_slice_cstr, NULL);
    turi_env_register_native(env, "tur_slice_retain", n_slice_retain, NULL);
    turi_env_register_native(env, "tur_slice_release", n_slice_release, NULL);
    turi_env_register_native(env, "tur_slice_len", n_slice_len, NULL);
    turi_env_register_native(env, "tur_slice_empty", n_slice_empty, NULL);
    turi_env_register_native(env, "tur_slice_byte_at", n_slice_byte_at, NULL);
    turi_env_register_native(env, "tur_slice_sub", n_slice_sub, NULL);
    turi_env_register_native(env, "tur_slice_to_string", n_slice_to_string, NULL);
    turi_env_register_native(env, "tur_slice_to_cstr", n_slice_to_cstr, NULL);
    turi_env_register_native(env, "tur_slice_eq", n_slice_eq, NULL);
    turi_env_register_native(env, "tur_slice_cmp", n_slice_cmp, NULL);
    turi_env_register_native(env, "tur_slice_hash", n_slice_hash, NULL);
}
