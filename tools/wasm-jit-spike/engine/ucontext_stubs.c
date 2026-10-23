/* The resident runtime's fiber code calls ucontext, which Emscripten declares
 * but does not implement; the stock module never references it, this one
 * does through the symbol table.  Continuations are a fallback family on
 * this engine (wasm-jit-plan 5.6), so these only have to fail loudly.  Own
 * TU: the turmeric headers carry their own ucontext_t stub for the
 * interpreter's fibers, which conflicts with the system one. */
#include <stdio.h>
#include <stdlib.h>
#include <ucontext.h>
static void nope (void) { fputs ("tur-wasm-jit: fibers are not available on this engine\n", stderr); abort (); }
int getcontext (ucontext_t *u) { (void) u; nope (); return -1; }
void makecontext (ucontext_t *u, void (*fn) (void), int argc, ...) { (void) u; (void) fn; (void) argc; nope (); }
int swapcontext (ucontext_t *a, const ucontext_t *b) { (void) a; (void) b; nope (); return -1; }
