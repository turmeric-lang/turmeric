/* elab_reflect.c -- reflected measures: the `^reflect` totality gate.
 *
 * docs/upcoming/reflected-measures-plan.md, phases RF0 (the site table),
 * RF1 (purity + structural termination) and RF2 (coverage), behind
 * `--enable=reflected-measures`.
 *
 * WHY THIS EXISTS.  The refinement encoder treats every named measure as an
 * uninterpreted function: `len` is a symbol congruence closure can compare to
 * itself and nothing more.  RF3 (refine_collect.c) lets a `^reflect` function
 * MEAN something by unfolding its body at constructor-headed arguments and
 * asserting `f(t) = <body at t>`.  That equation is only a fact when `f` is
 * total: unfolding `f(x) = 1 + f(x)` asserts `0 = 1`, and one inconsistent
 * hypothesis discharges every obligation in the unit -- silently.  So a
 * function's equation is admitted ONLY after this file has shown it
 *
 *   1. pure        -- the same default-deny walk that grants congruence
 *                     (rt_binding_is_pure); an impure measure's occurrences
 *                     are fresh symbols anyway, so reflecting it is void;
 *   2. terminating -- every self-call passes, in ONE FIXED argument position,
 *                     a strict structural subterm of the corresponding
 *                     parameter (a variable bound by a constructor pattern in
 *                     a `match` on that parameter, or a subterm of such);
 *                     mutual recursion, arithmetic on the argument, and calls
 *                     through variables all reject;
 *   3. covered     -- every `match` it contains is proven exhaustive.  ADT and
 *                     union scrutinees already are (compiling implies it:
 *                     elab_match's TUR-E0301 / covered[] bitmap); the walk
 *                     rejects the two holes -- a `^non-exhaustive` opt-out,
 *                     and a literal-scrutinee match with no `_`/variable arm
 *                     (the S4-lit path has no coverage check) -- and, default-
 *                     deny, any form it does not positively recognise.
 *
 * "Fixed position" is load-bearing for termination.  A rule that accepted
 * "some position decreases at each call" admits f(x,y) that alternates
 * decreasing x while growing y and decreasing y while growing x -- each call
 * shrinks something and the pair loops forever.  One position that shrinks at
 * EVERY self-call is a well-founded order on that argument alone.
 *
 * This is NOT a termination checker for programs.  A function that is never
 * `^reflect`ed is untouched; the plan's carve-out ("a reflected function must
 * be shown total; program termination stays out of scope") is recorded in the
 * plan's header, and this file enforces exactly that much.
 *
 * Structure follows WF2's write-frame walk: sites are registered during
 * elab_defn, classified by a deferred pass after the unit
 * (rf_resolve_reflect_sites) so that purity of a callee defined LATER in the
 * file is part of the verdict and the verdict never depends on definition
 * order.  An eager attempt at the end of elab_defn stamps TOTAL early when it
 * can, so an obligation decided in place (a return refinement discharged
 * while its own defn is elaborated) can already unfold; rejections are only
 * ever reported by the deferred pass. */

#include "elab_internal.h"
#include "diag.h"
#include "runtime/experiments.h"
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------------- *
 * RF0: the site table
 * ------------------------------------------------------------------------- */

void rf_note_reflect_site(Elab *e, Binding *fn, Binding **params, uint32_t n_params,
                          const Form *defn_form, uint32_t body_start,
                          const Form *annot) {
    if (!e || !fn) return;
    /* A monomorphization clone re-elaborates the SAME defn form under a fresh
     * binding.  One site per form: the original is classified and reported
     * once, and the clone inherits its verdict (below, and again whenever
     * the original's verdict lands, through rf_resolve_reflect_sites) rather
     * than raising a second TUR-E0384 on the same source line. */
    for (uint32_t i = 0; i < e->n_reflect_sites; i++) {
        ReflectSite *s0 = &e->reflect_sites[i];
        if (s0->defn_form != defn_form || s0->fn == fn) continue;
        fn->reflect_body        = s0->fn->reflect_body;
        fn->reflect_param_names = s0->fn->reflect_param_names;
        fn->reflect_n_params    = s0->fn->reflect_n_params;
        fn->reflect_total       = s0->fn->reflect_total;
        return;
    }
    if (e->n_reflect_sites == e->cap_reflect_sites) {
        uint32_t ncap = e->cap_reflect_sites ? e->cap_reflect_sites * 2 : 8;
        ReflectSite *nb = (ReflectSite *)arena_alloc(e->arena, ncap * sizeof(ReflectSite));
        if (e->reflect_sites)
            memcpy(nb, e->reflect_sites, e->n_reflect_sites * sizeof(ReflectSite));
        e->reflect_sites     = nb;
        e->cap_reflect_sites = ncap;
    }
    ReflectSite *s = &e->reflect_sites[e->n_reflect_sites++];
    s->fn         = fn;
    s->params     = params;
    s->n_params   = n_params;
    s->defn_form  = defn_form;
    s->body_start = body_start;
    s->annot      = annot;

    /* Publish the body and parameter names on the binding now, so the
     * encoder (through rt_resolve_fn) can reach them whichever pass stamps
     * the verdict.  A multi-form body becomes one `do`, the shape the
     * unfolder's tail-position rule understands. */
    uint32_t nb = defn_form->as.list.len > body_start
                ? defn_form->as.list.len - body_start : 0;
    if (nb == 1) {
        fn->reflect_body = defn_form->as.list.items[body_start];
    } else if (nb > 1) {
        Form **items = (Form **)arena_alloc(e->arena, (nb + 1) * sizeof(Form *));
        items[0] = form_sym(e->arena, defn_form->span, e->sym_do);
        for (uint32_t i = 0; i < nb; i++)
            items[i + 1] = defn_form->as.list.items[body_start + i];
        fn->reflect_body = form_list(e->arena, defn_form->span, items, nb + 1);
    }
    const char **names = (const char **)arena_alloc(
        e->arena, (n_params ? n_params : 1) * sizeof(char *));
    for (uint32_t i = 0; i < n_params; i++)
        names[i] = (params[i] && params[i]->name) ? params[i]->name->name : NULL;
    fn->reflect_param_names = names;
    fn->reflect_n_params    = n_params;
}

/* ------------------------------------------------------------------------- *
 * RF1/RF2: the body walk
 * ------------------------------------------------------------------------- */

/* One lexical entry.  `root` is the parameter index this name descends from
 * (-1: none -- a shadowing binder, or a name bound to something that is not a
 * subterm of any parameter); `strict` says it is a STRICT subterm of that
 * parameter (bound through at least one constructor pattern), which is what a
 * decreasing self-call argument needs.  A parameter's own entry is root=i,
 * strict=false. */
typedef struct RfEntry {
    const Symbol *name;
    int32_t       root;
    bool          strict;
} RfEntry;

#define RF_MAX_ENV   256
#define RF_MAX_DEPTH 64
#define RF_BUDGET    20000

typedef struct RfCtx {
    Elab         *e;
    const ReflectSite *site;
    const Symbol *fn_name;
    RfEntry       env[RF_MAX_ENV];
    uint32_t      n_env;
    uint32_t      depth;
    uint32_t      budget;
    /* Per-position "decreases at every self-call" mask, AND-ed across calls;
     * `saw_self_call` distinguishes "no recursion" from "all bits cleared". */
    uint64_t      dec_mask;
    bool          saw_self_call;
    /* Rejection, when any. */
    const char   *gate;      /* "purity" / "termination" / "coverage" */
    const char   *reason;
    Span          at;
    bool          has_at;
} RfCtx;

static void rf_reject(RfCtx *c, const Form *at, const char *gate, const char *reason) {
    if (c->gate) return;   /* first rejection wins */
    c->gate   = gate;
    c->reason = reason;
    if (at) { c->at = at->span; c->has_at = true; }
}

static const RfEntry *rf_lookup(const RfCtx *c, const Symbol *name) {
    for (uint32_t i = c->n_env; i-- > 0; )
        if (c->env[i].name == name) return &c->env[i];
    return NULL;
}

static bool rf_push(RfCtx *c, const Symbol *name, int32_t root, bool strict) {
    if (c->n_env >= RF_MAX_ENV) return false;
    c->env[c->n_env].name   = name;
    c->env[c->n_env].root   = root;
    c->env[c->n_env].strict = strict;
    c->n_env++;
    return true;
}

static bool rf_sym_is(const Form *f, const char *s) {
    return f && f->tag == F_SYM && f->as.sym && strcmp(f->as.sym->name, s) == 0;
}

static bool rf_is_literal(const Form *f) {
    return f && (f->tag == F_INT || f->tag == F_FLOAT || f->tag == F_BOOL ||
                 f->tag == F_STR || f->tag == F_NIL);
}

/* Heads the refinement predicate language itself understands (refine_collect.c
 * `enc`), so a body built from them unfolds into terms the solver can use.
 * Anything else that computes a value must be a call to a known function or
 * constructor. */
static bool rf_head_is_operator(const Form *h) {
    static const char *const ops[] = {
        "+", "-", "*", "/", "mod", "=", "==", "<", "<=", ">", ">=",
        "not=", "!=", "<>", "and", "or", "not", "=>", "implies", NULL };
    if (!h || h->tag != F_SYM || !h->as.sym) return false;
    for (uint32_t i = 0; ops[i]; i++)
        if (strcmp(h->as.sym->name, ops[i]) == 0) return true;
    return false;
}

/* Heads that make a body partial by construction. */
static bool rf_head_is_partial(const Form *h) {
    static const char *const ps[] = {
        "panic", "panic!", "error", "assert!", "require!", "ensure!",
        "unreachable", "unreachable!", "abort", "exit", NULL };
    if (!h || h->tag != F_SYM || !h->as.sym) return false;
    for (uint32_t i = 0; ps[i]; i++)
        if (strcmp(h->as.sym->name, ps[i]) == 0) return true;
    return false;
}

static bool rf_walk(RfCtx *c, const Form *f);

/* Does any form under `f` mention `target` (as a symbol, in any position)?
 * Bounded; a budget miss answers TRUE so a deep callee is treated as possibly
 * recursive rather than proven not to be. */
static bool rf_form_mentions(const Form *f, const Symbol *target,
                             uint32_t depth, uint32_t *budget) {
    if (!f) return false;
    if (depth > RF_MAX_DEPTH || *budget == 0) return true;
    (*budget)--;
    switch (f->tag) {
    case F_SYM:
        return f->as.sym == target;
    case F_LIST: case F_VEC: case F_MAP: case F_MAP_LITERAL: case F_SET:
        for (uint32_t i = 0; i < f->as.list.len; i++)
            if (rf_form_mentions(f->as.list.items[i], target, depth + 1, budget))
                return true;
        return false;
    default:
        return false;
    }
}

/* The registered defn form of a binding, if elab_defn registered one (it does
 * for every defn, through wf_note_frame_site -- the global-write question
 * needed every body reachable). */
static const WriteFrameSite *rf_defn_site_of(const Elab *e, const Binding *b) {
    for (uint32_t i = 0; i < e->n_wf_frame_sites; i++)
        if (e->wf_frame_sites[i].fn == b) return &e->wf_frame_sites[i];
    return NULL;
}

/* Mutual recursion: can a call to `callee` reach `target` again?  Transitive
 * over registered defn bodies, bounded, and default-YES on anything it cannot
 * see (no body in the registry: an extern, an inline-C defn) -- unless the
 * callee is known pure through the purity walk's own evidence, in which case
 * an unwalkable body is still not a self-reference.  Being generous here
 * costs one declined reflection; being lax costs an inconsistent axiom. */
static bool rf_reaches(const Elab *e, const Binding *callee, const Binding *target,
                       const Binding **stack, uint32_t depth, uint32_t *budget) {
    if (!callee) return true;
    if (callee == target) return true;
    for (uint32_t i = 0; i < depth; i++)
        if (stack[i] == callee) return false;   /* a cycle not through target */
    if (depth >= RF_MAX_DEPTH || *budget == 0) return true;
    const WriteFrameSite *ws = rf_defn_site_of(e, callee);
    if (!ws || !ws->defn_form) {
        /* No forms to look at.  A binding with no defn body cannot call back
         * into user code through a name it never spells, and every path by
         * which an inline-C body could re-enter user code is one the purity
         * walk already refuses (inline C is UNKNOWN there).  So: not a
         * self-reference. */
        return false;
    }
    const Form *d = ws->defn_form;
    /* Direct mention of the target anywhere in the callee's body. */
    for (uint32_t bi = ws->body_start; bi < d->as.list.len; bi++)
        if (rf_form_mentions(d->as.list.items[bi], target->name, 0, budget))
            return true;
    /* Transitively, through every symbol in head position that resolves to a
     * function.  Walking every symbol (not just heads) would also follow
     * data; heads are what a call needs. */
    const Binding *st2[RF_MAX_DEPTH];
    for (uint32_t i = 0; i < depth; i++) st2[i] = stack[i];
    st2[depth] = callee;
    /* Iterative worklist over the body forms, looking at list heads. */
    const Form *work[512]; uint32_t nw = 0;
    for (uint32_t bi = ws->body_start; bi < d->as.list.len && nw < 512; bi++)
        work[nw++] = d->as.list.items[bi];
    while (nw > 0) {
        const Form *f = work[--nw];
        if (!f || *budget == 0) { if (*budget == 0) return true; continue; }
        (*budget)--;
        if (f->tag != F_LIST && f->tag != F_VEC) continue;
        if (f->tag == F_LIST && f->as.list.len > 0) {
            const Form *h = f->as.list.items[0];
            if (h->tag == F_SYM && h->as.sym) {
                Binding *hb = scope_lookup(&((Elab *)e)->global, h->as.sym);
                if (hb && hb != callee && hb->type.kind == TY_FN &&
                    !elab_lookup_ctor((Elab *)e, h->as.sym) &&
                    rf_reaches(e, hb, target, st2, depth + 1, budget))
                    return true;
            }
        }
        for (uint32_t i = 0; i < f->as.list.len && nw < 512; i++)
            work[nw++] = f->as.list.items[i];
        if (nw >= 512) return true;   /* overflow: cannot see everything */
    }
    return false;
}

/* Bind the variables of one pattern.  `root`/`strict_in` describe the value
 * being matched; sub-patterns of a constructor pattern are STRICT subterms of
 * it.  Returns false (with a rejection recorded) on a pattern the walk does
 * not understand. */
static bool rf_bind_pattern(RfCtx *c, const Form *pat, int32_t root, bool strict_in,
                            bool *is_literal, bool *is_default) {
    if (!pat) { rf_reject(c, NULL, "coverage", "empty pattern"); return false; }
    if (rf_is_literal(pat)) { *is_literal = true; return true; }
    if (pat->tag == F_SYM) {
        if (rf_sym_is(pat, "_")) { *is_default = true; return true; }
        *is_default = true;   /* a bare variable matches anything */
        return rf_push(c, pat->as.sym, root, strict_in);
    }
    if (pat->tag == F_LIST && pat->as.list.len > 0) {
        const Form *h = pat->as.list.items[0];
        if (h->tag != F_SYM || !h->as.sym) {
            rf_reject(c, pat, "coverage", "pattern head is not a constructor name");
            return false;
        }
        if (!elab_lookup_ctor(c->e, h->as.sym)) {
            /* `(x : T)` union patterns and record-style `{...}` shapes are
             * outside the first cut's fragment. */
            rf_reject(c, pat, "coverage", "pattern is not a constructor pattern");
            return false;
        }
        for (uint32_t i = 1; i < pat->as.list.len; i++) {
            const Form *sp = pat->as.list.items[i];
            bool lit = false, def = false;
            if (rf_is_literal(sp)) continue;
            if (sp->tag == F_SYM) {
                if (rf_sym_is(sp, "_")) continue;
                if (!rf_push(c, sp->as.sym, root, /*strict=*/root >= 0)) {
                    rf_reject(c, sp, "coverage", "too many bindings in scope");
                    return false;
                }
                continue;
            }
            if (!rf_bind_pattern(c, sp, root, /*strict_in=*/root >= 0, &lit, &def))
                return false;
        }
        return true;
    }
    rf_reject(c, pat, "coverage", "pattern shape is not recognised");
    return false;
}

static bool rf_walk_match(RfCtx *c, const Form *f) {
    if (f->as.list.len < 2) { rf_reject(c, f, "coverage", "match without a scrutinee"); return false; }
    const Form *scrut = f->as.list.items[1];
    if (scrut->tag == F_MAP ||
        (scrut->tag == F_SYM && scrut->as.sym == c->e->sym_caret_non_exhaustive)) {
        /* `^non-exhaustive` (or the deprecated `#fx{NonExhaustive}`, or any
         * marker): the opt-out is an unchecked promise, and a reflected axiom
         * cannot rest on one. */
        rf_reject(c, scrut, "coverage",
                  "a `^non-exhaustive` match is not proven exhaustive");
        return false;
    }
    if (!rf_walk(c, scrut)) return false;
    int32_t root = -1; bool strict = false;
    if (scrut->tag == F_SYM) {
        const RfEntry *en = rf_lookup(c, scrut->as.sym);
        if (en) { root = en->root; strict = en->strict; }
    }
    bool is_literal = false, has_default = false;
    uint32_t n_arms = 0;
    uint32_t idx = 2;
    while (idx < f->as.list.len) {
        const Form *pat = f->as.list.items[idx++];
        const Form *guard = NULL;
        if (idx + 1 < f->as.list.len && f->as.list.items[idx]->tag == F_SYM &&
            f->as.list.items[idx]->as.sym == c->e->sym_when) {
            guard = f->as.list.items[idx + 1];
            idx += 2;
        }
        if (idx >= f->as.list.len) {
            rf_reject(c, f, "coverage", "match arm without a body");
            return false;
        }
        const Form *body = f->as.list.items[idx++];
        uint32_t saved = c->n_env;
        bool lit = false, def = false;
        if (!rf_bind_pattern(c, pat, root, strict, &lit, &def)) return false;
        if (lit) is_literal = true;
        /* A guarded arm can fail, so it never supplies the default. */
        if (def && !guard) has_default = true;
        if (guard && !rf_walk(c, guard)) return false;
        if (!rf_walk(c, body)) return false;
        c->n_env = saved;
        n_arms++;
    }
    if (n_arms == 0) { rf_reject(c, f, "coverage", "match with no arms"); return false; }
    if (is_literal && !has_default) {
        rf_reject(c, f, "coverage",
                  "a match over literals needs a trailing `_` or variable arm "
                  "(literal matches are not coverage-checked)");
        return false;
    }
    return true;
}

static bool rf_walk_let(RfCtx *c, const Form *f) {
    if (f->as.list.len < 3 || f->as.list.items[1]->tag != F_VEC) {
        rf_reject(c, f, "coverage", "let shape is not recognised");
        return false;
    }
    const Form *bv = f->as.list.items[1];
    uint32_t saved = c->n_env;
    uint32_t i = 0;
    while (i < bv->as.list.len) {
        const Form *name = bv->as.list.items[i++];
        if (name->tag != F_SYM || !name->as.sym) {
            rf_reject(c, name, "coverage", "let binder is not a plain name");
            return false;
        }
        /* Optional `: T` annotation. */
        if (i + 1 < bv->as.list.len && bv->as.list.items[i]->tag == F_SYM &&
            bv->as.list.items[i]->as.sym == c->e->sym_colon)
            i += 2;
        if (i >= bv->as.list.len) {
            rf_reject(c, name, "coverage", "let binder without an initializer");
            return false;
        }
        const Form *init = bv->as.list.items[i++];
        if (!rf_walk(c, init)) return false;
        /* An alias of a subterm is that subterm; anything else is a fresh
         * value that descends from no parameter. */
        int32_t root = -1; bool strict = false;
        if (init->tag == F_SYM) {
            const RfEntry *en = rf_lookup(c, init->as.sym);
            if (en) { root = en->root; strict = en->strict; }
        }
        if (!rf_push(c, name->as.sym, root, strict)) {
            rf_reject(c, name, "coverage", "too many bindings in scope");
            return false;
        }
    }
    for (uint32_t bi = 2; bi < f->as.list.len; bi++)
        if (!rf_walk(c, f->as.list.items[bi])) return false;
    c->n_env = saved;
    return true;
}

static bool rf_walk(RfCtx *c, const Form *f) {
    if (c->gate) return false;
    if (!f) { rf_reject(c, NULL, "coverage", "empty form"); return false; }
    if (c->depth > RF_MAX_DEPTH || c->budget == 0) {
        rf_reject(c, f, "coverage", "body too large or too deeply nested to check");
        return false;
    }
    c->budget--;
    bool ok = true;
    c->depth++;
    switch (f->tag) {
    case F_INT: case F_FLOAT: case F_BOOL: case F_STR: case F_NIL:
        break;
    case F_KEYWORD:
        break;
    case F_SYM:
        if (f->as.sym == c->fn_name && !rf_lookup(c, f->as.sym)) {
            rf_reject(c, f, "termination",
                      "the function is referenced as a value, not called");
            ok = false;
        }
        break;
    case F_LIST: {
        if (f->as.list.len == 0) { rf_reject(c, f, "coverage", "empty list"); ok = false; break; }
        const Form *h = f->as.list.items[0];
        if (h->tag != F_SYM || !h->as.sym) {
            rf_reject(c, h, "coverage", "call head is not a name");
            ok = false; break;
        }
        const Symbol *hs = h->as.sym;
        if (hs == c->e->sym_if) {
            if (f->as.list.len != 4) {
                rf_reject(c, f, "coverage", "`if` without an else branch is not total");
                ok = false; break;
            }
            for (uint32_t i = 1; i < 4 && ok; i++) ok = rf_walk(c, f->as.list.items[i]);
            break;
        }
        if (hs == c->e->sym_do) {
            for (uint32_t i = 1; i < f->as.list.len && ok; i++) ok = rf_walk(c, f->as.list.items[i]);
            break;
        }
        if (hs == c->e->sym_let)   { ok = rf_walk_let(c, f);   break; }
        if (hs == c->e->sym_match) { ok = rf_walk_match(c, f); break; }
        if (rf_head_is_partial(h)) {
            rf_reject(c, f, "coverage", "the body can fail to return a value here");
            ok = false; break;
        }
        if (rf_lookup(c, hs)) {
            rf_reject(c, f, "termination", "a call through a variable cannot be reflected");
            ok = false; break;
        }
        if (hs == c->fn_name) {
            /* A self-call.  Arity must match the declaration exactly (a
             * partial application returns a closure, which is a value the
             * logic has no term for), and some FIXED position must carry a
             * strict subterm of its parameter at EVERY self-call. */
            uint32_t argc = f->as.list.len - 1;
            if (argc != c->site->n_params) {
                rf_reject(c, f, "termination", "self-call does not supply every parameter");
                ok = false; break;
            }
            uint64_t here = 0;
            for (uint32_t i = 0; i < argc && i < 64; i++) {
                const Form *a = f->as.list.items[i + 1];
                if (a->tag != F_SYM) continue;
                const RfEntry *en = rf_lookup(c, a->as.sym);
                if (en && en->strict && en->root == (int32_t)i) here |= (1ull << i);
            }
            c->dec_mask &= here;
            c->saw_self_call = true;
            if (c->dec_mask == 0) {
                rf_reject(c, f, "termination",
                          "no argument position is a strict structural subterm of "
                          "its parameter at every self-call");
                ok = false; break;
            }
            for (uint32_t i = 1; i < f->as.list.len && ok; i++) ok = rf_walk(c, f->as.list.items[i]);
            break;
        }
        if (rf_head_is_operator(h)) {
            for (uint32_t i = 1; i < f->as.list.len && ok; i++) ok = rf_walk(c, f->as.list.items[i]);
            break;
        }
        if (elab_lookup_ctor(c->e, hs)) {
            for (uint32_t i = 1; i < f->as.list.len && ok; i++) ok = rf_walk(c, f->as.list.items[i]);
            break;
        }
        Binding *cb = scope_lookup(&c->e->global, hs);
        if (cb && cb->type.kind == TY_FN) {
            /* Another function.  Purity is the fn-level gate's business (the
             * purity walk is transitive); here only mutual recursion matters:
             * a callee that can reach this function again makes the
             * definition non-structural, and the fixpoint that could admit it
             * is deliberately out of scope (plan, settled question 3). */
            const Binding *stack[RF_MAX_DEPTH];
            uint32_t budget = RF_BUDGET;
            if (rf_reaches(c->e, cb, c->site->fn, stack, 0, &budget)) {
                rf_reject(c, f, "termination",
                          "mutual recursion: the callee can reach this function again");
                ok = false; break;
            }
            for (uint32_t i = 1; i < f->as.list.len && ok; i++) ok = rf_walk(c, f->as.list.items[i]);
            break;
        }
        /* A macro, a special form outside the fragment (`fn`, `while`,
         * `set!`, `cond`, `when`, ...), a builtin the predicate language does
         * not have, or a name that does not resolve.  Default-deny: a missed
         * case costs one declined reflection, never a wrong axiom. */
        rf_reject(c, f, "coverage", "form is outside the reflectable fragment");
        ok = false;
        break;
    }
    default:
        rf_reject(c, f, "coverage", "form is outside the reflectable fragment");
        ok = false;
        break;
    }
    c->depth--;
    return ok;
}

/* Classify one site.  Returns true for TOTAL; on false `*gate`/`*reason`/
 * `*at` describe the first failure.  `*dec_pos` is the decreasing position
 * (or -1 for a non-recursive body). */
static bool rf_classify(Elab *e, const ReflectSite *s, const char **gate,
                        const char **reason, Span *at, bool *has_at, int32_t *dec_pos) {
    *gate = NULL; *reason = NULL; *has_at = false; *dec_pos = -1;
    Binding *fn = s->fn;
    if (!fn || !fn->name) { *gate = "coverage"; *reason = "unnamed function"; return false; }

    /* Gate 1: purity, by the congruence walk.  UNKNOWN reads as impure: a
     * body the walk cannot model (inline C, a callee with no body yet) is not
     * evidence of anything, and an axiom needs evidence. */
    if (!rt_binding_is_pure(fn)) {
        *gate = "purity";
        *reason = "the body is not proven pure (a `^reflect` measure clears the same "
                  "default-deny walk that grants congruence: literals, if/let/do/match, "
                  "arithmetic, and calls to functions already known pure)";
        return false;
    }
    if (!fn->reflect_body) { *gate = "coverage"; *reason = "the function has no body"; return false; }

    /* Gates 2 and 3: one walk over the body forms. */
    RfCtx c; memset(&c, 0, sizeof(c));
    c.e = e; c.site = s; c.fn_name = fn->name;
    c.budget = RF_BUDGET;
    c.dec_mask = ~0ull;
    for (uint32_t i = 0; i < s->n_params; i++) {
        if (!s->params[i] || !s->params[i]->name) continue;
        rf_push(&c, s->params[i]->name, (int32_t)i, false);
    }
    if (s->n_params == 0) c.dec_mask = 0;   /* a nullary self-call can never decrease */
    bool ok = rf_walk(&c, fn->reflect_body);
    if (!ok) {
        *gate = c.gate ? c.gate : "coverage";
        *reason = c.reason ? c.reason : "form is outside the reflectable fragment";
        *at = c.at; *has_at = c.has_at;
        return false;
    }
    if (c.saw_self_call) {
        for (uint32_t i = 0; i < 64; i++)
            if (c.dec_mask & (1ull << i)) { *dec_pos = (int32_t)i; break; }
    }
    return true;
}

void rf_stamp_reflect_site_eager(Elab *e, Binding *fn) {
    if (!e || !fn || fn->reflect_total) return;
    for (uint32_t i = 0; i < e->n_reflect_sites; i++) {
        const ReflectSite *s = &e->reflect_sites[i];
        if (s->fn != fn) continue;
        const char *gate, *reason; Span at; bool has_at; int32_t dec;
        if (rf_classify(e, s, &gate, &reason, &at, &has_at, &dec)) fn->reflect_total = 1;
        return;
    }
}

void rf_resolve_reflect_sites(Elab *e) {
    if (!e || e->n_reflect_sites == 0) return;
    for (uint32_t i = 0; i < e->n_reflect_sites; i++) {
        const ReflectSite *s = &e->reflect_sites[i];
        if (!s->fn) continue;
        const char *gate = NULL, *reason = NULL; Span at; bool has_at = false; int32_t dec = -1;
        bool total = s->fn->reflect_total == 1 ||
                     rf_classify(e, s, &gate, &reason, &at, &has_at, &dec);
        if (total) {
            if (s->fn->reflect_total != 1) s->fn->reflect_total = 1;
            else if (g_dump_reflect) {
                /* Eagerly stamped: recompute for the dump's decreasing column. */
                const char *g2, *r2; Span a2; bool h2;
                rf_classify(e, s, &g2, &r2, &a2, &h2, &dec);
            }
        } else {
            s->fn->reflect_total = 2;
            Span where = has_at ? at : (s->annot ? s->annot->span : s->defn_form->span);
            diag_emit_with_code(DIAG_ERROR, where, TUR_E0384_REFLECT_NOT_TOTAL,
                                "'%s' cannot be reflected: it fails the %s gate -- %s",
                                s->fn->name ? s->fn->name->name : "<fn>", gate, reason);
            if (s->annot)
                diag_emit(DIAG_NOTE, s->annot->span,
                          "`^reflect` asks the solver to admit this function's "
                          "defining equation, which is only sound for a total "
                          "function; remove the annotation or restructure the body");
        }
        if (g_dump_reflect) {
            if (total) {
                if (dec >= 0)
                    printf("reflect %s: TOTAL dec=%d\n",
                           s->fn->name ? s->fn->name->name : "<fn>", dec);
                else
                    printf("reflect %s: TOTAL dec=none\n",
                           s->fn->name ? s->fn->name->name : "<fn>");
            } else {
                printf("reflect %s: REJECTED gate=%s reason=%s\n",
                       s->fn->name ? s->fn->name->name : "<fn>", gate, reason);
            }
            /* The compiled program's output follows on the same stdout; flush
             * so the dump lines land before it, not at the compiler's exit. */
            fflush(stdout);
        }
    }
}
