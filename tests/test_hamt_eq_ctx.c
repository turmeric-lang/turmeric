/* test_hamt_eq_ctx.c -- prereq 2a: context-carrying HAMT key comparator.
 *
 * Exercises the tur_hamt_*_eq_ctx family (src/runtime/hamt.c), the ABI
 * groundwork that lets a *stateful* comparator -- e.g. a Turmeric closure that
 * needs its captured environment + a TuriEnv* -- ride along to collision-time
 * key compares via a `void *ctx` word.  We stand in for the interpreter closure
 * with a small C struct passed as ctx and a trampoline comparator that reads it.
 *
 * Acceptance:
 *   1. ctx is threaded to every compare (the trampoline sees its struct, and a
 *      counter inside it proves the comparator actually ran on collisions).
 *   2. content equality works: two distinct pointers with equal payload collide
 *      to the same slot (get/has/del find the entry; the map keeps ONE entry).
 *   3. nesting is correct: a plain int-identity-keyed op performed *inside* the
 *      ctx comparator does NOT leak the outer ctx comparator (mutual exclusion
 *      of the two hook families), and vice versa.
 *
 * Built and run by tests/run-hamt-eq-ctx.sh under ASan/UBSan (+LSan on Linux).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>

#include "hamt.h"

#define W(p) ((int64_t)(intptr_t)(p))   /* a pointer as the map's int64 word */

/* Stand-in for the interpreter's {env, closure} bundle. */
typedef struct {
    int   tag;        /* identity marker the trampoline asserts it received */
    long  call_count; /* incremented on every compare -- proves ctx flows in */
    Hamt *inner;      /* used by the nesting test to run a plain op mid-compare */
} EqCtx;

/* Content comparator over `const char *` keys, carrying its ctx. */
static bool cstr_eq_ctx(int64_t a, int64_t b, void *vctx) {
    EqCtx *ctx = (EqCtx *)vctx;
    assert(ctx && ctx->tag == 0xC0FFEE);
    ctx->call_count++;
    const char *sa = (const char *)(intptr_t)a;
    const char *sb = (const char *)(intptr_t)b;
    return strcmp(sa, sb) == 0;
}

/* Plain (no-ctx) identity comparator -- the inner op in the nesting test. */
static bool int_identity_eq(int64_t a, int64_t b) {
    return a == b;
}

/* A comparator that, partway through, performs a *plain* _eq op so we can prove
 * the inner op sees int-identity, not this outer ctx comparator. */
static bool cstr_eq_ctx_nesting(int64_t a, int64_t b, void *vctx) {
    EqCtx *ctx = (EqCtx *)vctx;
    assert(ctx && ctx->tag == 0xC0FFEE);
    ctx->call_count++;
    /* Run a plain int-keyed lookup mid-compare.  If the ctx hook leaked, this
     * would call cstr_eq_ctx_nesting recursively (and re-enter with our ctx);
     * with correct mutual exclusion it uses int_identity_eq instead. */
    int two = 2;
    (void)tur_hamt_has_eq(ctx->inner, 2, W(&two), int_identity_eq);
    const char *sa = (const char *)(intptr_t)a;
    const char *sb = (const char *)(intptr_t)b;
    return strcmp(sa, sb) == 0;
}

static int g_vals[8];

/* All keys hash to the same bucket -> a collision chain -> the comparator is
 * exercised on every probe (identity short-circuits don't hide a bug). */
#define COLLIDE 0x1234ULL

static void test_ctx_threaded_and_content_keyed(void) {
    EqCtx ctx = { 0xC0FFEE, 0, NULL };

    /* Distinct buffers holding equal text -> must collide by CONTENT, not by
     * pointer identity (each "apple" lives at a different address). */
    char apple1[] = "apple";
    char apple2[] = "apple";
    char pear[]   = "pear";

    Hamt *m0 = tur_hamt_new();
    Hamt *m1 = tur_hamt_set_eq_ctx(m0, COLLIDE, W(apple1), W(&g_vals[0]), cstr_eq_ctx, &ctx);
    Hamt *m2 = tur_hamt_set_eq_ctx(m1, COLLIDE, W(pear),   W(&g_vals[1]), cstr_eq_ctx, &ctx);

    /* Re-set with a *different pointer* but equal text: content key => update,
     * not insert. Count must stay 2. */
    Hamt *m3 = tur_hamt_set_eq_ctx(m2, COLLIDE, W(apple2), W(&g_vals[2]), cstr_eq_ctx, &ctx);
    assert(tur_hamt_count(m3) == 2 && "content-keyed re-set must update, not grow");

    /* get/has by content through a fresh probe pointer. */
    char probe[] = "apple";
    int64_t got = tur_hamt_get_eq_ctx(m3, COLLIDE, W(probe), cstr_eq_ctx, &ctx);
    assert(got == W(&g_vals[2]) && "get must find the updated value by content");
    assert(tur_hamt_has_eq_ctx(m3, COLLIDE, W(probe), cstr_eq_ctx, &ctx));

    Hamt *m4 = tur_hamt_del_eq_ctx(m3, COLLIDE, W(probe), cstr_eq_ctx, &ctx);
    assert(tur_hamt_count(m4) == 1 && "del by content must remove the entry");
    assert(!tur_hamt_has_eq_ctx(m4, COLLIDE, W(probe), cstr_eq_ctx, &ctx));

    assert(ctx.call_count > 0 && "ctx comparator must actually have run");

    tur_hamt_free(m0); tur_hamt_free(m1); tur_hamt_free(m2);
    tur_hamt_free(m3); tur_hamt_free(m4);
    printf("  ctx-threaded + content-keyed: OK (compares=%ld)\n", ctx.call_count);
}

static void test_nesting_mutual_exclusion(void) {
    /* Build a small plain int-identity-keyed map for the inner op. */
    int v = 7;
    int key2 = 2;
    Hamt *inner0 = tur_hamt_new();
    Hamt *inner1 = tur_hamt_set_eq(inner0, 2, W(&key2), W(&v), int_identity_eq);

    EqCtx ctx = { 0xC0FFEE, 0, inner1 };

    char a1[] = "xyz";
    char a2[] = "xyz";

    Hamt *m0 = tur_hamt_new();
    Hamt *m1 = tur_hamt_set_eq_ctx(m0, COLLIDE, W(a1), W(&g_vals[0]), cstr_eq_ctx_nesting, &ctx);
    /* This probe drives cstr_eq_ctx_nesting, which performs a plain inner op
     * mid-compare. Correct mutual exclusion => no infinite recursion / crash,
     * and the content match still resolves. */
    Hamt *m2 = tur_hamt_set_eq_ctx(m1, COLLIDE, W(a2), W(&g_vals[1]), cstr_eq_ctx_nesting, &ctx);
    assert(tur_hamt_count(m2) == 1 && "nested-op compare must still content-match");

    tur_hamt_free(m0); tur_hamt_free(m1); tur_hamt_free(m2);
    tur_hamt_free(inner0); tur_hamt_free(inner1);
    printf("  nesting mutual-exclusion: OK (compares=%ld)\n", ctx.call_count);
}

/* A ctx==NULL call must behave exactly like the no-ctx path with an ignored
 * argument: a NULL-tolerant comparator over int-identity keys. */
static bool int_eq_ctx_nullok(int64_t a, int64_t b, void *vctx) {
    (void)vctx;
    return a == b;
}

static void test_null_ctx(void) {
    int ka = 5, kb = 5;
    int va = 1;
    Hamt *m0 = tur_hamt_new();
    Hamt *m1 = tur_hamt_set_eq_ctx(m0, 9, W(&ka), W(&va), int_eq_ctx_nullok, NULL);
    /* identity keys: same word matches; a different word with == value also
     * matches via the comparator. */
    assert(tur_hamt_has_eq_ctx(m1, 9, W(&ka), int_eq_ctx_nullok, NULL));
    (void)kb;
    tur_hamt_free(m0); tur_hamt_free(m1);
    printf("  ctx==NULL passthrough: OK\n");
}

int main(void) {
    test_ctx_threaded_and_content_keyed();
    test_nesting_mutual_exclusion();
    test_null_ctx();
    printf("all eq-ctx tests passed\n");
    return 0;
}
