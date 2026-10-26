/* ceiling.c -- price the fixes proposed in
 * docs/archive/multi-variant-adts-always-heap-allocate.md, before anyone
 * spends compiler time on them.
 *
 * RECONSTRUCTED 2026-08-25.  The original harness was written on 2026-08-22
 * (commit 00052f16) and never made it into the repository: .gitignore carries
 * a blanket `*.c` with negations for src/ tests/ examples/ docs/ tools/ but
 * NOT benchmarks/, so `git add` silently skipped it and the commit landed with
 * only the README.  Every published ratio -- by-value 1.8x, reclamation 2.6x,
 * slab 2.4x, both 18x -- came from a file nobody could re-run.
 *
 * This is rebuilt from the report's description and from the layouts the
 * compiler actually emits today (checked against `tur emit-c` for the same
 * `defdata Term` / `defdata Subst` in stdlib/logic.tur), NOT recovered from the
 * original.  Treat its numbers as a fresh measurement that happens to answer
 * the same question; where they disagree with the published ones, this file is
 * the one that can be re-run.
 *
 * Five representations of one workload: build an n-binding substitution, walk
 * all n, discard.  The checksum must match across all five -- that is what
 * stops a representation from looking fast by doing less.
 *
 *   A  boxed, leaked                      -- what the compiler emits today
 *   B  boxed, freed                       -- reclamation alone
 *   C  Term by value, spine boxed, leaked -- by-value alone (SR4's row)
 *   D  by value AND reclaimed             -- both
 *   E  boxed, leaked, slab-bump-allocated -- the shelved slab allocator
 *   F  by value, arena-reclaimed          -- both, with region reclamation
 *   G  boxed, arena-reclaimed             -- region reclamation ALONE, no ABI change
 *
 * Build and run (each representation in its OWN process -- A, C and E leak by
 * design, so running them in sequence lets whichever goes first hand its heap
 * to the rest; that produced a fake 8x "degradation" once already):
 *
 *   gcc -O2 -D_POSIX_C_SOURCE=200809L -o /tmp/ceiling \
 *       benchmarks/adt-alloc/ceiling.c -std=c99
 *   for rep in A B C D E F G; do /tmp/ceiling $rep 16000; done
 *
 * Output: rep,passes,ns_per_op,checksum
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#define NBIND 8          /* bindings per substitution, per the report's n=8 */

/* --------------------------------------------------------------------------
 * The layouts the compiler emits today, copied from `tur emit-c` output for
 * stdlib/logic.tur's `Term` and `Subst`.  Both are multi-variant, so both are
 * malloc'd on every construction and never freed.
 * -------------------------------------------------------------------------- */

typedef struct Term {
    int tag;                                     /* 0 TInt, 1 TVar, 2 TPair, 3 TNil */
    union {
        struct { int64_t _0; } TInt;
        struct { int64_t _0; } TVar;
        struct { int64_t _0; int64_t _1; } TPair;
        struct { int _unused; } TNil;
    } as;
} Term;

typedef struct Subst {
    int tag;                                     /* 0 SNil, 1 SBind */
    union {
        struct { int64_t _0; } SNil;
        struct { int64_t _0; int64_t _1; int64_t _2; } SBind;   /* var, Term*, Subst* */
    } as;
} Subst;

/* Rows C and D: the Term travels INSIDE the node; only the self-referential
 * `next` stays a pointer.  This is what SR4 proposes, and it is why 1.8x is
 * SR4's number rather than SR1's -- it removes one of the two allocations per
 * binding, not both. */
typedef struct VSubst {
    int tag;
    int64_t var;
    Term term;                                   /* by value */
    struct VSubst *next;                         /* spine still boxed */
} VSubst;

/* --------------------------------------------------------------------------
 * Row E's slab: a bump allocator over 256 KB chunks, never released.  Same
 * shape as the one the compiler emits behind TUR_ADT_SLAB=1
 * (src/compiler/emit_module.c), so the row prices that allocator and not a
 * nicer hypothetical one.
 * -------------------------------------------------------------------------- */

typedef struct Slab { struct Slab *next; size_t off; char buf[262144]; } Slab;
static Slab *g_slab = NULL;

static void *slab_alloc(size_t n) {
    n = (n + 15u) & ~(size_t)15u;
    if (n > sizeof(((Slab *)0)->buf)) return malloc(n);
    if (!g_slab || g_slab->off + n > sizeof(g_slab->buf)) {
        Slab *s = (Slab *)malloc(sizeof(Slab));
        if (!s) return malloc(n);
        s->next = g_slab; s->off = 0; g_slab = s;
    }
    void *p = g_slab->buf + g_slab->off;
    g_slab->off += n;
    return p;
}

/* --------------------------------------------------------------------------
 * Row F's arena: bump-allocate like the slab, but RESET it at the end of every
 * pass instead of never releasing.  This is the other candidate the SR plan
 * names for reclamation ("arena / drop glue"), and it is the row that explains
 * the gap between per-node free and the published 18x -- see RESULTS.md.
 * -------------------------------------------------------------------------- */

static char g_arena[1 << 20];
static size_t g_arena_off = 0;

static void *arena_alloc(size_t n) {
    n = (n + 15u) & ~(size_t)15u;
    if (g_arena_off + n > sizeof(g_arena)) return malloc(n);   /* overflow escape */
    void *p = g_arena + g_arena_off;
    g_arena_off += n;
    return p;
}
static void arena_reset(void) { g_arena_off = 0; }

/* --------------------------------------------------------------------------
 * The workload.  Each variant builds NBIND bindings, then looks up every one
 * of them, and accumulates the payloads into a checksum.
 * -------------------------------------------------------------------------- */

/* Boxed lookup, shared by A, B and E: walk the spine comparing var ids. */
static int64_t boxed_lookup(Subst *s, int64_t vid) {
    while (s && s->tag == 1) {
        if (s->as.SBind._0 == vid) {
            Term *t = (Term *)(intptr_t)s->as.SBind._1;
            return t->tag == 0 ? t->as.TInt._0 : 0;
        }
        s = (Subst *)(intptr_t)s->as.SBind._2;
    }
    return 0;
}

static int64_t byvalue_lookup(VSubst *s, int64_t vid) {
    while (s && s->tag == 1) {
        if (s->var == vid)
            return s->term.tag == 0 ? s->term.as.TInt._0 : 0;
        s = s->next;
    }
    return 0;
}

/* A / B / E share a builder; the allocator and the disposal differ. */
static Subst *build_boxed_with(void *(*alloc)(size_t)) {
    Subst *s = (Subst *)alloc(sizeof(Subst));
    s->tag = 0; s->as.SNil._0 = 0;
    for (int i = 0; i < NBIND; i++) {
        Term *t = (Term *)alloc(sizeof(Term));
        t->tag = 0; t->as.TInt._0 = i * 3 + 1;
        Subst *n = (Subst *)alloc(sizeof(Subst));
        n->tag = 1;
        n->as.SBind._0 = i;
        n->as.SBind._1 = (int64_t)(intptr_t)t;
        n->as.SBind._2 = (int64_t)(intptr_t)s;
        s = n;
    }
    return s;
}

static Subst *build_boxed(int use_slab) {
    return build_boxed_with(use_slab ? slab_alloc : malloc);
}

static void free_boxed(Subst *s) {
    while (s && s->tag == 1) {
        Subst *next = (Subst *)(intptr_t)s->as.SBind._2;
        free((Term *)(intptr_t)s->as.SBind._1);
        free(s);
        s = next;
    }
    free(s);                                     /* the SNil */
}

static VSubst *build_byvalue_with(void *(*alloc)(size_t)) {
    VSubst *s = (VSubst *)alloc(sizeof(VSubst));
    s->tag = 0; s->var = 0; s->next = NULL;
    memset(&s->term, 0, sizeof(s->term));
    for (int i = 0; i < NBIND; i++) {
        VSubst *n = (VSubst *)alloc(sizeof(VSubst));
        n->tag = 1;
        n->var = i;
        n->term.tag = 0;
        n->term.as.TInt._0 = i * 3 + 1;
        n->next = s;
        s = n;
    }
    return s;
}

static VSubst *build_byvalue(void) { return build_byvalue_with(malloc); }

static void free_byvalue(VSubst *s) {
    while (s) { VSubst *next = s->next; free(s); s = next; }
}

/* --------------------------------------------------------------------------
 * Driver
 * -------------------------------------------------------------------------- */

static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <A|B|C|D|E|F|G> <passes>\n", argv[0]);
        return 2;
    }
    char rep = argv[1][0];
    long passes = strtol(argv[2], NULL, 10);
    if (passes <= 0) { fprintf(stderr, "passes must be positive\n"); return 2; }

    int64_t checksum = 0;
    double t0 = now_ns();

    for (long p = 0; p < passes; p++) {
        switch (rep) {
        case 'A': {                              /* boxed, leaked */
            Subst *s = build_boxed(0);
            for (int i = 0; i < NBIND; i++) checksum += boxed_lookup(s, i);
            break;                               /* discarded, never freed */
        }
        case 'B': {                              /* boxed, freed */
            Subst *s = build_boxed(0);
            for (int i = 0; i < NBIND; i++) checksum += boxed_lookup(s, i);
            free_boxed(s);
            break;
        }
        case 'C': {                              /* by value, spine boxed, leaked */
            VSubst *s = build_byvalue();
            for (int i = 0; i < NBIND; i++) checksum += byvalue_lookup(s, i);
            break;
        }
        case 'D': {                              /* by value AND reclaimed */
            VSubst *s = build_byvalue();
            for (int i = 0; i < NBIND; i++) checksum += byvalue_lookup(s, i);
            free_byvalue(s);
            break;
        }
        case 'E': {                              /* boxed, leaked, slab */
            Subst *s = build_boxed(1);
            for (int i = 0; i < NBIND; i++) checksum += boxed_lookup(s, i);
            break;
        }
        case 'G': {                              /* boxed, arena-reclaimed */
            Subst *s = build_boxed_with(arena_alloc);
            for (int i = 0; i < NBIND; i++) checksum += boxed_lookup(s, i);
            arena_reset();
            break;
        }
        case 'F': {                              /* by value, arena-reclaimed */
            VSubst *s = build_byvalue_with(arena_alloc);
            for (int i = 0; i < NBIND; i++) checksum += byvalue_lookup(s, i);
            arena_reset();
            break;
        }
        default:
            fprintf(stderr, "unknown representation '%c'\n", rep);
            return 2;
        }
    }

    double t1 = now_ns();
    /* One "op" is one binding built and one looked up, per the report's
     * "one SBind + one TInt construction and a logic-walk per operation". */
    double ns_per_op = (t1 - t0) / (double)(passes * NBIND);

    printf("%c,%ld,%.1f,%lld\n", rep, passes, ns_per_op, (long long)checksum);
    return 0;
}
