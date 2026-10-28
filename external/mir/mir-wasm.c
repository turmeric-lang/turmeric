/* This file is a part of MIR project.
   Copyright (C) 2020-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.

   wasm32 / Emscripten "target" for MIR.

   WebAssembly cannot generate machine code at runtime, so the JIT
   generator is unavailable and MIR can only run in interpreter mode.
   Everything here exists to satisfy the interpreter, and the one piece
   that carries real weight is _MIR_get_ff_call: mir-interp.c routes
   every call from interpreted code into a native function through it
   (see get_ff_interface).  A stubbed-out version means printf and all
   of libc silently do nothing.

   The implementation trampolines through JavaScript.  A wasm function
   pointer is an index into the wasm table, and wasmTable.get(ptr) yields
   a JS-callable function; JS can then spread an argument list of
   arbitrary length and type.  That sidesteps the fact that a wasm
   call_indirect is strictly type-checked and would otherwise require one
   pre-generated C stub per signature (~4^nargs of them).

   A MIR function's address is a wasm table entry as well (see "Function
   thunks"), so native code can call an interpreted function through a
   pointer however it got the pointer.

   Build requirements:
     -sALLOW_TABLE_GROWTH                       (one table entry per function)
     -sWASM_BIGINT on an older Emscripten       (i64 <-> BigInt; the default now)
   The JS runtime pieces used here are pulled in with EM_JS_DEPS, so nothing
   has to be listed in EXPORTED_RUNTIME_METHODS.
*/

#include "mir.h"

#include <emscripten.h>
#include <stdint.h>
#include <string.h>

#define MIR_DIRECT_DISPATCH 0
#define MAX_HARD_REG 0

/* wasm has no hard registers, but MAX_HARD_REG 0 still declares one, and
   hard_reg_name_init hashes every name with strlen.  A NULL entry reads as ""
   only because address 0 is mapped in a plain wasm module; under -sSAFE_HEAP
   it is a segfault in MIR_init.  Nothing can bind to this name: the type
   check below refuses every type. */
static const char *const target_hard_reg_names[] = {"wasm_hr0"};

static inline int target_hard_reg_type_ok_p (MIR_reg_t hard_reg MIR_UNUSED,
                                             MIR_type_t type MIR_UNUSED) {
  return 0;
}

static inline int target_fixed_hard_reg_p (MIR_reg_t hard_reg MIR_UNUSED) { return 0; }

static inline int target_locs_num (MIR_reg_t loc MIR_UNUSED, MIR_type_t type MIR_UNUSED) {
  return 1;
}

/* ---------------------------------------------------------------- */
/* Foreign function calls                                            */
/* ---------------------------------------------------------------- */

#define WASM_FF_MAX_SIGS 100 /* interned one per distinct signature */
#define WASM_FF_MAX_ARGS 32
#define WASM_FF_VA_BUF_SIZE 512

/* Type codes handed to the JS side. */
#define WASM_TC_I32 'i'
#define WASM_TC_I64 'j'
#define WASM_TC_F32 'f'
#define WASM_TC_F64 'd'
#define WASM_TC_VOID 'v'

typedef struct {
  int used;
  int vararg_p;
  size_t nres;
  size_t nargs;   /* total, including the variadic tail */
  size_t nfixed;  /* number of declared (non-variadic) parameters */
  char res_code;  /* WASM_TC_*, or WASM_TC_VOID */
  MIR_type_t arg_types[WASM_FF_MAX_ARGS];
} wasm_ff_sig_t;

static wasm_ff_sig_t wasm_ff_sigs[WASM_FF_MAX_SIGS];
static int wasm_ff_sigs_num = 0;

/* Map a MIR type onto the wasm value type it is passed as.  wasm32 is
   ILP32, so pointers are i32. */
static char wasm_type_code (MIR_type_t t) {
  switch (t) {
  case MIR_T_I8:
  case MIR_T_U8:
  case MIR_T_I16:
  case MIR_T_U16:
  case MIR_T_I32:
  case MIR_T_U32:
  case MIR_T_P: return WASM_TC_I32;
  case MIR_T_I64:
  case MIR_T_U64: return WASM_TC_I64;
  case MIR_T_F: return WASM_TC_F32;
  case MIR_T_D:
  case MIR_T_LD: return WASM_TC_F64; /* long double is double on this target */
  default: return WASM_TC_I32;       /* blocks are passed by address */
  }
}

/* Call wasmTable[fp] with nargs arguments read out of SLOTS (one 8-byte
   slot each, tagged by SIG), storing any result into RETBUF. */
EM_JS (void, mir_wasm_js_call,
       (uint32_t fp, const char *sig, char res_code, uint8_t *slots, uint8_t *retbuf), {
         var s = UTF8ToString (sig);
         var f = wasmTable.get (fp);
         if (!f) throw new Error ("MIR: bad function pointer " + fp);
         var args = [];
         for (var i = 0; i < s.length; i++) {
           var off = slots + i * 8;
           switch (s.charCodeAt (i)) {
           case 105: args.push (HEAP32[off >> 2]); break;               /* 'i' */
           case 106: args.push (HEAP64[off >> 3]); break;               /* 'j' */
           case 102: args.push (HEAPF32[off >> 2]); break;              /* 'f' */
           case 100: args.push (HEAPF64[off >> 3]); break;              /* 'd' */
           default: throw new Error ("MIR: bad type code " + s[i]);
           }
         }
         var r = f.apply (null, args);
         switch (res_code) {
         case 105: HEAP32[retbuf >> 2] = r; break;
         case 106: HEAP64[retbuf >> 3] = r; break;
         case 102: HEAPF32[retbuf >> 2] = r; break;
         case 100: HEAPF64[retbuf >> 3] = r; break;
         default: break; /* void */
         }
       });

/* ---------------------------------------------------------------- */
/* Function thunks                                                   */
/* ---------------------------------------------------------------- */

/* Every MIR function gets an address from _MIR_get_thunk when its module is
   loaded, and MIR_link later points that address at an interpreter shim (via
   MIR_set_interp_interface) or at undefined_interface.  On a native target a
   thunk is a few bytes of patchable machine code and the redirect rewrites a
   jump.  wasm cannot generate code, so here a thunk is an entry in the wasm
   function table: its address IS the table index, reserved at load, and the
   redirect fills the entry once the target -- and so the signature -- is
   known.

   That makes a thunk address an ordinary wasm function pointer, callable by
   anything: interpreted code, and also native code that does a genuine
   call_indirect on it -- qsort's comparator, a callback the host runtime
   stored in a struct, a function value passed as a 64-bit integer.  An
   earlier version made a thunk a slot in a C array, callable only through
   wasm_ff_dispatch, and substituted a real table entry when the address was
   passed as a 32-bit argument of a native call.  Every other way the address
   reached native code -- stored in memory first, or passed as an i64 --
   trapped with "table index is out of bounds".

   Calls out of interpreted code still funnel through wasm_ff_dispatch, which
   recognises a thunk address and re-enters the interpreter directly instead
   of going through the table and JavaScript.

   This is load-bearing for ordinary recursion, not just for exotic cases.
   While _MIR_get_thunk returned one shared do-nothing stub for every
   function and _MIR_redirect_thunk was inert, *every* call from interpreted
   code to another interpreted function invoked that empty stub and silently
   produced 0.  Shallow calls appeared to work only because MIR's inliner had
   already replaced them; the bug surfaced as "recursion returns a wrong
   answer past ~15 frames", which is simply where the inliner gave up.

   Thunks belong to their context: MIR_finish releases them (wasm_context_finish,
   called from code_finish), empties their table entries and reuses them, so a
   page that runs many programs in one module does not run out. */

#define WASM_MAX_THUNKS 8192

typedef enum {
  WASM_THUNK_FREE,   /* released with its context; the table entry is null */
  WASM_THUNK_UNSET,  /* reserved, not yet redirected */
  WASM_THUNK_NATIVE, /* redirected at a real wasm function pointer */
  WASM_THUNK_INTERP, /* redirected at an interpreted MIR function */
} wasm_thunk_kind_t;

typedef struct {
  wasm_thunk_kind_t kind;
  uint32_t index;     /* the thunk's wasm table index, which is its address */
  void *target;       /* WASM_THUNK_NATIVE */
  MIR_context_t ctx;  /* the owner; kept for a native redirect too, so errors have one */
  MIR_item_t func_item; /* WASM_THUNK_INTERP */
} wasm_thunk_t;

/* In ascending table index, always: a new thunk takes the next index the
   table grows to, and a reused one keeps its own.  So an address is found by
   binary search. */
static wasm_thunk_t wasm_thunks[WASM_MAX_THUNKS];
static size_t wasm_thunks_num;
static uint32_t wasm_thunks_free[WASM_MAX_THUNKS]; /* positions in wasm_thunks */
static size_t wasm_thunks_free_num;

/* What _MIR_get_interp_shim returns: never called, only handed to
   _MIR_redirect_thunk, so it needs no table entry. */
typedef struct {
  MIR_context_t ctx; /* NULL when free */
  MIR_item_t func_item;
} wasm_shim_t;

static wasm_shim_t wasm_shims[WASM_MAX_THUNKS];
static size_t wasm_shims_num;
static uint32_t wasm_shims_free[WASM_MAX_THUNKS];
static size_t wasm_shims_free_num;

EM_JS_DEPS (mir_wasm_thunks, "$wasmTable,$UTF8ToString,$stackSave,$stackAlloc,$stackRestore");

/* Grow the table by one null entry and return its index. */
EM_JS (uint32_t, mir_wasm_table_reserve, (void), { return wasmTable.grow (1); });

/* Make entry DST call what entry SRC calls; SRC 0 empties DST. */
EM_JS (void, mir_wasm_table_copy, (uint32_t dst, uint32_t src),
       { wasmTable.set (dst, src ? wasmTable.get (src) : null); });

static wasm_thunk_t *wasm_thunk_alloc (MIR_context_t ctx) {
  wasm_thunk_t *t;

  if (wasm_thunks_free_num > 0) {
    t = &wasm_thunks[wasm_thunks_free[--wasm_thunks_free_num]];
  } else if (wasm_thunks_num >= WASM_MAX_THUNKS) {
    MIR_get_error_func (ctx) (MIR_alloc_error, "wasm32: too many function thunks");
    return NULL;
  } else {
    uint32_t index = mir_wasm_table_reserve ();

    /* The table only grows (Emscripten's addFunction appends too, and reuses
       only what removeFunction freed), so indices arrive in ascending order. */
    if (wasm_thunks_num > 0 && index <= wasm_thunks[wasm_thunks_num - 1].index) {
      MIR_get_error_func (ctx) (MIR_alloc_error, "wasm32: function table shrank under MIR");
      return NULL;
    }
    t = &wasm_thunks[wasm_thunks_num++];
    t->index = index;
  }
  t->kind = WASM_THUNK_UNSET;
  t->target = NULL;
  t->ctx = ctx;
  t->func_item = NULL;
  return t;
}

/* NULL unless ADDR is one of ours.  Any other pointer -- a libc function, a
   symbol from MIR_load_external -- has to fall through to the JS call. */
static wasm_thunk_t *wasm_thunk_of (void *addr) {
  uint32_t index = (uint32_t) (uintptr_t) addr;
  size_t lo = 0, hi = wasm_thunks_num;

  if (wasm_thunks_num == 0 || index < wasm_thunks[0].index
      || index > wasm_thunks[wasm_thunks_num - 1].index)
    return NULL;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;

    if (wasm_thunks[mid].index == index) return &wasm_thunks[mid];
    if (wasm_thunks[mid].index < index)
      lo = mid + 1;
    else
      hi = mid;
  }
  return NULL;
}

static wasm_shim_t *wasm_shim_alloc (MIR_context_t ctx) {
  wasm_shim_t *s;

  if (wasm_shims_free_num > 0) {
    s = &wasm_shims[wasm_shims_free[--wasm_shims_free_num]];
  } else if (wasm_shims_num >= WASM_MAX_THUNKS) {
    MIR_get_error_func (ctx) (MIR_alloc_error, "wasm32: too many interpreter shims");
    return NULL;
  } else {
    s = &wasm_shims[wasm_shims_num++];
  }
  s->ctx = ctx;
  s->func_item = NULL;
  return s;
}

static wasm_shim_t *wasm_shim_of (void *addr) {
  size_t off;

  if ((char *) addr < (char *) wasm_shims || (char *) addr >= (char *) (wasm_shims + wasm_shims_num))
    return NULL;
  off = (size_t) ((char *) addr - (char *) wasm_shims);
  if (off % sizeof (wasm_shim_t) != 0) return NULL; /* interior pointer */
  return ((wasm_shim_t *) addr)->ctx == NULL ? NULL : (wasm_shim_t *) addr;
}

/* Called from code_finish: CTX's thunks and shims become reusable, and its
   table entries are emptied, so a stale function pointer into a finished
   context traps as a null call instead of running freed code. */
static void wasm_context_finish (MIR_context_t ctx) {
  for (size_t i = 0; i < wasm_thunks_num; i++) {
    wasm_thunk_t *t = &wasm_thunks[i];

    if (t->kind == WASM_THUNK_FREE || t->ctx != ctx) continue;
    mir_wasm_table_copy (t->index, 0);
    t->kind = WASM_THUNK_FREE;
    t->ctx = NULL;
    t->func_item = NULL;
    t->target = NULL;
    wasm_thunks_free[wasm_thunks_free_num++] = (uint32_t) i;
  }
  for (size_t i = 0; i < wasm_shims_num; i++) {
    if (wasm_shims[i].ctx != ctx) continue;
    wasm_shims[i].ctx = NULL;
    wasm_shims[i].func_item = NULL;
    wasm_shims_free[wasm_shims_free_num++] = (uint32_t) i;
  }
}

/* ---------------------------------------------------------------- */
/* An interpreted function's table entry                             */
/* ---------------------------------------------------------------- */

/* How an argument has to be widened into its 8-byte MIR_val_t slot.  The wasm
   signature cannot express this: it only says i32, while the interpreter reads
   a full 64-bit field and would see garbage in the high half. */
static char wasm_marshal_code (MIR_type_t t) {
  switch (t) {
  case MIR_T_I8:
  case MIR_T_I16:
  case MIR_T_I32: return 'i'; /* sign-extend */
  case MIR_T_U8:
  case MIR_T_U16:
  case MIR_T_U32:
  case MIR_T_P: return 'u'; /* zero-extend */
  case MIR_T_I64:
  case MIR_T_U64: return 'j';
  case MIR_T_F: return 'f';
  case MIR_T_D:
  case MIR_T_LD: return 'd';
  default: return 'u';
  }
}

/* Called from the JS trampoline below with a buffer laid out the way
   wasm_ff_dispatch lays one out: results first, then arguments.  POS is the
   thunk's position in wasm_thunks. */
EMSCRIPTEN_KEEPALIVE void mir_wasm_interp_entry (uint32_t pos, MIR_val_t *ras) {
  wasm_thunk_t *t;
  MIR_func_t func;

  if (pos >= wasm_thunks_num) return;
  t = &wasm_thunks[pos];
  if (t->kind != WASM_THUNK_INTERP) return;
  func = t->func_item->u.func;
  MIR_interp_arr (t->ctx, t->func_item, ras, func->nargs, ras + func->nres);
}

/* Fill table entry DST with a wasm function of signature WSIG (result first)
   that trampolines into the interpreter for the thunk at POS.  MSIG says how
   to widen each argument.  STRIDE is sizeof (MIR_val_t), which is *not* 8: the
   union carries a host long double, and Emscripten's is 128-bit, so the slots
   are 16 bytes apart.  Getting that wrong reads every argument one slot low.

   A table entry has to be a wasm function, so the JS closure is wrapped in a
   one-import module exporting it at WSIG -- what Emscripten's addFunction does,
   except that the compiled module is kept per signature and only instantiated
   per thunk, which costs a fraction of a compile.  Every function a program
   defines gets one at link time, so that difference is the link's.

   The buffer is stack-allocated per call, so a comparator invoked repeatedly --
   or recursively -- never shares it. */
EM_JS (void, mir_wasm_set_interp_entry,
       (uint32_t dst, uint32_t pos, const char *wsig, const char *msig, uint32_t nres,
        uint32_t stride),
       {
         var ws = UTF8ToString (wsig), ms = UTF8ToString (msig);
         var f = function () {
           var sp = stackSave ();
           try {
             var n = arguments.length, i, off;
             var buf = stackAlloc ((nres + n) * stride);
             for (i = 0; i < ((nres + n) * stride) >> 3; i++) HEAP64[(buf >> 3) + i] = 0n;
             for (i = 0; i < n; i++) {
               off = buf + (nres + i) * stride;
               switch (ms.charCodeAt (i)) {
               case 106: HEAP64[off >> 3] = arguments[i]; break;                /* j */
               case 102: HEAPF32[off >> 2] = arguments[i]; break;               /* f */
               case 100: HEAPF64[off >> 3] = arguments[i]; break;               /* d */
               case 117: HEAP64[off >> 3] = BigInt (arguments[i] >>> 0); break; /* u */
               default: HEAP64[off >> 3] = BigInt (arguments[i] | 0); break;    /* i */
               }
             }
             _mir_wasm_interp_entry (pos, buf);
             if (nres == 0) return;
             switch (ws.charCodeAt (0)) {
             case 106: return HEAP64[buf >> 3];
             case 102: return HEAPF32[buf >> 2];
             case 100: return HEAPF64[buf >> 3];
             default: return HEAP32[buf >> 2];
             }
           } finally {
             stackRestore (sp);
           }
         };
         var cache = Module["mirWasmEntryModules"] || (Module["mirWasmEntryModules"] = {});
         var mod = cache[ws];
         if (!mod) {
           var code = {105 : 0x7f, 106 : 0x7e, 102 : 0x7d, 100 : 0x7c}; /* i j f d */
           var params = [], results = [];
           for (var k = 1; k < ws.length; k++) params.push (code[ws.charCodeAt (k)]);
           if (ws.charCodeAt (0) != 118) results.push (code[ws.charCodeAt (0)]); /* 'v' */
           var type = [ 0x01, 0x60, params.length ].concat (params, [results.length], results);
           mod = cache[ws] = new WebAssembly.Module (Uint8Array.from ([].concat (
             [ 0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00 ], /* magic, version */
             [ 0x01, type.length ], type,                       /* type section: 1 entry */
             [ 0x02, 0x07, 0x01, 0x01, 0x65, 0x01, 0x66, 0x00, 0x00 ], /* import "e" "f" */
             [ 0x07, 0x05, 0x01, 0x01, 0x66, 0x00, 0x00 ]))); /* export "f" */
         }
         wasmTable.set (dst, new WebAssembly.Instance (mod, {e : {f : f}}).exports.f);
       });

/* Give an interpreted thunk its table entry.  A function the JS trampoline
   cannot express -- variadic, more than one result, too many arguments -- gets
   none: interpreted code still calls it (wasm_ff_dispatch never goes through
   the table), and native code calling it traps on a null entry. */
static void wasm_thunk_set_interp_entry (wasm_thunk_t *t) {
  char wsig[WASM_FF_MAX_ARGS + 2], msig[WASM_FF_MAX_ARGS + 1];
  MIR_func_t func = t->func_item->u.func;
  MIR_var_t *vars;
  size_t i, n = func->nargs;

  if (func->vararg_p || func->nres > 1 || n > WASM_FF_MAX_ARGS) {
    mir_wasm_table_copy (t->index, 0);
    return;
  }
  wsig[0] = func->nres == 0 ? WASM_TC_VOID : wasm_type_code (func->res_types[0]);
  vars = VARR_ADDR (MIR_var_t, func->vars);
  for (i = 0; i < n; i++) {
    wsig[i + 1] = wasm_type_code (vars[i].type);
    msig[i] = wasm_marshal_code (vars[i].type);
  }
  wsig[n + 1] = msig[n] = '\0';
  mir_wasm_set_interp_entry (t->index, (uint32_t) (t - wasm_thunks), wsig, msig,
                             (uint32_t) func->nres, (uint32_t) sizeof (MIR_val_t));
}

static void wasm_ff_dispatch (int sig_index, void *addr, void *res_and_args) {
  wasm_ff_sig_t *sig = &wasm_ff_sigs[sig_index];
  MIR_val_t *ras = (MIR_val_t *) res_and_args;
  MIR_val_t *args = ras + sig->nres;
  wasm_thunk_t *thunk = wasm_thunk_of (addr);
  uint8_t slots[WASM_FF_MAX_ARGS * 8];
  char codes[WASM_FF_MAX_ARGS + 1];
  uint8_t va_buf[WASM_FF_VA_BUF_SIZE];
  uint8_t retbuf[8];
  size_t i, ncall = 0, va_off = 0;

  if (thunk != NULL) {
    if (thunk->kind == WASM_THUNK_INTERP) {
      /* Straight back into the interpreter.  res_and_args is already in the
         MIR_val_t layout MIR_interp_arr wants, and it copies the arguments
         into the callee's frame before writing any result, so results and
         arguments sharing the buffer is safe. */
      /* c2mir lowers a variadic *call* into a buffer plus a non-variadic call
         (VA_BUF_TARGET_P), but the matching change on the definition side is
         not done: an interpreted variadic function still expects its va_list
         through the interpreter's own channel, and has no parameter to receive
         the buffer pointer the caller now passes. Calling a *native* variadic
         function -- the printf family -- works. */
      if (thunk->func_item->u.func->vararg_p) {
        MIR_get_error_func (thunk->ctx) (MIR_call_op_error,
                                         "wasm32: calling a variadic function defined in "
                                         "interpreted code is not implemented");
        return;
      }
      MIR_interp_arr (thunk->ctx, thunk->func_item, ras, sig->nargs, args);
      return;
    }
    if (thunk->kind == WASM_THUNK_UNSET || thunk->target == NULL) {
      MIR_get_error_func (thunk->ctx) (MIR_call_op_error,
                                       "wasm32: call of an unresolved function");
      return;
    }
    addr = thunk->target;
  }

  memset (slots, 0, sizeof (slots));
  memset (retbuf, 0, sizeof (retbuf));

  for (i = 0; i < sig->nargs && ncall < WASM_FF_MAX_ARGS; i++) {
    MIR_type_t t = sig->arg_types[i];
    uint8_t *slot;

    if (sig->vararg_p && i >= sig->nfixed) {
      /* Variadic tail goes into a buffer, not into registers.  Emscripten
         lowers f(fixed..., ...) to f(fixed..., void *va_buf).

         c2mir no longer reaches this path: its wasm32 target builds the
         buffer itself at the call site (VA_BUF_TARGET_P) and emits a
         non-variadic call, precisely because the widths are still known
         there and are not recoverable here -- mir-interp.c widens every
         variadic integer to MIR_T_I64 before we see it, so `long long`
         cannot be told from `int`.  What follows is the best guess for any
         other producer of vararg MIR calls: 4-byte integer slots, which is
         right for %d/%c/%s/%p and truncates a genuine long long. */
      if (wasm_type_code (t) == WASM_TC_F64 || wasm_type_code (t) == WASM_TC_F32) {
        double d = (t == MIR_T_F) ? (double) args[i].f : args[i].d;
        va_off = (va_off + 7) & ~(size_t) 7;
        if (va_off + 8 > sizeof (va_buf)) break;
        memcpy (va_buf + va_off, &d, 8);
        va_off += 8;
      } else {
        uint32_t v = (uint32_t) args[i].u;
        va_off = (va_off + 3) & ~(size_t) 3;
        if (va_off + 4 > sizeof (va_buf)) break;
        memcpy (va_buf + va_off, &v, 4);
        va_off += 4;
      }
      continue;
    }

    slot = slots + ncall * 8;
    codes[ncall] = wasm_type_code (t);
    switch (codes[ncall]) {
    case WASM_TC_I32: {
      uint32_t v = (uint32_t) args[i].u;
      memcpy (slot, &v, 4);
      break;
    }
    case WASM_TC_I64: {
      uint64_t v = args[i].u;
      memcpy (slot, &v, 8);
      break;
    }
    case WASM_TC_F32: {
      float v = args[i].f;
      memcpy (slot, &v, 4);
      break;
    }
    default: {
      double v = (t == MIR_T_LD) ? (double) args[i].ld : args[i].d;
      memcpy (slot, &v, 8);
      break;
    }
    }
    ncall++;
  }

  /* The variadic buffer is passed as one extra pointer argument. */
  if (sig->vararg_p && ncall < WASM_FF_MAX_ARGS) {
    uint32_t p = (uint32_t) (uintptr_t) va_buf;
    codes[ncall] = WASM_TC_I32;
    memcpy (slots + ncall * 8, &p, 4);
    ncall++;
  }
  codes[ncall] = '\0';

  mir_wasm_js_call ((uint32_t) (uintptr_t) addr, codes, sig->res_code, slots, retbuf);

  if (sig->nres > 0) {
    switch (sig->res_code) {
    case WASM_TC_I32: {
      int32_t v;
      memcpy (&v, retbuf, 4);
      ras[0].i = v;
      break;
    }
    case WASM_TC_I64: {
      int64_t v;
      memcpy (&v, retbuf, 8);
      ras[0].i = v;
      break;
    }
    case WASM_TC_F32: {
      float v;
      memcpy (&v, retbuf, 4);
      ras[0].f = v;
      break;
    }
    case WASM_TC_F64: {
      double v;
      memcpy (&v, retbuf, 8);
      ras[0].d = v;
      break;
    }
    default: break;
    }
  }
}

/* _MIR_get_ff_call must return a plain void (*) (void *, void *), so the
   signature has to be baked into the pointer itself.  Without codegen the
   only way is a pool of pre-generated stubs, each closing over its index. */
#define WASM_FF_STUB(R, C)                                            \
  static void wasm_ff_stub_##R##_##C (void *addr, void *res_and_args) { \
    wasm_ff_dispatch ((R) *10 + (C), addr, res_and_args);             \
  }
#define WASM_FF_STUB_ROW(R)                                                     \
  WASM_FF_STUB (R, 0) WASM_FF_STUB (R, 1) WASM_FF_STUB (R, 2) WASM_FF_STUB (R, 3) \
    WASM_FF_STUB (R, 4) WASM_FF_STUB (R, 5) WASM_FF_STUB (R, 6)                 \
      WASM_FF_STUB (R, 7) WASM_FF_STUB (R, 8) WASM_FF_STUB (R, 9)

WASM_FF_STUB_ROW (0)
WASM_FF_STUB_ROW (1)
WASM_FF_STUB_ROW (2)
WASM_FF_STUB_ROW (3)
WASM_FF_STUB_ROW (4)
WASM_FF_STUB_ROW (5)
WASM_FF_STUB_ROW (6)
WASM_FF_STUB_ROW (7)
WASM_FF_STUB_ROW (8)
WASM_FF_STUB_ROW (9)

#define WASM_FF_STUB_REF(R, C) wasm_ff_stub_##R##_##C,
#define WASM_FF_STUB_REF_ROW(R)                                                             \
  WASM_FF_STUB_REF (R, 0) WASM_FF_STUB_REF (R, 1) WASM_FF_STUB_REF (R, 2)                   \
    WASM_FF_STUB_REF (R, 3) WASM_FF_STUB_REF (R, 4) WASM_FF_STUB_REF (R, 5)                 \
      WASM_FF_STUB_REF (R, 6) WASM_FF_STUB_REF (R, 7) WASM_FF_STUB_REF (R, 8)               \
        WASM_FF_STUB_REF (R, 9)

static void (*const wasm_ff_stubs[WASM_FF_MAX_SIGS]) (void *, void *) = {
  WASM_FF_STUB_REF_ROW (0) WASM_FF_STUB_REF_ROW (1) WASM_FF_STUB_REF_ROW (2)
    WASM_FF_STUB_REF_ROW (3) WASM_FF_STUB_REF_ROW (4) WASM_FF_STUB_REF_ROW (5)
      WASM_FF_STUB_REF_ROW (6) WASM_FF_STUB_REF_ROW (7) WASM_FF_STUB_REF_ROW (8)
        WASM_FF_STUB_REF_ROW (9)};

void *_MIR_get_ff_call (MIR_context_t ctx, size_t nres, MIR_type_t *res_types, size_t nargs,
                        _MIR_arg_desc_t *arg_descs, size_t arg_vars_num) {
  wasm_ff_sig_t *sig;
  size_t i;

  if (nres > 1)
    MIR_get_error_func (ctx) (MIR_call_op_error,
                              "wasm32: multiple return values are not supported");
  if (nargs > WASM_FF_MAX_ARGS)
    MIR_get_error_func (ctx) (MIR_call_op_error, "wasm32: too many arguments in a foreign call");
  /* Interned: every context asks again for the signatures it calls through,
     so a page running one program after another in the same module used to
     exhaust the pool after a couple of dozen runs. */
  for (int k = 0; k < wasm_ff_sigs_num; k++) {
    sig = &wasm_ff_sigs[k];
    if (sig->nres != nres || sig->nargs != nargs || sig->nfixed != arg_vars_num
        || sig->res_code != (nres == 0 ? WASM_TC_VOID : wasm_type_code (res_types[0])))
      continue;
    for (i = 0; i < nargs && sig->arg_types[i] == arg_descs[i].type; i++)
      ;
    if (i == nargs) return (void *) wasm_ff_stubs[k];
  }
  if (wasm_ff_sigs_num >= WASM_FF_MAX_SIGS)
    MIR_get_error_func (ctx) (MIR_call_op_error,
                              "wasm32: too many distinct foreign call signatures");
  sig = &wasm_ff_sigs[wasm_ff_sigs_num];
  sig->used = 1;
  sig->nres = nres;
  sig->nargs = nargs;
  sig->nfixed = arg_vars_num;
  sig->vararg_p = nargs > arg_vars_num;
  sig->res_code = nres == 0 ? WASM_TC_VOID : wasm_type_code (res_types[0]);
  for (i = 0; i < nargs; i++) sig->arg_types[i] = arg_descs[i].type;

  return (void *) wasm_ff_stubs[wasm_ff_sigs_num++];
}

/* ---------------------------------------------------------------- */
/* Remaining interpreter hooks                                       */
/* ---------------------------------------------------------------- */

/* _MIR_get_thunk / _MIR_redirect_thunk / _MIR_get_interp_shim are implemented
   against the thunk entries above, which is what makes interpreted code able
   to call interpreted code, and native code able to call it too.

   _MIR_get_wrapper and the lazy-generation hooks remain inert: those exist for
   the code generator, which this target does not have. */

static void *bstart_func (void) {
  static char buf[16];
  return buf;
}
static void bend_func (void *p MIR_UNUSED) {}
void *_MIR_get_bstart_builtin (MIR_context_t ctx MIR_UNUSED) { return (void *) bstart_func; }
void *_MIR_get_bend_builtin (MIR_context_t ctx MIR_UNUSED) { return (void *) bend_func; }

void *va_arg_builtin (void *p, uint64_t t) {
  /* va_list is a pointer into the buffer built by wasm_ff_dispatch. */
  void **va = (void **) p;
  void *a = *va;

  switch ((MIR_type_t) t) {
  case MIR_T_I64:
  case MIR_T_U64:
  case MIR_T_D:
  case MIR_T_LD:
    a = (void *) (((uintptr_t) a + 7) & ~(uintptr_t) 7);
    *va = (char *) a + 8;
    break;
  default:
    a = (void *) (((uintptr_t) a + 3) & ~(uintptr_t) 3);
    *va = (char *) a + 4;
    break;
  }
  return a;
}

void va_block_arg_builtin (void *res, void *p, size_t s, uint64_t ncase MIR_UNUSED) {
  void **va = (void **) p;
  if (res != NULL) memcpy (res, *va, s);
  *va = (char *) *va + ((s + 3) & ~(size_t) 3);
}

void va_start_interp_builtin (MIR_context_t ctx MIR_UNUSED, void *p, void *a) {
  void **va = (void **) p;
  *va = a;
}

void va_end_interp_builtin (MIR_context_t ctx MIR_UNUSED, void *p MIR_UNUSED) {}

/* A thunk is a wasm table entry rather than patchable code; see "Function
   thunks" above.  Its address is the entry's index, a real function pointer
   once _MIR_redirect_thunk has filled it. */
void *_MIR_get_thunk (MIR_context_t ctx) {
  wasm_thunk_t *t = wasm_thunk_alloc (ctx);

  return t == NULL ? NULL : (void *) (uintptr_t) t->index;
}

void *_MIR_get_thunk_addr (MIR_context_t ctx MIR_UNUSED, void *thunk) {
  wasm_thunk_t *t = wasm_thunk_of (thunk);

  if (t == NULL) return thunk;
  return t->kind == WASM_THUNK_NATIVE ? t->target : thunk;
}

void _MIR_redirect_thunk (MIR_context_t ctx MIR_UNUSED, void *thunk, void *to) {
  wasm_thunk_t *t = wasm_thunk_of (thunk), *src;
  wasm_shim_t *shim;

  if (t == NULL) return;
  if ((shim = wasm_shim_of (to)) != NULL) { /* at an interpreted function */
    t->kind = WASM_THUNK_INTERP;
    t->target = NULL;
    t->ctx = shim->ctx;
    t->func_item = shim->func_item;
    wasm_thunk_set_interp_entry (t);
  } else if ((src = wasm_thunk_of (to)) != NULL) { /* at another thunk */
    t->kind = src->kind;
    t->target = src->target;
    t->ctx = src->ctx;
    t->func_item = src->func_item;
    mir_wasm_table_copy (t->index, src->index);
  } else { /* at a native function, e.g. undefined_interface */
    t->kind = WASM_THUNK_NATIVE;
    t->target = to;
    t->func_item = NULL;
    mir_wasm_table_copy (t->index, (uint32_t) (uintptr_t) to);
  }
}

/* HANDLER is mir-interp.c's static `interp`, which reads its arguments out of
   a va_list a native shim would have marshalled.  There is no code to
   generate here, and wasm_ff_dispatch already holds the arguments as
   MIR_val_t, so it calls the equivalent MIR_interp_arr directly and the
   handler is not needed. */
void *_MIR_get_interp_shim (MIR_context_t ctx, MIR_item_t func_item, void *handler MIR_UNUSED) {
  wasm_shim_t *s = wasm_shim_alloc (ctx);

  if (s == NULL) return NULL;
  s->func_item = func_item;
  return s;
}

static void wrapper_func (void) {}
void *_MIR_get_wrapper (MIR_context_t ctx MIR_UNUSED, MIR_item_t called_func MIR_UNUSED,
                        void *hook_address MIR_UNUSED) {
  return (void *) wrapper_func;
}
static char wrapper_end_buf[16];
void *_MIR_get_wrapper_end (MIR_context_t ctx MIR_UNUSED) { return wrapper_end_buf; }

/* Code generator entry points, which embedders reference on paths interpreter
   mode never takes (c2mir-driver.c among them).  There is no generator on this
   target; interpreter mode only.  The ones that would produce code report it
   through the error function rather than returning something plausible. */
void MIR_gen_init (MIR_context_t ctx MIR_UNUSED) {}
void MIR_gen_set_optimize_level (MIR_context_t ctx MIR_UNUSED, unsigned int level MIR_UNUSED) {}
void *MIR_gen (MIR_context_t ctx, MIR_item_t func_item MIR_UNUSED) {
  MIR_get_error_func (ctx) (MIR_call_op_error,
                            "wasm32: there is no code generator; use the interpreter");
  return NULL;
}
void MIR_gen_set_debug_file (MIR_context_t ctx MIR_UNUSED, const char *file MIR_UNUSED) {}
void MIR_gen_set_debug_level (MIR_context_t ctx MIR_UNUSED, int level MIR_UNUSED) {}
void MIR_set_gen_interface (MIR_context_t ctx MIR_UNUSED, void *gen_interface MIR_UNUSED) {}
void MIR_set_lazy_bb_gen_interface (MIR_context_t ctx MIR_UNUSED, void *i MIR_UNUSED) {}
void MIR_set_lazy_gen_interface (MIR_context_t ctx MIR_UNUSED, void *i MIR_UNUSED) {}
void MIR_gen_finish (MIR_context_t ctx MIR_UNUSED) {}
