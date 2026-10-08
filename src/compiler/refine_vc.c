/* refine_vc.c -- RT2: normalized VC construction (hash-consed terms).
 *
 * See refine_vc.h for the model.  Two properties this file guarantees, both
 * relied on by every backend:
 *
 *   1. HASH-CONSING.  Structurally equal terms are the same VCTerm*, so
 *      pointer equality is structural equality.  S0's syntactic entailment and
 *      S1's congruence closure both lean on this.
 *   2. CONSTANT FOLDING AT INTERN TIME.  vc_mk folds any node whose kids are
 *      all literals, so `(< 0 3)` interns as VC_TRUE and S0 discharges a whole
 *      class of obligations without a solver.
 *
 * Normalization done here (so no backend repeats it):
 *   - `>` / `>=` are never built; callers swap and use VC_LT / VC_LE.
 *   - `!=` is built as (not (= ..)).
 *   - (not (not p)) collapses; (and) => true; (or) => false; single-kid
 *     and/or collapse to the kid.
 */

#include "refine_vc.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------------- *
 * Growable arrays (arena-backed; grow by copy, no free -- compile-lifetime)
 * ------------------------------------------------------------------------- */

static void *grow(Arena *a, void *old, uint32_t used, uint32_t *cap, size_t elem) {
    if (used < *cap) return old;
    uint32_t ncap = *cap ? *cap * 2 : 8;
    void *nw = arena_alloc(a, ncap * elem);
    if (old && used) memcpy(nw, old, (size_t)used * elem);
    *cap = ncap;
    return nw;
}

RefineVC *vc_new(Arena *a) {
    RefineVC *vc = (RefineVC *)arena_alloc(a, sizeof(RefineVC));
    memset(vc, 0, sizeof(*vc));
    vc->arena = a;
    vc->htab_cap = 256;
    vc->htab = (VCTerm **)arena_alloc(a, vc->htab_cap * sizeof(VCTerm *));
    memset(vc->htab, 0, vc->htab_cap * sizeof(VCTerm *));
    return vc;
}

/* ------------------------------------------------------------------------- *
 * Symbol tables
 * ------------------------------------------------------------------------- */

uint32_t vc_declare_var(RefineVC *vc, const char *name, VCSort sort) {
    for (uint32_t i = 0; i < vc->n_vars; i++) {
        if (strcmp(vc->vars[i].name, name) == 0) {
            /* A later, better-informed declaration promotes int -> real; the
             * reverse never happens (promotion is one-way and sound). */
            if (sort == VS_REAL) vc->vars[i].sort = VS_REAL;
            if (vc->vars[i].sort == VS_REAL) vc->has_real = true;
            return i;
        }
    }
    vc->vars = (VCVar *)grow(vc->arena, vc->vars, vc->n_vars, &vc->cap_vars, sizeof(VCVar));
    vc->vars[vc->n_vars].name = arena_strdup(vc->arena, name, strlen(name));
    vc->vars[vc->n_vars].sort = sort;
    if (sort == VS_REAL) vc->has_real = true;
    return vc->n_vars++;
}

uint32_t vc_declare_ufunc(RefineVC *vc, const char *name, uint32_t arity,
                          VCSort sort, const Form *origin, bool nonlinear) {
    for (uint32_t i = 0; i < vc->n_ufuncs; i++) {
        if (vc->ufuncs[i].arity == arity && strcmp(vc->ufuncs[i].name, name) == 0)
            return i;
    }
    vc->ufuncs = (VCUFunc *)grow(vc->arena, vc->ufuncs, vc->n_ufuncs, &vc->cap_ufuncs,
                                 sizeof(VCUFunc));
    VCUFunc *u = &vc->ufuncs[vc->n_ufuncs];
    u->name      = arena_strdup(vc->arena, name, strlen(name));
    u->arity     = arity;
    u->sort      = sort;
    u->origin    = origin;
    u->nonlinear = nonlinear;
    u->is_ctor   = false;   /* RF6: set by the encoder once it knows */
    u->reflected = false;
    if (nonlinear) {
        vc->has_nonlinear = true;
        if (!vc->nonlinear_src) vc->nonlinear_src = origin;
    }
    if (sort == VS_REAL) vc->has_real = true;
    return vc->n_ufuncs++;
}

/* ------------------------------------------------------------------------- *
 * Hash-consing
 * ------------------------------------------------------------------------- */

static uint32_t mix(uint32_t h, uint32_t v) {
    h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
    return h;
}

static uint32_t term_hash(VCOp op, VCSort sort, uint32_t n, VCTerm **kids,
                          int64_t iv, double rv, uint32_t idx) {
    uint32_t h = mix(0x811c9dc5u, (uint32_t)op);
    h = mix(h, (uint32_t)sort);
    h = mix(h, n);
    h = mix(h, idx);
    h = mix(h, (uint32_t)iv);
    h = mix(h, (uint32_t)(iv >> 32));
    if (rv != 0.0) {
        uint64_t bits; memcpy(&bits, &rv, sizeof(bits));
        h = mix(h, (uint32_t)bits);
        h = mix(h, (uint32_t)(bits >> 32));
    }
    for (uint32_t i = 0; i < n; i++) h = mix(h, kids[i] ? kids[i]->id + 1 : 0);
    return h ? h : 1;
}

static bool term_eq(const VCTerm *t, VCOp op, VCSort sort, uint32_t n,
                    VCTerm **kids, int64_t iv, double rv, uint32_t idx) {
    if (t->op != op || t->sort != sort || t->n != n) return false;
    if (op == VC_CONST_INT) return t->as.i == iv;
    if (op == VC_CONST_REAL) return t->as.r == rv || (isnan(t->as.r) && isnan(rv));
    if (op == VC_VAR || op == VC_APP) { if (t->as.idx != idx) return false; }
    for (uint32_t i = 0; i < n; i++) if (t->kids[i] != kids[i]) return false;
    return true;
}

static void htab_rehash(RefineVC *vc) {
    uint32_t ncap = vc->htab_cap * 2;
    VCTerm **nt = (VCTerm **)arena_alloc(vc->arena, ncap * sizeof(VCTerm *));
    memset(nt, 0, ncap * sizeof(VCTerm *));
    for (uint32_t i = 0; i < vc->htab_cap; i++) {
        VCTerm *t = vc->htab[i];
        if (!t) continue;
        uint32_t j = t->hash & (ncap - 1);
        while (nt[j]) j = (j + 1) & (ncap - 1);
        nt[j] = t;
    }
    vc->htab = nt;
    vc->htab_cap = ncap;
}

static VCTerm *intern(RefineVC *vc, VCOp op, VCSort sort, uint32_t n, VCTerm **kids,
                      int64_t iv, double rv, uint32_t idx) {
    uint32_t h = term_hash(op, sort, n, kids, iv, rv, idx);
    uint32_t j = h & (vc->htab_cap - 1);
    while (vc->htab[j]) {
        VCTerm *t = vc->htab[j];
        if (t->hash == h && term_eq(t, op, sort, n, kids, iv, rv, idx)) return t;
        j = (j + 1) & (vc->htab_cap - 1);
    }
    VCTerm *t = (VCTerm *)arena_alloc(vc->arena, sizeof(VCTerm));
    memset(t, 0, sizeof(*t));
    t->op = op; t->sort = sort; t->n = n; t->hash = h; t->id = vc->next_id++;
    if (op == VC_CONST_INT)      t->as.i = iv;
    else if (op == VC_CONST_REAL) t->as.r = rv;
    else                          t->as.idx = idx;
    if (n) {
        t->kids = (VCTerm **)arena_alloc(vc->arena, n * sizeof(VCTerm *));
        memcpy(t->kids, kids, n * sizeof(VCTerm *));
    }
    vc->htab[j] = t;
    if (++vc->htab_len * 2 >= vc->htab_cap) htab_rehash(vc);
    return t;
}

/* ------------------------------------------------------------------------- *
 * Leaf constructors
 * ------------------------------------------------------------------------- */

VCTerm *vc_int(RefineVC *vc, int64_t v) {
    return intern(vc, VC_CONST_INT, VS_INT, 0, NULL, v, 0.0, 0);
}
VCTerm *vc_real(RefineVC *vc, double v) {
    vc->has_real = true;
    return intern(vc, VC_CONST_REAL, VS_REAL, 0, NULL, 0, v, 0);
}
VCTerm *vc_bool(RefineVC *vc, bool v) {
    return intern(vc, v ? VC_TRUE : VC_FALSE, VS_BOOL, 0, NULL, 0, 0.0, 0);
}
VCTerm *vc_var_ref(RefineVC *vc, uint32_t idx) {
    VCSort s = idx < vc->n_vars ? vc->vars[idx].sort : VS_INT;
    return intern(vc, VC_VAR, s, 0, NULL, 0, 0.0, idx);
}
VCTerm *vc_app(RefineVC *vc, uint32_t fn, VCTerm **args, uint32_t n) {
    VCSort s = fn < vc->n_ufuncs ? vc->ufuncs[fn].sort : VS_INT;
    return intern(vc, VC_APP, s, n, args, 0, 0.0, fn);
}

/* ------------------------------------------------------------------------- *
 * Folding helpers
 * ------------------------------------------------------------------------- */

static bool is_int_lit(const VCTerm *t)  { return t && t->op == VC_CONST_INT; }
static bool is_real_lit(const VCTerm *t) { return t && t->op == VC_CONST_REAL; }
static bool is_num_lit(const VCTerm *t)  { return is_int_lit(t) || is_real_lit(t); }
static double num_of(const VCTerm *t)    { return is_int_lit(t) ? (double)t->as.i : t->as.r; }

/* Result sort for a binary arithmetic node: real is contagious. */
static VCSort arith_sort(VCTerm *a, VCTerm *b) {
    if ((a && a->sort == VS_REAL) || (b && b->sort == VS_REAL)) return VS_REAL;
    return VS_INT;
}

/* Add/sub/mul over int literals, guarding overflow (fall through to an
 * unfolded node rather than wrapping -- a wrapped fold would be unsound). */
static bool fold_int(VCOp op, int64_t a, int64_t b, int64_t *out) {
    switch (op) {
        case VC_ADD: if (__builtin_add_overflow(a, b, out)) return false; return true;
        case VC_SUB: if (__builtin_sub_overflow(a, b, out)) return false; return true;
        case VC_MUL: if (__builtin_mul_overflow(a, b, out)) return false; return true;
        case VC_DIV: if (b == 0 || (a == INT64_MIN && b == -1)) return false;
                     *out = a / b; return true;
        case VC_MOD: if (b == 0 || (a == INT64_MIN && b == -1)) return false;
                     *out = a % b; return true;
        default: return false;
    }
}

/* ------------------------------------------------------------------------- *
 * vc_mk -- the normalizing, folding node constructor
 * ------------------------------------------------------------------------- */

VCTerm *vc_mk(RefineVC *vc, VCOp op, VCTerm **kids, uint32_t n) {
    switch (op) {
        case VC_TRUE:  return vc_bool(vc, true);
        case VC_FALSE: return vc_bool(vc, false);

        case VC_NEG: {
            VCTerm *a = kids[0];
            if (is_int_lit(a)) {
                int64_t r;
                if (!__builtin_sub_overflow((int64_t)0, a->as.i, &r)) return vc_int(vc, r);
            } else if (is_real_lit(a)) {
                return vc_real(vc, -a->as.r);
            }
            return intern(vc, VC_NEG, a->sort, 1, kids, 0, 0.0, 0);
        }

        case VC_ADD: case VC_SUB: case VC_MUL: case VC_DIV: case VC_MOD: {
            VCTerm *a = kids[0], *b = kids[1];
            if (is_int_lit(a) && is_int_lit(b)) {
                int64_t r;
                if (fold_int(op, a->as.i, b->as.i, &r)) return vc_int(vc, r);
            } else if (is_num_lit(a) && is_num_lit(b) && op != VC_MOD) {
                double x = num_of(a), y = num_of(b);
                switch (op) {
                    case VC_ADD: return vc_real(vc, x + y);
                    case VC_SUB: return vc_real(vc, x - y);
                    case VC_MUL: return vc_real(vc, x * y);
                    case VC_DIV: if (y != 0.0) return vc_real(vc, x / y); break;
                    default: break;
                }
            }
            /* x + 0, x - 0, x * 1, x * 0, x / 1 */
            if (op == VC_ADD && is_int_lit(b) && b->as.i == 0) return a;
            if (op == VC_ADD && is_int_lit(a) && a->as.i == 0) return b;
            if (op == VC_SUB && is_int_lit(b) && b->as.i == 0) return a;
            if (op == VC_MUL && is_int_lit(b) && b->as.i == 1) return a;
            if (op == VC_MUL && is_int_lit(a) && a->as.i == 1) return b;
            if (op == VC_DIV && is_int_lit(b) && b->as.i == 1) return a;
            return intern(vc, op, arith_sort(a, b), 2, kids, 0, 0.0, 0);
        }

        case VC_EQ: case VC_LT: case VC_LE: {
            VCTerm *a = kids[0], *b = kids[1];
            /* x=x, x<=x true; x<x false.  Not the first two for a real
             * term: a NaN is unequal to itself (float-proofs-assume-exact-
             * reals, refine_solver.h); x<x is false for a NaN too. */
            if (a == b && (op == VC_LT || a->sort != VS_REAL))
                return vc_bool(vc, op != VC_LT);
            if (is_num_lit(a) && is_num_lit(b)) {
                double x = num_of(a), y = num_of(b);
                bool r = op == VC_EQ ? (x == y) : op == VC_LT ? (x < y) : (x <= y);
                return vc_bool(vc, r);
            }
            return intern(vc, op, VS_BOOL, 2, kids, 0, 0.0, 0);
        }

        case VC_NOT: {
            VCTerm *a = kids[0];
            if (a->op == VC_TRUE)  return vc_bool(vc, false);
            if (a->op == VC_FALSE) return vc_bool(vc, true);
            if (a->op == VC_NOT)   return a->kids[0];
            return intern(vc, VC_NOT, VS_BOOL, 1, kids, 0, 0.0, 0);
        }

        case VC_AND: case VC_OR: {
            /* Flatten, drop units, short-circuit on the annihilator. */
            VCOp unit = (op == VC_AND) ? VC_TRUE : VC_FALSE;
            VCOp zero = (op == VC_AND) ? VC_FALSE : VC_TRUE;
            uint32_t cnt = 0;
            for (uint32_t i = 0; i < n; i++)
                cnt += (kids[i]->op == op) ? kids[i]->n : 1;
            VCTerm **flat = (VCTerm **)arena_alloc(vc->arena, (cnt ? cnt : 1) * sizeof(VCTerm *));
            uint32_t m = 0;
            for (uint32_t i = 0; i < n; i++) {
                VCTerm *k = kids[i];
                uint32_t kn = (k->op == op) ? k->n : 1;
                for (uint32_t j = 0; j < kn; j++) {
                    VCTerm *x = (k->op == op) ? k->kids[j] : k;
                    if (x->op == zero) return vc_bool(vc, op != VC_AND);
                    if (x->op == unit) continue;
                    bool dup = false;
                    for (uint32_t q = 0; q < m; q++) if (flat[q] == x) { dup = true; break; }
                    if (!dup) flat[m++] = x;
                }
            }
            if (m == 0) return vc_bool(vc, op == VC_AND);
            if (m == 1) return flat[0];
            return intern(vc, op, VS_BOOL, m, flat, 0, 0.0, 0);
        }

        case VC_IMPLIES: {
            /* (=> p q) == (or (not p) q); keep it desugared so backends need
             * only and/or/not. */
            VCTerm *np = vc_not(vc, kids[0]);
            VCTerm *two[2] = { np, kids[1] };
            return vc_mk(vc, VC_OR, two, 2);
        }

        default:
            return intern(vc, op, VS_BOOL, n, kids, 0, 0.0, 0);
    }
}

VCTerm *vc_mk1(RefineVC *vc, VCOp op, VCTerm *a) {
    VCTerm *k[1] = { a };
    return vc_mk(vc, op, k, 1);
}
VCTerm *vc_mk2(RefineVC *vc, VCOp op, VCTerm *a, VCTerm *b) {
    VCTerm *k[2] = { a, b };
    return vc_mk(vc, op, k, 2);
}
VCTerm *vc_not(RefineVC *vc, VCTerm *a) { return vc_mk1(vc, VC_NOT, a); }

void vc_add_divmod_axioms(RefineVC *vc, VCTerm *a, VCTerm *k) {
    if (!vc || !a || !k || k->op != VC_CONST_INT || k->as.i == 0 || a->sort != VS_INT) return;
    if (a->op == VC_CONST_INT || a->op == VC_CONST_REAL) return;  /* folded whole */
    int64_t kv = k->as.i;
    if (kv == INT64_MIN) return;
    int64_t K = kv < 0 ? -kv : kv;
    VCTerm *q    = vc_mk2(vc, VC_DIV, a, k);
    VCTerm *r    = vc_mk2(vc, VC_MOD, a, k);
    VCTerm *zero = vc_int(vc, 0);
    VCTerm *rel  = vc_mk2(vc, VC_EQ, a, vc_mk2(vc, VC_ADD, vc_mk2(vc, VC_MUL, k, q), r));
    /* Already axiomatized (the same (a, k) pair encoded twice)?  vc_add_hyp
     * would dedup the terms anyway; the check keeps the split budget honest. */
    for (uint32_t i = 0; i < vc->n_hyps; i++) if (vc->hyps[i] == rel) return;
    vc_add_hyp(vc, rel);
    VCTerm *km1 = vc_int(vc, K - 1), *nkm1 = vc_int(vc, -(K - 1));
    if (vc->n_divmod_splits < VC_MAX_DIVMOD_SPLITS) {
        vc->n_divmod_splits++;
        VCTerm *pos[3] = { vc_mk2(vc, VC_LE, zero, a), vc_mk2(vc, VC_LE, zero, r),
                           vc_mk2(vc, VC_LE, r, km1) };
        VCTerm *neg[3] = { vc_mk2(vc, VC_LT, a, zero), vc_mk2(vc, VC_LE, nkm1, r),
                           vc_mk2(vc, VC_LE, r, zero) };
        vc_add_hyp(vc, vc_mk2(vc, VC_OR, vc_mk(vc, VC_AND, pos, 3),
                              vc_mk(vc, VC_AND, neg, 3)));
    } else {
        vc_add_hyp(vc, vc_mk2(vc, VC_LE, nkm1, r));
        vc_add_hyp(vc, vc_mk2(vc, VC_LE, r, km1));
    }
}

void vc_add_hyp(RefineVC *vc, VCTerm *t) {
    if (!t || t->op == VC_TRUE) return;
    for (uint32_t i = 0; i < vc->n_hyps; i++) if (vc->hyps[i] == t) return;
    vc->hyps = (VCTerm **)grow(vc->arena, vc->hyps, vc->n_hyps, &vc->cap_hyps,
                               sizeof(VCTerm *));
    vc->hyps[vc->n_hyps++] = t;
}

void vc_set_goal(RefineVC *vc, VCTerm *t) { vc->goal = t; }

/* ------------------------------------------------------------------------- *
 * Printing
 * ------------------------------------------------------------------------- */

typedef struct { char *p; size_t left; } PBuf;

static void pb_puts(PBuf *b, const char *s) {
    size_t n = strlen(s);
    if (n >= b->left) n = b->left ? b->left - 1 : 0;
    if (n) { memcpy(b->p, s, n); b->p += n; b->left -= n; }
    if (b->left) *b->p = '\0';
}

static void pb_printf(PBuf *b, const char *fmt, ...) TUR_PRINTF_FMT(2, 3);
static void pb_printf(PBuf *b, const char *fmt, ...) {
    char tmp[128];
    va_list ap; va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    pb_puts(b, tmp);
}

static const char *op_name(VCOp op) {
    switch (op) {
        case VC_ADD: return "+"; case VC_SUB: return "-"; case VC_MUL: return "*";
        case VC_DIV: return "/"; case VC_MOD: return "mod"; case VC_NEG: return "-";
        case VC_EQ:  return "="; case VC_LT:  return "<";  case VC_LE:  return "<=";
        case VC_AND: return "and"; case VC_OR: return "or"; case VC_NOT: return "not";
        default: return "?";
    }
}

static void print_rec(const RefineVC *vc, const VCTerm *t, PBuf *b) {
    if (!t) { pb_puts(b, "<null>"); return; }
    switch (t->op) {
        case VC_TRUE:  pb_puts(b, "true");  return;
        case VC_FALSE: pb_puts(b, "false"); return;
        case VC_CONST_INT:  pb_printf(b, "%lld", (long long)t->as.i); return;
        case VC_CONST_REAL: pb_printf(b, "%g", t->as.r); return;
        case VC_VAR:
            pb_puts(b, t->as.idx < vc->n_vars ? vc->vars[t->as.idx].name : "?var");
            return;
        case VC_APP: {
            const VCUFunc *u = t->as.idx < vc->n_ufuncs ? &vc->ufuncs[t->as.idx] : NULL;
            pb_puts(b, "(");
            pb_puts(b, u ? u->name : "?fn");
            for (uint32_t i = 0; i < t->n; i++) { pb_puts(b, " "); print_rec(vc, t->kids[i], b); }
            pb_puts(b, ")");
            return;
        }
        default:
            pb_puts(b, "(");
            pb_puts(b, op_name(t->op));
            for (uint32_t i = 0; i < t->n; i++) { pb_puts(b, " "); print_rec(vc, t->kids[i], b); }
            pb_puts(b, ")");
            return;
    }
}

void vc_term_print(const RefineVC *vc, const VCTerm *t, char *buf, size_t cap) {
    PBuf b = { buf, cap };
    if (cap) buf[0] = '\0';
    print_rec(vc, t, &b);
}

/* ------------------------------------------------------------------------- *
 * RT7: identity -- fingerprint + structural equality under alpha-renaming.
 * ------------------------------------------------------------------------- */

#define VCID_MAX_SYMS 256

/* Canonical numbering of the symbols met so far, in first-occurrence order. */
typedef struct {
    uint32_t var_of[VCID_MAX_SYMS];   /* source index -> canonical ordinal */
    uint32_t fn_of[VCID_MAX_SYMS];
    uint32_t n_var, n_fn;
    bool     overflow;
} VCIdMap;

static void vcid_init(VCIdMap *m) {
    memset(m, 0, sizeof(*m));
    for (uint32_t i = 0; i < VCID_MAX_SYMS; i++) {
        m->var_of[i] = UINT32_MAX;
        m->fn_of[i]  = UINT32_MAX;
    }
}

static uint32_t vcid_var(VCIdMap *m, uint32_t idx) {
    if (idx >= VCID_MAX_SYMS) { m->overflow = true; return 0; }
    if (m->var_of[idx] == UINT32_MAX) m->var_of[idx] = m->n_var++;
    return m->var_of[idx];
}

static uint32_t vcid_fn(VCIdMap *m, uint32_t idx) {
    if (idx >= VCID_MAX_SYMS) { m->overflow = true; return 0; }
    if (m->fn_of[idx] == UINT32_MAX) m->fn_of[idx] = m->n_fn++;
    return m->fn_of[idx];
}

static void fp_mix(uint64_t *h, uint64_t v) {
    *h ^= v + 0x9e3779b97f4a7c15ull + (*h << 6) + (*h >> 2);
}

static void fp_term(const RefineVC *vc, const VCTerm *t, VCIdMap *m, uint64_t *h) {
    if (!t) { fp_mix(h, 0xdead); return; }
    fp_mix(h, (uint64_t)t->op);
    fp_mix(h, (uint64_t)t->sort);
    switch (t->op) {
    case VC_CONST_INT:  fp_mix(h, (uint64_t)t->as.i); break;
    case VC_CONST_REAL: {
        /* Hash the bit pattern: two doubles that print alike must not be
         * allowed to differ, and two that differ must not collide by
         * rounding. */
        uint64_t bits; memcpy(&bits, &t->as.r, sizeof(bits));
        fp_mix(h, bits);
        break;
    }
    case VC_VAR:
        /* The canonical ordinal, not the name -- that is the alpha-renaming. */
        fp_mix(h, 1 + (uint64_t)vcid_var(m, t->as.idx));
        break;
    case VC_APP:
        fp_mix(h, 2 + (uint64_t)vcid_fn(m, t->as.idx));
        if (t->as.idx < vc->n_ufuncs) {
            fp_mix(h, vc->ufuncs[t->as.idx].arity);
            fp_mix(h, (uint64_t)vc->ufuncs[t->as.idx].sort);
        }
        break;
    default: break;
    }
    fp_mix(h, t->n);
    for (uint32_t i = 0; i < t->n; i++) fp_term(vc, t->kids[i], m, h);
}

uint64_t refine_vc_fingerprint(const RefineVC *vc) {
    if (!vc) return 0;
    VCIdMap m; vcid_init(&m);
    uint64_t h = 0xcbf29ce484222325ull;
    fp_mix(&h, vc->n_hyps);
    for (uint32_t i = 0; i < vc->n_hyps; i++) fp_term(vc, vc->hyps[i], &m, &h);
    fp_mix(&h, 0x60a1);   /* separator between hypotheses and goal */
    fp_term(vc, vc->goal, &m, &h);
    /* A VC whose symbol count blew the map gets 0 -- never a memo key -- so it
     * is re-decided rather than matched on a truncated identity. */
    if (m.overflow) return 0;
    return h ? h : 1;
}

/* Lockstep structural compare under a shared alpha-renaming. */
static bool eq_term(const RefineVC *va, const VCTerm *a, VCIdMap *ma,
                    const RefineVC *vb, const VCTerm *b, VCIdMap *mb) {
    if (!a || !b) return a == b;
    if (a->op != b->op || a->sort != b->sort || a->n != b->n) return false;
    switch (a->op) {
    case VC_CONST_INT:
        if (a->as.i != b->as.i) return false;
        break;
    case VC_CONST_REAL: {
        uint64_t x, y;
        memcpy(&x, &a->as.r, sizeof(x));
        memcpy(&y, &b->as.r, sizeof(y));
        if (x != y) return false;
        break;
    }
    case VC_VAR:
        if (vcid_var(ma, a->as.idx) != vcid_var(mb, b->as.idx)) return false;
        break;
    case VC_APP:
        if (vcid_fn(ma, a->as.idx) != vcid_fn(mb, b->as.idx)) return false;
        if (a->as.idx >= va->n_ufuncs || b->as.idx >= vb->n_ufuncs) return false;
        if (va->ufuncs[a->as.idx].arity != vb->ufuncs[b->as.idx].arity) return false;
        if (va->ufuncs[a->as.idx].sort  != vb->ufuncs[b->as.idx].sort)  return false;
        break;
    default: break;
    }
    for (uint32_t i = 0; i < a->n; i++)
        if (!eq_term(va, a->kids[i], ma, vb, b->kids[i], mb)) return false;
    return true;
}

bool refine_vc_equal(const RefineVC *a, const RefineVC *b) {
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->n_hyps != b->n_hyps) return false;
    VCIdMap ma, mb; vcid_init(&ma); vcid_init(&mb);
    for (uint32_t i = 0; i < a->n_hyps; i++)
        if (!eq_term(a, a->hyps[i], &ma, b, b->hyps[i], &mb)) return false;
    if (!eq_term(a, a->goal, &ma, b, b->goal, &mb)) return false;
    return !ma.overflow && !mb.overflow;
}
