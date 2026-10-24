/* jit_ffi.h -- JIT-scoped dynamic FFI via c2mir (jit-ffi-c2mir-plan).
 *
 * The interpreter is the one consumer that genuinely needs call signatures
 * materialized at runtime; the JIT is exactly where it already lives.  This
 * header is the insulation layer between the two: tur_core TUs (ffi_thunk.c,
 * eval.c) consult a PROVIDER installed at startup by a JIT build, and stay
 * MIR-free themselves -- eighteen targets consume tur_core's objects
 * (including the WASM build) and none of them may grow a MIR link line.
 *
 * The provider synthesizes tiny C thunks with c2mir for signatures described
 * by a shape string (section 2.1 of the plan):
 *
 *     sig := <ret-class> ':' <arg-class>*
 *
 *     'i'  64-bit integer class (:int, :int64, :uint64, :cstr, :ptr<T>)
 *     'b'/'B'  signed/unsigned  8-bit integer (:bool, :int8 / :uint8)
 *     'h'/'H'  signed/unsigned 16-bit integer (:int16 / :uint16)
 *     'w'/'W'  signed/unsigned 32-bit integer (:int32 / :uint32)
 *     'f'  double
 *     'F'  float32 (exact ABI class -- NOT widened at the call boundary)
 *     'v'  void (return position only)
 *     '{'  ... '}'  struct by value (F4, see below)
 *
 * The narrow codes exist for RETURN fidelity (scalar-width-fidelity): a C
 * callee returning `int` leaves the upper register bits unspecified, so
 * the thunk must declare the exact C type and let the C cast extend.  On
 * the argument side they are equivalent to 'i' on both supported ABIs.
 *
 * e.g. "i:ifi" = int64 f(int64, double, int64); "v:" = void f(void);
 * "{ff}:{ff}{ff}" = struct{double,double} f(struct{...}, struct{...}).
 *
 * F4 struct-by-value.  A '{' opens an aggregate whose members are spelled
 * with EXACT-WIDTH codes -- unlike the scalar vocabulary above, an
 * aggregate's layout and its ABI classification both depend on the precise
 * size and alignment of every member, so 'i' (a register class) is not
 * enough information to render one:
 *
 *     'b'  1-byte integer  (:bool, :int8, :uint8)
 *     'h'  2-byte integer  (:int16, :uint16)
 *     'w'  4-byte integer  (:int32, :uint32)
 *     'q'  8-byte integer  (:int, :int64, :uint64)
 *     'p'  pointer         (:cstr, :ptr<T>)
 *     'F'  float  (4 bytes)
 *     'f'  double (8 bytes)
 *     '{' ... '}'  nested aggregate
 *
 * Signedness is deliberately absent: it affects neither layout nor
 * classification, and the Turmeric-side field types (the record ADT's
 * CtorFields) are what decide how a member is read back into a value.  The
 * sig carries layout only.
 *
 * In a non-JIT build no provider is ever installed and every consumer keeps
 * today's behavior (per-export __ffi shims, the extern-c nil stub, a clean "requires a JIT-enabled build" diagnostic for
 * call-ptr).  The provider interface is deliberately the swap point if
 * Windows/WASM ever need a non-MIR thunk source (dyncall / libffi). */
#ifndef TUR_TURI_JIT_FFI_H
#define TUR_TURI_JIT_FFI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "compiler/types.h"   /* TypeKind */

/* A compiled call thunk.  `fn` is the target function pointer; `iv`/`fv`
 * are the position-indexed marshalled argument buffers (arg k reads iv[k]
 * or fv[k] by its class -- the same convention the per-export __ffi shims
 * already use).  F4: `sv` follows the same
 * position-indexed convention -- sv[k] points at the raw bytes of arg k
 * when arg k is an aggregate, and is unread otherwise (NULL when the
 * signature has no aggregate parameters).  The return value is written to
 * *out_i or *out_f by the return class ('v' writes neither); an aggregate
 * return is stored through `out_s`, which the caller sizes with
 * tur_jit_ffi_struct_layout. */
typedef void (*TurJitFfiThunkFn)(void *fn, const int64_t *iv,
                                 const double *fv, void *sv,
                                 int64_t *out_i, double *out_f, void *out_s);

typedef struct TurJitFfiProvider {
    /* Return a cached or freshly compiled thunk for `sig`, or NULL with a
     * human-readable reason in errbuf (c2mir compile errors surface here as
     * diagnostics, never aborts). */
    TurJitFfiThunkFn (*thunk_for)(const char *sig, char *errbuf,
                                  size_t errcap);
    /* Resolve a C symbol against this process (dlsym(RTLD_DEFAULT)), for
     * extern-c registration.  NULL when the symbol is not visible; the
     * resolution order (process image incl. ENABLE_EXPORTS runtime, then
     * anything dlopened RTLD_GLOBAL, e.g. jit autolink libs) is dlsym's. */
    void *(*resolve)(const char *name);
    /* F5: return a C function pointer with signature `sig` that calls back
     * into the interpreter.  `ctx` is an opaque context pointer baked into
     * the generated code as an address literal and handed to
     * tur_ffi_cb_dispatch (ffi_thunk.h) on every call, so one compiled
     * callback serves exactly one (signature, Turmeric function) pair.
     *
     * Unlike thunk_for, the result is NOT shareable across contexts, so the
     * cache is keyed on the context as well as the signature.  Callbacks are
     * process-lifetime: a C library holding a function pointer has no way to
     * announce it is finished with one, so there is no safe moment to
     * reclaim the MIR image.  NULL with a reason in errbuf on failure. */
    void *(*callback_for)(const char *sig, void *ctx, char *errbuf,
                          size_t errcap);
} TurJitFfiProvider;

/* Install / read the process-wide provider.  Installed once at startup by a
 * JIT build (tur_jit_ffi_install in jit_ffi.c, called from main); NULL in
 * non-JIT builds and in embedders that never install one. */
void tur_jit_ffi_set_provider(const TurJitFfiProvider *p);
const TurJitFfiProvider *tur_jit_ffi_provider(void);

/* Classify a TypeKind for the signature vocabulary above.  Mirrors codegen's
 * ffi_shim_class_for_kind (emit_module.c) except that float32 reports its
 * exact 'F' class here -- the thunk implements the precise ABI, so it must
 * not widen -- and narrow integers report exact-width classes ('b'/'h'/'w',
 * capitalized for unsigned) so a thunk's C declaration carries the true
 * return width: a callee returning `int` leaves the upper register bits
 * unspecified, and only the exact type makes the read-back extend
 * correctly.  '?' = not representable as a scalar (structs, ADTs,
 * carriers); the caller declines and falls back / errors cleanly. */
char tur_jit_ffi_class_for_kind(TypeKind k, int is_return);

/* True for every integer-register scalar class ('i' and the exact-width
 * codes) -- the marshalling-side complement of the 'f'/'F' float classes. */
bool tur_jit_ffi_class_is_int(char c);

/* Render `n` parameter kinds + a return kind into `buf` as a sig string.
 * Returns 0 on success, -1 (buf untouched) when any kind classifies '?' or
 * the buffer is too small.  bufcap must cover n + 3 bytes.  Scalars only --
 * an aggregate slot has no TypeKind that describes its layout, so struct
 * signatures are built by the caller (see elab_call_ptr / eval_call_ptr,
 * which walk the record ADT's CtorFields with
 * tur_jit_ffi_member_code_for_kind). */
int tur_jit_ffi_sig_render(TypeKind ret, const TypeKind *params, uint32_t n,
                           char *buf, size_t bufcap);

/* ------------------------------------------------------------------ */
/* F4: aggregate layout                                                */
/* ------------------------------------------------------------------ */

/* Classify a TypeKind as an aggregate MEMBER (the exact-width vocabulary
 * documented above).  Returns 0 when the kind cannot be a by-value member.
 * Distinct from tur_jit_ffi_class_for_kind, which reports a *register*
 * class and deliberately collapses every integer width onto 'i'. */
char tur_jit_ffi_member_code_for_kind(TypeKind k);

/* Size / alignment of one member code ('{' is not a member code -- use
 * tur_jit_ffi_struct_layout for a nested aggregate).  Returns 0 size for an
 * unknown code. */
void tur_jit_ffi_member_size_align(char code, size_t *size, size_t *align);

/* Compute the layout of the aggregate whose sig begins at `s` (which must
 * point at '{').  Fills *out_size / *out_align (either may be NULL) and,
 * when `offs`/`codes` are non-NULL, the byte offset and member code of each
 * LEAF member in declaration order (nested aggregates flatten).  Layout is
 * the ordinary C rule: every member is placed at the next offset meeting its
 * own alignment, and the aggregate's size is rounded up to its alignment.
 *
 * Returns a pointer just past the matching '}' on success, or NULL on a
 * malformed sig (unknown code, unbalanced brace, empty aggregate, or more
 * than `max` leaves).  *out_nleaf receives the leaf count. */
const char *tur_jit_ffi_struct_layout(const char *s, size_t *out_size,
                                      size_t *out_align, size_t *offs,
                                      char *codes, int max, int *out_nleaf);

/* True when this host's thunk provider can pass the aggregate at `s`
 * correctly.  Today: everywhere -- the aarch64 HFA carve-out is gone now that
 * the pinned MIR fork implements the AAPCS64 HFA rule (see
 * docs/archive/mir-aarch64-fp-aggregate-abi.md).  Kept as the extension point
 * for the next host that cannot pass some aggregate shape.  `why` (optional)
 * receives a short human-readable reason when the answer is false. */
bool tur_jit_ffi_struct_supported(const char *s, const char **why);

/* Install the c2mir-backed provider.  Defined in jit_ffi.c (tur_jit_obj,
 * JIT builds only); call from the host's startup under #ifdef TUR_HAVE_JIT.
 * Idempotent. */
void tur_jit_ffi_install(void);

#endif /* TUR_TURI_JIT_FFI_H */
