/* jit_engine.c -- the in-process MIR JIT engine behind `tur jit`.
 *
 * Phase J1 of docs/archive/jit-engine-plan.md.  Consumes the same emitted C
 * that `tur build` hands to cc -- after the same in-process post-passes
 * (hoist_tur_include_directives, scan_autolink_markers) -- and executes it
 * with no cc subprocess and no disk artifacts:
 *
 *   c2mir (C11 front end) -> MIR_link (symbols resolved against THIS process
 *   by dlsym(RTLD_DEFAULT)) -> MIR_gen (lazy, serialized -- see the generation
 *   mode note below) -> call main on a sized-stack thread.
 *
 * Everything in here is a straight port of the J0 spike harness
 * (tools/jit-spike/tur-jit-spike.c), which the findings doc validated against
 * the full 1,680-fixture corpus at 98.0%.  Where the two diverge it is because
 * `tur` is the host process: the runtime is already linked in (no
 * whole-archive question), tur_collect_symbols is the real one, and the C text
 * arrives as an in-memory buffer rather than a file.
 *
 * Compiled into `tur` only under TUR_JIT (ON by default on 64-bit x86-64 and
 * arm64; it builds the vendored MIR in external/mir via cmake/mir.cmake and
 * sets ENABLE_EXPORTS so dlsym can see the runtime).
 * Without it, cmd_jit (src/driver/jit.c) reports the missing capability.
 */

#include "jit_engine.h"

#ifdef _WIN32
/* MinGW ships no <dlfcn.h>.  platform_dl.h is the same LoadLibrary shim the
 * rest of the tree already uses for this (src/turi/spice_loader.c, and the
 * Godot shim's AOT image loader), so host-symbol resolution goes through one
 * implementation rather than a second, subtly-different one. */
#  include "platform_dl.h"
#  include "jit_win_prelude.h"  /* JIT_PRELUDE_WIN -- see jit_compile_and_link */
#else
#  include <dlfcn.h>
#endif
#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "mir.h"
#include "mir-gen.h"
#include "c2mir.h"
#include "runtime/buf.h"
#include "prof.h"   /* --timings: the jit and run zones */
/* turi/compiled_turn.h, by its one declaration: the engine does not need the
 * interpreter's headers. */
extern void *tur_repl_slot (const char *name, const char *typesig, size_t size, int *fresh);
extern void tur_repl_result_piece (const char *s);

/* ------------------------------------------------------------------ */
/* I0 SPIKE INSTRUMENTATION (mir-interp-tier-plan phase I0)            */
/* Temporary: measures the tier so I0's go/no-go gate has numbers.     */
/* Remove wholesale if I0 comes back negative and the plan is shelved. */
/* ------------------------------------------------------------------ */
#ifndef _WIN32
/* getrlimit/setrlimit live here; MinGW has neither the header nor the calls.
 * Nothing in this file actually calls them today -- the include rides along
 * with the temporary instrumentation block above -- so guarding it costs no
 * functionality on Windows. If a real rlimit use lands, it needs a Win32
 * equivalent (Job Objects), not just an include. */
#  include <sys/resource.h>
#endif
#include <time.h>
#include "async/tur_sjlj.h"

static int g_jit_interp_mode;  /* TUR_JIT_GEN=interp */
static int g_jit_gen_inited;   /* was MIR_gen_init called? */

static double jit_now_ms (void) {
  struct timespec ts;
  clock_gettime (CLOCK_MONOTONIC, &ts);
  return (double) ts.tv_sec * 1000.0 + (double) ts.tv_nsec / 1.0e6;
}

static double g_jit_t0, g_jit_t_prev;
static int g_jit_timing;   /* TUR_JIT_TIMING=1 */
static bool g_jit_quiet_warnings;  /* tur_jit_set_quiet_warnings */

bool tur_jit_set_quiet_warnings (bool quiet) {
  bool was = g_jit_quiet_warnings;
  g_jit_quiet_warnings = quiet;
  return was;
}

static bool g_jit_keep_contexts;   /* tur_jit_set_keep_contexts */
static bool g_jit_preamble_cache;  /* tur_jit_set_preamble_cache */

/* A kept program's footprint (aot-compiled-repl-plan C2, point 5).
 *
 * A compiled REPL turn's program must outlive the turn: a slot may hold a
 * pointer into it -- a function, a string literal, a static.  What such a
 * pointer can reach is the program's machine code and the storage behind its
 * data items, and nothing else: the MIR context around them (its IR, its
 * symbol and string tables, the generator's state, the interpreter's tables)
 * is only ever read while compiling and linking.  Keeping the whole context
 * cost ~1.3 MB a turn, ~280 KB of it the fixed tables MIR_init builds.
 *
 * So a kept context is finished like any other, with its code and data
 * carved out first:
 *
 *   - data: a section head's storage is freed by MIR_finish only while the
 *     item's addr is set (remove_item), so each head's addr moves to
 *     g_kept_data and is cleared before the finish;
 *   - code: the context's code allocator is jit_keep_code_alloc, whose unmap
 *     keeps the pages up to the highest one ever made writable (every byte of
 *     code is written through a protect call) and releases only the untouched
 *     rest of the reservation.
 *
 * Generation is eager under keep (see the knob in tur_jit_execute), so no
 * function is left to generate from the IR that goes.  Where the program
 * runs on MIR's interpreter (TUR_JIT_GEN=interp, wasm) the IR is the program,
 * and the whole context stays; Windows keeps it too, for want of the
 * allocator below.  What is kept lives for the process. */
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#define JIT_KEEP_CARVE 1
#include <sys/mman.h>
#include <unistd.h>

/* mir-code-alloc-default.c's, which carries Apple silicon's JIT write
 * protection; its map and unmap are static and are reproduced below. */
extern int default_mem_protect (void *addr, size_t len, MIR_mem_protect_t prot, void *user_data);

typedef struct { uint8_t *start, *hi; size_t len; } JitKeepMap;
typedef struct { void *p; size_t len; } JitKeptCode;

static JitKeepMap *g_keep_maps;   /* the live kept context's mappings */
static size_t g_n_keep_maps, g_cap_keep_maps;
static JitKeptCode *g_kept_code;  /* pages kept for the process */
static size_t g_n_kept_code, g_cap_kept_code;
static void **g_kept_data;
static size_t g_n_kept_data, g_cap_kept_data;

#define JIT_KEEP_PUSH(arr, n, cap, val)                                     \
  do {                                                                      \
    if ((n) == (cap)) {                                                     \
      size_t nc_ = (cap) ? (cap) * 2 : 16;                                  \
      void *np_ = realloc ((arr), nc_ * sizeof *(arr));                     \
      if (np_ == NULL) abort ();                                            \
      (arr) = np_;                                                          \
      (cap) = nc_;                                                          \
    }                                                                       \
    (arr)[(n)++] = (val);                                                   \
  } while (0)

static void *jit_keep_mem_map (size_t len, void *ud) {
  (void) ud;
#if defined(__APPLE__) && defined(__aarch64__)
  void *p = mmap (NULL, len, PROT_EXEC | PROT_WRITE | PROT_READ,
                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT, -1, 0);
#else
  void *p = mmap (NULL, len, PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
  if (p != MAP_FAILED) {
    JitKeepMap m = { (uint8_t *) p, (uint8_t *) p, len };
    JIT_KEEP_PUSH (g_keep_maps, g_n_keep_maps, g_cap_keep_maps, m);
  }
  return p;
}

static JitKeepMap *jit_keep_find_map (void *addr) {
  for (size_t i = 0; i < g_n_keep_maps; i++)
    if ((uint8_t *) addr >= g_keep_maps[i].start
        && (uint8_t *) addr < g_keep_maps[i].start + g_keep_maps[i].len)
      return &g_keep_maps[i];
  return NULL;
}

static int jit_keep_mem_protect (void *addr, size_t len, MIR_mem_protect_t prot, void *ud) {
  JitKeepMap *m = jit_keep_find_map (addr);
  if (m != NULL && (uint8_t *) addr + len > m->hi) m->hi = (uint8_t *) addr + len;
  return default_mem_protect (addr, len, prot, ud);
}

static int jit_keep_mem_unmap (void *addr, size_t len, void *ud) {
  (void) ud;
  JitKeepMap *m = jit_keep_find_map (addr);
  size_t keep = len;   /* a mapping we did not see: keep it whole */
  if (m != NULL) {
    size_t page = (size_t) sysconf (_SC_PAGESIZE);
    keep = (size_t) (m->hi - m->start);
    keep = (keep + page - 1) / page * page;
    if (keep > len) keep = len;
    *m = g_keep_maps[--g_n_keep_maps];
  }
  if (keep < len) munmap ((uint8_t *) addr + keep, len - keep);
  if (keep > 0) {
    JitKeptCode c = { addr, keep };
    JIT_KEEP_PUSH (g_kept_code, g_n_kept_code, g_cap_kept_code, c);
  }
  return 0;
}

static struct MIR_code_alloc jit_keep_code_alloc = {
  .mem_map = jit_keep_mem_map,
  .mem_unmap = jit_keep_mem_unmap,
  .mem_protect = jit_keep_mem_protect,
  .user_data = NULL,
};

/* Finish a kept context, keeping its code and data (above). */
static void jit_finish_kept_context (MIR_context_t ctx) {
  if (g_jit_interp_mode) return;   /* the IR is the program: keep it all */
  if (g_jit_gen_inited) MIR_gen_finish (ctx);
  for (MIR_module_t m = DLIST_HEAD (MIR_module_t, *MIR_get_module_list (ctx)); m != NULL;
       m = DLIST_NEXT (MIR_module_t, m))
    for (MIR_item_t it = DLIST_HEAD (MIR_item_t, m->items); it != NULL;
         it = DLIST_NEXT (MIR_item_t, it)) {
      switch (it->item_type) {
      case MIR_data_item:
      case MIR_ref_data_item:
      case MIR_lref_data_item:
      case MIR_expr_data_item:
      case MIR_bss_item:
        if (it->addr != NULL && it->section_head_p) {
          JIT_KEEP_PUSH (g_kept_data, g_n_kept_data, g_cap_kept_data, it->addr);
          it->addr = NULL;
        }
        break;
      default: break;
      }
    }
  MIR_finish (ctx);
}
#else
#define JIT_KEEP_CARVE 0
static void jit_finish_kept_context (MIR_context_t ctx) { (void) ctx; }
#endif

bool tur_jit_set_keep_contexts (bool keep) {
  bool was = g_jit_keep_contexts;
  g_jit_keep_contexts = keep;
  return was;
}

bool tur_jit_set_preamble_cache (bool on) {
  bool was = g_jit_preamble_cache;
  g_jit_preamble_cache = on;
  return was;
}

static void jit_timing_begin (void) {
  const char *t = getenv ("TUR_JIT_TIMING");
  g_jit_timing = (t != NULL && strcmp (t, "1") == 0);
  g_jit_t0 = g_jit_t_prev = jit_now_ms ();
}

/* Phase delta since the previous mark, on stderr so fixture stdout stays
 * byte-comparable under the parity harness. */
static void jit_timing_mark (const char *phase) {
  if (!g_jit_timing) return;
  double now = jit_now_ms ();
  fprintf (stderr, "TUR_JIT_TIMING\t%s\t%s\t%.3f\n",
           g_jit_interp_mode ? "interp" : "gen", phase, now - g_jit_t_prev);
  g_jit_t_prev = now;
}

static void jit_timing_rss (void) {
  if (!g_jit_timing) return;
#ifdef _WIN32
  /* No getrusage on MinGW.  PeakWorkingSetSize is the same quantity
   * ru_maxrss reports (peak resident set); psapi comes in via platform_dl.h,
   * which this file already includes for dlsym. */
  PROCESS_MEMORY_COUNTERS pmc;
  double kb = 0.0;
  if (GetProcessMemoryInfo (GetCurrentProcess (), &pmc, sizeof (pmc)))
    kb = (double) pmc.PeakWorkingSetSize / 1024.0;
#else
  struct rusage ru;
  getrusage (RUSAGE_SELF, &ru);
  /* Linux reports KB, macOS bytes; normalize to KB. */
#if defined(__APPLE__)
  double kb = (double) ru.ru_maxrss / 1024.0;
#else
  double kb = (double) ru.ru_maxrss;
#endif
#endif
  fprintf (stderr, "TUR_JIT_TIMING\t%s\tmaxrss_kb\t%.0f\n",
           g_jit_interp_mode ? "interp" : "gen", kb);
  fprintf (stderr, "TUR_JIT_TIMING\t%s\ttotal\t%.3f\n",
           g_jit_interp_mode ? "interp" : "gen", jit_now_ms () - g_jit_t0);
}

/* ------------------------------------------------------------------ */
/* input: the emitted C, in memory                                     */
/* ------------------------------------------------------------------ */
static const char *g_src;
static size_t g_src_len, g_src_pos;

static int jit_getc (void *data) {
  (void) data;
  return g_src_pos >= g_src_len ? EOF : (unsigned char) g_src[g_src_pos++];
}

/* Builtin prototypes, prepended ahead of the TU.  c2mir does not know the
 * __builtin_* family; an undeclared __builtin_sqrt is implicitly declared as
 * int-returning and the call reads the integer return register while the shim
 * delivers the value in xmm0 -- floor(sqrt(25.0)) came out as 1 before the
 * spike learned this (findings 11.7).  These reach the emitted C from INLINE C
 * (stdlib/math.tur and friends), never from codegen.
 *
 * Deliberately absent: the spike shim's `#define __thread` and fake
 * non-atomic __atomic_* lowerings.  Those exist to squeeze fixture coverage
 * out of inline-C the emitter does not own, and shipping them would trade a
 * clean compile error for silent corruption under spawn.  Inline C that uses
 * them fails c2mir loudly and takes the step-6 fallback to cc instead. */
static const char JIT_PRELUDE[] =
  /* Tells runtime C it is being compiled by this engine: r7gc.c (the
   * r7rs-gc experiment) switches its collector off, since a JIT'd
   * program's globals are not in the data segment it scans. */
  "#define TUR_JIT_ENGINE 1\n"
  /* The HOST's sanitizer state, passed into the program because the program
   * cannot see it: c2mir defines neither `__SANITIZE_ADDRESS__` nor
   * `__has_feature`, so inline C compiled here has no way to tell that the
   * process it is about to run in is on ASan's allocator.
   *
   * That distinction matters for fork().  A child forked while another thread
   * holds ASan's allocator lock inherits it HELD, and deadlocks on its next
   * malloc -- glibc's allocator is fork-safe, ASan's is not
   * (docs/archive/jit-fork-child-inherits-asan-allocator-lock.md).  A
   * fixture that forks and then allocates can only know to avoid that if we
   * tell it, so this is the channel.
   *
   * Deliberately narrow: this says "the host is sanitized", NOT "skip hard
   * things".  Only a program that forks and allocates should consult it; the
   * Release JIT and the compiled path leave it undefined and keep testing
   * the real fork path. */
#if defined(__SANITIZE_ADDRESS__)
  "#define TUR_JIT_HOST_ASAN 1\n"
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer)
  "#define TUR_JIT_HOST_ASAN 1\n"
#  endif
#endif
  /* MIR's Apple/aarch64 prelude spells `#define __arm64__` with NO
   * replacement list (c2mir/aarch64/mirc_aarch64_linux.h:135), where Apple
   * clang defines it as 1.  Every SDK `#if __arm64__` therefore expands to a
   * bare `#if` -- "empty preprocessor expression" -- and the guarded block is
   * lost.  `mach/port.h:100` is the one the corpus hits; it guards
   * `xnu_static_assert_struct_size`, so losing it silently DISABLES the SDK's
   * own struct-size assertions.  Restoring the value re-arms them.
   *
   * That mattered: it exposed that c2mir was ignoring the
   * `#pragma pack(push, 4)` mach/message.h:291 wraps those structs in, laying
   * them out at natural alignment (16 bytes where clang gives 12, on a pack(4)
   * probe).  `#pragma pack` is implemented as of MIR fork commit d7e19e8d, so
   * the assertions now PASS and those programs JIT; they stay armed as a live
   * guard against the layout drifting again.  See
   * docs/archive/jit-c2mir-ignores-pragma-pack.md.
   *
   * This runs before any SDK header: c2mir's stream stack reads its own
   * prelude first, so a redefine here lands after MIR's and ahead of the
   * emitted TU's #includes.  Scoped to Apple/aarch64 -- the only target whose
   * prelude carries the empty spelling. */
  "#if defined(__APPLE__) && defined(__aarch64__)\n"
  "#undef __arm64__\n"
  "#define __arm64__ 1\n"
  /* <libkern/OSByteOrder.h>:80 selects on __LITTLE_ENDIAN__/__BIG_ENDIAN__ and
   * #errors "Unknown endianess" when neither is set.  c2mir advertises neither;
   * arm64 Darwin is unambiguously little-endian. */
  "#define __LITTLE_ENDIAN__ 1\n"
  /* <TargetConditionals.h> auto-detects the compiler and #errors at :398 when
   * it recognizes none.  Its own documented workaround (see the comment above
   * that #error) is to set TARGET_CPU_/TARGET_OS_ on the command line, which is
   * what this does -- rather than advertising __GNUC__, which does suppress the
   * #error but then unlocks GCC-only spellings elsewhere in the SDK that c2mir
   * cannot parse (__header_always_inline in sys/_types/_fd_def.h:59 is the
   * first one that bites).
   *
   * Only the macros clang computes as 1 for arm64 macOS are listed, taken from
   * `clang -dM -E -include TargetConditionals.h`.  The ~30 it computes as 0 are
   * deliberately omitted: an undefined macro already evaluates to 0 in #if, so
   * defining them adds nothing, and a short list is far easier to keep honest.
   * Guarded on __aarch64__ so this cannot mislabel an Intel host. */
  "#define TARGET_CPU_ARM64 1\n"
  "#define TARGET_OS_MAC 1\n"
  "#define TARGET_OS_OSX 1\n"
  "#define TARGET_RT_64_BIT 1\n"
  "#define TARGET_RT_LITTLE_ENDIAN 1\n"
  "#define TARGET_RT_MAC_MACHO 1\n"
  "#endif\n"
#ifdef __EMSCRIPTEN__
  /* The browser engine (wasm-jit-plan P3) is the one place the fake
   * non-atomic lowerings the comment above refuses are sound: it refuses
   * every way a program starts a thread (JIT_WASM_REFUSED below), so the
   * program runs on one thread, where an atomic operation is the operation
   * and no other thread can observe the difference.  The fallback here is the
   * interpreter, not cc, so refusing them would cost the 300-odd fixtures whose
   * stdlib inline C uses them their compiled run.  tools/wasm-jit/stdatomic.h
   * is the C11 spelling of the same thing, and thread-local storage is plain
   * static storage for the same reason.
   *
   * c2mir reserves `typeof` but never parses it, so the macros that return
   * the old value hold it in a long long.  GCC defines these builtins on
   * integers and pointers only, and every integer up to 64 bits survives the
   * round trip; the corpus applies them to integers alone. */
  "#define __thread\n"
  "#define _Thread_local\n"
  "#define __ATOMIC_RELAXED 0\n"
  "#define __ATOMIC_CONSUME 1\n"
  "#define __ATOMIC_ACQUIRE 2\n"
  "#define __ATOMIC_RELEASE 3\n"
  "#define __ATOMIC_ACQ_REL 4\n"
  "#define __ATOMIC_SEQ_CST 5\n"
  "#define __atomic_load_n(p, o) (*(p))\n"
  "#define __atomic_store_n(p, v, o) ((void) (*(p) = (v)))\n"
  "#define __atomic_add_fetch(p, v, o) (*(p) += (v))\n"
  "#define __atomic_sub_fetch(p, v, o) (*(p) -= (v))\n"
  "#define __atomic_fetch_add(p, v, o)"
  " ({ long long __tur_o = *(p); *(p) += (v); __tur_o; })\n"
  "#define __atomic_fetch_sub(p, v, o)"
  " ({ long long __tur_o = *(p); *(p) -= (v); __tur_o; })\n"
  "#define __atomic_exchange_n(p, v, o)"
  " ({ long long __tur_o = *(p); *(p) = (v); __tur_o; })\n"
  "#define __atomic_compare_exchange_n(p, e, d, w, s, f)"
  " ({ int __tur_ok = *(p) == *(e); if (__tur_ok) *(p) = (d); else *(e) = *(p); __tur_ok; })\n"
  "#define __atomic_thread_fence(o) ((void) 0)\n"
  "#define __atomic_signal_fence(o) ((void) 0)\n"
  "#define __sync_fetch_and_add(p, v) __atomic_fetch_add (p, v, 5)\n"
  "#define __sync_fetch_and_sub(p, v) __atomic_fetch_sub (p, v, 5)\n"
  "#define __sync_add_and_fetch(p, v) (*(p) += (v))\n"
  "#define __sync_sub_and_fetch(p, v) (*(p) -= (v))\n"
  "#define __sync_bool_compare_and_swap(p, o, n)"
  " ({ int __tur_ok = *(p) == (o); if (__tur_ok) *(p) = (n); __tur_ok; })\n"
  "#define __sync_synchronize() ((void) 0)\n"
#endif
  "double __builtin_pow (double, double);\n"
  "double __builtin_sqrt (double);\n"
  "double __builtin_ceil (double);\n"
  "double __builtin_floor (double);\n"
  "double __builtin_fabs (double);\n"
  "void __builtin_trap (void);\n"
  "unsigned long __builtin_strlen (const char *);\n"
  "int __builtin_popcount (unsigned int);\n"
  "void *__builtin_memcpy (void *, const void *, unsigned long);\n"
  "double __builtin_sin (double);\n"
  "double __builtin_cos (double);\n"
  "double __builtin_exp (double);\n"
  "double __builtin_log (double);\n"
  "double __builtin_atan2 (double, double);\n"
  "double __builtin_tan (double);\n"
  "double __builtin_asin (double);\n"
  "double __builtin_acos (double);\n"
  "double __builtin_atan (double);\n"
  "double __builtin_trunc (double);\n"
  "double __builtin_rint (double);\n"
  "int __builtin_isinf (double);\n"
  "int __builtin_isfinite (double);\n"
  "double __builtin_nan (const char *);\n"
  "double __builtin_inf (void);\n"
  "unsigned short _OSSwapInt16 (unsigned short);\n"
  "unsigned int _OSSwapInt32 (unsigned int);\n"
  "unsigned long long _OSSwapInt64 (unsigned long long);\n";

static double jit_builtin_pow (double x, double y) { return pow (x, y); }
static double jit_builtin_sqrt (double x) { return sqrt (x); }
static double jit_builtin_ceil (double x) { return ceil (x); }
static double jit_builtin_floor (double x) { return floor (x); }
static double jit_builtin_fabs (double x) { return fabs (x); }
static void jit_builtin_trap (void) { abort (); }
static size_t jit_builtin_strlen (const char *s) { return strlen (s); }
static int jit_builtin_popcount (unsigned x) {
  int n = 0;
  while (x) { n += (int) (x & 1u); x >>= 1; }
  return n;
}
static void *jit_builtin_memcpy (void *d, const void *s, size_t n) {
  return memcpy (d, s, n);
}
/* The numeric tower (Rational/Complex, merged from main 2026-07-29) reaches
 * these from stdlib inline C; enumerated by the same grep as the rest. */
static double jit_builtin_sin (double x) { return sin (x); }
static double jit_builtin_cos (double x) { return cos (x); }
static double jit_builtin_exp (double x) { return exp (x); }
static double jit_builtin_log (double x) { return log (x); }
static double jit_builtin_atan2 (double y, double x) { return atan2 (y, x); }
/* r7rs-lang-plan R5/T0 grew stdlib/math.tur (tan, asin, acos, atan, trunc,
 * rint) and the #lang r7rs numeric C (isinf, isfinite, nan, inf -- every use
 * is on a double).  Without these, every program that loads stdlib/math.tur
 * took the cc fallback on macOS.  c2mir itself handles the __builtin_*_overflow
 * family and __builtin_alloca. */
static double jit_builtin_tan (double x) { return tan (x); }
static double jit_builtin_asin (double x) { return asin (x); }
static double jit_builtin_acos (double x) { return acos (x); }
static double jit_builtin_atan (double x) { return atan (x); }
static double jit_builtin_trunc (double x) { return trunc (x); }
static double jit_builtin_rint (double x) { return rint (x); }
static int jit_builtin_isinf (double x) { return isinf (x) ? 1 : 0; }
static int jit_builtin_isfinite (double x) { return isfinite (x) ? 1 : 0; }
static double jit_builtin_nan (const char *tag) { return nan (tag); }
static double jit_builtin_inf (void) { return HUGE_VAL; }

/* ------------------------------------------------------------------ */
/* atexit interception (findings 9.4)                                  */
/* ------------------------------------------------------------------ */
/* Registering the real atexit is worse than failing: the handler is JIT'd
 * code and MIR_gen_finish unmaps it before libc drains its list, so the
 * process dies in freed code at exit.  The JIT owns the list and drains it
 * on the entry thread while the generated code is still mapped. */
#define MAX_JIT_ATEXIT 64
static void (*g_atexit_fns[MAX_JIT_ATEXIT]) (void);
static int g_n_atexit = 0;

static int jit_atexit (void (*fn) (void)) {
  if (g_n_atexit >= MAX_JIT_ATEXIT) return -1;
  g_atexit_fns[g_n_atexit++] = fn;
  return 0;
}

static void jit_atexit_drain (void) {
  while (g_n_atexit > 0) g_atexit_fns[--g_n_atexit] ();
}

/* ------------------------------------------------------------------ */
/* Darwin byte-swap inlines                                            */
/* ------------------------------------------------------------------ */
/* <libkern/_OSByteOrder.h> spells _OSSwapInt{16,32,64} as __DARWIN_OS_INLINE
 * (= static __inline__), and htons/ntohs reach them through the
 * __DARWIN_OSSwapInt16 macro.  c2mir emits no definition for them, so every
 * program touching network byte order -- the whole httpd family plus
 * async-echo-server -- died at MIR_link on `import of undefined item
 * _OSSwapInt16` and took the cc fallback (27 fixtures on macOS).
 *
 * Unlike the __atomic_* family above, these are safe to shim: one fixed
 * signature per name, a pure function of its argument, no memory ordering.
 *
 * The JIT_PRELUDE declarations are NOT optional.  Shimming these by address
 * alone reproduces findings 11.7 exactly: c2mir falls back to an implicit
 * int-returning declaration, the call reads the wrong register, and
 * htons(8080) yields 0xb8f6 instead of 0x901f -- silently corrupting ports
 * and header lengths rather than failing loudly.  Declaring the real
 * prototype ahead of the TU is what makes the shim correct. */
static uint16_t jit_osswap16 (uint16_t x) {
  return (uint16_t) ((x << 8) | (x >> 8));
}
static uint32_t jit_osswap32 (uint32_t x) {
  return ((x << 24) | ((x << 8) & 0x00ff0000u)
          | ((x >> 8) & 0x0000ff00u) | (x >> 24));
}
static uint64_t jit_osswap64 (uint64_t x) {
  return ((uint64_t) jit_osswap32 ((uint32_t) x) << 32)
         | jit_osswap32 ((uint32_t) (x >> 32));
}

static int jit_pthread_create (pthread_t *tid, const pthread_attr_t *attr,
                               void *(*fn) (void *), void *arg);

#ifdef __EMSCRIPTEN__
/* wasm-jit-plan 5.7: a program's exit, _exit or abort ends the program, not
 * the page.  Under Emscripten, libc's exit throws out through the JS caller
 * and the module is not usable afterwards; these unwind to the landing
 * tur_jit_execute sets before entering main (a direct setjmp in compiled
 * code, which longjmp reaches across interpreter frames and the JS
 * trampoline -- tools/wasm-jit-spike/sjlj.c).  exit drains the program's
 * atexit list there; _exit and abort do not.  abort reports 134, the status
 * a shell sees for SIGABRT, which is what the fixtures expect natively. */
static jmp_buf g_jit_exit_jb;
static volatile int g_jit_exit_active;
static volatile int g_jit_exit_code;
static volatile int g_jit_exit_drain;

static void MIR_NO_RETURN jit_wasm_unwind (int code, int drain) {
  if (!g_jit_exit_active) {
    if (drain) exit (code);
    _Exit (code);
  }
  g_jit_exit_code = code;
  g_jit_exit_drain = drain;
  longjmp (g_jit_exit_jb, 1);
}
static void jit_wasm_exit (int code) { jit_wasm_unwind (code, 1); }
static void jit_wasm__exit (int code) { jit_wasm_unwind (code, 0); }
static void jit_wasm_abort (void) {
  fflush (stdout);
  fputs ("Aborted\n", stderr);
  jit_wasm_unwind (134, 0);
}

/* A compiled REPL turn's panic (aot-compiled-repl-plan C2) ends the turn, as
 * natively (jit_turn_unwind): the program's root reports it and would call the
 * module's abort, which throws out through JavaScript and loses the worker.
 * So the two ways a program ends in a panic -- tur_panic_root_exit, where an
 * unwound `(panic ...)` lands, and tur_panic, the runtime's direct one --
 * report it as the runtime does and unwind to the landing.  The panic state
 * is this thread's, and every turn runs on this one, so it is cleared. */
extern void tur_panic_root_print (void);
extern int *tur_tls_panicking_ptr (void);
extern int tur_panic_in_progress;
static void jit_wasm_panic_clear_and_unwind (void) {
  fflush (NULL);
  *tur_tls_panicking_ptr () = 0;
  tur_panic_in_progress = 0;
  jit_wasm_unwind (134, 0);
}
static void jit_wasm_panic_root_exit (void) {
  tur_panic_root_print ();
  jit_wasm_panic_clear_and_unwind ();
}
static void jit_wasm_panic (const char *msg) {
  fprintf (stderr, "panic: %s\n", msg ? msg : "(no message)");
  jit_wasm_panic_clear_and_unwind ();
}
#endif

static const struct { const char *name; void *addr; } JIT_SHIMS[] = {
  {"__builtin_pow", (void *) jit_builtin_pow},
  {"__builtin_sqrt", (void *) jit_builtin_sqrt},
  {"__builtin_ceil", (void *) jit_builtin_ceil},
  {"__builtin_floor", (void *) jit_builtin_floor},
  {"__builtin_fabs", (void *) jit_builtin_fabs},
  {"__builtin_trap", (void *) jit_builtin_trap},
  {"__builtin_strlen", (void *) jit_builtin_strlen},
  {"__builtin_popcount", (void *) jit_builtin_popcount},
  {"__builtin_memcpy", (void *) jit_builtin_memcpy},
  {"__builtin_sin", (void *) jit_builtin_sin},
  {"__builtin_cos", (void *) jit_builtin_cos},
  {"__builtin_exp", (void *) jit_builtin_exp},
  {"__builtin_log", (void *) jit_builtin_log},
  {"__builtin_atan2", (void *) jit_builtin_atan2},
  {"__builtin_tan", (void *) jit_builtin_tan},
  {"__builtin_asin", (void *) jit_builtin_asin},
  {"__builtin_acos", (void *) jit_builtin_acos},
  {"__builtin_atan", (void *) jit_builtin_atan},
  {"__builtin_trunc", (void *) jit_builtin_trunc},
  {"__builtin_rint", (void *) jit_builtin_rint},
  {"__builtin_isinf", (void *) jit_builtin_isinf},
  {"__builtin_isfinite", (void *) jit_builtin_isfinite},
  {"__builtin_nan", (void *) jit_builtin_nan},
  {"__builtin_inf", (void *) jit_builtin_inf},
  {"atexit", (void *) jit_atexit},
  {"pthread_create", (void *) jit_pthread_create},
  /* aot-compiled-repl-plan C2: a compiled REPL turn's globals live in the
   * host's slots (turi/compiled_turn.c); named here so the wasm module, which
   * has no dlsym, resolves it as tur does. */
  {"tur_repl_slot", (void *) tur_repl_slot},
  {"tur_repl_result_piece", (void *) tur_repl_result_piece},
  {"_OSSwapInt16", (void *) jit_osswap16},
  {"_OSSwapInt32", (void *) jit_osswap32},
  {"_OSSwapInt64", (void *) jit_osswap64},
#ifdef _WIN32
  /* The DK trampoline's landing pad (async/tur_sjlj_x64_win.S).  The program
   * half is compiled by c2mir, which has no __builtin_setjmp, so the emitted
   * preamble calls these instead -- and the host half, compiled by the real
   * toolchain, calls the very same functions.  That is the whole point: the
   * two halves have to agree on a setjmp mechanism, and a symbol is something
   * both can name.  Listed here rather than left to the dlsym walk for the
   * same reason printf is: --export-all-symbols does not reach every object
   * inside tur.exe, and a miss here is silent (the pair simply never fires).  */
  {"tur_sjlj_set", (void *) tur_sjlj_set},
  {"tur_sjlj_jump", (void *) tur_sjlj_jump},
  /* The printf/snprintf family are NOT dlsym-resolvable on MinGW: ucrtbase
   * exports only __stdio_common_vf* (the header-inline bodies call those),
   * and ld's --export-all-symbols deliberately excludes the MinGW runtime
   * objects that define the classic names inside tur.exe.  So the resolver's
   * RTLD_DEFAULT walk misses them everywhere.  Handing out the HOST's own
   * addresses (this TU is compiled by the real toolchain) is exactly what
   * this table is for.  strdup rides along for the same export-list reason. */
  {"printf", (void *) printf},
  {"fprintf", (void *) fprintf},
  {"snprintf", (void *) snprintf},
  {"vsnprintf", (void *) vsnprintf},
  {"vfprintf", (void *) vfprintf},
  {"sscanf", (void *) sscanf},
  {"puts", (void *) puts},
  {"putchar", (void *) putchar},
  {"fputs", (void *) fputs},
  {"fputc", (void *) fputc},
  {"fflush", (void *) fflush},
  {"fgets", (void *) fgets},
  {"fopen", (void *) fopen},
  {"fclose", (void *) fclose},
  {"fread", (void *) fread},
  {"fwrite", (void *) fwrite},
  {"fseek", (void *) fseek},
  {"ftell", (void *) ftell},
  {"feof", (void *) feof},
  {"ferror", (void *) ferror},
  {"remove", (void *) remove},
  {"rename", (void *) rename},
  {"strdup", (void *) strdup},
  {"strerror", (void *) strerror},
  {"__acrt_iob_func", (void *) __acrt_iob_func},
  {"sprintf", (void *) sprintf},
  /* winpthreads is linked into tur.exe STATICALLY and, like the MinGW runtime
   * above, is left out of --export-all-symbols -- so none of these reach the
   * RTLD_DEFAULT walk either.  The split path never noticed: its program half
   * calls no pthread function, the host half does.  The whole-preamble
   * fallback compiles the runtime too, and died at MIR_link on the first one
   * (`import of undefined item pthread_cond_timedwait`). */
  {"pthread_join", (void *) pthread_join},
  {"pthread_detach", (void *) pthread_detach},
  {"pthread_exit", (void *) pthread_exit},
  {"pthread_self", (void *) pthread_self},
  {"pthread_equal", (void *) pthread_equal},
  {"pthread_mutex_init", (void *) pthread_mutex_init},
  {"pthread_mutex_lock", (void *) pthread_mutex_lock},
  {"pthread_mutex_trylock", (void *) pthread_mutex_trylock},
  {"pthread_mutex_unlock", (void *) pthread_mutex_unlock},
  {"pthread_mutex_destroy", (void *) pthread_mutex_destroy},
  {"pthread_cond_init", (void *) pthread_cond_init},
  {"pthread_cond_wait", (void *) pthread_cond_wait},
  {"pthread_cond_timedwait", (void *) pthread_cond_timedwait},
  {"pthread_cond_signal", (void *) pthread_cond_signal},
  {"pthread_cond_broadcast", (void *) pthread_cond_broadcast},
  {"pthread_cond_destroy", (void *) pthread_cond_destroy},
  {"pthread_once", (void *) pthread_once},
  {"pthread_key_create", (void *) pthread_key_create},
  {"pthread_getspecific", (void *) pthread_getspecific},
  {"pthread_setspecific", (void *) pthread_setspecific},
  {"clock_gettime", (void *) clock_gettime},
  {"nanosleep", (void *) nanosleep},
#endif
#ifdef __EMSCRIPTEN__
  {"exit", (void *) jit_wasm_exit},
  {"_exit", (void *) jit_wasm__exit},
  {"_Exit", (void *) jit_wasm__exit},
  {"abort", (void *) jit_wasm_abort},
#endif
};

/* ------------------------------------------------------------------ */
/* serialized lazy generation (findings 8.1 / 34)                      */
/* ------------------------------------------------------------------ */
/* MIR's own `MIR_set_lazy_gen_interface` installs a first-call wrapper that
 * runs MIR-gen on the context's single shared gen_ctx.  Two threads entering
 * two not-yet-generated functions therefore corrupt that state: measured as
 * three different assertions across five runs of one fixture
 * (`destroy_func_cfg`, `mark_unreachable_bbs`, `undeclared reg N of func`).
 * MIR-gen is simply not thread-safe, and Turmeric has `spawn`, fibers, and a
 * work-stealing scheduler.
 *
 * The plan's instruction is "serialize generation behind a lock or generate
 * eagerly for any program that can spawn".  Serializing needs no fork patch:
 * every piece MIR's lazy path uses is public (`_MIR_get_wrapper`,
 * `_MIR_redirect_thunk`, `MIR_gen`, and `machine_code` on the func), so the
 * interface is reimplemented here with a mutex around it.  `MIR_gen` is
 * literally `generate_func_code (ctx, item, TRUE)` -- the same call MIR's hook
 * makes -- so this is MIR's lazy semantics plus mutual exclusion, not a
 * different code path.
 *
 * The DOUBLE-CHECK is the load-bearing half.  A plain lock still lets two
 * threads that both got past the stub generate the same function twice, which
 * is what trips `_MIR_duplicate_func_insns`; re-reading `machine_code` under
 * the lock makes the second arrival a lookup.
 *
 * Contention is bounded and self-extinguishing: a function is generated once,
 * after which its thunk goes straight to the code and never reaches this hook
 * again. The lock is process-wide rather than per-context because gen state is
 * per-context but the cost of over-serializing across images is a few
 * microseconds on a path that runs once per function. */
static pthread_mutex_t g_gen_lock = PTHREAD_MUTEX_INITIALIZER;

/* fork (docs/archive/jit-fork-child-hangs-with-threads.md): a child forked
 * while another thread was generating inherited g_gen_lock held by a thread
 * that does not exist in it, and hung on the first function it called that
 * was not yet generated.  The lock is taken before the fork, so generation
 * is never mid-way when the address space is copied, and released on both
 * sides after.  No fork on Windows. */
#ifndef _WIN32
static void jit_gen_atfork_prepare (void) { pthread_mutex_lock (&g_gen_lock); }
static void jit_gen_atfork_release (void) { pthread_mutex_unlock (&g_gen_lock); }
static pthread_once_t g_gen_atfork_once = PTHREAD_ONCE_INIT;
static void jit_gen_atfork_register (void) {
  pthread_atfork (jit_gen_atfork_prepare, jit_gen_atfork_release, jit_gen_atfork_release);
}
#endif

static void *jit_lazy_gen_locked (MIR_context_t ctx, MIR_item_t func_item) {
  pthread_mutex_lock (&g_gen_lock);
  void *code = func_item->u.func->machine_code;
  if (code == NULL) code = MIR_gen (ctx, func_item);
  pthread_mutex_unlock (&g_gen_lock);
  return code;
}

/* Drop-in for MIR_set_lazy_gen_interface, routing first-call generation
 * through the lock above.  Mirrors MIR's version exactly otherwise. */
static void jit_set_lazy_gen_interface (MIR_context_t ctx, MIR_item_t func_item) {
  void *addr;

  if (func_item == NULL) return;
#ifndef _WIN32
  pthread_once (&g_gen_atfork_once, jit_gen_atfork_register);
#endif
  addr = _MIR_get_wrapper (ctx, func_item, jit_lazy_gen_locked);
  _MIR_redirect_thunk (ctx, func_item->addr, addr);
}

/* Threads and lazy generation (docs/archive/jit-threaded-program-hangs-under-load.md).
 * Generating a function ends in MIR redirecting its call thunk: 13 bytes
 * rewritten in place with a plain memcpy (_MIR_redirect_thunk ->
 * _MIR_change_code).  The lock above keeps two GENERATORS apart, but not a
 * thread that is at that moment executing the thunk: it can run a jump whose
 * displacement is half old, half new, and land anywhere -- a SIGSEGV, or a
 * spin in unrelated code.  Under CPU load, with more preemption inside those
 * instructions, `r7rs-threads-pause` hung in 6 of 150 runs lazy (and
 * segfaulted in 2 of another 150) and 0 of 150 eager.
 *
 * So a program that starts a thread stops being lazy at that moment: the
 * first pthread_create generates every function not yet generated, while the
 * program is still single-threaded, and after it no thunk is ever rewritten
 * again.  A program that never starts a thread keeps the whole lazy saving.
 * (The engine's own entry thread is started by the host, not through here,
 * and runs the program alone.) */
static MIR_context_t g_jit_lazy_ctx;   /* the context still generating lazily */

/* A thread the program started can still be running when its `main` returns
 * -- a detached worker, or one the program never joins.  A native program
 * then exits and the thread dies with the process; here the engine would go
 * on to MIR_finish, freeing the module's code and globals under it.  Seen in
 * `r7rs-threads-lifecycle` under suite load: a worker's memcpy read a bss
 * global that MIR_finish's remove_item had just freed.  So once the program
 * has started a thread, its context is retired, not torn down: it lives, as
 * a native program's image does, until the process ends, and stays reachable
 * from g_jit_retired_ctx so LeakSanitizer does not report it. */
static int g_jit_program_started_threads;
static MIR_context_t g_jit_retired_ctx __attribute__ ((unused));

static void jit_generate_rest (MIR_context_t ctx) {
  pthread_mutex_lock (&g_gen_lock);
  for (MIR_module_t m = DLIST_HEAD (MIR_module_t, *MIR_get_module_list (ctx)); m != NULL;
       m = DLIST_NEXT (MIR_module_t, m))
    for (MIR_item_t it = DLIST_HEAD (MIR_item_t, m->items); it != NULL;
         it = DLIST_NEXT (MIR_item_t, it))
      if (it->item_type == MIR_func_item && it->u.func->machine_code == NULL)
        MIR_gen (ctx, it);
  pthread_mutex_unlock (&g_gen_lock);
}

/* A context being torn down stops being the one a thread start finishes. */
static void jit_forget_lazy_ctx (MIR_context_t ctx) {
  MIR_context_t expected = ctx;
  __atomic_compare_exchange_n (&g_jit_lazy_ctx, &expected, NULL, 0,
                               __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

static int jit_pthread_create (pthread_t *tid, const pthread_attr_t *attr,
                               void *(*fn) (void *), void *arg) {
  MIR_context_t ctx = __atomic_exchange_n (&g_jit_lazy_ctx, NULL, __ATOMIC_SEQ_CST);
  if (ctx != NULL) jit_generate_rest (ctx);
  __atomic_store_n (&g_jit_program_started_threads, 1, __ATOMIC_SEQ_CST);
  return pthread_create (tid, attr, fn, arg);
}

/* MIR's default error handler prints and EXITS the process -- from inside
 * MIR_link, an unresolved import (e.g. a GCC atomic builtin in user inline-C
 * that c2mir compiled as an implicit call) would kill `tur` before cmd_jit's
 * step-6 fallback could run.  Found by the first full-corpus sweep of the
 * real subcommand: 13 fixtures whose stdlib inline-C uses __atomic_* died
 * with empty output instead of falling back to cc (findings 18.1).  Unwind
 * to tur_jit_execute instead; the half-initialized context is deliberately
 * LEAKED (tearing it down from an undefined intermediate state is how a
 * fallback becomes a crash), which is acceptable in a one-shot CLI. */
static jmp_buf g_jit_err_jb;
static volatile int g_jit_err_active = 0;

/* tur_jit_last_error: the most recent MIR error, or an import this target
 * refused (which wins: the refusal says why, the MIR error after it only
 * repeats the name). */
static char g_jit_last_error[512];
static bool g_jit_refused;

const char *tur_jit_last_error (void) { return g_jit_last_error; }

static void jit_last_error_clear (void) {
  g_jit_last_error[0] = '\0';
  g_jit_refused = false;
}

static void MIR_NO_RETURN jit_mir_error (MIR_error_type_t type, const char *fmt, ...)
    TUR_PRINTF_FMT(2, 3);
static void MIR_NO_RETURN jit_mir_error (MIR_error_type_t type, const char *fmt, ...) {
  va_list ap;
  va_start (ap, fmt);
  if (!g_jit_refused) {
    va_list cp;
    va_copy (cp, ap);
    vsnprintf (g_jit_last_error, sizeof g_jit_last_error, fmt, cp);
    va_end (cp);
  }
  fprintf (stderr, "tur: jit: ");
  vfprintf (stderr, fmt, ap);
  fputc ('\n', stderr);
  va_end (ap);
  (void) type;
  if (g_jit_err_active) longjmp (g_jit_err_jb, 1);
  exit (1);
}

#ifdef __EMSCRIPTEN__
/* wasm-jit-plan 5.7: imports the browser engine refuses, so the program
 * fails at MIR_link -- before main, before any output -- and the page falls
 * back to the interpreter.  Each starts a thread that would run interpreted
 * code: the program's own spawn, the multi-threaded scheduler's workers, the
 * timer wheel's callbacks.  The MIR interpreter keeps its state per context,
 * not per thread, so a second thread in it races the first (the threads
 * family of the P0 corpus run). */
static const char *const JIT_WASM_REFUSED[] = {
  "pthread_create", "tur_scheduler_mt_new", "tur_timer_wheel_new",
};

static bool jit_wasm_refuses (const char *name) {
  for (size_t i = 0; i < sizeof JIT_WASM_REFUSED / sizeof JIT_WASM_REFUSED[0]; i++)
    if (strcmp (name, JIT_WASM_REFUSED[i]) == 0) {
      snprintf (g_jit_last_error, sizeof g_jit_last_error,
                "the program starts threads (%s), which this engine cannot run", name);
      g_jit_refused = true;
      return true;
    }
  return false;
}
#endif

#ifdef __EMSCRIPTEN__
/* wasm-jit-plan 5.6: setjmp's address cannot be taken on this target (emcc
 * refuses any indirect use of it), so the import resolves to this stand-in;
 * MIR_load_external swaps it for its own marker, and the interpreter calls
 * setjmp directly, in the frame of the function that asked for it. */
static char jit_wasm_setjmp_import;
#endif

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
static void jit_turn_exit (int code);
static void jit_turn__exit (int code);
static void jit_turn_abort (void);
#endif

static void *jit_import_resolver (const char *name) {
#ifdef __EMSCRIPTEN__
  if (jit_wasm_refuses (name)) return NULL;
  if (g_jit_keep_contexts) {   /* a REPL turn's panic ends the turn */
    if (strcmp (name, "tur_panic_root_exit") == 0) return (void *) jit_wasm_panic_root_exit;
    if (strcmp (name, "tur_panic") == 0) return (void *) jit_wasm_panic;
  }
  if (strcmp (name, "setjmp") == 0 || strcmp (name, "_setjmp") == 0)
    return &jit_wasm_setjmp_import;
#endif
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
  if (g_jit_keep_contexts) {   /* a REPL turn ends, not the REPL */
    if (strcmp (name, "exit") == 0) return (void *) jit_turn_exit;
    if (strcmp (name, "_exit") == 0 || strcmp (name, "_Exit") == 0) return (void *) jit_turn__exit;
    if (strcmp (name, "abort") == 0) return (void *) jit_turn_abort;
  }
#endif
  for (size_t i = 0; i < sizeof JIT_SHIMS / sizeof JIT_SHIMS[0]; i++)
    if (strcmp (name, JIT_SHIMS[i].name) == 0) return JIT_SHIMS[i].addr;
#ifdef __EMSCRIPTEN__
  /* wasm-jit-plan 5.3: a plain Emscripten module has no dlsym, so the
   * program's imports resolve against a table generated at build time from
   * the runtime's declarations region plus the libc names the emitted C
   * uses (tools/wasm-jit/gen-symtab.py). */
  extern void *tur_wasm_jit_symtab_lookup (const char *);
  return tur_wasm_jit_symtab_lookup (name);
#else
  return dlsym (RTLD_DEFAULT, name);
#endif
}

/* ------------------------------------------------------------------ */
/* weak-symbol handshake (findings 11.7 / archived reactor report)     */
/* ------------------------------------------------------------------ */
/* libturi declares `__attribute__((weak)) int tur_closure_headers_enabled`
 * and the emitted program overrides it with a strong definition; the linker
 * resolves that under cc, but host code in this process was linked long ago
 * and reads its own weak copy.  Copy the program's value onto the host's
 * global after the module is loaded. */
extern int tur_closure_headers_enabled;

static void jit_sync_config_globals (MIR_context_t ctx) {
  for (MIR_module_t m = DLIST_HEAD (MIR_module_t, *MIR_get_module_list (ctx)); m != NULL;
       m = DLIST_NEXT (MIR_module_t, m))
    for (MIR_item_t it = DLIST_HEAD (MIR_item_t, m->items); it != NULL;
         it = DLIST_NEXT (MIR_item_t, it))
      if (it->item_type == MIR_data_item && it->u.data->name != NULL
          && strcmp (it->u.data->name, "tur_closure_headers_enabled") == 0
          && it->addr != NULL)
        tur_closure_headers_enabled = *(int *) it->addr;
}

/* ------------------------------------------------------------------ */
/* autolink: dlopen -l<name> entries so dlsym can reach them           */
/* ------------------------------------------------------------------ */
/* The cc path hands `__tur_autolink__` flags to the linker; the JIT's
 * equivalent is loading each -l<name> into the process with RTLD_GLOBAL.
 * -lturi and the libs `tur` already links (m, pthread, dl) are skipped --
 * their symbols are resolvable in this process by construction.  Any entry
 * that fails to load fails the whole JIT attempt, which the caller turns
 * into the step-6 fallback to cc (or, for the REPL spice hook, the
 * subprocess build).
 *
 * ffi-spices-integration-plan S1: -L dirs are honored -- a cmake-built
 * spice dependency lives in the build tree, not on the default search
 * path, so each -l probes `<Ldir>/lib<name>.so` (and .dylib on Apple)
 * before falling back to the bare soname.  A static-only dep (.a, which
 * dlopen cannot load) therefore fails here cleanly rather than resolving
 * against nothing. */
static int jit_load_autolink (const char *flags) {
  if (!flags) return 0;
  enum { MAX_LDIRS = 16 };
  const char *ldir[MAX_LDIRS];
  size_t ldir_len[MAX_LDIRS];
  size_t n_ldirs = 0;
  /* Pass 1: collect -L dirs (cc semantics: all -L apply to every -l,
   * regardless of order). */
  for (const char *p = flags; *p;) {
    while (*p == ' ') p++;
    const char *e = p;
    while (*e && *e != ' ') e++;
    if (e - p > 2 && p[0] == '-' && p[1] == 'L' && n_ldirs < MAX_LDIRS) {
      ldir[n_ldirs] = p + 2;
      ldir_len[n_ldirs] = (size_t) (e - p - 2);
      n_ldirs++;
    }
    p = e;
  }
  /* Pass 2: dlopen each -l entry. */
  const char *p = flags;
  while (*p) {
    while (*p == ' ') p++;
    const char *e = p;
    while (*e && *e != ' ') e++;
    if (e - p > 2 && p[0] == '-' && p[1] == 'l') {
      char name[256];
      size_t n = (size_t) (e - p - 2);
      if (n < sizeof name - 16) {
        memcpy (name, p + 2, n);
        name[n] = '\0';
        if (strcmp (name, "turi") != 0 && strcmp (name, "m") != 0
            && strcmp (name, "pthread") != 0 && strcmp (name, "dl") != 0) {
          void *h = NULL;
          char soname[4400];
          for (size_t d = 0; h == NULL && d < n_ldirs; d++) {
            if (ldir_len[d] + n + 32 >= sizeof soname) continue;
            snprintf (soname, sizeof soname, "%.*s/lib%s.so",
                      (int) ldir_len[d], ldir[d], name);
            h = dlopen (soname, RTLD_NOW | RTLD_GLOBAL);
#if defined(__APPLE__)
            if (h == NULL) {
              snprintf (soname, sizeof soname, "%.*s/lib%s.dylib",
                        (int) ldir_len[d], ldir[d], name);
              h = dlopen (soname, RTLD_NOW | RTLD_GLOBAL);
            }
#endif
          }
          if (h == NULL) {
            snprintf (soname, sizeof soname, "lib%s.so", name);
            h = dlopen (soname, RTLD_NOW | RTLD_GLOBAL);
          }
#if defined(__APPLE__)
          if (h == NULL) {
            snprintf (soname, sizeof soname, "lib%s.dylib", name);
            h = dlopen (soname, RTLD_NOW | RTLD_GLOBAL);
          }
#endif
          if (h == NULL) {
            fprintf (stderr, "tur: jit: cannot load lib%s: %s\n", name,
                     dlerror ());
            snprintf (g_jit_last_error, sizeof g_jit_last_error,
                      "cannot load lib%s", name);
            return -1;
          }
        }
      }
    }
    p = e;
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* entry thread (findings 15.3)                                        */
/* ------------------------------------------------------------------ */
/* MIR-gen frames for direct-path recursion are ~1.4x gcc's, so a recursion
 * that fits the default 8 MB under `tur build` can blow the stack here.  A
 * sized entry stack is the sanctioned stopgap ("any size temporarily is
 * fine", owner 2026-07-29); the long-run fix is MIR frame-size work or the
 * runtime's existing stackless machinery, never a bigger constant.  The
 * atexit drain runs on this thread because handlers may read host TLS
 * (tur_tls.c) the program wrote here. */
struct jit_entry_box {
  int (*fn) (int, char **, char **);
  int argc;
  char **argv;
  int rc;
};

/* jit-stack-overflow-has-no-message: a recursion that ran off this thread's
 * stack used to end `tur` itself with a bare SIGSEGV (SIGBUS on macOS) and
 * nothing on stderr, in every dialect -- the compiled r7rs path's handler is
 * emitted behind `!defined(__MIRC__)`, and under the JIT the dying process is
 * the engine's anyway.  So the entry thread carries the same handler, on an
 * alternate stack, with the same test and message (runtime/stack_overflow.h):
 * an overflow prints the line and the fault then repeats with the default
 * action, so the exit status is what it was.  Any other fault is handed back
 * to whatever handled the signal before (the default, or ASan's) and repeats
 * there, so a wild pointer in JIT'd code behaves exactly as it did.  The
 * handlers are installed for the run and restored after the join.  The
 * browser engine has no entry thread (it runs main on the caller's), so none
 * of this is compiled there. */
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#  include <signal.h>
#  include <unistd.h>
#  include "runtime/stack_overflow.h"

static unsigned char *volatile g_jit_entry_lo;  /* low end of the entry stack, or NULL */
static struct sigaction g_jit_prev_segv, g_jit_prev_bus;

static void jit_entry_fault (int sig, siginfo_t *si, void *uc) {
  (void) uc;
  unsigned char *a = (unsigned char *) si->si_addr, *lo = g_jit_entry_lo;
  if (TUR_STACK_FAULT_IS_OVERFLOW (a, lo)) {
    static const char m[] = TUR_STACK_OVERFLOW_MSG "\n";
    if (write (2, m, sizeof m - 1) < 0) { }
    signal (sig, SIG_DFL);   /* the fault repeats on return, unhandled */
    return;
  }
  sigaction (sig, sig == SIGSEGV ? &g_jit_prev_segv : &g_jit_prev_bus, NULL);
  if (si->si_code <= 0) raise (sig);   /* sent, not a fault: it will not repeat */
}

static void jit_entry_fault_install (void) {
  struct sigaction sa;
  memset (&sa, 0, sizeof sa);
  sa.sa_sigaction = jit_entry_fault;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigemptyset (&sa.sa_mask);
  sigaction (SIGSEGV, &sa, &g_jit_prev_segv);
  sigaction (SIGBUS, &sa, &g_jit_prev_bus);
}

static void jit_entry_fault_restore (void) {
  g_jit_entry_lo = NULL;
  sigaction (SIGSEGV, &g_jit_prev_segv, NULL);
  sigaction (SIGBUS, &g_jit_prev_bus, NULL);
}

/* A compiled REPL turn (tur_jit_set_keep_contexts) runs inside the REPL's own
 * process, so its program's exit, _exit or abort -- and a runtime panic, which
 * ends in the host's abort() -- must end the turn, not the session.  As the
 * browser engine does for the page (5.7, jit_wasm_unwind), they unwind to a
 * landing jit_run_entry sets before main, and the turn reports the status a
 * shell would have seen: the exit code, or 134 for an abort.  The program's
 * calls resolve to the shims below (jit_import_resolver); the host's own
 * abort() arrives as SIGABRT on the entry thread.  `tur jit` keeps libc's,
 * so a program run whole still ends the process as it would natively. */
static sigjmp_buf g_jit_turn_jb;
static volatile sig_atomic_t g_jit_turn_armed;
static volatile int g_jit_turn_code, g_jit_turn_drain;
static pthread_t g_jit_turn_thread;
static struct sigaction g_jit_prev_abrt;

static int jit_turn_landing_here (void) {
  return g_jit_turn_armed && pthread_equal (pthread_self (), g_jit_turn_thread);
}

static void MIR_NO_RETURN jit_turn_unwind (int code, int drain) {
  if (!jit_turn_landing_here ()) {
    if (drain) exit (code);
    _Exit (code);
  }
  g_jit_turn_code = code;
  g_jit_turn_drain = drain;
  siglongjmp (g_jit_turn_jb, 1);
}
static void jit_turn_exit (int code) { jit_turn_unwind (code, 1); }
static void jit_turn__exit (int code) { jit_turn_unwind (code, 0); }
static void jit_turn_abort (void) {
  fflush (stdout);
  fputs ("Aborted\n", stderr);
  jit_turn_unwind (134, 0);
}

static void jit_turn_sigabrt (int sig) {
  if (jit_turn_landing_here ()) {
    g_jit_turn_code = 134;
    g_jit_turn_drain = 0;
    siglongjmp (g_jit_turn_jb, 1);
  }
  sigaction (sig, &g_jit_prev_abrt, NULL);
  raise (sig);
}

static void jit_turn_abrt_install (void) {
  struct sigaction sa;
  memset (&sa, 0, sizeof sa);
  sa.sa_handler = jit_turn_sigabrt;
  sigemptyset (&sa.sa_mask);
  sigaction (SIGABRT, &sa, &g_jit_prev_abrt);
}

static void jit_turn_abrt_restore (void) { sigaction (SIGABRT, &g_jit_prev_abrt, NULL); }

/* main under the landing.  A function of its own so the landing's frame holds
 * nothing a longjmp could clobber. */
struct jit_entry_box;
static void jit_turn_run (struct jit_entry_box *box, char **envp);

/* On the entry thread: find its stack's low end and give it an alternate
 * stack to report from.  Returns the alternate stack to free, or NULL; *old
 * receives the one it replaced (ASan gives every thread its own, and frees
 * it at thread exit, so it goes back before the thread ends). */
static void *jit_entry_guard_enter (stack_t *old) {
  unsigned char *lo = NULL;
#  if defined(__APPLE__)
  lo = (unsigned char *) pthread_get_stackaddr_np (pthread_self ())
       - pthread_get_stacksize_np (pthread_self ());
#  elif defined(__GLIBC__)
  {
    extern int pthread_getattr_np (pthread_t, pthread_attr_t *);
    pthread_attr_t a;
    void *addr = NULL;
    size_t sz = 0;
    if (pthread_getattr_np (pthread_self (), &a) == 0) {
      if (pthread_attr_getstack (&a, &addr, &sz) == 0) lo = (unsigned char *) addr;
      pthread_attr_destroy (&a);
    }
  }
#  endif
  if (!lo) return NULL;
  size_t alt_size = 65536;
  void *alt = malloc (alt_size);
  if (!alt) return NULL;
  stack_t ss;
  ss.ss_sp = alt;
  ss.ss_size = alt_size;
  ss.ss_flags = 0;
  if (sigaltstack (&ss, old) != 0) {
    free (alt);
    return NULL;
  }
  g_jit_entry_lo = lo;
  return alt;
}

static void jit_entry_guard_leave (void *alt, const stack_t *old) {
  if (!alt) return;
  g_jit_entry_lo = NULL;
  stack_t prev = *old;
  if (sigaltstack (&prev, NULL) != 0) {
    stack_t off;
    memset (&off, 0, sizeof off);
    off.ss_flags = SS_DISABLE;
    sigaltstack (&off, NULL);
  }
  free (alt);
}
#endif

#ifndef __EMSCRIPTEN__
#ifndef _WIN32
static void jit_turn_run (struct jit_entry_box *box, char **envp) {
  extern int tur_panic_in_progress;
  struct jit_entry_box *volatile vbox = box;
  g_jit_turn_thread = pthread_self ();
  if (sigsetjmp (g_jit_turn_jb, 1) == 0) {
    g_jit_turn_armed = 1;
    vbox->rc = vbox->fn (vbox->argc, vbox->argv, envp);
    g_jit_turn_armed = 0;
    jit_atexit_drain ();
  } else {
    g_jit_turn_armed = 0;
    vbox->rc = g_jit_turn_code;
    if (g_jit_turn_drain) jit_atexit_drain ();
    tur_panic_in_progress = 0;   /* the next turn's panic is not a double one */
  }
  fflush (stdout);
  fflush (stderr);
}
#endif

static void *jit_run_entry (void *p) {
  struct jit_entry_box *box = (struct jit_entry_box *) p;
  char *fake_envp[] = {NULL};
#ifndef _WIN32
  stack_t old_alt;
  void *alt = jit_entry_guard_enter (&old_alt);
#endif
#ifndef _WIN32
  if (g_jit_keep_contexts) {   /* a REPL turn: see jit_turn_unwind */
    jit_turn_run (box, fake_envp);
    jit_entry_guard_leave (alt, &old_alt);
    return NULL;
  }
#endif
  box->rc = box->fn (box->argc, box->argv, fake_envp);
  jit_atexit_drain ();
  fflush (stdout);
#ifndef _WIN32
  jit_entry_guard_leave (alt, &old_alt);
#endif
  return NULL;
}
#endif /* !__EMSCRIPTEN__ */

#ifdef _WIN32
/* ------------------------------------------------------------------ */
/* prelude shadowing: a program may define a name the prelude declares  */
/* ------------------------------------------------------------------ */
/* JIT_PRELUDE_WIN declares ~106 libc/libm/pthread entry points, because c2mir
 * cannot read the UCRT headers.  A Turmeric program may legitimately define a
 * function with one of those names -- `(defn log2 [x : int] : void ...)` is
 * ordinary code -- and the emitted TU then carries `static void log2(int64_t);`
 * AFTER the prelude's `double log2(double);`.
 *
 * That TU is ill-formed, and gcc rejects the pair outright.  c2mir accepts it
 * silently and keeps the FIRST prototype at every call site: the int argument
 * goes in xmm0 per the `double` signature while the callee reads rcx, so the
 * program runs to completion and prints a garbage value.  Ten of the prelude's
 * one-argument math names reproduce it (log2 log10 sqrt sin cos fabs round
 * trunc ceil floor); every other declared name is a live footgun of the same
 * shape, waiting for a program to pick it.
 *
 * So drop any prelude declaration whose name the emitted TU defines: the TU's
 * own `static` declaration then serves its calls, with no conflict for c2mir to
 * resolve wrongly.  Only names the TU DEFINES are dropped, so nothing loses a
 * prototype it still needs -- which would be the implicit-int pointer
 * truncation from the same family, see
 * docs/archive/jit-c2mir-implicit-decl-truncates-pointers.md.
 *
 * The cc path never needs this: it compiles the raw TU against real headers,
 * where the only `log2` in scope is a gcc BUILTIN, and gcc lets a local
 * definition shadow a builtin (with -Wbuiltin-declaration-mismatch). */

/* The identifier immediately before the first `(` in [s,e), if any.  Trailing
 * space is skipped, so `log2 (double)` and `log2(double)` both yield "log2";
 * `void (*f)(int)` yields nothing, because `*` ends the backward scan with an
 * empty identifier. */
static int jit_name_before_paren (const char *s, const char *e, const char **name,
                                  size_t *nlen) {
  const char *p = (const char *) memchr (s, '(', (size_t) (e - s));
  if (p == NULL) return 0;
  while (p > s && (p[-1] == ' ' || p[-1] == '\t')) p--;
  const char *idend = p;
  while (p > s && (isalnum ((unsigned char) p[-1]) || p[-1] == '_')) p--;
  if (p == idend || isdigit ((unsigned char) *p)) return 0;
  *name = p;
  *nlen = (size_t) (idend - p);
  return 1;
}

/* A prelude line is a single-line declaration when it starts in column 0 with
 * an identifier character (so: not `#define`, not a comment, not an indented
 * continuation of the two multi-line declarations) and ends in `;`. */
static int jit_prelude_decl_name (const char *s, const char *e, const char **name,
                                  size_t *nlen) {
  if (s == e) return 0;
  if (!isalpha ((unsigned char) *s) && *s != '_') return 0;
  if (e[-1] != ';') return 0;
  return jit_name_before_paren (s, e, name, nlen);
}

/* Does the emitted TU declare or define `name` at file scope?  Every function
 * a single-TU emission produces is `static` (jit_find_entry relies on the same
 * property), so a line whose first token is `static` and whose first `(` is
 * preceded by `name` is the TU claiming that name.  A future emission that
 * stopped saying `static` would only make this filter stop firing -- it can
 * never make it drop a declaration the TU does not own. */
static int jit_tu_defines (const char *csrc, size_t csrc_len, const char *name,
                           size_t nlen) {
  const char *p = csrc, *end = csrc + csrc_len;
  while (p < end) {
    const char *nl = (const char *) memchr (p, '\n', (size_t) (end - p));
    const char *le = nl ? nl : end;
    if ((size_t) (le - p) > 7 && memcmp (p, "static", 6) == 0
        && (p[6] == ' ' || p[6] == '\t')) {
      const char *n;
      size_t l;
      if (jit_name_before_paren (p, le, &n, &l) && l == nlen
          && memcmp (n, name, nlen) == 0)
        return 1;
    }
    if (!nl) break;
    p = nl + 1;
  }
  return 0;
}

/* JIT_PRELUDE_WIN with every declaration the TU defines commented out, or NULL
 * when there is nothing to drop (the overwhelmingly common case -- no copy is
 * made and the caller uses the literal).  The replacement keeps the line count
 * stable so a c2mir diagnostic's <tur-jit>:LINE still points where it did. */
static char *jit_prelude_win_shadowed (const char *csrc, size_t csrc_len,
                                       size_t *out_len) {
  const size_t pre_len = sizeof JIT_PRELUDE_WIN - 1;
  Buf b;
  int dropped = 0;
  buf_init (&b);
  const char *p = JIT_PRELUDE_WIN, *end = JIT_PRELUDE_WIN + pre_len;
  while (p < end) {
    const char *nl = (const char *) memchr (p, '\n', (size_t) (end - p));
    const char *le = nl ? nl : end;
    const char *n;
    size_t l;
    if (jit_prelude_decl_name (p, le, &n, &l) && jit_tu_defines (csrc, csrc_len, n, l)) {
      buf_puts (&b, "/* shadowed by the program: ");
      buf_write (&b, n, l);
      buf_puts (&b, " */");
      dropped++;
      const char *v = getenv ("TUR_JIT_TIMING");
      if (v && *v && strcmp (v, "0") != 0)
        fprintf (stderr, "TUR_JIT_TIMING\tprelude\tshadowed\t%.*s\n", (int) l, n);
    } else {
      buf_write (&b, p, (size_t) (le - p));
    }
    if (!nl) break;
    buf_putc (&b, '\n');
    p = nl + 1;
  }
  if (!dropped) {
    buf_free (&b);
    return NULL;
  }
  *out_len = b.len;
  return b.data;
}
#endif /* _WIN32 */

/* ------------------------------------------------------------------ */
/* the preamble cache (aot-compiled-repl-plan C2)                       */
/* ------------------------------------------------------------------ */
/* Every program the emitter writes starts the same way: JIT_PRELUDE, the
 * hoisted prefix, and the committed declarations region the S2 split swaps
 * in, which ends at JIT_PRE_CACHE_END.  c2mir preprocesses that part --
 * mostly the system headers it includes -- in every compile, ~45 of the ~85
 * ms a REPL turn spends in c2mir.  So the first compile preprocesses it once
 * (c2mir -E -fdump-defines: the expanded text, then the #pragma pack state
 * and every macro still defined, as directives), and a compile whose first
 * part is the same text feeds c2mir [that output][#line][the rest] instead,
 * which reads back as the original did.  The program half keeps its line
 * numbers: the #line puts it where it was.
 *
 * One entry, keyed by the text itself and the include dirs: a REPL session's
 * turns all share it.  A compile from the cached form that fails is done
 * again from the original text, silently first, so a program's own error is
 * reported once and as before; if the original compiles, the cache is turned
 * off for the process and TUR_JIT_TIMING says so.  Off unless
 * tur_jit_set_preamble_cache turned it on (a compiled REPL session) or
 * TUR_JIT_PREAMBLE_CACHE=1 -- a one-shot `tur jit` would pay the extra
 * preprocessing pass and never reuse it; TUR_JIT_PREAMBLE_CACHE=0 turns it off.
 * Needs open_memstream, so not on Windows, and a c2mir with
 * prepro_defines_p (MIR fork, C2MIR_PREPRO_DEFINES). */
#if !defined(_WIN32) && defined(C2MIR_PREPRO_DEFINES)
#define JIT_PRE_CACHE 1
static const char JIT_PRE_CACHE_END[] = "/* ==== S2: host-resident runtime; declarations only ==== */\n";
static char *g_pre_key;            /* the first part it was made from */
static size_t g_pre_key_len;
static char *g_pre_key_incs;       /* the include dirs, each ending in \n */
static char *g_pre_out;            /* its -E -fdump-defines output; NULL when it did not preprocess */
static size_t g_pre_out_len;
static size_t g_pre_key_lines;     /* newlines in the key: the rest starts on the next line */
static bool g_pre_cache_broken;    /* a compile from it failed that the original did not */

static bool jit_pre_cache_on (void) {
  const char *e = getenv ("TUR_JIT_PREAMBLE_CACHE");
  if (e != NULL && *e) return strcmp (e, "0") != 0 && !g_pre_cache_broken;
  return g_jit_preamble_cache && !g_pre_cache_broken;
}

static size_t jit_pre_cache_find_end (const char *s, size_t len) {
  const size_t m = sizeof JIT_PRE_CACHE_END - 1;
  for (const char *p = s; p != NULL && (size_t) (p - s) + m <= len;
       p = memchr (p + 1, '/', len - (size_t) (p + 1 - s))) {
    if (memcmp (p, JIT_PRE_CACHE_END, m) == 0) return (size_t) (p - s) + m;
  }
  return 0;
}

static char *jit_pre_cache_incs (const char **include_dirs, int n_include_dirs) {
  Buf b;
  buf_init (&b);
  for (int i = 0; i < n_include_dirs; i++) {
    buf_puts (&b, include_dirs[i]);
    buf_putc (&b, '\n');
  }
  buf_putc (&b, '\0');
  return b.data;
}

/* Preprocess full[0, key_len) into the cache.  The output stays NULL when
 * c2mir refuses it; the normal compile then reports why. */
static void jit_pre_cache_fill (const char *full, size_t key_len, char *incs,
                                const char **include_dirs, int n_include_dirs) {
  free (g_pre_key);
  free (g_pre_key_incs);
  free (g_pre_out);
  g_pre_key = (char *) malloc (key_len);
  g_pre_key_len = key_len;
  g_pre_key_incs = incs;
  g_pre_out = NULL;
  g_pre_out_len = 0;
  g_pre_key_lines = 0;
  if (g_pre_key == NULL) {
    g_pre_key_len = 0;
    return;
  }
  memcpy (g_pre_key, full, key_len);
  for (size_t i = 0; i < key_len; i++) g_pre_key_lines += full[i] == '\n';

  char *out = NULL;
  size_t out_len = 0;
  FILE *f = open_memstream (&out, &out_len);
  if (f == NULL) return;
  MIR_context_t ctx = MIR_init ();
  c2mir_init (ctx);
  struct c2mir_options ops;
  memset (&ops, 0, sizeof ops);
  ops.message_file = NULL;
  ops.prepro_only_p = 1;
  ops.prepro_defines_p = 1;
  ops.prepro_output_file = f;
  ops.include_dirs_num = (size_t) (n_include_dirs > 0 ? n_include_dirs : 0);
  ops.include_dirs = include_dirs;
  g_src = full;
  g_src_len = key_len;
  g_src_pos = 0;
  int ok = c2mir_compile (ctx, &ops, jit_getc, NULL, "<tur-jit>", NULL);
  c2mir_finish (ctx);
  MIR_finish (ctx);
  fclose (f);
  if (ok) {
    g_pre_out = out;
    g_pre_out_len = out_len;
  } else {
    free (out);
  }
}

/* The cached form of full, or NULL to compile full itself: the cached first
 * part's output, a #line, and the rest. */
static char *jit_pre_cache_source (const char *full, size_t full_len, size_t prelude_len,
                                   const char **include_dirs, int n_include_dirs,
                                   size_t *out_len) {
  if (!jit_pre_cache_on ()) return NULL;
  size_t end = jit_pre_cache_find_end (full + prelude_len, full_len - prelude_len);
  if (end == 0) return NULL;   /* not a split TU: no committed region */
  size_t key_len = prelude_len + end;
  char *incs = jit_pre_cache_incs (include_dirs, n_include_dirs);
  if (incs == NULL) return NULL;
  if (g_pre_key != NULL && g_pre_key_len == key_len && strcmp (g_pre_key_incs, incs) == 0
      && memcmp (g_pre_key, full, key_len) == 0) {
    free (incs);
  } else {
    jit_pre_cache_fill (full, key_len, incs, include_dirs, n_include_dirs);
    jit_timing_mark ("preamble_cache_fill");
  }
  if (g_pre_out == NULL) return NULL;
  char line[64];
  int ll = snprintf (line, sizeof line, "#line %zu \"<tur-jit>\"\n", g_pre_key_lines + 1);
  size_t rest = full_len - key_len;
  char *s = (char *) malloc (g_pre_out_len + (size_t) ll + rest + 1);
  if (s == NULL) return NULL;
  memcpy (s, g_pre_out, g_pre_out_len);
  memcpy (s + g_pre_out_len, line, (size_t) ll);
  memcpy (s + g_pre_out_len + ll, full + key_len, rest);
  *out_len = g_pre_out_len + (size_t) ll + rest;
  s[*out_len] = '\0';
  return s;
}
#endif /* JIT_PRE_CACHE */

static MIR_context_t jit_new_context (void) {
#if JIT_KEEP_CARVE
  return g_jit_keep_contexts ? MIR_init2 (NULL, &jit_keep_code_alloc) : MIR_init ();
#else
  return MIR_init ();
#endif
}

#ifdef JIT_PRE_CACHE
/* Compile the cached form: the context, c2mir still live in it, or NULL and
 * the caller compiles the original.  Its messages are held back until it is
 * known to have compiled, so a failure says nothing. */
static MIR_context_t jit_pre_cache_compile (const char *src, size_t len,
                                            const char **include_dirs, int n_include_dirs) {
  char *msgs = NULL;
  size_t msgs_len = 0;
  FILE *mf = open_memstream (&msgs, &msgs_len);
  if (mf == NULL) return NULL;
  MIR_context_t ctx = jit_new_context ();
  MIR_set_error_func (ctx, jit_mir_error);
  g_jit_err_active = 1;
  if (setjmp (g_jit_err_jb) != 0) {   /* the context is left, as on any longjmp'd error */
    g_jit_err_active = 0;
    fclose (mf);
    free (msgs);
    return NULL;
  }
  c2mir_init (ctx);
  struct c2mir_options ops;
  memset (&ops, 0, sizeof ops);
  ops.message_file = mf;
  ops.ignore_warnings_p = g_jit_quiet_warnings;
  ops.include_dirs_num = (size_t) (n_include_dirs > 0 ? n_include_dirs : 0);
  ops.include_dirs = include_dirs;
  g_src = src;
  g_src_len = len;
  g_src_pos = 0;
  int ok = c2mir_compile (ctx, &ops, jit_getc, NULL, "<tur-jit>", NULL);
  g_jit_err_active = 0;
  fclose (mf);
  if (!ok) {
    c2mir_finish (ctx);
    MIR_finish (ctx);
    free (msgs);
    return NULL;
  }
  if (msgs_len) fwrite (msgs, 1, msgs_len, stderr);
  free (msgs);
  return ctx;
}
#endif

/* ------------------------------------------------------------------ */
/* shared front half: compile + load + link one emitted TU              */
/* ------------------------------------------------------------------ */
/* Everything up to "the program is linked and generated": prelude concat,
 * c2mir, module load, eager MIR_link with the process resolver, and the
 * config-global sync.  Shared verbatim by the one-shot tur_jit_execute and
 * the J2 image path; on success the caller owns tearing the context down
 * (MIR_gen_finish -> c2mir_finish -> MIR_finish, in that order).
 *
 * Returns TUR_JIT_OK with *out_ctx set, or a TUR_JIT_ERR_* class with all
 * engine state already torn down (except the deliberately-leaked context
 * on a longjmp'd link error -- see jit_mir_error). */
static int jit_compile_and_link (const char *csrc, size_t csrc_len,
                                 const char *autolink,
                                 const char **include_dirs, int n_include_dirs,
                                 MIR_context_t *out_ctx) {
  *out_ctx = NULL;
  jit_last_error_clear ();
  if (jit_load_autolink (autolink) != 0) return TUR_JIT_ERR_LINK;

  /* Prepend the builtin prototypes.  A memory concat beats teaching c2mir
   * about a second stream.
   *
   * On Windows, JIT_PRELUDE_WIN (src/jit_win_prelude.h) goes in too: c2mir
   * cannot digest the UCRT/MinGW system headers (vadefs.h hard-#errors
   * without compiler va intrinsics; winnt.h pulls the GCC-internal
   * x86intrin.h), so the prelude predefines every include guard the emitted
   * TU can reach -- the #includes then open the real files and expand to
   * NOTHING -- and declares the libc/pthread/winsock surface the emitted
   * code uses, with by-value struct layouts matched to the host toolchain.
   * It also swallows the emitted ucontext shim's file-scope __asm__ (the
   * host carries those primitives; MIR_link resolves them).  cc never sees
   * any of this -- the fallback compiles the raw TU against real headers. */
#ifdef _WIN32
  /* ...except where the program defines one of the names the prelude declares,
   * which c2mir resolves to the prelude's signature and miscompiles silently.
   * jit_prelude_win_shadowed returns NULL (and copies nothing) unless the TU
   * actually claims one. */
  size_t prelude_win_len = sizeof JIT_PRELUDE_WIN - 1;
  const char *prelude_win = JIT_PRELUDE_WIN;
  char *prelude_win_owned = jit_prelude_win_shadowed (csrc, csrc_len, &prelude_win_len);
  if (prelude_win_owned != NULL) prelude_win = prelude_win_owned;
#else
  const size_t prelude_win_len = 0;
#endif
  size_t full_len = sizeof JIT_PRELUDE - 1 + prelude_win_len + csrc_len;
  char *full = (char *) malloc (full_len + 1);
#ifdef _WIN32
  if (!full) { free (prelude_win_owned); return TUR_JIT_ERR_COMPILE; }
#else
  if (!full) return TUR_JIT_ERR_COMPILE;
#endif
  memcpy (full, JIT_PRELUDE, sizeof JIT_PRELUDE - 1);
#ifdef _WIN32
  memcpy (full + sizeof JIT_PRELUDE - 1, prelude_win, prelude_win_len);
  free (prelude_win_owned);
#endif
  memcpy (full + sizeof JIT_PRELUDE - 1 + prelude_win_len, csrc, csrc_len);
  /* wasm-jit-plan P1: the text c2mir is handed, with the prelude, so a
   * <tur-jit>:LINE diagnostic can be looked up -- from the page, or from a
   * compiled REPL turn, whose C is never written anywhere else. */
  {
    const char *dump = getenv ("TUR_JIT_DUMP_FULL");
    if (dump && *dump) {
      FILE *df = fopen (dump, "wb");
      if (df) { fwrite (full, 1, full_len, df); fclose (df); }
    }
  }
  full[full_len] = '\0';

  jit_timing_begin ();
#ifdef __EMSCRIPTEN__
  /* wasm-jit-plan 5.5: the program half's calls into the runtime take exact
   * call_indirect stubs rather than MIR's JS trampoline (generated with the
   * import table, tools/wasm-jit/gen-symtab.py --stubs).  The registry is
   * the module's, so once is enough. */
  {
    extern void tur_wasm_jit_register_stubs (void);
    static bool stubs_registered;
    if (!stubs_registered) {
      tur_wasm_jit_register_stubs ();
      stubs_registered = true;
    }
  }
#endif
  MIR_context_t ctx = NULL;
#ifdef JIT_PRE_CACHE
  bool cache_failed = false;
  {
    size_t cached_len = 0;
    char *cached = jit_pre_cache_source (full, full_len, full_len - csrc_len,
                                         include_dirs, n_include_dirs, &cached_len);
    if (cached != NULL) {
      ctx = jit_pre_cache_compile (cached, cached_len, include_dirs, n_include_dirs);
      cache_failed = ctx == NULL;
      free (cached);
    }
  }
#endif
  const bool compiled = ctx != NULL;
  if (!compiled) ctx = jit_new_context ();
  g_src = full;
  g_src_len = full_len;
  g_src_pos = 0;
  MIR_set_error_func (ctx, jit_mir_error);
  g_jit_err_active = 1;
  if (setjmp (g_jit_err_jb) != 0) {
    g_jit_err_active = 0;
    free (full);
    return TUR_JIT_ERR_LINK;
  }
  if (!compiled) {
    c2mir_init (ctx);

    struct c2mir_options ops;
    memset (&ops, 0, sizeof ops);
    ops.message_file = stderr;
    ops.ignore_warnings_p = g_jit_quiet_warnings;

    ops.include_dirs_num = (size_t) (n_include_dirs > 0 ? n_include_dirs : 0);
    ops.include_dirs = include_dirs;

    int ok = c2mir_compile (ctx, &ops, jit_getc, NULL, "<tur-jit>", NULL);
    if (!ok) {
      g_jit_err_active = 0;
      c2mir_finish (ctx);
      MIR_finish (ctx);
      free (full);
      return TUR_JIT_ERR_COMPILE;
    }
#ifdef JIT_PRE_CACHE
    if (cache_failed) {   /* the cached form failed and the original did not */
      g_pre_cache_broken = true;
      const char *v = getenv ("TUR_JIT_TIMING");
      if (v && *v && strcmp (v, "0") != 0)
        fprintf (stderr, "TUR_JIT_TIMING\tpreamble_cache\tbroken\toff for this process\n");
    }
#endif
  }

  /* I0 SPIKE (mir-interp-tier-plan section 3, phase I0): TUR_JIT_GEN=interp
   * selects MIR's bytecode interpreter instead of the generator, so the tier
   * can be measured on the production path rather than a scratch driver.
   * MIR_gen_init is SKIPPED in that mode -- the plan's benefit-3 claim (an
   * interp-only deployment need not link mir-gen-<arch>.c) is only testable
   * if the generator is never initialized.  g_jit_gen_inited keeps the
   * matching MIR_gen_finish calls honest. */
  g_jit_interp_mode = 0;
  {
    const char *g = getenv ("TUR_JIT_GEN");
    if (g != NULL && strcmp (g, "interp") == 0) g_jit_interp_mode = 1;
  }
#ifdef __EMSCRIPTEN__
  /* wasm-jit-plan 5.1: WebAssembly cannot generate code at runtime, so the
   * interpreter is the only engine here (mir-gen.c is not linked). */
  g_jit_interp_mode = 1;
#endif
  jit_timing_mark ("c2mir");
  if (!g_jit_interp_mode) {
    MIR_gen_init (ctx);
    MIR_gen_set_optimize_level (ctx, 2);
    g_jit_gen_inited = 1;
  } else {
    g_jit_gen_inited = 0;
  }
  jit_timing_mark ("gen_init");

  for (MIR_module_t m = DLIST_HEAD (MIR_module_t, *MIR_get_module_list (ctx)); m != NULL;
       m = DLIST_NEXT (MIR_module_t, m))
    MIR_load_module (ctx, m);

  /* Generation mode.  LAZY is the default -- the plan's original
   * recommendation, restored now that both defects that withdrew it are
   * addressed (findings 34):
   *
   *   8.1  lazy is not re-entrant -> jit_set_lazy_gen_interface above
   *        serializes generation and double-checks machine_code, so two
   *        threads racing a first call cannot corrupt gen state or generate
   *        the same function twice.
   *   8.4.1  lazy miscompiled pthread entries single-threaded -> does not
   *        reproduce at the current fork pin; re-measured directly on both
   *        fixtures the finding named.  Its `undeclared reg N` symptom now
   *        appears only as one of the race's several faces, so 8.4.1 was
   *        most likely always 8.1 seen without the concurrency in view.
   *
   * Full corpus is identical either way (2394/0/47), and lazy saves 23-36%
   * of end-to-end wall time on a Release build -- enough to move `tur jit`
   * from parity with the cc round trip to clearly ahead of it.
   *
   *   TUR_JIT_GEN=eager   MIR_set_gen_interface
   *   TUR_JIT_GEN=lazy    jit_set_lazy_gen_interface (default)
   *
   * The escape hatch is worth keeping and worth understanding: eager doubles
   * as a VERIFICATION pass, generating every function before any of the
   * program runs, so a generation failure surfaces at compile time where
   * cmd_jit's step-6 cc fallback can still catch it.  Under lazy the same
   * failure surfaces at first call -- after output may already have been
   * written, and past the point where g_jit_err_active can unwind to the
   * fallback.  The set of functions that fail is the same in both modes (same
   * generator, same input; lazy merely skips ones never called), so this is a
   * question of WHEN, not WHETHER.  No fixture reaches it.  If one ever does,
   * TUR_JIT_GEN=eager is the diagnostic.
   *
   * Deliberately an env knob, not an --enable= experiment: it selects between
   * two implementations of one already-shipping behavior. */
  void (*gen_iface) (MIR_context_t, MIR_item_t) = jit_set_lazy_gen_interface;
  {
    const char *g = getenv ("TUR_JIT_GEN");
    if (g != NULL && strcmp (g, "eager") == 0) gen_iface = MIR_set_gen_interface;
    if (g_jit_keep_contexts) gen_iface = MIR_set_gen_interface;   /* see the knob */
    /* I0 spike: see the mode selection above. */
    if (g_jit_interp_mode) gen_iface = MIR_set_interp_interface;
  }
  jit_timing_mark ("load");
  MIR_link (ctx, gen_iface, jit_import_resolver);
  jit_timing_mark ("link");
  if (gen_iface == jit_set_lazy_gen_interface)
    __atomic_store_n (&g_jit_lazy_ctx, ctx, __ATOMIC_SEQ_CST);
  jit_sync_config_globals (ctx);
  g_jit_err_active = 0;   /* past the last MIR call that can raise */

  free (full);             /* c2mir consumed the stream; the text is done */
  *out_ctx = ctx;
  return TUR_JIT_OK;
}

/* Find the generated function named `name` across the context's modules.
 * MIR item lookup sees static functions too (unlike dlsym), which is what
 * lets the single-TU spice emission keep its `static` linkage. */
static MIR_item_t jit_find_func (MIR_context_t ctx, const char *name) {
  for (MIR_module_t m = DLIST_HEAD (MIR_module_t, *MIR_get_module_list (ctx)); m != NULL;
       m = DLIST_NEXT (MIR_module_t, m))
    for (MIR_item_t it = DLIST_HEAD (MIR_item_t, m->items); it != NULL;
         it = DLIST_NEXT (MIR_item_t, it))
      if (it->item_type == MIR_func_item && strcmp (it->u.func->name, name) == 0)
        return it;
  return NULL;
}

/* ------------------------------------------------------------------ */
/* the engine                                                          */
/* ------------------------------------------------------------------ */
/* post-jit-benchmark-resurrection-plan B4: last-execution phase timings,
 * readable after tur_jit_execute returns.  compile covers c2mir + link
 * (+ eager gen when TUR_JIT_GEN=eager); run is the entry thread's wall
 * time -- which under the default LAZY gen interface includes first-call
 * code generation, a fact the benchmark methodology records rather than
 * hides. */
static double g_jit_stat_compile_ms, g_jit_stat_run_ms;

void tur_jit_last_timings (double *compile_ms, double *run_ms) {
  if (compile_ms) *compile_ms = g_jit_stat_compile_ms;
  if (run_ms) *run_ms = g_jit_stat_run_ms;
}

int tur_jit_execute (const char *csrc, size_t csrc_len, const char *autolink,
                     const char **include_dirs, int n_include_dirs,
                     int prog_argc, char **prog_argv, int *prog_rc) {
  g_n_atexit = 0;
  double t_start = jit_now_ms ();
  MIR_context_t ctx;
  /* --timings: c2mir + MIR link is "jit"; the entry thread below is "run"
   * (which, under the default lazy generation, includes first-call codegen --
   * the same split run_ms reports). */
  prof_enter (PROF_JIT);
  int frc = jit_compile_and_link (csrc, csrc_len, autolink,
                                  include_dirs, n_include_dirs, &ctx);
  prof_exit (PROF_JIT);
  if (frc != TUR_JIT_OK) return frc;
  g_jit_stat_compile_ms = jit_now_ms () - t_start;

  MIR_item_t main_item = jit_find_func (ctx, "main");
  if (main_item == NULL) {
    jit_forget_lazy_ctx (ctx);
    if (g_jit_gen_inited) MIR_gen_finish (ctx);
    c2mir_finish (ctx);
    MIR_finish (ctx);
    fprintf (stderr, "tur: jit: no main in generated module\n");
    return TUR_JIT_ERR_COMPILE;
  }

  typedef int (*main_fn) (int, char **, char **);
  struct jit_entry_box box = { (main_fn) main_item->addr, prog_argc, prog_argv, 0 };

  /* r7rs-deep-recursion-segfaults-silently: the same 1 GiB the cc path gives
   * a `#lang r7rs` main (emit_deep_stack_runtime) -- address space, committed
   * only as deep as the program recurses.  64 MiB ran SICP 1.2.1's
   * million-deep linear recursion off the end; MIR frames are larger than
   * gcc's and it does no sibling-call optimization. */
  /* r7rs-deep-stack-size-not-configurable: TUR_MAIN_STACK_MB is the one
   * name for main's stack on both engines (the cc path reads it in
   * tur_deep_size, emit_module.c); TUR_JIT_STACK_MB, the older JIT-only
   * name, still works and wins when both are set. */
  size_t stack_mb = sizeof (void *) >= 8 ? 1024 : 64;
  const char *senv_name = NULL;
  static const char *const stack_vars[] = { "TUR_JIT_STACK_MB", "TUR_MAIN_STACK_MB" };
  for (size_t vi = 0; vi < sizeof stack_vars / sizeof stack_vars[0] && !senv_name; vi++) {
    const char *senv = getenv (stack_vars[vi]);
    if (!senv || !*senv) continue;
    char *end = NULL;
    unsigned long long mb = strtoull (senv, &end, 10);
    if (end && *end == '\0' && mb > 0 && mb <= (unsigned long long) (SIZE_MAX >> 20)) {
      stack_mb = (size_t) mb;
      senv_name = stack_vars[vi];
    } else {
      fprintf (stderr, "tur: jit: ignoring %s=%s (want a positive number of MiB)\n",
               stack_vars[vi], senv);
    }
  }
  bool stack_asked = senv_name != NULL;
#ifdef __EMSCRIPTEN__
  /* wasm-jit-plan 5.1: no entry thread on the page.  The caller is the eval
   * worker, whose stack is the module's -sSTACK_SIZE; a sized pthread would
   * put the interpreter on a thread the JS trampoline cannot serve. */
  (void) stack_asked;
  (void) stack_mb;
  double t_run = jit_now_ms ();
  prof_enter (PROF_RUN);
  {
    /* On wasm an interpreted function's `addr` is a table slot only
     * wasm_ff_dispatch can call (mir-wasm.c), not a wasm function pointer,
     * so the host enters main through MIR_interp_arr instead of calling
     * box.fn: the host-to-interpreted direction needs no shim. */
    MIR_val_t res, args[3];
    char *fake_envp[] = { NULL };
    args[0].i = box.argc;
    args[1].a = box.argv;
    args[2].a = fake_envp;
    g_jit_exit_active = 1;
    if (setjmp (g_jit_exit_jb) == 0) {
      MIR_interp_arr (ctx, main_item, &res, 3, args);
      box.rc = (int) res.i;
      jit_atexit_drain ();
    } else {
      box.rc = g_jit_exit_code;
      if (g_jit_exit_drain) jit_atexit_drain ();
    }
    g_jit_exit_active = 0;
    fflush (stdout);
    fflush (stderr);
  }
  prof_exit (PROF_RUN);
  g_jit_stat_run_ms = jit_now_ms () - t_run;
#else
  pthread_attr_t attr;
  pthread_t entry_thread;
  pthread_attr_init (&attr);
  pthread_attr_setstacksize (&attr, stack_mb << 20);
#ifndef _WIN32
  jit_entry_fault_install ();
  if (g_jit_keep_contexts) jit_turn_abrt_install ();
#endif
  if (pthread_create (&entry_thread, &attr, jit_run_entry, &box) != 0) {
#ifndef _WIN32
    jit_entry_fault_restore ();
    if (g_jit_keep_contexts) jit_turn_abrt_restore ();
#endif
    pthread_attr_destroy (&attr);
    jit_forget_lazy_ctx (ctx);
    if (g_jit_gen_inited) MIR_gen_finish (ctx);
    c2mir_finish (ctx);
    MIR_finish (ctx);
    if (stack_asked)
      fprintf (stderr, "tur: jit: cannot make a %zu MiB entry stack (%s)\n", stack_mb, senv_name);
    else
      fprintf (stderr, "tur: jit: entry thread create failed\n");
    return TUR_JIT_ERR_RUN;
  }
  double t_run = jit_now_ms ();
#ifndef _WIN32
  /* A process-directed signal (kill, Ctrl+C, a timeout's SIGTERM) goes to any
   * thread that does not block it.  Natively that is the program's own
   * thread; here it could be this host thread, idle in pthread_join.  Then a
   * signal the program catches with signal/handle set its flag without
   * interrupting the program's blocking call (a net/accept waited on), and
   * one the program had blocked for reactor-add-signal's signalfd ran its
   * default action here and killed the process.  So this thread blocks them
   * for the run, entry thread created first so it keeps the caller's mask.
   * Synchronous faults stay open: they are thread-directed, and this thread
   * takes none while it joins. */
  sigset_t host_block, host_prev;
  sigfillset (&host_block);
  sigdelset (&host_block, SIGSEGV);
  sigdelset (&host_block, SIGBUS);
  sigdelset (&host_block, SIGFPE);
  sigdelset (&host_block, SIGILL);
  sigdelset (&host_block, SIGTRAP);
  sigdelset (&host_block, SIGABRT);
  pthread_sigmask (SIG_BLOCK, &host_block, &host_prev);
#endif
  prof_enter (PROF_RUN);
  pthread_join (entry_thread, NULL);
  prof_exit (PROF_RUN);
#ifndef _WIN32
  pthread_sigmask (SIG_SETMASK, &host_prev, NULL);
  jit_entry_fault_restore ();
  if (g_jit_keep_contexts) jit_turn_abrt_restore ();
#endif
  g_jit_stat_run_ms = jit_now_ms () - t_run;
  pthread_attr_destroy (&attr);
#endif
  jit_timing_mark ("run");
  jit_timing_rss ();

  prof_enter (PROF_JIT);   /* --timings: tearing the MIR context down is the engine's */
  jit_forget_lazy_ctx (ctx);
  if (g_jit_keep_contexts) {
    /* Resident for the session (tur_jit_set_keep_contexts): the program's
     * code and data stay, and the context around them goes. */
    c2mir_finish (ctx);
    jit_finish_kept_context (ctx);
  } else if (__atomic_load_n (&g_jit_program_started_threads, __ATOMIC_SEQ_CST)) {
    g_jit_retired_ctx = ctx;   /* its threads may still be running in it */
  } else {
    if (g_jit_gen_inited) MIR_gen_finish (ctx);
    c2mir_finish (ctx);
    MIR_finish (ctx);
  }
  prof_exit (PROF_JIT);

  if (prog_rc) *prog_rc = box.rc;
  return TUR_JIT_OK;
}

/* ------------------------------------------------------------------ */
/* J2: persistent image mode (plan section 3.3)                        */
/* ------------------------------------------------------------------ */
struct TurJitImage {
  MIR_context_t ctx;
  int c2mir_live;   /* c2mir_finish still owed (TUR_JIT_KEEP_C2MIR=1) */
};

/* aot-compiled-repl-plan C1: an image no longer needs the C front end once its
 * module is loaded, linked and initialized -- generation (lazy or eager) reads
 * MIR's own IR, and MIR copied every name and literal it kept.  The front end
 * is the bulk of an image's resident memory (the parsed system headers and the
 * runtime declarations), and the REPL keeps images for the life of the
 * session, so it is released here rather than in tur_jit_image_free.
 * TUR_JIT_KEEP_C2MIR=1 restores the old lifetime, for bisecting. */
static int jit_image_keeps_c2mir (void) {
  const char *v = getenv ("TUR_JIT_KEEP_C2MIR");
  return v != NULL && strcmp (v, "1") == 0;
}

int tur_jit_compile_image (const char *csrc, size_t csrc_len,
                           const char *autolink,
                           const char **include_dirs, int n_include_dirs,
                           TurJitImage **out) {
  *out = NULL;
  /* atexit interception note: the list is engine-global.  Image-registered
   * handlers (module defers) accumulate and are drained when the image is
   * freed, while the generated code is still mapped -- approximate for
   * overlapping images (a reload window), exact for the common one-image
   * session.  A per-image list is J3 work if it ever matters. */
  MIR_context_t ctx;
  int frc = jit_compile_and_link (csrc, csrc_len, autolink,
                                  include_dirs, n_include_dirs, &ctx);
  if (frc != TUR_JIT_OK) return frc;

  /* S1b, applied to the no-main case: c2mir discards the constructor
   * attribute, so the wrapper never fires and module state (dynvar keys,
   * module defs, interned symbols, direct->CPS registry) stays
   * uninitialized.  Call the explicit initializer by name -- it is static
   * in the TU, which MIR item lookup sees fine, and idempotent by
   * construction. */
  MIR_item_t init = jit_find_func (ctx, "__tur_static_init");
  if (init != NULL && init->addr != NULL) ((void (*) (void)) init->addr) ();

  int keep_c2mir = jit_image_keeps_c2mir ();
  if (!keep_c2mir) c2mir_finish (ctx);

  TurJitImage *img = (TurJitImage *) malloc (sizeof *img);
  if (!img) {
    jit_forget_lazy_ctx (ctx);
    if (g_jit_gen_inited) MIR_gen_finish (ctx);
    if (keep_c2mir) c2mir_finish (ctx);
    MIR_finish (ctx);
    return TUR_JIT_ERR_RUN;
  }
  img->ctx = ctx;
  img->c2mir_live = keep_c2mir;
  *out = img;
  return TUR_JIT_OK;
}

void *tur_jit_image_sym (TurJitImage *img, const char *name) {
  if (!img) return NULL;
  MIR_item_t it = jit_find_func (img->ctx, name);
  return it ? it->addr : NULL;
}

void tur_jit_image_free (TurJitImage *img) {
  if (!img) return;
  /* Drain pending image atexit handlers (module defers) while the
   * generated code is still mapped; the callers' contract is that no
   * image function pointer is used after this call. */
  jit_atexit_drain ();
  jit_forget_lazy_ctx (img->ctx);
  MIR_gen_finish (img->ctx);
  if (img->c2mir_live) c2mir_finish (img->ctx);
  MIR_finish (img->ctx);
  free (img);
}
