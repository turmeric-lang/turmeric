/* inline_c_jit.h -- C1 of docs/upcoming/aot-compiled-repl-plan.md.
 *
 * The tree-walking interpreter runs an inline-C `defn` only when its body
 * matches a pattern try_exec_simple_inline_c recognises; every other body is
 * "inline-C not supported in interpreter mode".  With the `repl-jit-inline-c`
 * experiment on, and a host that can compile C in process (a TUR_JIT build of
 * `tur`), such a defn is instead compiled on its FIRST call: its own source
 * form goes through the real emitter and the MIR engine, and the resulting
 * function is called through its exact-signature `__ffi` shim.
 *
 * The interpreter stays the evaluator.  Only the bodies it refuses compile, so
 * the stdlib's inline-C (answered by native overrides) never does, and a defn
 * that is never called costs nothing.
 *
 * This half is MIR-free and lives in tur_core: it finds the source form,
 * marshals arguments, and caches the compiled function per FnDef.  The
 * compile itself is the host's (`tur` registers it from main.c under
 * TUR_HAVE_JIT), so a build with no engine -- the WASM playground, a
 * -DTUR_JIT=OFF tree -- has no hook and keeps today's error. */
#ifndef TUR_TURI_INLINE_C_JIT_H
#define TUR_TURI_INLINE_C_JIT_H

#include "turi/env.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct FnDef;

typedef struct TuriInlineCJitHook {
    /* Compile `src` -- a complete Turmeric program of `len` bytes that
     * `(export ...)`s the function -- in process.  `name` is the Turmeric
     * name, for diagnostics and the scratch file.  On success returns 0 with
     * *out_image (kept resident by the caller for the life of the process)
     * and *out_manifest (malloc'd exports manifest, NUL-terminated).  On
     * failure returns -1, having printed the compiler's or c2mir's
     * diagnostics. */
    int   (*build)(const char *name, const char *src, size_t len,
                   void **out_image, char **out_manifest);
    /* Resolve a C symbol in an image `build` returned (static ones too). */
    void *(*sym)(void *image, const char *cname);
} TuriInlineCJitHook;

/* Install the host's compiler.  NULL uninstalls.  `tur` calls this once at
 * startup on a TUR_HAVE_JIT build. */
void turi_set_inline_c_jit_hook(const TuriInlineCJitHook *hook);

/* Called where the interpreter would otherwise report an inline-C body as
 * unsupported.  Returns false -- leaving *out untouched -- when the feature
 * does not apply (experiment off, no hook, not an inline-C defn), so the
 * caller keeps today's behaviour.  Returns true with *out set when it does:
 * the call's result, or an error value naming why the body could not be
 * compiled or called. */
bool turi_inline_c_jit_try(TuriEnv *env, struct FnDef *fn,
                           uint32_t param_offset,
                           TuriValue *args, uint32_t n_args,
                           TuriValue *out);

#ifdef __cplusplus
}
#endif

#endif /* TUR_TURI_INLINE_C_JIT_H */
