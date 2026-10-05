#include "arena.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define DEFAULT_SLAB (64 * 1024)

struct ArenaSlab {
    ArenaSlab *next;
    size_t cap;
    size_t used;
    /* data[cap] follows inline */
    unsigned char data[];
};

/* ---- ASan-aware debug poisoning (docs/archive/history/arena-debug-poisoning-plan.md)
 *
 * The bump arena is invisible to ASan at sub-allocation granularity: a stale
 * pointer into a reset arena reads still-mapped garbage, and a stale pointer
 * into a FREED arena usually aliases the next compile's reallocated slabs
 * (malloc reuses the addresses), corrupting live data with no report.  In a
 * Debug+ASan build we close both holes:
 *
 *   - reset/free POISON the reclaimed bytes with the real ASan interface, so a
 *     straggler deref traps as use-after-poison AT the deref;
 *   - free QUARANTINES the slabs (poisoned, chained off a global so LSan still
 *     sees them as reachable) instead of returning them to malloc, so freed
 *     arena addresses are never reused within the process and the poison
 *     cannot be undone by a later malloc.
 *
 * Opt out with TUR_DEBUG_ARENA_POISON=0 (e.g. for a long-lived Debug REPL
 * where the quarantine's bounded, deliberate retention is unwanted).  The
 * whole mechanism compiles away in Release / non-ASan builds. */
#if defined(__has_feature)
# if __has_feature(address_sanitizer)
#  define TUR_ARENA_ASAN 1
# endif
#elif defined(__SANITIZE_ADDRESS__)
# define TUR_ARENA_ASAN 1
#endif

/* Guard-page mode (plan phase AP4): TUR_DEBUG_ARENA_GUARD=1 in a Debug build
 * backs every slab with its own mmap and mprotect(PROT_NONE)s it on
 * arena_free instead of returning it to malloc.  Any later access through a
 * stale pointer into a freed arena is then a hard SIGSEGV AT THE DEREF, with
 * the faulting address still inside the old slab -- the strongest form of
 * the diagnostic, and the one that works when ASan's own allocator perturbs
 * the layout enough to hide the bug.  The protected regions are retained for
 * the life of the process (bounded, deliberate).  Opt-in only.
 *
 * Unavailable on Windows, hence TUR_ARENA_GUARD rather than a bare NDEBUG
 * test: the mode needs mprotect(PROT_NONE) to retire a mapping in place, and
 * platform_mman.h deliberately emulates only anonymous mmap/munmap -- it
 * declines mprotect rather than fake it (see the header's comment).  Since
 * this is an opt-in diagnostic and not a correctness feature, the Windows
 * build simply loses it and keeps the malloc path; TUR_DEBUG_ARENA_GUARD=1
 * is a no-op there. */
#if !defined(NDEBUG) && !defined(_WIN32)
#define TUR_ARENA_GUARD 1
#include <sys/mman.h>
#include <unistd.h>

static int arena_guard_mode(void) {
    static int mode = -1;
    if (mode < 0) {
        const char *e = getenv("TUR_DEBUG_ARENA_GUARD");
        mode = (e && e[0] == '1' && e[1] == '\0');
    }
    return mode;
}
#endif

#if defined(TUR_ARENA_ASAN) && !defined(NDEBUG)
#include <sanitizer/asan_interface.h>

static int arena_poison_mode(void) {
    static int mode = -1;
    if (mode < 0) {
        const char *e = getenv("TUR_DEBUG_ARENA_POISON");
        mode = !(e && e[0] == '0' && e[1] == '\0');
    }
    return mode;
}

/* Freed-slab quarantine.  Chained via the slabs' own next pointers and rooted
 * in a global so the memory stays reachable (LSan-clean) while its addresses
 * stay out of malloc circulation for the life of the process. */
static ArenaSlab *g_arena_quarantine = NULL;

static void arena_dbg_unpoison(void *p, size_t n) {
    if (arena_poison_mode()) __asan_unpoison_memory_region(p, n);
}
#else
static void arena_dbg_unpoison(void *p, size_t n) { (void)p; (void)n; }
#endif

static void oom(void) {
    fprintf(stderr, "tur: out of memory\n");
    abort();
}

static ArenaSlab *slab_new(size_t cap) {
#ifdef TUR_ARENA_GUARD
    if (arena_guard_mode()) {
        /* Page-rounded private mapping so arena_free can mprotect it whole.
         * cap absorbs the rounding slack (header + cap == mapping exactly). */
        size_t page  = (size_t)sysconf(_SC_PAGESIZE);
        size_t total = (sizeof(ArenaSlab) + cap + page - 1) & ~(page - 1);
        ArenaSlab *s = (ArenaSlab *)mmap(NULL, total, PROT_READ | PROT_WRITE,
                                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (s == MAP_FAILED) oom();
        s->next = NULL;
        s->cap = total - sizeof(ArenaSlab);
        s->used = 0;
        return s;
    }
#endif
    ArenaSlab *s = (ArenaSlab *)malloc(sizeof(ArenaSlab) + cap);
    if (!s) oom();
    s->next = NULL;
    s->cap = cap;
    s->used = 0;
    return s;
}

TUR_RT_API void arena_init(Arena *a, size_t default_slab_size) {
    a->head = NULL;
    a->spare = NULL;
    a->default_slab = default_slab_size ? default_slab_size : DEFAULT_SLAB;
    a->total_bytes = 0;
    a->total_allocs = 0;
}

static size_t align_up(size_t n, size_t align) {
    assert(align && (align & (align - 1)) == 0);
    return (n + (align - 1)) & ~(align - 1);
}

TUR_RT_API void *arena_alloc_aligned(Arena *a, size_t size, size_t align) {
    if (size == 0) size = 1;
    if (align < sizeof(void *)) align = sizeof(void *);

    /* Align the ABSOLUTE address, not the offset: the slab's data[] field sits
     * at a non-trivial offset past the ArenaSlab header (e.g. 24 bytes), so a
     * 16-byte-aligned malloc base leaves data[] only 8-byte aligned.  Aligning
     * s->used alone would therefore never satisfy align > 8 (a TuriFiber leads
     * with a ucontext_t needing 16-byte alignment -- see turi/fiber.c).  Round
     * the real pointer up instead so any power-of-two alignment is honored. */
    ArenaSlab *s = a->head;
    if (s) {
        uintptr_t base = (uintptr_t)s->data;
        size_t aligned_used = align_up(base + s->used, align) - base;
        if (aligned_used + size <= s->cap) {
            void *p = s->data + aligned_used;
            s->used = aligned_used + size;
            a->total_bytes += size;
            a->total_allocs++;
            arena_dbg_unpoison(p, size);
            return p;
        }
    }

    /* Need another slab.  Reuse one a reset emptied when it is big enough --
     * otherwise a rewound arena refills only its old head slab and mallocs the
     * rest afresh on every generation, so a region rewound in a loop grows
     * without bound.  Else grow: the extra `align` bytes cover worst-case
     * alignment padding at the head of a fresh slab. */
    ArenaSlab *fresh;
    if (a->spare && a->spare->cap >= size + align) {
        fresh = a->spare;
        a->spare = fresh->next;
    } else {
        size_t cap = a->default_slab;
        if (size + align > cap) cap = size + align;
        fresh = slab_new(cap);
    }
    fresh->next = a->head;
    /* The link before the publication, as a signal handler on this thread
     * sees it: the r7rs-gc collector stops a thread anywhere and then walks
     * its region generations (r7gc.c, tur_region_each_registered), and a
     * head published ahead of its link would hide every older slab. */
    atomic_signal_fence(memory_order_seq_cst);
    a->head = fresh;

    uintptr_t base = (uintptr_t)fresh->data;
    size_t aligned_used = align_up(base, align) - base;
    void *p = fresh->data + aligned_used;
    fresh->used = aligned_used + size;
    a->total_bytes += size;
    a->total_allocs++;
    arena_dbg_unpoison(p, size);
    return p;
}

TUR_RT_API void *arena_alloc(Arena *a, size_t size) {
    return arena_alloc_aligned(a, size, sizeof(void *));
}

TUR_RT_API char *arena_strdup(Arena *a, const char *s, size_t len) {
    char *p = (char *)arena_alloc_aligned(a, len + 1, 1);
    if (len) memcpy(p, s, len);
    p[len] = '\0';
    return p;
}

static void free_slabs(ArenaSlab *s) {
    while (s) {
        ArenaSlab *next = s->next;
#ifdef TUR_ARENA_GUARD
        if (arena_guard_mode()) {
            /* Retire the whole mapping: any stale pointer into this arena now
             * faults at the deref.  Never unmapped, so the address range is
             * never recycled into a later arena. */
            mprotect(s, sizeof(ArenaSlab) + s->cap, PROT_NONE);
            s = next;
            continue;
        }
#endif
#if defined(TUR_ARENA_ASAN) && !defined(NDEBUG)
        if (arena_poison_mode()) {
            /* Quarantine instead of free: poison the payload and park the slab
             * (header included) so its addresses are never handed out again by
             * malloc.  A stale cross-arena pointer then traps at the deref
             * instead of silently aliasing the next arena's live data. */
            __asan_poison_memory_region(s->data, s->cap);
            s->next = g_arena_quarantine;
            g_arena_quarantine = s;
        } else {
            free(s);
        }
#else
        free(s);
#endif
        s = next;
    }
}

TUR_RT_API void arena_free(Arena *a) {
    free_slabs(a->head);
    free_slabs(a->spare);
    a->head = NULL;
    a->spare = NULL;
    a->total_bytes = 0;
    a->total_allocs = 0;
}

/* Poison byte for reclaimed scratch memory; 0xDE reads back as an obviously-bad
 * pointer (0xDEDEDEDE...) if a straggler is dereferenced. */
#define ARENA_POISON 0xDE

TUR_RT_API void arena_reset(Arena *a) {
    for (ArenaSlab *s = a->head; s; s = s->next) {
#ifndef NDEBUG
        /* Poison the bytes we are about to hand out again so a missed pointer
         * into the rewound region crashes loudly instead of reading stale data. */
        if (s->used) {
#if defined(TUR_ARENA_ASAN)
            if (arena_poison_mode()) {
                /* The used region is a patchwork of unpoisoned allocations and
                 * still-poisoned alignment gaps from earlier generations, so
                 * lift the poison before the memset can trip over a gap, then
                 * re-poison the whole span with the real ASan interface: a
                 * straggler deref now traps as use-after-poison at the deref
                 * (arena_alloc unpoisons ranges as they are handed back out). */
                __asan_unpoison_memory_region(s->data, s->used);
                memset(s->data, ARENA_POISON, s->used);
                __asan_poison_memory_region(s->data, s->used);
            } else {
                memset(s->data, ARENA_POISON, s->used);
            }
#else
            memset(s->data, ARENA_POISON, s->used);
#endif
        }
#endif /* !NDEBUG */
        s->used = 0;
    }
    /* Keep the head slab where allocations land and park the rest on the
     * spare list (in their existing order, ahead of any older spares), so the
     * next generation refills them before calling malloc.  The head's link is
     * cut after the spare list holds the tail: a signal handler walking the
     * head chain (r7gc) only ever sees empty slabs drop out of it. */
    if (a->head && a->head->next) {
        ArenaSlab *tail = a->head->next;
        ArenaSlab *last = tail;
        while (last->next) last = last->next;
        last->next = a->spare;
        a->spare = tail;
        atomic_signal_fence(memory_order_seq_cst);
        a->head->next = NULL;
    }
    a->total_bytes = 0;
    a->total_allocs = 0;
}

TUR_RT_API void arena_each_used(const Arena *a,
                                void (*cb)(const void *p, size_t n, void *ud),
                                void *ud) {
    for (const ArenaSlab *s = a->head; s; s = s->next)
        if (s->used) cb(s->data, s->used, ud);
}

TUR_RT_API bool arena_owns(const Arena *a, const void *p) {
    if (!p) return false;
    const unsigned char *cp = (const unsigned char *)p;
    for (const ArenaSlab *s = a->head; s; s = s->next) {
        if (cp >= s->data && cp < s->data + s->cap) return true;
    }
    for (const ArenaSlab *s = a->spare; s; s = s->next) {
        if (cp >= s->data && cp < s->data + s->cap) return true;
    }
    return false;
}
