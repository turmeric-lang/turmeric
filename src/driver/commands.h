/* commands.h -- the subcommand entry points (main-c-split-plan): each
 * `tur <command>`'s `cmd_*` functions and its `usage_*` text.
 *
 * A `cmd_<name>_main(argc, argv)` is what the command table in cli.c runs for
 * `tur <name>`: argv is the whole command line with the global flags already
 * stripped, so argv[1] is the command word as typed (perhaps a prefix or an
 * alias) and its own arguments start at argv[2].  The table points straight
 * at a `cmd_*` that already takes (argc, argv).  The other `cmd_*` functions
 * do a command's work once its arguments are parsed.
 *
 * Sectioned by defining file like driver.h, and checked the same way
 * (tests/check-driver-headers.py). */
#ifndef TUR_DRIVER_COMMANDS_H
#define TUR_DRIVER_COMMANDS_H

#include <stdbool.h>

/* == driver/preamble_split.c =============================================== */
int cmd_emit_rt_split(int argc, char **argv);

/* == driver/cmd_emit.c ===================================================== */
int cmd_emit_c_main(int argc, char **argv);
int cmd_emit_h_main(int argc, char **argv);
int cmd_check_main(int argc, char **argv);
int cmd_expand_main(int argc, char **argv);
int cmd_audit_spans_main(int argc, char **argv);

/* == driver/cmd_build.c ==================================================== */
int cmd_build(const char *input, const char *out_path,
              const char **include_dirs, int n_include_dirs,
              const char *target,
              const char **reader_macro_paths,
              int n_reader_macro_paths);
int cmd_build_main(int argc, char **argv);

/* == driver/project.c ====================================================== */
int cmd_build_multi(const char *dir, const char *out_path, bool shared,
                    const char *manifest_path, const char *cli_build_dir);
int cmd_build_project(const char *root_in, const char *out_path,
                      bool shared, const char *manifest_path,
                      const char **user_inc, int n_user_inc,
                      const char *cli_build_dir,
                      const char *target);

/* == driver/compile_link.c ================================================= */
int cmd_compile(const char *input, const char *out_obj,
                const char **include_dirs, int n_include_dirs,
                const char **reader_macro_paths,
                int n_reader_macro_paths);
int cmd_link(const char *out, const char **inputs, int n_inputs,
             bool shared, const char *extra_link_flags);
int cmd_compile_main(int argc, char **argv);
int cmd_link_main(int argc, char **argv);

/* == driver/cmd_run.c ====================================================== */
int cmd_run(int argc, char **argv);
int cmd_check_dir(const char *dir);
int cmd_run_main(int argc, char **argv);
int cmd_test_main(int argc, char **argv);
int cmd_serve(int argc, char **argv);
/* project-less-tools-plan PT-D1: the one "build entry, run with args" path
 * `tur run`, `tur serve` and `tur x` share. */
int drv_exec_program(const char *exe, char **prog_argv, int prog_argc);
int drv_run_program(const char *engine, const char *entry,
                    char **inc, int n_inc,
                    char **prog_argv, int prog_argc);

/* == driver/cmd_x.c ======================================================== */
int cmd_x(int argc, char **argv);
/* namespaced-spice-binaries D3: `<spice>/<bin>` -- one `/`, a name on each
 * side -- runs that installed spice's binary whether or not it holds the
 * bare name.  `tur <spice>/<bin>` and `tur x <spice>/<bin>` share it. */
bool drv_qualified_bin_shaped(const char *w);
int  drv_run_qualified_bin(const char *who, const char *word,
                           char **prog_argv, int prog_argc);

/* == driver/jit.c ========================================================== */
int cmd_jit(int argc, char **argv);

/* == driver/cmd_format.c =================================================== */
int cmd_fmt(int argc, char **argv);
int cmd_format_main(int argc, char **argv);
int cmd_parse_check_main(int argc, char **argv);

/* == driver/cmd_eval.c ===================================================== */
int cmd_eval_inc(const char *path, bool use_color,
                 char **user_inc, int n_user_inc,
                 char **extra_argv, int extra_argc, bool debug);
int cmd_dap(void);
int cmd_trace_main(int argc, char **argv);
int cmd_repl_main(int argc, char **argv);
int cmd_interpret_main(int argc, char **argv);
int cmd_debug_main(int argc, char **argv);
int cmd_eval_main(int argc, char **argv);

/* == driver/worker.c ======================================================= */
int cmd_worker(void);

/* == driver/cmd_misc.c ===================================================== */
int cmd_docs(int argc, char **argv);
int cmd_smt(int argc, char **argv);
int cmd_experiments(int argc, char **argv);
int cmd_dialects(int argc, char **argv);
int cmd_explain(const char *code);
int cmd_lsp_main(int argc, char **argv);
int cmd_mcp_main(int argc, char **argv);
int cmd_lsp_lite_main(int argc, char **argv);
int cmd_doc_main(int argc, char **argv);
int cmd_image_info_main(int argc, char **argv);
int cmd_image_verify_main(int argc, char **argv);
int cmd_explain_main(int argc, char **argv);

/* == driver/cli.c ========================================================== */
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
