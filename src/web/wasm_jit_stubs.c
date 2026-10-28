/* wasm_jit_stubs.c -- definitions the browser JIT module links but must never
 * reach (wasm-jit-plan P3).  Each fails loudly rather than returning
 * something plausible.
 *
 * MIR's generator.  WebAssembly cannot generate code at run time, so mir-gen.c
 * is not linked and the engine is forced into interpreter mode on this target
 * (src/jit_engine.c).  mir-wasm.c stubs most of the generator's surface, but
 * not these three, which jit_engine.c and turi/jit_ffi.c still name on paths
 * interpreter mode never takes.  They belong in mir-wasm.c, and the fork now
 * defines them there (turmeric-lang/mir, wasm-jit-plan section 6); until the
 * vendored copy carries that, these are weak, so a build against either tree
 * links and the fork's definitions win.  Delete them with the sync.
 *
 * ucontext.  The resident runtime's fiber code calls getcontext/makecontext/
 * swapcontext, which Emscripten declares and does not implement.  The stock
 * module never references them; this one does, because the import table takes
 * the address of the whole runtime.  Continuations are a fallback family on
 * this engine (plan 5.6).  This TU includes the system <ucontext.h> and no
 * turmeric header: turi/env.h carries a ucontext_t stand-in of its own for the
 * interpreter's fibers, and the two conflict.
 */
#include <stdio.h>
#include <stdlib.h>
#include <ucontext.h>

#include "mir.h"
#include "mir-gen.h"

static void MIR_NO_RETURN unreachable(const char *what) {
    fprintf(stderr, "tur: jit: %s is not available in the browser engine\n", what);
    abort();
}

__attribute__((weak)) void MIR_gen_init(MIR_context_t ctx) {
    (void)ctx;
    unreachable("MIR's code generator");
}

__attribute__((weak)) void MIR_gen_set_optimize_level(MIR_context_t ctx, unsigned int level) {
    (void)ctx;
    (void)level;
    unreachable("MIR's code generator");
}

__attribute__((weak)) void *MIR_gen(MIR_context_t ctx, MIR_item_t func_item) {
    (void)ctx;
    (void)func_item;
    unreachable("MIR's code generator");
}

int getcontext(ucontext_t *u) {
    (void)u;
    unreachable("getcontext (fibers)");
}

void makecontext(ucontext_t *u, void (*fn)(void), int argc, ...) {
    (void)u;
    (void)fn;
    (void)argc;
    unreachable("makecontext (fibers)");
}

int swapcontext(ucontext_t *a, const ucontext_t *b) {
    (void)a;
    (void)b;
    unreachable("swapcontext (fibers)");
}
