#ifndef TUR_REFINE_SOLVER_H
#define TUR_REFINE_SOLVER_H

/* refine_solver.h -- shared declarations for the in-house staged decision
 * procedure (S0..S3).  These stages ARE the solver, and always were the only
 * one a user ran: the Z3 backend was dev-only scaffolding, and was retired
 * outright in 0.32.5.
 *
 * Every stage implements the RefineBackend seam from refine_vc.h and obeys the
 * one-directional soundness invariant: never RT_VALID unless genuinely
 * entailed; RT_UNKNOWN is always safe.  A stage returns RT_UNKNOWN for
 * anything outside its competence and the next stage picks it up.
 *
 *   S0  normalize + trivial discharge      refine_solver_s0.c
 *   S1  congruence closure (EUF)           refine_solver_euf.c
 *   S2  linear arithmetic (Fourier-Motzkin) refine_solver_arith.c
 *   S3  Nelson-Oppen combination of the two refine_solver_no.c
 *
 * Common shape: to prove `hyps |= goal` we refute `hyps AND (not goal)`.  That
 * formula is put in NNF and expanded to a small DNF -- a set of CUBES, each a
 * conjunction of literals.  The goal is valid iff EVERY cube is unsatisfiable.
 * Cube expansion is capped; blowing the cap yields RT_UNKNOWN (this is the
 * "naive S4" the plan permits -- we never build a DPLL(T) engine).
 *
 * See docs/archive/refinement-types-plan.md (S0--S4). */

#include "refine_vc.h"

/* Caps.  Every one of these, when hit, produces RT_UNKNOWN -> runtime check. */
#define REFINE_MAX_CUBES        64
#define REFINE_MAX_CUBE_LITS    64
/* Recursion backstop for the DNF product expansion.  Each frame either splits
 * one disjunction or flattens one conjunction, both of which strictly reduce
 * the formula's remaining boolean structure -- so this should never be reached
 * by a well-formed input.  It exists because the alternative to a cap on a
 * recursive walk is a stack overflow, and a crashed compiler is worse than a
 * kept runtime check. */
#define REFINE_MAX_EXPAND_DEPTH 256
#define REFINE_MAX_LA_VARS      32
#define REFINE_MAX_LA_CONSTR    512
#define REFINE_MAX_EUF_TERMS    512
/* S3's two caps live here rather than in refine_solver_no.c so the telemetry
 * below can report a peak against the limit it is a peak of. */
#define NO_MAX_ROUNDS           4
/* Raised 8 -> 16 on SX0(b)'s evidence (solver-extension-plan SX5).  It was the
 * ONLY cap in the solver with a live signal: it turned away eligible terms on
 * four units across the three swept populations, every time by exactly one
 * (9 eligible against a cap of 8), while every other cap sat on 72-98%
 * headroom.  Not free -- the exchange is quadratic in the shared set and each
 * `la_entails_eq` runs Fourier-Motzkin twice, so the pair work per S3 cube is
 * 4x at the new cap and is paid by every obligation reaching S3, not only the
 * ones near the cap.  Measured before landing: no verdict moved on the 125
 * corpus benchmarks or the 89 in-tree refinement fixtures, and the corpus
 * replay did not slow measurably.  Numbers and method in
 * docs/archive/history/no-max-shared-raise.md. */
#define NO_MAX_SHARED           16

/* The bounded counterexample search's scope.  Here rather than beside its own
 * code for the telemetry reason the NO_MAX_* ones are: the reporter names the
 * limit its peak is a peak of.
 *
 * This is the cap most likely to SURPRISE, because it does not cost a proof --
 * it costs a REFUTATION.  The proving stages only ever answer Valid or
 * Unknown, so a counterexample can come from nowhere else; a VC with more
 * variables than this can therefore never be reported as definitely wrong,
 * however obviously wrong it is.  Raising it costs enumeration exponentially
 * (n_cand ** n_vars), which is why it wants a measurement rather than a
 * guess -- see model_vars_hits below for what that measurement has to
 * separate.
 *
 * Raised 3 -> 8 on 2026-09-05 and paired with MODEL_MAX_EVALS, which is the
 * cap that actually binds now.  At 3 the search declined EVERY four-parameter
 * function with a refined return -- an ordinary shape, and one none of the
 * three swept populations happened to contain (the fuzzer generates at most
 * two parameters), which is why the sweep read "never bites".  A structural
 * width limit was the wrong shape for the cost it guards: `n_cand ** n_vars`
 * is what the odometer pays, and a five-variable VC over five candidates
 * (3125 evaluations) is cheaper than a three-variable one over sixteen
 * (4096).  So the width cap is now only a backstop for the array sizes, and
 * the evaluation budget below decides.  See
 * docs/archive/solver-integer-tail-plan.md. */
#define MODEL_MAX_VARS          8
/* Ceiling on `n_cand ** n_vars`, the number of full evaluations one search
 * may run.  131072 tiny evaluations is a few milliseconds; the old cap's
 * worst case (16 ** 3) was 4096.  Declines are counted in model_evals_hits. */
#define MODEL_MAX_EVALS         131072u

/* A COLLECTION cap, not a solver one, and it lives here for the same reason
 * the two NO_MAX_* ones do: the telemetry below reports a peak against the
 * limit it is a peak of, and the reporter cannot see elab_fns.c's internals.
 *
 * It bounds the PATH CONDITIONS recovered for a call-site crossing -- the
 * branch guards that had to hold to reach the call.  It is the tightest cap
 * in the whole refinement path, and until 2026-09-04 it was also the only one
 * with no telemetry, so nobody could say whether it bit.  Dropping a guard is
 * sound in the one direction that matters (fewer hypotheses make the goal
 * HARDER to prove, never easier), so a hit costs a proof, not a wrong answer.
 * Enforced in rt_collect_path_conds (elab_fns.c). */
#define RT_CS_PATH_MAX_HYPS     8

/* ------------------------------------------------------------------------- *
 * Cap telemetry
 * ------------------------------------------------------------------------- */

/* Every cap above degrades to RT_UNKNOWN when it bites, which is sound but
 * silent: from the outside, an obligation the solver capped out on and one
 * that was never in its competence look exactly alike.  These counters make
 * the difference visible under TUR_REFINE_STATS=1.
 *
 * They are also decision data.  The solver-extension plan gates its two
 * largest phases on this measurement -- SX4 (incremental simplex) is worth
 * building only if REFINE_MAX_LA_CONSTR actually bites, and SX6 (boolean
 * structure beyond small DNF) only if the cube caps do.  See
 * docs/archive/solver-extension-plan.md, SX0(b).
 *
 * `hits` counts the times a cap fired.  `peak` is the high-water mark of the
 * quantity that cap bounds, recorded on every query rather than only on the
 * ones that overflow -- so a cap that never fires still reports how much
 * headroom was left, which is the difference between "we are nowhere near
 * this" and "we are one wide function away from it". */
typedef struct RefineCapStats {
    uint32_t cubes_hits,        cubes_peak;
    uint32_t cube_lits_hits,    cube_lits_peak;
    uint32_t expand_depth_hits, expand_depth_peak;
    uint32_t la_vars_hits,      la_vars_peak;
    uint32_t la_constr_hits,    la_constr_peak;
    uint32_t la_fm_hits;        /* FM elimination backed off before growing   */
    uint32_t euf_terms_hits,    euf_terms_peak;
    uint32_t no_shared_hits,    no_shared_peak;
    uint32_t no_rounds_hits;    /* exchange still progressing when rounds ran out */
    /* Collection side (RT_CS_PATH_MAX_HYPS).  `peak` SATURATES at the limit --
     * the walk stops collecting when it fills, so there is no way to know how
     * many guards a crossing would have produced.  So peak is a headroom
     * reading only while hits is 0; once it bites, `hits` is the signal and
     * peak just reads 8.  Counting past the cap would mean restructuring a
     * recursive walk with early returns, which risks changing WHICH guards get
     * collected -- a verdict change for a measurement, which is the wrong
     * trade. */
    uint32_t path_hyps_hits,    path_hyps_peak;
    /* The counterexample search's variable cap.  Unlike path_hyps, the peak
     * here is REAL rather than saturating: `n_vars` is known before the check,
     * so a declined VC still reports how wide it actually was.  It is recorded
     * only for VCs that got past the uninterpreted-symbol gate, since the
     * others could not use the search at any cap.
     *
     * Two hit counters, and the second is the one that decides anything.
     * `model_vars_hits` counts every decline at the cap; `model_vars_would_run`
     * counts the subset that would ACTUALLY start searching at a higher cap --
     * a VC over the cap may also carry a non-int variable, and the sort gate
     * sits after the count gate, so raising the limit would buy those nothing.
     * A raise is justified by the second number, never the first. */
    uint32_t model_vars_hits,   model_vars_peak;
    uint32_t model_vars_would_run;
    /* The evaluation budget (MODEL_MAX_EVALS): a VC inside the width cap and
     * past the sort gate whose `n_cand ** n_vars` would exceed the budget.
     * Every one of these WOULD run at a higher budget, so this row needs no
     * `would_run` twin. */
    uint32_t model_evals_hits;
    /* solver-integer-tail-plan, the two open phases' TRIGGERS.  Neither is a
     * cap; both are counts of the shapes a phase would decide, so the plan's
     * "build it when the population shows up" has a number to read.
     *   eq_nounit_split        Phase 2: an all-integer equation with no unit
     *                          coefficient reached eq_eliminate and was split
     *                          into two inequalities (exactly as complete as
     *                          before Phase 1) rather than substituted away.
     *                          Pugh's sigma-substitution would eliminate it.
     *   la_int_relax_feasible  Phase 3(a): S2 declined an obligation on a cube
     *                          whose constraints are all-integer, involve a
     *                          constraint over two or more variables, and whose
     *                          RATIONAL relaxation Fourier-Motzkin found
     *                          feasible.  That is the population the dark
     *                          shadow / branch-and-bound would look at; it is an
     *                          UPPER bound on what they could decide (most such
     *                          sets are integer-feasible too). */
    uint32_t eq_nounit_split;
    uint32_t la_int_relax_feasible;
    /* The same two, at OBLIGATION level: how many obligations were left
     * UNKNOWN by the whole chain (model search included) after carrying the
     * shape.  The raw counts above are per backend call and are inflated by
     * every stage, probe and hint that re-decides the same VC (a three-
     * obligation probe read 14 and 18); these are what the plan's triggers
     * mean by "an obligation shows up".  Tallied in refine_discharge_one
     * from the per-obligation delta. */
    uint32_t eq_nounit_unknown_obl;
    uint32_t la_int_unknown_obl;
} RefineCapStats;

/* Mutable on purpose: the stages bump their own counters in place, which is
 * cheaper and clearer than threading a stats pointer through every seam. */
RefineCapStats *refine_caps(void);
void refine_caps_reset(void);

static inline void refine_cap_peak(uint32_t *slot, uint32_t v) {
    if (v > *slot) *slot = v;
}

/* `out = now - before`, per field, for attributing a window of solver work to
 * whatever asked for it.  The counters are global (a per-compile summary), so
 * a delta around one decision is the only way to say which obligation hit a
 * cap.
 *
 * The peaks are NOT differenced: they are maxima, not sums, so a difference of
 * two high-water marks is meaningless.  `out` records the peak the window SAW,
 * which is what "how close did this come to the cap" asks. */
static inline void refine_caps_delta(RefineCapStats *out,
                                     const RefineCapStats *now,
                                     const RefineCapStats *before) {
    out->cubes_hits        = now->cubes_hits        - before->cubes_hits;
    out->cube_lits_hits    = now->cube_lits_hits    - before->cube_lits_hits;
    out->expand_depth_hits = now->expand_depth_hits - before->expand_depth_hits;
    out->la_vars_hits      = now->la_vars_hits      - before->la_vars_hits;
    out->la_constr_hits    = now->la_constr_hits    - before->la_constr_hits;
    out->la_fm_hits        = now->la_fm_hits        - before->la_fm_hits;
    out->euf_terms_hits    = now->euf_terms_hits    - before->euf_terms_hits;
    out->no_shared_hits    = now->no_shared_hits    - before->no_shared_hits;
    out->no_rounds_hits    = now->no_rounds_hits    - before->no_rounds_hits;
    out->path_hyps_hits    = now->path_hyps_hits    - before->path_hyps_hits;
    out->path_hyps_peak    = now->path_hyps_peak;
    out->model_vars_hits      = now->model_vars_hits      - before->model_vars_hits;
    out->model_vars_would_run = now->model_vars_would_run - before->model_vars_would_run;
    out->model_evals_hits     = now->model_evals_hits     - before->model_evals_hits;
    out->eq_nounit_split       = now->eq_nounit_split       - before->eq_nounit_split;
    out->la_int_relax_feasible = now->la_int_relax_feasible - before->la_int_relax_feasible;
    out->eq_nounit_unknown_obl = now->eq_nounit_unknown_obl - before->eq_nounit_unknown_obl;
    out->la_int_unknown_obl    = now->la_int_unknown_obl    - before->la_int_unknown_obl;
    out->model_vars_peak      = now->model_vars_peak;
    out->cubes_peak        = now->cubes_peak;
    out->cube_lits_peak    = now->cube_lits_peak;
    out->expand_depth_peak = now->expand_depth_peak;
    out->la_vars_peak      = now->la_vars_peak;
    out->la_constr_peak    = now->la_constr_peak;
    out->euf_terms_peak    = now->euf_terms_peak;
    out->no_shared_peak    = now->no_shared_peak;
}

/* `a += b`, hits only.  Used to accumulate several probes' deltas into the one
 * site they were run for. */
static inline void refine_caps_add_hits(RefineCapStats *a,
                                        const RefineCapStats *b) {
    a->cubes_hits        += b->cubes_hits;
    a->cube_lits_hits    += b->cube_lits_hits;
    a->expand_depth_hits += b->expand_depth_hits;
    a->la_vars_hits      += b->la_vars_hits;
    a->la_constr_hits    += b->la_constr_hits;
    a->la_fm_hits        += b->la_fm_hits;
    a->euf_terms_hits    += b->euf_terms_hits;
    a->no_shared_hits    += b->no_shared_hits;
    a->no_rounds_hits    += b->no_rounds_hits;
    a->path_hyps_hits    += b->path_hyps_hits;
    a->model_vars_hits      += b->model_vars_hits;
    a->model_vars_would_run += b->model_vars_would_run;
    a->model_evals_hits     += b->model_evals_hits;
    refine_cap_peak(&a->path_hyps_peak,    b->path_hyps_peak);
    refine_cap_peak(&a->model_vars_peak,   b->model_vars_peak);
    refine_cap_peak(&a->cubes_peak,        b->cubes_peak);
    refine_cap_peak(&a->cube_lits_peak,    b->cube_lits_peak);
    refine_cap_peak(&a->expand_depth_peak, b->expand_depth_peak);
    refine_cap_peak(&a->la_vars_peak,      b->la_vars_peak);
    refine_cap_peak(&a->la_constr_peak,    b->la_constr_peak);
    refine_cap_peak(&a->euf_terms_peak,    b->euf_terms_peak);
    refine_cap_peak(&a->no_shared_peak,    b->no_shared_peak);
}
/* True when any cap fired since the last reset -- lets the printer stay quiet
 * on the overwhelmingly common "nothing was capped" compile. */
bool refine_caps_any(void);

/* ------------------------------------------------------------------------- *
 * Cubes
 * ------------------------------------------------------------------------- */

/* A literal is a VCTerm that is either an atom or (not atom); use
 * refine_lit_atom / refine_lit_is_neg to take it apart. */
typedef struct VCCube {
    VCTerm **lits;
    uint32_t n;
} VCCube;

typedef struct VCCubeSet {
    VCCube  *cubes;
    uint32_t n;
    bool     overflow;  /* cap hit -- caller must answer RT_UNKNOWN */
    bool     trivial;   /* the refutation formula folded to `false` outright */
} VCCubeSet;

static inline bool refine_lit_is_neg(const VCTerm *l) { return l && l->op == VC_NOT; }
static inline VCTerm *refine_lit_atom(VCTerm *l) {
    return refine_lit_is_neg(l) ? l->kids[0] : l;
}

/* float-proofs-assume-exact-reals: the stages decide over exact rationals,
 * and a `float` is a C double.  Two places that matters, each closed by one
 * predicate:
 *
 *   ROUNDING.  `+ - * /` and negation of a real-sorted term round in double,
 *   so `(x + 0.1) - 0.1 = x` is a theorem over the rationals and false for
 *   x = 0.3.  The LA encoder treats such a node as an opaque variable
 *   (`refine_real_arith`) instead of interpreting it: comparisons, bounds and
 *   transitivity between real terms and literals are exact in double, and are
 *   all S2 still decides for reals.  EUF still sees the node as an
 *   application, which is sound -- double arithmetic is a function.
 *
 *   NaN.  `(not (< x y))` is `y <= x` only when neither side is NaN, and
 *   `(= x x)` is false for NaN.  A real term is NaN-free in a cube when it is
 *   a literal or an argument of a POSITIVE comparison literal of that cube
 *   (which cannot hold for a NaN).  A negated comparison or a disequality is
 *   used only when its real-sorted arguments are NaN-free
 *   (`refine_cube_nan_free`); otherwise it is skipped, which for a refuter
 *   only loses information.  `clamp` written with two `if`s is NOT proved
 *   in range: for a NaN argument it returns NaN, and the runtime check is
 *   what says so.
 *
 * Not covered: congruence through a real-sorted equality (`a = b |- f(a) =
 * f(b)`), which double breaks only at signed zero (0.0 = -0.0, and 1/0.0 is
 * not 1/-0.0). */
bool refine_real_arith(const VCTerm *t);
bool refine_cube_nan_free(const VCCube *c, const VCTerm *t);

/* Build the DNF cube set of `hyps AND (not goal)`.  Returns false (and sets
 * out->overflow) when a cap is hit. */
bool refine_cubes_build(RefineVC *vc, Arena *a, VCCubeSet *out);

/* refine-chain-expands-the-same-dnf-four-times: one chain run's cube set.
 *
 * S0 -> S1 -> S2 -> S3 each opened by calling `refine_cubes_build` on an
 * UNCHANGED vc, so an obligation reaching S3 ran the same NNF conversion and
 * the same DNF product expansion four times and threw three of them away --
 * 75% of the cube expansion in this solver was duplicate work (measured: 48
 * builds across the heaviest in-tree refinement fixture, 4 per obligation on
 * the corpus benchmarks that reach S3).
 *
 * This is a LOCAL with the lifetime of one chain run, not a cache on the
 * RefineVC.  That distinction is the whole safety argument: SX8b's `(pop)`
 * truncates `vc->n_hyps` directly, so a cache keyed on (n_hyps, goal) can be
 * stale after pop + re-assert re-grows the count to the same value with
 * different hypotheses, and invalidating it correctly would need a generation
 * counter -- a soundness-critical invariant for a noise-level win.  A chain-run
 * local needs no invalidation because the vc cannot change under it.
 *
 * LAZY on purpose.  S0 decides several shapes syntactically (`hyp_contains`,
 * ex falso) BEFORE it needs cubes, and those obligations build none today.
 * Building eagerly in the driver would have made them pay for one, trading a
 * duplicate-work win for a new cost on the cheapest path.  Building on first
 * ask keeps 0 builds at 0 and turns 4 into 1. */
typedef struct RefineCubeCache {
    bool     tried;   /* refine_cubes_build has run for this chain */
    bool     ok;      /* ... and returned true */
    VCCubeSet cs;
} RefineCubeCache;

/* Cube set for this chain run, built on the first ask.  Returns false exactly
 * when `refine_cubes_build` would have (a cap hit), and does so for every later
 * ask in the same run without re-expanding. */
bool refine_cubes_get(RefineVC *vc, Arena *a, RefineCubeCache *cc,
                      const VCCubeSet **out);

/* ------------------------------------------------------------------------- *
 * S1: congruence closure (EUF)
 * ------------------------------------------------------------------------- */

typedef struct EufState EufState;

EufState *euf_new(RefineVC *vc, Arena *a);
/* SX3: mark / undo-to-mark over one state (incremental EUF), plus the env
 * seam selecting it.  See trail_c.h for the trail; TUR_REFINE_EUF=rebuild
 * restores the per-cube rebuild path. */
typedef struct {
    uint32_t trail_len;    /* TrailCMark, flattened to keep trail_c.h private */
    uint32_t trail_level;
    uint32_t n;
    uint32_t n_shared;
    bool     unsat;
} EufMark;
EufMark  euf_mark(EufState *st);
void     euf_undo_to(EufState *st, EufMark m);
bool     euf_incremental_mode(void);
/* Assert every literal of `c`.  Returns false when a contradiction is found. */
bool      euf_assert_cube(EufState *st, const VCCube *c);
/* Assert `a == b` and re-close.  Returns false on contradiction. */
bool      euf_assert_eq(EufState *st, VCTerm *a, VCTerm *b);
bool      euf_equal(EufState *st, VCTerm *a, VCTerm *b);
/* Terms EUF knows about, for the S3 equality exchange. */
uint32_t  euf_term_count(const EufState *st);
VCTerm   *euf_term_at(const EufState *st, uint32_t i);
/* The subset of those terms both theories see (la_is_shared_term, non-Bool),
 * in registration order; maintained at registration, truncated by undo. */
uint32_t  euf_shared_count(const EufState *st);
VCTerm   *euf_shared_at(const EufState *st, uint32_t i);

/* ------------------------------------------------------------------------- *
 * S2: linear arithmetic (Fourier-Motzkin over the rationals)
 * ------------------------------------------------------------------------- */

typedef struct LaState LaState;

LaState *la_new(RefineVC *vc, Arena *a);
/* Encode every arithmetic literal of `c`.  Non-arithmetic literals are
 * ignored (that only loses information, never soundness).  Returns false when
 * the encoding gives up (too many variables/constraints). */
bool     la_assert_cube(LaState *st, const VCCube *c);
bool     la_assert_le(LaState *st, VCTerm *lhs, VCTerm *rhs, bool strict);
bool     la_assert_eq(LaState *st, VCTerm *a, VCTerm *b);
/* True when the asserted constraint set is provably unsatisfiable. */
bool     la_unsat(LaState *st);
/* True when the constraints entail `a == b` (used by the S3 exchange). */
bool     la_entails_eq(LaState *st, VCTerm *a, VCTerm *b);
/* solver-integer-tail-plan Phase 3(a) trigger: every asserted variable is
 * int-sorted and some constraint spans two or more of them. */
bool     la_set_is_int_multivar(const LaState *st);
/* True when `t` is a term the LA encoder treats as an opaque variable
 * (a VC_VAR or an uninterpreted application) -- i.e. a shared term. */
bool     la_is_shared_term(const VCTerm *t);

/* ------------------------------------------------------------------------- *
 * The stages
 * ------------------------------------------------------------------------- */

/* Bounded counterexample search.  Runs AFTER the proving chain has come back
 * Unknown: enumerate a small candidate assignment space and EVALUATE
 * `hyps AND (not goal)` exactly.  A satisfying assignment is a genuine
 * counterexample, so this is the only thing in the solver that may answer
 * RT_INVALID -- and it does so with a model, never a guess.  Returns NULL when
 * it finds nothing or the VC is outside its (deliberately tiny) scope. */
RefineModel *refine_model_search(RefineVC *vc, Arena *a);

RefineDecision refine_s0_decide(RefineVC *vc, Arena *a);
RefineDecision refine_s1_decide(RefineVC *vc, Arena *a);
RefineDecision refine_s2_decide(RefineVC *vc, Arena *a);
RefineDecision refine_s3_decide(RefineVC *vc, Arena *a);

/* The same stages sharing one chain run's cube set.  The four entry points
 * above are thin wrappers over these with a fresh cache, so `tur smt`,
 * tests/unit/refine_solver.c and the SX8a/SX8b doors call exactly what they
 * called before. */
RefineDecision refine_s0_decide_cc(RefineVC *vc, Arena *a, RefineCubeCache *cc);
RefineDecision refine_s1_decide_cc(RefineVC *vc, Arena *a, RefineCubeCache *cc);
RefineDecision refine_s2_decide_cc(RefineVC *vc, Arena *a, RefineCubeCache *cc);
RefineDecision refine_s3_decide_cc(RefineVC *vc, Arena *a, RefineCubeCache *cc);

#endif /* TUR_REFINE_SOLVER_H */
