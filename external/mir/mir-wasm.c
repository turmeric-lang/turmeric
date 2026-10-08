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

   Build requirements:
     -sWASM_BIGINT                              (i64 <-> BigInt)
     -sEXPORTED_RUNTIME_METHODS=wasmTable,UTF8ToString
*/

#include "mir.h"

#include <emscripten.h>
#include <stdint.h>
#include <string.h>

#define MIR_DIRECT_DISPATCH 0
#define MAX_HARD_REG 0

static const char *const target_hard_reg_names[] = {NULL};

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

/* Every MIR function gets an address from _MIR_get_thunk, and MIR_link then
   points that address at an interpreter shim (via MIR_set_interp_interface)
   or at undefined_interface.  On a native target a thunk is a few bytes of
   patchable machine code and the redirect rewrites a jump; wasm cannot
   generate code, so a thunk here is a slot in a table and the redirect
   rewrites the slot.

   Calls out of interpreted code all funnel through wasm_ff_dispatch, which
   recognises a thunk address and re-enters the interpreter directly rather
   than trying to call it as a wasm function.

   This is load-bearing for ordinary recursion, not just for exotic cases.
   While _MIR_get_thunk returned one shared do-nothing stub for every
   function and _MIR_redirect_thunk was inert, *every* call from interpreted
   code to another interpreted function invoked that empty stub and silently
   produced 0.  Shallow calls appeared to work only because MIR's inliner had
   already replaced them; the bug surfaced as "recursion returns a wrong
   answer past ~15 frames", which is simply where the inliner gave up. */

#define WASM_MAX_THUNKS 8192

typedef enum {
  WASM_THUNK_UNSET,
  WASM_THUNK_NATIVE, /* redirected at a real wasm function pointer */
  WASM_THUNK_INTERP, /* redirected at an interpreted MIR function */
} wasm_thunk_kind_t;

typedef struct {
  wasm_thunk_kind_t kind;
  void *target;        /* WASM_THUNK_NATIVE */
  MIR_context_t ctx;   /* WASM_THUNK_INTERP */
  MIR_item_t func_item;
  uint32_t fn_ptr;     /* lazily created wasm table entry; 0 = none yet */
} wasm_thunk_t;

static wasm_thunk_t wasm_thunks[WASM_MAX_THUNKS];
static size_t wasm_thunks_num;

static wasm_thunk_t *wasm_thunk_alloc (MIR_context_t ctx) {
  wasm_thunk_t *t;

  if (wasm_thunks_num >= WASM_MAX_THUNKS) {
    MIR_get_error_func (ctx) (MIR_alloc_error, "wasm32: too many function thunks");
    return NULL;
  }
  t = &wasm_thunks[wasm_thunks_num++];
  t->kind = WASM_THUNK_UNSET;
  t->target = NULL;
  t->ctx = ctx; /* kept even for a native redirect, so errors have a context */
  t->func_item = NULL;
  t->fn_ptr = 0;
  return t;
}

/* NULL unless ADDR is one of ours.  Any other pointer -- a libc function, a
   symbol from MIR_load_external -- has to fall through to the JS call. */
static wasm_thunk_t *wasm_thunk_of (void *addr) {
  size_t off;

  if ((char *) addr < (char *) wasm_thunks
      || (char *) addr >= (char *) (wasm_thunks + WASM_MAX_THUNKS))
    return NULL;
  off = (size_t) ((char *) addr - (char *) wasm_thunks);
  if (off % sizeof (wasm_thunk_t) != 0) return NULL; /* interior pointer: not a thunk */
  return (wasm_thunk_t *) addr;
}

/* ---------------------------------------------------------------- */
/* Interpreted functions as real wasm function pointers              */
/* ---------------------------------------------------------------- */

/* A thunk is a table slot, which is all interpreted->interpreted calls need,
   but it is not something a *native* callee can call: qsort does a genuine
   call_indirect on whatever pointer it was handed.  So when an interpreted
   function's address is about to cross into native code, give it a real entry
   in the wasm table that trampolines back into the interpreter.

   Created on demand and cached, because most functions never escape and each
   entry costs a table slot and a JS closure. */

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
   wasm_ff_dispatch lays one out: results first, then arguments. */
EMSCRIPTEN_KEEPALIVE void mir_wasm_interp_entry (uint32_t slot, MIR_val_t *ras) {
  wasm_thunk_t *t;
  MIR_func_t func;

  if (slot >= wasm_thunks_num) return;
  t = &wasm_thunks[slot];
  if (t->kind != WASM_THUNK_INTERP) return;
  func = t->func_item->u.func;
  MIR_interp_arr (t->ctx, t->func_item, ras, func->nargs, ras + func->nres);
}

/* WSIG is the wasm signature addFunction needs (result first).  MSIG says how
   to widen each argument.  STRIDE is sizeof (MIR_val_t), which is *not* 8: the
   union carries a host long double, and Emscripten's is 128-bit, so the slots
   are 16 bytes apart.  Getting that wrong reads every argument one slot low.

   The buffer is stack-allocated per call, so a comparator invoked repeatedly --
   or recursively -- never shares it. */
EM_JS (int, mir_wasm_add_interp_entry,
       (uint32_t slot, const char *wsig, const char *msig, uint32_t nres, uint32_t stride), {
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
             _mir_wasm_interp_entry (slot, buf);
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
         return addFunction (f, ws);
       });

static uint32_t wasm_thunk_fn_ptr (wasm_thunk_t *t) {
  char wsig[WASM_FF_MAX_ARGS + 2], msig[WASM_FF_MAX_ARGS + 1];
  MIR_func_t func;
  MIR_var_t *vars;
  size_t i, n;

  if (t->fn_ptr != 0) return t->fn_ptr;
  func = t->func_item->u.func;
  n = func->nargs;
  if (func->vararg_p || func->nres > 1 || n > WASM_FF_MAX_ARGS) {
    MIR_get_error_func (t->ctx) (MIR_call_op_error,
                                 "wasm32: cannot pass %s to native code as a function pointer",
                                 func->name);
    return 0;
  }
  wsig[0] = func->nres == 0 ? WASM_TC_VOID : wasm_type_code (func->res_types[0]);
  vars = VARR_ADDR (MIR_var_t, func->vars);
  for (i = 0; i < n; i++) {
    wsig[i + 1] = wasm_type_code (vars[i].type);
    msig[i] = wasm_marshal_code (vars[i].type);
  }
  wsig[n + 1] = msig[n] = '\0';
  t->fn_ptr = (uint32_t) mir_wasm_add_interp_entry ((uint32_t) (t - wasm_thunks), wsig, msig,
                                                    (uint32_t) func->nres,
                                                    (uint32_t) sizeof (MIR_val_t));
  return t->fn_ptr;
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
      /* An interpreted function's address crossing into native code: hand over
         a real table entry instead of the thunk slot, which is not callable.
         wasm32 is ILP32, so a pointer and an int are the same wasm type and a
         plain integer that happens to equal a live thunk's address would be
         substituted too -- it has to be slot-aligned, in range, and an
         interpreted thunk, which makes that vanishingly unlikely. */
      wasm_thunk_t *arg_thunk = wasm_thunk_of ((void *) (uintptr_t) v);

      if (arg_thunk != NULL && arg_thunk->kind == WASM_THUNK_INTERP)
        v = wasm_thunk_fn_ptr (arg_thunk);
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
   against the thunk table above, which is what makes interpreted code able to
   call interpreted code.

   _MIR_get_wrapper and the lazy-generation hooks remain inert: those exist for
   the code generator, which this target does not have.  Handing a *native*
   callee a pointer to an interpreted function (qsort's comparator, say) is
   still unsupported -- a thunk is a table slot, not a wasm function pointer,
   so it cannot be called through wasmTable.  That needs Emscripten's
   addFunction to append a real table entry at runtime. */

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

/* A thunk is a table slot rather than patchable code; see "Function thunks"
   above.  The pointer handed back is therefore not a callable wasm function
   pointer -- it is only ever meaningful to wasm_ff_dispatch, which is where
   every call out of interpreted code goes. */
void *_MIR_get_thunk (MIR_context_t ctx) { return wasm_thunk_alloc (ctx); }

void *_MIR_get_thunk_addr (MIR_context_t ctx MIR_UNUSED, void *thunk) {
  wasm_thunk_t *t = wasm_thunk_of (thunk);

  if (t == NULL) return thunk;
  return t->kind == WASM_THUNK_NATIVE ? t->target : thunk;
}

void _MIR_redirect_thunk (MIR_context_t ctx MIR_UNUSED, void *thunk, void *to) {
  wasm_thunk_t *t = wasm_thunk_of (thunk), *src = wasm_thunk_of (to);

  if (t == NULL) return;
  if (src != NULL) { /* pointing one thunk at another, i.e. at an interp shim */
    *t = *src;
  } else {
    t->kind = WASM_THUNK_NATIVE;
    t->target = to;
  }
}

/* HANDLER is mir-interp.c's static `interp`, which reads its arguments out of
   a va_list a native shim would have marshalled.  There is no code to
   generate here, and wasm_ff_dispatch already holds the arguments as
   MIR_val_t, so it calls the equivalent MIR_interp_arr directly and the
   handler is not needed. */
void *_MIR_get_interp_shim (MIR_context_t ctx, MIR_item_t func_item, void *handler MIR_UNUSED) {
  wasm_thunk_t *t = wasm_thunk_alloc (ctx);

  if (t == NULL) return NULL;
  t->kind = WASM_THUNK_INTERP;
  t->ctx = ctx;
  t->func_item = func_item;
  return t;
}

static void wrapper_func (void) {}
void *_MIR_get_wrapper (MIR_context_t ctx MIR_UNUSED, MIR_item_t called_func MIR_UNUSED,
                        void *hook_address MIR_UNUSED) {
  return (void *) wrapper_func;
}
static char wrapper_end_buf[16];
void *_MIR_get_wrapper_end (MIR_context_t ctx MIR_UNUSED) { return wrapper_end_buf; }

/* Code generator entry points referenced by c2mir-driver.c.  There is no
   generator on this target; interpreter mode only. */
void MIR_gen_set_debug_file (MIR_context_t ctx MIR_UNUSED, const char *file MIR_UNUSED) {}
void MIR_gen_set_debug_level (MIR_context_t ctx MIR_UNUSED, int level MIR_UNUSED) {}
void MIR_set_gen_interface (MIR_context_t ctx MIR_UNUSED, void *gen_interface MIR_UNUSED) {}
void MIR_set_lazy_bb_gen_interface (MIR_context_t ctx MIR_UNUSED, void *i MIR_UNUSED) {}
void MIR_set_lazy_gen_interface (MIR_context_t ctx MIR_UNUSED, void *i MIR_UNUSED) {}
void MIR_gen_finish (MIR_context_t ctx MIR_UNUSED) {}
