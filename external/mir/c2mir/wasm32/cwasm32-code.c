/* This file is a part of MIR project.
   Copyright (C) 2020-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
*/

#include "../mirc.h"
#include "mirc_wasm32_emscripten.h"

#include "mirc_wasm32_float.h"
#include "mirc_wasm32_limits.h"
#include "mirc_wasm32_stdarg.h"
#include "mirc_wasm32_stdint.h"
#include "mirc_wasm32_stddef.h"

/* Only compiler-provided headers, as on every other target.
 *
 * This target used to also ship minimal stdio/stdlib/string/math here, because
 * a browser has no filesystem to read the real ones from.  That put the
 * declarations in the compiler and the matching symbol table in the embedder --
 * two different repositories -- and they drifted: the headers ended up
 * declaring roughly twice as many functions as the embedder actually supplied,
 * so calls to the rest compiled cleanly and then failed at link.
 *
 * An embedder needing libc declarations should serve them through
 * c2mir_options.include_dirs instead (Emscripten's MEMFS is readable by the
 * ordinary fopen-based include search) and generate them from the same list it
 * registers with MIR_load_external.  Note that standard_includes is consulted
 * *before* system_header_dirs, so anything named here would shadow the
 * embedder's copy. */
static string_include_t standard_includes[]
  = {{NULL, mirc}, {NULL, wasm32_mirc}, TARGET_STD_INCLUDES};

#define MAX_ALIGNMENT 8

#define ADJUST_VAR_ALIGNMENT(c2m_ctx, align, type) \
  wasm32_adjust_var_alignment (c2m_ctx, align, type)

static int wasm32_adjust_var_alignment (c2m_ctx_t c2m_ctx MIR_UNUSED, int align,
                                        struct type *type MIR_UNUSED) {
  return align;
}

static int invalid_alignment (mir_llong align) {
  return align != 0 && align != 1 && align != 2 && align != 4 && align != 8;
}
