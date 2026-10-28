/* stdatomic.h -- C11 atomics for c2mir in the browser JIT module
 * (wasm-jit-plan P3).  Embedded at /sysinclude/stdatomic.h by
 * tools/wasm-jit/build-module.sh.
 *
 * stdatomic.h is the compiler's header, not libc's, so musl's sysroot has
 * none and c2mir found nothing to open: 208 corpus fixtures failed on the
 * bare `#include <stdatomic.h>` that most emitted units carry, whether they
 * use an atomic or not.  Natively c2mir reads the host compiler's copy.
 *
 * On this engine every operation is a plain access.  That is sound here and
 * nowhere else: the engine refuses every way a program starts a thread
 * (pthread_create, the multi-threaded scheduler, the timer wheel --
 * src/jit_engine.c, JIT_WASM_REFUSED), so the program runs on one thread, and
 * on one thread an atomic operation is the operation.  The GCC builtins the
 * emitted C also uses directly (__atomic_*, __sync_*) get the same treatment
 * in the engine's prelude.
 *
 * c2mir reserves `typeof` but never parses it, so an operation that returns
 * the old value holds it in a long long: exact for every integer type up to
 * 64 bits, which is what the corpus applies them to.  An atomic pointer or
 * floating type would need a cast back (or lose bits, for a double); none is
 * in use.
 */
#ifndef TUR_WASM_JIT_STDATOMIC_H
#define TUR_WASM_JIT_STDATOMIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
  memory_order_relaxed,
  memory_order_consume,
  memory_order_acquire,
  memory_order_release,
  memory_order_acq_rel,
  memory_order_seq_cst
} memory_order;

#define ATOMIC_BOOL_LOCK_FREE 2
#define ATOMIC_CHAR_LOCK_FREE 2
#define ATOMIC_CHAR16_T_LOCK_FREE 2
#define ATOMIC_CHAR32_T_LOCK_FREE 2
#define ATOMIC_WCHAR_T_LOCK_FREE 2
#define ATOMIC_SHORT_LOCK_FREE 2
#define ATOMIC_INT_LOCK_FREE 2
#define ATOMIC_LONG_LOCK_FREE 2
#define ATOMIC_LLONG_LOCK_FREE 2
#define ATOMIC_POINTER_LOCK_FREE 2

typedef bool atomic_bool;
typedef char atomic_char;
typedef signed char atomic_schar;
typedef unsigned char atomic_uchar;
typedef short atomic_short;
typedef unsigned short atomic_ushort;
typedef int atomic_int;
typedef unsigned int atomic_uint;
typedef long atomic_long;
typedef unsigned long atomic_ulong;
typedef long long atomic_llong;
typedef unsigned long long atomic_ullong;
typedef int32_t atomic_int32_t;
typedef uint32_t atomic_uint32_t;
typedef int64_t atomic_int64_t;
typedef uint64_t atomic_uint64_t;
typedef intptr_t atomic_intptr_t;
typedef uintptr_t atomic_uintptr_t;
typedef size_t atomic_size_t;
typedef ptrdiff_t atomic_ptrdiff_t;
typedef intmax_t atomic_intmax_t;
typedef uintmax_t atomic_uintmax_t;

typedef struct { bool __tur_v; } atomic_flag;
#define ATOMIC_FLAG_INIT { 0 }
#define ATOMIC_VAR_INIT(v) (v)

#define atomic_init(p, v) ((void) (*(p) = (v)))
#define kill_dependency(y) (y)
#define atomic_thread_fence(o) ((void) (o))
#define atomic_signal_fence(o) ((void) (o))
#define atomic_is_lock_free(p) ((void) (p), 1)

#define atomic_store(p, v) ((void) (*(p) = (v)))
#define atomic_load(p) (*(p))
#define atomic_exchange(p, v) ({ long long __tur_old = *(p); *(p) = (v); __tur_old; })
#define atomic_compare_exchange_strong(p, e, d) \
  ({ int __tur_ok = (*(p) == *(e)); if (__tur_ok) *(p) = (d); else *(e) = *(p); __tur_ok; })
#define atomic_compare_exchange_weak(p, e, d) atomic_compare_exchange_strong (p, e, d)
#define atomic_fetch_add(p, v) ({ long long __tur_old = *(p); *(p) += (v); __tur_old; })
#define atomic_fetch_sub(p, v) ({ long long __tur_old = *(p); *(p) -= (v); __tur_old; })
#define atomic_fetch_or(p, v) ({ long long __tur_old = *(p); *(p) |= (v); __tur_old; })
#define atomic_fetch_xor(p, v) ({ long long __tur_old = *(p); *(p) ^= (v); __tur_old; })
#define atomic_fetch_and(p, v) ({ long long __tur_old = *(p); *(p) &= (v); __tur_old; })

#define atomic_store_explicit(p, v, o) atomic_store (p, v)
#define atomic_load_explicit(p, o) atomic_load (p)
#define atomic_exchange_explicit(p, v, o) atomic_exchange (p, v)
#define atomic_compare_exchange_strong_explicit(p, e, d, s, f) atomic_compare_exchange_strong (p, e, d)
#define atomic_compare_exchange_weak_explicit(p, e, d, s, f) atomic_compare_exchange_strong (p, e, d)
#define atomic_fetch_add_explicit(p, v, o) atomic_fetch_add (p, v)
#define atomic_fetch_sub_explicit(p, v, o) atomic_fetch_sub (p, v)
#define atomic_fetch_or_explicit(p, v, o) atomic_fetch_or (p, v)
#define atomic_fetch_xor_explicit(p, v, o) atomic_fetch_xor (p, v)
#define atomic_fetch_and_explicit(p, v, o) atomic_fetch_and (p, v)

#define atomic_flag_test_and_set(f) ({ bool __tur_old = (f)->__tur_v; (f)->__tur_v = 1; __tur_old; })
#define atomic_flag_clear(f) ((void) ((f)->__tur_v = 0))
#define atomic_flag_test_and_set_explicit(f, o) atomic_flag_test_and_set (f)
#define atomic_flag_clear_explicit(f, o) atomic_flag_clear (f)

#endif
