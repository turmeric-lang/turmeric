/* prof.h -- `--timings`: the compiler's own zone timer.
 *
 * profiling-plan C0+C1 (docs/upcoming/profiling-plan.md sections 1.2 and 3).
 * A handful of coarse boundaries in the driver and the elaborator call
 * prof_enter / prof_exit; the timer keeps an explicit zone stack and charges
 * EXCLUSIVE time on every transition, so each phase's self time is time spent
 * in that phase and nowhere nested inside it, and the phases partition the
 * invocation's wall clock: sum(self_ms) == wall_ms by construction.
 *
 * Off (the default) every hook is one predictable branch on g_timings -- no
 * #ifdef, no build variant.  On, a transition costs one monotonic clock read;
 * re-entering the zone already on top (ELAB inside ELAB, the common case) costs
 * no read at all, because the time it would split belongs to the same zone
 * either way.
 *
 * The zone list and src/runtime/pass.h's PassKind are kept in step by a switch
 * with no default in the driver (pass_zone): a new PassKind without a zone is a
 * -Wswitch error rather than a pass whose time quietly lands in a neighbor.
 */
#ifndef TUR_PROF_H
#define TUR_PROF_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "globals.h"   /* g_timings, g_timings_out */

/* Pipeline order; also the order phases[] is written in.  PROF_OTHER is the
 * bottom of the stack: time no zone claims (argument parsing, path and
 * manifest resolution, writing outputs, process teardown). */
typedef enum ProfZone {
    PROF_READ,          /* "read"         reader: text -> forms                */
    PROF_SWEET,         /* "sweet"        sweet-exp t-expression preprocessor   */
    PROF_MODULE_LOAD,   /* "module-load"  import/load/stdlib-autoload: finding,
                         *                reading and splicing module files (the
                         *                parse is "read", the module's own
                         *                elaboration is "elaborate")           */
    PROF_MACRO_EXPAND,  /* "macro-expand" elab_expand_macro                     */
    PROF_ELAB,          /* "elaborate"    PASS_ELABORATE minus the zones above  */
    PROF_TYPECLASS,     /* "typeclass"    class/instance processing, method
                         *                dispatch, instance resolution         */
    PROF_KIND,          /* "kind-check"   PASS_KIND_CHECK                       */
    PROF_EFFECT_ENV,    /* "effect-env"   PASS_EFFECT_ENV (was "effect-lower") */
    PROF_EFFECT_ROW,    /* "effect-row"   PASS_EFFECT_ROW_INFER                 */
    PROF_CPS,           /* "cps"          PASS_CPS                              */
    PROF_BORROW,        /* "borrow-check" PASS_BORROW_CHECK                     */
    PROF_REFINE,        /* "refine"       refinement obligations: frame checks,
                         *                call-site crossings, discharge        */
    PROF_MONO,          /* "mono"         the by-value spec resolution pass     */
    PROF_EMIT,          /* "emit"         C emission and the driver's split swap */
    PROF_CC,            /* "cc"           wall time waiting on toolchain
                         *                subprocesses (cc, ld, nm)             */
    PROF_JIT,           /* "jit"          tur jit: c2mir + MIR link             */
    PROF_RUN,           /* "run"          the compiled program's own execution
                         *                (tur run, tur jit)                    */
    PROF_OTHER,         /* "other"        everything no zone claims             */
    PROF_ZONE_N
} ProfZone;

/* The zone currently on top of the stack (PROF_OTHER when none is open). */
extern ProfZone g_prof_top;

void prof_enter_slow(ProfZone z);
void prof_exit_slow(ProfZone z);

static inline void prof_enter(ProfZone z) {
    if (!g_timings) return;
    prof_enter_slow(z);
}

static inline void prof_exit(ProfZone z) {
    if (!g_timings) return;
    prof_exit_slow(z);
}

/* True when `z` is already the innermost open zone, so entering it again
 * would change nothing.  Lets a hot recursive entry point (elab_form) skip
 * the push entirely. */
static inline bool prof_top_is(ProfZone z) { return g_prof_top == z; }

/* Turn the timer on: g_timings = true, g_timings_out = out_path (NULL for the
 * table only), the clock starts now, and the report is written at process
 * exit (atexit) unless prof_finish runs first.  argv is the command line as
 * the user typed it, for the report's "command".  Idempotent. */
void prof_start(int argc, char **argv, const char *out_path);

/* Close the books: charge the open zone up to now, print the table to stderr
 * and, when a path was given, write the tur-profile/1 JSON there.  Turns the
 * timer off; later hooks are no-ops.  Safe to call more than once and when
 * the timer was never started.  A forked child that leaves with _exit must
 * call it itself (atexit does not run there). */
void prof_finish(void);

/* Attach an honesty caveat to the report's "notes" (copied; at most 8). */
void prof_note(const char *note);

/* The JSON name of a zone ("elaborate", "cc", ...). */
const char *prof_zone_name(ProfZone z);

#endif /* TUR_PROF_H */
