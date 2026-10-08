/* refine_solver_no.c -- S3: Nelson-Oppen combination of EUF (S1) and linear
 * arithmetic (S2).
 *
 * Neither theory alone decides a cube that mixes them.  The classic example:
 *
 *     len(v) = n,  0 <= i,  i < n   |-   i < len(v)
 *
 * LA cannot see that `len(v)` and `n` are the same thing; EUF cannot see that
 * `i < n` and `n = len(v)` give `i < len(v)`.  Nelson-Oppen fixes that by
 * having the two theories exchange the equalities each entails over their
 * SHARED terms -- here `len(v)`, which LA treats as an opaque variable
 * (purification, see refine_solver_arith.c) and EUF treats as an application.
 *
 * EUF and LRA are both convex, so the deterministic exchange below is complete
 * for them.  Integers are non-convex, which in general forces case-splitting
 * on disjunctions of equalities; we do not do that -- the loop simply reaches
 * a fixpoint and, if neither theory reports unsat, answers RT_UNKNOWN.  Sound,
 * incomplete on exactly the integer tail the plan reserves for S2c. */

#include "refine_solver.h"

#include <string.h>

/* Collect the terms both theories can see: the opaque terms LA purified out
 * (variables and uninterpreted applications), which EUF also has as nodes. */
/* EUF keeps that set as it registers terms (euf_shared_at), so this reads
 * only the eligible terms rather than filtering every term per cube. */
static uint32_t collect_shared(EufState *euf, VCTerm **out, uint32_t cap) {
    uint32_t n = 0;
    /* The full eligible count, even past the cap: the count we discard is the
     * telemetry -- it is the difference between "8 shared terms, exactly at
     * the cap" and "40 shared terms, 32 of them dropped". */
    uint32_t eligible = euf_shared_count(euf);
    for (uint32_t i = 0; i < eligible && n < cap; i++) out[n++] = euf_shared_at(euf, i);
    refine_cap_peak(&refine_caps()->no_shared_peak, eligible);
    if (eligible > cap) refine_caps()->no_shared_hits++;
    return n;
}

/* Decide a single cube by running both theories and exchanging entailed
 * equalities to a fixpoint.  Returns true when the cube is refuted. */
/* `shared_euf` non-NULL selects the SX3 incremental path: the caller owns one
 * EufState and brackets each call with euf_mark / euf_undo_to, so this
 * function's asserts are rolled back on return.  NULL keeps the per-cube
 * rebuild (TUR_REFINE_EUF=rebuild).  LaState stays per-cube either way --
 * making IT incremental is SX4, which is parked. */
static bool no_cube_unsat(RefineVC *vc, Arena *a, const VCCube *c,
                          EufState *shared_euf) {
    EufState *euf = shared_euf ? shared_euf : euf_new(vc, a);
    LaState  *la  = la_new(vc, a);

    if (!euf_assert_cube(euf, c)) return true;
    la_assert_cube(la, c);
    if (la_unsat(la)) return true;

    /* The exchange is quadratic in the shared set and each `la_entails_eq`
     * runs Fourier-Motzkin twice, so cap the set rather than let a wide
     * obligation turn into a compile-time stall.  Over the cap we simply do
     * not combine -- which is Unknown, which is sound. */
    VCTerm *shared[NO_MAX_SHARED];
    uint32_t n_shared = collect_shared(euf, shared, NO_MAX_SHARED);
    if (n_shared < 2) return false;

    bool progress = false;
    for (uint32_t round = 0; round < NO_MAX_ROUNDS; round++) {
        progress = false;

        /* EUF -> LA: every congruence the equality solver established becomes
         * a linear equation. */
        for (uint32_t i = 0; i < n_shared; i++) {
            for (uint32_t j = i + 1; j < n_shared; j++) {
                if (!euf_equal(euf, shared[i], shared[j])) continue;
                if (la_entails_eq(la, shared[i], shared[j])) continue;  /* already known */
                la_assert_eq(la, shared[i], shared[j]);
                progress = true;
            }
        }
        if (la_unsat(la)) return true;

        /* LA -> EUF: every equality the arithmetic entails becomes a union. */
        for (uint32_t i = 0; i < n_shared; i++) {
            for (uint32_t j = i + 1; j < n_shared; j++) {
                if (euf_equal(euf, shared[i], shared[j])) continue;
                if (!la_entails_eq(la, shared[i], shared[j])) continue;
                progress = true;
                if (!euf_assert_eq(euf, shared[i], shared[j])) return true;
            }
        }
        /* Re-check the cube's disequalities against the enlarged partition. */
        for (uint32_t i = 0; i < c->n; i++) {
            VCTerm *lit = c->lits[i];
            VCTerm *at  = refine_lit_atom(lit);
            if (refine_lit_is_neg(lit) && at->op == VC_EQ &&
                (refine_cube_nan_free(c, at->kids[0]) ||
                 refine_cube_nan_free(c, at->kids[1])) &&
                euf_equal(euf, at->kids[0], at->kids[1]))
                return true;
        }
        if (!progress) break;
    }
    /* Falling out of the loop with `progress` still set means the exchange was
     * STILL learning equalities when the round budget ran out -- a real cap
     * hit.  Reaching a fixpoint inside the budget is not, even though both
     * paths leave the loop the same way. */
    if (progress) refine_caps()->no_rounds_hits++;
    return false;
}

RefineDecision refine_s3_decide_cc(RefineVC *vc, Arena *a, RefineCubeCache *cc) {
    if (!vc || !vc->goal) return refine_unknown();

    const VCCubeSet *cs;
    if (!refine_cubes_get(vc, a, cc, &cs)) return refine_unknown();
    if (cs->trivial || cs->n == 0) return refine_valid();

    EufState *inc = euf_incremental_mode() ? euf_new(vc, a) : NULL;
    for (uint32_t i = 0; i < cs->n; i++) {
        bool refuted;
        if (inc) {
            EufMark m = euf_mark(inc);
            refuted = no_cube_unsat(vc, a, &cs->cubes[i], inc);
            euf_undo_to(inc, m);
        } else {
            refuted = no_cube_unsat(vc, a, &cs->cubes[i], NULL);
        }
        if (!refuted) return refine_unknown();
    }
    return refine_valid();
}

/* refine-chain-expands-the-same-dnf-four-times: the original entry point, kept
 * so every caller outside the chain driver (`tur smt`, tests/unit, the SX8
 * doors) is unchanged -- a fresh cache means it builds exactly once, as it
 * always did. */
RefineDecision refine_s3_decide(RefineVC *vc, Arena *a) {
    RefineCubeCache cc = {0};
    return refine_s3_decide_cc(vc, a, &cc);
}
