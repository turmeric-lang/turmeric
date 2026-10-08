/* refine_solver_euf.c -- S1: congruence closure (EUF).
 *
 * Union-find + congruence closure decides quantifier-free equality with
 * uninterpreted functions -- exactly where every measure-as-opaque-function
 * (`len`, `elems`) and every nonlinear-as-uninterpreted term lives.  Textbook
 * treatment: Harrison, *Handbook of Practical Logic and Automated Reasoning*;
 * Bradley & Manna, *The Calculus of Computation*.
 *
 * Every non-leaf term is treated as an application of its operator, so
 * congruence covers `(+ a b)` and `(len v)` uniformly.  Three sources of
 * contradiction:
 *
 *   1. a disequality `a != b` whose sides became congruent;
 *   2. two DISTINCT literals in one class (`3` and `5` can never be equal);
 *   3. a positive atom and a negative atom that are congruent.
 *
 * Terms are interned through a hash index keyed on the hash-cons id, and the
 * closure is a signature-table fixpoint: each round hashes every application
 * by (operator, symbol, argument ROOTS), so two congruent terms meet in one
 * bucket instead of being found by an all-pairs compare.  A round is O(n *
 * arity); it is still a fixpoint over rounds rather than Nieuwenhuis-Oliveras
 * use-lists, which is the right tradeoff at REFINE_MAX_EUF_TERMS.
 * (solver-hot-structures-linear-scans: all three hot scans -- the term index,
 * the all-pairs closure, and the literal-conflict pair scan -- were O(n^2).) */

#include "refine_solver.h"
#include "trail_c.h"

#include <stdlib.h>
#include <string.h>

struct EufState {
    RefineVC *vc;
    Arena    *a;
    VCTerm  **terms;   /* every subterm, deduplicated by pointer (hash-consed) */
    uint32_t *parent;  /* union-find over term indices */
    uint32_t *pstamp;  /* SX3: per-slot trail stamps, parallel to parent */
    uint32_t *hslot;   /* per term: its slot in htab (parallel to terms) */
    uint32_t  n, cap;
    bool      unsat;
    TrailC    trail;   /* SX3: value trail over parent[]; see trail_c.h */

    /* Term index: open addressing, linear probing, keyed on VCTerm.id.  A slot
     * holds term index + 1 (0 = empty).  hcap is a power of two >= 2 * cap,
     * so the load factor never passes 1/2.  Undo is exact without tombstones:
     * terms are only ever appended and undo truncates them LIFO, and clearing
     * the slot of the most recent insert under linear probing restores the
     * table to precisely its state before that insert. */
    uint32_t *htab;
    uint32_t  hcap;

    /* Scratch for euf_close's signature table (hcap slots) and
     * literal_conflict's root map (cap slots); contents are per call. */
    uint32_t *sigtab;
    uint32_t *litroot;

    /* The S3-shared terms (la_is_shared_term, non-Bool), as term indices in
     * registration order -- maintained at registration so collect_shared
     * does not rescan every term per cube.  Truncated by undo like terms. */
    uint32_t *shared;
    uint32_t  n_shared;
};

/* SX3: every parent[] write funnels through here so mark/undo see all of
 * them -- the union in uf_union AND the path compression in uf_find.
 * Trailing compression too is what keeps undo exact; the alternative
 * (compression-free find) costs more than it saves at these sizes.  The
 * merge history a Nieuwenhuis-Oliveras proof forest would need (SX6b's
 * euf_explain) is recoverable from these entries, which is the
 * representation choice the plan asked to keep open. */
static inline void euf_pset(EufState *st, uint32_t i, uint32_t v) {
    trailc_note(&st->trail, i, st->parent[i], &st->pstamp[i]);
    st->parent[i] = v;
}

/* ------------------------------------------------------------------------- *
 * Term registry + union-find
 * ------------------------------------------------------------------------- */

static uint32_t euf_index(EufState *st, VCTerm *t);

static inline uint32_t euf_hash_id(uint32_t id) {
    return id * 2654435761u;   /* Knuth multiplicative; ids are dense */
}

static void euf_hinsert(EufState *st, uint32_t idx) {
    uint32_t mask = st->hcap - 1;
    uint32_t s = euf_hash_id(st->terms[idx]->id) & mask;
    while (st->htab[s]) s = (s + 1) & mask;
    st->htab[s]  = idx + 1;
    st->hslot[idx] = s;
}

/* Lookup only -- never registers.  The closure loop iterates over st->n, so a
 * registering lookup there would mutate the array mid-iteration. */
static uint32_t euf_lookup(const EufState *st, const VCTerm *t) {
    if (!t || !st->hcap) return UINT32_MAX;
    uint32_t mask = st->hcap - 1;
    for (uint32_t s = euf_hash_id(t->id) & mask; st->htab[s]; s = (s + 1) & mask) {
        uint32_t idx = st->htab[s] - 1;
        if (st->terms[idx] == t) return idx;
    }
    return UINT32_MAX;
}

static void euf_add(EufState *st, VCTerm *t) {
    if (st->n >= REFINE_MAX_EUF_TERMS) {
        refine_caps()->euf_terms_hits++;
        refine_cap_peak(&refine_caps()->euf_terms_peak, REFINE_MAX_EUF_TERMS);
        st->unsat = false; return;
    }
    if (st->n == st->cap) {
        uint32_t ncap = st->cap ? st->cap * 2 : 32;
        VCTerm **nt = (VCTerm **)arena_alloc(st->a, ncap * sizeof(VCTerm *));
        uint32_t *np = (uint32_t *)arena_alloc(st->a, ncap * sizeof(uint32_t));
        uint32_t *ns = (uint32_t *)arena_alloc(st->a, ncap * sizeof(uint32_t));
        uint32_t *nh = (uint32_t *)arena_alloc(st->a, ncap * sizeof(uint32_t));
        uint32_t *nl = (uint32_t *)arena_alloc(st->a, ncap * sizeof(uint32_t));
        uint32_t *nsh = (uint32_t *)arena_alloc(st->a, ncap * sizeof(uint32_t));
        if (st->n) {
            memcpy(nt, st->terms, st->n * sizeof(VCTerm *));
            memcpy(np, st->parent, st->n * sizeof(uint32_t));
            memcpy(ns, st->pstamp, st->n * sizeof(uint32_t));
        }
        if (st->n_shared) memcpy(nsh, st->shared, st->n_shared * sizeof(uint32_t));
        st->terms = nt; st->parent = np; st->pstamp = ns; st->cap = ncap;
        st->hslot = nh; st->litroot = nl; st->shared = nsh;
        /* Rehash into a table twice the new capacity.  Re-inserting the live
         * terms in index order yields exactly the table incremental inserts
         * in that order would have, so LIFO undo stays exact across a grow. */
        st->hcap   = ncap * 2;
        st->htab   = (uint32_t *)arena_alloc(st->a, st->hcap * sizeof(uint32_t));
        st->sigtab = (uint32_t *)arena_alloc(st->a, st->hcap * sizeof(uint32_t));
        memset(st->htab, 0, st->hcap * sizeof(uint32_t));
        for (uint32_t i = 0; i < st->n; i++) euf_hinsert(st, i);
    }
    st->terms[st->n]  = t;
    st->parent[st->n] = st->n;
    st->pstamp[st->n] = 0;      /* SX3: never stamped at any live level */
    euf_hinsert(st, st->n);
    if (t->sort != VS_BOOL && la_is_shared_term(t)) st->shared[st->n_shared++] = st->n;
    st->n++;
    refine_cap_peak(&refine_caps()->euf_terms_peak, st->n);
}

/* Register `t` and every subterm; returns t's index (UINT32_MAX if capped). */
static uint32_t euf_index(EufState *st, VCTerm *t) {
    if (!t) return UINT32_MAX;
    uint32_t hit = euf_lookup(st, t);
    if (hit != UINT32_MAX) return hit;
    for (uint32_t i = 0; i < t->n; i++) euf_index(st, t->kids[i]);
    if (st->n >= REFINE_MAX_EUF_TERMS) {
        refine_caps()->euf_terms_hits++;
        refine_cap_peak(&refine_caps()->euf_terms_peak, REFINE_MAX_EUF_TERMS);
        return UINT32_MAX;
    }
    euf_add(st, t);
    return st->n - 1;
}

static uint32_t uf_find(EufState *st, uint32_t i) {
    while (st->parent[i] != i) {
        uint32_t gp = st->parent[st->parent[i]];
        euf_pset(st, i, gp);        /* path compression, trailed (SX3) */
        i = gp;
    }
    return i;
}

static bool uf_union(EufState *st, uint32_t i, uint32_t j) {
    uint32_t ri = uf_find(st, i), rj = uf_find(st, j);
    if (ri == rj) return false;
    euf_pset(st, rj, ri);
    return true;
}

/* ------------------------------------------------------------------------- *
 * Closure
 * ------------------------------------------------------------------------- */

/* Two terms are congruent when they have the same operator/symbol/arity and
 * their arguments are pairwise equal in the current partition. */
static bool congruent(EufState *st, VCTerm *x, VCTerm *y) {
    if (x->op != y->op || x->n != y->n || x->n == 0) return false;
    if (x->op == VC_APP && x->as.idx != y->as.idx) return false;
    for (uint32_t i = 0; i < x->n; i++) {
        uint32_t a = euf_lookup(st, x->kids[i]);
        uint32_t b = euf_lookup(st, y->kids[i]);
        if (a == UINT32_MAX || b == UINT32_MAX) return false;
        if (uf_find(st, a) != uf_find(st, b)) return false;
    }
    return true;
}

/* Hash of a term's congruence signature -- (op, arity, symbol, argument
 * roots) -- or false when it has none: a leaf, or an argument the capped
 * registry never saw (congruent() treats both as matching nothing). */
static bool euf_signature(EufState *st, VCTerm *x, uint32_t *out) {
    if (x->n == 0) return false;
    uint32_t h = (uint32_t)x->op * 0x9e3779b1u ^ x->n * 0x85ebca6bu;
    if (x->op == VC_APP) h = (h ^ x->as.idx) * 0xc2b2ae35u;
    for (uint32_t k = 0; k < x->n; k++) {
        uint32_t a = euf_lookup(st, x->kids[k]);
        if (a == UINT32_MAX) return false;
        h = (h ^ uf_find(st, a)) * 0x27d4eb2fu;
    }
    *out = h;
    return true;
}

/* Signature-table fixpoint.  Each round buckets every application by its
 * signature under the current partition; a term whose bucket already holds a
 * congruent term with a different root is merged into it.  A merge makes the
 * signatures already hashed this round stale, so a round that merged anything
 * is followed by another -- the fixpoint is reached when a round merges
 * nothing, which is exactly the all-pairs loop's exit condition (every
 * congruent pair shares a root), so the partition it produces is the same
 * congruence closure. */
static void euf_close(EufState *st) {
    bool changed = true;
    uint32_t rounds = 0;
    uint32_t mask = st->hcap - 1;
    while (changed && rounds++ < 64) {
        changed = false;
        if (!st->n) break;
        memset(st->sigtab, 0, st->hcap * sizeof(uint32_t));
        for (uint32_t i = 0; i < st->n; i++) {
            VCTerm *x = st->terms[i];
            uint32_t h;
            if (!euf_signature(st, x, &h)) continue;
            uint32_t s = h & mask;
            bool placed = false;
            for (; st->sigtab[s]; s = (s + 1) & mask) {
                uint32_t j = st->sigtab[s] - 1;
                if (!congruent(st, st->terms[j], x)) continue;
                if (uf_union(st, j, i)) changed = true;
                placed = true;
                break;
            }
            if (!placed) st->sigtab[s] = i + 1;
        }
    }
}

/* Two numeric literals in one class conflict when their VALUES differ.  Two
 * distinct terms of the SAME kind always do -- hash-consing interns one term
 * per value -- but an int literal and a real literal are two terms for one
 * value: `3` and `3.0`.  Until 2026-09-29 that pair was called a conflict,
 * which refuted a satisfiable cube and proved whatever goal sat under it.
 * It is reachable from source wherever a real-sorted term is equated with an
 * int-sorted one -- `(= x (to-f n))` with `x = 3.0` and `n = 3`, `to-f` a
 * float-returning measure -- because S3's exchange then merges the two
 * classes (fixture refine-int-real-literal-not-contradictory).
 *
 * The mixed compare is deliberately CONSERVATIVE: `(double)i == r` whenever
 * the exact values could be equal (a real literal IS a double, so an exactly
 * equal int converts to exactly it), so only a definite inequality conflicts
 * and a rounding coincidence at 2^53 and beyond costs a proof, never a wrong
 * one. */
static bool lit_values_differ(const VCTerm *x, const VCTerm *y) {
    if (x->op == y->op) return true;                 /* distinct terms, one kind */
    const VCTerm *iv = x->op == VC_CONST_INT ? x : y;
    const VCTerm *rv = x->op == VC_CONST_INT ? y : x;
    return (double)iv->as.i != rv->as.r;
}

/* One pass: remember the first literal seen in each class and conflict on a
 * second whose value differs. */
static bool literal_conflict(EufState *st) {
    for (uint32_t i = 0; i < st->n; i++) st->litroot[i] = UINT32_MAX;
    for (uint32_t i = 0; i < st->n; i++) {
        VCTerm *x = st->terms[i];
        if (x->op != VC_CONST_INT && x->op != VC_CONST_REAL) continue;
        uint32_t r = uf_find(st, i);
        if (st->litroot[r] != UINT32_MAX) {
            if (lit_values_differ(st->terms[st->litroot[r]], x)) return true;
            continue;   /* `3` and `3.0`: one value, no conflict */
        }
        st->litroot[r] = i;
    }
    return false;
}

/* ------------------------------------------------------------------------- *
 * Public API
 * ------------------------------------------------------------------------- */

EufState *euf_new(RefineVC *vc, Arena *a) {
    EufState *st = (EufState *)arena_alloc(a, sizeof(EufState));
    memset(st, 0, sizeof(*st));
    st->vc = vc; st->a = a;
    trailc_init(&st->trail, a);
    return st;
}

/* SX3: mark / undo-to-mark.  The trail restores parent[]; the mark itself
 * restores the append-only pieces (n, unsat) by truncation.  Terms above the
 * mark are simply forgotten -- euf_add re-initializes parent/pstamp on reuse,
 * and trailc_undo_to skips entries for truncated slots. */
EufMark euf_mark(EufState *st) {
    TrailCMark tm = trailc_mark(&st->trail);
    EufMark m = { tm.len, tm.level, st->n, st->n_shared, st->unsat };
    return m;
}

void euf_undo_to(EufState *st, EufMark m) {
    TrailCMark tm = { m.trail_len, m.trail_level };
    trailc_undo_to(&st->trail, tm, st->parent, m.n);
    /* Unindex the truncated terms newest-first -- the LIFO order that makes
     * a plain slot clear an exact undo under linear probing. */
    for (uint32_t i = st->n; i > m.n; i--) st->htab[st->hslot[i - 1]] = 0;
    st->n        = m.n;
    st->n_shared = m.n_shared;
    st->unsat    = m.unsat;
}

/* SX3 seam: TUR_REFINE_EUF=rebuild restores the per-cube euf_new path so the
 * corpus can be replayed against both.  Env-only, like
 * TUR_REFINE_NO_DISCHARGE; default is incremental. */
bool euf_incremental_mode(void) {
    static int mode = -1;
    if (mode < 0) {
        const char *e = getenv("TUR_REFINE_EUF");
        mode = !(e && strcmp(e, "rebuild") == 0);
    }
    return mode != 0;
}

bool euf_assert_eq(EufState *st, VCTerm *x, VCTerm *y) {
    uint32_t i = euf_index(st, x), j = euf_index(st, y);
    if (i == UINT32_MAX || j == UINT32_MAX) return true;   /* capped: no info */
    uf_union(st, i, j);
    euf_close(st);
    if (literal_conflict(st)) { st->unsat = true; return false; }
    return true;
}

bool euf_equal(EufState *st, VCTerm *x, VCTerm *y) {
    uint32_t i = euf_index(st, x), j = euf_index(st, y);
    if (i == UINT32_MAX || j == UINT32_MAX) return false;
    return uf_find(st, i) == uf_find(st, j);
}

uint32_t euf_term_count(const EufState *st) { return st->n; }
VCTerm  *euf_term_at(const EufState *st, uint32_t i) {
    return i < st->n ? st->terms[i] : NULL;
}
uint32_t euf_shared_count(const EufState *st) { return st->n_shared; }
VCTerm  *euf_shared_at(const EufState *st, uint32_t i) {
    return i < st->n_shared ? st->terms[st->shared[i]] : NULL;
}

bool euf_assert_cube(EufState *st, const VCCube *c) {
    /* Pass 1: register every term and assert the positive equalities. */
    for (uint32_t i = 0; i < c->n; i++) {
        VCTerm *lit = c->lits[i];
        VCTerm *at  = refine_lit_atom(lit);
        euf_index(st, at);
        if (!refine_lit_is_neg(lit) && at->op == VC_EQ) {
            uint32_t x = euf_index(st, at->kids[0]);
            uint32_t y = euf_index(st, at->kids[1]);
            if (x != UINT32_MAX && y != UINT32_MAX) uf_union(st, x, y);
        }
    }
    euf_close(st);
    if (literal_conflict(st)) { st->unsat = true; return false; }

    /* Pass 2: contradictions. */
    for (uint32_t i = 0; i < c->n; i++) {
        VCTerm *lit = c->lits[i];
        VCTerm *at  = refine_lit_atom(lit);
        /* `x != y` with x, y in one class: a contradiction unless the class
         * may be NaN, which is unequal to itself (refine_solver.h). */
        if (refine_lit_is_neg(lit) && at->op == VC_EQ &&
            (refine_cube_nan_free(c, at->kids[0]) || refine_cube_nan_free(c, at->kids[1]))) {
            if (euf_equal(st, at->kids[0], at->kids[1])) { st->unsat = true; return false; }
        }
        /* A positive and a negative occurrence of congruent atoms. */
        for (uint32_t j = i + 1; j < c->n; j++) {
            VCTerm *lit2 = c->lits[j];
            if (refine_lit_is_neg(lit) == refine_lit_is_neg(lit2)) continue;
            VCTerm *at2 = refine_lit_atom(lit2);
            if (at == at2) { st->unsat = true; return false; }
            if (congruent(st, at, at2)) { st->unsat = true; return false; }
        }
    }
    return true;
}

RefineDecision refine_s1_decide_cc(RefineVC *vc, Arena *a, RefineCubeCache *cc) {
    if (!vc || !vc->goal) return refine_unknown();

    const VCCubeSet *cs;
    if (!refine_cubes_get(vc, a, cc, &cs)) return refine_unknown();
    if (cs->trivial || cs->n == 0) return refine_valid();

    /* SX3: one state, mark/undo per cube, instead of a fresh euf_new per
     * cube -- the arrays and trail grow once and are reused.  Each cube still
     * starts from the empty partition (the mark is taken on the empty state),
     * so verdicts and cap telemetry are identical to the rebuild path by
     * construction. */
    EufState *inc = euf_incremental_mode() ? euf_new(vc, a) : NULL;
    for (uint32_t i = 0; i < cs->n; i++) {
        bool survived;
        if (inc) {
            EufMark m = euf_mark(inc);
            survived = euf_assert_cube(inc, &cs->cubes[i]);
            euf_undo_to(inc, m);
        } else {
            EufState *st = euf_new(vc, a);
            survived = euf_assert_cube(st, &cs->cubes[i]);
        }
        if (survived) return refine_unknown();  /* cube survived */
    }
    return refine_valid();
}

/* refine-chain-expands-the-same-dnf-four-times: the original entry point, kept
 * so every caller outside the chain driver (`tur smt`, tests/unit, the SX8
 * doors) is unchanged -- a fresh cache means it builds exactly once, as it
 * always did. */
RefineDecision refine_s1_decide(RefineVC *vc, Arena *a) {
    RefineCubeCache cc = {0};
    return refine_s1_decide_cc(vc, a, &cc);
}
