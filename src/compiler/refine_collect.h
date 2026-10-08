#ifndef TUR_REFINE_COLLECT_H
#define TUR_REFINE_COLLECT_H

/* refine_collect.h -- RT1: the constraint collector.
 *
 * Each point where a value crosses INTO a `#refine{ x : T | p }` type is a
 * proof obligation: prove `p[subject/x]` under the hypotheses in scope.  This
 * module owns the obligation record, the hypothesis environment, and the
 * Form -> normalized-VC encoder that RT2 consumes.
 *
 * Crossing points collected today:
 *   - a `defn` whose RETURN type is a contract type (subject = the body form)
 *   - a `defn` `:post` predicate            (subject = the body form, var `result`)
 *   - a CALL-SITE argument crossing into a contract-typed parameter (subject =
 *     the argument form; the callee's other parameter names are substituted by
 *     the arguments in their slots, so a predicate mentioning a sibling
 *     parameter is checked against real values)
 *
 * Hypotheses come from:
 *   - each parameter declared with a contract type (`v` renamed to the param)
 *   - the function's `:pre` predicate
 *
 * See docs/archive/refinement-types-plan.md (phase RT1). */

#include <stdbool.h>
#include <stdint.h>

#include "forms.h"
#include "refine_solver.h"   /* RefineCapStats, for the per-obligation caps */
#include "refine_vc.h"
#include "runtime/arena.h"

/* ------------------------------------------------------------------------- *
 * Hypothesis environment
 * ------------------------------------------------------------------------- */

/* One known-true fact.  `pred` is the predicate as written; `bound_var` is the
 * `v` of `#refine{ v : T | p }` and `subject_name` is the in-scope name it
 * stands for, so the encoder renames without rewriting the Form tree.  A
 * `:pre` predicate has `bound_var == NULL` (nothing to rename). */
typedef struct RefineHyp {
    const Form       *pred;
    const char       *bound_var;
    const char       *subject_name;
    struct RefineHyp *next;
} RefineHyp;

/* What the encoder needs to know about a CALLED function to make use of its
 * result.  A call inside a predicate or an argument is encoded as an opaque
 * (uninterpreted) application either way -- but when the callee declares a
 * return refinement, that refinement is a fact about the value the call
 * produces, and asserting it turns `(twice (double-pos p))` from unknown into
 * provable. */
typedef struct RefineFnInfo {
    const Form  *ret_pred;     /* the q of `: #refine{ r : T | q }`, or NULL */
    const char  *ret_var;      /* the r */
    const char **param_names;  /* so q may mention the callee's parameters */
    uint32_t     n_params;
    /* Whether two occurrences of this call may be treated as THE SAME VALUE.
     *
     * Encoding a call as an uninterpreted function makes its occurrences
     * congruent -- `f(a)` equals `f(a)` -- which is the whole point for a
     * measure like `len`, and which is FALSE for anything effectful.  With it
     * assumed, `(- (tick) (tick))` where `tick` counts up encodes as `t - t`,
     * the solver proves `t - t >= 0`, and the runtime check that would have
     * caught the real value of -1 is elided.  That is a miscompile, and it is
     * the one thing turning the experiment on must never do.
     *
     * So congruence is opt-in: only a callee that is KNOWN pure gets it. */
    bool         pure;
    /* RM-B1: the VC sort the callee's RETURN TYPE denotes.
     *
     * A measure used to be declared VS_INT unconditionally, which made a
     * `bool`-returning function unusable as a predicate atom (the goal came
     * back Int-sorted and `refine_vc_build` rejected it) and -- worse -- made a
     * `float`-returning one MIS-sorted: `(< (norm v) 4.0)` declared `norm` as
     * Int, and S2's integer tightening turned `norm(v) < 4` into
     * `norm(v) <= 3`, "proving" a goal that is false at norm(v) = 3.5 and
     * eliding the runtime check that would have caught it.
     *
     * VS_INT is the zero value, so a resolver that never touches this field
     * keeps the old behaviour. */
    VCSort       ret_sort;
    /* C2 / #reads: bitmask of the ^borrow parameters this measure reads the
     * mutable state of (`#reads w` / `#reads [w g]`), bit i == param i, or 0
     * for none.  When EVERY such argument at a call site is a value FROZEN in
     * scope (a live borrow -- the `(& w)` a `frozen` region holds), the measure
     * is a function of frozen state and its pure arguments, so the encoder may
     * treat it as congruent even though its body is impure.
     *
     * The quantifier is the load-bearing part: one unfrozen named parameter is
     * enough for two occurrences to denote different values, so the grant is
     * conjunctive over the mask, never disjunctive.  Sound only because the
     * callee's own entry check is never elided; see
     * docs/guides/stateful-refinements-guide.md. */
    uint64_t     reads_params_mask;
    /* R2 + R4 slice 1 (trusted-refinement-claims-plan): positive evidence
     * the `#reads` frame above is broken (the body directly reads a mutable
     * global, or mutable state rooted in a parameter the frame omits).
     * Mirrors Binding.reads_frame_omits_state.  Refuses the congruence grant;
     * false means "no evidence", never "verified clean". */
    bool         reads_frame_omits_state;
    /* Mirrors Binding.reads_write_unverified: the measure's body could not be
     * shown to only READ its framed parameters.  Refuses the loop-invariant
     * frozen grant (li_name_reads_only). */
    bool         reads_write_unverified;
    /* WF1/WF2 / #writes: this callee's declared write frame, mirroring the
     * Binding fields of the same names.  `writes_declared` distinguishes "the
     * frame is empty" from "there is no frame" -- see expr.h.  WF3 uses these
     * to decide whether a CALL in a caller body can stale a hypothesis; a frame
     * that is not `writes_checked` is a promise, so it may not back that
     * decision (a trusted frame can still document intent, but eliding on it
     * would be trusting an unverified claim). */
    uint32_t     writes_param_mask;
    bool         writes_declared;
    bool         writes_checked;
    /* reflected-measures (docs/upcoming/reflected-measures-plan.md):
     * is_ctor -- the name is a DATA CONSTRUCTOR, which is what makes an
     *   argument form "ground" for the unfolder: a `match` arm can be
     *   selected syntactically against `(Cons 1 (Nil))` and against nothing
     *   else.
     * reflect_total -- the callee is `^reflect` AND passed the totality gate
     *   (elab_reflect.c), so its defining equation may be admitted.  Never
     *   set for a rejected or ungated function: the encoder unfolds on this
     *   bit alone, and everything RF1/RF2 proves is what makes that sound.
     * reflect_body / reflect_param_names / reflect_n_params -- the body as
     *   ONE Form and the parameter names it is written in. */
    bool         is_ctor;
    /* RF4: the constructor's shape, so the unfolder can select a `match` arm
     * from a TAG FACT (`(= (#dt/tag s) k)`, what a caller's own match arm
     * knows) and bind the arm's variables to field SELECTORS (`(.tl s)`) when
     * the scrutinee is not a constructor term.  Only a record constructor has
     * named fields to select; a positional one binds nothing and declines. */
    uint32_t     ctor_tag;
    bool         ctor_is_record;
    const char **ctor_field_names;
    uint32_t     ctor_n_fields;
    bool         reflect_total;
    const Form  *reflect_body;
    const char **reflect_param_names;
    uint32_t     reflect_n_params;
} RefineFnInfo;

/* Resolve a called name.  Returns false when the name does not resolve to a
 * function at all -- an abstract measure, which the language treats as an
 * uninterpreted (and therefore congruent) mathematical function.  Supplied by
 * the elaborator, which owns the scope; NULL disables both result propagation
 * and purity discrimination, so every call falls back to the safe
 * fresh-per-occurrence encoding. */
typedef bool (*RefineFnResolver)(void *ud, const char *name, RefineFnInfo *out);

typedef struct RefineEnv {
    Arena       *arena;
    RefineHyp   *head;
    /* Declared sorts for in-scope names (from parameter types).  A name with
     * no entry defaults to VS_INT. */
    const char **names;
    VCSort      *sorts;
    uint32_t     n_names, cap_names;
    /* Return-refinement lookup for calls appearing in encoded expressions. */
    RefineFnResolver resolve_fn;
    void            *resolve_ud;
} RefineEnv;

RefineEnv *refine_env_new(Arena *a);
void refine_env_declare(RefineEnv *env, const char *name, VCSort sort);
void refine_env_set_resolver(RefineEnv *env, RefineFnResolver fn, void *ud);
void refine_env_push(RefineEnv *env, const Form *pred,
                     const char *bound_var, const char *subject_name);
VCSort refine_env_sort_of(const RefineEnv *env, const char *name);

/* ------------------------------------------------------------------------- *
 * Obligation record
 * ------------------------------------------------------------------------- */

/* An extra name -> expression substitution applied while encoding a goal.
 * Call-site crossings use these to replace the CALLEE's parameter names with
 * the caller's argument expressions, so a predicate that mentions a sibling
 * parameter (`[n : int, i : #refine{ j : int | (< j n) }]`) is checked against
 * the actual arguments rather than against free variables. */
typedef struct RefineSubst {
    const char *name;
    const Form *form;
} RefineSubst;

typedef struct RefineObligation {
    const Form  *predicate;      /* the p in #refine{ x : T | p } */
    const char  *var_name;       /* the x */
    const Form  *subject;        /* the form substituted for x (may be NULL) */
    RefineSubst *subst;          /* applied before var_name/subject */
    uint32_t     n_subst;
    /* C2 / #reads: names borrowed (frozen) in scope at this obligation's site.
     * A `#reads w` measure whose world argument is one of these is congruent
     * here.  Carried from the crossing's borrow snapshot; NULL / 0 otherwise. */
    const char **frozen_names;
    uint32_t     n_frozen;
    /* True when a runtime check ELSEWHERE already guards this obligation.  Set
     * for call-site crossings: the callee validates its own parameters on
     * entry, so an argument we merely cannot prove is the normal state of
     * affairs, not news.  Such an obligation reports only when it is
     * DEFINITELY wrong -- a closed goal that evaluates false, `(safe-div 10 0)`
     * -- or when --strict-refine asks for a fully-discharged build.
     *
     * The distinction is who owes the proof.  A function's own return
     * refinement is a claim it makes about itself, so an open counterexample
     * means the claim is false: that is an error.  A call-site crossing is
     * about a value flowing in, and "not proven for every input" is the
     * ordinary condition of code that has not been fully annotated yet.
     * Erroring on it would make `refined` impossible to adopt incrementally. */
    bool         runtime_guarded;
    /* C2 / #reads: this crossing's callee refinement is a `#reads`-measure
     * predicate, which is impure and therefore has NO runtime contract (the
     * entry check is TUR-E0375-unemittable and is suppressed).  So there is no
     * runtime backstop: an unproven such crossing must NOT be silently trusted
     * in non-strict mode, and its diagnostic must not claim a "runtime check
     * kept".  When set, runtime_guarded is false (the crossing is proof-only)
     * and the W0372 text branches to the no-fallback wording. */
    bool         reads_no_runtime;
    /* R2: the `#reads` measure this crossing depends on carries broken-frame
     * evidence, so the congruence grant was REFUSED.  Only refines the W0372
     * wording: "guard
     * it inside a `frozen` region" is misleading advice when the crossing IS
     * frozen and the frame is what failed. */
    bool         reads_grant_refused;
    /* A speculative probe (RT4 template inference): decide it, report nothing,
     * count nothing.  The caller only wants the verdict. */
    bool         speculative;
    /* RT4 path splitting: a speculative probe for ONE PATH of a branching
     * body.  Counted separately from RT4 template probes so the stats line
     * still means what it says -- these are not inferred refinements. */
    bool         path_probe;
    VCSort       base_sort;      /* sort of the refined base type T */
    const char  *base_type_name; /* T, for diagnostics */
    Span         loc;
    RefineEnv   *env;            /* in-scope hypotheses at the crossing */
    const char  *what;           /* "return value", "postcondition", ... */
    const char  *fn_name;

    RefineVC    *vc;             /* RT2: filled by the discharge pass */
    bool         discharged;     /* RT3: a verdict was reached */
    bool         proven;         /* RT3: a backend returned RT_VALID */
    RefineModel *counterex;      /* RT3: model when a backend said RT_INVALID */

    /* SX8a: provenance for the JSON obligation dump.  Recorded always rather
     * than under the flag -- the cost is three stores per obligation, and a
     * dump that can only be produced by re-running the compile with a
     * different flag is not much of an interrogation surface. */
    const char    *decided_by;   /* which stage answered ("S2 (arithmetic)"), or NULL */
    bool           memo_hit;     /* RT7: the chain was skipped, answer remembered */
    RefineCapStats caps;         /* caps this obligation alone hit (SX0(b) deltas) */
    /* Caps hit by the RT4 path-splitting probes run on this site's behalf,
     * BEFORE this obligation existed.
     *
     * `caps` above is a delta around this obligation's own chain run, which is
     * the right window for the question "what did deciding this cost" -- but
     * it is not all the solver work the site paid for. Path splitting tries
     * each path silently first; those probes are separate obligations,
     * discharged earlier, and their cap hits used to land in the global
     * counters and be attributed to nobody: the per-compile summary said a cap
     * bit and every obligation's `caps_hit` read empty.
     *
     * Kept as a second field rather than folded into `caps` because a probe
     * asks a different question (one path, not the whole body) -- a consumer
     * deciding whether a cap is worth raising wants to know which. Summing
     * them is one addition; separating them after the fact is impossible. */
    RefineCapStats caps_probe;
    /* loop-invariants-plan: decide it and RECORD the verdict, but report
     * nothing -- the caller words the headline, because "the loop does not
     * preserve (>= acc 0)" is not a sentence the generic reporter can build.
     * Stats, the RT7 memo and the refutation search run exactly as for a
     * reported obligation; refine_emit_obligation_notes then attaches the
     * same notes an E0371 / W0372 carries. */
    bool         quiet;
    bool         refuted;         /* quiet: a counterexample was found      */
    bool         refuted_closed;  /* quiet: ... and the goal was ground      */
    const char  *unknown_reason;  /* quiet: why it could not be encoded     */
} RefineObligation;

typedef struct RefineObligationVec {
    RefineObligation **obs;
    uint32_t           n, cap;
    Arena             *arena;
} RefineObligationVec;

void refine_obligations_init(RefineObligationVec *v, Arena *a);

/* Record one crossing.  Returns the obligation so a caller that wants an
 * immediate verdict (to elide the runtime check it is about to inject) can
 * hand it straight to refine_discharge_one. */
RefineObligation *refine_collect_obligation(RefineObligationVec *v,
                                            const Form *predicate,
                                            const char *var_name,
                                            const Form *subject,
                                            VCSort base_sort,
                                            const char *base_type_name,
                                            Span loc,
                                            RefineEnv *env,
                                            const char *what,
                                            const char *fn_name);

/* Attach the sibling-parameter substitutions a call-site crossing needs.
 * `n` entries are copied into the obligation's arena. */
void refine_obligation_set_subst(RefineObligation *ob, Arena *a,
                                 const RefineSubst *subst, uint32_t n);

/* C2 / #reads: record the names frozen (borrowed) in scope at this obligation's
 * site.  Shares the array by reference -- the caller owns arena-allocated,
 * immutable storage (the crossing's snapshot). */
void refine_obligation_set_frozen(RefineObligation *ob,
                                  const char **frozen_names, uint32_t n);

/* ------------------------------------------------------------------------- *
 * RT2 lowering: obligation -> normalized VC
 * ------------------------------------------------------------------------- */

/* Build the normalized VC for `ob` (hypotheses from its env, goal from its
 * predicate with `var_name` substituted by `subject`).  Returns NULL when the
 * obligation escapes the supported predicate fragment -- the caller then
 * treats it as RT_UNKNOWN, which is always sound.  `*out_reason` (optional)
 * is set to a short human-readable cause. */
RefineVC *refine_vc_build(RefineObligation *ob, Arena *a, const char **out_reason);

#endif /* TUR_REFINE_COLLECT_H */
