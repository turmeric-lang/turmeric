#include <emscripten.h>
#include <stdio.h>
#include <string.h>
#include "mir.h"
#include "c2mir.h"
typedef struct { const char *s; size_t i; } rd_t;
static int rd_getc (void *d) { rd_t *r = d; return r->s[r->i] ? r->s[r->i++] : EOF; }
static MIR_context_t ctx;
long long native_add (long long a, long long b) { return a + b; }
EMSCRIPTEN_KEEPALIVE int spike_compile (const char *src) {
  if (ctx) MIR_finish (ctx);
  ctx = MIR_init ();
  c2mir_init (ctx);
  struct c2mir_options ops; memset (&ops, 0, sizeof ops);
  const char *dirs[] = {"/hdr"};
  ops.include_dirs = dirs; ops.include_dirs_num = 1; ops.message_file = stderr;
  rd_t r = {src, 0};
  if (!c2mir_compile (ctx, &ops, rd_getc, &r, "<spike>", NULL)) return -1;
  for (MIR_module_t m = DLIST_HEAD (MIR_module_t, *MIR_get_module_list (ctx)); m; m = DLIST_NEXT (MIR_module_t, m))
    MIR_load_module (ctx, m);
  MIR_load_external (ctx, "printf", (void *) printf);
  MIR_load_external (ctx, "native_add", (void *) native_add);
  MIR_link (ctx, MIR_set_interp_interface, NULL);
  return 0;
}
EMSCRIPTEN_KEEPALIVE int spike_call (const char *fn, int arg) {
  for (MIR_module_t m = DLIST_HEAD (MIR_module_t, *MIR_get_module_list (ctx)); m; m = DLIST_NEXT (MIR_module_t, m))
    for (MIR_item_t it = DLIST_HEAD (MIR_item_t, m->items); it; it = DLIST_NEXT (MIR_item_t, it))
      if (it->item_type == MIR_func_item && strcmp (it->u.func->name, fn) == 0) {
        MIR_val_t res, a; a.i = arg;
        MIR_interp_arr (ctx, it, &res, 1, &a);
        return (int) res.i;
      }
  return -999;
}
