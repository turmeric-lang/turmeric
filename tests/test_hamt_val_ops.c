/* test_hamt_val_ops.c -- caller-supplied VALUE ownership unit test.
 *
 * collections-cannot-hold-rc-values, map side, step (b).  Value ownership used
 * to be a single `bool val_owned` on the map, and the ops it selected were
 * hardcoded to the tur_hamt_box_key box refcount.  That is fine for a boxed
 * multi-word value, but an rc<T> value is not a box -- its lifetime runs
 * through rc_strong_increment / rc_strong_decrement, which the map had no way
 * to be told about.  The map now stores caller-supplied value ops, threaded
 * in through the tur_hamt_{set,del}_eq_vo entry points and kept on the
 * resulting map so a later tur_hamt_free releases with the SAME ops.
 *
 * What this asserts:
 *
 *   1. The caller's ops are the ones that actually run -- not the box ops.
 *      Both counters here are plain statics over stack storage; if the box
 *      refcount were still hardcoded it would be handed a non-box pointer and
 *      ASan would fire immediately.
 *   2. Retain/release are BALANCED across structural sharing.  Every reference
 *      the map mints (collision-chain copies) is released exactly once, plus
 *      the one incoming reference per stored entry.
 *   3. The ops survive onto derived maps.  A set/del produces a NEW map, and
 *      freeing that later map must use the ops the original was built with.
 *   4. owned == 0 leaves value lifetime completely untouched (the common
 *      single-word int/cstr/handle value rides the carrier inline).
 *
 * Built and run by tests/run-hamt-val-ops.sh.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>

#include "hamt.h"

#define W(p) ((int64_t)(intptr_t)(p))   /* a pointer as the map's int64 word */

static int g_retain;
static int g_release;

static void counting_retain(void *v) { (void)v; g_retain++; }
static void counting_release(void *v) { (void)v; g_release++; }

static void reset_counts(void) { g_retain = 0; g_release = 0; }

/* Values stay caller-owned storage; the ops only count. */
static long g_vals[64];

/* Distinct hashes -> a normal bitmap trie, no entry duplication. */
static void test_distinct_hashes(void) {
    reset_counts();
    Hamt *m = NULL;
    const int n = 8;

    for (long i = 0; i < n; i++) {
        g_vals[i] = i * 10;
        Hamt *old = m;
        m = tur_hamt_set_eq_vo(m, (uint64_t)i * 0x9E3779B97F4A7C15ULL,
                               (int64_t)(i + 1), W(&g_vals[i]),
                               NULL, 2, counting_retain, counting_release);
        if (old && old != m) tur_hamt_free(old);
    }
    assert(tur_hamt_count(m) == (uint32_t)n);

    tur_hamt_free(m);
    /* One incoming reference per entry, one release per entry on free. */
    assert(g_retain + n == g_release);
    printf("  distinct hashes: retain=%d release=%d (balanced)\n",
           g_retain, g_release);
}

/* One shared hash -> a collision chain, so every insert copies the chain and
 * the retain hook fires on each surviving entry.  This is the case that
 * actually exercises the retain side. */
static void test_collision_chain(void) {
    reset_counts();
    Hamt *m = NULL;
    const int n = 8;

    for (long i = 0; i < n; i++) {
        g_vals[i] = i * 10;
        Hamt *old = m;
        m = tur_hamt_set_eq_vo(m, (uint64_t)0xC0FFEE, (int64_t)(i + 1),
                               W(&g_vals[i]), NULL, 2,
                               counting_retain, counting_release);
        if (old && old != m) tur_hamt_free(old);
    }
    assert(tur_hamt_count(m) == (uint32_t)n);
    assert(g_retain > 0);  /* the chain copies really did duplicate entries */

    tur_hamt_free(m);
    assert(g_retain + n == g_release);
    printf("  collision chain: retain=%d release=%d (balanced)\n",
           g_retain, g_release);
}

/* The ops must be inherited by maps derived via set/del, and by the whole
 * persistent lineage -- an older version freed after a newer one still has to
 * release with the ops it was built with. */
static void test_ops_survive_derivation(void) {
    reset_counts();
    const int n = 6;

    Hamt *versions[8];
    Hamt *m = NULL;
    for (long i = 0; i < n; i++) {
        g_vals[i] = i * 10;
        m = tur_hamt_set_eq_vo(m, (uint64_t)0xBEEF + (uint64_t)(i % 2),
                               (int64_t)(i + 1), W(&g_vals[i]),
                               NULL, 2, counting_retain, counting_release);
        versions[i] = m;  /* keep every version alive simultaneously */
    }

    /* A delete off the newest version inherits the ops too. */
    Hamt *deleted = tur_hamt_del_eq_vo(m, (uint64_t)0xBEEF, 1,
                                       NULL, 2, counting_retain, counting_release);

    /* Free the lineage oldest-first; then the derived map. */
    for (int i = 0; i < n; i++) tur_hamt_free(versions[i]);
    if (deleted != m) tur_hamt_free(deleted);

    /* n incoming references from the inserts (each stored), plus every
     * reference the map minted, all released. */
    assert(g_retain + n == g_release);
    printf("  derived lineage: retain=%d release=%d (balanced)\n",
           g_retain, g_release);
}

/* owned == 0: the map must not touch value lifetime at all. */
static void test_unowned_values_untouched(void) {
    reset_counts();
    Hamt *m = NULL;

    for (long i = 0; i < 6; i++) {
        g_vals[i] = i * 10;
        Hamt *old = m;
        m = tur_hamt_set_eq_vo(m, (uint64_t)0xD00D, (int64_t)(i + 1),
                               W(&g_vals[i]), NULL, 0,
                               counting_retain, counting_release);
        if (old && old != m) tur_hamt_free(old);
    }
    tur_hamt_free(m);

    assert(g_retain == 0);
    assert(g_release == 0);
    printf("  owned==0:        retain=0 release=0 (untouched)\n");
}

/* The legacy _eq_o entry point must keep meaning exactly what it meant before:
 * value ownership via the box refcount. */
static void test_legacy_eq_o_still_boxes(void) {
    Hamt *m = NULL;
    for (long i = 0; i < 6; i++) {
        void *boxed = tur_hamt_box_key(&i, sizeof i);
        Hamt *old = m;
        m = tur_hamt_set_eq_o(m, (uint64_t)0xFEED, (int64_t)(i + 1),
                              W(boxed), NULL, 2);
        if (old && old != m) tur_hamt_free(old);
    }
    assert(tur_hamt_count(m) == 6);
    tur_hamt_free(m);  /* leak-clean only if the box ops still run here */
    printf("  legacy _eq_o:    boxed values freed via the box refcount\n");
}

int main(void) {
    printf("hamt caller-supplied value ops:\n");
    test_distinct_hashes();
    test_collision_chain();
    test_ops_survive_derivation();
    test_unowned_values_untouched();
    test_legacy_eq_o_still_boxes();
    printf("all value-ops tests passed\n");
    return 0;
}
