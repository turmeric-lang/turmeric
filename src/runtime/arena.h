#ifndef TUR_ARENA_H
#define TUR_ARENA_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Linkage qualifier -- see region.h.  Empty in an ordinary runtime TU; the
 * emitter defines it before pasting this header into an emitted program. */
#ifndef TUR_RT_API
#define TUR_RT_API
#endif

typedef struct ArenaSlab ArenaSlab;

typedef struct Arena {
    ArenaSlab *head;
    /* Slabs emptied by arena_reset, waiting to be reused.  The head chain is
     * where allocations land and what arena_each_used walks; a reset keeps
     * the head slab there and parks the rest here, so the next generation
     * refills them before it asks malloc for more. */
    ArenaSlab *spare;
    size_t default_slab;
    size_t total_bytes;
    size_t total_allocs;
} Arena;

TUR_RT_API void  arena_init(Arena *a, size_t default_slab_size);
TUR_RT_API void *arena_alloc(Arena *a, size_t size);
TUR_RT_API void *arena_alloc_aligned(Arena *a, size_t size, size_t align);
TUR_RT_API char *arena_strdup(Arena *a, const char *s, size_t len);
TUR_RT_API void  arena_free(Arena *a);

/* Rewind every slab to empty without releasing the backing memory, so the arena
 * can be reused for a fresh generation of allocations in O(slabs).  This is the
 * scratch-region reset primitive for the turi value-pool scratch/permanent split
 * (turi-value-pool-scratch-promotion-plan): a long-lived TuriEnv rewinds its
 * scratch pool at each top-level eval boundary after promoting escapees.
 *
 * In a Debug build (NDEBUG undefined) the reclaimed bytes are overwritten with a
 * poison pattern first, so any pointer that survived into the rewound region and
 * is dereferenced afterwards crashes loudly under ASan instead of reading stale
 * (but still-mapped) data -- the "poison-on-reset debug mode" the plan calls for. */
TUR_RT_API void  arena_reset(Arena *a);

/* True when p points into any slab currently owned by a.  Used by the promotion
 * walk to decide whether a payload pointer is scratch-allocated (copy + forward)
 * or lives elsewhere -- permanent pool, eval arenas, sym arena, static data --
 * and must be left untouched.  O(slabs). */
TUR_RT_API bool  arena_owns(const Arena *a, const void *p);

/* Call `cb` on the used bytes of every slab -- the memory a conservative
 * collector must read as roots (the r7rs-gc experiment scans region memory
 * this way, since an object built in a region can point into its heap). */
TUR_RT_API void  arena_each_used(const Arena *a,
                                 void (*cb)(const void *p, size_t n, void *ud),
                                 void *ud);

#endif
