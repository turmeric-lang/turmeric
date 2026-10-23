#!/bin/sh
# wasm-jit-plan P1 pre-spike: the MIR interpreter under Emscripten, measured.
#
# Builds the vendored MIR (external/mir) for wasm32 with a tiny driver that
# compiles C through c2mir and interprets it, then measures recursion depth
# and the per-call cost of mir-wasm.c's JS trampoline.  Also builds sjlj.c,
# which shows a longjmp issued through that trampoline unwinding back to a
# direct setjmp in a compiled frame (the host-owned landing of plan 5.6).
#
# Needs emcc (emsdk) and node on PATH.  Never edits external/mir: the one
# patch it needs (a NULL hard-register name that _MIR_init strlen()s) is
# applied to a scratch copy under build/ until the fork carries the fix.
set -e
cd "$(dirname "$0")"
ROOT=../..
rm -rf build && mkdir -p build && cp -r "$ROOT/external/mir" build/mir
sed -i.bak 's/target_hard_reg_names\[\] = {NULL}/target_hard_reg_names[] = {"wasm_hr0"}/' build/mir/mir-wasm.c
M=build/mir
COMMON="-I$M -I$M/c2mir spike.c $M/mir.c $M/c2mir/c2mir.c --embed-file hdr@/hdr \
  -sMODULARIZE -sEXPORT_NAME=Spike -sALLOW_TABLE_GROWTH -sALLOW_MEMORY_GROWTH \
  -sINITIAL_MEMORY=128mb -sSTACK_SIZE=16mb \
  -sEXPORTED_RUNTIME_METHODS=wasmTable,UTF8ToString,addFunction,stackAlloc,stackSave,stackRestore,ccall,cwrap \
  -sEXPORTED_FUNCTIONS=_spike_compile,_spike_call,_malloc,_free"
emcc -O2 $COMMON -o build/spike-opt.js
emcc -O1 -g -sASSERTIONS=2 -sSAFE_HEAP=1 $COMMON -o build/spike-dbg.js
emcc -O2 sjlj.c -o build/sjlj.js -sEXPORTED_RUNTIME_METHODS=wasmTable
echo "== sjlj: longjmp through the JS trampoline back to a direct setjmp"
node build/sjlj.js
echo "== mini (assertions build): small calls"
node mini.js build/spike-dbg.js 2>&1 | grep -v "no end of line"
echo "== drive (-O2): depth and per-iteration cost"
node drive.js 2>&1 | grep -v "no end of line"
