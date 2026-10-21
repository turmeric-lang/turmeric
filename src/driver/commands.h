/* commands.h -- the subcommand entry points the dispatcher calls
 * (main-c-split-plan): each `tur <command>`'s `cmd_*` handler and its
 * `usage_*` text.  Sectioned by defining file like driver.h, and checked the
 * same way (tests/check-driver-headers.py). */
#ifndef TUR_DRIVER_COMMANDS_H
#define TUR_DRIVER_COMMANDS_H

#include <stdbool.h>
#include <stdint.h>

#include "diag.h"        /* ReaderType */
#include "turi/trace.h"  /* TurTraceGrain */

/* == main.c ================================================================ */

/* -- moving to driver/preamble_split.c -- */
int cmd_emit_rt_split(int argc, char **argv);

/* -- moving to driver/cmd_emit.c -- */
int cmd_emit_c(const char *path,
               const char **include_dirs, int n_include_dirs);
int cmd_emit_c_to_dir(const char *out_dir, char **inputs, int n_inputs,
                      const char **include_dirs, int n_include_dirs);
int cmd_emit_h(const char *path,
               const char **include_dirs, int n_include_dirs);

/* -- moving to driver/cmd_build.c -- */
int cmd_build(const char *input, const char *out_path,
              const char **include_dirs, int n_include_dirs,
              const char *target,
              const char **reader_macro_paths,
              int n_reader_macro_paths);

/* -- moving to driver/project.c -- */
int cmd_build_multi(const char *dir, const char *out_path, bool shared,
                    const char *manifest_path, const char *cli_build_dir);
int cmd_build_project(const char *root_in, const char *out_path,
                      bool shared, const char *manifest_path,
                      const char **user_inc, int n_user_inc,
                      const char *cli_build_dir,
                      const char *target);

/* -- moving to driver/compile_link.c -- */
int cmd_compile(const char *input, const char *out_obj,
                const char **include_dirs, int n_include_dirs,
                const char **reader_macro_paths,
                int n_reader_macro_paths);
int cmd_link(const char *out, const char **inputs, int n_inputs,
             bool shared, const char *extra_link_flags);

/* -- moving to driver/cmd_run.c -- */
int cmd_run(int argc, char **argv);
int cmd_test(const char *dir);
int cmd_check_dir(const char *dir);

/* -- moving to driver/jit.c -- */
int cmd_jit(int argc, char **argv);

/* == driver/cmd_format.c =================================================== */
int cmd_format(const char *path, bool check_only, bool diff_mode,
               bool lang_set, ReaderType force_lang);
int cmd_parse_check(const char *path_a, const char *path_b);
int cmd_fmt(int argc, char **argv);

/* == driver/cmd_eval.c ===================================================== */
int cmd_eval_inc(const char *path, bool use_color,
                 char **user_inc, int n_user_inc,
                 char **extra_argv, int extra_argc, bool debug);
int cmd_dap(void);
int cmd_trace(const char *path, const char *out_path,
              char **extra_argv, int extra_argc, uint32_t max_steps,
              TurTraceGrain grain);
int cmd_trace_dump(const char *path);
int cmd_eval_expr(const char *expr, bool use_color);
int cmd_repl(bool watch_mode);

/* == main.c ================================================================ */

/* -- moving to driver/worker.c -- */
int cmd_worker(void);

/* -- moving to driver/cmd_misc.c -- */
int cmd_image_info(const char *path);
int cmd_image_verify(const char *path, const char *binary);
int cmd_doc_cli(const char *sym);
int cmd_docs(int argc, char **argv);
int cmd_smt(int argc, char **argv);
int cmd_experiments(int argc, char **argv);
int cmd_dialects(int argc, char **argv);
int cmd_explain(const char *code);

/* -- moving to driver/cli.c -- */
int usage_build(void);
int usage_run(void);
int usage_expand(void);
int usage_check(void);
int usage_eval(void);
int usage_doc(void);
int usage_format(void);
int usage_parse_check(void);
int usage_test(void);
int usage_repl(void);
int usage_explain(void);

#endif /* TUR_DRIVER_COMMANDS_H */
