/* refine_solver_arith.c -- S2: linear arithmetic.
 *
 * The plan's S2a target is difference logic (`x - y <= c`) via Bellman-Ford,
 * with full simplex LRA as S2b.  This implementation takes a different route
 * to the same coverage band: FOURIER-MOTZKIN elimination over exact rationals.
 * It is a strict superset of difference logic (so the S2a target -- index and
 * bounds reasoning, `0 <= i`, `i < len`, `i + 1 <= n` -- is covered), it
 * decides conjunctions of linear constraints over the rationals outright, and
 * it is a few hundred lines rather than the multi-week incremental-simplex
 * investment.  Its weakness is combinatorial blow-up, which is exactly what
 * the RT_UNKNOWN escape hatch absorbs: REFINE_MAX_LA_CONSTR bounds the growth
 * and a cap hit degrades to a runtime check.  If profiling ever shows the cap
 * biting on real obligations, the Dutertre--de Moura simplex slots in behind
 * the same la_* interface.
 *
 * Integers: rational unsatisfiability implies integer unsatisfiability, so
 * every refutation found here is sound for `int` too.  On top of that the
 * integer-sorted constraints get the Omega test's two cheap exact steps
 * (docs/archive/solver-integer-tail-plan.md):
 *   - NORMALIZATION (`int_normalize`): clear denominators, divide by the gcd
 *     of the coefficients, round the bound to the integer hull.  Subsumes the
 *     strict tightening (`e < 0` -> `e <= -1`) that lets
 *     `(> x 0) |= (> (* x 2) 0)` go through, and adds `2q >= -1 |= q >= 0`.
 *   - EQUALITY ELIMINATION (`eq_eliminate`): the gcd divisibility test on
 *     every equation (`2x = 2y + 1` is unsat over the integers -- parity),
 *     then substitution through any unit-coefficient variable, so the test
 *     re-runs on what the substitution produces.
 * Both are equivalences over the integers, so the set keeps exactly its
 * integer models and the refuter only gets stronger.  What is still NOT here
 * is the rest of the S2c tail: Pugh's sigma-substitution for equations with
 * no unit coefficient, and the dark/grey shadows (branch-and-bound) for
 * inequality systems that are rationally feasible but integer-infeasible.
 *
 * PURIFICATION.  Any term the linear encoder cannot see inside -- a VC_VAR, an
 * uninterpreted application (a measure, or a nonlinear product already
 * abstracted by the RT2 encoder) -- becomes an opaque LA variable.  That is
 * the Nelson-Oppen purification step, and it is what lets S3 exchange
 * equalities with EUF over exactly these shared terms. */

#include "refine_solver.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- *
 * Exact rationals (int64 num/den; every step overflow-checked -> give up)
 * ------------------------------------------------------------------------- */

typedef struct { int64_t num, den; bool bad; } Rat;

static int64_t igcd(int64_t a, int64_t b) {
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    while (b) { int64_t t = a % b; a = b; b = t; }
    return a ? a : 1;
}

static Rat rat_bad(void)        { Rat r = {0, 1, true}; return r; }
static Rat rat_of(int64_t v)    { Rat r = {v, 1, false}; return r; }

/* Normalize n/d.  Every arithmetic step below is overflow-checked and falls
 * back to `bad` rather than wrapping: a wrapped coefficient could manufacture
 * a refutation that does not exist, which would break the soundness
 * invariant.  A `bad` rational drops its constraint instead, which can only
 * ever weaken the refutation. */
static Rat rat_norm(int64_t n, int64_t d) {
    if (d == 0) return rat_bad();
    if (d < 0) {
        if (n == INT64_MIN || d == INT64_MIN) return rat_bad();
        n = -n; d = -d;
    }
    int64_t g = igcd(n, d);
    return (Rat){ n / g, d / g, false };
}

static Rat rat_add(Rat a, Rat b) {
    if (a.bad || b.bad) return rat_bad();
    int64_t t1, t2, num, den;
    if (__builtin_mul_overflow(a.num, b.den, &t1)) return rat_bad();
    if (__builtin_mul_overflow(b.num, a.den, &t2)) return rat_bad();
    if (__builtin_add_overflow(t1, t2, &num))      return rat_bad();
    if (__builtin_mul_overflow(a.den, b.den, &den)) return rat_bad();
    return rat_norm(num, den);
}
static Rat rat_mul(Rat a, Rat b) {
    if (a.bad || b.bad) return rat_bad();
    /* Cross-cancel first so ordinary coefficients never approach the limit. */
    int64_t g1 = igcd(a.num, b.den), g2 = igcd(b.num, a.den);
    int64_t an = a.num / g1, bd = b.den / g1;
    int64_t bn = b.num / g2, ad = a.den / g2;
    int64_t num, den;
    if (__builtin_mul_overflow(an, bn, &num)) return rat_bad();
    if (__builtin_mul_overflow(ad, bd, &den)) return rat_bad();
    return rat_norm(num, den);
}
static Rat rat_neg(Rat a)  { if (a.bad || a.num == INT64_MIN) return rat_bad(); a.num = -a.num; return a; }
static bool rat_zero(Rat a){ return !a.bad && a.num == 0; }
static bool rat_pos(Rat a) { return !a.bad && a.num > 0; }
static bool rat_neg_p(Rat a){ return !a.bad && a.num < 0; }
static bool rat_is_int(Rat a){ return !a.bad && a.den == 1; }
/* Exact floor / ceil of a rational (den > 0 by rat_norm). */
static int64_t rat_floor(Rat a) {
    int64_t q = a.num / a.den, r = a.num % a.den;
    return (r != 0 && a.num < 0) ? q - 1 : q;
}
static int64_t rat_ceil(Rat a) {
    int64_t q = a.num / a.den, r = a.num % a.den;
    return (r != 0 && a.num > 0) ? q + 1 : q;
}
/* gcd with gcd(0, x) = |x|, unlike igcd's "1 for the empty case" -- the
 * integer normalization below accumulates over the nonzero coefficients and
 * needs the identity element to be 0. */
static int64_t igcd0(int64_t a, int64_t b) {
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    while (b) { int64_t t = a % b; a = b; b = t; }
    return a;
}
/* lcm of two positive int64s; 0 on overflow. */
static int64_t ilcm(int64_t a, int64_t b) {
    int64_t g = igcd0(a, b), r;
    if (g == 0) return 0;
    if (__builtin_mul_overflow(a / g, b, &r)) return 0;
    return r;
}

/* Convert a double literal to an exact rational when it is safely
 * representable; otherwise mark it bad so the constraint is dropped (dropping
 * a constraint only weakens the refutation -- it can never manufacture one). */
static Rat rat_of_double(double d) {
    if (d != d || d > 1e15 || d < -1e15) return rat_bad();
    /* Try denominators up to 1e6; float refinements in practice are things
     * like 0.0, 0.5, 3.25. */
    for (int64_t den = 1; den <= 1000000; den *= 10) {
        double scaled = d * (double)den;
        double r = scaled < 0 ? -scaled : scaled;
        if (r < 9e18 && scaled == (double)(int64_t)scaled)
            return rat_norm((int64_t)scaled, den);
    }
    return rat_bad();
}

/* ------------------------------------------------------------------------- *
 * Linear expressions and constraints
 * ------------------------------------------------------------------------- */

/* `coef[0..n_vars-1] . x + konst  {<,<=,=}  0`
 *
 * An equality is kept AS an equality (`is_eq`) rather than split into two
 * inequalities at assertion time, because the integer phase of `la_unsat`
 * needs to see it whole: the gcd divisibility test and the substitution step
 * both work on equations, and neither can be recovered from the pair.  The
 * split happens inside `la_unsat`, on the equalities the integer phase could
 * not eliminate, just before Fourier-Motzkin. */
typedef struct {
    Rat  coef[REFINE_MAX_LA_VARS];
    Rat  konst;
    bool strict;
    bool is_eq;
    bool bad;
} LinC;

struct LaState {
    RefineVC *vc;
    Arena    *a;
    VCTerm   *vars[REFINE_MAX_LA_VARS];   /* opaque terms, in registration order */
    uint32_t  n_vars;
    LinC     *cs;
    uint32_t  n_cs, cap_cs;
    bool      gave_up;   /* a cap was hit: never claim unsat afterwards */
};

/* A term the linear encoder must treat as opaque. */
bool la_is_shared_term(const VCTerm *t) {
    return t && (t->op == VC_VAR || t->op == VC_APP);
}

static uint32_t la_var(LaState *st, VCTerm *t) {
    for (uint32_t i = 0; i < st->n_vars; i++) if (st->vars[i] == t) return i;
    if (st->n_vars >= REFINE_MAX_LA_VARS) {
        refine_caps()->la_vars_hits++;
        refine_cap_peak(&refine_caps()->la_vars_peak, REFINE_MAX_LA_VARS);
        st->gave_up = true; return UINT32_MAX;
    }
    st->vars[st->n_vars] = t;
    refine_cap_peak(&refine_caps()->la_vars_peak, st->n_vars + 1);
    return st->n_vars++;
}

typedef struct {
    Rat  coef[REFINE_MAX_LA_VARS];
    Rat  konst;
    bool bad;
} LinExp;

static LinExp lin_zero(void) {
    LinExp e; memset(&e, 0, sizeof(e));
    for (uint32_t i = 0; i < REFINE_MAX_LA_VARS; i++) e.coef[i] = rat_of(0);
    e.konst = rat_of(0);
    e.bad = false;
    return e;
}

static LinExp lin_scale(LinExp e, Rat k) {
    if (e.bad || k.bad) { e.bad = true; return e; }
    for (uint32_t i = 0; i < REFINE_MAX_LA_VARS; i++) {
        e.coef[i] = rat_mul(e.coef[i], k);
        if (e.coef[i].bad) { e.bad = true; return e; }
    }
    e.konst = rat_mul(e.konst, k);
    if (e.konst.bad) e.bad = true;
    return e;
}

static LinExp lin_add(LinExp a, LinExp b) {
    if (a.bad || b.bad) { a.bad = true; return a; }
    for (uint32_t i = 0; i < REFINE_MAX_LA_VARS; i++) {
        a.coef[i] = rat_add(a.coef[i], b.coef[i]);
        if (a.coef[i].bad) { a.bad = true; return a; }
    }
    a.konst = rat_add(a.konst, b.konst);
    if (a.konst.bad) a.bad = true;
    return a;
}

/* Deepest arithmetic nesting linearize will walk before giving the whole
 * constraint up as `bad` (dropped -- which only ever weakens a refutation,
 * so the bound is sound by construction).  It exists because LinExp is
 * passed and returned BY VALUE: with 32 exact-rational coefficients a frame
 * carries several ~800-byte copies, and under ASan redzones the recursion
 * measured dead between depth 750 and 1000 on an 8 MB stack -- far below
 * the corpus reader's term-depth cap, which is what let a legal benchmark
 * crash the child.  500 sits under the measured floor with margin; a real
 * obligation deeper than this is FM blow-up bait anyway. */
#define LA_MAX_LINEARIZE_DEPTH 500

/* Linearize `t`.  Anything not built from +,-,unary-,literal, and
 * multiplication/division by a literal becomes an opaque variable. */
static LinExp linearize(LaState *st, VCTerm *t, uint32_t depth) {
    LinExp e = lin_zero();
    if (!t) { e.bad = true; return e; }
    if (depth > LA_MAX_LINEARIZE_DEPTH) { e.bad = true; return e; }

    /* A real-sorted `+ - * /` or negation rounds in double, so it is not the
     * field operation Fourier-Motzkin would read it as: opaque
     * (float-proofs-assume-exact-reals, refine_solver.h). */
    if (refine_real_arith(t)) goto opaque;

    switch (t->op) {
        case VC_CONST_INT:  e.konst = rat_of(t->as.i); return e;
        case VC_CONST_REAL: e.konst = rat_of_double(t->as.r);
                            if (e.konst.bad) e.bad = true;
                            return e;
        case VC_ADD: return lin_add(linearize(st, t->kids[0], depth + 1),
                                    linearize(st, t->kids[1], depth + 1));
        case VC_SUB: return lin_add(linearize(st, t->kids[0], depth + 1),
                                    lin_scale(linearize(st, t->kids[1], depth + 1),
                                              rat_of(-1)));
        case VC_NEG: return lin_scale(linearize(st, t->kids[0], depth + 1), rat_of(-1));
        case VC_MUL: {
            VCTerm *a = t->kids[0], *b = t->kids[1];
            VCTerm *lit = NULL, *other = NULL;
            if (a->op == VC_CONST_INT || a->op == VC_CONST_REAL) { lit = a; other = b; }
            else if (b->op == VC_CONST_INT || b->op == VC_CONST_REAL) { lit = b; other = a; }
            if (!lit) break;   /* nonlinear: fall through to opaque */
            Rat k = (lit->op == VC_CONST_INT) ? rat_of(lit->as.i) : rat_of_double(lit->as.r);
            if (k.bad) { e.bad = true; return e; }
            return lin_scale(linearize(st, other, depth + 1), k);
        }
        case VC_DIV: {
            VCTerm *b = t->kids[1];
            if (b->op != VC_CONST_INT && b->op != VC_CONST_REAL) break;
            Rat k = (b->op == VC_CONST_INT) ? rat_of(b->as.i) : rat_of_double(b->as.r);
            if (k.bad || rat_zero(k)) { e.bad = true; return e; }
            Rat inv = rat_norm(k.den, k.num);
            if (inv.bad) { e.bad = true; return e; }
            /* Integer division truncates, so `(/ x 2)` is NOT `x/2` over the
             * integers.  Only treat it as exact division when the term is
             * real-sorted; otherwise it stays opaque. */
            if (t->sort != VS_REAL) break;
            return lin_scale(linearize(st, t->kids[0], depth + 1), inv);
        }
        default: break;
    }

opaque:;
    /* Opaque: a variable, an application, a nonlinear product, `mod`, an
     * integer division, or real arithmetic.  This is the purification
     * boundary S3 shares. */
    uint32_t v = la_var(st, t);
    if (v == UINT32_MAX) { e.bad = true; return e; }
    e.coef[v] = rat_of(1);
    return e;
}

/* A variable-free constraint that cannot hold. */
static bool constant_false(const LinC *c, uint32_t n_vars) {
    for (uint32_t i = 0; i < n_vars; i++) if (!rat_zero(c->coef[i])) return false;
    if (c->konst.bad) return false;
    if (c->is_eq) return c->konst.num != 0;
    return c->strict ? (c->konst.num >= 0) : (c->konst.num > 0);
}

/* Overwrite `c` with a canonical variable-free contradiction (`0 < 0`), so
 * whatever scan runs next reports the set unsat.  Used when the integer
 * normalization finds an equality with no integer solution: the constraint
 * is not merely tight, it is impossible, and saying so in-band keeps
 * `la_entails_eq`'s truncate-and-retry discipline working unchanged. */
static void make_false(LinC *c) {
    for (uint32_t i = 0; i < REFINE_MAX_LA_VARS; i++) c->coef[i] = rat_of(0);
    c->konst  = rat_of(0);
    c->strict = true;
    c->is_eq  = false;
    c->bad    = false;
}

/* INTEGER NORMALIZATION -- the Omega test's "normalize" step, generalizing
 * the strict-to-non-strict tightening this file always did.
 *
 * For a constraint whose every variable with a nonzero coefficient is
 * int-sorted, the left-hand side is an integer whatever the variables take,
 * so the rational relaxation Fourier-Motzkin works on can be tightened to the
 * integer hull of the constraint without losing a single integer solution:
 *
 *     clear the coefficient denominators (scale by their lcm, positive);
 *     sum a_i x_i <  b   ==>   sum a_i x_i <= ceil(b) - 1
 *     sum a_i x_i <= b   ==>   sum a_i x_i <= floor(b)
 *     sum a_i x_i <= b   ==>   sum (a_i/g) x_i <= floor(b/g),  g = gcd(a_i)
 *
 * Every step is an EQUIVALENCE over the integers, which is what makes it
 * sound for a refuter: the set keeps exactly its integer models and the
 * refutation only gets easier.  For an EQUALITY the same step is the gcd
 * divisibility test -- `sum a_i x_i = b` has an integer solution only if g
 * divides b -- and a failed test means the whole cube is unsat (`2x = 2y + 1`
 * has rational solutions and no integer ones; this is what makes S2 decide
 * parity).  Returns false exactly in that case.
 *
 * Any overflow leaves the constraint as it was: unnormalized is still a
 * valid constraint, just a weaker one. */
static bool int_normalize(LaState *st, LinC *c) {
    if (c->bad || c->konst.bad) return true;
    int64_t L = 1;
    bool any = false;
    for (uint32_t i = 0; i < st->n_vars; i++) {
        if (rat_zero(c->coef[i])) continue;
        if (c->coef[i].bad) return true;
        if (st->vars[i]->sort != VS_INT) return true;   /* a real: no hull */
        any = true;
        L = ilcm(L, c->coef[i].den);
        if (!L) return true;
    }
    if (!any) return true;                               /* constant: see constant_false */

    Rat Lr = rat_of(L);
    Rat coef[REFINE_MAX_LA_VARS];
    int64_t g = 0;
    for (uint32_t i = 0; i < REFINE_MAX_LA_VARS; i++) coef[i] = rat_of(0);
    for (uint32_t i = 0; i < st->n_vars; i++) {
        coef[i] = rat_mul(c->coef[i], Lr);
        if (coef[i].bad) return true;
        g = igcd0(g, coef[i].num);
    }
    if (g <= 0) return true;
    /* sum coef x {<,<=,=} b, with b = -(L * konst). */
    Rat b = rat_neg(rat_mul(c->konst, Lr));
    if (b.bad) return true;

    int64_t bound;
    if (c->is_eq) {
        if (!rat_is_int(b)) { make_false(c); return false; }
        if (b.num % g != 0)  { make_false(c); return false; }
        bound = b.num / g;
    } else {
        int64_t bi = c->strict ? rat_ceil(b) : rat_floor(b);
        if (c->strict) { if (bi == INT64_MIN) return true; bi -= 1; }
        bound = bi / g;
        if ((bi % g) != 0 && bi < 0) bound--;              /* floor(bi / g) */
    }
    if (bound == INT64_MIN) return true;
    for (uint32_t i = 0; i < st->n_vars; i++) coef[i] = rat_of(coef[i].num / g);
    memcpy(c->coef, coef, sizeof(c->coef));
    c->konst  = rat_of(-bound);
    c->strict = false;
    return true;
}

static void la_push(LaState *st, LinExp e, bool strict, bool is_eq) {
    if (e.bad) return;              /* dropping a constraint only weakens us */
    if (st->n_cs >= REFINE_MAX_LA_CONSTR) {
        refine_caps()->la_constr_hits++;
        refine_cap_peak(&refine_caps()->la_constr_peak, REFINE_MAX_LA_CONSTR);
        st->gave_up = true; return;
    }
    refine_cap_peak(&refine_caps()->la_constr_peak, st->n_cs + 1);
    if (st->n_cs == st->cap_cs) {
        uint32_t ncap = st->cap_cs ? st->cap_cs * 2 : 16;
        LinC *nc = (LinC *)arena_alloc(st->a, ncap * sizeof(LinC));
        if (st->n_cs) memcpy(nc, st->cs, st->n_cs * sizeof(LinC));
        st->cs = nc; st->cap_cs = ncap;
    }
    LinC *c = &st->cs[st->n_cs++];
    memcpy(c->coef, e.coef, sizeof(c->coef));
    c->konst  = e.konst;
    c->strict = strict && !is_eq;
    c->is_eq  = is_eq;
    c->bad    = false;

    /* A variable-free equality is decided here: `0 = 0` says nothing and is
     * dropped, `0 = 5` is the contradiction constant_false reports. */
    if (is_eq) {
        bool any = false;
        for (uint32_t i = 0; i < st->n_vars; i++) if (!rat_zero(c->coef[i])) { any = true; break; }
        if (!any) { if (rat_zero(c->konst)) st->n_cs--; return; }
    }
    (void)int_normalize(st, c);   /* a failed gcd test left `0 < 0` in place */
}

/* ------------------------------------------------------------------------- *
 * Assertions
 * ------------------------------------------------------------------------- */

LaState *la_new(RefineVC *vc, Arena *a) {
    LaState *st = (LaState *)arena_alloc(a, sizeof(LaState));
    memset(st, 0, sizeof(*st));
    st->vc = vc; st->a = a;
    return st;
}

/* lhs <(=) rhs  ==>  lhs - rhs <(=) 0 */
bool la_assert_le(LaState *st, VCTerm *lhs, VCTerm *rhs, bool strict) {
    LinExp e = lin_add(linearize(st, lhs, 0),
                       lin_scale(linearize(st, rhs, 0), rat_of(-1)));
    la_push(st, e, strict, false);
    return !e.bad;
}

/* x == y  ==>  x - y = 0, kept as ONE equality so the integer phase of
 * la_unsat can run the gcd test and substitute through it. */
bool la_assert_eq(LaState *st, VCTerm *x, VCTerm *y) {
    LinExp e = lin_add(linearize(st, x, 0),
                       lin_scale(linearize(st, y, 0), rat_of(-1)));
    la_push(st, e, false, true);
    return !e.bad;
}

bool la_assert_cube(LaState *st, const VCCube *c) {
    for (uint32_t i = 0; i < c->n; i++) {
        VCTerm *lit = c->lits[i];
        VCTerm *at  = refine_lit_atom(lit);
        bool    neg = refine_lit_is_neg(lit);
        if (at->n != 2) continue;
        VCTerm *x = at->kids[0], *y = at->kids[1];
        if (!vc_is_arith(x) || !vc_is_arith(y)) continue;
        /* `!(x<y) => y<=x` is false when either side is a NaN; skip the
         * literal unless the cube rules that out (refine_solver.h). */
        if (neg && !(refine_cube_nan_free(c, x) && refine_cube_nan_free(c, y))) continue;
        switch (at->op) {
            case VC_LT:
                if (neg) la_assert_le(st, y, x, false);   /* !(x<y) => y<=x */
                else     la_assert_le(st, x, y, true);
                break;
            case VC_LE:
                if (neg) la_assert_le(st, y, x, true);    /* !(x<=y) => y<x */
                else     la_assert_le(st, x, y, false);
                break;
            case VC_EQ:
                /* A disequality carries no linear information on its own (it
                 * is a disjunction); S2 skips it, which is sound.  Integer
                 * case-splitting on it is the S2c tail. */
                if (!neg) la_assert_eq(st, x, y);
                break;
            default: break;
        }
    }
    return !st->gave_up;
}

/* ------------------------------------------------------------------------- *
 * Integer equality elimination (the Omega test's equality phase)
 * ------------------------------------------------------------------------- */

/* Eliminate the equalities in `cur[0..*n)` before Fourier-Motzkin sees the
 * set.  Works on la_unsat's private copy; `cur` has room for 2 * n_in entries
 * so an equality that has to be split into two inequalities fits.
 *
 * For each equality E:  sum a_i x_i + k = 0
 *   - all-integer with some |a_p| == 1: solve for x_p and SUBSTITUTE into
 *     every other constraint.  Coefficients stay integral, so the constraints
 *     it lands in re-normalize (gcd test included) -- which is how a parity
 *     contradiction two equations deep is found.
 *   - mixed real/int: Gaussian substitution on any nonzero pivot; exact over
 *     the rationals, no integer claims made.
 *   - all-integer with no unit coefficient (`2x + 3y = 1`): the gcd test has
 *     already run at push time; the equality is now read as the two
 *     inequalities it always was, which is sound and exactly as complete as
 *     before this phase existed.  Pugh's sigma-substitution would finish the
 *     job; see docs/archive/solver-integer-tail-plan.md.
 * Returns false when a contradiction surfaces (the set is unsat). */
static bool eq_eliminate(LaState *st, LinC *cur, uint32_t *n_io) {
    uint32_t n_vars = st->n_vars, n = *n_io;
    for (;;) {
        uint32_t e = n;
        for (uint32_t i = 0; i < n; i++) if (cur[i].is_eq) { e = i; break; }
        if (e == n) break;
        LinC E = cur[e];

        bool allint = true;
        for (uint32_t j = 0; j < n_vars; j++)
            if (!rat_zero(E.coef[j]) && st->vars[j]->sort != VS_INT) { allint = false; break; }

        uint32_t pivot = n_vars;
        for (uint32_t j = 0; j < n_vars; j++) {
            if (rat_zero(E.coef[j]) || E.coef[j].bad) continue;
            if (!allint) { pivot = j; break; }
            if (E.coef[j].den == 1 && (E.coef[j].num == 1 || E.coef[j].num == -1)) { pivot = j; break; }
        }

        if (pivot == n_vars) {
            /* Split into E <= 0 and -E <= 0 in place.  For an all-integer
             * equation this is the Phase 2 fallback the plan gates
             * sigma-substitution on; count it so the gate has a reading. */
            if (allint) refine_caps()->eq_nounit_split++;
            cur[e].is_eq = false; cur[e].strict = false;
            LinC neg = cur[e];
            bool bad = false;
            for (uint32_t j = 0; j < n_vars; j++) {
                neg.coef[j] = rat_neg(neg.coef[j]);
                if (neg.coef[j].bad) { bad = true; break; }
            }
            neg.konst = rat_neg(neg.konst);
            if (!bad && !neg.konst.bad) cur[n++] = neg;   /* room: 2 * n_in */
            continue;
        }

        /* x_p = -(E - a_p x_p) / a_p ; for constraint C with coefficient c_p on
         * x_p:  C' = C - (c_p / a_p) * E, which zeroes x_p in C'. */
        Rat inv = rat_norm(E.coef[pivot].den, E.coef[pivot].num);
        for (uint32_t i = 0; i < n; i++) {
            if (i == e || rat_zero(cur[i].coef[pivot])) continue;
            Rat f = rat_mul(cur[i].coef[pivot], inv);
            LinC out = cur[i];
            bool bad = f.bad;
            for (uint32_t j = 0; j < n_vars && !bad; j++) {
                out.coef[j] = rat_add(out.coef[j], rat_neg(rat_mul(f, E.coef[j])));
                if (out.coef[j].bad) bad = true;
            }
            if (!bad) { out.konst = rat_add(out.konst, rat_neg(rat_mul(f, E.konst))); bad = out.konst.bad; }
            if (bad) { cur[i].bad = true; continue; }      /* dropped below: weaker only */
            out.coef[pivot] = rat_of(0);
            if (!int_normalize(st, &out)) return false;   /* gcd test failed: unsat */
            if (constant_false(&out, n_vars)) return false;
            cur[i] = out;
        }
        /* Remove E and any constraint the substitution overflowed. */
        uint32_t m = 0;
        for (uint32_t i = 0; i < n; i++) if (i != e && !cur[i].bad) cur[m++] = cur[i];
        n = m;
    }
    *n_io = n;
    return true;
}

/* ------------------------------------------------------------------------- *
 * Fourier-Motzkin
 * ------------------------------------------------------------------------- */

bool la_unsat(LaState *st) {
    if (st->gave_up) return false;

    uint32_t n_vars = st->n_vars;
    uint32_t n = st->n_cs;
    if (n == 0) return false;

    /* Private copy, with room for every equality to split in two. */
    LinC *cur = (LinC *)arena_alloc(st->a, (2 * n) * sizeof(LinC));
    memcpy(cur, st->cs, n * sizeof(LinC));

    for (uint32_t i = 0; i < n; i++)
        if (constant_false(&cur[i], n_vars)) return true;

    /* Phase E: integer-exact equality elimination, then the real shadow. */
    if (!eq_eliminate(st, cur, &n)) return true;
    if (n == 0) return false;
    for (uint32_t i = 0; i < n; i++)
        if (constant_false(&cur[i], n_vars)) return true;

    for (uint32_t v = 0; v < n_vars; v++) {
        /* Partition on the sign of x_v. */
        uint32_t n_pos = 0, n_neg = 0, n_zero = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (rat_pos(cur[i].coef[v]))        n_pos++;
            else if (rat_neg_p(cur[i].coef[v])) n_neg++;
            else                                n_zero++;
        }
        uint64_t next = (uint64_t)n_zero + (uint64_t)n_pos * (uint64_t)n_neg;
        if (next > REFINE_MAX_LA_CONSTR) {
            /* Counted apart from the static adder above: this is elimination
             * GROWING past the cap, not an input that arrived too wide, and
             * the two point at different fixes (SX4's simplex kills this one;
             * only a wider cap helps the other). */
            refine_caps()->la_fm_hits++;
            refine_cap_peak(&refine_caps()->la_constr_peak, REFINE_MAX_LA_CONSTR);
            return false;   /* blow-up: give up, stay sound */
        }
        refine_cap_peak(&refine_caps()->la_constr_peak, (uint32_t)next);

        LinC *nxt = (LinC *)arena_alloc(st->a, (next ? next : 1) * sizeof(LinC));
        uint32_t m = 0;
        for (uint32_t i = 0; i < n; i++)
            if (rat_zero(cur[i].coef[v])) nxt[m++] = cur[i];

        for (uint32_t i = 0; i < n && m <= next; i++) {
            if (!rat_pos(cur[i].coef[v])) continue;
            for (uint32_t j = 0; j < n; j++) {
                if (!rat_neg_p(cur[j].coef[v])) continue;
                /* Scale so the x_v terms cancel: |a_j| * C_i + a_i * C_j. */
                Rat si = rat_neg(cur[j].coef[v]);   /* > 0 */
                Rat sj = cur[i].coef[v];            /* > 0 */
                LinC out; memset(&out, 0, sizeof(out));
                for (uint32_t k = 0; k < REFINE_MAX_LA_VARS; k++) out.coef[k] = rat_of(0);
                out.konst = rat_of(0);
                bool bad = false;
                for (uint32_t k = 0; k < n_vars; k++) {
                    Rat t = rat_add(rat_mul(cur[i].coef[k], si), rat_mul(cur[j].coef[k], sj));
                    if (t.bad) { bad = true; break; }
                    out.coef[k] = t;
                }
                if (bad) continue;   /* overflow: drop this combination */
                Rat kk = rat_add(rat_mul(cur[i].konst, si), rat_mul(cur[j].konst, sj));
                if (kk.bad) continue;
                out.konst  = kk;
                out.strict = cur[i].strict || cur[j].strict;
                out.is_eq  = false;
                out.bad    = false;
                (void)int_normalize(st, &out);   /* never fails for an inequality */
                if (m < next) nxt[m++] = out;
                if (constant_false(&out, n_vars)) return true;
            }
        }
        cur = nxt; n = m;
        for (uint32_t i = 0; i < n; i++)
            if (constant_false(&cur[i], n_vars)) return true;
        if (n == 0) return false;
    }
    for (uint32_t i = 0; i < n; i++)
        if (constant_false(&cur[i], n_vars)) return true;
    return false;
}

/* Phase 3(a) trigger predicate: every variable in the asserted set is
 * int-sorted and at least one constraint has two or more nonzero
 * coefficients.  Single-variable bounds are decided exactly by
 * int_normalize's floor/ceil, so a set without a multi-variable constraint
 * has no integer hull left to close. */
bool la_set_is_int_multivar(const LaState *st) {
    if (!st || st->n_cs == 0) return false;
    for (uint32_t j = 0; j < st->n_vars; j++)
        if (st->vars[j]->sort != VS_INT) return false;
    for (uint32_t i = 0; i < st->n_cs; i++) {
        uint32_t nz = 0;
        for (uint32_t j = 0; j < st->n_vars; j++)
            if (!rat_zero(st->cs[i].coef[j])) nz++;
        if (nz >= 2) return true;
    }
    return false;
}

bool la_entails_eq(LaState *st, VCTerm *x, VCTerm *y) {
    if (st->gave_up) return false;
    uint32_t saved = st->n_cs;

    la_assert_le(st, x, y, true);         /* x < y */
    bool lt_unsat = la_unsat(st);
    st->n_cs = saved;
    if (!lt_unsat) return false;

    la_assert_le(st, y, x, true);         /* y < x */
    bool gt_unsat = la_unsat(st);
    st->n_cs = saved;
    return gt_unsat;
}

RefineDecision refine_s2_decide_cc(RefineVC *vc, Arena *a, RefineCubeCache *cc) {
    if (!vc || !vc->goal) return refine_unknown();

    const VCCubeSet *cs;
    if (!refine_cubes_get(vc, a, cc, &cs)) return refine_unknown();
    if (cs->trivial || cs->n == 0) return refine_valid();

    for (uint32_t i = 0; i < cs->n; i++) {
        LaState *st = la_new(vc, a);
        la_assert_cube(st, &cs->cubes[i]);
        if (!la_unsat(st)) {
            /* Phase 3(a) trigger (solver-integer-tail-plan): the cube S2
             * could not refute -- if it is all-integer with a genuinely
             * multi-variable constraint, its rational relaxation was found
             * feasible while its INTEGER feasibility was never asked, which
             * is exactly what the dark shadow / branch-and-bound would ask.
             * Counted per obligation (first such cube), not per cube, and
             * not when the stage gave up on a cap. */
            if (!st->gave_up && la_set_is_int_multivar(st))
                refine_caps()->la_int_relax_feasible++;
            return refine_unknown();
        }
    }
    return refine_valid();
}

/* refine-chain-expands-the-same-dnf-four-times: the original entry point, kept
 * so every caller outside the chain driver (`tur smt`, tests/unit, the SX8
 * doors) is unchanged -- a fresh cache means it builds exactly once, as it
 * always did. */
RefineDecision refine_s2_decide(RefineVC *vc, Arena *a) {
    RefineCubeCache cc = {0};
    return refine_s2_decide_cc(vc, a, &cc);
}
