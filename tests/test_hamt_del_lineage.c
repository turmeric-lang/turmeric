/* test_hamt_del_lineage.c -- regression for the delete-path sibling refcount bug.
 *
 * Bug (docs/archive/history/hamt-delete-sibling-refcount-report.md): node_delete's collapse
 * arms (bitmap and array) build a fresh node holding only the surviving
 * siblings, but also released the *deleted* child from the OLD, shared node
 * `n`.  That old node is persistent and still owns that reference, so once the
 * whole lineage is freed the child is released a second time -> heap
 * use-after-free / double free.
 *
 * This exercises the exact repro from the report: build {1}, {1,2}, delete 2,
 * then free EVERY map in the lineage.  Under ASan+LSan (Debug build) the bug
 * fired as a heap-use-after-free in tur_hamt_node_release; the fix must free
 * the whole lineage cleanly.  Assoc-only lineages were never affected and are
 * kept here as a control.
 *
 * Compiled + run with ASan/UBSan (+LSan on Linux) by tests/run-hamt-del-lineage.sh.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include "hamt.h"

/* Keys/values are plain integer words carried through the void* slots, exactly
 * as stdlib set-add/set-remove do (h == x, val == 1). */
static int64_t w(int64_t x) { return x; }

/* The report's minimal repro: add 1, add 2, remove 2, free all four maps.
 * `2` lands in its own level-0 slot as a sibling of `1`; deleting it collapses
 * the two-child bitmap back down while `1`'s node stays shared across the
 * lineage.  Pre-fix: heap-use-after-free when the second map in the lineage is
 * freed (its bitmap still points at the already-freed deleted child). */
static void test_add_add_remove(void) {
    Hamt *e  = tur_hamt_new();
    Hamt *s1 = tur_hamt_set(e,  1, w(1), w(1));
    Hamt *s2 = tur_hamt_set(s1, 2, w(2), w(1));
    Hamt *s3 = tur_hamt_del(s2, 2, w(2));

    /* s3 no longer has 2; s2 (shared parent) still does. */
    if (tur_hamt_has(s3, 2, w(2))) { fprintf(stderr, "FAIL: 2 still in s3\n"); exit(1); }
    if (!tur_hamt_has(s2, 2, w(2))) { fprintf(stderr, "FAIL: 2 missing from s2\n"); exit(1); }
    if (!tur_hamt_has(s3, 1, w(1))) { fprintf(stderr, "FAIL: 1 missing from s3\n"); exit(1); }

    tur_hamt_free(e);
    tur_hamt_free(s1);
    tur_hamt_free(s2);
    tur_hamt_free(s3);
    printf("  PASS: add,add,remove lineage frees cleanly\n");
}

/* Control from the report: assoc-only lineage always freed cleanly. */
static void test_add_add_add(void) {
    Hamt *e  = tur_hamt_new();
    Hamt *s1 = tur_hamt_set(e,  1, w(1), w(1));
    Hamt *s2 = tur_hamt_set(s1, 2, w(2), w(1));
    Hamt *s3 = tur_hamt_set(s2, 3, w(3), w(1));

    tur_hamt_free(e);
    tur_hamt_free(s1);
    tur_hamt_free(s2);
    tur_hamt_free(s3);
    printf("  PASS: add,add,add control frees cleanly\n");
}

/* Keys colliding on the low 5 bits (1, 33, 65 are all == 1 mod 32) force the
 * trie to diverge at a deeper level, so deleting them exercises the nested
 * collapse arms rather than the single top-level case.  Free the whole lineage. */
static void test_deep_collapse(void) {
    Hamt *e  = tur_hamt_new();
    Hamt *s1 = tur_hamt_set(e,  33, w(33), w(1));
    Hamt *s2 = tur_hamt_set(s1,  1, w(1),  w(1));
    Hamt *s3 = tur_hamt_set(s2, 65, w(65), w(1));
    Hamt *s4 = tur_hamt_del(s3,  1, w(1));
    Hamt *s5 = tur_hamt_del(s4, 65, w(65));

    if (tur_hamt_has(s5, 1, w(1)))   { fprintf(stderr, "FAIL: 1 still in s5\n"); exit(1); }
    if (tur_hamt_has(s5, 65, w(65))) { fprintf(stderr, "FAIL: 65 still in s5\n"); exit(1); }
    if (!tur_hamt_has(s5, 33, w(33))){ fprintf(stderr, "FAIL: 33 missing from s5\n"); exit(1); }
    /* Older snapshots keep their full contents. */
    if (!tur_hamt_has(s3, 1, w(1)))  { fprintf(stderr, "FAIL: 1 missing from s3\n"); exit(1); }
    if (!tur_hamt_has(s3, 65, w(65))){ fprintf(stderr, "FAIL: 65 missing from s3\n"); exit(1); }

    tur_hamt_free(e);
    tur_hamt_free(s1);
    tur_hamt_free(s2);
    tur_hamt_free(s3);
    tur_hamt_free(s4);
    tur_hamt_free(s5);
    printf("  PASS: deep-collapse lineage frees cleanly\n");
}

int main(void) {
    printf("Testing HAMT delete-lineage refcount safety...\n");
    test_add_add_remove();
    test_add_add_add();
    test_deep_collapse();
    printf("all hamt del-lineage tests passed\n");
    return 0;
}
