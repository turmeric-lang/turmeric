/* refine_collect.c -- RT1: constraint collector + RT2 Form -> VC encoder.
 *
 * See refine_collect.h for the model.  The encoder is deliberately narrow: it
 * accepts exactly the predicate fragment the plan permits (quantifier-free
 * linear integer/real arithmetic with equality and uninterpreted functions)
 * and returns NULL for everything else.  A NULL VC is not a failure mode --
 * it is RT_UNKNOWN, which falls back to the runtime contract check the
 * predicate would have had anyway. */

#include "refine_collect.h"

#include <stdio.h>
#include <stdlib.h>   /* getenv -- TUR_REFINE_NO_DISCHARGE test seam */
#include <string.h>

/* ------------------------------------------------------------------------- *
 * Environment
 * ------------------------------------------------------------------------- */

RefineEnv *refine_env_new(Arena *a) {
    RefineEnv *env = (RefineEnv *)arena_alloc(a, sizeof(RefineEnv));
    memset(env, 0, sizeof(*env));
    env->arena = a;
    return env;
}

void refine_env_declare(RefineEnv *env, const char *name, VCSort sort) {
    if (!env || !name) return;
    for (uint32_t i = 0; i < env->n_names; i++) {
        if (strcmp(env->names[i], name) == 0) { env->sorts[i] = sort; return; }
    }
    if (env->n_names == env->cap_names) {
        uint32_t ncap = env->cap_names ? env->cap_names * 2 : 8;
        const char **nn = (const char **)arena_alloc(env->arena, ncap * sizeof(char *));
        VCSort *ns = (VCSort *)arena_alloc(env->arena, ncap * sizeof(VCSort));
        if (env->n_names) {
            memcpy(nn, env->names, env->n_names * sizeof(char *));
            memcpy(ns, env->sorts, env->n_names * sizeof(VCSort));
        }
        env->names = nn; env->sorts = ns; env->cap_names = ncap;
    }
    env->names[env->n_names] = name;
    env->sorts[env->n_names] = sort;
    env->n_names++;
}

void refine_env_set_resolver(RefineEnv *env, RefineFnResolver fn, void *ud) {
    if (!env) return;
    env->resolve_fn = fn;
    env->resolve_ud = ud;
}

VCSort refine_env_sort_of(const RefineEnv *env, const char *name) {
    if (!env || !name) return VS_INT;
    for (uint32_t i = 0; i < env->n_names; i++)
        if (strcmp(env->names[i], name) == 0) return env->sorts[i];
    return VS_INT;
}

void refine_env_push(RefineEnv *env, const Form *pred,
                     const char *bound_var, const char *subject_name) {
    if (!env || !pred) return;
    RefineHyp *h = (RefineHyp *)arena_alloc(env->arena, sizeof(RefineHyp));
    h->pred         = pred;
    h->bound_var    = bound_var;
    h->subject_name = subject_name;
    h->next         = env->head;
    env->head       = h;
}

/* ------------------------------------------------------------------------- *
 * Obligations
 * ------------------------------------------------------------------------- */

void refine_obligations_init(RefineObligationVec *v, Arena *a) {
    v->obs = NULL; v->n = 0; v->cap = 0; v->arena = a;
}

/* TUR_REFINE_NO_DISCHARGE -- a TEST SEAM, not a feature gate.  There is no
 * `--enable`, no EXPERIMENTS[] row and no CLI flag; it lives alongside
 * TUR_REFINE_STATS / TUR_REFINE_DUMP as an env-only knob.
 *
 * It exists because the source-level differential fuzzer
 * (tests/refine-fuzz-src.py) needs a reference build in which every refinement
 * keeps its runtime check -- the ground truth for "does this program actually
 * violate its own refinement".  That is what the `refined` experiment gate gave
 * it until refinement types graduated in v0.33.0.  No shipping flag
 * reconstructs it: `--no-contracts` emits NO checks and `--keep-contracts`
 * emits checks MINUS whatever discharge elided, and it is precisely the elided
 * set that the fuzzer's miscompile property is about (both known refinement
 * soundness bugs proved something false and dropped the check that would have
 * caught it).
 *
 * Suppressing obligations HERE, at the one chokepoint every obligation flows
 * through, is what keeps this to a single conditional: all six callers already
 * treat a NULL obligation as "not proven", so nothing is decided, nothing is
 * elided, and no refinement diagnostic fires -- exactly the old gate-off
 * behavior. */
static bool refine_discharge_disabled(void) {
    static int cached = -1;
    if (cached < 0) cached = getenv("TUR_REFINE_NO_DISCHARGE") ? 1 : 0;
    return cached == 1;
}

RefineObligation *refine_collect_obligation(RefineObligationVec *v,
                                            const Form *predicate,
                                            const char *var_name,
                                            const Form *subject,
                                            VCSort base_sort,
                                            const char *base_type_name,
                                            Span loc,
                                            RefineEnv *env,
                                            const char *what,
                                            const char *fn_name) {
    if (!v || !v->arena || !predicate) return NULL;
    if (refine_discharge_disabled()) return NULL;
    RefineObligation *ob = (RefineObligation *)arena_alloc(v->arena, sizeof(RefineObligation));
    memset(ob, 0, sizeof(*ob));
    ob->predicate      = predicate;
    ob->var_name       = var_name;
    ob->subject        = subject;
    ob->base_sort      = base_sort;
    ob->base_type_name = base_type_name;
    ob->loc            = loc;
    ob->env            = env;
    ob->what           = what ? what : "value";
    ob->fn_name        = fn_name;

    if (v->n == v->cap) {
        uint32_t ncap = v->cap ? v->cap * 2 : 16;
        RefineObligation **no = (RefineObligation **)arena_alloc(v->arena,
                                                                 ncap * sizeof(*no));
        if (v->n) memcpy(no, v->obs, v->n * sizeof(*no));
        v->obs = no; v->cap = ncap;
    }
    v->obs[v->n++] = ob;
    return ob;
}

void refine_obligation_set_subst(RefineObligation *ob, Arena *a,
                                 const RefineSubst *subst, uint32_t n) {
    if (!ob || !subst || n == 0) return;
    RefineSubst *copy = (RefineSubst *)arena_alloc(a, n * sizeof(RefineSubst));
    memcpy(copy, subst, n * sizeof(RefineSubst));
    ob->subst   = copy;
    ob->n_subst = n;
}

void refine_obligation_set_frozen(RefineObligation *ob,
                                  const char **frozen_names, uint32_t n) {
    if (!ob || !frozen_names || n == 0) return;
    ob->frozen_names = frozen_names;   /* shared by reference -- immutable */
    ob->n_frozen     = n;
}

/* ------------------------------------------------------------------------- *
 * Form -> VCTerm encoder
 * ------------------------------------------------------------------------- */

#define ENC_MAX_SUBST 8
#define ENC_MAX_DEPTH 64

typedef struct EncSubst {
    const char *name;
    VCTerm     *term;
} EncSubst;

#define ENC_MAX_PROPAGATE 4
#define ENC_MAX_MEASURES  32

/* RM-B2: the sort ONE measure name is declared at, for the whole VC.
 *
 * A symbol that is Int in a hypothesis and Bool in the goal is two symbols
 * that print the same, which is how a congruence bug gets written.  So the
 * sort of every occurrence of a name is settled BEFORE any of them is
 * declared, and a name genuinely used at both sorts rejects the VC instead of
 * guessing (Unknown is always sound -- the runtime check survives). */
typedef struct EncMeasure {
    const char *name;
    VCSort      sort;
    bool        resolved;   /* sort came from the callee's declared return type */
    bool        seen_bool;  /* occurred where a PROPOSITION is required */
    bool        seen_val;   /* occurred where a VALUE is required */
} EncMeasure;

typedef struct EncSorts {
    EncMeasure m[ENC_MAX_MEASURES];
    uint32_t   n;
} EncSorts;

typedef struct Enc {
    RefineVC        *vc;
    const RefineEnv *env;
    const EncSorts  *sorts;  /* RM-B2: name -> sort, resolved once per VC */
    EncSubst         subst[ENC_MAX_SUBST];
    uint32_t         n_subst;
    const char      *fail;   /* set on the first unsupported construct */
    uint32_t         depth;
    /* Names whose return refinement is currently being propagated, so a
     * refinement that mentions its own function cannot recurse forever. */
    const char      *propagating[ENC_MAX_PROPAGATE];
    uint32_t         n_propagating;
    /* C2 / #reads: names frozen (borrowed) at this obligation's site.  A
     * `#reads w` measure whose world argument is one of these is treated as
     * congruent (stable symbol) even though its body is impure.  Shared by
     * reference from the obligation; NULL / 0 when nothing is frozen. */
    const char *const *frozen_names;
    uint32_t          n_frozen;
    /* C2 / #reads: the obligation's callee-param -> caller-arg substitution, so
     * a measure argument written in a CALLEE parameter name (the goal
     * predicate) resolves to the caller expression before the frozen check --
     * which is what keeps a shadowed binder from a false "frozen".  Set only on
     * the GOAL enc, where this subst applies; NULL on hypothesis encs, whose
     * names are already the caller's. */
    const RefineSubst *ob_subst;
    uint32_t           n_ob_subst;
    /* reflected-measures RF3: the per-obligation unfolding budget, shared by
     * every Enc built for one VC (root encs and the nested ones the
     * propagation and unfolding blocks create).  NULL disables unfolding. */
    uint32_t          *rf_fuel;
    /* reflected-measures RF3: the refinement's bound variable and the FORM it
     * stands for at this obligation (the subject), so an argument written as
     * the bound variable -- `(len v)` in a parameter predicate -- reduces at
     * the subject form `(Cons 1 (Nil))` and not at the symbol `v`.  Goal enc
     * only; NULL elsewhere. */
    const char        *rf_subject_name;
    const Form        *rf_subject_form;
} Enc;

/* C2 / #reads: is `name` frozen (borrowed) at this obligation's site? */
static bool enc_name_is_frozen(const Enc *E, const char *name) {
    if (!E || !name || !E->frozen_names) return false;
    for (uint32_t i = 0; i < E->n_frozen; i++)
        if (E->frozen_names[i] && strcmp(E->frozen_names[i], name) == 0)
            return true;
    return false;
}

/* C2 / #reads: does the `reads`-param argument of measure `f` name a value that
 * is frozen at this site?  `reads_plus1` is 1-based (0 = not a #reads measure).
 * The argument is resolved through the obligation's callee-param subst first --
 * so a goal-predicate argument in a callee parameter name is checked against
 * the actual caller expression, and a shadowed binder cannot masquerade as the
 * frozen one -- then matched by name against the frozen set. */
/* Is the argument at 0-based param index `p` a name frozen in scope here? */
static bool enc_reads_one_arg_frozen(const Enc *E, const Form *f, uint32_t p) {
    if (!f || f->tag != F_LIST || (uint32_t)(p + 1) >= f->as.list.len) return false;
    const Form *arg = f->as.list.items[p + 1];    /* items[0] is the head */
    if (!arg || arg->tag != F_SYM || !arg->as.sym) return false;
    const char *nm = arg->as.sym->name;
    for (uint32_t i = 0; i < E->n_ob_subst; i++) {
        if (E->ob_subst[i].name && strcmp(E->ob_subst[i].name, nm) == 0) {
            const Form *tgt = E->ob_subst[i].form;
            nm = (tgt && tgt->tag == F_SYM && tgt->as.sym) ? tgt->as.sym->name : NULL;
            break;
        }
    }
    return nm && enc_name_is_frozen(E, nm);
}

/* multiple-reads-params: the congruence grant is CONJUNCTIVE over the frame.
 *
 * A `#reads` frame naming several parameters says the measure is a function of
 * all of their mutable state.  Two occurrences denote one value only if every
 * one of those states is pinned at this site -- a single unfrozen named
 * parameter is enough for the measure to differ between them, which is exactly
 * the crossing check this grant elides.  So: all frozen, or no grant.
 *
 * Reading this as "any frozen" would be unsound, and silently so: it would
 * elide crossings for a measure whose other read state is free to change.  The
 * mask is never empty when we get here (an empty `#reads` is TUR-E0024), so
 * the all-quantifier cannot degenerate into a vacuous true. */
static bool enc_reads_args_frozen(const Enc *E, const Form *f, uint64_t mask) {
    if (!E || mask == 0 || E->n_frozen == 0) return false;
    for (uint32_t p = 0; p < 64; p++) {
        if (!(mask & (UINT64_C(1) << p))) continue;
        if (!enc_reads_one_arg_frozen(E, f, p)) return false;
    }
    return true;
}

static VCTerm *enc(Enc *E, const Form *f);

static bool sym_is(const Form *f, const char *s) {
    return f && (f->tag == F_SYM || f->tag == F_KEYWORD) && f->as.sym &&
           strcmp(f->as.sym->name, s) == 0;
}

/* What a list head denotes, and therefore what its operands are.  Shared by the
 * encoder and the RM-B2 prescan so the two can never disagree about which
 * positions are propositions. */
typedef enum EncHead {
    EH_MEASURE = 0,  /* unrecognised head: a named measure          */
    EH_CAST,         /* (as T e)          -- a type name, then a value */
    EH_ARITH,        /* + - * / mod       -- value operands         */
    EH_ORD,          /* < <= > >=         -- value operands         */
    EH_EQ,           /* = == not= != <>   -- sort-polymorphic       */
    EH_LOGIC,        /* and or not => ... -- proposition operands   */
} EncHead;

/* Which sort a head DEMANDS of its operands.  `=` demands neither: two
 * propositions may be compared for equality just as two numbers may, and
 * enc_cmp is what enforces that the two SIDES agree.  Treating an equality
 * operand as a value would make the perfectly ordinary
 * `(= (alive? w x) (alive? w y))` a sort conflict. */
typedef enum EncPos {
    POS_NEUTRAL = 0,  /* either sort is admissible here */
    POS_VALUE,        /* a number is required           */
    POS_PROP,         /* a proposition is required      */
} EncPos;

static EncHead enc_head_kind(const Form *h) {
    if (sym_is(h, "+") || sym_is(h, "-") || sym_is(h, "*") ||
        sym_is(h, "/") || sym_is(h, "mod")) return EH_ARITH;
    if (sym_is(h, "<") || sym_is(h, "<=") || sym_is(h, ">") ||
        sym_is(h, ">=")) return EH_ORD;
    if (sym_is(h, "=") || sym_is(h, "==") || sym_is(h, "not=") ||
        sym_is(h, "!=") || sym_is(h, "<>")) return EH_EQ;
    if (sym_is(h, "and") || sym_is(h, "or") || sym_is(h, "not") ||
        sym_is(h, "=>")  || sym_is(h, "implies")) return EH_LOGIC;
    if (sym_is(h, "as")) return EH_CAST;
    return EH_MEASURE;
}

/* The sort every occurrence of `name` is declared at.  The prescan table is
 * authoritative; a name it never saw (a propagated return refinement is
 * encoded after the prescan ran) falls back to the callee's own sort, and
 * vc_declare_ufunc keys on the name, so a symbol still gets exactly one sort
 * either way. */
static VCSort enc_name_sort(const Enc *E, const char *name, VCSort fallback) {
    if (E->sorts) {
        for (uint32_t i = 0; i < E->sorts->n; i++)
            if (strcmp(E->sorts->m[i].name, name) == 0) return E->sorts->m[i].sort;
    }
    return fallback;
}

static VCTerm *enc_lookup(Enc *E, const char *name) {
    for (uint32_t i = 0; i < E->n_subst; i++)
        if (strcmp(E->subst[i].name, name) == 0) return E->subst[i].term;
    return NULL;
}

/* A literal-valued term: the multiplication/division linearity test. */
static bool is_const_term(const VCTerm *t) {
    return t && (t->op == VC_CONST_INT || t->op == VC_CONST_REAL);
}

/* Abstract a nonlinear application as an uninterpreted function.  Sound:
 * congruence closure still relates two occurrences of the same product, we
 * only lose the arithmetic facts.  Flags the VC so RT3 can emit TUR-W0373. */
static VCTerm *enc_nonlinear(Enc *E, const char *base, const Form *src,
                             VCTerm *a, VCTerm *b) {
    VCSort s = (a->sort == VS_REAL || b->sort == VS_REAL) ? VS_REAL : VS_INT;
    char nm[48];
    snprintf(nm, sizeof(nm), "%s_%s", base, s == VS_REAL ? "r" : "i");
    uint32_t fn = vc_declare_ufunc(E->vc, nm, 2, s, src, /*nonlinear=*/true);
    VCTerm *args[2] = { a, b };
    VCTerm *app = vc_app(E->vc, fn, args, 2);
    /* A SQUARE IS NON-NEGATIVE.  `(* x x)` is still uninterpreted -- nothing
     * here says what its value is -- but its sign is a fact about every
     * integer and every real, so it is asserted as a hypothesis about the
     * abstracted term.  `a == b` is structural equality (hash-consing), so
     * this fires for `(* (- x 1) (- x 1))` too.  Only products: `x / x` and
     * `x mod x` share this abstraction path and have no such sign law. */
    if (a == b && strcmp(base, "__nl_mul") == 0) {
        VCTerm *zero = s == VS_REAL ? vc_real(E->vc, 0.0) : vc_int(E->vc, 0);
        vc_add_hyp(E->vc, vc_mk2(E->vc, VC_LE, zero, app));
    }
    return app;
}

/* INTEGER DIVISION AND REMAINDER BY A LITERAL -- axiomatized rather than left
 * opaque.  `(/ a k)` and `(mod a k)` with `k` an integer literal were always
 * inside the predicate grammar, but S2 purified both into opaque variables,
 * so `(= (mod n 2) 0) |- (= (mod (+ n 2) 2) 0)` was Unknown.  The two terms
 * stay opaque to the linear encoder (they are still shared terms S3 can
 * exchange over); what changes is that the VC now carries what they MEAN:
 *
 *     a = k * q + r                    (q = (/ a k), r = (mod a k))
 *     (0 <= a  and  0 <= r <= |k|-1)  or  (a < 0  and  -(|k|-1) <= r <= 0)
 *
 * These are the TRUNCATING semantics of C's `/` and `%`, which is what both
 * backends emit for `/` and `mod` on ints (builtins.c maps `mod` to `%`; the
 * interpreter's eval uses `%` too), so the axioms describe exactly the value
 * the runtime check would see.  They are NOT SMT-LIB's Euclidean `div` and
 * `mod` -- and since 2026-09-05 the SMT-LIB reader (`tur smt`) does NOT get
 * a truncating reading of those: it builds the Euclidean value out of this
 * same truncating pair plus these axioms, lifted through an `ite`
 * (refine_smtlib.c, tr_euclid_divmod), so both surfaces mean what their own
 * language says.
 *
 * The axioms themselves live on the VC (vc_add_divmod_axioms, refine_vc.c)
 * because both the encoder and the reader assert them; the sign clause is a
 * disjunction and each pair doubles the cube count, so past
 * VC_MAX_DIVMOD_SPLITS pairs in one VC the weaker conjunctive bound is used
 * instead -- still sound, and a VC that used to prove without knowing
 * anything about its `mod` terms cannot be pushed over REFINE_MAX_CUBES by
 * learning about them.  Both the relation and the cube telemetry are the
 * evidence for raising it. */
static void enc_divmod_axioms(Enc *E, VCTerm *a, VCTerm *k) {
    vc_add_divmod_axioms(E->vc, a, k);
}

/* RM-B2: a proposition is not a number.  vc_mk would happily build
 * `(+ <bool> 1)` with an arith sort and hand a backend a term whose kid it
 * cannot interpret, so reject instead -- Unknown keeps the runtime check. */
static bool enc_want_value(Enc *E, const VCTerm *t) {
    if (t && t->sort == VS_BOOL) {
        E->fail = "arithmetic operand is a proposition, not a number";
        return false;
    }
    return t != NULL;
}

static bool enc_want_prop(Enc *E, const VCTerm *t) {
    if (t && t->sort != VS_BOOL) {
        E->fail = "logical operand does not denote a proposition";
        return false;
    }
    return t != NULL;
}

static VCTerm *enc_binary_arith(Enc *E, VCOp op, const Form *f,
                                VCTerm *a, VCTerm *b) {
    if (op == VC_MUL && !is_const_term(a) && !is_const_term(b))
        return enc_nonlinear(E, "__nl_mul", f, a, b);
    if ((op == VC_DIV || op == VC_MOD) && !is_const_term(b))
        return enc_nonlinear(E, op == VC_DIV ? "__nl_div" : "__nl_mod", f, a, b);
    VCTerm *t = vc_mk2(E->vc, op, a, b);
    if ((op == VC_DIV || op == VC_MOD) && t && t->sort == VS_INT)
        enc_divmod_axioms(E, a, b);
    return t;
}

/* n-ary fold for + - * with the language's variadic arithmetic. */
static VCTerm *enc_nary_arith(Enc *E, VCOp op, const Form *f) {
    uint32_t n = f->as.list.len;
    if (n < 2) {
        if (op == VC_SUB && n == 2 - 1) { /* unreachable: n<2 means only head */ }
        E->fail = "arithmetic operator needs at least one operand";
        return NULL;
    }
    /* (- x) is negation. */
    if (op == VC_SUB && n == 2) {
        VCTerm *a = enc(E, f->as.list.items[1]);
        if (!enc_want_value(E, a)) return NULL;
        return vc_mk1(E->vc, VC_NEG, a);
    }
    VCTerm *acc = enc(E, f->as.list.items[1]);
    if (!enc_want_value(E, acc)) return NULL;
    for (uint32_t i = 2; i < n; i++) {
        VCTerm *b = enc(E, f->as.list.items[i]);
        if (!enc_want_value(E, b)) return NULL;
        acc = enc_binary_arith(E, op, f, acc, b);
        if (!acc) return NULL;
    }
    return acc;
}

/* Chained comparison, as the language reads it: (< a b c) == (and (< a b) (< b c)). */
static VCTerm *enc_cmp(Enc *E, VCOp op, bool swap, const Form *f) {
    uint32_t n = f->as.list.len;
    if (n < 3) { E->fail = "comparison needs two operands"; return NULL; }
    VCTerm *acc = NULL;
    VCTerm *prev = enc(E, f->as.list.items[1]);
    if (!prev) return NULL;
    for (uint32_t i = 2; i < n; i++) {
        VCTerm *cur = enc(E, f->as.list.items[i]);
        if (!cur) return NULL;
        /* An ORDERING over propositions is meaningless; an EQUALITY between
         * them is not, but only when both sides are propositions.  A mixed
         * pair is a sort error the encoder must not paper over. */
        if (op != VC_EQ) {
            if (!enc_want_value(E, prev) || !enc_want_value(E, cur)) return NULL;
        } else if ((prev->sort == VS_BOOL) != (cur->sort == VS_BOOL)) {
            E->fail = "equality compares a proposition with a number";
            return NULL;
        }
        VCTerm *atom;
        if (op == VC_EQ && prev->sort == VS_BOOL) {
            /* An equality between propositions is an `iff`.  As a VC_EQ atom
             * the cube expansion cannot see inside it, so `(= r true)` stayed
             * unknown where a bare `r` proved (reflect-two-provable-facts-
             * report-as-not-holding) -- the limitation rf_def
             * already works around for a Bool measure's own equation.  Against
             * a literal it is the other side itself (or its negation); otherwise
             * the implication pair, which the expansion splits natively. */
            if      (cur->op  == VC_TRUE)  atom = prev;
            else if (cur->op  == VC_FALSE) atom = vc_not(E->vc, prev);
            else if (prev->op == VC_TRUE)  atom = cur;
            else if (prev->op == VC_FALSE) atom = vc_not(E->vc, cur);
            else atom = vc_mk2(E->vc, VC_AND,
                               vc_mk2(E->vc, VC_IMPLIES, prev, cur),
                               vc_mk2(E->vc, VC_IMPLIES, cur, prev));
        } else {
            atom = swap ? vc_mk2(E->vc, op, cur, prev)
                        : vc_mk2(E->vc, op, prev, cur);
        }
        acc = acc ? vc_mk2(E->vc, VC_AND, acc, atom) : atom;
        prev = cur;
    }
    return acc;
}

static VCTerm *enc_nary_bool(Enc *E, VCOp op, const Form *f) {
    uint32_t n = f->as.list.len;
    if (n < 2) return vc_bool(E->vc, op == VC_AND);
    VCTerm *acc = enc(E, f->as.list.items[1]);
    if (!enc_want_prop(E, acc)) return NULL;
    for (uint32_t i = 2; i < n; i++) {
        VCTerm *b = enc(E, f->as.list.items[i]);
        if (!enc_want_prop(E, b)) return NULL;
        acc = vc_mk2(E->vc, op, acc, b);
    }
    return acc;
}

/* An unrecognised head becomes a named measure -- an uninterpreted function
 * symbol reasoned about by congruence closure, never unfolded.  This is the
 * language rule that keeps S1 tractable. */
/* Two occurrences of a call may only be modelled as the same value when the
 * callee is KNOWN pure.  An unresolvable name is an abstract measure, which the
 * language defines as an uninterpreted mathematical function -- congruent by
 * construction.  Anything else has to say so.
 *
 * Getting this wrong is not a missed proof, it is a miscompile: with `tick`
 * counting up, `(- (tick) (tick))` encoded congruently becomes `t - t`, the
 * solver proves `t - t >= 0`, and the runtime check that would have caught the
 * real value of -1 is elided. */
static bool enc_callee_is_pure(Enc *E, const char *name, RefineFnInfo *out) {
    memset(out, 0, sizeof(*out));
    if (!E->env || !E->env->resolve_fn) return false;   /* no information: assume not */
    if (!E->env->resolve_fn(E->env->resolve_ud, name, out)) {
        /* Unresolved: an abstract measure. */
        out->pure = true;
        return true;
    }
    return out->pure;
}

/* Mint a name that cannot collide with another occurrence's. */
static const char *enc_fresh_name(Enc *E, const char *base) {
    char buf[96];
    snprintf(buf, sizeof(buf), "%s#%u", base, E->vc->fresh_ctr++);
    return arena_strdup(E->vc->arena, buf, strlen(buf));
}

/* ------------------------------------------------------------------------- *
 * reflected-measures RF3: bounded ground unfolding
 * (docs/upcoming/reflected-measures-plan.md)
 *
 * A `^reflect` function that passed the totality gate (elab_reflect.c) has a
 * defining equation the solver may use.  The supported fragment is
 * quantifier-free, so the equation is never asserted as `forall x. f(x) = ...`
 * -- that is the solver cliff the design stays on the cheap side of.  It is
 * asserted one GROUND INSTANCE at a time, by syntactic reduction:
 *
 *   for an application f(t) in the VC, substitute the argument forms for the
 *   parameters in the body, select `match` arms syntactically where the
 *   scrutinee is constructor-headed or a literal, and assert
 *   f(t) = <the surviving expression>.
 *
 * `if` and guarded arms have no term in the logic, so a body under one is
 * asserted as a PROPOSITION rather than an equation:
 *   def(f(t), (if c a b)) = (c => def(f(t), a)) and (not c => def(f(t), b)).
 * The match itself is gone by the time anything is encoded -- no encoding of
 * `match` exists here -- and a `match` whose scrutinee is NOT ground declines
 * (RF4 territory).  New application terms the reduction introduces
 * (`len(Nil)` inside `1 + len(Nil)`) are encoded through enc_measure again and
 * unfold in turn, bounded by the obligation's fuel: running out costs
 * completeness (the equation is simply not asserted; TUR-W0385 if the
 * obligation then stays unknown), never soundness.
 *
 * Every equation is DEFINITIONAL -- true of the total function it came from
 * -- so, like RT4's propagated refinements, it is sound wherever the
 * application appears, including under a negation.  The whole soundness
 * argument rests on the callee being total, which is why `reflect_total` is
 * the only thing that turns this on and why RF1/RF2 are a hard gate.
 * ------------------------------------------------------------------------- */

#define RF_ENV_MAX   48
#define RF_DEF_DEPTH 32

/* The measure's local names, bound to forms in the CALLER's namespace (the
 * argument forms of the application, and pattern/let variables bound to
 * sub-forms of those).  Because every value is a caller-namespace form, the
 * reduced expression never mentions a measure-local name and the caller's
 * substitution applies to it unchanged. */
typedef struct RfBind { const char *name; const Form *form; } RfBind;
typedef struct RfEnv  { RfBind b[RF_ENV_MAX]; uint32_t n; } RfEnv;

static uint32_t rf_fuel_default(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *s = getenv("TUR_REFLECT_FUEL");
        long v = s ? strtol(s, NULL, 10) : 8;
        if (v < 0) v = 0;
        /* Each unfolding step is a C-stack frame (rf_unfold -> rf_def -> enc
         * -> enc_measure -> rf_unfold), so the override is capped where the
         * stack is still comfortably bounded. */
        if (v > 256) v = 256;
        cached = (int)v;
    }
    return (uint32_t)cached;
}

static const Form *rf_env_lookup(const RfEnv *env, const char *name) {
    for (uint32_t i = env->n; i-- > 0; )
        if (strcmp(env->b[i].name, name) == 0) return env->b[i].form;
    return NULL;
}

static bool rf_env_bind(RfEnv *env, const char *name, const Form *form) {
    if (!name || env->n >= RF_ENV_MAX) return false;
    env->b[env->n].name = name; env->b[env->n].form = form; env->n++;
    return true;
}

static bool rf_form_is_literal(const Form *f) {
    return f && (f->tag == F_INT || f->tag == F_FLOAT || f->tag == F_BOOL ||
                 f->tag == F_STR || f->tag == F_NIL);
}

/* A symbol form the encoder can mint without a symbol table.  Every consumer
 * of a Form inside the encoder compares symbols by NAME (form_equal, sym_is,
 * enc's `->name` reads), so an arena Symbol that is never interned is
 * indistinguishable from an interned one here -- and these forms never leave
 * the encoder (a ufunc `origin` is the one place they are stored, and it is
 * read for its span alone). */
static const Form *rf_sym_form(RefineVC *vc, const char *name) {
    Symbol *s = (Symbol *)arena_alloc(vc->arena, sizeof(Symbol));
    size_t n = strlen(name);
    s->name = arena_strdup(vc->arena, name, n);
    s->len  = (uint32_t)n;
    s->hash = 0;
    Span sp; memset(&sp, 0, sizeof(sp));
    return form_sym(vc->arena, sp, s);
}

/* `(.field v)` -- the selector form the arm hypotheses are written with. */
static const Form *rf_sel_form(RefineVC *vc, const char *field, const Form *v) {
    char acc[128];
    snprintf(acc, sizeof(acc), ".%s", field);
    Form **k = (Form **)arena_alloc(vc->arena, 2 * sizeof(Form *));
    k[0] = (Form *)rf_sym_form(vc, acc);
    k[1] = (Form *)v;
    return form_list(vc->arena, v->span, k, 2);
}

/* ---- RF4: what the hypotheses say about a NON-ground scrutinee ----------
 *
 * A caller's own `match` arm puts two kinds of fact in the environment
 * (rt_prove_paths): the constructor's discriminant, `(= (#dt/tag s) k)`, and
 * each binder's identity, `(= t (.tl s))`.  Neither is `s = (Cons h t)`, so
 * the syntactic reduction of RF3 has no constructor term to select an arm
 * against.  It does have enough: the TAG picks the arm, and the binders of
 * that arm are the field SELECTORS applied to the scrutinee -- the very
 * terms the arm hypotheses already equate the caller's binders to, so
 * congruence closure connects them for free.
 *
 * Forms are compared modulo the hypotheses' variable equations: `t` and
 * `(.tl xs)` are one thing when `(= t (.tl xs))` is in scope, so a tag fact
 * about `t` answers for the reduced form `(.tl xs)` and vice versa.  That is
 * a bounded syntactic canonicalisation, not congruence closure -- a
 * completeness knob; every fact consulted is a hypothesis of this path, so
 * an arm selected here is the arm that runs on it. */

#define RF_CANON_DEPTH 6

static bool rf_hyp_is_plain_eq(const RefineHyp *h, const Form **a, const Form **b) {
    if (!h || h->bound_var || !h->pred || h->pred->tag != F_LIST ||
        h->pred->as.list.len != 3) return false;
    const Form *hd = h->pred->as.list.items[0];
    if (!sym_is(hd, "=") && !sym_is(hd, "==")) return false;
    *a = h->pred->as.list.items[1];
    *b = h->pred->as.list.items[2];
    return true;
}

/* Replace every symbol that some hypothesis equates to a non-symbol form by
 * that form, recursively and boundedly, so two spellings of one value
 * compare equal.  Returns `f` itself when nothing changes. */
static const Form *rf_canon(const Enc *E, const Form *f, uint32_t depth) {
    if (!f || depth > RF_CANON_DEPTH || !E->env) return f;
    if (f->tag == F_SYM) {
        for (const RefineHyp *h = E->env->head; h; h = h->next) {
            const Form *a, *b;
            if (!rf_hyp_is_plain_eq(h, &a, &b)) continue;
            const Form *other = NULL;
            if (form_equal(a, f) && b->tag != F_SYM) other = b;
            else if (form_equal(b, f) && a->tag != F_SYM) other = a;
            if (other && other->tag == F_LIST) return rf_canon(E, other, depth + 1);
        }
        return f;
    }
    if (f->tag != F_LIST || f->as.list.len == 0) return f;
    Form **items = NULL;
    for (uint32_t i = 0; i < f->as.list.len; i++) {
        const Form *c = rf_canon(E, f->as.list.items[i], depth + 1);
        if (c == f->as.list.items[i] && !items) continue;
        if (!items) {
            items = (Form **)arena_alloc(E->vc->arena, f->as.list.len * sizeof(Form *));
            for (uint32_t j = 0; j < i; j++) items[j] = f->as.list.items[j];
        }
        items[i] = (Form *)c;
    }
    return items ? form_list(E->vc->arena, f->span, items, f->as.list.len) : f;
}

/* The constructor tag the hypotheses pin `scrut` to, if any. */
static bool rf_hyp_tag_of(const Enc *E, const Form *scrut, int64_t *tag) {
    if (!E->env) return false;
    const Form *cs = rf_canon(E, scrut, 0);
    for (const RefineHyp *h = E->env->head; h; h = h->next) {
        const Form *a, *b;
        if (!rf_hyp_is_plain_eq(h, &a, &b)) continue;
        for (int side = 0; side < 2; side++) {
            const Form *x = side ? b : a, *y = side ? a : b;
            if (!x || x->tag != F_LIST || x->as.list.len != 2 ||
                !sym_is(x->as.list.items[0], "#dt/tag") || !y || y->tag != F_INT)
                continue;
            if (form_equal(rf_canon(E, x->as.list.items[1], 0), cs)) { *tag = y->as.i; return true; }
        }
    }
    return false;
}

/* The literal the hypotheses equate `scrut` to, if any (a literal-pattern
 * arm's `(= s 0)`). */
static const Form *rf_hyp_literal_of(const Enc *E, const Form *scrut) {
    if (!E->env) return NULL;
    const Form *cs = rf_canon(E, scrut, 0);
    for (const RefineHyp *h = E->env->head; h; h = h->next) {
        const Form *a, *b;
        if (!rf_hyp_is_plain_eq(h, &a, &b)) continue;
        if (rf_form_is_literal(b) && form_equal(rf_canon(E, a, 0), cs)) return b;
        if (rf_form_is_literal(a) && form_equal(rf_canon(E, b, 0), cs)) return a;
    }
    return NULL;
}

/* Is this caller-namespace form headed by a data constructor? */
static bool rf_form_is_ctor_app(const Enc *E, const Form *f) {
    if (!f || f->tag != F_LIST || f->as.list.len == 0) return false;
    const Form *h = f->as.list.items[0];
    if (h->tag != F_SYM || !h->as.sym) return false;
    if (!E->env || !E->env->resolve_fn) return false;
    RefineFnInfo ci; memset(&ci, 0, sizeof(ci));
    return E->env->resolve_fn(E->env->resolve_ud, h->as.sym->name, &ci) && ci.is_ctor;
}

/* Reduce a body expression in VALUE position to a caller-namespace form.
 * NULL declines: a name the environment does not bind (a global, which the
 * first cut does not read), control flow in value position, or a shape the
 * gate never admitted. */
static const Form *rf_reduce(Enc *E, const Form *f, const RfEnv *env, uint32_t depth) {
    if (!f || depth > RF_DEF_DEPTH) return NULL;
    switch (f->tag) {
    case F_INT: case F_FLOAT: case F_BOOL: case F_STR: case F_NIL:
        return f;
    case F_SYM: {
        const char *nm = f->as.sym ? f->as.sym->name : NULL;
        if (!nm) return NULL;
        if (strcmp(nm, "true") == 0 || strcmp(nm, "false") == 0) return f;
        return rf_env_lookup(env, nm);
    }
    case F_LIST: {
        if (f->as.list.len == 0) return NULL;
        const Form *h = f->as.list.items[0];
        if (h->tag != F_SYM || !h->as.sym) return NULL;
        if (sym_is(h, "if") || sym_is(h, "match") || sym_is(h, "let") || sym_is(h, "do"))
            return NULL;   /* control flow in value position: not in the first cut */
        uint32_t n = f->as.list.len;
        Form **items = (Form **)arena_alloc(E->vc->arena, n * sizeof(Form *));
        items[0] = (Form *)h;
        for (uint32_t i = 1; i < n; i++) {
            const Form *r = rf_reduce(E, f->as.list.items[i], env, depth + 1);
            if (!r) return NULL;
            items[i] = (Form *)r;
        }
        return form_list(E->vc->arena, f->span, items, n);
    }
    default:
        return NULL;
    }
}

/* Match one pattern against a caller-namespace form.  1 = matches (binding
 * its variables into `env`), 0 = does not match, -1 = cannot tell (the form
 * is not ground where the pattern needs it to be). */
static int rf_match_pat(Enc *E, const Form *pat, const Form *v, RfEnv *env) {
    if (!pat || !v) return -1;
    if (pat->tag == F_SYM) {
        if (sym_is(pat, "_")) return 1;
        return rf_env_bind(env, pat->as.sym ? pat->as.sym->name : NULL, v) ? 1 : -1;
    }
    if (rf_form_is_literal(pat)) {
        if (!rf_form_is_literal(v)) {
            /* RF4: a literal-pattern arm's own `(= s <lit>)` fact. */
            const Form *lit = rf_hyp_literal_of(E, v);
            if (!lit) return -1;
            E->vc->reflect_arms_by_hyp++;
            v = lit;
        }
        if (pat->tag != v->tag) {
            /* An int literal against a float literal (or any cross-tag pair)
             * is a typing question the elaborator settled; decline. */
            return -1;
        }
        switch (pat->tag) {
        case F_INT:   return pat->as.i == v->as.i;
        case F_BOOL:  return pat->as.b == v->as.b;
        case F_FLOAT: return pat->as.f == v->as.f;
        case F_STR:   return pat->as.s.len == v->as.s.len &&
                             memcmp(pat->as.s.p, v->as.s.p, pat->as.s.len) == 0;
        case F_NIL:   return 1;
        default:      return -1;
        }
    }
    if (pat->tag == F_LIST && pat->as.list.len > 0) {
        const Form *ph = pat->as.list.items[0];
        if (ph->tag != F_SYM || !ph->as.sym) return -1;
        if (!rf_form_is_ctor_app(E, v)) {
            /* RF4: not a constructor term -- ask the hypotheses for its tag,
             * select by tag, and bind the arm's variables to the field
             * selectors the caller's own arm hypotheses are written with. */
            int64_t tag;
            if (!rf_hyp_tag_of(E, v, &tag)) return -1;
            if (!E->env || !E->env->resolve_fn) return -1;
            RefineFnInfo ci; memset(&ci, 0, sizeof(ci));
            if (!E->env->resolve_fn(E->env->resolve_ud, ph->as.sym->name, &ci) || !ci.is_ctor)
                return -1;
            if ((int64_t)ci.ctor_tag != tag) return 0;
            if (pat->as.list.len - 1 != ci.ctor_n_fields) return -1;
            E->vc->reflect_arms_by_hyp++;
            for (uint32_t i = 1; i < pat->as.list.len; i++) {
                const Form *sp = pat->as.list.items[i];
                if (sp->tag == F_SYM && sym_is(sp, "_")) continue;
                /* Only a record constructor has a selector to bind through. */
                if (!ci.ctor_is_record || !ci.ctor_field_names || !ci.ctor_field_names[i - 1])
                    return -1;
                const Form *sel = rf_sel_form(E->vc, ci.ctor_field_names[i - 1], v);
                int r = rf_match_pat(E, sp, sel, env);
                if (r != 1) return r;
            }
            return 1;
        }
        const Form *vh = v->as.list.items[0];
        if (strcmp(ph->as.sym->name, vh->as.sym->name) != 0) return 0;
        if (pat->as.list.len != v->as.list.len) return -1;   /* arity: not ours to judge */
        for (uint32_t i = 1; i < pat->as.list.len; i++) {
            int r = rf_match_pat(E, pat->as.list.items[i], v->as.list.items[i], env);
            if (r != 1) return r;
        }
        return 1;
    }
    return -1;
}

static VCTerm *rf_def(Enc *E, VCTerm *app, const Form *body, const RfEnv *env, uint32_t depth);

/* The arms of a `match` from index `idx` on, against the reduced scrutinee
 * `fs`.  Returns the proposition for the first matching arm (a guarded arm
 * splits on its guard and falls through to the arms after it). */
static VCTerm *rf_def_arms(Enc *E, VCTerm *app, const Form *m, uint32_t idx,
                           const Form *fs, const RfEnv *env, uint32_t depth) {
    while (idx < m->as.list.len) {
        const Form *pat = m->as.list.items[idx++];
        const Form *guard = NULL;
        if (idx + 1 < m->as.list.len && sym_is(m->as.list.items[idx], "when")) {
            guard = m->as.list.items[idx + 1];
            idx += 2;
        }
        if (idx >= m->as.list.len) return NULL;
        const Form *arm_body = m->as.list.items[idx++];
        RfEnv env2 = *env;
        int r = rf_match_pat(E, pat, fs, &env2);
        if (r < 0) return NULL;
        if (r == 0) continue;
        if (!guard) return rf_def(E, app, arm_body, &env2, depth + 1);
        const Form *fg = rf_reduce(E, guard, &env2, depth + 1);
        if (!fg) return NULL;
        VCTerm *tg = enc(E, fg);
        if (!tg || tg->sort != VS_BOOL) return NULL;
        VCTerm *yes = rf_def(E, app, arm_body, &env2, depth + 1);
        if (!yes) return NULL;
        VCTerm *no = rf_def_arms(E, app, m, idx, fs, env, depth + 1);
        if (!no) return NULL;
        return vc_mk2(E->vc, VC_AND,
                      vc_mk2(E->vc, VC_IMPLIES, tg, yes),
                      vc_mk2(E->vc, VC_IMPLIES, vc_not(E->vc, tg), no));
    }
    return NULL;   /* no arm matched -- cannot happen for a covered match */
}

/* The proposition "app is what `body` computes", in the environment. */
static VCTerm *rf_def(Enc *E, VCTerm *app, const Form *body, const RfEnv *env, uint32_t depth) {
    if (!body || depth > RF_DEF_DEPTH || E->fail) return NULL;
    if (body->tag == F_LIST && body->as.list.len > 0) {
        const Form *h = body->as.list.items[0];
        if (sym_is(h, "if")) {
            if (body->as.list.len != 4) return NULL;
            const Form *fc = rf_reduce(E, body->as.list.items[1], env, depth + 1);
            if (!fc) return NULL;
            VCTerm *tc = enc(E, fc);
            if (!tc || tc->sort != VS_BOOL) return NULL;
            VCTerm *a = rf_def(E, app, body->as.list.items[2], env, depth + 1);
            if (!a) return NULL;
            VCTerm *b = rf_def(E, app, body->as.list.items[3], env, depth + 1);
            if (!b) return NULL;
            return vc_mk2(E->vc, VC_AND,
                          vc_mk2(E->vc, VC_IMPLIES, tc, a),
                          vc_mk2(E->vc, VC_IMPLIES, vc_not(E->vc, tc), b));
        }
        if (sym_is(h, "do")) {
            if (body->as.list.len < 2) return NULL;
            /* Pure body: only the last form's value matters. */
            return rf_def(E, app, body->as.list.items[body->as.list.len - 1], env, depth + 1);
        }
        if (sym_is(h, "let")) {
            if (body->as.list.len < 3 || body->as.list.items[1]->tag != F_VEC) return NULL;
            const Form *bv = body->as.list.items[1];
            RfEnv env2 = *env;
            uint32_t i = 0;
            while (i < bv->as.list.len) {
                const Form *name = bv->as.list.items[i++];
                if (name->tag != F_SYM || !name->as.sym) return NULL;
                if (i + 1 < bv->as.list.len && sym_is(bv->as.list.items[i], ":")) i += 2;
                if (i >= bv->as.list.len) return NULL;
                const Form *init = rf_reduce(E, bv->as.list.items[i++], &env2, depth + 1);
                if (!init) return NULL;
                if (!rf_env_bind(&env2, name->as.sym->name, init)) return NULL;
            }
            return rf_def(E, app, body->as.list.items[body->as.list.len - 1], &env2, depth + 1);
        }
        if (sym_is(h, "match")) {
            if (body->as.list.len < 4) return NULL;
            const Form *fs = rf_reduce(E, body->as.list.items[1], env, depth + 1);
            if (!fs) return NULL;
            /* A constructor term or a literal selects its arm directly; any
             * other scrutinee selects through the hypotheses (RF4,
             * rf_match_pat) or declines with -1 and no equation. */
            return rf_def_arms(E, app, body, 2, fs, env, depth + 1);
        }
    }
    const Form *fr = rf_reduce(E, body, env, depth + 1);
    if (!fr) return NULL;
    VCTerm *t = enc(E, fr);
    if (!t) return NULL;
    /* The equation must be well-sorted: a Bool measure equals a proposition,
     * an Int/Real one equals a value of the SAME sort (an Int measure over a
     * Real body would be the RM-B0 missort in a new coat). */
    if ((t->sort == VS_BOOL) != (app->sort == VS_BOOL)) return NULL;
    if (t->sort != VS_BOOL && t->sort != app->sort) return NULL;
    /* A Bool measure is a proposition, and its equation is an `iff`.  Spelled
     * as two implications rather than `(= p q)`: the solver's cube expansion
     * splits an implication natively, while an equality between propositions
     * is an atom it cannot see inside (the `all-pos?` fixture stayed Unknown
     * under `=` and proves under the pair). */
    if (t->sort == VS_BOOL)
        return vc_mk2(E->vc, VC_AND,
                      vc_mk2(E->vc, VC_IMPLIES, app, t),
                      vc_mk2(E->vc, VC_IMPLIES, t, app));
    return vc_mk2(E->vc, VC_EQ, app, t);
}

/* An argument form as the CALLER wrote it.  A predicate's argument may be the
 * refinement's bound variable or a callee parameter name; both stand for a
 * caller form at this obligation (the subject, or a crossing's argument), and
 * that form is what a `match` arm can be selected against.  The encoder's
 * own substitution already maps these names to the same TERMS, so the
 * reduced expression encodes to terms `app` was built from. */
static const Form *rf_resolve_arg(const Enc *E, const Form *a) {
    for (uint32_t hop = 0; hop < 4 && a && a->tag == F_SYM && a->as.sym; hop++) {
        const char *nm = a->as.sym->name;
        const Form *next = NULL;
        if (E->rf_subject_name && E->rf_subject_form && strcmp(nm, E->rf_subject_name) == 0)
            next = E->rf_subject_form;
        for (uint32_t i = 0; !next && i < E->n_ob_subst; i++)
            if (E->ob_subst[i].name && E->ob_subst[i].form &&
                strcmp(E->ob_subst[i].name, nm) == 0)
                next = E->ob_subst[i].form;
        if (!next || next == a) break;
        a = next;
    }
    return a;
}

/* Assert the defining equation of a TOTAL reflected callee at `app` (the
 * application of `call`'s head to `call`'s arguments), within fuel. */
static void rf_unfold(Enc *E, const RefineFnInfo *info, const Form *call, VCTerm *app) {
    if (!E->rf_fuel || !info->reflect_body || !info->reflect_param_names) return;
    RefineVC *vc = E->vc;
    uint32_t argc = call->as.list.len - 1;
    if (argc != info->reflect_n_params) return;
    /* Once per distinct application term: a term reached again (the same
     * VCTerm, by hash-consing) already carries its equation. */
    for (uint32_t i = 0; i < vc->n_reflect_done; i++)
        if (vc->reflect_done[i] == app->id) return;
    if (*E->rf_fuel == 0) { vc->reflect_fuel_exhausted = true; return; }
    (*E->rf_fuel)--;
    if (vc->n_reflect_done == vc->cap_reflect_done) {
        uint32_t ncap = vc->cap_reflect_done ? vc->cap_reflect_done * 2 : 16;
        uint32_t *nb = (uint32_t *)arena_alloc(vc->arena, ncap * sizeof(uint32_t));
        if (vc->reflect_done) memcpy(nb, vc->reflect_done, vc->n_reflect_done * sizeof(uint32_t));
        vc->reflect_done = nb; vc->cap_reflect_done = ncap;
    }
    vc->reflect_done[vc->n_reflect_done++] = app->id;

    RfEnv env; memset(&env, 0, sizeof(env));
    for (uint32_t i = 0; i < argc; i++)
        if (!rf_env_bind(&env, info->reflect_param_names[i],
                         rf_resolve_arg(E, call->as.list.items[i + 1])))
            return;
    /* An isolated encoder: a reduction the fragment cannot encode drops just
     * this equation (fewer hypotheses only make the goal harder), never the
     * predicate that mentioned the call.  The substitution, frozen set and
     * fuel are the caller's, so the argument forms encode to the very terms
     * `app` was built from. */
    Enc E2 = *E;
    E2.fail = NULL; E2.depth = 0;
    VCTerm *prop = rf_def(&E2, app, info->reflect_body, &env, 0);
    if (prop && !E2.fail && prop->sort == VS_BOOL) {
        vc_add_hyp(vc, prop);
        vc->reflect_unfolds++;
    }
}

static VCTerm *enc_measure(Enc *E, const Form *f) {
    const Form *head = f->as.list.items[0];
    if (head->tag != F_SYM && head->tag != F_KEYWORD) {
        E->fail = "predicate head is not a name";
        return NULL;
    }
    RefineFnInfo info;
    bool pure = enc_callee_is_pure(E, head->as.sym->name, &info);

    /* C2 / #reads: a measure declared `#reads w` (or `#reads [w g]`) is
     * congruent when EVERY named argument is FROZEN at this site -- a live borrow (the `(& w)` a `frozen`
     * region holds) proves the state it reads cannot change here, so two
     * occurrences denote one value.  This grants congruence to an otherwise
     * impure measure, and it is sound ONLY because the callee's own entry check
     * is never elided (this proof elides the caller-side crossing check, not
     * the safety check) -- see docs/guides/stateful-refinements-guide.md.  It
     * never turns a proof INTO impurity, so it cannot lose an existing one. */
    /* R2 (trusted-refinement-claims-plan, graduated 2026-08-20): refuse the
     * grant on positive evidence that the frame omits mutable state the body
     * reads (TUR-W0383's finding, carried on the resolved info).  The measure
     * then encodes fresh-per-occurrence like any unframed impure callee, and
     * the crossing gets the ordinary TUR-W0372.  Keys on "saw a read", never
     * "could not see" -- an unwalkable (inline-C) body carries no evidence and
     * keeps the trusted grant. */
    if (!pure && info.reads_params_mask != 0 &&
        !info.reads_frame_omits_state &&
        enc_reads_args_frozen(E, f, info.reads_params_mask))
        pure = true;

    /* RM-B1/RM-B2: the sort this measure symbol is declared at.  Settled once
     * per VC by the prescan (which consults the same resolver), so every
     * occurrence of a name agrees. */
    VCSort msort = enc_name_sort(E, head->as.sym->name, info.ret_sort);

    uint32_t argc = f->as.list.len - 1;
    if (argc == 0) {
        /* A nullary call is an opaque constant.  When it is not known pure,
         * each occurrence is a DIFFERENT constant -- which is exactly what
         * makes `(- (tick) (tick))` unprovable again. */
        const char *nm = pure ? head->as.sym->name
                              : enc_fresh_name(E, head->as.sym->name);
        uint32_t v = vc_declare_var(E->vc, nm, msort);
        /* RF6: a nullary constructor is a free constant, not a value to
         * enumerate; remember it so the model search treats it as one. */
        if (info.is_ctor) {
            RefineVC *vc = E->vc;
            bool seen = false;
            for (uint32_t i = 0; i < vc->n_ctor_consts; i++)
                if (vc->ctor_consts[i] == v) { seen = true; break; }
            if (!seen) {
                if (vc->n_ctor_consts == vc->cap_ctor_consts) {
                    uint32_t ncap = vc->cap_ctor_consts ? vc->cap_ctor_consts * 2 : 8;
                    uint32_t *nb = (uint32_t *)arena_alloc(vc->arena, ncap * sizeof(uint32_t));
                    if (vc->ctor_consts) memcpy(nb, vc->ctor_consts, vc->n_ctor_consts * sizeof(uint32_t));
                    vc->ctor_consts = nb; vc->cap_ctor_consts = ncap;
                }
                vc->ctor_consts[vc->n_ctor_consts++] = v;
            }
        }
        return vc_var_ref(E->vc, v);
    }
    VCTerm **args = (VCTerm **)arena_alloc(E->vc->arena, argc * sizeof(VCTerm *));
    for (uint32_t i = 0; i < argc; i++) {
        args[i] = enc(E, f->as.list.items[i + 1]);
        if (!args[i]) return NULL;
    }
    const char *fname = pure ? head->as.sym->name
                             : enc_fresh_name(E, head->as.sym->name);
    uint32_t fn = vc_declare_ufunc(E->vc, fname, argc, msort,
                                   f, /*nonlinear=*/false);
    /* RF6: what this symbol denotes, for the model search's evaluability
     * test.  Only a PURE (stable-named) symbol can be either. */
    if (pure) {
        if (info.is_ctor)       E->vc->ufuncs[fn].is_ctor   = true;
        if (info.reflect_total) E->vc->ufuncs[fn].reflected = true;
    }
    VCTerm *app = vc_app(E->vc, fn, args, argc);

    /* reflected-measures RF3: a TOTAL `^reflect` callee has a defining
     * equation the solver may use.  Assert it at this application, by
     * reduction, within the obligation's fuel. */
    if (pure && info.reflect_total) rf_unfold(E, &info, f, app);

    /* RT4: if the callee declares (or had inferred) a return refinement, that
     * predicate holds of the value this call produced -- either because it was
     * proved statically or because the runtime check would have panicked
     * otherwise.  Assert it, so the result of a refined function can satisfy
     * the next obligation instead of being an opaque term.
     *
     * That is a PARTIAL-correctness argument: for a call that never returns
     * there is no produced value and the fact is vacuous, which is fine only
     * because the obligation it feeds is about a program point after the
     * call.  For a `^reflect` callee the totality gate (elab_reflect.c) makes
     * the argument unconditional -- the call returns, so the fact is about a
     * value that exists (reflected-measures-plan RF6.3).
     *
     * The hypothesis is about THIS application term, so it is sound wherever
     * the call appears in the formula -- including under a negation. */
    if (E->env && E->env->resolve_fn) {
        const char *nm = head->as.sym->name;
        bool cycling = false;
        for (uint32_t i = 0; i < E->n_propagating; i++)
            if (strcmp(E->propagating[i], nm) == 0) { cycling = true; break; }
        /* `info` was filled by the purity probe above.  Propagating a return
         * refinement is sound whether or not the callee is pure -- the fact is
         * asserted about THIS occurrence's term, and an impure callee simply
         * has a term of its own. */
        if (!cycling && E->n_propagating < ENC_MAX_PROPAGATE &&
            info.ret_pred && info.ret_var) {
            Enc E2; memset(&E2, 0, sizeof(E2));
            E2.vc = E->vc; E2.env = E->env; E2.sorts = E->sorts;
            E2.rf_fuel = E->rf_fuel;
            for (uint32_t i = 0; i < E->n_propagating; i++)
                E2.propagating[E2.n_propagating++] = E->propagating[i];
            E2.propagating[E2.n_propagating++] = nm;
            /* The refinement is written in the CALLEE's names: its own result
             * variable and its own parameters.  Bind both to what this call
             * site actually supplied. */
            E2.subst[E2.n_subst].name = info.ret_var;
            E2.subst[E2.n_subst].term = app;
            E2.n_subst++;
            for (uint32_t i = 0; i < info.n_params && i < argc &&
                                 E2.n_subst < ENC_MAX_SUBST; i++) {
                if (!info.param_names[i]) continue;
                E2.subst[E2.n_subst].name = info.param_names[i];
                E2.subst[E2.n_subst].term = args[i];
                E2.n_subst++;
            }
            VCTerm *fact = enc(&E2, info.ret_pred);
            /* A refinement we cannot encode is simply not asserted -- fewer
             * hypotheses can only make a goal harder to prove. */
            if (fact && fact->sort == VS_BOOL) vc_add_hyp(E->vc, fact);
        }
    }
    return app;
}

/* `(as T e)` -- the language's numeric conversion.  It is a builtin, not a
 * function, so it must not fall through to enc_measure: that path declared
 * `as` as an ABSTRACT measure at the position-default sort (Int), and its
 * first argument -- the type NAME -- as a variable.  `(as float x)` over a
 * float `x` then became an Int-sorted opaque term, and S2's integer hull
 * tightened `t <= 2.75` to `t <= 2` and `2.25 <= t` to `3 <= t`, refuting a
 * cube that `x = 2.5` satisfies (fixtures errors/refine-cast-in-predicate-refuted
 * and refine-cast-in-predicate).
 *
 *   (as float e), e int   -> e itself.  The VC's reals are exact, and an
 *                            int converted to float denotes the same number
 *                            (the double rounding past 2^53 is the same
 *                            approximation every real-sorted fact here makes).
 *                            Keeping e's Int sort is what lets S2 keep using
 *                            its integrality -- `(as float n) <= 2.75` IS
 *                            `n <= 2`.
 *   (as float e), e real  -> e (identity).
 *   (as int e),   e int   -> e (identity).
 *   (as int e),   e real  -> truncation: an opaque INT-sorted term, congruent
 *                            across occurrences (the conversion is pure),
 *                            with no axioms -- opaque is sound.
 *   any other target      -> not encoded; the obligation keeps its runtime
 *                            check (narrowing conversions change values in
 *                            ways this fragment does not model; see the table in the body). */
static bool name_in(const char *n, const char *const *tbl, size_t k) {
    for (size_t i = 0; i < k; i++) if (strcmp(n, tbl[i]) == 0) return true;
    return false;
}

static VCTerm *enc_cast(Enc *E, const Form *f) {
    if (f->as.list.len != 3) { E->fail = "as takes a type and one operand"; return NULL; }
    const Form *ty = f->as.list.items[1];
    if ((ty->tag != F_SYM && ty->tag != F_KEYWORD) || !ty->as.sym) {
        E->fail = "cast target is not a type name"; return NULL;
    }
    const char *tn = ty->as.sym->name;
    /* The numeric targets `as` accepts (elab_as_cast: typekind_is_numeric),
     * by what the conversion does to the VALUE:
     *   exact_real  -- `float` / `f64`: the operand's number, unchanged
     *                  (the double rounding of a wide int is the same
     *                  approximation every real-sorted fact here makes);
     *   wide_int    -- `int` / `i64` / `isize`: identity on an int operand,
     *                  C truncation on a real one;
     *   anything else numeric -- `f32` (rounds), the narrower and unsigned
     *                  ints (wrap): value-changing, kept OPAQUE at the
     *                  target's sort, congruent across occurrences. */
    static const char *const EXACT_REAL[] = { "float", "f64" };
    static const char *const WIDE_INT[]   = { "int", "i64", "isize" };
    static const char *const OPAQUE_REAL[] = { "f32" };
    static const char *const OPAQUE_INT[]  = { "i8", "i16", "i32", "u8", "u16",
                                               "u32", "u64", "usize" };
    #define IN(tbl) name_in(tn, tbl, sizeof(tbl) / sizeof(tbl[0]))
    bool exact_real  = IN(EXACT_REAL), wide_int = IN(WIDE_INT);
    bool opaque_real = IN(OPAQUE_REAL), opaque_int = IN(OPAQUE_INT);
    #undef IN
    if (!exact_real && !wide_int && !opaque_real && !opaque_int) {
        E->fail = "unsupported cast target in predicate"; return NULL;
    }
    VCTerm *e = enc(E, f->as.list.items[2]);
    if (!enc_want_value(E, e)) return NULL;
    if (exact_real || (wide_int && e->sort == VS_INT)) return e;
    char nm[48];
    snprintf(nm, sizeof(nm), "as-%s#conv", tn);
    uint32_t fn = vc_declare_ufunc(E->vc, nm, 1, opaque_real ? VS_REAL : VS_INT, f, false);
    VCTerm *args[1] = { e };
    return vc_app(E->vc, fn, args, 1);
}

static VCTerm *enc(Enc *E, const Form *f) {
    if (!f) { E->fail = "empty predicate"; return NULL; }
    if (E->fail) return NULL;
    if (++E->depth > ENC_MAX_DEPTH) { E->fail = "predicate nested too deeply"; return NULL; }

    VCTerm *r = NULL;
    switch (f->tag) {
        case F_INT:   r = vc_int(E->vc, f->as.i);   break;
        case F_FLOAT: r = vc_real(E->vc, f->as.f);  break;
        case F_BOOL:  r = vc_bool(E->vc, f->as.b);  break;

        case F_KEYWORD:
        case F_SYM: {
            const char *nm = f->as.sym ? f->as.sym->name : NULL;
            if (!nm) { E->fail = "unnamed symbol"; break; }
            if (strcmp(nm, "true") == 0)  { r = vc_bool(E->vc, true);  break; }
            if (strcmp(nm, "false") == 0) { r = vc_bool(E->vc, false); break; }
            VCTerm *sub = enc_lookup(E, nm);
            if (sub) { r = sub; break; }
            uint32_t v = vc_declare_var(E->vc, nm, refine_env_sort_of(E->env, nm));
            r = vc_var_ref(E->vc, v);
            break;
        }

        case F_LIST: {
            if (f->as.list.len == 0) { E->fail = "empty list in predicate"; break; }
            const Form *h = f->as.list.items[0];
            if      (sym_is(h, "+"))    r = enc_nary_arith(E, VC_ADD, f);
            else if (sym_is(h, "-"))    r = enc_nary_arith(E, VC_SUB, f);
            else if (sym_is(h, "*"))    r = enc_nary_arith(E, VC_MUL, f);
            else if (sym_is(h, "/"))    r = enc_nary_arith(E, VC_DIV, f);
            else if (sym_is(h, "mod"))  r = enc_nary_arith(E, VC_MOD, f);
            else if (sym_is(h, "=") || sym_is(h, "==")) r = enc_cmp(E, VC_EQ, false, f);
            else if (sym_is(h, "<"))    r = enc_cmp(E, VC_LT, false, f);
            else if (sym_is(h, "<="))   r = enc_cmp(E, VC_LE, false, f);
            else if (sym_is(h, ">"))    r = enc_cmp(E, VC_LT, true,  f);
            else if (sym_is(h, ">="))   r = enc_cmp(E, VC_LE, true,  f);
            else if (sym_is(h, "not=") || sym_is(h, "!=") || sym_is(h, "<>")) {
                VCTerm *eq = enc_cmp(E, VC_EQ, false, f);
                r = eq ? vc_not(E->vc, eq) : NULL;
            }
            else if (sym_is(h, "and"))  r = enc_nary_bool(E, VC_AND, f);
            else if (sym_is(h, "or"))   r = enc_nary_bool(E, VC_OR, f);
            else if (sym_is(h, "not")) {
                if (f->as.list.len != 2) { E->fail = "not takes one operand"; break; }
                VCTerm *a = enc(E, f->as.list.items[1]);
                r = enc_want_prop(E, a) ? vc_not(E->vc, a) : NULL;
            }
            else if (sym_is(h, "=>") || sym_is(h, "implies")) {
                if (f->as.list.len != 3) { E->fail = "=> takes two operands"; break; }
                VCTerm *a = enc(E, f->as.list.items[1]);
                if (!enc_want_prop(E, a)) break;
                VCTerm *b = enc(E, f->as.list.items[2]);
                if (!enc_want_prop(E, b)) break;
                r = vc_mk2(E->vc, VC_IMPLIES, a, b);
            }
            else if (sym_is(h, "as")) r = enc_cast(E, f);
            else r = enc_measure(E, f);
            break;
        }

        default:
            E->fail = "unsupported form in predicate";
            break;
    }
    E->depth--;
    return E->fail ? NULL : r;
}

/* ------------------------------------------------------------------------- *
 * RM-B2: measure sort prescan
 *
 * Runs over every Form the VC will encode, BEFORE any symbol is declared, and
 * answers one question per measure name: Int, Real, or Bool?
 *
 *   - the name resolves to a function  -> its declared return type decides
 *     (RM-B1; this is what makes `(alive? w x)` a proposition and stops a
 *     float-returning `norm` from being declared Int and then integer-tightened)
 *   - the name resolves to nothing     -> it is an ABSTRACT measure with no
 *     declaration to read, so POSITION decides: Bool where the predicate
 *     grammar requires a proposition (the goal itself, or an operand of
 *     and/or/not/=>), Int everywhere else.
 *
 * A name used at BOTH sorts rejects the VC.  Picking one would declare a
 * symbol that means different things in the hypotheses and in the goal, which
 * is precisely how a congruence bug is written; the runtime check still covers
 * the obligation either way.
 * ------------------------------------------------------------------------- */

static void presort_note(EncSorts *S, const RefineEnv *env,
                         const char *name, EncPos pos) {
    EncMeasure *m = NULL;
    for (uint32_t i = 0; i < S->n; i++)
        if (strcmp(S->m[i].name, name) == 0) { m = &S->m[i]; break; }
    if (!m) {
        /* Table full: leave the name out.  enc_name_sort then falls back to the
         * callee's own sort, which is still ONE sort per name. */
        if (S->n == ENC_MAX_MEASURES) return;
        m = &S->m[S->n++];
        memset(m, 0, sizeof(*m));
        m->name = name;
        m->sort = VS_INT;
        RefineFnInfo info; memset(&info, 0, sizeof(info));
        if (env && env->resolve_fn && env->resolve_fn(env->resolve_ud, name, &info)) {
            m->resolved = true;
            m->sort     = info.ret_sort;
        }
    }
    if (pos == POS_PROP)       m->seen_bool = true;
    else if (pos == POS_VALUE) m->seen_val  = true;
}

static void presort_walk(EncSorts *S, const RefineEnv *env, const Form *f,
                         EncPos pos, uint32_t depth) {
    if (!f || depth > ENC_MAX_DEPTH) return;
    if (f->tag != F_LIST || f->as.list.len == 0) return;
    const Form *h = f->as.list.items[0];
    EncHead k = enc_head_kind(h);
    if (k == EH_MEASURE) {
        if ((h->tag == F_SYM || h->tag == F_KEYWORD) && h->as.sym)
            presort_note(S, env, h->as.sym->name, pos);
        /* A measure's own arity and parameter types are not known here (an
         * abstract measure has none), so its ARGUMENTS demand nothing. */
        for (uint32_t i = 1; i < f->as.list.len; i++)
            presort_walk(S, env, f->as.list.items[i], POS_NEUTRAL, depth + 1);
        return;
    }
    if (k == EH_CAST) {
        /* `as` is a builtin, not a measure, and its first operand is a type
         * name, not a term: only the value operand is walked. */
        if (f->as.list.len == 3)
            presort_walk(S, env, f->as.list.items[2], POS_VALUE, depth + 1);
        return;
    }
    EncPos kid = (k == EH_LOGIC) ? POS_PROP
               : (k == EH_EQ)    ? POS_NEUTRAL
                                 : POS_VALUE;
    for (uint32_t i = 1; i < f->as.list.len; i++)
        presort_walk(S, env, f->as.list.items[i], kid, depth + 1);
}

/* Settle every unresolved name's sort and report the first name that cannot
 * have one.  Returns NULL when the table is consistent.
 *
 * Only an UNRESOLVED name can conflict: a resolved one has exactly one sort,
 * its callee's, whatever positions it turns up in.  A resolved measure used in
 * the wrong kind of position is a local error and is handled locally, by the
 * enc_want_value / enc_want_prop guards -- which drop just that hypothesis
 * (sound: fewer hypotheses only make a goal harder) instead of discarding the
 * whole VC. */
static const char *presort_finish(EncSorts *S) {
    for (uint32_t i = 0; i < S->n; i++) {
        EncMeasure *m = &S->m[i];
        if (m->resolved) continue;
        m->sort = m->seen_bool ? VS_BOOL : VS_INT;
        if (m->seen_bool && m->seen_val) return m->name;
    }
    return NULL;
}

/* ------------------------------------------------------------------------- *
 * RT2: obligation -> normalized VC
 * ------------------------------------------------------------------------- */

RefineVC *refine_vc_build(RefineObligation *ob, Arena *a, const char **out_reason) {
    if (out_reason) *out_reason = NULL;
    if (!ob || !ob->predicate) {
        if (out_reason) *out_reason = "no predicate";
        return NULL;
    }

    RefineVC *vc = vc_new(a);
    /* reflected-measures RF3: one unfolding budget per obligation, shared by
     * every encoder below.  Deterministic reduction plus the RT7 memo make the
     * default a completeness knob only; TUR_REFLECT_FUEL overrides it. */
    uint32_t rf_fuel = rf_fuel_default();

    /* Declare every in-scope name with its sort up front so the encoder never
     * has to guess (an undeclared name still defaults to VS_INT). */
    if (ob->env) {
        for (uint32_t i = 0; i < ob->env->n_names; i++)
            vc_declare_var(vc, ob->env->names[i], ob->env->sorts[i]);
    }

    /* --- RM-B2: settle every measure's sort before declaring any symbol --- */
    EncSorts sorts; memset(&sorts, 0, sizeof(sorts));
    for (RefineHyp *h = ob->env ? ob->env->head : NULL; h; h = h->next)
        presort_walk(&sorts, ob->env, h->pred, POS_PROP, 0);
    /* A sibling substitution stands in for a CALLEE parameter whose type this
     * side does not have, so it demands nothing.  The subject does: it stands
     * in for the refinement's bound variable, whose sort is the base type's. */
    for (uint32_t i = 0; i < ob->n_subst; i++)
        presort_walk(&sorts, ob->env, ob->subst[i].form, POS_NEUTRAL, 0);
    presort_walk(&sorts, ob->env, ob->subject,
                 ob->base_sort == VS_BOOL ? POS_PROP : POS_VALUE, 0);
    presort_walk(&sorts, ob->env, ob->predicate, POS_PROP, 0);
    const char *clash = presort_finish(&sorts);
    if (clash) {
        if (out_reason) {
            char buf[160];
            snprintf(buf, sizeof(buf),
                     "measure '%s' is used both as a proposition and as a value",
                     clash);
            *out_reason = arena_strdup(a, buf, strlen(buf));
        }
        return NULL;
    }

    /* --- hypotheses ------------------------------------------------------ */
    for (RefineHyp *h = ob->env ? ob->env->head : NULL; h; h = h->next) {
        Enc E; memset(&E, 0, sizeof(E));
        E.vc = vc; E.env = ob->env; E.sorts = &sorts; E.rf_fuel = &rf_fuel;
        /* C2 / #reads: a hypothesis (a recovered guard) is written in the
         * CALLER's names, so it needs the frozen set but NOT the callee-param
         * subst. */
        E.frozen_names = ob->frozen_names; E.n_frozen = ob->n_frozen;
        if (h->bound_var && h->subject_name) {
            uint32_t v = vc_declare_var(vc, h->subject_name,
                                        refine_env_sort_of(ob->env, h->subject_name));
            E.subst[E.n_subst].name = h->bound_var;
            E.subst[E.n_subst].term = vc_var_ref(vc, v);
            E.n_subst++;
            /* RF4: the same binding as a FORM, so a reflected measure applied
             * to the bound variable unfolds at the subject name -- where the
             * caller's tag facts live. */
            E.rf_subject_name = h->bound_var;
            E.rf_subject_form = rf_sym_form(vc, h->subject_name);
        }
        VCTerm *t = enc(&E, h->pred);
        /* A hypothesis we cannot encode is simply dropped: fewer hypotheses
         * can only make the goal HARDER to prove, never easier, so this stays
         * on the safe side of the soundness invariant.  It is NOT safe for a
         * refutation, so the VC remembers the drop and the model search
         * declines it: until 2026-09-29 a `:pre` written with a `let` was
         * dropped here and the search then refuted the goal without it --
         * a TUR-E0371 hard error, with a witness, on a correct function
         * (fixture refine-dropped-hypothesis-keeps-check). */
        if (t) vc_add_hyp(vc, t);
        else {
            vc->hyps_dropped = true;
            if (getenv("TUR_REFINE_STATS"))
                fprintf(stderr, "refine: hypothesis not encoded (%s): %s\n",
                        E.fail ? E.fail : "outside the supported fragment",
                        ob->what ? ob->what : "obligation");
        }
    }

    /* --- goal ------------------------------------------------------------ */
    Enc E; memset(&E, 0, sizeof(E));
    E.vc = vc; E.env = ob->env; E.sorts = &sorts; E.rf_fuel = &rf_fuel;
    /* C2 / #reads: the goal predicate is written in the CALLEE's parameter
     * names, so it needs both the frozen set and the callee-param subst to
     * resolve a `#reads` world argument back to the caller expression. */
    E.frozen_names = ob->frozen_names; E.n_frozen = ob->n_frozen;
    E.ob_subst = ob->subst; E.n_ob_subst = ob->n_subst;
    /* Sibling substitutions first (a call-site crossing replaces every callee
     * parameter name with the caller's argument), then the refinement's own
     * bound variable.  Encoding each substituted form BEFORE the map is
     * consulted is deliberate: an argument expression is written in the
     * caller's names, so it must not be rewritten by the callee's. */
    for (uint32_t i = 0; i < ob->n_subst && E.n_subst < ENC_MAX_SUBST; i++) {
        if (!ob->subst[i].name || !ob->subst[i].form) continue;
        Enc E2; memset(&E2, 0, sizeof(E2));
        E2.vc = vc; E2.env = ob->env; E2.sorts = &sorts; E2.rf_fuel = &rf_fuel;
        VCTerm *t = enc(&E2, ob->subst[i].form);
        /* An un-encodable argument leaves the callee's parameter name FREE,
         * which is weaker than the truth in the same way a dropped
         * hypothesis is: a witness could bind it to a value the actual
         * argument never takes. */
        if (!t) { vc->hyps_dropped = true; continue; }
        E.subst[E.n_subst].name = ob->subst[i].name;
        E.subst[E.n_subst].term = t;
        E.n_subst++;
    }
    if (ob->var_name && ob->subject) {
        Enc E2; memset(&E2, 0, sizeof(E2));
        E2.vc = vc; E2.env = ob->env; E2.sorts = &sorts; E2.rf_fuel = &rf_fuel;
        VCTerm *subj = enc(&E2, ob->subject);
        if (!subj) {
            if (out_reason) *out_reason = E2.fail ? E2.fail : "subject expression is outside the supported fragment";
            return NULL;
        }
        if (E.n_subst < ENC_MAX_SUBST) {
            E.subst[E.n_subst].name = ob->var_name;
            E.subst[E.n_subst].term = subj;
            E.n_subst++;
        }
        E.rf_subject_name = ob->var_name;
        E.rf_subject_form = ob->subject;
    }
    VCTerm *goal = enc(&E, ob->predicate);
    if (!goal) {
        if (out_reason) *out_reason = E.fail ? E.fail : "predicate is outside the supported fragment";
        return NULL;
    }
    if (goal->sort != VS_BOOL) {
        if (out_reason) *out_reason = "predicate does not denote a proposition";
        return NULL;
    }
    vc_set_goal(vc, goal);
    /* RF6: may the bounded model search run on a VC that mentions
     * uninterpreted functions?  Yes iff every one of them is a data
     * constructor (a free value) or a reflected measure (defined by the
     * equations asserted above), and no unfolding ran out of fuel -- an
     * application left without its equation would be a free value the
     * search could bend a spurious counterexample around. */
    {
        bool ok = !vc->reflect_fuel_exhausted;
        for (uint32_t i = 0; ok && i < vc->n_ufuncs; i++)
            ok = vc->ufuncs[i].is_ctor || vc->ufuncs[i].reflected;
        vc->reflect_model_ok = ok;
    }
    return vc;
}
