/* wasm_jit_glue.c -- wasm-jit-plan P1: `tur jit`'s front half and engine
 * entry, callable from the page.
 *
 * Mirrors cmd_jit (src/driver/jit.c) minus the command line: emit C for the
 * program (compile_to_c), hoist includes, scan autolink markers, swap the
 * fixed preamble for the resident runtime's declarations (S2 split), prune,
 * and hand the text to tur_jit_execute, which on this target runs the MIR
 * interpreter on the calling thread and resolves imports through the
 * generated symbol table (tur_wasm_jit_symtab.c).
 *
 * The generator stubs at the bottom exist because mir-gen.c is not linked on
 * wasm (mir-wasm.c stubs the rest of MIR's generator surface, not these). */
#include "driver.h"
#include "commands.h"
#include "jit_engine.h"
#include "compiler/jit_prune.h"
#include "compiler/refine_discharge.h"
#include "mir.h"
#include "mir-gen.h"
#include "runtime/globals.h" /* g_interpret_mode: turi_env_new sets it, the emitter must not see it */
#include <emscripten.h>
#include <sys/stat.h>

/* flags: bit 0 = emit as `tur build --target wasm` does (rc/GC runtime
 * replicated into the TU rather than taken from the host); bit 1 = skip the
 * split; bit 2 = skip the prune; bit 3 = dump the TU to /tmp/jit-tu.c. */
EMSCRIPTEN_KEEPALIVE
int turi_wasm_jit_run (const char *src, const char *name, int flags) {
    mkdir ("/tmp", 0777);
    const char *path = "/tmp/jit-prog.tur";
    FILE *f = fopen (path, "wb");
    if (!f) return 101;
    fputs (src, f);
    fclose (f);

    refine_discharge_reset ();
    /* As repl_jit_build does (src/driver/jit.c): the module's turi_env_new
     * left g_interpret_mode on, and under it the emitter selects the
     * interpreter's branches and evicts the CPS backend's handlers. */
    bool saved_interp = g_interpret_mode;
    g_interpret_mode = false;
    g_emit_for_link = true;
    g_emit_for_wasm = (flags & 1) != 0;
    Buf csrc;
    buf_init (&csrc);
    int rc = compile_to_c (path, &csrc, NULL, 0, NULL, 0);
    g_interpret_mode = saved_interp;
    if (rc != 0) { buf_free (&csrc); return 100 + rc; }
    hoist_tur_include_directives (&csrc);
    Buf autolink;
    buf_init (&autolink);
    if (!scan_autolink_markers (&csrc, &autolink)) { buf_free (&csrc); buf_free (&autolink); return 102; }

    Buf split_src;
    buf_init (&split_src);
    bool split_used = (flags & 2) ? false : jit_try_split_preamble (&csrc, &split_src);
    bool reduced_used = split_used;
    JitPruneStats ps;
    memset (&ps, 0, sizeof ps);
    bool pruned = false;
    if (!(flags & 4)) {
        if (split_used) {
            pruned = jit_prune_split_source (&split_src, &ps);
        } else {
            buf_write (&split_src, csrc.data, csrc.len);
            pruned = jit_prune_full_source (&split_src, &ps);
            if (pruned) reduced_used = true;
            else { buf_free (&split_src); buf_init (&split_src); }
        }
    }
    const Buf *use = reduced_used ? &split_src : &csrc;
    fprintf (stderr, "tur-wasm-jit: split=%d pruned=%d bytes=%zu -> %zu (%u of %u defs, %u includes dropped)\n",
             split_used, pruned, csrc.len, use->len, ps.nodes_dropped, ps.nodes, ps.includes_dropped);
    if (flags & 8) buf_to_path (use, "/tmp/jit-tu.c");

    static const char *incs[] = { "/sysinclude", "/src", "/src/runtime" };
    char *argv[] = { (char *) name, NULL };
    int prog_rc = 0;
    int jrc = tur_jit_execute (use->data, use->len, autolink.len ? autolink.data : NULL,
                               incs, 3, 1, argv, &prog_rc);
    if ((jrc == TUR_JIT_ERR_COMPILE || jrc == TUR_JIT_ERR_LINK) && !(flags & 16)) {
        if (reduced_used) {
            fprintf (stderr, "tur-wasm-jit: TUR-W0071 retry with the full TU\n");
            jrc = tur_jit_execute (csrc.data, csrc.len, autolink.len ? autolink.data : NULL,
                                   incs, 3, 1, argv, &prog_rc);
        }
    }
    fprintf (stderr, "tur-wasm-jit: jrc=%d prog_rc=%d\n", jrc, prog_rc);
    buf_free (&csrc); buf_free (&split_src); buf_free (&autolink);
    return jrc == TUR_JIT_OK ? prog_rc : 200 + jrc;
}

/* Raw C straight into the engine (no front half): for bisecting a c2mir
 * diagnostic against the exact text, from the page. */
EMSCRIPTEN_KEEPALIVE
int turi_wasm_jit_c (const char *csrc, int run) {
    static const char *incs[] = { "/sysinclude", "/src", "/src/runtime" };
    char *argv[] = { "c", NULL };
    int prog_rc = 0;
    int jrc = tur_jit_execute (csrc, strlen (csrc), NULL, incs, 3, 1, argv, &prog_rc);
    (void) run;
    return jrc == TUR_JIT_OK ? prog_rc : 200 + jrc;
}

/* mir-gen.c is not linked on this target; the engine references these from
 * its lazy-generation interface, which interp mode never installs. */
void MIR_gen_init (MIR_context_t ctx) { (void) ctx; }
void MIR_gen_set_optimize_level (MIR_context_t ctx, unsigned int level) { (void) ctx; (void) level; }
void *MIR_gen (MIR_context_t ctx, MIR_item_t func_item) { (void) ctx; (void) func_item; return NULL; }
