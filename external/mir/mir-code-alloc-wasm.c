/* This file is a part of MIR project.
   Copyright (C) 2018-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.

   wasm32 / Emscripten code allocator.

   WebAssembly has no writable-executable memory: code cannot be generated
   at runtime, so MIR runs in interpreter mode only and never needs a code
   region.  These entry points exist so mir.c links; every one of them
   fails, which is correct -- nothing should be asking for code memory. */

#include <stdlib.h>
#include "mir-code-alloc.h"

static void *wasm_mem_map (size_t len, void *user_data) {
  (void) len;
  (void) user_data;
  return MAP_FAILED;
}

static int wasm_mem_unmap (void *addr, size_t len, void *user_data) {
  (void) addr;
  (void) len;
  (void) user_data;
  return -1;
}

static int wasm_mem_protect (void *addr, size_t len, MIR_mem_protect_t prot, void *user_data) {
  (void) addr;
  (void) len;
  (void) prot;
  (void) user_data;
  return -1;
}

static struct MIR_code_alloc default_code_alloc
  = {.mem_map = wasm_mem_map,
     .mem_unmap = wasm_mem_unmap,
     .mem_protect = wasm_mem_protect,
     .user_data = NULL};
