/* wasm_jit_stubs.c -- definitions the browser JIT module links but must never
 * reach (wasm-jit-plan P3).  Each fails loudly rather than returning
 * something plausible.  (MIR's generator entry points used to be here too;
 * mir-wasm.c defines them since turmeric-lang/mir#16.)
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

static _Noreturn void unreachable(const char *what) {
    fprintf(stderr, "tur: jit: %s is not available in the browser engine\n", what);
    abort();
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
