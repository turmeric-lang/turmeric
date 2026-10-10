/* ffi_thunk.h -- RP4 of docs/spice-repl-plan.md.
 *
 * Bridge between the REPL's eval layer (TuriValue / TuriEnv) and the
 * shape-keyed dispatcher in src/runtime/ffi_dispatch_thunk.c. For each
 * export in a loaded TurSpiceImage, tur_ffi_install_spice_bindings()
 * registers a TuriNativeFn shim that:
 *
 *   1. Validates arg count against the recorded signature.
 *   2. Marshals each TuriValue arg to int64_t (':int' class) or
 *      double (':float' class) per the export's arg_classes.
 *   3. Calls tur_ffi_thunk_call() to pick the right typed dispatcher
 *      from src/runtime/ffi_dispatch.h.
 *   4. Wraps the result back into a TuriValue ('i' -> turi_int,
 *      'f' -> turi_float, 'v' -> turi_nil).
 *
 * Each export is bound under TWO names so the user can call it either
 * way at the REPL prompt:
 *   - bare       (e.g. `add42`)
 *   - qualified  (e.g. `smokelib/add42`)
 *
 * The bare form makes the smoke test `(add42 100)` work without an
 * explicit `(import ...)` (which the REPL still rejects at the top
 * level today). The qualified form avoids collisions when two spices
 * happen to export the same defn name.
 */
#ifndef TUR_TURI_FFI_THUNK_H
#define TUR_TURI_FFI_THUNK_H

#include "turi/env.h"
#include "turi/spice_loader.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Install one TuriNativeFn binding per export in `img`. Returns the
 * number of bindings installed (= image's export count). Safe to call
 * with NULL img -- returns 0. */
uint32_t tur_ffi_install_spice_bindings(TuriEnv *env, TurSpiceImage *img);

/* The same, for an image an embedder attaches (turi_env_attach_spice): the
 * per-export user data and qualified names are freed with the env instead of
 * leaked at process exit, and an export the shim cannot marshal (a struct
 * return or argument) is left unbound, so its interpreted defn still runs.
 * Returns the number bound. */
uint32_t tur_ffi_install_spice_bindings_owned(TuriEnv *env, TurSpiceImage *img);

/* The defmodule a native was bound from, when it is a spice-image export
 * installed by either function above; NULL for any other native.  The
 * interpreter asks so that evaluating the spice's own source (an `import` of
 * the module) does not replace the compiled export with its interpreted defn. */
const char *tur_ffi_native_spice_module(TuriNativeFn fn, const void *ud);

/* A spice-image native whose export takes or returns a by-value record
 * (manifest :record) needs the record's layout, which only the module's own
 * declared types give.  wants_records is true until it has one; note_records
 * hands it the layout sig of the whole signature (agg_sig_build's shape), each
 * parameter's offset into it, and the record def per slot ([0] the result,
 * [1 + k] parameter k, NULL for a scalar), taking ownership of the three
 * malloc'd arrays when it returns true. */
struct AdtDef;
bool tur_ffi_native_wants_records(TuriNativeFn fn, const void *ud);
bool tur_ffi_native_note_records(TuriNativeFn fn, void *ud, char *sig,
                                 size_t *arg_at, const struct AdtDef **defs,
                                 uint32_t n_params);

/* jit-ffi-c2mir-plan F2: bind `name` to a thunk-backed native that calls
 * the resolved C function `fn` with the signature described by `ret_class`
 * + `arg_classes[n]` (the 'i'/'f'/'F'/'v' vocabulary of turi/jit_ffi.h).
 * The call goes through the JIT provider's per-signature thunk at each
 * invocation, so registration is cheap and a provider that appears later
 * (or a compile failure) surfaces per-call, as a clean error value.  The
 * classes buffer is copied; `name` must outlive the env (interned symbol
 * name).  Calls are gated on TURI_CAP_FFI.  Returns 0 on success, -1 on
 * OOM. */
int tur_ffi_register_extern_thunk(TuriEnv *env, const char *name, void *fn,
                                  char ret_class, const char *arg_classes,
                                  uint32_t n);

/* ------------------------------------------------------------------ */
/* jit-ffi-c2mir-plan F5: callbacks (C calling back into Turmeric)     */
/* ------------------------------------------------------------------ */

/* The context a generated callback carries.  Its ADDRESS is baked into the
 * generated C as a literal, so it must outlive every C library that holds
 * the function pointer -- i.e. the process.  Allocated by
 * tur_ffi_cb_ctx_new and never freed, matching turi's closure policy. */
typedef struct TurFfiCbCtx {
    TuriEnv  *env;
    TuriValue fn;
    char      ret_class;              /* 'i' / 'f' / 'F' / 'v' / '{' */
    char     *arg_classes;            /* n entries, owned ('{' = aggregate) */
    uint32_t  n_args;
    /* Aggregate slots (F4 follow-on).  NULL/unset for scalar-only
     * signatures.  `sig` is the full signature (inline `{...}` layouts);
     * arg_at[k] is the sig offset of parameter k's slot; arg_defs[k] is the
     * record def of an aggregate parameter (NULL for scalars); ret_def is
     * the aggregate return's def.  All owned; set once via
     * tur_ffi_cb_ctx_set_agg. */
    char                 *sig;
    size_t               *arg_at;
    const struct AdtDef **arg_defs;
    const struct AdtDef  *ret_def;
} TurFfiCbCtx;

/* Build a process-lifetime callback context.  Returns NULL on OOM. */
TurFfiCbCtx *tur_ffi_cb_ctx_new(TuriEnv *env, TuriValue fn, char ret_class,
                                const char *arg_classes, uint32_t n);

/* Attach aggregate-slot information to a context whose classes include
 * '{'.  Takes ownership of `sig` and `arg_at` (malloc'd by the caller);
 * copies `arg_defs`.  Returns 0 on success, nonzero on OOM (ownership is
 * NOT taken on failure). */
int tur_ffi_cb_ctx_set_agg(TurFfiCbCtx *ctx, char *sig, size_t *arg_at,
                           const struct AdtDef **arg_defs,
                           const struct AdtDef *ret_def);

/* The fixed entry point every generated callback calls.  Deliberately NOT
 * static and spelled with a stable name: the generated C declares it
 * `extern` and resolves it against this process (the `tur` executable links
 * with ENABLE_EXPORTS), so renaming it breaks every compiled callback.
 *
 * Unpacks the position-indexed `iv`/`fv` buffers into TuriValues per the
 * context's classes -- an aggregate parameter's bytes arrive through the
 * `sv` pointer channel and are rebuilt into a record value -- calls the
 * Turmeric function, and writes the result back through *out_i / *out_f by
 * the return class (an aggregate return is packed through `out_s`, which
 * the generated callback points at its own return slot).  A Turmeric-side
 * error is reported on stderr and yields a zero result -- there is no error
 * channel back through a C callback slot, and unwinding through foreign
 * frames is not something we can do safely. */
void tur_ffi_cb_dispatch(void *ctx, const long long *iv, const double *fv,
                         const void *const *sv, long long *out_i,
                         double *out_f, void *out_s);

/* eval.c bridges for the aggregate directions of tur_ffi_cb_dispatch: the
 * pack/unpack marshaller lives with eval_call_ptr (it needs the record
 * machinery), the dispatch lives here. */
TuriValue tur_eval_agg_from_bytes(TuriEnv *env, const struct AdtDef *def,
                                  const char *sig, const void *bytes);
bool tur_eval_agg_to_bytes(const struct AdtDef *def, const char *sig,
                           TuriValue v, void *bytes);

/* RP5: register the `reload` native so the user can type `(reload)`
 * at the prompt. Available even when no spice is currently loaded
 * (the call surfaces a clean "no spice loaded" error in that case).
 * Called once during REPL start, before the first eval. */
void tur_ffi_register_reload_native(TuriEnv *env);

/* RP5: re-discover + rebuild + reinstall bindings for the spice in
 * env->spice_image. The current image is pushed onto
 * env->retired_spice_images so any in-flight bindings keep their
 * borrowed name strings valid. Returns:
 *   :nil    -- success (a TuriValue tagged TURI_NIL)
 *   :error  -- failure (env->spice_image left unchanged; a status
 *              message has been printed)
 * The function prints one line summarising the outcome ("no changes",
 * "rebuilt N exports", or an error explanation). */
TuriValue tur_ffi_reload_spice(TuriEnv *env);

#ifdef __cplusplus
}
#endif

#endif /* TUR_TURI_FFI_THUNK_H */
