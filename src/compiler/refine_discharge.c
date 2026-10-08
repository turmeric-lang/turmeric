/* refine_discharge.c -- RT3: the discharge pass.
 *
 * Owns the ordered backend array and the fall-through loop.  Backends run in
 * ascending cost and the loop stops at the first non-Unknown verdict:
 *
 *   S0 (normalize/trivial) -> S1 (EUF) -> S2 (arithmetic) -> S3 (N-O combine)
 *      -> RT_UNKNOWN => keep the runtime check
 *
 * The chain is the in-house stages, full stop -- a dev-only Z3 oracle sat at
 * the tail during bootstrap and was retired in 0.32.5 once the in-house stages
 * met the plan's retirement criteria.  An obligation no stage can decide falls
 * straight to its runtime contract check.  Adding a stage only ever moves
 * obligations LEFT in the chain; it never changes an answer, because every
 * stage preserves the one-directional soundness invariant.
 *
 * See docs/archive/refinement-types-plan.md (phase RT3). */

#include "refine_discharge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diag.h"
#include "fmt.h"
#include "refine_smtlib.h"
#include "refine_solver.h"
#include "runtime/globals.h"

/* ------------------------------------------------------------------------- *
 * Stats
 * ------------------------------------------------------------------------- */

static RefineStats g_stats;

const RefineStats *refine_stats(void) { return &g_stats; }
void refine_memo_reset(void);
void refine_discharge_reset(void) {
    memset(&g_stats, 0, sizeof(g_stats));
    refine_caps_reset();
    refine_memo_reset();
}

static bool stats_enabled(void) {
    const char *s = getenv("TUR_REFINE_STATS");
    return s && s[0] == '1';
}

static bool dump_enabled(void) {
    const char *s = getenv("TUR_REFINE_DUMP");
    return s && s[0] == '1';
}

/* ------------------------------------------------------------------------- *
 * The chain
 * ------------------------------------------------------------------------- */

/* Parallel to CHAIN_CC, for the SX8a dump and `tur smt`: "which stage decided
 * this" is the first thing anyone asks of an unexpected verdict. */
static const char *const CHAIN_NAMES[] = {
    "S0 (trivial)", "S1 (EUF)", "S2 (arithmetic)", "S3 (Nelson-Oppen)",
};
/* refine-chain-expands-the-same-dnf-four-times: the chain, sharing ONE run's
 * cube set.  Each stage used to open by calling `refine_cubes_build` on an
 * unchanged vc, so an obligation reaching S3 ran the same NNF conversion and
 * DNF expansion four times and discarded three.  Every driver below now
 * declares one `RefineCubeCache` and threads it through.
 *
 * The single-stage `refine_sN_decide` entry points still exist (they wrap these
 * with a fresh cache), which is what `tur smt`, `src/web/wasm_glue.c` and
 * tests/unit/refine_solver.c call -- unchanged, and they never had the
 * duplication because they run one stage. */
typedef RefineDecision (*RefineBackendCC)(RefineVC *, Arena *, RefineCubeCache *);
static const RefineBackendCC CHAIN_CC[] = {
    refine_s0_decide_cc,   /* trivial / syntactic          */
    refine_s1_decide_cc,   /* congruence closure (EUF)     */
    refine_s2_decide_cc,   /* linear arithmetic            */
    refine_s3_decide_cc,   /* Nelson-Oppen combination     */
};
#define CHAIN_LEN (sizeof(CHAIN_CC) / sizeof(CHAIN_CC[0]))

/* ------------------------------------------------------------------------- *
 * Diagnostics
 * ------------------------------------------------------------------------- */

/* `what` is written fully-formed by the collector ("the return value of
 * 'f'", "argument 2 of 'g'"), so the message needs no connective of its own --
 * which is what keeps "on the argument 2 of 'g'" from happening. */
static void describe(const RefineObligation *ob, char *buf, size_t cap) {
    snprintf(buf, cap, "%s", ob->what ? ob->what : "value");
}

static void emit_model_note(const RefineObligation *ob, bool closed) {
    /* The closed case is handled by emit_predicate_note, which can say it in
     * one line ("... is false for the value given here") instead of two
     * notes that repeat each other.
     *
     * `closed` is passed rather than re-derived from the model's size: a model
     * can bind variables the GOAL never mentions -- the caller's own
     * parameters -- and printing "counterexample: n = -2" for a goal that is
     * false regardless of `n` points at the wrong thing entirely. */
    if (closed) return;
    if (!ob->counterex || ob->counterex->n == 0) return;
    char buf[256]; size_t off = 0;
    for (uint32_t i = 0; i < ob->counterex->n && off + 1 < sizeof(buf); i++) {
        const RefineModelBinding *b = &ob->counterex->bindings[i];
        int k = b->is_real
              ? snprintf(buf + off, sizeof(buf) - off, "%s%s = %g",
                         i ? ", " : "", b->name, b->rval)
              : b->is_bool
              ? snprintf(buf + off, sizeof(buf) - off, "%s%s = %s",
                         i ? ", " : "", b->name, b->ival ? "true" : "false")
              : snprintf(buf + off, sizeof(buf) - off, "%s%s = %lld",
                         i ? ", " : "", b->name, (long long)b->ival);
        if (k < 0) break;
        off += (size_t)k;
    }
    diag_emit(DIAG_NOTE, ob->loc, "counterexample: %s", buf);
}

/* ------------------------------------------------------------------------- *
 * RT6: hint generation.
 *
 * "Cannot be proved" is only half an answer.  The other half -- which extra
 * fact would have discharged it -- is a second query through the same seam:
 * add a candidate hypothesis and ask the chain again.  A candidate that works
 * is a real answer, because it was checked, not guessed.
 * ------------------------------------------------------------------------- */

#define HINT_MAX_VARS   4
#define HINT_MAX_LITS   4
#define HINT_MAX_TRIES  96

/* Run the whole chain over `vc` as the discharge loop does. */
static RefineVerdict run_chain(RefineVC *vc, Arena *a) {
    RefineCubeCache cc = {0};
    for (size_t i = 0; i < CHAIN_LEN; i++) {
        RefineDecision d = CHAIN_CC[i](vc, a, &cc);
        if (d.verdict != RT_UNKNOWN) return d.verdict;
    }
    return RT_UNKNOWN;
}

/* Collect the integer literals mentioned anywhere in the VC, so a hint can
 * suggest a bound the surrounding code actually talks about (`(>= i 0)`,
 * `(< i n)`) rather than only comparisons against zero. */
static void hint_collect_lits(const VCTerm *t, int64_t *out, uint32_t *n) {
    if (!t || *n >= HINT_MAX_LITS) return;
    if (t->op == VC_CONST_INT) {
        for (uint32_t i = 0; i < *n; i++) if (out[i] == t->as.i) return;
        out[(*n)++] = t->as.i;
        return;
    }
    for (uint32_t i = 0; i < t->n; i++) hint_collect_lits(t->kids[i], out, n);
}

static VCTerm *hint_build(RefineVC *vc, const char *op, VCTerm *x, VCTerm *k) {
    if (strcmp(op, ">")  == 0) return vc_mk2(vc, VC_LT, k, x);
    if (strcmp(op, ">=") == 0) return vc_mk2(vc, VC_LE, k, x);
    if (strcmp(op, "<")  == 0) return vc_mk2(vc, VC_LT, x, k);
    if (strcmp(op, "<=") == 0) return vc_mk2(vc, VC_LE, x, k);
    return vc_not(vc, vc_mk2(vc, VC_EQ, x, k));   /* not= */
}

/* Would asserting `cand` discharge the goal?  Two things have to hold: the
 * goal must become valid, AND the hypotheses must stay satisfiable.  Without
 * the second test a candidate that CONTRADICTS what is already known would
 * "work" by ex falso, and we would helpfully suggest constraining `x` to be
 * both positive and negative. */
static bool hint_discharges(RefineVC *vc, Arena *a, VCTerm *cand) {
    uint32_t saved_hyps = vc->n_hyps;
    VCTerm  *saved_goal = vc->goal;

    vc_add_hyp(vc, cand);
    if (vc->n_hyps == saved_hyps) return false;   /* already known: no new information */

    vc->goal = vc_bool(vc, false);
    bool contradictory = (run_chain(vc, a) == RT_VALID);
    vc->goal = saved_goal;

    bool ok = !contradictory && (run_chain(vc, a) == RT_VALID);
    vc->n_hyps = saved_hyps;
    return ok;
}

/* Search for a strengthening that discharges `vc`.  Returns true and fills
 * `out` with source syntax in the variable's own name (`(> x 0)`) and
 * `out_decl` with the same fact in a refinement's bound variable (`(> v 0)`).
 *
 * Two families of candidate, literal bounds first because they are the common
 * case, then variable-vs-variable -- which is what produces `(< i n)` for an
 * index obligation, the shape a bound literal can never express. */
bool refine_hint_search(RefineVC *vc, Arena *a, char *out, size_t cap,
                        char *out_decl, size_t decl_cap,
                        const char **out_var, bool *out_real) {
    if (!vc || !vc->goal) return false;
    /* An obligation that already holds has nothing to suggest -- every
     * consistent candidate would "discharge" it vacuously.  The discharge
     * pass only calls this on a failure, but the check belongs here so the
     * function is correct for any caller, not just the careful one. */
    if (run_chain(vc, a) == RT_VALID) return false;

    static const char *const OPS[]     = { ">", ">=", "not=", "<", "<=" };
    static const char *const REL_OPS[] = { "<", "<=", ">", ">=" };

    int64_t lits[HINT_MAX_LITS]; uint32_t n_lits = 0;
    for (uint32_t i = 0; i < vc->n_hyps; i++) hint_collect_lits(vc->hyps[i], lits, &n_lits);
    hint_collect_lits(vc->goal, lits, &n_lits);
    /* Zero first: it is the bound most refinements are about. */
    int64_t bounds[HINT_MAX_LITS + 1]; uint32_t n_bounds = 0;
    bounds[n_bounds++] = 0;
    for (uint32_t i = 0; i < n_lits && n_bounds <= HINT_MAX_LITS; i++)
        if (lits[i] != 0) bounds[n_bounds++] = lits[i];

    uint32_t tries = 0;
    uint32_t n_vars = vc->n_vars < HINT_MAX_VARS ? vc->n_vars : HINT_MAX_VARS;

    /* Family 1: x <op> <literal>. */
    for (uint32_t b = 0; b < n_bounds; b++) {
        for (size_t o = 0; o < sizeof(OPS) / sizeof(OPS[0]); o++) {
            for (uint32_t v = 0; v < n_vars; v++) {
                if (tries++ >= HINT_MAX_TRIES) return false;
                bool is_real = vc->vars[v].sort == VS_REAL;
                VCTerm *x = vc_var_ref(vc, v);
                VCTerm *k = is_real ? vc_real(vc, (double)bounds[b])
                                    : vc_int(vc, bounds[b]);
                VCTerm *cand = hint_build(vc, OPS[o], x, k);
                if (!cand || cand->sort != VS_BOOL) continue;
                if (!hint_discharges(vc, a, cand)) continue;
                if (is_real) {
                    snprintf(out, cap, "(%s %s %.1f)", OPS[o], vc->vars[v].name,
                             (double)bounds[b]);
                    snprintf(out_decl, decl_cap, "(%s v %.1f)", OPS[o], (double)bounds[b]);
                } else {
                    snprintf(out, cap, "(%s %s %lld)", OPS[o], vc->vars[v].name,
                             (long long)bounds[b]);
                    snprintf(out_decl, decl_cap, "(%s v %lld)", OPS[o],
                             (long long)bounds[b]);
                }
                *out_var  = vc->vars[v].name;
                *out_real = is_real;
                return true;
            }
        }
    }

    /* Family 2: x <op> y.  An index bound (`(< i n)`) is only expressible
     * here -- no literal can stand in for another variable. */
    for (size_t o = 0; o < sizeof(REL_OPS) / sizeof(REL_OPS[0]); o++) {
        for (uint32_t vi = 0; vi < n_vars; vi++) {
            for (uint32_t vj = 0; vj < n_vars; vj++) {
                if (vi == vj) continue;
                if (vc->vars[vi].sort != vc->vars[vj].sort) continue;
                if (tries++ >= HINT_MAX_TRIES) return false;
                VCTerm *cand = hint_build(vc, REL_OPS[o], vc_var_ref(vc, vi),
                                          vc_var_ref(vc, vj));
                if (!cand || cand->sort != VS_BOOL) continue;
                if (!hint_discharges(vc, a, cand)) continue;
                snprintf(out, cap, "(%s %s %s)", REL_OPS[o],
                         vc->vars[vi].name, vc->vars[vj].name);
                snprintf(out_decl, decl_cap, "(%s v %s)", REL_OPS[o],
                         vc->vars[vj].name);
                *out_var  = vc->vars[vi].name;
                *out_real = vc->vars[vi].sort == VS_REAL;
                return true;
            }
        }
    }
    return false;
}

/* Render a source Form the way the user wrote it. */
static void render_form(const Form *f, char *buf, size_t cap) {
    buf[0] = '\0';
    if (!f) return;
    Buf fb; buf_init(&fb);
    FmtOptions fo; memset(&fo, 0, sizeof(fo));
    fo.indent_width = 2; fo.line_width = 200;
    Form *one = (Form *)f;
    if (fmt_print(&fb, &one, 1, fo) == 0 && fb.data && fb.len) {
        size_t n = fb.len < cap - 1 ? fb.len : cap - 1;
        while (n && (fb.data[n - 1] == '\n' || fb.data[n - 1] == ' ')) n--;
        memcpy(buf, fb.data, n);
        buf[n] = '\0';
    }
    buf_free(&fb);
}

/* What the predicate was.  Emitted before the counterexample so the diagnostic
 * reads as claim -> witness -> remedy.  A CLOSED refutation gets the stronger
 * phrasing: the values are written right there, so this is not "not for every
 * input", it is "not for this one". */
/* True when `t` mentions no variable, so it denotes the same value under every
 * assignment.  Uninterpreted applications count as non-ground even though a
 * model implies there are none (`refine_model_search` declines a VC carrying
 * one) -- being asked about a term the caller has not screened is exactly when
 * the conservative answer matters.
 *
 * The depth cap answers "not ground" on exhaustion, which is the pre-existing
 * behaviour: it can only withhold an error, never invent one. */
#define VC_GROUND_MAX_DEPTH 64
static bool vc_term_is_ground(const VCTerm *t, uint32_t depth) {
    if (!t) return true;
    if (depth >= VC_GROUND_MAX_DEPTH) return false;
    if (t->op == VC_VAR || t->op == VC_APP) return false;
    for (uint32_t i = 0; i < t->n; i++)
        if (!vc_term_is_ground(t->kids[i], depth + 1)) return false;
    return true;
}

/* What the verdict says about the predicate.  Only a REFUTATION is evidence
 * against the user's code -- a model exists and the predicate fails on it.  An
 * unknown is the solver declining to decide, so its note must not read as a
 * claim that the code is wrong (reflect-two-provable-facts-report-as-not-
 * holding: two provable facts were reported as "does not hold"). */
typedef enum { PN_UNKNOWN, PN_REFUTED, PN_REFUTED_CLOSED } PredNote;

static void emit_predicate_note(const RefineObligation *ob, PredNote kind) {
    char pred[192];
    render_form(ob->predicate, pred, sizeof(pred));
    if (!pred[0]) return;
    diag_emit(DIAG_NOTE, ob->loc,
              kind == PN_REFUTED_CLOSED ? "the predicate %s is false for the value given here"
            : kind == PN_REFUTED        ? "the predicate %s does not hold for every input here"
                                        : "the predicate %s could not be proved here, "
                                          "which is not evidence that it fails",
              pred);
}

/* ------------------------------------------------------------------------- *
 * RT7: within-unit memo.
 *
 * Deciding an obligation is by far the most expensive thing this feature does,
 * and the cost is concentrated in the ones that DO NOT discharge: a provable
 * goal exits at the first stage that proves it, while an unprovable one runs
 * every stage over every cube before answering unknown (~245 ms each with a
 * measure in scope, measured; ~5 ms for a provable one).  Real code repeats
 * predicates -- `Nat` on forty functions is forty identical questions -- so
 * asking each distinct question once is where the win is.
 *
 * The key is `refine_vc_fingerprint`, which canonicalises variable and
 * uninterpreted-symbol names, because obligations that differ only in what
 * their parameter is called are the same question.
 *
 * A HIT IS CONFIRMED BY refine_vc_equal() BEFORE THE VERDICT IS REUSED.  A
 * 64-bit collision is unlikely, but "unlikely" is the wrong standard when the
 * consequence is reusing VALID for a different obligation and eliding a check
 * that was protecting something.  Confirming costs one structural compare on
 * a hit and removes the risk entirely.  (Worth noting for the persistent
 * half of RT7: an on-disk cache has no second VC to compare against, so it
 * cannot do this and must carry a much wider digest.)
 *
 * The memo is per-process and holds only decided, non-speculative
 * obligations.  It is cleared with the stats between units. */

#define REFINE_MEMO_CAP 2048   /* power of two */

typedef struct {
    uint64_t        fp;       /* 0 == empty slot */
    const RefineVC *vc;
    bool            proven;
    /* The RT6 hint for this VC.  Searching for one is the single most
     * expensive thing in the whole feature -- `hint_discharges` runs the full
     * chain TWICE per candidate, over every literal and variable pair -- and
     * it is decided entirely by the VC, so it belongs behind the same key.
     * Memoizing the chain alone moved 600 obligations from 21.9s to 17.6s;
     * memoizing the hint search is what collects the rest. */
    bool            hint_done;
    bool            hint_found;
    char            hint_cand[128];
    char            hint_decl[128];
    char            hint_var[64];
    bool            hint_is_real;
} RefineMemoSlot;

static RefineMemoSlot g_memo[REFINE_MEMO_CAP];
static uint32_t       g_memo_len;

void refine_note_split_proven(void) {
    g_stats.collected++;
    g_stats.proven++;
    g_stats.proven_by_path++;
}

void refine_memo_reset(void) {
    memset(g_memo, 0, sizeof(g_memo));
    g_memo_len = 0;
}

/* Returns true on a CONFIRMED hit, writing the remembered verdict. */
static bool memo_lookup(const RefineVC *vc, uint64_t fp, bool *out_proven) {
    if (!fp) return false;
    uint32_t i = (uint32_t)(fp & (REFINE_MEMO_CAP - 1));
    for (uint32_t probe = 0; probe < REFINE_MEMO_CAP; probe++) {
        RefineMemoSlot *s = &g_memo[(i + probe) & (REFINE_MEMO_CAP - 1)];
        if (!s->fp) return false;
        if (s->fp == fp && refine_vc_equal(s->vc, vc)) {
            *out_proven = s->proven;
            g_stats.memo_hits++;
            return true;
        }
    }
    return false;
}

/* Find the slot for this VC, or NULL.  Used by the hint path, which wants to
 * read AND write the cached hint on the same entry. */
static RefineMemoSlot *memo_slot(const RefineVC *vc, uint64_t fp) {
    if (!fp) return NULL;
    uint32_t i = (uint32_t)(fp & (REFINE_MEMO_CAP - 1));
    for (uint32_t probe = 0; probe < REFINE_MEMO_CAP; probe++) {
        RefineMemoSlot *s = &g_memo[(i + probe) & (REFINE_MEMO_CAP - 1)];
        if (!s->fp) return NULL;
        if (s->fp == fp && refine_vc_equal(s->vc, vc)) return s;
    }
    return NULL;
}

static void memo_insert(const RefineVC *vc, uint64_t fp, bool proven) {
    if (!fp || g_memo_len >= REFINE_MEMO_CAP / 2) return;   /* keep it sparse */
    uint32_t i = (uint32_t)(fp & (REFINE_MEMO_CAP - 1));
    for (uint32_t probe = 0; probe < REFINE_MEMO_CAP; probe++) {
        RefineMemoSlot *s = &g_memo[(i + probe) & (REFINE_MEMO_CAP - 1)];
        if (!s->fp) { s->fp = fp; s->vc = vc; s->proven = proven; g_memo_len++; return; }
        if (s->fp == fp && refine_vc_equal(s->vc, vc)) return;   /* already there */
    }
}

/* What would fix it. */
static void emit_hint(const RefineObligation *ob, RefineVC *vc, Arena *a) {
    char cand[128], decl[128];
    const char *var = NULL;
    bool is_real = false;

    /* Reuse the remembered hint when this exact question was already asked.
     * The hint is a property of the VC, not of the site, so the text is the
     * same -- it is just emitted at each site's own location. */
    RefineMemoSlot *slot = vc ? memo_slot(vc, refine_vc_fingerprint(vc)) : NULL;
    if (slot && slot->hint_done) {
        if (!slot->hint_found) return;
        diag_emit(DIAG_HELP, ob->loc,
                  "%s would discharge it -- e.g. declare %s : #refine{ v : %s | %s }",
                  slot->hint_cand, slot->hint_var,
                  slot->hint_is_real ? "float" : "int", slot->hint_decl);
        return;
    }

    bool found = vc && refine_hint_search(vc, a, cand, sizeof(cand),
                                          decl, sizeof(decl), &var, &is_real);
    if (slot) {
        slot->hint_done  = true;
        slot->hint_found = found;
        if (found) {
            snprintf(slot->hint_cand, sizeof(slot->hint_cand), "%s", cand);
            snprintf(slot->hint_decl, sizeof(slot->hint_decl), "%s", decl);
            snprintf(slot->hint_var,  sizeof(slot->hint_var),  "%s", var ? var : "?");
            slot->hint_is_real = is_real;
        }
    }
    if (!found) return;
    diag_emit(DIAG_HELP, ob->loc,
              "%s would discharge it -- e.g. declare %s : #refine{ v : %s | %s }",
              cand, var, is_real ? "float" : "int", decl);
}

/* ------------------------------------------------------------------------- *
 * Discharge
 * ------------------------------------------------------------------------- */

void refine_note_invariant(bool proven) {
    if (proven) g_stats.inv_proven++;
    else        g_stats.inv_unproven++;
}

void refine_note_invariant_declined(void) { g_stats.inv_declined++; }

void refine_emit_obligation_notes(const RefineObligation *ob, Arena *a,
                                  bool refuted, bool closed) {
    if (!ob) return;
    if (refuted) {
        emit_predicate_note(ob, closed ? PN_REFUTED_CLOSED : PN_REFUTED);
        emit_model_note(ob, closed);
    } else {
        emit_predicate_note(ob, PN_UNKNOWN);
    }
    emit_hint(ob, ob->vc, a);
}

bool refine_discharge_one(RefineObligation *ob, Arena *a) {
    if (!ob) return false;
    if (ob->discharged) return ob->proven;
    ob->discharged = true;
    /* A speculative probe (RT4 template inference) asks a question the user
     * did not; it reports nothing and is counted separately so the summary
     * still describes real obligations. */
    if (ob->speculative) {
        if (ob->path_probe) g_stats.path_probes++;
        else                g_stats.templates_tried++;
        const char *why = NULL;
        RefineVC *pvc = refine_vc_build(ob, a, &why);
        ob->vc = pvc;   /* so a caller can follow up (e.g. ask for a witness) */
        if (!pvc) return false;
        /* reflected-measures: a probe's unfoldings are real work the summary
         * should show -- a per-arm path probe is exactly where RF4's
         * hypothesis-selected arms happen.  No fuel diagnostic here: a probe
         * that runs out simply fails, and the real obligation reports. */
        g_stats.reflect_unfolds     += pvc->reflect_unfolds;
        g_stats.reflect_arms_by_hyp += pvc->reflect_arms_by_hyp;
        /* A probe's VC is where a per-arm proof actually happens, so the dump
         * switch shows it too -- labelled, since it is not an obligation the
         * user wrote. */
        if (dump_enabled()) {
            Buf b; buf_init(&b);
            refine_smtlib_emit(pvc, &b);
            fprintf(stderr, "--- refinement VC (%s probe) ---\n",
                    ob->path_probe ? "path" : "template");
            if (b.data && b.len) fwrite(b.data, 1, b.len, stderr);
            buf_free(&b);
        }
        /* A probe's caps are accounted exactly like a real obligation's.  They
         * used to be counted globally and recorded nowhere, so a cap that bit
         * during a probe made the per-compile summary say "** HIT" while every
         * obligation's `caps_hit` read empty -- the wrong direction to be
         * wrong in for a field that is start/do-not-start evidence for SX6.
         * The caller rolls this up into the site's `caps_probe`. */
        const RefineCapStats probe_before = *refine_caps();
        bool proved = false;
        RefineCubeCache pcc = {0};
        for (size_t i = 0; i < CHAIN_LEN; i++) {
            g_stats.backend_calls++;
            RefineDecision pd = CHAIN_CC[i](pvc, a, &pcc);
            if (pd.verdict == RT_VALID) {
                ob->proven = true;
                if (!ob->path_probe) g_stats.inferred++;
                proved = true;
                break;
            }
            if (pd.verdict != RT_UNKNOWN) break;
        }
        refine_caps_delta(&ob->caps, refine_caps(), &probe_before);
        return proved;
    }
    g_stats.collected++;

    char what[128];
    describe(ob, what, sizeof(what));

    const char *reason = NULL;
    RefineVC *vc = refine_vc_build(ob, a, &reason);
    ob->vc = vc;
    /* reflected-measures RF3: account for the unfolding this VC did.  The
     * fuel note rides the stats switch like RM-B3's encoding reasons -- it is
     * a note about a budget, not a defect in the program -- and becomes a
     * diagnostic (TUR-W0385) only if the obligation then stays unknown. */
    if (vc) {
        g_stats.reflect_unfolds += vc->reflect_unfolds;
        g_stats.reflect_arms_by_hyp += vc->reflect_arms_by_hyp;
        if (vc->reflect_fuel_exhausted) {
            g_stats.reflect_fuel_out++;
            if (stats_enabled())
                fprintf(stderr, "refine: reflect fuel exhausted (%u unfolding(s) "
                                "asserted): %s\n", vc->reflect_unfolds, what);
        }
    }

    /* RT7: has this exact question already been decided in this unit?  Only
     * the VERDICT is reused -- diagnostics still run below, so a repeated
     * unproven obligation reports at its own source location. */
    uint64_t memo_fp = vc ? refine_vc_fingerprint(vc) : 0;
    bool memo_proven = false;
    bool memo_hit = vc && memo_lookup(vc, memo_fp, &memo_proven);

    if (!vc) {
        g_stats.unknown++;
        if (ob->quiet) {
            ob->unknown_reason = reason ? reason : "outside the supported fragment";
            if (stats_enabled())
                fprintf(stderr, "refine: not encoded (%s): %s\n",
                        ob->unknown_reason, what);
            return false;
        }
        /* RM-B3: an obligation that fails to ENCODE and one the solver could
         * not decide both report as `unknown`, and for a runtime-guarded
         * crossing neither reports at all -- so the reason, which is computed
         * either way, reached nobody.  That is what turned a two-line
         * completeness hole into a source read.  Not a diagnostic: an encoding
         * failure is a note about the supported fragment, not a defect in the
         * user's program, so it rides on the stats switch. */
        if (stats_enabled())
            fprintf(stderr, "refine: not encoded (%s): %s\n",
                    reason ? reason : "outside the supported fragment", what);
        if (g_strict_refine || !ob->runtime_guarded)
            diag_emit_with_code(g_strict_refine ? DIAG_ERROR : DIAG_WARNING, ob->loc,
                                TUR_W0372_REFINE_UNKNOWN,
                                "refinement on %s could not be decided statically "
                                "(%s); %s",
                                what, reason ? reason : "outside the supported fragment",
                                ob->reads_grant_refused
                                  ? "no runtime fallback for an impure #reads "
                                    "measure, and its congruence grant was refused: "
                                    "the frame omits mutable state the body reads "
                                    "(TUR-W0383) -- fix the frame, not the region"
                                : ob->reads_no_runtime
                                  ? "no runtime fallback for an impure #reads "
                                    "measure -- the crossing must be proven (guard "
                                    "it inside a `frozen` region)"
                                  : "runtime check kept");
        return false;
    }

    if (vc->has_nonlinear && !ob->runtime_guarded) {
        /* Name the SOURCE subterm, not the internal symbol we abstracted it
         * to -- "(* x y)" is actionable, "__nl_mul_i" is not. */
        char sub[160] = "<nonlinear subterm>";
        for (uint32_t i = 0; i < vc->n_ufuncs; i++) {
            if (!vc->ufuncs[i].nonlinear) continue;
            snprintf(sub, sizeof(sub), "%s", vc->ufuncs[i].name);
            const Form *origin = vc->ufuncs[i].origin;
            if (origin) {
                Buf fb; buf_init(&fb);
                FmtOptions fo; memset(&fo, 0, sizeof(fo));
                fo.indent_width = 2; fo.line_width = 120;
                Form *one = (Form *)origin;
                if (fmt_print(&fb, &one, 1, fo) == 0 && fb.data && fb.len) {
                    size_t n = fb.len < sizeof(sub) - 1 ? fb.len : sizeof(sub) - 1;
                    while (n && (fb.data[n - 1] == '\n' || fb.data[n - 1] == ' ')) n--;
                    memcpy(sub, fb.data, n);
                    sub[n] = '\0';
                }
                buf_free(&fb);
            }
            break;
        }
        diag_emit_with_code(DIAG_WARNING, ob->loc, TUR_W0373_REFINE_NONLINEAR,
                            "non-linear predicate subterm '%s' treated as "
                            "uninterpreted; arithmetic reasoning is incomplete "
                            "for it", sub);
    }

    if (dump_enabled()) {
        Buf b; buf_init(&b);
        refine_smtlib_emit(vc, &b);
        fprintf(stderr, "--- refinement VC (%s) ---\n", what);
        if (b.data && b.len) fwrite(b.data, 1, b.len, stderr);  /* Buf is not NUL-terminated */
        buf_free(&b);
    }

    /* Per-obligation cap accounting: snapshot, run, subtract.  The counters
     * are global (they are a per-compile summary), so the only way to say
     * which obligation hit a cap is to diff around its own decision. */
    const RefineCapStats caps_before = *refine_caps();

    RefineDecision d = refine_unknown();
    ob->memo_hit = memo_hit;
    if (memo_hit) {
        /* The chain is the expensive part and its answer depends only on the
         * VC, so a confirmed hit skips it.  Only PROVED is carried across:
         * `unknown` falls through to the refutation search below, which is
         * cheap, bounded, and re-run per VC so any model it produces names
         * THIS obligation's variables rather than the remembered one's. */
        d = memo_proven ? refine_valid() : refine_unknown();
    } else {
        RefineCubeCache cc = {0};
        for (size_t i = 0; i < CHAIN_LEN; i++) {
            g_stats.backend_calls++;
            d = CHAIN_CC[i](vc, a, &cc);
            if (d.verdict != RT_UNKNOWN) { ob->decided_by = CHAIN_NAMES[i]; break; }
        }
        memo_insert(vc, memo_fp, d.verdict == RT_VALID);
    }

    /* Nothing proved it.  Before settling for "unknown", try to REFUTE it: a
     * concrete satisfying assignment for `hyps AND (not goal)` is a genuine
     * counterexample and turns a vague W0372 into an actionable E0371 with a
     * model.  Finding none leaves the verdict exactly where it was. */
    if (d.verdict == RT_UNKNOWN) {
        RefineModel *m = refine_model_search(vc, a);
        if (m) { d = refine_invalid(m); ob->decided_by = "bounded model search"; }
    }

    /* Subtract per field: a saturating "did anything move" bool would lose the
     * count.  This window covers this obligation's own chain run only; solver
     * work done for it BEFORE it existed -- the RT4 path probes -- is
     * attributed separately, in ob->caps_probe, by the caller that ran them. */
    refine_caps_delta(&ob->caps, refine_caps(), &caps_before);
    /* solver-integer-tail-plan triggers, at obligation level: an obligation
     * the whole chain (search included) left UNKNOWN after S2 saw the shape
     * the phase would decide.  A refuted or proven one is not a trigger --
     * something else decided it. */
    if (d.verdict == RT_UNKNOWN) {
        if (ob->caps.eq_nounit_split)       refine_caps()->eq_nounit_unknown_obl++;
        if (ob->caps.la_int_relax_feasible) refine_caps()->la_int_unknown_obl++;
    }

    switch (d.verdict) {
        case RT_VALID:
            ob->proven = true;
            g_stats.proven++;
            return true;

        case RT_INVALID: {
            /* A CLOSED counterexample -- no free variables, so the goal folded
             * to false on the values written at the site -- is unconditionally
             * wrong and always an error.  An OPEN one only says "not for every
             * input", which for a runtime-guarded obligation is not itself a
             * defect.
             *
             * Closedness is a property of the GOAL, not of the model.  The
             * model binds every variable the VC declared, which includes the
             * caller's own parameters whether or not the goal mentions them --
             * so measuring it by `model->n` made the identical violation
             * reportable in a zero-parameter caller and silent in a
             * one-parameter one:
             *
             *   (defn a []        : int (safe-div 10 0))   ; was reported
             *   (defn b [n : int] : int (safe-div 10 0))   ; was silent
             *
             * A variable-free goal that evaluates false is false for every
             * assignment, so every execution reaching the call violates it.
             * The model still matters: it witnesses that the HYPOTHESES are
             * satisfiable, which is what rules out reporting a path the
             * conditions have already excluded. */
            bool closed = !d.model || d.model->n == 0 ||
                          vc_term_is_ground(vc->goal, 0);
            if (ob->runtime_guarded && !closed && !g_strict_refine) {
                g_stats.unknown++;
                return false;
            }
            ob->counterex = d.model;
            g_stats.invalid++;
            if (ob->quiet) {
                ob->refuted        = true;
                ob->refuted_closed = closed;
                return false;
            }
            diag_emit_with_code(DIAG_ERROR, ob->loc, TUR_E0371_REFINE_NOT_PROVED,
                                "refinement on %s cannot be proved statically", what);
            emit_predicate_note(ob, closed ? PN_REFUTED_CLOSED : PN_REFUTED);
            emit_model_note(ob, closed);
            emit_hint(ob, vc, a);
            return false;
        }

        default:
            g_stats.unknown++;
            if (ob->quiet) return false;
            if (g_strict_refine || !ob->runtime_guarded) {
                /* reflected-measures RF3/RF5: the budget is the likeliest
                 * reason a reflected obligation stays unknown, so say so
                 * beside the generic W0372.  Strict mode promotes it exactly
                 * as it promotes W0372: severity only, never a new report. */
                if (vc->reflect_fuel_exhausted)
                    diag_emit_with_code(g_strict_refine ? DIAG_ERROR : DIAG_WARNING, ob->loc,
                                        TUR_W0385_REFLECT_FUEL_EXHAUSTED,
                                        "unfolding fuel exhausted while encoding the "
                                        "refinement on %s (%u equation(s) asserted); the "
                                        "obligation stays unknown -- raise TUR_REFLECT_FUEL "
                                        "or shorten the argument", what, vc->reflect_unfolds);
                diag_emit_with_code(g_strict_refine ? DIAG_ERROR : DIAG_WARNING, ob->loc,
                                    TUR_W0372_REFINE_UNKNOWN,
                                    "solver returned unknown for the refinement on %s; "
                                    "%s", what,
                                    ob->reads_grant_refused
                                      ? "no runtime fallback for an impure #reads "
                                        "measure, and its congruence grant was refused: "
                                        "the frame omits mutable state the body reads "
                                        "(TUR-W0383) -- fix the frame, not the region"
                                    : ob->reads_no_runtime
                                      ? "no runtime fallback for an impure #reads "
                                        "measure -- the crossing must be proven (guard "
                                        "it inside a `frozen` region)"
                                      : "runtime check kept");
                emit_predicate_note(ob, PN_UNKNOWN);
                emit_hint(ob, vc, a);
            }
            return false;
    }
}

/* Cap telemetry.  Prints one line per cap, with the peak the run actually
 * reached against the limit, and marks the caps that fired.  Every cap here
 * degrades to RT_UNKNOWN, so a hit is not an error -- it is the difference
 * between an obligation the solver declined and one it ran out of room on,
 * which is otherwise invisible from the outside.
 *
 * The peaks print even when nothing fired: headroom is the point.  "cubes
 * peaked at 3 of 64" and "cubes peaked at 61 of 64" are the same zero-hit
 * summary and completely different signals about whether S4 needs building.
 * See docs/archive/solver-extension-plan.md (SX0(b)). */
static void refine_report_caps(void) {
    const RefineCapStats *c = refine_caps();
    struct { const char *name; uint32_t hits, peak, limit; } rows[] = {
        { "cubes",         c->cubes_hits,        c->cubes_peak,        REFINE_MAX_CUBES        },
        { "cube literals", c->cube_lits_hits,    c->cube_lits_peak,    REFINE_MAX_CUBE_LITS    },
        { "expand depth",  c->expand_depth_hits, c->expand_depth_peak, REFINE_MAX_EXPAND_DEPTH },
        { "LA vars",       c->la_vars_hits,      c->la_vars_peak,      REFINE_MAX_LA_VARS      },
        { "LA constraints",c->la_constr_hits,    c->la_constr_peak,    REFINE_MAX_LA_CONSTR    },
        { "NO shared",     c->no_shared_hits,    c->no_shared_peak,    NO_MAX_SHARED           },
        { "EUF terms",     c->euf_terms_hits,    c->euf_terms_peak,    REFINE_MAX_EUF_TERMS    },
        /* Collection side, not a stage: the branch guards recovered for a
         * call-site crossing.  Its peak SATURATES at the limit (the walk stops
         * collecting once full), so read `hits` for whether it bit and the
         * peak only for headroom while hits is 0. */
        { "path hyps",     c->path_hyps_hits,    c->path_hyps_peak,    RT_CS_PATH_MAX_HYPS     },
        /* The counterexample search's width.  A hit here costs a REFUTATION,
         * not a proof: the obligation stays Unknown rather than reporting the
         * counterexample it has.  See the `would run` line below. */
        { "model vars",    c->model_vars_hits,   c->model_vars_peak,   MODEL_MAX_VARS          },
    };
    fprintf(stderr, "refine: caps %s\n",
            refine_caps_any() ? "(one or more BIT -- those obligations answered "
                                "unknown and kept their runtime check)"
                              : "(none hit)");
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++)
        fprintf(stderr, "refine:   %-15s peak %6u / %-6u %s\n",
                rows[i].name, rows[i].peak, rows[i].limit,
                rows[i].hits ? "** HIT" : "");
    /* Two counters with no meaningful peak of their own: FM growth is bounded
     * by the LA-constraint limit it shares, and the round budget is a count of
     * exchange passes, not of a sized structure. */
    fprintf(stderr, "refine:   %-15s %u\n",
            "FM blow-ups", c->la_fm_hits);
    fprintf(stderr, "refine:   %-15s %u (of %u rounds)\n",
            "NO rounds out", c->no_rounds_hits, (unsigned)NO_MAX_ROUNDS);
    /* The subset of `model vars` hits that a higher cap would actually help:
     * the rest carry a non-int variable and would be declined by the sort gate
     * whatever the limit.  Printed separately rather than folded into the row
     * above because the row counts what the cap TURNED AWAY and this counts
     * what raising it would BUY -- and only the second justifies a raise. */
    fprintf(stderr, "refine:   %-15s %u (of %u over the cap)\n",
            "model vars run", c->model_vars_would_run, c->model_vars_hits);
    /* The search's OTHER decline: inside the width cap, past the sort gate,
     * but `n_cand ** n_vars` over the evaluation budget.  A count rather than
     * a peak, like the FM row: the quantity it bounds is a product, not a
     * sized structure.  Every hit here would run at a bigger budget. */
    fprintf(stderr, "refine:   %-15s %u (budget %u evaluations)\n",
            "model evals out", c->model_evals_hits, (unsigned)MODEL_MAX_EVALS);
    /* solver-integer-tail-plan: the two open phases' triggers, as counts.
     * `eq no-unit split` is how many all-integer equations with no unit
     * coefficient fell back to two inequalities (Phase 2, sigma-substitution);
     * `LA int feasible` is how many obligations S2 declined on an all-integer
     * multi-variable cube whose rational relaxation was feasible (Phase 3(a),
     * branch-and-bound / dark shadow) -- the population, not the payoff. */
    fprintf(stderr, "refine:   %-15s %u unknown obligation(s) (%u splits; Phase 2 trigger: sigma-substitution)\n",
            "eq no-unit split", c->eq_nounit_unknown_obl, c->eq_nounit_split);
    fprintf(stderr, "refine:   %-15s %u unknown obligation(s) (%u S2 declines; Phase 3a trigger: integer hull unchecked)\n",
            "LA int feasible", c->la_int_unknown_obl, c->la_int_relax_feasible);
}

void refine_discharge_all(RefineObligationVec *v, Arena *a) {
    if (!v) return;
    for (uint32_t i = 0; i < v->n; i++) refine_discharge_one(v->obs[i], a);
    if (stats_enabled()) {
        fprintf(stderr,
                "refine: %u obligation(s): %u proven, %u refuted, %u unknown "
                "(%u backend call(s), %u memo hit(s))\n",
                g_stats.collected, g_stats.proven, g_stats.invalid,
                g_stats.unknown, g_stats.backend_calls, g_stats.memo_hits);
        if (g_stats.proven_by_path)
            fprintf(stderr,
                    "refine: %u proved by path splitting (%u path probe(s))\n",
                    g_stats.proven_by_path, g_stats.path_probes);
        if (g_stats.inv_proven || g_stats.inv_unproven || g_stats.inv_declined)
            fprintf(stderr,
                    "refine: invariant: %u proven, %u unproven, %u loop(s) "
                    "declined\n",
                    g_stats.inv_proven, g_stats.inv_unproven,
                    g_stats.inv_declined);
        if (g_stats.templates_tried)
            fprintf(stderr,
                    "refine: %u result refinement(s) inferred from %u template "
                    "probe(s)\n",
                    g_stats.inferred, g_stats.templates_tried);
        if (g_stats.reflect_unfolds || g_stats.reflect_fuel_out)
            fprintf(stderr,
                    "refine: %u reflected unfolding(s) asserted (%u arm(s) selected "
                    "from a hypothesis), %u obligation(s) ran out of fuel\n",
                    g_stats.reflect_unfolds, g_stats.reflect_arms_by_hyp,
                    g_stats.reflect_fuel_out);
        refine_report_caps();
    }
}
