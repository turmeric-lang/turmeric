/* spice_loader.h -- RP3 of docs/spice-repl-plan.md.
 *
 * Discovers an enclosing spice project (walking up from the cwd to find
 * a build.tur), invokes `tur build --shared` if the cached library is
 * stale, dlopens the resulting .so, parses its exports.manifest, and
 * primes a symbol map that the REPL's binding layer (RP4) can consult
 * when the user evaluates `(import ...)`.
 *
 * Lifetime: a TurSpiceImage owns the dlopen handle and the parsed
 * export list. Free it with tur_spice_image_free; the REPL does this
 * implicitly by freeing the TuriEnv that holds the image.
 */
#ifndef TUR_SPICE_LOADER_H
#define TUR_SPICE_LOADER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Small-arity fast-path width for the FFI marshalling scratch (see
 * ffi_thunk.c): calls at or below it avoid a heap allocation.  It is NOT a cap
 * on how many parameters an exported spice symbol may have -- the descriptor's
 * arg_classes is a heap array of length n_args (arbitrary), and the
 * exports.manifest arg-class string is naturally variable-length.  A call whose
 * arg-shape exceeds the generated dispatcher's --max-arity still fails cleanly
 * at call time with a regenerate-the-table diagnostic; that is a separate
 * interpreter-FFI limit, not a descriptor limit. */
#define TUR_SPICE_ARITY_FASTPATH 16

/* interpreter-arbitrary-arity-ffi (Phase 2): the uniform-signature FFI shim
 * emitted next to each exported defn (see src/compiler/emit_module.c
 * emit_ffi_export_shims).  It reads the interpreter's marshalled int-register
 * / float args from `iv` / `fv` and writes the result to *out_i (int-class
 * return) or *out_f (float-class return); a :void return writes neither.
 * Calling through it lifts the shape-table arity ceiling entirely -- the shim
 * was generated with the export's concrete signature, so arity and int/float
 * mix are irrelevant.  NULL when the loaded .so predates shim emission; the
 * caller then falls back to the generated shape table (still capped). */
typedef void (*TurFfiShimFn)(const int64_t *iv, const double *fv,
                             int64_t *out_i, double *out_f);

/* One row from exports.manifest, post-parse. The dispatcher class chars
 * use the encoding shared with src/runtime/ffi_dispatch.h:
 *   'i' -- int64_t (covers :int / :cstr / :bool / :ptr / sized ints)
 *   'f' -- double  (covers :float / :float64)
 *   'v' -- void return only (never appears in arg_classes)
 *   '?' -- unrepresentable in v1 (e.g. :struct return); the binding
 *          layer must surface a clear error when the user calls it. */
typedef struct TurSpiceExport {
    char    *module;      /* defmodule name, e.g. "smokelib" */
    char    *name;        /* defn name, e.g. "add42" */
    char    *mangled;     /* C symbol name, e.g. "smokelib__add42" */
    void    *fn_ptr;      /* dlsym'd address */
    TurFfiShimFn ffi_shim; /* dlsym'd `<mangled>__ffi` shim, or NULL (old .so) */
    char     ret_class;
    char     ret_tag;     /* 'c' for a :cstr return, 'b' for :bool, else 0: the
                           * int-class word the shim returns is handed back as
                           * a string / boolean, not as the bare word */
    char    *arg_classes; /* heap array of length n_args (NULL iff n_args == 0) */
    uint32_t n_args;      /* unbounded -- no fixed arity cap; for a variadic
                           * export this INCLUDES the trailing rest-list slot
                           * (class 'i': the callee receives one int64 cons-list
                           * pointer) */
    bool     is_variadic; /* true when the manifest line had `& :tag` */
    char     rest_class;  /* ffi-spices-integration-plan S2: the rest ELEMENT
                           * class ('i' or 'f') from the manifest's `& :tag`,
                           * so the REPL marshaller knows whether cons heads
                           * carry integers or IEEE-754 bit patterns.  'i'
                           * when not variadic / tag unknown. */
} TurSpiceExport;

/* Opaque image handle. */
typedef struct TurSpiceImage TurSpiceImage;

/* Discover + build + dlopen + parse, all in one call.
 *
 * `start_dir`  -- walk-up origin (typically the cwd). Required.
 * `tur_bin`    -- path to the `tur` executable used for the rebuild
 *                 subprocess; pass NULL to default to "tur" (PATH lookup).
 * `out_image`  -- on success, receives a heap-allocated image. NULL
 *                 on the "no project here" path (return 1).
 *
 * Return codes:
 *    0 -- success; *out_image is populated.
 *    1 -- no build.tur found by walking up from start_dir; *out_image
 *         is NULL. This is the expected pure-Turmeric REPL path.
 *   -1 -- hard error (build failed, dlopen failed, manifest malformed).
 *         A descriptive message has already been printed to stderr;
 *         *out_image is NULL. */
int tur_spice_image_load(const char *start_dir, const char *tur_bin,
                         TurSpiceImage **out_image);

void tur_spice_image_free(TurSpiceImage *img);

/* Accessors used by RP4's binding layer (and by tests / status output). */
const char     *tur_spice_image_root  (const TurSpiceImage *img);
const char     *tur_spice_image_lib   (const TurSpiceImage *img);
uint32_t        tur_spice_image_count (const TurSpiceImage *img);
const TurSpiceExport *tur_spice_image_at(const TurSpiceImage *img,
                                          uint32_t index);

/* Look up an export by (module, name). Returns NULL if not present. */
const TurSpiceExport *tur_spice_image_find(const TurSpiceImage *img,
                                            const char *module,
                                            const char *name);

/* RP5: true when every .tur file under the image's build directory has
 * mtime <= the cached library's. False (rebuild needed) when any source
 * is newer, the library is missing, or img is NULL. Used by (reload) to
 * short-circuit when nothing changed. */
bool tur_spice_image_is_fresh(const TurSpiceImage *img);

/* ------------------------------------------------------------------ */
/* J2 (jit-engine-plan section 3.3): in-process JIT hook.               */
/* ------------------------------------------------------------------ */
/* When installed, tur_spice_image_load builds the spice IN PROCESS via
 * the MIR engine instead of the `tur build --shared` subprocess + dlopen:
 * `build` compiles the spice at build_dir and hands back an opaque image
 * plus the exports.manifest TEXT (malloc'd; the loader owns and frees
 * it); `sym` replaces dlsym against that image; `free_image` replaces
 * dlclose.  The hook lives behind a function-pointer table because the
 * engine is only linked into `tur` under -DTUR_JIT=ON while this loader
 * is part of tur_core -- main.c installs it at REPL start when the `jit`
 * experiment is enabled.  NULL (the default) keeps the subprocess path. */
typedef struct TurSpiceJitHook {
    int   (*build)(const char *build_dir, void **out_image,
                   char **out_manifest);
    void *(*sym)(void *image, const char *mangled);
    void  (*free_image)(void *image);
} TurSpiceJitHook;

void tur_spice_set_jit_hook(const TurSpiceJitHook *hook);

#ifdef __cplusplus
}
#endif

#endif /* TUR_SPICE_LOADER_H */
