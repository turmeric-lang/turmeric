/* refine_solver.c -- shared machinery for the in-house stages: NNF + the
 * small-DNF cube expansion every theory solver runs over.
 *
 * To prove `hyps |= goal` we refute `hyps AND (not goal)`.  Cube expansion is
 * the "naive S4" the plan sanctions: explicit annotations keep the
 * propositional structure of a real obligation tiny, so small-DNF is enough
 * and we never build a DPLL(T) engine.  Every cap here degrades to
 * RT_UNKNOWN -> runtime check, which is always sound. */

#include "refine_solver.h"

#include <string.h>

/* ------------------------------------------------------------------------- *
 * Cap telemetry (see refine_solver.h)
 * ------------------------------------------------------------------------- */

static RefineCapStats g_caps;

RefineCapStats *refine_caps(void) { return &g_caps; }
void refine_caps_reset(void) { memset(&g_caps, 0, sizeof(g_caps)); }

bool refine_caps_any(void) {
    return g_caps.cubes_hits || g_caps.cube_lits_hits || g_caps.expand_depth_hits ||
           g_caps.la_vars_hits || g_caps.la_constr_hits || g_caps.la_fm_hits ||
           g_caps.euf_terms_hits || g_caps.no_shared_hits || g_caps.no_rounds_hits ||
           g_caps.path_hyps_hits || g_caps.model_vars_hits || g_caps.model_evals_hits;
}

/* ------------------------------------------------------------------------- *
 * NNF
 * ------------------------------------------------------------------------- */

/* Push negations to the leaves.  vc_mk already folds (not (not p)) and the
 * constants, so this only has to handle and/or. */
static VCTerm *nnf(RefineVC *vc, VCTerm *t, bool neg) {
    if (!t) return NULL;
    switch (t->op) {
        case VC_NOT:
            return nnf(vc, t->kids[0], !neg);
        case VC_AND:
        case VC_OR: {
            VCOp op = t->op;
            if (neg) op = (op == VC_AND) ? VC_OR : VC_AND;   /* De Morgan */
            VCTerm **kids = (VCTerm **)arena_alloc(vc->arena, t->n * sizeof(VCTerm *));
            for (uint32_t i = 0; i < t->n; i++) kids[i] = nnf(vc, t->kids[i], neg);
            return vc_mk(vc, op, kids, t->n);
        }
        default:
            /* A DISEQUALITY IS A DISJUNCTION.  `a != b` over a totally ordered
             * sort is `a < b OR b < a`, and until it is written that way S2
             * cannot use it at all: `la_assert_cube` skips a negated VC_EQ
             * because a single linear constraint cannot express it.  Since the
             * goal is refuted by asserting its negation, that made an EQUALITY
             * GOAL unprovable whenever it needed any arithmetic -- and `(= r
             * <expr>)` is the most natural postcondition there is. `(- (+ x 1)
             * 1)` against `(= r x)` was Unknown for exactly this reason.
             *
             * The original literal is KEPT alongside the split, so EUF still
             * sees the disequality it was already using; the cubes only gain
             * information. That matters for soundness: we are refuting, so the
             * rewrite has to be an EQUIVALENCE, never a strengthening. It is
             * one -- the added disjunction is implied by the literal it joins.
             *
             * Each disequality doubles the cube count, which REFINE_MAX_CUBES
             * bounds; blowing the cap answers Unknown and keeps the runtime
             * check, as every cap here does. */
            if (neg && t->op == VC_EQ && t->n == 2 &&
                vc_is_arith(t->kids[0]) && vc_is_arith(t->kids[1])) {
                VCTerm *lt  = vc_mk2(vc, VC_LT, t->kids[0], t->kids[1]);
                VCTerm *gt  = vc_mk2(vc, VC_LT, t->kids[1], t->kids[0]);
                return vc_mk2(vc, VC_AND, vc_not(vc, t),
                              vc_mk2(vc, VC_OR, lt, gt));
            }
            return neg ? vc_not(vc, t) : t;
    }
}

/* ------------------------------------------------------------------------- *
 * DNF expansion into cubes
 * ------------------------------------------------------------------------- */

typedef struct {
    Arena     *arena;
    VCCube    *cubes;
    uint32_t   n, cap;
    uint32_t   depth;
    bool       overflow;
} CubeAcc;

static void acc_push(CubeAcc *acc, VCTerm **lits, uint32_t n) {
    if (acc->overflow) return;
    if (acc->n >= REFINE_MAX_CUBES) {
        g_caps.cubes_hits++;
        refine_cap_peak(&g_caps.cubes_peak, REFINE_MAX_CUBES);
        acc->overflow = true; return;
    }
    if (acc->n == acc->cap) {
        uint32_t ncap = acc->cap ? acc->cap * 2 : 8;
        VCCube *nc = (VCCube *)arena_alloc(acc->arena, ncap * sizeof(VCCube));
        if (acc->n) memcpy(nc, acc->cubes, acc->n * sizeof(VCCube));
        acc->cubes = nc; acc->cap = ncap;
    }
    VCTerm **copy = (VCTerm **)arena_alloc(acc->arena, (n ? n : 1) * sizeof(VCTerm *));
    if (n) memcpy(copy, lits, n * sizeof(VCTerm *));
    acc->cubes[acc->n].lits = copy;
    acc->cubes[acc->n].n    = n;
    acc->n++;
}

/* Depth-first product expansion.  `conj` holds the literals chosen so far;
 * `pending` is the remaining conjuncts to expand.  A `false` literal prunes
 * the branch (that cube is trivially unsat, so it does not need to be
 * recorded -- an unsat cube is exactly what we want anyway). */
static void expand(CubeAcc *acc, VCTerm **pending, uint32_t n_pending,
                   VCTerm **conj, uint32_t n_conj) {
    if (acc->overflow) return;
    if (acc->depth >= REFINE_MAX_EXPAND_DEPTH) {
        g_caps.expand_depth_hits++;
        refine_cap_peak(&g_caps.expand_depth_peak, REFINE_MAX_EXPAND_DEPTH);
        acc->overflow = true; return;
    }
    acc->depth++;
    refine_cap_peak(&g_caps.expand_depth_peak, acc->depth);

    /* FLATTEN conjunctions first, so the disjunction scan below sees every
     * disjunct that is top-level in the FORMULA rather than top-level in this
     * array.  A `(and A (or B C))` conjunct hides its `or` from a scan that
     * only inspects `pending[i]->op`, and the collect step below used to
     * handle that by re-entering with the same pending list -- which made the
     * identical decision on the next call and recursed forever.  A three-line
     * program with an ordinary disjunctive postcondition crashed the compiler
     * with a stack overflow.
     *
     * Splicing the conjunction's children in place is what makes each frame
     * PROGRESS: one boolean node is removed from the structure every time. */
    for (uint32_t i = 0; i < n_pending; i++) {
        if (pending[i]->op != VC_AND) continue;
        VCTerm *t = pending[i];
        uint32_t cap = n_pending - 1 + t->n;
        if (cap > REFINE_MAX_CUBE_LITS * 4) {
            g_caps.cube_lits_hits++;
            refine_cap_peak(&g_caps.cube_lits_peak, cap);
            acc->overflow = true; acc->depth--; return;
        }
        VCTerm **flat = (VCTerm **)arena_alloc(acc->arena,
                                               (cap ? cap : 1) * sizeof(VCTerm *));
        uint32_t m = 0;
        for (uint32_t j = 0; j < n_pending; j++) {
            if (j == i) for (uint32_t d = 0; d < t->n; d++) flat[m++] = t->kids[d];
            else        flat[m++] = pending[j];
        }
        expand(acc, flat, m, conj, n_conj);
        acc->depth--;
        return;
    }

    /* UNIT PROPAGATION before any split.  A literal that is a top-level
     * conjunct of `pending` lands in every cube this frame produces, so for a
     * disjunction beside it:
     *
     *   - a disjunct that IS such a literal satisfies the disjunction outright.
     *     Every other branch would only add a literal to the cube that branch
     *     produces, and a cube that is unsat stays unsat with more literals --
     *     so the disjunction is dropped with no split.
     *   - a disjunct that is the COMPLEMENT of such a literal can only produce
     *     a cube holding `p` and `(not p)`, which is unsatisfiable; it is
     *     skipped exactly as a `false` literal's cube is below.
     *
     * Terms are hash-consed, so both tests are pointer comparisons.  Without
     * this, a chain of `iff`s -- the equations a Bool `^reflect` measure
     * unfolds into, `(p1 => p0) and (p0 => p1)` per step -- doubled the cube
     * count twice per link and a four-element ground list overflowed
     * REFINE_MAX_CUBES (docs/archive/reflect-bool-measure-cube-blowup.md),
     * though one literal settles each link.  A disjunction with one live
     * disjunct left is chosen first: it is a unit, splitting it costs nothing,
     * and the literal it adds feeds the next one. */
    {
        int32_t pick = -1, pick_live = 0;
        for (uint32_t i = 0; i < n_pending; i++) {
            VCTerm *t = pending[i];
            if (t->op != VC_OR) continue;
            bool sat = false;
            int32_t live = 0;
            for (uint32_t d = 0; d < t->n && !sat; d++) {
                VCTerm *k = t->kids[d];
                bool dead = (k->op == VC_FALSE);
                if (k->op == VC_TRUE) sat = true;
                for (uint32_t j = 0; j < n_pending && !sat && !dead; j++) {
                    VCTerm *u = pending[j];
                    if (j == i || u->op == VC_OR || u->op == VC_AND) continue;
                    if (u == k) sat = true;
                    else if ((u->op == VC_NOT && u->kids[0] == k) ||
                             (k->op == VC_NOT && k->kids[0] == u)) dead = true;
                }
                if (!dead) live++;
            }
            if (sat || live == 0) {
                /* Satisfied: drop it.  No live disjunct: every cube below is
                 * unsat, so the whole frame contributes none. */
                if (live == 0 && !sat) { acc->depth--; return; }
                VCTerm **rest = (VCTerm **)arena_alloc(acc->arena,
                                                       (n_pending ? n_pending : 1) * sizeof(VCTerm *));
                uint32_t m = 0;
                for (uint32_t j = 0; j < n_pending; j++) if (j != i) rest[m++] = pending[j];
                expand(acc, rest, m, conj, n_conj);
                acc->depth--;
                return;
            }
            if (pick < 0 || live < pick_live) { pick = (int32_t)i; pick_live = live; }
        }
        if (pick >= 0) {
            VCTerm *t = pending[pick];
            VCTerm **rest = (VCTerm **)arena_alloc(acc->arena,
                                                   (n_pending ? n_pending : 1) * sizeof(VCTerm *));
            uint32_t m = 0;
            for (uint32_t j = 0; j < n_pending; j++) if (j != (uint32_t)pick) rest[m++] = pending[j];
            for (uint32_t d = 0; d < t->n && !acc->overflow; d++) {
                VCTerm *k = t->kids[d];
                if (k->op == VC_FALSE) continue;
                bool dead = false;
                for (uint32_t j = 0; j < m && !dead; j++) {
                    VCTerm *u = rest[j];
                    if (u->op == VC_OR || u->op == VC_AND) continue;
                    if ((u->op == VC_NOT && u->kids[0] == k) ||
                        (k->op == VC_NOT && k->kids[0] == u)) dead = true;
                }
                if (dead) continue;
                VCTerm **nxt = (VCTerm **)arena_alloc(acc->arena, (m + 1) * sizeof(VCTerm *));
                if (m) memcpy(nxt, rest, m * sizeof(VCTerm *));
                nxt[m] = k;
                expand(acc, nxt, m + 1, conj, n_conj);
            }
            acc->depth--;
            return;
        }
    }

    /* No disjunctions left: everything pending is a literal (or a conjunction
     * already flattened by vc_mk).  Collect into one cube. */
    uint32_t total = n_conj + n_pending;
    refine_cap_peak(&g_caps.cube_lits_peak, total);
    if (total > REFINE_MAX_CUBE_LITS) {
        g_caps.cube_lits_hits++;
        acc->overflow = true; acc->depth--; return;
    }

    VCTerm **lits = (VCTerm **)arena_alloc(acc->arena, (total ? total : 1) * sizeof(VCTerm *));
    uint32_t k = 0;
    for (uint32_t i = 0; i < n_conj; i++) lits[k++] = conj[i];
    bool has_false = false;
    for (uint32_t i = 0; i < n_pending; i++) {
        VCTerm *x = pending[i];
        /* Both passes above ran to completion, so nothing here is an AND or an
         * OR.  If that ever stops holding, bail to Unknown rather than record
         * a boolean as though it were a literal -- a cube holding an
         * unsatisfied OR would read as more constrained than it is, and a
         * theory stage could then call it unsat and prove the goal. */
        if (x->op == VC_AND || x->op == VC_OR) { acc->overflow = true; acc->depth--; return; }
        if (x->op == VC_FALSE) { has_false = true; continue; }
        if (x->op == VC_TRUE)  continue;
        lits[k++] = x;
    }
    /* A cube containing `false` is already unsatisfiable, and validity needs
     * every cube to be unsatisfiable -- so dropping it is correct (recording
     * it as an empty cube would instead read as "no constraints", which is
     * satisfiable and would wrongly block the proof). */
    if (has_false) { acc->depth--; return; }
    acc_push(acc, lits, k);
    acc->depth--;
}

/* refine-chain-expands-the-same-dnf-four-times: build once per chain run.
 * See the RefineCubeCache comment in refine_solver.h for why this is a local
 * rather than a cache on the RefineVC, and why it is lazy. */
bool refine_cubes_get(RefineVC *vc, Arena *a, RefineCubeCache *cc,
                      const VCCubeSet **out) {
    if (!cc->tried) {
        cc->tried = true;
        cc->ok = refine_cubes_build(vc, a, &cc->cs);
    }
    *out = &cc->cs;
    return cc->ok;
}

bool refine_cubes_build(RefineVC *vc, Arena *a, VCCubeSet *out) {
    memset(out, 0, sizeof(*out));
    if (!vc || !vc->goal) { out->overflow = true; return false; }

    /* refutation formula: hyps AND (not goal) */
    uint32_t n = vc->n_hyps + 1;
    VCTerm **parts = (VCTerm **)arena_alloc(a, n * sizeof(VCTerm *));
    for (uint32_t i = 0; i < vc->n_hyps; i++) parts[i] = nnf(vc, vc->hyps[i], false);
    parts[vc->n_hyps] = nnf(vc, vc->goal, true);

    /* An immediate `false` anywhere means the refutation is trivially
     * unsatisfiable -- the goal is valid with no cube work at all. */
    for (uint32_t i = 0; i < n; i++) {
        if (!parts[i]) { out->overflow = true; return false; }
        if (parts[i]->op == VC_FALSE) { out->trivial = true; return true; }
    }

    CubeAcc acc; memset(&acc, 0, sizeof(acc));
    acc.arena = a;
    expand(&acc, parts, n, NULL, 0);
    if (acc.overflow) { out->overflow = true; return false; }
    refine_cap_peak(&g_caps.cubes_peak, acc.n);
    out->cubes = acc.cubes;
    out->n     = acc.n;
    return true;
}

/* ------------------------------------------------------------------------- *
 * Bounded counterexample search
 *
 * The proving stages only ever answer Valid or Unknown -- they refute the
 * negated goal, and failing to refute it proves nothing either way.  To say
 * INVALID we need an actual witness, so we build one the honest way: pick a
 * small candidate value set (the literals that appear in the VC, their
 * neighbours, and a handful of small integers), enumerate assignments, and
 * EVALUATE `hyps AND (not goal)` exactly.  An assignment that satisfies it is
 * a genuine counterexample -- no approximation is involved -- which is why
 * this is allowed to produce RT_INVALID.
 *
 * Scope is deliberately tiny: integer variables only, no uninterpreted
 * symbols (a measure has no fixed interpretation to evaluate), at most
 * MODEL_MAX_VARS of them.  Everything else simply finds nothing, and the
 * obligation stays Unknown -> runtime check.
 * ------------------------------------------------------------------------- */

/* MODEL_MAX_VARS lives in refine_solver.h beside the other capped quantities,
 * so the TUR_REFINE_STATS reporter can name the limit its peak is a peak of.
 * MODEL_MAX_CANDS stays here and is uninstrumented: it bounds the candidate
 * VALUE set rather than a structural quantity, and nothing has asked. */
#define MODEL_MAX_CANDS 16

/* A candidate or evaluated value.  Ints stay exact int64 (overflow-checked);
 * anything touching a real is evaluated in DOUBLE, which is not an
 * approximation of the runtime check but exactly it: the emitted contract
 * evaluates the predicate in C double arithmetic, so a double-evaluated
 * witness is one the program would reject.  Bools ride in `i` as 0 / 1. */
typedef struct { bool is_real; int64_t i; double r; } MVal;

/* reflected-measures RF6: an application's value is READ OFF ITS OWN
 * DEFINITIONAL EQUATION.  The encoder asserts `f(t) = <body at t>` (or the
 * iff pair for a Bool measure) for every reflected application it unfolded;
 * the search collects those into a table and evaluates `f(t)` as the right
 * hand side.  An application with no entry is a free value, and the search
 * declines rather than guess -- that is what keeps a refutation genuine.
 * Congruence needs no separate check under the VC's gate
 * (RefineVC.reflect_model_ok): every other ufunc is a constructor, distinct
 * ground constructor terms are distinct values, and two applications of one
 * measure to the same term are one hash-consed VCTerm. */
#define MODEL_MAX_DEFS  256
#define MODEL_DEF_DEPTH 64

typedef struct {
    const VCTerm *app;   /* a VC_APP of a reflected measure */
    const VCTerm *def;   /* what it equals (Int/Real) / is equivalent to (Bool) */
} ModelDef;

typedef struct {
    const MVal     *vals;   /* one per VC variable */
    bool            fail;   /* evaluation hit overflow / an opaque term */
    const ModelDef *defs;
    uint32_t        n_defs;
    uint32_t        depth;  /* definitional recursion guard */
} EvalCtx;

static const VCTerm *model_def_of(const EvalCtx *E, const VCTerm *app) {
    for (uint32_t i = 0; i < E->n_defs; i++)
        if (E->defs[i].app == app) return E->defs[i].def;
    return NULL;
}

static inline RefineVC *vc_mutable(const RefineVC *vc) { return (RefineVC *)vc; }

/* An implication as the builder stores it: `(=> a b)` is normalized to
 * `(or (not a) b)` at construction, so both spellings are read. */
static bool model_impl_parts(const VCTerm *t, const VCTerm **a, const VCTerm **b) {
    if (!t) return false;
    if (t->op == VC_IMPLIES && t->n == 2) { *a = t->kids[0]; *b = t->kids[1]; return true; }
    if (t->op == VC_OR && t->n == 2) {
        if (t->kids[0]->op == VC_NOT) { *a = t->kids[0]->kids[0]; *b = t->kids[1]; return true; }
        if (t->kids[1]->op == VC_NOT) { *a = t->kids[1]->kids[0]; *b = t->kids[0]; return true; }
    }
    return false;
}

/* Collect the definitional equations from the hypotheses: `(= app rhs)` for
 * a value measure, the iff pair for a Bool one (`p <=> true` folds to the
 * bare atom and `p <=> false` to its negation at construction) -- exactly
 * the shapes refine_collect.c's rf_def asserts.  Only a REFLECTED symbol's
 * application is admitted as a definition; an equation that happens to
 * mention a constructor application defines nothing. */
static uint32_t model_collect_defs(const RefineVC *vc, ModelDef *out, uint32_t cap) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < vc->n_hyps && n < cap; i++) {
        const VCTerm *h = vc->hyps[i];
        const VCTerm *app = NULL, *def = NULL;
        if (h->op == VC_EQ && h->n == 2) {
            if (h->kids[0]->op == VC_APP)      { app = h->kids[0]; def = h->kids[1]; }
            else if (h->kids[1]->op == VC_APP) { app = h->kids[1]; def = h->kids[0]; }
        } else if (h->op == VC_APP && h->sort == VS_BOOL) {
            app = h; def = vc_bool(vc_mutable(vc), true);
        } else if (h->op == VC_NOT && h->n == 1 && h->kids[0]->op == VC_APP &&
                   h->kids[0]->sort == VS_BOOL) {
            app = h->kids[0]; def = vc_bool(vc_mutable(vc), false);
        } else if (h->op == VC_AND && h->n == 2) {
            const VCTerm *a1, *b1, *a2, *b2;
            if (model_impl_parts(h->kids[0], &a1, &b1) &&
                model_impl_parts(h->kids[1], &a2, &b2) && a1 == b2 && b1 == a2) {
                if (a1->op == VC_APP)      { app = a1; def = b1; }
                else if (b1->op == VC_APP) { app = b1; def = a1; }
            }
        }
        if (!app || !def) continue;
        if (app->as.idx >= vc->n_ufuncs || !vc->ufuncs[app->as.idx].reflected) continue;
        if (def == app) continue;
        bool dup = false;
        for (uint32_t j = 0; j < n; j++) if (out[j].app == app) { dup = true; break; }
        if (dup) continue;
        out[n].app = app; out[n].def = def; n++;
    }
    return n;
}

static MVal eval_arith(EvalCtx *E, const VCTerm *t);

static inline double mv_d(MVal v) { return v.is_real ? v.r : (double)v.i; }

static bool eval_bool(EvalCtx *E, const VCTerm *t) {
    if (E->fail) return false;
    switch (t->op) {
        case VC_TRUE:  return true;
        case VC_FALSE: return false;
        case VC_VAR:
            if (t->sort != VS_BOOL) { E->fail = true; return false; }
            return E->vals[t->as.idx].i != 0;
        case VC_NOT:   return !eval_bool(E, t->kids[0]);
        case VC_AND:
            for (uint32_t i = 0; i < t->n; i++) if (!eval_bool(E, t->kids[i])) return false;
            return true;
        case VC_OR:
            for (uint32_t i = 0; i < t->n; i++) if (eval_bool(E, t->kids[i])) return true;
            return false;
        case VC_EQ: case VC_LT: case VC_LE: {
            if (t->kids[0]->sort == VS_BOOL || t->kids[1]->sort == VS_BOOL) {
                /* (= p q) over propositions: iff. */
                if (t->op != VC_EQ) { E->fail = true; return false; }
                bool a = eval_bool(E, t->kids[0]), b = eval_bool(E, t->kids[1]);
                return !E->fail && a == b;
            }
            MVal a = eval_arith(E, t->kids[0]);
            MVal b = eval_arith(E, t->kids[1]);
            if (E->fail) return false;
            if (a.is_real || b.is_real) {
                double x = mv_d(a), y = mv_d(b);
                return t->op == VC_EQ ? (x == y) : t->op == VC_LT ? (x < y) : (x <= y);
            }
            return t->op == VC_EQ ? (a.i == b.i) : t->op == VC_LT ? (a.i < b.i) : (a.i <= b.i);
        }
        case VC_IMPLIES: {
            bool a = eval_bool(E, t->kids[0]);
            if (E->fail) return false;
            return !a || eval_bool(E, t->kids[1]);
        }
        case VC_APP: {
            /* RF6: a reflected Bool measure at an unfolded application. */
            const VCTerm *d = t->sort == VS_BOOL ? model_def_of(E, t) : NULL;
            if (!d || E->depth >= MODEL_DEF_DEPTH) { E->fail = true; return false; }
            E->depth++;
            bool r = eval_bool(E, d);
            E->depth--;
            return r;
        }
        default:
            E->fail = true;   /* an opaque proposition: cannot evaluate */
            return false;
    }
}

static MVal mv_int(int64_t i)  { MVal v = { false, i, 0.0 }; return v; }
static MVal mv_real(double r)  { MVal v = { true, 0, r };    return v; }

static MVal eval_arith(EvalCtx *E, const VCTerm *t) {
    MVal a, b; int64_t r = 0;
    if (E->fail) return mv_int(0);
    switch (t->op) {
        case VC_CONST_INT:  return mv_int(t->as.i);
        case VC_CONST_REAL: return mv_real(t->as.r);
        case VC_VAR:
            if (t->sort == VS_BOOL) { E->fail = true; return mv_int(0); }
            return E->vals[t->as.idx];
        case VC_NEG:
            a = eval_arith(E, t->kids[0]);
            if (E->fail) return mv_int(0);
            if (a.is_real) return mv_real(-a.r);
            if (__builtin_sub_overflow((int64_t)0, a.i, &r)) { E->fail = true; return mv_int(0); }
            return mv_int(r);
        case VC_ADD: case VC_SUB: case VC_MUL: case VC_DIV: case VC_MOD:
            a = eval_arith(E, t->kids[0]);
            b = eval_arith(E, t->kids[1]);
            if (E->fail) return mv_int(0);
            if (a.is_real || b.is_real) {
                /* C promotes the int operand; `mod` has no double form in the
                 * predicate language, and a zero divisor would give an
                 * inf/nan the fragment does not model -- decline both. */
                double x = mv_d(a), y = mv_d(b);
                switch (t->op) {
                    case VC_ADD: return mv_real(x + y);
                    case VC_SUB: return mv_real(x - y);
                    case VC_MUL: return mv_real(x * y);
                    case VC_DIV: if (y == 0.0) { E->fail = true; return mv_int(0); }
                                 return mv_real(x / y);
                    default:     E->fail = true; return mv_int(0);
                }
            }
            switch (t->op) {
                case VC_ADD: if (__builtin_add_overflow(a.i, b.i, &r)) E->fail = true; return mv_int(r);
                case VC_SUB: if (__builtin_sub_overflow(a.i, b.i, &r)) E->fail = true; return mv_int(r);
                case VC_MUL: if (__builtin_mul_overflow(a.i, b.i, &r)) E->fail = true; return mv_int(r);
                case VC_DIV: if (b.i == 0 || (a.i == INT64_MIN && b.i == -1)) { E->fail = true; return mv_int(0); }
                             return mv_int(a.i / b.i);
                default:     if (b.i == 0 || (a.i == INT64_MIN && b.i == -1)) { E->fail = true; return mv_int(0); }
                             return mv_int(a.i % b.i);
            }
        case VC_APP: {
            /* RF6: a reflected Int/Real measure at an unfolded application. */
            const VCTerm *d = t->sort != VS_BOOL ? model_def_of(E, t) : NULL;
            if (!d || E->depth >= MODEL_DEF_DEPTH) { E->fail = true; return mv_int(0); }
            E->depth++;
            MVal v = eval_arith(E, d);
            E->depth--;
            return v;
        }
        default:
            E->fail = true;   /* an opaque term */
            return mv_int(0);
    }
}

/* Every numeric literal in the VC, as a double, for the candidate sets. */
static void collect_lits(const VCTerm *t, MVal *out, uint32_t *n, uint32_t cap) {
    if (!t || *n >= cap) return;
    if (t->op == VC_CONST_INT || t->op == VC_CONST_REAL) {
        MVal v = t->op == VC_CONST_INT ? mv_int(t->as.i) : mv_real(t->as.r);
        for (uint32_t i = 0; i < *n; i++)
            if (out[i].is_real == v.is_real && (v.is_real ? out[i].r == v.r : out[i].i == v.i)) return;
        out[(*n)++] = v;
        return;
    }
    for (uint32_t i = 0; i < t->n; i++) collect_lits(t->kids[i], out, n, cap);
}

static void add_cand_int(MVal *c, uint32_t *n, int64_t v) {
    if (*n >= MODEL_MAX_CANDS) return;
    for (uint32_t i = 0; i < *n; i++) if (c[i].i == v) return;
    c[(*n)++] = mv_int(v);
}
static void add_cand_real(MVal *c, uint32_t *n, double v) {
    if (*n >= MODEL_MAX_CANDS) return;
    for (uint32_t i = 0; i < *n; i++) if (c[i].r == v) return;
    c[(*n)++] = mv_real(v);
}

RefineModel *refine_model_search(RefineVC *vc, Arena *a) {
    if (!vc || !vc->goal) return NULL;
    /* Measures have no fixed meaning -- unless every symbol here is a
     * constructor or a reflected measure the encoder unfolded (RF6), in
     * which case each application has a definitional equation to read. */
    if (vc->n_ufuncs > 0 && !vc->reflect_model_ok) return NULL;
    ModelDef defs[MODEL_MAX_DEFS];
    uint32_t n_defs = vc->n_ufuncs > 0 ? model_collect_defs(vc, defs, MODEL_MAX_DEFS) : 0;
    /* A hypothesis the encoder left out would make any witness suspect: the
     * dropped fact may exclude it.  Declining costs a refutation, never a
     * proof -- and the alternative was a hard error on a correct program. */
    if (vc->hyps_dropped) return NULL;
    /* n_vars == 0 is allowed and is the IMPORTANT case: a call site with
     * literal arguments (`(safe-div 10 0)`) has a closed goal, so one
     * evaluation decides it outright.  The odometer below runs exactly once and
     * the model is empty -- there is nothing to bind, the arguments already
     * say it. */
    /* Past the uninterpreted-symbol gate, so this VC could plausibly use the
     * search at SOME cap -- which is what makes its width worth recording.
     * The peak is real, not saturating: n_vars is known before the check. */
    refine_cap_peak(&g_caps.model_vars_peak, vc->n_vars);
    if (vc->n_vars > MODEL_MAX_VARS) {
        g_caps.model_vars_hits++;
        /* Every sort has a candidate set now (Int, Real and Bool -- the sort
         * gate that used to exclude Real and Bool variables is gone), so a
         * VC turned away here WOULD run at a higher cap. */
        g_caps.model_vars_would_run++;
        return NULL;
    }

    /* Per-variable candidates.  Ints: the integer literals in the VC, their
     * immediate neighbours, and a few small integers around zero.  Reals: the
     * numeric literals (of either kind) as doubles, each with a half-unit
     * either side, and a few small values; the runtime evaluates in double,
     * so a real witness is exactly one the program would reject.  Bools:
     * false and true. */
    MVal lits[MODEL_MAX_CANDS]; uint32_t n_lits = 0;
    for (uint32_t i = 0; i < vc->n_hyps; i++)
        collect_lits(vc->hyps[i], lits, &n_lits, MODEL_MAX_CANDS);
    collect_lits(vc->goal, lits, &n_lits, MODEL_MAX_CANDS);

    MVal     cand[MODEL_MAX_VARS][MODEL_MAX_CANDS];
    uint32_t n_cand[MODEL_MAX_VARS];
    uint32_t nv = vc->n_vars;
    bool is_const[MODEL_MAX_VARS];
    for (uint32_t v = 0; v < nv; v++) {
        uint32_t *n = &n_cand[v]; *n = 0;
        MVal *c = cand[v];
        /* RF6: a nullary constructor (`Nil`) is a free constant.  One fixed
         * value no literal can collide with, distinct per constant -- not a
         * dimension of the search, and not a binding worth printing. */
        is_const[v] = false;
        for (uint32_t j = 0; j < vc->n_ctor_consts; j++)
            if (vc->ctor_consts[j] == v) { is_const[v] = true; break; }
        if (is_const[v]) { add_cand_int(c, n, INT64_MIN / 2 + (int64_t)v); continue; }
        switch (vc->vars[v].sort) {
            case VS_BOOL:
                add_cand_int(c, n, 0); add_cand_int(c, n, 1);
                break;
            case VS_INT:
                for (int64_t k = -2; k <= 2; k++) add_cand_int(c, n, k);
                for (uint32_t i = 0; i < n_lits; i++) {
                    if (lits[i].is_real) continue;
                    add_cand_int(c, n, lits[i].i);
                    if (lits[i].i < INT64_MAX) add_cand_int(c, n, lits[i].i + 1);
                    if (lits[i].i > INT64_MIN) add_cand_int(c, n, lits[i].i - 1);
                }
                break;
            case VS_REAL:
                add_cand_real(c, n, 0.0);
                add_cand_real(c, n, 0.5);  add_cand_real(c, n, -0.5);
                add_cand_real(c, n, 1.0);  add_cand_real(c, n, -1.0);
                for (uint32_t i = 0; i < n_lits; i++) {
                    double d = mv_d(lits[i]);
                    add_cand_real(c, n, d);
                    add_cand_real(c, n, d + 0.5);
                    add_cand_real(c, n, d - 0.5);
                }
                break;
        }
        if (*n == 0) return NULL;
    }

    /* The cap that binds: the odometer runs prod(n_cand) full evaluations.
     * Past the budget we decline rather than start something that might not
     * finish in compile-time noise; a decline here is one a bigger budget
     * WOULD have let run, which is what the counter says. */
    {
        uint64_t evals = 1;
        for (uint32_t v = 0; v < nv && evals <= MODEL_MAX_EVALS; v++) evals *= n_cand[v];
        if (evals > MODEL_MAX_EVALS) { g_caps.model_evals_hits++; return NULL; }
    }

    MVal     vals[MODEL_MAX_VARS];
    uint32_t idx[MODEL_MAX_VARS] = {0};

    for (;;) {
        for (uint32_t v = 0; v < nv; v++) vals[v] = cand[v][idx[v]];

        EvalCtx E = { vals, false, defs, n_defs, 0 };
        bool sat = true;
        for (uint32_t i = 0; i < vc->n_hyps && sat; i++)
            if (!eval_bool(&E, vc->hyps[i])) sat = false;
        if (sat && !E.fail && !eval_bool(&E, vc->goal) && !E.fail) {
            RefineModel *m = (RefineModel *)arena_alloc(a, sizeof(RefineModel));
            uint32_t n_shown = 0;
            for (uint32_t v = 0; v < nv; v++) if (!is_const[v]) n_shown++;
            m->n = n_shown;
            m->bindings = n_shown ? (RefineModelBinding *)arena_alloc(
                                        a, n_shown * sizeof(RefineModelBinding)) : NULL;
            for (uint32_t v = 0, k = 0; v < nv; v++) {
                if (is_const[v]) continue;   /* RF6: a constructor constant */
                m->bindings[k].name    = vc->vars[v].name;
                m->bindings[k].is_real = vc->vars[v].sort == VS_REAL;
                m->bindings[k].is_bool = vc->vars[v].sort == VS_BOOL;
                m->bindings[k].ival    = vals[v].is_real ? 0 : vals[v].i;
                m->bindings[k].rval    = vals[v].is_real ? vals[v].r : 0.0;
                k++;
            }
            return m;
        }
        /* An assignment that could not be evaluated (overflow, a zero
         * divisor) is skipped, not fatal: the next one may be a witness.
         * Only a formula that fails on its FIRST assignment is given up on,
         * since that is the opaque-term case and every assignment would fail
         * the same way. */
        if (E.fail) {
            bool first = true;
            for (uint32_t v = 0; v < nv; v++) if (idx[v]) { first = false; break; }
            if (first) return NULL;
        }

        /* odometer */
        uint32_t k = 0;
        while (k < nv && ++idx[k] >= n_cand[k]) { idx[k] = 0; k++; }
        if (k == nv) break;
    }
    return NULL;
}
