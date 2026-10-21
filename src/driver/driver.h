/* driver.h -- what the compiler driver's files share (main-c-split-plan).
 *
 * The `tur` executable's driver lives in src/driver/ beside main.c.  This
 * header declares the helpers and the state that cross between those files;
 * commands.h declares the `cmd_*` / `usage_*` entry points the dispatcher
 * calls.  A helper that only its own file uses stays `static` there: `tur`
 * exports its symbols for the JIT, so every name declared here is visible to
 * JIT-compiled programs.
 *
 * Sectioned by defining file, one `== driver/<file>.c ==` heading comment
 * each; tests/check-driver-headers.py fails when a prototype sits under a
 * file that does not define it.  Each global is defined in the file that owns
 * its logic and declared `extern` in that file's section. */
#ifndef TUR_DRIVER_H
#define TUR_DRIVER_H

/* Every driver file includes this header first, so these feature-test macros
 * precede every system header.
 *
 * _DARWIN_C_SOURCE exposes mkstemps and DT_REG on macOS without suppressing
 * POSIX extensions; on Linux _DEFAULT_SOURCE covers both. */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#if defined(__APPLE__)
#  ifndef _DARWIN_C_SOURCE
#    define _DARWIN_C_SOURCE
#  endif
#elif !defined(_WIN32)
/* Windows is excluded deliberately: MinGW reads _POSIX_C_SOURCE as "hide the
 * Win32 CRT names", which un-declares mkdir/getcwd and hides _finddata_t --
 * which in turn breaks <dirent.h> itself.  glibc needs this macro to EXPOSE
 * those declarations; on Windows it does the exact opposite. */
#  ifndef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE 200809L
#  endif
#endif

/* The system headers and platform shims the whole driver leans on.  A header
 * only one file needs (poll.h, sys/wait.h, the socket headers, ...) is
 * included by that file. */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <dirent.h>
#include "platform_proc.h"
#include "platform_fs.h"  /* realpath/mkdir/setenv/mkstemps/... on Windows */

#include "buf.h"
#include "diag.h"         /* ReaderType, LangDialect */
#include "elab.h"         /* Ls2ResolverCtx */
#include "emit.h"         /* ForcedAbiSpec, BorrowSpecInfo */
#include "experiments.h"  /* ExperimentSource */
#include "expr.h"
#include "pkg.h"          /* PkgManifest */
#include "turi/inline_c_jit.h"  /* TuriInlineCJitHook */
#include "turi/spice_loader.h"  /* TurSpiceJitHook */

#ifndef TUR_VERSION
#define TUR_VERSION "unknown"
#endif

/* Every `cc` the driver runs over EMITTED C gets this after the user's flags.
 * GCC's -Wmisleading-indentation (in -Wall) is quadratic on the long
 * brace-less `if` chains a Scheme program's prelude lowers to: with it, the
 * parse of a one-line `#lang r7rs` program took 2.9 s of a 6.4 s build; without
 * it, 0.1 s (docs/archive/r7rs-programs-compile-slowly.md).  Nobody reads the
 * indentation of generated C, and a harness's own TUR_CC_FLAGS still carry
 * -Wall, so the driver adds the opt-out itself rather than to each default.
 *
 * -fwrapv is a fact about the language, not a tuning knob: `int` is a 64-bit
 * two's-complement integer whose `+ - *` wrap, and the emitter lowers them to
 * the C operators on int64_t, where signed overflow is undefined.  Without it
 * the optimizer acted on that -- tur-prob's Count-Min returned 0 for every key
 * at -O2, correct at -O0 (docs/archive/emitted-c-signed-overflow-no-fwrapv.md).
 * It rides here, after TUR_CC_FLAGS, so a harness or user override that
 * replaces the defaults cannot drop it. */
#define TUR_EMITTED_C_CC_FLAGS " -Wno-misleading-indentation -fwrapv"

/* tur-link-and-build-split-plan Phase 2/3c/6: runtime-linkage mode, read by
 * apply_runtime_lib_mode() in cmd_build / cmd_compile.  Set by the build/compile
 * dispatch and seeded from TUR_RUNTIME.  Values:
 *   TUR_RT_AUTO   (0, the default) -- prefer the lean non-ASan libturt_runtime.a
 *                 when it is locatable, else transparently fall back to
 *                 recompiling the bare runtime sources.  Never links the
 *                 (possibly ASan) full libturi.a on its own, so a default build
 *                 is always behaviorally identical to the old source path.
 *                 Also takes the preamble split below when
 *                 preamble_split_auto_applies() says so, and quietly keeps the
 *                 whole preamble when it does not.
 *   TUR_RT_LIB    (1) -- force the archive link (lean preferred, else libturi.a,
 *                 else a -lturi fallback with a warning); explicit opt-in.
 *   TUR_RT_SOURCE (2) -- force recompiling the bare runtime sources.
 *   TUR_RT_SPLIT  (3) -- cc-path-preamble-split-plan: emit the DECLS region in
 *                 place of the fixed runtime preamble and link
 *                 libturt_preamble.a, so the preamble is compiled ONCE rather
 *                 than once per program.  4360 of 8310 emitted lines for a
 *                 one-line program are that preamble, byte-identical every
 *                 time; dropping it measures at 29% off the cc call.  This
 *                 mode INSISTS -- it says so when the swap declines -- where
 *                 AUTO only takes the split when every precondition holds. */
enum { TUR_RT_AUTO = 0, TUR_RT_LIB = 1, TUR_RT_SOURCE = 2, TUR_RT_SPLIT = 3 };

/* The split build's handle on its cached library unit; the cache is
 * described in preamble_split.c, above g_split_lib_cache_off. */
typedef struct {
    bool     on;          /* this compile may read and write the cache */
    bool     verify;      /* TUR_SPLIT_LIB_CACHE=verify */
    bool     no_restore;  /* the entry did not fit: emit, and replace it */
    uint64_t key;
} SplitLibCache;

/* == driver/paths.c ======================================================== */
extern const char *g_argv0;
extern int g_main_argc;
extern const char *const *g_main_argv;
void drv_json_escape(const char *s, char *out, size_t cap);
const char *drv_basename_of(const char *path);
size_t drv_source_stem_len(const char *name, size_t len);
int cc_system(const char *cmd);
ReaderType detect_and_adjust_lang(const char *path, char *src, size_t len,
                                 const char **out_src, size_t *out_len,
                                 LangDialect *out_dialect);
void drv_dir_of_path(const char *path, char *out, size_t cap);
int drv_get_exe_path(char *out, size_t cap);
const char *resolve_stdlib_root(void);
const char *tur_stdlib_path(const char *basename,
                            char *out, size_t outlen);
int drv_read_entire_file_quiet(const char *path, char **out, size_t *out_len);
int drv_read_entire_file(const char *path, char **out, size_t *out_len);
int drv_is_directory(const char *path);

/* == driver/spice.c ======================================================== */
extern bool g_user_config_experiments_done;
const char *drv_resolve_engine(const char *input_or_root,
                               const char *cli_flag);
char **resolve_manifest_reader_macros(const char *root_dir,
                                      const PkgManifest *m,
                                      int *n_out);
void free_reader_macro_paths(char **paths, int n);
char *drv_find_project_root(const char *start);
void apply_manifest_experiments(const PkgManifest *m);
void apply_user_config_experiments(const PkgManifest *m_or_null);
void apply_enclosing_manifest_experiments(const char *input_path);
char **discover_manifest_reader_macros(const char *input_path,
                                       int *n_out);
int auto_append_spice_includes(const char *input,
                               char ***inc, int *n_inc,
                               char ***owned, int *n_owned,
                               Ls2ResolverCtx *out_ls2);
void ls2_resolver_ctx_dispose(Ls2ResolverCtx *ctx);
char **collect_tur_files(const char *dir, int *n_out);
void free_tur_files(char **files, int n);
int drv_compare_cstr_ptrs(const void *a, const void *b);
void collect_tur_recursive(const char *dir,
                           char ***files, int *n, int *cap);
char **collect_tur_files_deep(const char *dir, int *n_out);
char **collect_project_src_files(const char *root, int *n_out);
void resolve_include_dirs_from_manifest(const char *root,
                                        const PkgManifest *m,
                                        bool include_own_src,
                                        const char ***out_dirs,
                                        int *out_n);
void collect_spice_aux_c(const char *root, Buf *includes, Buf *sources);
void resolve_project_include_dirs(const char *dir,
                                  const char ***out_dirs, int *out_n);

/* == driver/pipeline.c ===================================================== */
extern bool g_dump_kinds;
extern bool g_audit_spans;
extern int g_audit_span_holes;
extern Buf *g_manifest_sink;
extern Buf *g_split_lib_sink;
extern Buf *g_split_imp_sink;
extern bool g_import_split_failed;
extern bool g_split_lib_from_cache;
int import_split_pieces(void);
void resolve_rcgc_from_archive(void);
bool preamble_split_auto_applies(void);
int compile_to_c(const char *path, Buf *out_c,
                  const char **include_dirs, int n_include_dirs,
                  const char **reader_macro_paths,
                  int n_reader_macro_paths);
int compile_to_h(const char *path, Buf *out_h, const char *module_name,
                 const char **include_dirs, int n_include_dirs,
                 const char **reader_macro_paths,
                 int n_reader_macro_paths,
                 const ForcedAbiSpec *forced, uint32_t n_forced);
int compile_to_implementation(const char *path, Buf *out_c, const char *module_name,
                              const char **include_dirs, int n_include_dirs,
                              const char **reader_macro_paths,
                              int n_reader_macro_paths,
                              Buf *out_manifest,
                              const ForcedAbiSpec *forced, uint32_t n_forced,
                              BorrowSpecInfo **out_borrow_specs, uint32_t *out_n_borrow_specs);

/* == driver/cc.c =========================================================== */
extern int g_runtime_mode;
extern bool g_emit_for_link;
extern bool g_cc_release;
void hoist_tur_include_directives(Buf *csrc);
char *drv_last_path_sep(char *p);
bool drv_tmp_dir_is_ours(const char *dir);
const char *drv_stable_c_prefix(void);
FILE *drv_fopen_nofollow_w(const char *path);
void drv_stable_c_path(const char *input, char *out, size_t cap);
bool scan_autolink_markers(const Buf *csrc, Buf *autolink);
void append_manifest_link_flags(const char *proj_root, Buf *cmake_flags);
bool drv_collect_web_link_flags(const char *input, Buf *out);
void drv_collect_build_aux(const char *input, Buf *cmake_flags,
                           Buf *aux_includes, Buf *aux_sources,
                           const char *target);
void drv_append_include_tokens(Buf *dst, const char *flags);
int locate_runtime_lib(char *libdir, size_t dcap,
                       char *libname, size_t ncap);
void apply_runtime_lib_mode(Buf *autolink);
void autolink_regions_off(Buf *autolink);
void resolve_autolink_flags(Buf *autolink, const char *cc_flags,
                             bool *out_needs_asan);
bool drv_buf_put_quoted(Buf *b, const char *s);
void drv_replay_to_stderr(const char *path);
int drv_link_command_run(const char *cc, const char *cc_flags,
                         const char *inputs,
                         const Buf *aux_includes, const Buf *aux_sources,
                         const Buf *autolink, bool needs_asan,
                         const Buf *cmake_flags,
                         const char **include_dirs, int n_include_dirs,
                         const char *out_path, const char *cc_stderr_path);

/* == driver/preamble_split.c =============================================== */
extern bool g_split_lib_cache_off;
int prelude_pieces(void);
int prelude_split_object(const Buf *lib_c, const char *cc, const char *cc_flags,
                         const Buf *aux_includes, const Buf *autolink,
                         bool needs_asan, const Buf *cmake_flags,
                         const char **include_dirs, int n_include_dirs,
                         const char *piece_prefix, int np, bool accept_quoted,
                         char *obj, size_t obj_cap);
void split_lib_cache_init(SplitLibCache *c, const Expr *prog, const char *input);
bool split_lib_cache_restore(SplitLibCache *c, const Expr *prog, Buf *lib_r);
void split_lib_cache_record(SplitLibCache *c, const Expr *prog, const Buf *lib_r);
bool jit_try_split_preamble(Buf *csrc, Buf *out);

/* == main.c ================================================================ */

/* -- moving to driver/cmd_emit.c -- */
void drv_default_output_name(const char *input, char *out, size_t cap);
char *drv_resolve_build_dir(const char *input_or_root, const char *cli_flag);

/* -- moving to driver/project.c -- */
extern bool g_build_bundle_deps;
char **collect_used_attr_modules(const char *entry_path,
                                 const char **include_dirs,
                                 int n_include_dirs, int *n_out);

/* == driver/compile_link.c ================================================= */
void drv_obj_sibling_path(const char *obj, const char *suffix,
                          char *out, size_t cap);

/* == driver/jit.c ========================================================== */
#ifdef TUR_HAVE_JIT
extern const TurSpiceJitHook g_repl_jit_hook;
extern const TuriInlineCJitHook g_repl_inline_c_jit_hook;
#endif

/* == driver/cmd_format.c =================================================== */
bool fmt_reader_from_lang_name(const char *lang, ReaderType *out);

/* == main.c ================================================================ */

/* -- moving to driver/cli.c -- */
extern bool g_no_auto_spice;
extern bool g_no_auto_stdlib;
extern bool g_no_abi_cache;
int drv_usage_error(int (*usage)(void));
bool is_known_deprecated_x_flag(const char *s);
void warn_deprecated_x_flag(const char *name);
bool enable_experiment_list(const char *list, ExperimentSource src);
bool drv_is_include_flag(int argc, char **argv, int i, int *consumed);
int drv_parse_include_flags(int argc, char **argv, int start, char ***out_dirs);

#endif /* TUR_DRIVER_H */
