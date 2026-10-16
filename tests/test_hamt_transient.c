/* test_hamt_transient.c -- in-place HAMT transients against a model.
 *
 * tur_hamt_transient_set changes nodes in place once the transient owns them,
 * and copies a node only while the map it came from still shares it
 * (node_insert_mut, performance audit R-5).  The ways that goes wrong are a
 * wrong answer from the transient, a change leaking into the source map or
 * another map built from it, and a lost or doubled reference.  So this checks:
 *
 *   1. random set / update / delete sequences against a plain array model,
 *      with hashes chosen to reach every insert path: spread hashes, hashes
 *      that agree on their low chunks and part deep, and true 64-bit
 *      collisions;
 *   2. that the source map, a persistent map derived from it while the
 *      transient is open, and a second transient over the same source all
 *      keep their own contents;
 *   3. under LeakSanitizer, that every map and node is freed -- including
 *      after transient deletes, whose root used to be retained once too often.
 *
 * Compiled + run with ASan/UBSan (+LSan on Linux) by tests/run-hamt-transient.sh.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "hamt.h"

#define K 3000                 /* key universe: keys are 1..K */

static void *w(int64_t x) { return (void *)(intptr_t)x; }

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); failures++; } } while (0)

/* Hash families.  Keys are plain words compared by identity, so the hash is
 * free to collide however a case wants. */
typedef uint64_t (*hash_fn)(int64_t k);
static uint64_t h_spread(int64_t k)  { return (uint64_t)k * 0x9E3779B97F4A7C15ULL; }
/* Every key has the same low 20 bits, so the tree runs four levels deep before
 * it branches, and two keys part at level 4, 5 or 8. */
static uint64_t h_deep(int64_t k)    { return ((uint64_t)(k % 64) << 20) | 0x5A5A5ULL | ((uint64_t)(k / 64) << 40); }
/* Groups of 5 keys share a full 64-bit hash: collision-node chains. */
static uint64_t h_collide(int64_t k) { return h_spread(k / 5); }

/* The model: model[k] is k's value, 0 for absent. */
typedef struct { int64_t v[K + 1]; uint32_t count; } Model;

static void model_set(Model *m, int64_t k, int64_t v) { if (!m->v[k]) m->count++; m->v[k] = v; }
static void model_del(Model *m, int64_t k) { if (m->v[k]) m->count--; m->v[k] = 0; }

static void check_map(const char *what, Hamt *h, const Model *m, hash_fn hf) {
    CHECK(tur_hamt_count(h) == m->count, "%s: count %u, model %u", what, tur_hamt_count(h), m->count);
    for (int64_t k = 1; k <= K; k++) {
        bool has = tur_hamt_has(h, hf(k), w(k));
        CHECK(has == (m->v[k] != 0), "%s: has(%lld) = %d, model %lld", what, (long long)k, has, (long long)m->v[k]);
        if (has) {
            void *got = tur_hamt_get(h, hf(k), w(k));
            CHECK(got == w(m->v[k]), "%s: get(%lld) = %lld, model %lld", what, (long long)k,
                  (long long)(intptr_t)got, (long long)m->v[k]);
        }
        if (failures > 20) { fprintf(stderr, "too many failures\n"); exit(1); }
    }
}

static uint64_t rng = 88172645463325252ULL;
static uint64_t next_rand(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

/* A persistent map holding the model, built with tur_hamt_set. */
static Hamt *build_persistent(const Model *m, hash_fn hf) {
    Hamt *h = tur_hamt_new();
    for (int64_t k = 1; k <= K; k++) {
        if (!m->v[k]) continue;
        Hamt *n = tur_hamt_set(h, hf(k), w(k), w(m->v[k]));
        tur_hamt_free(h);
        h = n;
    }
    return h;
}

/* Random ops on a transient over `src`, mirrored on a model; returns the
 * sealed map.  Updates are a third of the sets once a key is present. */
static Hamt *random_ops(Hamt *src, Model *m, hash_fn hf, int ops, int del_pct) {
    HamtTransient *t = tur_hamt_transient(src);
    for (int i = 0; i < ops; i++) {
        int64_t k = (int64_t)(next_rand() % K) + 1;
        if ((int)(next_rand() % 100) < del_pct) {
            tur_hamt_transient_del(t, hf(k), w(k));
            model_del(m, k);
        } else {
            int64_t v = (int64_t)(next_rand() % 1000000) + 1;
            tur_hamt_transient_set(t, hf(k), w(k), w(v));
            model_set(m, k, v);
        }
    }
    Hamt *out = tur_hamt_persistent(t);
    free(t);
    return out;
}

static void test_against_model(const char *name, hash_fn hf) {
    char what[96];
    /* From empty: inserts and updates only, then with deletes. */
    Model m = {0};
    Hamt *e = tur_hamt_new();
    Hamt *a = random_ops(e, &m, hf, 6000, 0);
    snprintf(what, sizeof what, "%s: from empty", name);
    check_map(what, a, &m, hf);

    Model m2 = m;
    Hamt *b = random_ops(a, &m2, hf, 6000, 30);
    snprintf(what, sizeof what, "%s: with deletes", name);
    check_map(what, b, &m2, hf);
    snprintf(what, sizeof what, "%s: source after", name);
    check_map(what, a, &m, hf);   /* the transient's source is untouched */

    tur_hamt_free(e);
    tur_hamt_free(a);
    tur_hamt_free(b);
    printf("  PASS: %s hashes match the model\n", name);
}

/* Isolation: a persistent source built with tur_hamt_set, a persistent map
 * derived from it while a transient over it is open, and two transients over
 * the same source, each changing keys the others share. */
static void test_isolation(const char *name, hash_fn hf) {
    char what[96];
    Model base = {0};
    for (int64_t k = 1; k <= K; k += 2) model_set(&base, k, k * 10);
    Hamt *src = build_persistent(&base, hf);

    HamtTransient *t1 = tur_hamt_transient(src);
    HamtTransient *t2 = tur_hamt_transient(src);
    Model m1 = base, m2 = base, md = base;

    /* A persistent sibling, derived while both transients are open. */
    Hamt *sib = tur_hamt_set(src, hf(1), w(1), w(777));
    model_set(&md, 1, 777);

    for (int64_t k = 1; k <= K; k++) {
        if (k % 3 == 0) { tur_hamt_transient_del(t1, hf(k), w(k)); model_del(&m1, k); }
        else            { tur_hamt_transient_set(t1, hf(k), w(k), w(k + 1)); model_set(&m1, k, k + 1); }
        if (k % 4 == 0) { tur_hamt_transient_set(t2, hf(k), w(k), w(k + 2)); model_set(&m2, k, k + 2); }
    }
    Hamt *r1 = tur_hamt_persistent(t1);
    Hamt *r2 = tur_hamt_persistent(t2);
    free(t1);
    free(t2);

    snprintf(what, sizeof what, "%s: source", name);      check_map(what, src, &base, hf);
    snprintf(what, sizeof what, "%s: sibling", name);     check_map(what, sib, &md, hf);
    snprintf(what, sizeof what, "%s: transient 1", name); check_map(what, r1, &m1, hf);
    snprintf(what, sizeof what, "%s: transient 2", name); check_map(what, r2, &m2, hf);

    /* Free in an order that leaves shared nodes for the last owner. */
    tur_hamt_free(src);
    tur_hamt_free(r1);
    tur_hamt_free(sib);
    tur_hamt_free(r2);
    printf("  PASS: %s hashes keep source, sibling and transients apart\n", name);
}

int main(void) {
    printf("hamt transient tests:\n");
    hash_fn fns[] = { h_spread, h_deep, h_collide };
    const char *names[] = { "spread", "deep", "colliding" };
    for (int i = 0; i < 3; i++) {
        test_against_model(names[i], fns[i]);
        test_isolation(names[i], fns[i]);
    }
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all hamt transient tests passed\n");
    return 0;
}
