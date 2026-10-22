/* compile_link.c -- `tur compile` and `tur link`
 * (tur-link-and-build-split-plan), and the `.link` sidecar that carries a
 * compile's link inputs to the link.
 *
 * Part of the compiler driver (src/driver/); driver.h says how the files
 * share declarations. */

#include "driver.h"
#include "commands.h"

#include "globals.h"

/* tur-link-and-build-split-plan Phase 3: derive the `.link` sidecar path for an
 * object -- strip a trailing ".o" and append ".link"; otherwise append ".link"
 * to the whole name.  Also used to derive the intermediate ".c" path (with a
 * ".c" suffix) for `tur compile`. */
void drv_obj_sibling_path(const char *obj, const char *suffix,
                          char *out, size_t cap) {
    size_t n = strlen(obj);
    if (n >= 2 && obj[n - 2] == '.' && obj[n - 1] == 'o') n -= 2;
    snprintf(out, cap, "%.*s%s", (int)n, obj, suffix);
}

/* Write one sidecar field line.  Handles both NUL-terminated buffers (autolink)
 * and non-terminated ones (cmake/aux) by writing exact content bytes minus a
 * single trailing NUL. */
static void sidecar_write_field(FILE *f, const char *key, const Buf *b) {
    fputs(key, f);
    if (b && b->len > 0) {
        size_t n = b->len;
        if (b->data[n - 1] == '\0') n--;
        if (n > 0) fwrite(b->data, 1, n, f);
    }
    fputc('\n', f);
}

/* tur-link-and-build-split-plan Phase 3a-A: write the `.link` sidecar next to a
 * compiled object.  It records the fully resolved link flags so `tur link` can
 * reproduce the link without re-running the frontend -- the single source of
 * truth for link-flag drift (plan section 7). */
static int write_link_sidecar(const char *path, const Buf *autolink,
                              bool needs_asan, const Buf *cmake_flags,
                              const Buf *aux_sources) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fputs("# tur link sidecar v1\n", f);
    fprintf(f, "asan: %d\n", needs_asan ? 1 : 0);
    sidecar_write_field(f, "autolink:", autolink);
    sidecar_write_field(f, "cmake:", cmake_flags);
    sidecar_write_field(f, "auxsrc:", aux_sources);
    fclose(f);
    return 0;
}

/* Append `val` to `dst` (NUL-terminating) only if that exact value has not been
 * appended before, tracked via the caller's seen-list.  Keeps `tur link` from
 * duplicating identical sidecar content when several objects of one program
 * each carry the same resolved flags (which would re-link a runtime .c twice
 * and fail with multiple-definition). */
static void sidecar_accum_unique(Buf *dst, char ***seen, int *n_seen,
                                 const char *val) {
    if (!val || !*val) return;
    for (int i = 0; i < *n_seen; i++)
        if (strcmp((*seen)[i], val) == 0) return;
    *seen = (char **)realloc(*seen, sizeof(char *) * (size_t)(*n_seen + 1));
    (*seen)[(*n_seen)++] = tur_strdup(val);
    buf_puts(dst, val);
}

/* Read one object's `.link` sidecar, accumulating its fields into the caller's
 * link-flag buffers.  ASan is OR-accumulated; the string fields are unioned via
 * sidecar_accum_unique.  Missing sidecar is not an error (a bare .o may have
 * none). */
static void read_link_sidecar(const char *path, Buf *autolink, bool *needs_asan,
                              Buf *cmake_flags, Buf *aux_sources,
                              char ***seen_al, int *n_seen_al,
                              char ***seen_cm, int *n_seen_cm,
                              char ***seen_ax, int *n_seen_ax) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char line[8192];
    while (fgets(line, sizeof(line), f)) {
        size_t L = strlen(line);
        while (L > 0 && (line[L - 1] == '\n' || line[L - 1] == '\r'))
            line[--L] = '\0';
        if (strncmp(line, "asan:", 5) == 0) {
            if (atoi(line + 5) != 0) *needs_asan = true;
        } else if (strncmp(line, "autolink:", 9) == 0) {
            sidecar_accum_unique(autolink, seen_al, n_seen_al, line + 9);
        } else if (strncmp(line, "cmake:", 6) == 0) {
            sidecar_accum_unique(cmake_flags, seen_cm, n_seen_cm, line + 6);
        } else if (strncmp(line, "auxsrc:", 7) == 0) {
            sidecar_accum_unique(aux_sources, seen_ax, n_seen_ax, line + 7);
        }
    }
    fclose(f);
}

/* tur-link-and-build-split-plan Phase 3a-0: `tur compile <in.tur> -o <out.o>`.
 * Lowers one Turmeric source to an object file (the emit-c lowering fused with
 * `cc -c`) plus a `.link` sidecar carrying the resolved link flags.  This is
 * the cacheable half of the compile/link pipeline -- ccache hits on unchanged
 * input.  `emit-c` is untouched; this is the additive fused step. */
int cmd_compile(const char *input, const char *out_obj,
                const char **include_dirs, int n_include_dirs,
                const char **reader_macro_paths,
                int n_reader_macro_paths) {
    /* Frontend: lower to C, force-loading any #[used] sibling modules just as
     * cmd_build does (single-file whole-program TU). */
    Buf csrc;
    buf_init(&csrc);
    int n_used_mods = 0;
    char **used_mods = collect_used_attr_modules(input, include_dirs,
                                                 n_include_dirs, &n_used_mods);
    UsedModulesCtx used_ctx = { (const char **)used_mods, n_used_mods };
    if (n_used_mods > 0) used_modules_ctx_set(&used_ctx);
    int rc = compile_to_c(input, &csrc, include_dirs, n_include_dirs,
                          reader_macro_paths, n_reader_macro_paths);
    if (n_used_mods > 0) used_modules_ctx_set(NULL);
    free_tur_files(used_mods, n_used_mods);
    if (rc != 0) { buf_free(&csrc); return rc; }
    hoist_tur_include_directives(&csrc);

    /* Resolve the object path (default: <name>.o beside cwd). */
    char obj_buf[1024];
    if (!out_obj) {
        char base[512];
        drv_default_output_name(input, base, sizeof(base));
        snprintf(obj_buf, sizeof(obj_buf), "%s.o", base);
        out_obj = obj_buf;
    }

    /* Write the generated C next to the object (deterministic -> ccache-able). */
    char cpath[1100];
    drv_obj_sibling_path(out_obj, ".c", cpath, sizeof(cpath));
    FILE *tf = fopen(cpath, "wb");
    if (!tf || fwrite(csrc.data, 1, csrc.len, tf) != csrc.len) {
        fprintf(stderr, "tur compile: cannot write generated C to '%s'\n", cpath);
        if (tf) fclose(tf);
        buf_free(&csrc);
        return 2;
    }
    fclose(tf);

    Buf autolink;
    buf_init(&autolink);
    bool al_ok = scan_autolink_markers(&csrc, &autolink);
    buf_free(&csrc);
    if (!al_ok) { buf_free(&autolink); return 2; }

    /* Phase 2/3c: --runtime=lib swaps bare runtime sources for libturi.a so the
     * sidecar records the -lturi link and the object never carries the runtime
     * TUs.  cmd_compile is native-only, so no wasm guard is needed. */
    apply_runtime_lib_mode(&autolink);
    autolink_regions_off(&autolink);

    const char *cc = getenv("CC");
    if (!cc || !*cc) cc = "cc";
    const char *cc_flags = getenv("TUR_CC_FLAGS");
    if (!cc_flags || !*cc_flags) {
        cc_flags = g_emit_debug_lines
            ? "-g -Og -std=c99 -Wall -fno-strict-aliasing"
            : g_cc_release ? "-O3 -std=c99 -Wall -fno-strict-aliasing"
                           : "-O2 -std=c99 -Wall -fno-strict-aliasing";
    }

    Buf cmake_flags;  buf_init(&cmake_flags);
    Buf aux_includes; buf_init(&aux_includes);
    Buf aux_sources;  buf_init(&aux_sources);
    drv_collect_build_aux_with_installed(input, &cmake_flags, &aux_includes, &aux_sources,
                                         NULL);

    bool needs_asan = false;
    resolve_autolink_flags(&autolink, cc_flags, &needs_asan);

    /* The cacheable `cc -c`.  Include dirs come from the spice aux includes,
     * cmake -I, the autolink -I tokens (e.g. -Isrc/runtime), and any explicit
     * -I.  -l/-L and bare .c sources are link-time only and stay out of here. */
    Buf cmd;
    buf_init(&cmd);
    bool cc_quoted_ok = true;
    buf_printf(&cmd, "%s %s" TUR_EMITTED_C_CC_FLAGS, cc, cc_flags);
    if (aux_includes.len > 0) buf_puts(&cmd, aux_includes.data);
    if (cmake_flags.len > 0)  buf_puts(&cmd, cmake_flags.data);
    drv_append_include_tokens(&cmd, autolink.len > 0 ? autolink.data : NULL);
    for (int i = 0; i < n_include_dirs; i++) {
        if (include_dirs[i] && include_dirs[i][0]) {
            buf_puts(&cmd, " -I");
            cc_quoted_ok = drv_buf_put_quoted(&cmd, include_dirs[i]) && cc_quoted_ok;
        }
    }
    buf_puts(&cmd, " -c ");
    cc_quoted_ok = drv_buf_put_quoted(&cmd, cpath) && cc_quoted_ok;
    buf_puts(&cmd, " -o ");
    cc_quoted_ok = drv_buf_put_quoted(&cmd, out_obj) && cc_quoted_ok;
    buf_putc(&cmd, '\0');
    if (!cc_quoted_ok) {
        fprintf(stderr, "tur compile: a path was too long to quote\n");
        buf_free(&cmd); buf_free(&aux_includes);
        buf_free(&autolink); buf_free(&cmake_flags);
        return 2;
    }
    if (getenv("TUR_SHOW_CC")) fprintf(stderr, "CC: %s\n", cmd.data);
    int sys_rc = cc_system(cmd.data);
    buf_free(&cmd);
    buf_free(&aux_includes);

    if (sys_rc != 0) {
        fprintf(stderr, "tur compile: cc -c invocation failed (status %d)\n", sys_rc);
        buf_free(&autolink);
        buf_free(&cmake_flags);
        buf_free(&aux_sources);
        return 2;
    }

    /* Emit the .link sidecar carrying the resolved link flags. */
    char side[1100];
    drv_obj_sibling_path(out_obj, ".link", side, sizeof(side));
    if (write_link_sidecar(side, &autolink, needs_asan, &cmake_flags,
                           &aux_sources) != 0) {
        fprintf(stderr, "tur compile: cannot write link sidecar '%s'\n", side);
        buf_free(&autolink);
        buf_free(&cmake_flags);
        buf_free(&aux_sources);
        return 2;
    }

    buf_free(&autolink);
    buf_free(&cmake_flags);
    buf_free(&aux_sources);
    return 0;
}

/* tur-link-and-build-split-plan Phase 3a: `tur link <obj-or-source>... -o <out>`.
 * Links precompiled objects (+ their `.link` sidecars) and/or .c sources into an
 * executable (or a shared library with --shared).  Reuses drv_link_command_run --
 * the same link path `tur build` drives -- so the two cannot drift. */
int cmd_link(const char *out, const char **inputs, int n_inputs,
             bool shared, const char *extra_link_flags) {
    if (n_inputs == 0) {
        fprintf(stderr, "tur link: no input objects/sources\n");
        return 1;
    }

    /* Join the input paths and gather link flags from each object's sidecar. */
    Buf inputs_joined; buf_init(&inputs_joined);
    Buf autolink;      buf_init(&autolink);
    Buf cmake_flags;   buf_init(&cmake_flags);
    Buf aux_sources;   buf_init(&aux_sources);
    bool needs_asan = false;
    char **seen_al = NULL; int n_seen_al = 0;
    char **seen_cm = NULL; int n_seen_cm = 0;
    char **seen_ax = NULL; int n_seen_ax = 0;

    bool inputs_quoted_ok = true;
    for (int i = 0; i < n_inputs; i++) {
        if (inputs_joined.len > 0) buf_putc(&inputs_joined, ' ');
        inputs_quoted_ok = drv_buf_put_quoted(&inputs_joined, inputs[i])
                           && inputs_quoted_ok;
        /* A .o (or bare object) may carry a sidecar; look it up. */
        char side[1100];
        drv_obj_sibling_path(inputs[i], ".link", side, sizeof(side));
        read_link_sidecar(side, &autolink, &needs_asan, &cmake_flags,
                          &aux_sources, &seen_al, &n_seen_al,
                          &seen_cm, &n_seen_cm, &seen_ax, &n_seen_ax);
    }
    buf_putc(&inputs_joined, '\0');
    for (int i = 0; i < n_seen_al; i++) free(seen_al[i]);
    for (int i = 0; i < n_seen_cm; i++) free(seen_cm[i]);
    for (int i = 0; i < n_seen_ax; i++) free(seen_ax[i]);
    free(seen_al); free(seen_cm); free(seen_ax);

    /* User-supplied extra link flags fold into the autolink buffer (which
     * drv_link_command_run appends verbatim after the inputs). */
    if (extra_link_flags && *extra_link_flags) {
        if (autolink.len > 0) buf_putc(&autolink, ' ');
        buf_puts(&autolink, extra_link_flags);
    }
    /* NUL-terminate the accumulated flag buffers so drv_link_command_run's `%s` /
     * buf_puts reads stop cleanly. */
    if (autolink.len > 0)    buf_putc(&autolink, '\0');
    if (cmake_flags.len > 0) buf_putc(&cmake_flags, '\0');
    if (aux_sources.len > 0) buf_putc(&aux_sources, '\0');

    /* --shared rides on cc_flags (position-independent shared object). */
    const char *cc = getenv("CC");
    if (!cc || !*cc) cc = "cc";
    const char *base_flags = getenv("TUR_CC_FLAGS");
    if (!base_flags || !*base_flags)
        base_flags = g_cc_release ? "-O3 -fno-strict-aliasing" : "-O2 -fno-strict-aliasing";
    Buf cc_flags; buf_init(&cc_flags);
    buf_puts(&cc_flags, base_flags);
    if (shared) buf_puts(&cc_flags, " -shared -fPIC");
    buf_putc(&cc_flags, '\0');

    char out_buf[1024];
    if (!out) {
        drv_default_output_name(inputs[0], out_buf, sizeof(out_buf));
        out = out_buf;
    }

    if (!inputs_quoted_ok) {
        fprintf(stderr, "tur link: an input path was too long to quote\n");
        buf_free(&inputs_joined); buf_free(&autolink);
        buf_free(&cmake_flags); buf_free(&aux_sources); buf_free(&cc_flags);
        return 2;
    }
    int link_rc = drv_link_command_run(cc, cc_flags.data, inputs_joined.data,
                                       NULL, &aux_sources, &autolink, needs_asan,
                                       &cmake_flags, NULL, 0, out, NULL);
    buf_free(&inputs_joined);
    buf_free(&autolink);
    buf_free(&cmake_flags);
    buf_free(&aux_sources);
    buf_free(&cc_flags);
    return link_rc;
}

/* -------------------------------------------------------------------------
 * Entry points: the command table in cli.c runs these for `tur <command>`.
 * ---------------------------------------------------------------------- */

/* tur-link-and-build-split-plan Phase 3a-0: tur compile <in.tur> -o <out.o> */
int cmd_compile_main(int argc, char **argv) {
    const char *input = NULL;
    const char *out = NULL;
    const char *cli_build_dir = NULL;
    char  **comp_inc = NULL;
    int     n_comp_inc = drv_parse_include_flags(argc, argv, 2, &comp_inc);
    if (n_comp_inc < 0) { free(comp_inc); return drv_usage_error(usage_build); }
    for (int i = 2; i < argc; i++) {
        int c;
        if (drv_is_include_flag(argc, argv, i, &c)) { i += c - 1; continue; }
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            free(comp_inc); return usage_build();
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            out = argv[++i];
        } else if ((strcmp(argv[i], "--build-dir") == 0 ||
                    strcmp(argv[i], "-B") == 0) && i + 1 < argc) {
            cli_build_dir = argv[++i];
        } else if (strncmp(argv[i], "--runtime=", 10) == 0 ||
                   (strcmp(argv[i], "--runtime") == 0 && i + 1 < argc)) {
            const char *mode = argv[i][9] == '=' ? argv[i] + 10 : argv[++i];
            if (strcmp(mode, "lib") == 0) g_runtime_mode = TUR_RT_LIB;
            else if (strcmp(mode, "source") == 0) g_runtime_mode = TUR_RT_SOURCE;
            else if (strcmp(mode, "auto") == 0) g_runtime_mode = TUR_RT_AUTO;
            else {
                fprintf(stderr, "tur compile: unknown --runtime '%s' "
                        "(supported: auto, lib, source)\n", mode);
                free(comp_inc); return 1;
            }
        } else if (strcmp(argv[i], "--no-abi-cache") == 0 ||
                   strcmp(argv[i], "--no-r7rs-gc") == 0 ||
                   strcmp(argv[i], "--no-saffron-gc") == 0) {
            /* global, consumed elsewhere */
        } else if (argv[i][0] != '-') {
            if (input) { free(comp_inc); return drv_usage_error(usage_build); }
            input = argv[i];
        } else {
            free(comp_inc); return drv_usage_error(usage_build);
        }
    }
    if (!input) { free(comp_inc); return drv_usage_error(usage_build); }
    if (drv_is_directory(input)) {
        fprintf(stderr, "tur compile: expects a single .tur file, not a directory\n");
        free(comp_inc); return 1;
    }
    /* Default the object under <build-dir>/obj/ when --build-dir/-B (or a
     * manifest :build-dir) applies and no explicit -o was given. */
    char obj_default[1200];
    if (!out) {
        char *bd = drv_resolve_build_dir(input, cli_build_dir);
        if (bd) {
            char base[512];
            drv_default_output_name(input, base, sizeof(base));
            snprintf(obj_default, sizeof(obj_default), "%s/obj/%s.o", bd, base);
            out = obj_default;
            free(bd);
        }
    }
    /* Reader macros, experiments + spice include auto-discovery,
     * mirroring cmd_build. */
    char **c_rm = NULL; int c_n = 0;
    c_rm = discover_manifest_reader_macros(input, &c_n);
    char **c_owned = NULL; int n_c_owned = 0;
    Ls2ResolverCtx c_ls2 = {0};
    auto_append_spice_includes(input, &comp_inc, &n_comp_inc,
                               &c_owned, &n_c_owned, &c_ls2);
    ls2_resolver_ctx_set(&c_ls2);
    int rc = cmd_compile(input, out, (const char **)comp_inc, n_comp_inc,
                         (const char **)c_rm, c_n);
    ls2_resolver_ctx_set(NULL);
    ls2_resolver_ctx_dispose(&c_ls2);
    for (int i = 0; i < n_c_owned; i++) free(c_owned[i]);
    free(c_owned);
    free_reader_macro_paths(c_rm, c_n);
    free(comp_inc);
    return rc;
}

/* tur-link-and-build-split-plan Phase 3a: tur link <obj/src>... -o <out> */
int cmd_link_main(int argc, char **argv) {
    const char *out = NULL;
    bool shared = false;
    const char *link_flags = NULL;
    const char **inputs = (const char **)malloc((size_t)argc * sizeof(char *));
    int n_inputs = 0;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            free(inputs); return usage_build();
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            out = argv[++i];
        } else if (strcmp(argv[i], "--shared") == 0) {
            shared = true;
        } else if (strcmp(argv[i], "--link-flags") == 0 && i + 1 < argc) {
            link_flags = argv[++i];
        } else if (argv[i][0] != '-') {
            inputs[n_inputs++] = argv[i];
        } else {
            fprintf(stderr, "tur link: unknown option '%s'\n", argv[i]);
            free(inputs); return drv_usage_error(usage_build);
        }
    }
    if (n_inputs == 0) { free(inputs); return drv_usage_error(usage_build); }
    int rc = cmd_link(out, inputs, n_inputs, shared, link_flags);
    free(inputs);
    return rc;
}
