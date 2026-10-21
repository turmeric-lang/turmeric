#ifndef TUR_REGION_H
#define TUR_REGION_H
/* region.h -- declared lifetimes (RM3), over the Arena that already ships.
 *
 * The reclamation plan's third phase.  RM1 frees a box whose owner is a scope;
 * RM2 would free a node whose owner is a value, and cannot, because a
 * persistent structure's nodes have no unique owner -- `(SBind v t rest)`
 * shares `rest` with every older chain, so "is this the last reference?" is a
 * runtime fact no static rule answers.
 *
 * A region does not ask who owns a node.  It asks WHEN THE GENERATION DIES.
 * Every node allocated between a push and its matching pop dies at the pop, in
 * one O(slabs) rewind, with no per-node bookkeeping at all.
 *
 * See docs/archive/regions-plan.md.  ON BY DEFAULT since graduation out of
 * `--enable=regions` on 2026-09-05 (g_opt_regions, default true); TUR_REGIONS=0
 * is the bisection hatch that restores the pre-graduation build.
 *
 * THE SAFETY RULE, and it is the whole design:
 *
 *   A region that cannot prove every escaping value relocatable does not
 *   rewind.
 *
 * There are no partial rewinds and no best-effort.  A shape the escape check
 * cannot handle costs a SAVING, never correctness -- the same discipline
 * turi's value-pool promotion walk runs on ("a missed shape means this eval
 * does not shrink, never use-after-reset").  A pop rewinds only through
 * `tur_region_pop_checked`, after both locks pass (the static walk over the
 * bracket's result type, and the runtime escape note below); `tur_region_pop`
 * is what the emitter spells when the static walk refuses, and on the catch
 * boundary, and it never rewinds.
 *
 * The Debug arena poisons reclaimed bytes, so a value that outlives its region
 * crashes loudly under ASan rather than reading stale-but-mapped data.  That
 * backstop is part of why regions could graduate to on by default; do not
 * disable it. */
#include <stdbool.h>
#include <stddef.h>

/* Linkage qualifier for the public surface.  Empty when this is compiled as
 * an ordinary runtime TU (the compiler itself, libturt_runtime.a).  The
 * emitter pastes this header and region.c verbatim into an emitted program
 * that does not link the archive (src/compiler/emit_module.c,
 * emit_region_runtime_bodies); in a --shared build it defines this as
 * TUR_RT_LOCAL (hidden visibility) first, so a .so keeps its generation stack
 * to itself -- the same reasoning DEDUP-5 applied to the GC registry. */
#ifndef TUR_RT_API
#define TUR_RT_API
#endif

/* Push a new generation.  Returns the depth, which `tur_region_pop*` takes
 * back so a mismatched pair is caught rather than silently rewinding someone
 * else's generation.
 *
 * The generation stack is PER-THREAD (see region.c): a bracket opened on one
 * thread is popped on that thread, and a worker thread's allocations never
 * land in a generation the main thread has open.  Ownership (`tur_region_owns`,
 * and so `tur_region_free`) is still process-wide. */
TUR_RT_API int  tur_region_push(void);

/* Pop WITHOUT reclaiming: the generation's memory stays live for the process.
 * What the emitter spells when the static walk refuses the bracket's result
 * type, and on the catch boundary -- correctness with no saving, which is the
 * safe half of the rule above.
 *
 * Every pop tolerates a stack DEEPER than `depth`: the generations above it
 * had their pops skipped (a panic unwound through their brackets), and they
 * are retired -- never rewound -- on the way down, so the stack cannot stay
 * jammed on a stale innermost generation.  A `depth` deeper than the stack is
 * a spent handle and a no-op. */
TUR_RT_API void tur_region_pop(int depth);

/* --- R3: the escape check ---------------------------------------------- *
 *
 * Note that `p` crosses out of the innermost generation.  A value that is NOT
 * region memory (a malloc'd box, a static, a scalar cast to a pointer) is
 * recorded and harmless; one that IS marks the generation non-reclaimable.
 *
 * WHAT THIS DOES AND DOES NOT ESTABLISH, because the difference is the whole
 * safety argument:
 *
 *   It proves the escaping pointer ITSELF does not point into the generation.
 *   It proves nothing about what that pointer TRANSITIVELY reaches -- a
 *   malloc'd struct whose field points at a region node passes this check and
 *   would dangle after a rewind.
 *
 * So this is the SECOND lock, not the first.  The first is static: the region
 * form only reclaims when its result TYPE cannot transitively reach a
 * region-allocated node, which is a compile-time question with a decidable
 * conservative answer.  This runtime check catches the direct case cheaply and
 * makes a static mistake loud rather than silent.  Neither alone is the
 * argument; do not remove one on the strength of the other.
 *
 * The same note is the STORE-SIDE lock (region-lock-hardening, 2026-09-06):
 * every primitive that writes a caller's word into memory that can outlive
 * the bracket -- vec-push!, a map insert, a cell set, a closure capture, a
 * heap-boxed constructor field, an erasing `(:: node :int)` -- notes the word
 * through the emitted `TUR_REGION_NOTE` macro.  A store is an escape the
 * result walk cannot see, and before this it rewound a generation an outer
 * container still pointed into.  The note flags whichever LIVE generation
 * owns `p`, not only the innermost: a store fired inside a nested bracket for
 * a value the outer generation allocated must block the OUTER rewind. */
TUR_RT_API void tur_region_note_escape(const void *p);

/* Note every aligned word of `n` bytes at `p` -- a by-value aggregate crossing
 * a boundary or being boxed.  A scalar word compared as an address is simply
 * not region memory; an erased pointer field is, and flags its generation. */
TUR_RT_API void tur_region_note_escape_words(const void *p, size_t n);

/* Pop, reclaiming ONLY if no noted escape pointed into the generation.
 * Returns true when it reclaimed, false when it retired instead -- the caller
 * can use that to report whether a region paid for itself.
 *
 * Reclaiming REWINDS rather than releases: the arena is reset (which poisons
 * its bytes in a Debug build, so a straggler traps at the deref under ASan
 * instead of reading stale data) and kept for the next push.  A per-query
 * region in a loop therefore allocates its slabs once. */
TUR_RT_API bool tur_region_pop_checked(int depth);

/* Allocate `n` bytes in the innermost live generation.  Returns NULL when no
 * region is open, which every caller must treat as "use malloc instead" -- a
 * region is an optimisation, never a requirement. */
TUR_RT_API void *tur_region_alloc(size_t n);

/* Allocate `n` bytes in the innermost live generation if one is open, and from
 * the heap otherwise.  The routing point for RM3 R2: a spine-node constructor
 * calls this instead of `malloc`, so the SAME emitted code allocates by
 * generation inside a region and exactly as it does today outside one.
 *
 * A caller must therefore never assume the result is region memory -- pair it
 * with `tur_region_owns` before any `free()`.  That guard is the whole reason
 * the two allocators can share one call site. */
TUR_RT_API void *tur_region_alloc_or_malloc(size_t n);

/* True when `p` points into any live generation.  The guard the reclamation
 * plan requires on every free path: a pointer into region memory must never
 * reach `free()`, because the slab, not the pointer, owns it. */
TUR_RT_API bool tur_region_owns(const void *p);

/* `free(p)` unless `p` is region memory, in which case the generation owns it
 * and the pop reclaims it.
 *
 * R4 makes this load-bearing rather than advisory.  R2 routed the spine-node
 * constructor through `tur_region_alloc_or_malloc`, so from R2 onward a node
 * inside a region is arena memory while the SAME emitted drop glue still ends
 * in `free(ptr)` -- an allocator mismatch that aborts, not a leak.  Every node
 * free path therefore spells this instead of `free` when regions are enabled,
 * exactly mirroring the allocation side's one-call-site routing.
 *
 * A NULL is a no-op, like `free`. */
TUR_RT_API void tur_region_free(void *p);

/* True when at least one generation is open (on this thread). */
TUR_RT_API bool tur_region_active(void);

/* Number of generations open on this thread.  A test / probe surface: a
 * bracket that did not pop shows up as a depth that never returns to 0. */
TUR_RT_API int  tur_region_depth(void);

/* Release every generation and the backing arena.  Process teardown only.
 * Under TUR_REGION_STATS=1 it first prints `region-stats: pushes=N
 * rewinds=N retires=N` to stderr -- the instrument tests/regions-fuzz-src.py
 * checks its rewind / retire model against, so a lock that quietly became a
 * blanket refusal shows up as a savings regression rather than as nothing. */
TUR_RT_API void tur_region_shutdown(void);

/* Call `cb` on the used memory of every live and retired generation on this
 * thread (a pooled, rewound one is dead memory and is skipped).  The r7rs
 * collector reads these as roots: a node built in a bracket can point at an
 * object on the collected heap. */
TUR_RT_API void tur_region_each_used(void (*cb)(const void *p, size_t n, void *ud),
                                     void *ud);

/* The same over every thread's generations (the ownership registry), for a
 * collector that has stopped the other threads.  The lock is a spinlock a
 * stopped thread may hold, so the collector tries it and walks under it. */
TUR_RT_API bool tur_region_registry_trylock(void);
TUR_RT_API void tur_region_registry_unlock(void);
TUR_RT_API void tur_region_each_registered(void (*cb)(const void *p, size_t n, void *ud),
                                           void *ud);
TUR_RT_API void tur_region_each_used_all(void (*cb)(const void *p, size_t n, void *ud),
                                         void *ud);

#endif
