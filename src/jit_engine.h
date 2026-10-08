/* jit_engine.h -- the in-process MIR JIT engine behind `tur jit` (Phase J1).
 *
 * Built under -DTUR_JIT=ON into tur_jit_obj, which BOTH `tur` and libturi
 * pick up (I4, docs/archive/mir-interp-tier-plan.md section 2.4).  cmd_jit
 * in main.c compiles against this header unconditionally and reports the
 * missing capability when TUR_HAVE_JIT is not defined.  See jit_engine.c and
 * docs/archive/jit-engine-plan.md section 3.2.
 *
 * ---------------------------------------------------------------------
 * EMBEDDING (I4)
 * ---------------------------------------------------------------------
 * A C host that links libturi can use this API directly instead of shelling
 * out to `tur jit`.  Three things the host is responsible for:
 *
 * 1. Probe with `#ifdef TUR_HAVE_JIT`.  libturi defines it PUBLIC-ly when
 *    the library was built with an engine, so the probe propagates to your
 *    target automatically -- no manual define, and no link error if the
 *    library happens to have been built without one.
 *
 * 2. Export your own symbols.  The engine resolves the compiled program's
 *    externals with dlsym(RTLD_DEFAULT), so a host whose symbols are not
 *    exported will see the JIT'd code fail to find the runtime it is
 *    linked against.  With CMake that is
 *    `set_target_properties(<host> PROPERTIES ENABLE_EXPORTS TRUE)`; by
 *    hand it is `-rdynamic` / `-Wl,--export-dynamic`.  `tur` and the
 *    tur_jit_embed test both set it.
 *
 * 3. Fall back to the tree-walking interpreter, not to cc.  `tur jit`'s
 *    step-6 fallback shells out to a C compiler; an embedded host generally
 *    has no toolchain at runtime, so the fallback that is actually
 *    available to it is turi_eval (already linked -- same library).
 *    tests/turi/jit-embed.c shows both paths.
 *
 * Note the platform constraint that comes with this: a process that JITs is
 * a JIT application.  On Apple platforms that means MAP_JIT and, for a
 * notarized binary with hardened runtime, the
 * com.apple.security.cs.allow-jit entitlement -- and it is unavailable on
 * iOS.  A host that cannot accept that should build libturi without
 * TUR_JIT and use the interpreter, which generates no code at all. */
#ifndef TUR_JIT_ENGINE_H
#define TUR_JIT_ENGINE_H

#include <stdbool.h>
#include <stddef.h>

/* Outcomes of a JIT attempt.  Anything other than TUR_JIT_OK means the
 * program did NOT run and the caller should take the plan's step-6 fallback
 * to the cc path (with a diagnostic; c2mir already printed the specifics
 * for COMPILE failures). */
enum {
    TUR_JIT_OK          = 0,   /* ran; *prog_rc holds the program's exit code */
    TUR_JIT_ERR_COMPILE = 1,   /* c2mir rejected the C (subset gap / inline-C) */
    TUR_JIT_ERR_LINK    = 2,   /* an autolinked library could not be loaded */
    TUR_JIT_ERR_RUN     = 3,   /* engine-level failure setting up execution */
};

/* Compile `csrc` (the emitted C, post hoist_tur_include_directives) in
 * process and run its main with prog_argc/prog_argv.  `autolink` is the
 * resolved __tur_autolink__ flag string (may be NULL); -l entries are
 * dlopen'd RTLD_GLOBAL so dlsym can reach them.  `include_dirs` is where
 * `#include "hamt.h"` et al. resolve -- the caller (cmd_jit) supplies the
 * Turmeric tree / SDK dirs it already knows how to find. */
int tur_jit_execute(const char *csrc, size_t csrc_len, const char *autolink,
                    const char **include_dirs, int n_include_dirs,
                    int prog_argc, char **prog_argv, int *prog_rc);

/* B4 (post-jit-benchmark-resurrection-plan): phase timings of the most
 * recent tur_jit_execute in this process.  compile_ms covers c2mir +
 * MIR_link (+ eager generation when TUR_JIT_GEN=eager); run_ms is the entry
 * thread's wall time, which under the default lazy gen interface includes
 * first-call code generation.  Backs `tur jit --timing-json`. */
void tur_jit_last_timings(double *compile_ms, double *run_ms);

/* ------------------------------------------------------------------ */
/* J2 (plan section 3.3): persistent image mode for the REPL.          */
/* ------------------------------------------------------------------ */
/* A TurJitImage is a compiled-and-linked module set (a whole spice) kept
 * resident so its functions can be called repeatedly from the REPL --
 * the in-process replacement for the `tur build --shared` + dlopen path.
 *
 * Lifecycle: tur_jit_compile_image compiles the emitted C, links against
 * this process (same resolver as tur_jit_execute), eagerly generates
 * code, and runs the module's `__tur_static_init` (c2mir discards the
 * constructor attribute, so it must be called explicitly -- the same S1b
 * fact that shaped `main`).  Exported (or static) functions are then
 * resolved by tur_jit_image_sym -- MIR item lookup by name, which unlike
 * dlsym sees static functions, so the single-TU spice emission needs no
 * linkage changes.  tur_jit_image_free tears the context down; any
 * pointer obtained from the image is dead after that, so the caller must
 * rebind before freeing (the REPL's (reload) order).
 *
 * The C front end's state is released as soon as the image is initialized
 * (TUR_JIT_KEEP_C2MIR=1 keeps it to tur_jit_image_free, as before
 * 2026-10-02): it was most of an image's resident memory, ~25 MB of parsed
 * headers and declarations per image, and the REPL keeps images for the whole
 * session (aot-compiled-repl-plan C1). */
typedef struct TurJitImage TurJitImage;

/* Returns TUR_JIT_OK and sets *out on success; on failure returns the
 * TUR_JIT_ERR_* class (diagnostics already printed) and *out is NULL. */
int tur_jit_compile_image(const char *csrc, size_t csrc_len,
                          const char *autolink,
                          const char **include_dirs, int n_include_dirs,
                          TurJitImage **out);

/* Address of the generated function named `name` (mangled C name), or
 * NULL if the image has no such function item. */
void *tur_jit_image_sym(TurJitImage *img, const char *name);

void tur_jit_image_free(TurJitImage *img);

/* aot-compiled-repl-plan C1: suppress c2mir's WARNINGS (not its errors) for
 * the compiles that follow, returning the previous setting so a caller can put
 * it back.  c2mir warns on things the system headers do that it does not model
 * -- `unknown pragma`, an empty struct -- several lines per compile, which is
 * tolerable once per `tur jit` run and is not at a REPL prompt that compiles
 * an inline-C defn on its first call.  Off by default; `tur jit` keeps them. */
bool tur_jit_set_quiet_warnings(bool quiet);

#endif /* TUR_JIT_ENGINE_H */
