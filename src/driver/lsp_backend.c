/* lsp_backend.c -- the native LSP and MCP analysis backend: tur_collect_symbols
 * and tur_check_only, which run the whole compile_to_c pipeline over a buffer.
 *
 * Kept out of pipeline.c so the front half (tur_front: pipeline, preamble_split,
 * cc, paths, spice) carries no LSP entry point of its own.  The browser module
 * supplies a cheaper tur_collect_symbols in web/wasm_lsp.c (it stops after
 * elaboration), and the two would otherwise collide when the front half joins
 * that module (wasm-jit-plan P3).
 */

#include "driver.h"

#include "experiments.h"
#include "globals.h"
#include "lsp/lsp_collect.h"
#include "lsp/lsp_sym.h"
#include "script_deps.h"

/* -------------------------------------------------------------------------
 * LSP symbol collection (LD1)
 * --------------------------------------------------------------------- */

int tur_collect_symbols(const char *path, const char *logical_path,
                        LspSymbol *out, int cap, int *count_out) {
    lsp_collect_begin(out, cap, count_out);

    /* Resolve the spice from where the buffer LIVES, not from where it was
     * written.
     *
     * The LSP writes the editor's text to a scratch file and analyses that,
     * so walk-up discovery from the scratch path finds no build.tur -- and a
     * module inside a spice came back with every `(import sibling)`
     * unresolved, no symbol index at all, and therefore no completion, no
     * go-to-definition, and a rename that refused because it could not see a
     * definition. `tur check` on the same file has always worked, because it
     * walks up from the real one. This is the difference.
     *
     * NULL means the path IS the real one (the MCP server, the stdlib prime
     * on an empty scratch file), which is the pre-existing behaviour. */
    const char *anchor = (logical_path && *logical_path) ? logical_path : path;

    char **inc = NULL;
    int    n_inc = 0;
    char **owned = NULL;
    int    n_owned = 0;
    Ls2ResolverCtx ls2;
    /* script-spice-deps-plan SD5: an editor keystroke resolves a script
     * header from the cache only -- it never fetches, prints or exits. */
    bool prev_editor = g_script_deps_editor;
    g_script_deps_editor = true;
    /* The editor honors a header whatever the flag says: it only reads the
     * cache, and a script whose `#!` line carries --enable=script-deps would
     * otherwise show TUR-E0633 in the editor and run fine. */
    bool prev_script_deps = g_opt_script_deps;
    g_opt_script_deps = true;
    auto_append_spice_includes(anchor, &inc, &n_inc, &owned, &n_owned, &ls2);
    g_script_deps_editor = prev_editor;

    Buf discard;
    buf_init(&discard);
    int rm_n = 0;
    char **rm_p = discover_manifest_reader_macros(anchor, &rm_n);
    ls2_resolver_ctx_set(&ls2);
    /* lsp-sibling-import-resolves-against-scratch-dir: spice includes were
     * already anchored on the real path (above); the entry file's own
     * directory has to be too. */
    g_logical_entry_path = (logical_path && *logical_path) ? logical_path : NULL;
    int rc = compile_to_c(path, &discard, (const char **)inc, n_inc,
                          (const char **)rm_p, rm_n);
    g_logical_entry_path = NULL;
    ls2_resolver_ctx_set(NULL);
    ls2_resolver_ctx_dispose(&ls2);
    free_reader_macro_paths(rm_p, rm_n);
    buf_free(&discard);

    for (int i = 0; i < n_owned; i++) free(owned[i]);
    free(owned);
    free(inc);
    lsp_collect_end();
    g_opt_script_deps = prev_script_deps;
    return rc;
}

/* Run type-check only on `path`; discard generated C.
 * Used by the LSP server. Must be called with diag_lsp_begin active.
 *
 * SC6: applies the same SC4+SC5 auto-discovery that `tur check` uses,
 * so editor diagnostics in spice files don't show bogus "module not
 * found" errors for intra-spice and cross-spice imports.  --no-auto-spice
 * is honored if the user passed it at the top level. */
int tur_check_only(const char *path) {
    /* LSP/MCP diagnostics run experiment-hot: enable every registered
     * in-flight feature (kind annotations, constrained forall, HKT rank-2,
     * dictionary passing, curried rank-2 results, ...) so the editor sees
     * the same "this parses" answer a developer would get with the matching
     * --enable= flags on the CLI.  The TUR-W006x lifecycle warnings write to
     * stderr, not the JSON diagnostics stream, so they never surface as
     * editor diagnostics. */
    for (size_t xi = 0, xn = experiment_count(); xi < xn; xi++) {
        const ExperimentDescriptor *d = experiment_at(xi);
        if (d) experiment_enable(d->name, XF_SRC_CLI);
    }
    /* UC-3: everything is already on at XF_SRC_CLI above, so the user-level
     * experiments file would be a redundant no-op -- and a malformed one
     * must not crash an editor keystroke.  Mark it consulted so the later
     * discover_manifest_reader_macros call skips the read entirely (and the
     * source column stays 'cli'). */
    g_user_config_experiments_done = true;

    char **inc = NULL;
    int    n_inc = 0;
    char **owned = NULL;
    int    n_owned = 0;
    Ls2ResolverCtx ls2;
    bool prev_editor = g_script_deps_editor;
    g_script_deps_editor = true;   /* see tur_collect_symbols */
    auto_append_spice_includes(path, &inc, &n_inc, &owned, &n_owned, &ls2);
    g_script_deps_editor = prev_editor;

    Buf discard;
    buf_init(&discard);
    int rm_n = 0;
    char **rm_p = discover_manifest_reader_macros(path, &rm_n);
    ls2_resolver_ctx_set(&ls2);
    int rc = compile_to_c(path, &discard, (const char **)inc, n_inc,
                          (const char **)rm_p, rm_n);
    ls2_resolver_ctx_set(NULL);
    ls2_resolver_ctx_dispose(&ls2);
    free_reader_macro_paths(rm_p, rm_n);
    buf_free(&discard);

    for (int i = 0; i < n_owned; i++) free(owned[i]);
    free(owned);
    free(inc);
    return rc;
}
