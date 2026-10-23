/* Mimic the host-owned landing: a compiled frame does a DIRECT setjmp, then
   calls "interpreted" code through the JS trampoline mir-wasm.c uses; that
   code longjmps -- also through the trampoline, as an interpreted program
   would.  Does the unwind come back to the direct landing? */
#include <emscripten.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdint.h>
static jmp_buf jb;
static int depth_log[8]; static int nlog;
EM_JS(void, js_call0, (uint32_t fp), { wasmTable.get(fp)(); });
EM_JS(void, js_call2, (uint32_t fp, uint32_t a, int b), { wasmTable.get(fp)(a, b); });
/* longjmp reached by address through JS, the way interpreted code reaches any native */
static void (*volatile lj)(jmp_buf, int) = longjmp;
static void body(void) {
  depth_log[nlog++] = 1;
  js_call2((uint32_t)(uintptr_t)lj, (uint32_t)(uintptr_t)jb, 7);   /* never returns */
  depth_log[nlog++] = 99;
}
static int try_run(void (*b)(void)) {
  int r = setjmp(jb);                 /* direct call: LLVM can lower it */
  if (r == 0) { js_call0((uint32_t)(uintptr_t)b); return 0; }
  return r;
}
int main(void) {
  int r = try_run(body);
  printf("try_run -> %d (expect 7); body ran %d step(s), after-longjmp code ran: %s\n",
         r, nlog, nlog == 2 ? "YES (bug)" : "no");
  /* twice, to show the landing is reusable */
  nlog = 0; r = try_run(body);
  printf("second -> %d\n", r);
  return 0;
}
