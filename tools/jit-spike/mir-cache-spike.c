/* mir-cache-spike.c -- performance-audit-plan E-2: would a cached MIR image of
 * the unit `tur jit` compiles beat re-running c2mir on it every run?
 *
 * Usage: mir-cache-spike <unit.c> <out.bmir> [-I dir ...]
 *
 * <unit.c> is the text `tur jit` hands the engine (TUR_JIT_DUMP_C=<path>).  The
 * engine prepends its builtin prelude before c2mir sees it; this harness does
 * the same from jit-prelude.h, which mir-cache-spike.sh extracts from
 * src/jit_engine.c on every run so the two cannot drift.  Best of 5 each:
 *
 *   c2mir   MIR_init + c2mir_compile of prelude + unit (what every run pays)
 *   write   MIR_write of the resulting modules (once, when a cache is filled)
 *   read    MIR_init + MIR_read of the image (what a cache hit would pay)
 *   ready   ... + MIR_load_module + MIR_gen_init + MIR_link (lazy gen), i.e.
 *           the point where `tur jit` starts the program, on both paths
 *
 * Nothing is run.  Imports the harness process cannot resolve (the Turmeric
 * runtime lives in `tur`, not here) link to a dummy address, which is enough
 * for MIR_link under lazy generation: no function body is generated.
 *
 * A one-off measurement, not a tool; see wp4-engines.md for what it found. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dlfcn.h>
#include "mir.h"
#include "mir-gen.h"
#include "c2mir.h"
#include "jit-prelude.h"

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static const char *g_src;
static size_t g_len, g_pos;
static int getc_fn(void *d) { (void)d; return g_pos < g_len ? (unsigned char)g_src[g_pos++] : EOF; }

static char *slurp(const char *p, size_t *n) {
  FILE *f = fopen(p, "rb");
  if (!f) { perror(p); exit(2); }
  fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
  char *b = malloc((size_t)sz + 1);
  if (fread(b, 1, (size_t)sz, f) != (size_t)sz) { perror("read"); exit(2); }
  b[sz] = 0; fclose(f); *n = (size_t)sz; return b;
}

static void dummy_import(void) {}
static void *resolve(const char *name) {
  void *p = dlsym(RTLD_DEFAULT, name);
  return p ? p : (void *)dummy_import;
}

static void load_and_link(MIR_context_t ctx) {
  for (MIR_module_t m = DLIST_HEAD(MIR_module_t, *MIR_get_module_list(ctx)); m;
       m = DLIST_NEXT(MIR_module_t, m))
    MIR_load_module(ctx, m);
  MIR_gen_init(ctx);
  MIR_gen_set_optimize_level(ctx, 2);
  MIR_link(ctx, MIR_set_lazy_gen_interface, resolve);
}

static size_t count_funcs(MIR_context_t ctx) {
  size_t n = 0;
  for (MIR_module_t m = DLIST_HEAD(MIR_module_t, *MIR_get_module_list(ctx)); m;
       m = DLIST_NEXT(MIR_module_t, m))
    for (MIR_item_t it = DLIST_HEAD(MIR_item_t, m->items); it; it = DLIST_NEXT(MIR_item_t, it))
      if (it->item_type == MIR_func_item) n++;
  return n;
}

int main(int argc, char **argv) {
  if (argc < 3) { fprintf(stderr, "usage: %s unit.c out.bmir [-I dir ...]\n", argv[0]); return 2; }
  size_t ulen; char *unit = slurp(argv[1], &ulen);
  size_t plen = sizeof JIT_PRELUDE - 1;
  char *full = malloc(plen + ulen + 1);
  memcpy(full, JIT_PRELUDE, plen); memcpy(full + plen, unit, ulen); full[plen + ulen] = 0;
  const char *incs[16]; int ninc = 0;
  for (int i = 3; i + 1 < argc && ninc < 16; i++)
    if (strcmp(argv[i], "-I") == 0) incs[ninc++] = argv[++i];

  double best_c = 1e9, best_w = 1e9, best_r = 1e9, best_rr = 1e9, best_cr = 1e9;
  double first_cr = 0, first_rr = 0;   /* rep 0: the cold-process number */
  long bmir_size = 0;
  size_t nfuncs = 0;
  for (int rep = 0; rep < 5; rep++) {
    /* today: c2mir from text, then load + link */
    double t0 = now_ms();
    MIR_context_t ctx = MIR_init();
    c2mir_init(ctx);
    struct c2mir_options ops; memset(&ops, 0, sizeof ops);
    ops.message_file = stderr; ops.ignore_warnings_p = 1;
    ops.include_dirs_num = (size_t)ninc; ops.include_dirs = incs;
    g_src = full; g_len = plen + ulen; g_pos = 0;
    if (!c2mir_compile(ctx, &ops, getc_fn, NULL, "<spike>", NULL)) { fprintf(stderr, "c2mir failed\n"); return 1; }
    double t1 = now_ms();
    FILE *w = fopen(argv[2], "wb"); MIR_write(ctx, w); bmir_size = ftell(w); fclose(w);
    double t2 = now_ms();
    nfuncs = count_funcs(ctx);
    double t2b = now_ms();
    load_and_link(ctx);
    double t3 = now_ms();
    MIR_gen_finish(ctx);
    c2mir_finish(ctx);
    MIR_finish(ctx);
    if (rep == 0) first_cr = (t1 - t0) + (t3 - t2b);
    if (t1 - t0 < best_c) best_c = t1 - t0;
    if (t2 - t1 < best_w) best_w = t2 - t1;
    if ((t1 - t0) + (t3 - t2b) < best_cr) best_cr = (t1 - t0) + (t3 - t2b);

    /* a cache hit: read the image, then load + link */
    double r0 = now_ms();
    MIR_context_t c2 = MIR_init();
    FILE *r = fopen(argv[2], "rb"); MIR_read(c2, r); fclose(r);
    double r1 = now_ms();
    load_and_link(c2);
    double r2 = now_ms();
    MIR_gen_finish(c2);
    MIR_finish(c2);
    if (rep == 0) first_rr = r2 - r0;
    if (r1 - r0 < best_r) best_r = r1 - r0;
    if (r2 - r0 < best_rr) best_rr = r2 - r0;
  }
  printf("unit\t%zu\tprelude\t%zu\tbmir\t%ld\tfuncs\t%zu\n", ulen, plen, bmir_size, nfuncs);
  printf("c2mir_ms\t%.2f\twrite_ms\t%.2f\tread_ms\t%.2f\n", best_c, best_w, best_r);
  printf("ready_c2mir_ms\t%.2f\tready_read_ms\t%.2f\tspeedup\t%.1f\n",
         best_cr, best_rr, best_cr / best_rr);
  printf("first_ready_c2mir_ms\t%.2f\tfirst_ready_read_ms\t%.2f\n", first_cr, first_rr);
  return 0;
}
