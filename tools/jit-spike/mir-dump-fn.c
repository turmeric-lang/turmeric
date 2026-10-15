/* mir-dump-fn.c -- performance-audit-plan E-3: what does MIR make of one
 * function of a `tur jit` unit?
 *
 * Usage: mir-dump-fn <unit.c> <function> [-I dir ...]
 *
 * <unit.c> is a TUR_JIT_DUMP_C dump, compiled exactly as the engine compiles it
 * (jit-prelude.h first; mir-cache-spike.sh extracts it).  Prints the function's
 * MIR as c2mir produced it, then generates that one function at the engine's
 * optimize level (2) with MIR-gen's level-2 debug trace on stdout: every pass,
 * ending with "MIR after forming prolog/epilog and insn splitting", which is
 * the post-register-allocation code that becomes machine code nearly 1:1.
 * Nothing is run; imports the harness cannot resolve link to a dummy. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include "mir.h"
#include "mir-gen.h"
#include "c2mir.h"
#include "jit-prelude.h"

static const char *g_src;
static size_t g_len, g_pos;
static int getc_fn(void *d) { (void)d; return g_pos < g_len ? (unsigned char)g_src[g_pos++] : EOF; }

static void dummy_import(void) {}
static void *resolve(const char *name) {
  void *p = dlsym(RTLD_DEFAULT, name);
  return p ? p : (void *)dummy_import;
}

int main(int argc, char **argv) {
  if (argc < 3) { fprintf(stderr, "usage: %s unit.c function [-I dir ...]\n", argv[0]); return 2; }
  FILE *f = fopen(argv[1], "rb");
  if (!f) { perror(argv[1]); return 2; }
  fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
  size_t plen = sizeof JIT_PRELUDE - 1;
  char *full = malloc(plen + (size_t)sz + 1);
  memcpy(full, JIT_PRELUDE, plen);
  if (fread(full + plen, 1, (size_t)sz, f) != (size_t)sz) { perror("read"); return 2; }
  fclose(f);
  full[plen + (size_t)sz] = 0;
  const char *incs[16]; int ninc = 0;
  for (int i = 3; i + 1 < argc && ninc < 16; i++)
    if (strcmp(argv[i], "-I") == 0) incs[ninc++] = argv[++i];

  MIR_context_t ctx = MIR_init();
  c2mir_init(ctx);
  struct c2mir_options ops; memset(&ops, 0, sizeof ops);
  ops.message_file = stderr; ops.ignore_warnings_p = 1;
  ops.include_dirs_num = (size_t)ninc; ops.include_dirs = incs;
  g_src = full; g_len = plen + (size_t)sz; g_pos = 0;
  if (!c2mir_compile(ctx, &ops, getc_fn, NULL, "<tur-jit>", NULL)) { fprintf(stderr, "c2mir failed\n"); return 1; }

  MIR_item_t target = NULL;
  for (MIR_module_t m = DLIST_HEAD(MIR_module_t, *MIR_get_module_list(ctx)); m; m = DLIST_NEXT(MIR_module_t, m))
    for (MIR_item_t it = DLIST_HEAD(MIR_item_t, m->items); it; it = DLIST_NEXT(MIR_item_t, it))
      if (it->item_type == MIR_func_item && strcmp(it->u.func->name, argv[2]) == 0) target = it;
  if (!target) { fprintf(stderr, "no function %s in the unit\n", argv[2]); return 1; }
  printf("==== c2mir output for %s\n", argv[2]);
  MIR_output_item(ctx, stdout, target);

  for (MIR_module_t m = DLIST_HEAD(MIR_module_t, *MIR_get_module_list(ctx)); m; m = DLIST_NEXT(MIR_module_t, m))
    MIR_load_module(ctx, m);
  MIR_gen_init(ctx);
  MIR_gen_set_optimize_level(ctx, 2);
  MIR_link(ctx, MIR_set_lazy_gen_interface, resolve);
  printf("==== MIR-gen trace for %s (optimize level 2)\n", argv[2]);
  fflush(stdout);
  MIR_gen_set_debug_file(ctx, stdout);
  MIR_gen_set_debug_level(ctx, 2);
  MIR_gen(ctx, target);
  fflush(stdout);
  MIR_gen_finish(ctx);
  c2mir_finish(ctx);
  MIR_finish(ctx);
  return 0;
}
