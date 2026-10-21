/* region.c -- declared lifetimes (RM3).  See region.h for the design and
 * docs/archive/regions-plan.md for the phase.
 *
 * One Arena per generation rather than watermarks into a shared one.  Arena
 * exposes `arena_reset` (whole-arena rewind) and `arena_owns`, not a
 * save/restore watermark, so a per-generation Arena is what the existing API
 * supports without changing it -- and it makes `tur_region_owns` a walk over
 * the live generations instead of an address-range comparison that would have
 * to know about interleaving.  The retired ones, which accumulate, are
 * indexed by address (g_ridx below); a retired arena never changes, so that
 * index never has to.
 *
 * A generation popped WITHOUT reclaim is retired, not freed: its memory stays
 * mapped for the process.  That is deliberately the status quo (the spine
 * leaks today), so the conservative path costs nothing new, and it keeps
 * `tur_region_owns` true for those pointers -- a value that outlived its
 * generation must still never reach `free()`.
 *
 * THREADS (region-lock-hardening, 2026-09-06).  The generation stack is
 * PER-THREAD (`__thread`, exactly as src/runtime/trail.c keeps the trail and
 * tur_tls.c keeps the panic state): a bracket is a stack discipline over one
 * thread's control flow, and `bt-scope` already pushes a per-thread trail
 * level, so its generation belongs to the same thread.  With one process-wide
 * stack, a worker thread's spine nodes landed in whatever generation the main
 * thread happened to have open -- through an unsynchronised push_ptr/realloc
 * -- and were rewound under the worker when that bracket closed.
 *
 * What does NOT follow the thread is OWNERSHIP.  A node allocated in thread
 * A's generation and dropped by thread B must still never reach free(), so
 * `tur_region_owns` consults a process-wide registry of every arena that is
 * live or retired in ANY thread.  The registry is touched under a spinlock at
 * push, retire and reclaim (once per bracket, never per allocation), and the
 * ownership query skips the lock entirely while every registered arena is
 * this thread's own -- which is every single-threaded program, and every
 * multi-threaded one in which only one thread has ever opened a region. */
#include "region.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arena.h"

#define TUR_REGION_SLAB (64u * 1024u)

/* Live generations, innermost last.  Depth is 1-based so 0 can mean "none". */
static __thread Arena  **g_live;
static __thread int      g_live_n;
static __thread int      g_live_cap;

/* Generations popped without reclaim.  Never freed before shutdown -- see the
 * file comment for why they are kept rather than released. */
static __thread Arena  **g_retired;
static __thread int      g_retired_n;
static __thread int      g_retired_cap;

/* The retired generations again, indexed by address: a slot per 64 KiB block
 * each retired slab overlaps, in an open-addressing table.  Every node free
 * asks tur_region_owns (regions are on by default, so the drop glue frees
 * through tur_region_free), and it used to walk g_retired -- so in a program
 * that keeps retiring, every free got slower for the life of the process
 * (performance-audit R-3).  Exact rather than approximate: a retired arena
 * never allocates again, so its slabs never change after they are indexed,
 * and a hit is confirmed with arena_owns.  If the table cannot grow, it is
 * marked broken and the query walks g_retired as before. */
typedef struct { uintptr_t blk; Arena *a; } RetiredSlot;
#define TUR_REGION_BLK_SHIFT 16
static __thread RetiredSlot *g_ridx;
static __thread size_t       g_ridx_cap;    /* a power of two; 0 = none yet */
static __thread size_t       g_ridx_n;
static __thread bool         g_ridx_broken;

/* Reclaimed generations, reset and available for the next push.  Reclaiming
 * REWINDS rather than releases: arena_reset keeps the slabs (and poisons them
 * in a Debug build), so a per-query region inside a loop pays for its slabs
 * once instead of per iteration.  A pooled arena is dead memory -- deliberately
 * NOT reported by tur_region_owns, which answers about live and retired
 * generations only. */
static __thread Arena  **g_pool;
static __thread int      g_pool_n;
static __thread int      g_pool_cap;

/* R3: set on a generation when a noted escape points into it.  Parallel to
 * g_live, one flag per open generation.  Sticky: once a generation has leaked
 * a pointer it can never be reclaimed, however many safe escapes follow. */
static __thread bool    *g_escaped;
static __thread int      g_escaped_cap;

/* Cross-thread ownership registry: every arena that is live or retired in any
 * thread.  `g_reg_total` counts them (atomic, read lock-free); `g_my_reg`
 * counts the ones this thread registered.  While the two agree, every
 * registered arena is ours and the per-thread walk is the whole answer. */
static Arena         **g_reg;
static int             g_reg_n;
static int             g_reg_cap;
static atomic_int      g_reg_total;
static atomic_flag     g_reg_lock = ATOMIC_FLAG_INIT;
static __thread int    g_my_reg;

static void reg_lock(void)   { while (atomic_flag_test_and_set_explicit(&g_reg_lock, memory_order_acquire)) { } }
static void reg_unlock(void) { atomic_flag_clear_explicit(&g_reg_lock, memory_order_release); }

/* TUR_REGION_STATS=1: count pushes, rewinds and retires (process-wide,
 * atomic) and print them at shutdown, so a harness can tell a region that
 * paid for itself from one that only ever retired.  The regions fuzzer
 * (tests/regions-fuzz-src.py) predicts these per program: a bracket with no
 * escape MUST rewind, or the lock has become a blanket refusal -- which is
 * safe, and exactly the regression a safety-only check would never see. */
static atomic_int g_stat_push, g_stat_rewind, g_stat_retire;
static int g_stats_enabled = -1;   /* -1: not read yet */

static bool stats_enabled(void) {
    if (g_stats_enabled < 0) {
        const char *e = getenv("TUR_REGION_STATS");
        g_stats_enabled = (e && e[0] == '1') ? 1 : 0;
    }
    return g_stats_enabled == 1;
}

/* Thread exit.  A worker that opened a region owns three small arrays and a
 * pool of reset arenas that nothing else references; a pthread key destructor
 * releases them when the thread ends (the main thread's go through
 * tur_region_shutdown at atexit instead).  Live generations it abandoned are
 * retired -- something may still point into them -- and retired ones stay
 * registered, because a node that crossed to another thread still needs
 * `tur_region_owns` to answer for it. */
static pthread_key_t   g_tls_key;
static pthread_once_t  g_tls_once = PTHREAD_ONCE_INIT;
static __thread bool   g_tls_armed;

static void retire_top(void);

static void tls_thread_exit(void *unused) {
    (void)unused;
    while (g_live_n > 0) retire_top();
    for (int i = 0; i < g_pool_n; i++) { arena_free(g_pool[i]); free(g_pool[i]); }
    free(g_live);    g_live = NULL;    g_live_n = g_live_cap = 0;
    free(g_retired); g_retired = NULL; g_retired_n = g_retired_cap = 0;
    free(g_pool);    g_pool = NULL;    g_pool_n = g_pool_cap = 0;
    free(g_escaped); g_escaped = NULL; g_escaped_cap = 0;
    free(g_ridx);    g_ridx = NULL;    g_ridx_cap = g_ridx_n = 0; g_ridx_broken = false;
    g_tls_armed = false;
}

static void tls_key_init(void) { pthread_key_create(&g_tls_key, tls_thread_exit); }

static void tls_arm(void) {
    if (g_tls_armed) return;
    pthread_once(&g_tls_once, tls_key_init);
    pthread_setspecific(g_tls_key, (void *)1);   /* non-NULL: run the destructor */
    g_tls_armed = true;
}

static bool push_ptr(Arena ***vec, int *n, int *cap, Arena *a) {
    if (*n == *cap) {
        int nc = *cap ? *cap * 2 : 8;
        Arena **nv = (Arena **)realloc(*vec, (size_t)nc * sizeof(Arena *));
        if (!nv) return false;
        *vec = nv;
        *cap = nc;
    }
    (*vec)[(*n)++] = a;
    return true;
}

static void reg_add(Arena *a) {
    reg_lock();
    if (push_ptr(&g_reg, &g_reg_n, &g_reg_cap, a)) {
        atomic_fetch_add_explicit(&g_reg_total, 1, memory_order_relaxed);
        g_my_reg++;
    }
    /* A failed registration is conservative in the only direction that
     * matters: the per-thread walk still knows this arena, and another thread
     * asked about it gets "not owned" -- the same answer a registry could not
     * have given it before this change.  It cannot rewind anything. */
    reg_unlock();
}

/* From the END: brackets are LIFO, so the arena a reclaim hands back is almost
 * always among the last registered -- behind it sit only the inner
 * generations that retired since it was pushed -- and the swap-with-last
 * removal keeps the survivors in that order.  Scanning from index 0 instead
 * paid for every generation the program had ever retired, on every rewind
 * (performance-audit R-3). */
static void reg_remove(Arena *a) {
    reg_lock();
    for (int i = g_reg_n - 1; i >= 0; i--) {
        if (g_reg[i] == a) {
            g_reg[i] = g_reg[--g_reg_n];
            atomic_fetch_sub_explicit(&g_reg_total, 1, memory_order_relaxed);
            g_my_reg--;
            break;
        }
    }
    reg_unlock();
}

static int cmp_arena_ptr(const void *x, const void *y) {
    uintptr_t a = (uintptr_t)*(Arena *const *)x, b = (uintptr_t)*(Arena *const *)y;
    return a < b ? -1 : a > b;
}

/* Drop every registry entry for this thread's live and retired generations in
 * ONE pass.  Shutdown used to reg_remove each arena in turn, which is R x R/2
 * compares for R retirements -- 71% of a 100k-round program's cycles, all
 * after main returned (performance-audit R-3).  When every registered arena is
 * this thread's (every single-threaded program) the registry is simply
 * cleared; otherwise this thread's arenas are sorted and the registry filtered
 * against them.  Subtracts exactly what it removed, so the lock-free "all
 * mine" test in reg_owns_foreign stays true for the threads that remain. */
static void reg_remove_mine(void) {
    reg_lock();
    int removed = 0;
    if (atomic_load_explicit(&g_reg_total, memory_order_relaxed) == g_my_reg) {
        removed = g_reg_n;
        g_reg_n = 0;
    } else {
        int n = g_live_n + g_retired_n;
        Arena **own = n > 0 ? (Arena **)malloc((size_t)n * sizeof *own) : NULL;
        if (own) {
            memcpy(own, g_live, (size_t)g_live_n * sizeof *own);
            memcpy(own + g_live_n, g_retired, (size_t)g_retired_n * sizeof *own);
            qsort(own, (size_t)n, sizeof *own, cmp_arena_ptr);
            int k = 0;
            for (int i = 0; i < g_reg_n; i++) {
                if (bsearch(&g_reg[i], own, (size_t)n, sizeof *own, cmp_arena_ptr))
                    removed++;
                else
                    g_reg[k++] = g_reg[i];
            }
            g_reg_n = k;
            free(own);
        } else if (n > 0) {
            /* No memory for the sort: one removal each, as before. */
            reg_unlock();
            for (int i = 0; i < g_live_n; i++)    reg_remove(g_live[i]);
            for (int i = 0; i < g_retired_n; i++) reg_remove(g_retired[i]);
            return;
        }
    }
    atomic_fetch_sub_explicit(&g_reg_total, removed, memory_order_relaxed);
    g_my_reg -= removed;
    reg_unlock();
}

static bool reg_owns_foreign(const void *p) {
    /* Lock-free fast path: nothing registered that this thread does not
     * already answer for. */
    if (atomic_load_explicit(&g_reg_total, memory_order_acquire) == g_my_reg)
        return false;
    bool owned = false;
    reg_lock();
    for (int i = 0; i < g_reg_n && !owned; i++)
        owned = arena_owns(g_reg[i], p);
    reg_unlock();
    return owned;
}

TUR_RT_API int tur_region_push(void) {
    Arena *a;
    tls_arm();
    if (stats_enabled()) atomic_fetch_add_explicit(&g_stat_push, 1, memory_order_relaxed);
    if (g_pool_n > 0) {
        a = g_pool[--g_pool_n];    /* already reset by the reclaim that pooled it */
    } else {
        a = (Arena *)calloc(1, sizeof(Arena));
        if (!a) return 0;
        arena_init(a, TUR_REGION_SLAB);
    }
    if (!push_ptr(&g_live, &g_live_n, &g_live_cap, a)) {
        arena_free(a);
        free(a);
        return 0;
    }
    reg_add(a);
    /* A fresh generation has escaped nothing.  Grow the flag vector alongside
     * the live stack; a failure to grow it is treated as "escaped", so the
     * generation is never reclaimed on the strength of a flag we could not
     * store -- the conservative direction. */
    if (g_live_n > g_escaped_cap) {
        int nc = g_escaped_cap ? g_escaped_cap * 2 : 8;
        if (nc < g_live_n) nc = g_live_n;
        bool *nf = (bool *)realloc(g_escaped, (size_t)nc * sizeof(bool));
        if (!nf) return g_live_n;   /* flag missing -> read as escaped below */
        g_escaped = nf;
        g_escaped_cap = nc;
    }
    g_escaped[g_live_n - 1] = false;
    return g_live_n;   /* 1-based depth */
}

TUR_RT_API void tur_region_note_escape(const void *p) {
    if (g_live_n <= 0 || !p) return;
    /* Only an escape that IS region memory matters.  A malloc'd or static
     * pointer crossing the boundary is ordinary and blocks nothing -- which is
     * what makes this check worth having rather than a blanket refusal.
     *
     * Flag the generation that OWNS the pointer, not just the innermost one.
     * The result note at a pop only ever sees its own generation, but a store
     * hook (vec-push! into an outer container, a closure capture, ...) can
     * fire inside a nested bracket for a value the OUTER generation allocated:
     * that outer generation is the one that must not rewind, and it would
     * have, because its own flag was never set.  Innermost-first, so the
     * common case is one arena walk. */
    for (int i = g_live_n - 1; i >= 0; i--) {
        if (arena_owns(g_live[i], p)) {
            if (i < g_escaped_cap) g_escaped[i] = true;
            return;
        }
    }
}

TUR_RT_API void tur_region_note_escape_words(const void *p, size_t n) {
    if (g_live_n <= 0 || !p) return;
    /* Every aligned word of a by-value aggregate, read as a possible address.
     * A word that is not a pointer at all compares as an address and simply is
     * not region memory; a word that IS a region pointer -- an erased `:int`
     * field, a boxed element's box, a closure env -- flags its generation.
     * False positives refuse a rewind (the safe direction), never permit one. */
    const unsigned char *b = (const unsigned char *)p;
    for (size_t off = 0; off + sizeof(void *) <= n; off += sizeof(void *)) {
        void *w;
        memcpy(&w, b + off, sizeof w);
        tur_region_note_escape(w);
    }
}

/* Read the flag conservatively: a generation whose flag we could not store is
 * treated as having escaped. */
static bool generation_escaped(int idx0) {
    if (idx0 < 0) return true;
    if (idx0 >= g_escaped_cap) return true;
    return g_escaped[idx0];
}

static size_t ridx_hash(uintptr_t blk, size_t cap) {
    uint64_t h = (uint64_t)blk * UINT64_C(0x9E3779B97F4A7C15);
    return (size_t)(h ^ (h >> 29)) & (cap - 1);
}

static void ridx_put(RetiredSlot *t, size_t cap, uintptr_t blk, Arena *a) {
    size_t i = ridx_hash(blk, cap);
    while (t[i].a) i = (i + 1) & (cap - 1);
    t[i].blk = blk;
    t[i].a   = a;
}

static bool ridx_grow(void) {
    size_t nc = g_ridx_cap ? g_ridx_cap * 2 : 1024;
    RetiredSlot *nt = (RetiredSlot *)calloc(nc, sizeof *nt);
    if (!nt) return false;
    for (size_t i = 0; i < g_ridx_cap; i++)
        if (g_ridx[i].a) ridx_put(nt, nc, g_ridx[i].blk, g_ridx[i].a);
    free(g_ridx);
    g_ridx = nt;
    g_ridx_cap = nc;
    return true;
}

/* arena_each_slab callback: one slot per 64 KiB block the slab overlaps, kept
 * at most half full so every probe ends at an empty slot. */
static void ridx_add_slab(const void *data, size_t cap, void *ud) {
    if (g_ridx_broken || cap == 0) return;
    uintptr_t lo = (uintptr_t)data >> TUR_REGION_BLK_SHIFT;
    uintptr_t hi = ((uintptr_t)data + cap - 1) >> TUR_REGION_BLK_SHIFT;
    for (uintptr_t b = lo; b <= hi; b++) {
        if ((g_ridx_n + 1) * 2 > g_ridx_cap && !ridx_grow()) {
            g_ridx_broken = true;   /* incomplete from here on: walk instead */
            return;
        }
        ridx_put(g_ridx, g_ridx_cap, b, (Arena *)ud);
        g_ridx_n++;
    }
}

static bool ridx_owns(const void *p) {
    uintptr_t blk = (uintptr_t)p >> TUR_REGION_BLK_SHIFT;
    for (size_t i = ridx_hash(blk, g_ridx_cap); g_ridx[i].a; i = (i + 1) & (g_ridx_cap - 1))
        if (g_ridx[i].blk == blk && arena_owns(g_ridx[i].a, p)) return true;
    return false;
}

static void retire_top(void) {
    Arena *a = g_live[--g_live_n];
    if (stats_enabled()) atomic_fetch_add_explicit(&g_stat_retire, 1, memory_order_relaxed);
    if (!push_ptr(&g_retired, &g_retired_n, &g_retired_cap, a)) {
        /* Out of memory retiring it: the arena stays mapped and unreferenced,
         * which is the same outcome the retired list produces.  Never free it
         * -- something may still point in.  It stays in the registry, so a
         * drop path that reaches it still refuses to free(). */
        return;
    }
    arena_each_slab(a, ridx_add_slab, a);
}

/* Both pops take the depth back so a mismatched pair cannot rewind somebody
 * else's generation.
 *
 * A depth SHALLOWER than the stack means the deeper generations' pops were
 * skipped -- a panic that unwound through a bracket without running its pop.
 * Those generations are retired here (never rewound: something the unwinder
 * left behind may still point into them), so the stack does not stay jammed
 * with a stale innermost that every later allocation would land in and that
 * would refuse every outer pop.  A depth DEEPER than the stack is a spent
 * handle and a no-op.  Either way the failure stays "no saving", never
 * "reclaimed memory still in use". */
static Arena *detach(int depth) {
    if (depth <= 0 || depth > g_live_n) return NULL;
    while (g_live_n > depth) retire_top();
    return g_live[--g_live_n];
}

TUR_RT_API void tur_region_pop(int depth) {
    if (depth <= 0 || depth > g_live_n) return;
    while (g_live_n >= depth) retire_top();
}

/* Pop AND rewind, unconditionally: the step tur_region_pop_checked takes once
 * both locks have passed.  It used to be public as tur_region_pop_reclaim
 * ("the caller asserts nothing is still reachable"); nothing in the product
 * ever called it, and a caller's word is not the proof the safety rule asks
 * for, so it is the checked pop's private half now. */
static void region_pop_rewind(int depth) {
    Arena *a = detach(depth);
    if (!a) return;
    /* Rewind, do not release.  arena_reset keeps the slabs and, in a Debug
     * build, poisons them -- so a straggler traps at the deref under ASan
     * instead of reading stale bytes, and the next push reuses the memory
     * rather than asking the allocator again. */
    arena_reset(a);
    reg_remove(a);
    if (stats_enabled()) atomic_fetch_add_explicit(&g_stat_rewind, 1, memory_order_relaxed);
    if (!push_ptr(&g_pool, &g_pool_n, &g_pool_cap, a)) {
        arena_free(a);
        free(a);
    }
}

TUR_RT_API bool tur_region_pop_checked(int depth) {
    if (depth <= 0 || depth > g_live_n) return false;   /* spent handle: refuse */
    while (g_live_n > depth) retire_top();               /* skipped inner pops */
    if (generation_escaped(depth - 1)) {
        tur_region_pop(depth);       /* retire: correctness over saving */
        return false;
    }
    region_pop_rewind(depth);
    return true;
}

TUR_RT_API void *tur_region_alloc(size_t n) {
    if (g_live_n <= 0) return NULL;   /* no region open -- caller uses malloc */
    return arena_alloc(g_live[g_live_n - 1], n);
}

TUR_RT_API void *tur_region_alloc_or_malloc(size_t n) {
    void *p = tur_region_alloc(n);
    return p ? p : malloc(n);
}

TUR_RT_API bool tur_region_owns(const void *p) {
    if (!p) return false;
    for (int i = 0; i < g_live_n; i++)
        if (arena_owns(g_live[i], p)) return true;
    if (g_retired_n > 0) {
        if (g_ridx && !g_ridx_broken) {
            if (ridx_owns(p)) return true;
        } else {
            for (int i = 0; i < g_retired_n; i++)
                if (arena_owns(g_retired[i], p)) return true;
        }
    }
    return reg_owns_foreign(p);
}

TUR_RT_API void tur_region_free(void *p) {
    if (!p) return;
    /* Region memory is owned by the generation, not by this pointer: handing it
     * to free() is an allocator mismatch (glibc aborts), so the guard is not a
     * leak-avoidance nicety but the thing that keeps a region-allocated node
     * survivable at all once a drop path runs over it. */
    if (tur_region_owns(p)) return;
    free(p);
}

TUR_RT_API bool tur_region_active(void) { return g_live_n > 0; }

TUR_RT_API void tur_region_each_used(void (*cb)(const void *p, size_t n, void *ud),
                                     void *ud) {
    for (int i = 0; i < g_live_n; i++) arena_each_used(g_live[i], cb, ud);
    for (int i = 0; i < g_retired_n; i++) arena_each_used(g_retired[i], cb, ud);
}

/* Every thread's live and retired generations, through the ownership
 * registry.  For the r7rs-gc collector (docs/archive/r7rs-gc-threads-plan.md):
 * it stops every other thread before it reads roots, so the arenas hold
 * still; but a stopped thread may be inside reg_add/reg_remove holding the
 * registry's spinlock, so the collector TRIES the lock, and on a refusal
 * lets the world run and stops it again. */
TUR_RT_API bool tur_region_registry_trylock(void) {
    return !atomic_flag_test_and_set_explicit(&g_reg_lock, memory_order_acquire);
}
TUR_RT_API void tur_region_registry_unlock(void) { reg_unlock(); }
TUR_RT_API void tur_region_each_registered(void (*cb)(const void *p, size_t n, void *ud),
                                           void *ud) {
    for (int i = 0; i < g_reg_n; i++) arena_each_used(g_reg[i], cb, ud);
}
TUR_RT_API void tur_region_each_used_all(void (*cb)(const void *p, size_t n, void *ud),
                                         void *ud) {
    reg_lock();
    tur_region_each_registered(cb, ud);
    reg_unlock();
}

TUR_RT_API int tur_region_depth(void) { return g_live_n; }

TUR_RT_API void tur_region_shutdown(void) {
    if (stats_enabled())
        fprintf(stderr, "region-stats: pushes=%d rewinds=%d retires=%d\n",
                atomic_load(&g_stat_push), atomic_load(&g_stat_rewind),
                atomic_load(&g_stat_retire));
    /* This thread's arenas only: another thread's live or retired generations
     * are its own, and at process exit they go with the process.  Their
     * registry entries are dropped too, so a shutdown that follows every
     * thread's exit leaves nothing allocated.  The entries go first, in one
     * pass, so no other thread can find a freed arena in the registry. */
    reg_remove_mine();
    for (int i = 0; i < g_live_n; i++)    { arena_free(g_live[i]);    free(g_live[i]); }
    for (int i = 0; i < g_retired_n; i++) { arena_free(g_retired[i]); free(g_retired[i]); }
    for (int i = 0; i < g_pool_n; i++)    { arena_free(g_pool[i]);    free(g_pool[i]); }
    free(g_live);    g_live = NULL;    g_live_n = g_live_cap = 0;
    free(g_retired); g_retired = NULL; g_retired_n = g_retired_cap = 0;
    free(g_pool);    g_pool = NULL;    g_pool_n = g_pool_cap = 0;
    free(g_escaped); g_escaped = NULL; g_escaped_cap = 0;
    free(g_ridx);    g_ridx = NULL;    g_ridx_cap = g_ridx_n = 0; g_ridx_broken = false;
    g_tls_armed = false;
    reg_lock();
    if (g_reg_n == 0) { free(g_reg); g_reg = NULL; g_reg_cap = 0; }
    reg_unlock();
}
