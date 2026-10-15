#define _GNU_SOURCE
#include <dlfcn.h>
#include <signal.h>
#include <stdio.h>
#include <ucontext.h>

static long n_swap, n_get, n_mask;

int swapcontext(ucontext_t *o, const ucontext_t *n) {
  static int (*real)(ucontext_t *, const ucontext_t *);
  if (!real) real = dlsym(RTLD_NEXT, "swapcontext");
  n_swap++;
  return real(o, n);
}
int getcontext(ucontext_t *u) {
  static int (*real)(ucontext_t *);
  if (!real) real = dlsym(RTLD_NEXT, "getcontext");
  n_get++;
  return real(u);
}
int sigprocmask(int how, const sigset_t *s, sigset_t *o) {
  static int (*real)(int, const sigset_t *, sigset_t *);
  if (!real) real = dlsym(RTLD_NEXT, "sigprocmask");
  n_mask++;
  return real(how, s, o);
}
__attribute__((destructor)) static void report(void) {
  fprintf(stderr, "ctxcount swapcontext=%ld getcontext=%ld sigprocmask=%ld\n",
          n_swap, n_get, n_mask);
}
