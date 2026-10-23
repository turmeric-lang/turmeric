#!/bin/bash
# wasm-jit-plan P1: link jit_engine.c (and MIR, and the driver's front half)
# into a copy of the tur_wasm link, with a generated symbol table.
#
# Needs: a configured build-wasm/ (cmake -DTUR_WASM=ON; its tur_wasm command
# line is the source list and the flags this copies), emcc on PATH, python3.
# Everything lands under build/; nothing in the tree is touched.
set -e
cd "$(dirname "$0")"
ROOT=$(cd ../../.. && pwd)
EMSDK=${EMSDK:-/home/user/emsdk}
SYSROOT=$EMSDK/upstream/emscripten/cache/sysroot
NM=$EMSDK/upstream/bin/llvm-nm
B=$PWD/build
MK=$ROOT/build-wasm/src/CMakeFiles/tur_wasm.dir/build.make
[ -f "$MK" ] || { echo "configure build-wasm first: cmake -S $ROOT -B $ROOT/build-wasm -DTUR_WASM=ON" >&2; exit 2; }
mkdir -p "$B/obj" "$B/hdr"

# --- MIR: a scratch copy with the fork fixes this spike found (never edit
# external/mir by hand; these ride the next fork PR, wasm-jit-plan section 6):
#   - a NULL hard-register name that _MIR_init strlen()s;
#   - the wasm32 target's ptrdiff_t/intptr_t/__PTRDIFF_TYPE__ are `int` where
#     clang's wasm32 (and therefore musl's headers, which derive _Addr from
#     __PTRDIFF_TYPE__) use `long`, so size_t came out as two different types.
rm -rf "$B/mir" && cp -r "$ROOT/external/mir" "$B/mir"
sed -i 's/target_hard_reg_names\[\] = {NULL}/target_hard_reg_names[] = {"wasm_hr0"}/' "$B/mir/mir-wasm.c"
sed -i 's/#define __PTRDIFF_TYPE__ int/#define __PTRDIFF_TYPE__ long/' "$B/mir/c2mir/wasm32/mirc_wasm32_emscripten.h"
sed -i 's/typedef int intptr_t;/typedef long intptr_t;/; s/typedef unsigned int uintptr_t;/typedef unsigned long uintptr_t;/' "$B/mir/c2mir/wasm32/mirc_wasm32_stdint.h"
sed -i 's/typedef int ptrdiff_t;/typedef long ptrdiff_t;/' "$B/mir/c2mir/wasm32/mirc_wasm32_stddef.h"
#   - the predefined-macro set lacks the compiler-provided type macros musl's
#     bits/alltypes.h typedefs from (__WCHAR_TYPE__, the _FAST/_LEAST family):
#     `typedef __WCHAR_TYPE__ wchar_t;` then fails and every later declaration
#     naming wchar_t derails the parser.  Take clang's own values.
echo | emcc -dM -E - > "$B/clang-macros.txt"
python3 - "$B/mir/c2mir/wasm32/mirc_wasm32_emscripten.h" "$B/clang-macros.txt" <<'PY'
import re, sys
h, m = sys.argv[1:3]
src = open(h).read()
have = set(re.findall(r'"#define (__[A-Z0-9_]+_TYPE__) ', src))
add = [(k, v) for k, v in re.findall(r'^#define (__[A-Z0-9_]+_TYPE__) (.*)$', open(m).read(), re.M) if k not in have]
anchor = '    "#define __UINTMAX_TYPE__ unsigned long long\\n"\n'
assert anchor in src, 'anchor line moved'
src = src.replace(anchor, anchor + ''.join('    "#define %s %s\\n"\n' % (k, v) for k, v in add))
open(h, 'w').write(src)
print('type macros added from clang: %d' % len(add))
PY
MIR=$B/mir

# --- sources: the stock tur_wasm list (relative to src/) ...
CMD=$(grep -o "emcc .*" "$MK" | head -1)
# web/wasm_lsp.c defines its own tur_collect_symbols / tur_check_only because
# the stock module has no driver; this one has, so leave the LSP shim out.
STOCK=$(printf '%s' "$CMD" | tr ' ' '\n' | grep '\.c$' | grep -v 'web/wasm_lsp.c')
# ... plus the driver (the front half lives there), the engine, MIR, the glue.
DRIVER="driver/cc.c driver/cli.c driver/cmd_build.c driver/cmd_emit.c driver/cmd_eval.c driver/cmd_format.c  driver/cmd_x.c driver/compile_link.c driver/jit.c driver/paths.c driver/pipeline.c driver/preamble_split.c driver/project.c driver/spice.c  lsp/mcp.c"
ENGINE="jit_engine.c turi/jit_ffi.c"
INCS=$(printf '%s' "$CMD" | tr ' ' '\n' | grep '^-I' | tr '\n' ' ')
VERSION=$(tr -d ' \n' < "$ROOT/VERSION")
# Flags go through response files: xargs would otherwise strip the quotes
# and reflow them.
printf '%s\n' $INCS -I$ROOT/external/mir -I$ROOT/external/mir/c2mir -I$ROOT/src/driver -O2 -std=c11 -D_DEFAULT_SOURCE -DTUR_HAVE_JIT=1 "-DTUR_VERSION=\\\"$VERSION\\\"" -pthread -w > "$B/cflags.rsp"
printf '%s\n' -I$MIR -I$MIR/c2mir -O2 -std=gnu11 -fsigned-char -w -pthread > "$B/mirflags.rsp"
CFLAGS="@$B/cflags.rsp"
MIRFLAGS="@$B/mirflags.rsp"

# --- compile, one object per source, in parallel
: > "$B/jobs.txt"
for s in $STOCK $DRIVER $ENGINE; do
  case $s in /*) src=$s;; *) src=$ROOT/src/$s;; esac
  obj=$B/obj/$(printf '%s' "$s" | tr '/' '_' | sed 's/\.c$/.o/')
  [ "$obj" -nt "$src" ] || printf '%s\t%s\t%s\n' "$src" "$obj" "$CFLAGS" >> "$B/jobs.txt"
done
for s in mir.c c2mir/c2mir.c; do
  src=$MIR/$s; obj=$B/obj/mir_$(basename "$s" .c).o
  printf '%s\t%s\t%s\n' "$src" "$obj" "$MIRFLAGS" >> "$B/jobs.txt"
done
printf '%s\t%s\t%s\n' "$PWD/wasm_jit_glue.c" "$B/obj/wasm_jit_glue.o" "$CFLAGS" >> "$B/jobs.txt"
printf '%s\t%s\t%s\n' "$PWD/ucontext_stubs.c" "$B/obj/ucontext_stubs.o" "$MIRFLAGS" >> "$B/jobs.txt"
echo "compiling $(wc -l < "$B/jobs.txt") translation unit(s)"
# shellcheck disable=SC2016
xargs -P 4 -L 1 sh -c 'emcc -c "$0" -o "$1" "$2" || echo "FAILED: $0" >&2' < "$B/jobs.txt" 2>&1 | grep -v "^$" || true
for obj in $(awk -F'\t' '{print $2}' "$B/jobs.txt"); do [ -f "$obj" ] || { echo "missing $obj" >&2; exit 1; }; done

# --- the symbol table: what the objects define, intersected with the decls
"$NM" --defined-only --extern-only "$B"/obj/*.o > "$B/nm.txt"
# Names the hosted declarations turn into macros (thread-locals routed
# through tur_tls_* accessors) are not symbols; ask the preprocessor.
printf '#define TUR_RT_SPLIT_HOSTED 1\n#include "runtime/generated/tur_rt_split_decls.h"\n' > "$B/macro-probe.c"
emcc -E -dM "$B/macro-probe.c" $CFLAGS | awk '{print $2}' | sed 's/(.*//' | sort -u > "$B/macros.txt"
# A defined symbol the region only names inside a macro or a comment (the
# tur_tls_* thread-local objects) has no declaration the table can take the
# address of: let the compiler name them, drop them, retry.
cp "$B/macros.txt" "$B/exclude.txt"
for attempt in 1 2 3 4 5 6; do
  python3 gen-symtab.py "$ROOT/src/runtime/generated/tur_rt_split_decls.h" "$B/nm.txt" "$B/tur_wasm_jit_symtab.c" "$B/exclude.txt"
  if emcc -c "$B/tur_wasm_jit_symtab.c" -o "$B/obj/tur_wasm_jit_symtab.o" $CFLAGS 2> "$B/symtab-cc.log"; then break; fi
  grep -o "use of undeclared identifier '[A-Za-z_0-9]*'" "$B/symtab-cc.log" | grep -o "'.*'" | tr -d "'" >> "$B/exclude.txt"
  [ "$attempt" = 6 ] && { cat "$B/symtab-cc.log" >&2; exit 1; }
done

# --- headers c2mir will read from MEMFS: the SDK's src/ tree and the sysroot
(cd "$ROOT/src" && tar cf - $(find . -name '*.h')) | tar xf - -C "$B/hdr"

# --- link: the stock tur_wasm flags, plus what the engine needs
emcc "$B"/obj/*.o -o "$B/turmeric-jit.js" \
  --embed-file "$ROOT/stdlib@/stdlib" \
  --embed-file "$SYSROOT/include@/sysinclude" \
  --embed-file "$B/hdr@/src" \
  -sMODULARIZE=1 -sEXPORT_NAME=TurmericModule -sALLOW_MEMORY_GROWTH=1 -sALLOW_TABLE_GROWTH=1 \
  -pthread -sPTHREAD_POOL_SIZE_STRICT=0 -sEXIT_RUNTIME=0 -sSTACK_SIZE=16777216 \
  -sDEFAULT_PTHREAD_STACK_SIZE=8388608 -sINITIAL_MEMORY=134217728 \
  -sEXPORTED_FUNCTIONS=_turi_wasm_init,_turi_wasm_eval,_turi_wasm_jit_run,_turi_wasm_jit_c,_malloc,_free \
  -sEXPORTED_RUNTIME_METHODS=stringToUTF8,UTF8ToString,lengthBytesUTF8,wasmTable,addFunction,stackAlloc,stackSave,stackRestore,ccall,cwrap,ENV,FS \
  -O2 2>&1 | grep -v "^$" | grep -v "Wpthreads-mem-growth" || true
ls -la "$B/turmeric-jit.wasm"
