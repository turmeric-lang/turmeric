#!/usr/bin/env python3
"""tests/type-fuzz-src.py -- SOURCE-LEVEL fuzzer for typed-boundary plumbing.

Why this exists
---------------
`tests/refine-fuzz-src.py` proved the pattern: generate whole programs, run
them through the real pipeline, compare against a property the design forbids
violating.  This harness applies the same pattern to a different bug family --
the one behind `result-monad-bind-typed-boundary-miscompiles`,
`poly-result-hof-capturing-closure-sigbus`, `generic-closure-return-type-app`,
and the No-Lazy-`:int` rule in CLAUDE.md.

Every one of those bugs has the same anatomy: a value's REAL type (a by-value
struct, a closure with an environment, a type application) meets the int64
carrier erasure at some boundary -- a function return, a `(fn ...)` parameter,
a typeclass method result, a Vec element slot -- and the two sides disagree
about the representation.  The full representation/boundary inventory this
harness walks is documented in docs/guides/value-representations-guide.md;
keep the two in sync.  The checker accepts, and then either cc rejects the
emitted C (`invalid initializer`), the linker misses a symbol, or the binary
reads a struct out of a pointer and dies.  Hand-written fixtures only cover
the combinations someone thought of; the reports above are all combinations
nobody did.

The property that matters
-------------------------
Every generated program is CORRECT BY CONSTRUCTION: the generator tracks the
concrete value it routes through the plumbing and knows the exact stdout the
program must produce.  So the oracle is total:

    tur check accepts  ==>  the C compiles, links, runs cleanly,
                            and prints exactly the predicted output

Four BUG classes fall out, each a checker/codegen disagreement:

  * BUG_invalid_c      -- `tur check` passed, cc rejected the emitted C
  * BUG_link           -- cc compiled, the link failed (undefined symbol)
  * BUG_crash          -- the binary died (SIGSEGV/SIGBUS/SIGABRT)
  * BUG_wrong_output   -- ran clean, printed the wrong values

One report-only class:

  * GEN_REJECT         -- `tur check` rejected a program the generator claims
    is legal.  Either the generator is wrong (fix it here) or the checker is
    (that is `generic-closure-return-type-app` Defect A's shape).  Saved for
    triage, never failed on, because the generator's legality claim is not
    itself machine-checked.

What it generates
-----------------
A program is 1-4 independent LEGS.  Each leg routes a known scalar (int,
float, bool, cstr) through a randomly chosen WRAPPER (bare, by-value struct,
:heap struct, ADT, (Option T), (Result T int), (Vec T), (Option Box),
(Result Box int), a capturing thunk) and then across 1-3 randomly chosen
BOUNDARY CROSSINGS while wrapped:

    pass-through defn / let-in-defn / if-in-defn / recursive defn
    let binding / ascription `(:: e T)` / generic identity `[A] x:A -> A`
    ^fat HOF (capturing closure in)  / thin HOF (carrier-safe types only)
    closure RETURN (defn returning `(fn [] T)` that captures its argument)
    typeclass method dispatch (bare and dotted)

then unwraps and prints.  Floats are printed as `(= e <lit>)` against the
generator's own literal (all float values used are exact in binary and only
+,-,* touch ints), so float formatting never enters the oracle.

Runtime seams (--seam-frac, default 0.30)
-----------------------------------------
Every crossing above is one the COMPILER owns both ends of: it emits the
producer and the consumer in the same pass and the value never leaves its
static type.  A SEAM is the other thing -- the payload is parked in a runtime
data structure and read back later by different emitted code:

    session send/recv      (TurChannel slot,  elab_sessions.c)
    router send-to/recv-from (protocol router, elab_global.c -- a SEPARATE
                            template from the binary one, so a fix to one
                            does not fix the other)
    generator yield/gen-next (generator frame)
    async/await            (future slot)
    perform/resume         (fiber slot)          -- correct; positive control
    any + cast             (tagged box)          -- correct; positive control
    tvar write/cas/read    (transactional cell)  -- was "correct" until the
                                                    value oracle: floats truncated

The runtime slot has ONE C type, so the payload is cast in and out, and a plain
C cast of a `double` is a value conversion that TRUNCATES.  This axis exists
because that defect has been found and fixed TEN times one feature at a time
(see docs/archive: ascribe-int-to-float-reinterprets,
forall-dict-float-result-truncated, fiber-effect-float-result-truncated, ...)
while four fuzzers that all fuzz floats never emitted a single seam -- the
shapes were absent, not suppressed, and absence is invisible.

Two things about the seam oracle are load-bearing:

  * It PRINTS the value rather than comparing `(= v <lit>)`.  A seam that
    erases its payload to int makes that comparison a TUR-E0042 reject, and
    a reject is not a failure -- the defect would be generated, rejected, and
    filed under "the generator made an illegal program".  Printing turns the
    same defect into a wrong ANSWER.  Every FLOAT_LITS literal round-trips
    through `println` exactly as written, so the predicted string is the
    literal.
  * A seam reject is its own class (SEAM_REJECT), reported and counted rather
    than folded into GEN_REJECT, because for a seam it means the CHECKER
    refused the payload -- which is fix direction 4 of
    session-payloads-are-int64-only, not generator noise.

Three of the seven seams are already CORRECT and are generated on purpose: a
run where every seam fails must be distinguishable from a broken emitter.
`--seam-matrix` prints the whole seam x payload table deterministically, which
is how a fix is verified.

Known open bugs
---------------
Shapes that reproduce ALREADY-FILED open reports are excluded from generation
by default -- a fuzz run's value is what it finds that is NOT yet on file.
`known_bug_slug()` is the single place that knowledge lives; `--emit-known`
re-enables those shapes (each finding is then classified KNOWN_<class> and
never fails the run) -- useful to watch a fix land.  `--known-probes` runs a
pinned minimal repro per open report and prints fires/fixed, so the avoid
list cannot silently rot after the fixes land: a `fixed` row means the entry
should be retired from `known_bug_slug` and its shape returned to the
default pool.

Attribution
-----------
A failing program is auto-bisected: each leg is rerun in isolation and the
failing leg(s) are saved with their shape tags, so a finding reads as
"wrapper=res route=fat_hof,gid scalar=float" rather than a 120-line blob.  If
no leg fails alone, the whole program is saved as an INTERACTION finding --
those are the interesting ones.

Proving the fuzzer can fail
---------------------------
`--self-test` feeds the classifier four fixed programs: a clean pass, a
deliberate wrong-output, a guaranteed crash (null write in inline C), and a
type error.  All four classifications are asserted.  Beyond plumbing, the
harness demonstrably detects every open report in the known table: run
`--known-probes` and watch all of them fire on an unfixed build.

Usage
-----
    python3 tests/type-fuzz-src.py [--n 200] [--seed 1] [--jobs N]
                                   [--tur ./build/tur] [--save-dir DIR]
                                   [--legs 1..4] [--emit-known]
                                   [--seam-frac 0.30] [--seam NAME]
                                   [--self-test] [--known-probes]
                                   [--seam-matrix]

Exit status is 1 if any BUG class fired (GEN_REJECT and KNOWN_* never fail
the run), 0 otherwise.
"""

import argparse
import os
import random
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor, as_completed

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fuzz_arm  # noqa: E402  (tests/fuzz_arm.py)
import fconv_lint  # noqa: E402  (tests/fconv_lint.py)

TIMEOUT = 90

# ---------------------------------------------------------------------------
# Known open reports -- the single place this knowledge lives.
#
# Each entry: (slug, predicate over a leg's tag set).  A leg whose tags match
# is (a) not generated by default and (b) classified KNOWN_* when --emit-known
# turns it back on.  When a report is fixed and archived, delete its row here
# and the shape returns to the default pool.
# ---------------------------------------------------------------------------

BYVALUE_WRAPPERS = {"box", "adt", "opt", "res", "opt_box", "res_box"}


# --crossing NAME: offer only `x_NAME` wherever it applies (None: all).
FORCE_CROSSING = None

CROSSING_TAGS = {"through", "deep", "let", "ascribe", "gid", "fat_hof",
                 "thin_hof", "class_thru", "tyvar_run",
                 "class_nested", "class_nullary_newtype",
                 "gid_let", "class_let", "gbody", "rank2_class"}


def known_bug_slug(tags):
    """Return the docs/reported slug a leg shape reproduces, or None."""
    # (Retired 2026-07-30 by fn-value-fat-normalization stage 2: fn-typed
    # VALUES survive pass-through returns, ascribe-around-let, and nested
    # fat HOFs now -- thunk legs are back in the full crossing pool.)
    # (fn-value-carrier-fat-seam-residuals: RESOLVED 2026-07-31 -- the tail
    # walkers resolve let-ALIASES to their origin (a carrier-param alias
    # converts via poly-to-fat instead of being thin-shimmed or skipped), the
    # if unifier admits carrier-vs-boxed-fn joins by inserting the conversion
    # at the join, and an ascription of a carrier param to its own fn type is
    # a no-op assertion -- and archived; thunk through/deep legs are back in
    # the full crossing pool.)
    # (Retired 2026-07-30 by fn-value-fat-normalization stage 1: thin fn
    # params with CONCRETE non-carrier-safe signatures -- by-value and heap
    # results/args -- are fat-normalized now, so those thin_hof shapes are
    # back in the default pool.)
    # (Retired 2026-08-01 by fn-value-fat-normalization increment 2: TYVAR
    # signatures are fat-normalized too -- the carrier-side feeds that stage 1
    # was waiting on (a call through a rank-2/forall param, and the
    # make-struct fn-field store) now shim as well -- so tyvar_run legs are
    # back in the default pool.  Pinned by
    # tests/fixtures/fn-value-fat-normalized-tyvar-params/.  What is still
    # open in that report is the EFFECT-ROW row only, which the generator
    # does not produce: no leg wrapper emits an effect-annotated fn param.)
    # (result-monad-bind-typed-boundary-miscompiles: RESOLVED 2026-07-31 by
    # consolidation increment 2 -- continuation-wrapper ABI paired with the
    # selected entry point + ascription-aware carrier-type recovery -- and
    # archived; bind legs are in the DEFAULT generation rotation now.)
    # (vec-byvalue-struct-element-invalid-c: RESOLVED 2026-07-31 by
    # consolidation increment 3 -- any-width by-value products are heap-boxed
    # into container slots (push-side escaping bridge, read-side deref-unbox,
    # ownership probes in lockstep via type_is_boxed_container_elem) -- and
    # archived; vec_box_byvalue wrappers are in the DEFAULT pool now.)
    # (class-method-result-into-generic-invalid-c: RESOLVED 2026-07-31 by
    # consolidation increment 2 -- the carrier-producer classifier now knows
    # M7 by-value instance results -- and archived; rows retired.)
    #
    # type-confusion-detection-plan F1: two typeclass shapes the generator
    # could not previously express at all (one instance per class, unary
    # methods only, no same-class nesting, no nullary methods).  Both are open
    # reports, so both are avoided by default and pinned by --known-probes.
    # (nested-class-method-call-picks-the-first-instance: RESOLVED 2026-09-11 --
    # emit_reresolve_disp_type recovers the dispatch type through a receiver
    # that is itself a re-resolved class-method call -- and archived.  The
    # class_nested shape is in the DEFAULT generation pool now; its probe below
    # stays as a FIXED regression row.)
    # (nullary-class-method-unresolvable-over-newtype-tyvar: RESOLVED
    # 2026-09-11 -- the return-directed representative search accepts a
    # carrier-compatible opaque newtype, gated on the class tyvar reaching a
    # parameter -- and archived.  Shape back in the DEFAULT pool; probe below
    # kept as a FIXED regression row.)
    # (let-bound-class-method-result-in-constrained-generic-truncates:
    # RESOLVED 2026-09-26 -- a class-method call on an abstract-tyvar receiver
    # is typed with that receiver's `A` -- and archived.  Its shape,
    # class_let, is in the DEFAULT pool; probe below kept as a FIXED row.)
    # (let-bound-generic-call-result-in-generic-truncates: RESOLVED
    # 2026-09-26 -- a let binding over a generic call instantiated to the
    # enclosing signature's own tyvar is typed with that tyvar, through a
    # reinterpret emit lowers per clone -- and archived.  gid_let is in the
    # DEFAULT pool; probe below kept as a FIXED row.)
    #
    # ---- runtime seams (seam axis, added 2026-09-16) ------------------------
    #
    # The avoidance POLICY here is deliberately the opposite of the crossings
    # above.  A known-bad crossing shape is not GENERATED by default; a known-bad
    # seam shape IS generated every run and merely downgraded to KNOWN here.
    #
    # That is the whole lesson of this axis.  Ten float-truncation reports were
    # found one at a time over months, and the reason the seam family was not
    # among them is that no generator ever emitted a seam at all -- the shapes
    # were absent, not suppressed, and absence is invisible.  Suppressing
    # generation to keep a run green reproduces that failure mode on purpose.
    # The seam pool is seven entries wide, so running every row costs a few
    # seconds and keeps each one continuously measured: when a row starts
    # passing it stops matching here, and --known-probes reports it FIXED.
    #
    # (session-payloads-are-int64-only, router-payloads-are-int64-only:
    # RESOLVED 2026-09-16 and archived -- float is bit-reinterpreted, cstr
    # cast through intptr_t, and a by-value struct payload is REJECTED by
    # design with TUR-E0212.  Rows retired 2026-09-26.  They had outlived
    # their reports by ten nights: every `payload_box` session/router leg --
    # a designed rejection, which is what SEAM_REJECT is for -- was being
    # counted as KNOWN against a closed report.  Both probes below stay as
    # FIXED regression rows.)
    # The generator and await seams were the worst of the family until
    # 2026-09-17 -- the slot's type reached the SIGNATURE (gen-unwrap / await
    # declared :int), so bool printed 1 and cstr a raw pointer.  Both read
    # the slot back at the payload's declared type now and have no row here.
    #
    # (generator-thunk-call-site-returns-void-ptr-not-carrier: RESOLVED
    # 2026-10-03 and archived -- the call site and the slot-0 widen wrapper
    # both follow the lambda's recorded return spelling now.  Its row
    # {gbody, thunk, closure_ret} is retired, so a regression is a
    # BUG_fnptr_trap again rather than a KNOWN; the probe below stays as a
    # FIXED regression row.)
    return None


# Probes whose ONLY symptom is a trap from clang's `-fsanitize=function`.
#
# Such a program builds, runs, and prints the CORRECT answer -- the mismatch is
# invisible to everything but the detector.  So on a box where the detector is
# unavailable (stock macOS: Apple clang does not provide it, and the banner
# reads `fnsan: UNAVAILABLE`) the probe comes back `clean`, which the FIXED arm
# below would report as "retire its known_bug_slug row" -- advice to close a
# report that is still open, on the one kind of box that cannot see it.
#
# A row listed here reports UNKNOWN rather than FIXED whenever the run's banner
# is not ARMED.  It says nothing about boxes that ARE armed: there the row is
# judged normally, and Linux CI is armed.
#
# This is about the SYMPTOM, not the tags: a wrong-ANSWER defect belongs in the
# 3-tuple form instead, which is visible without any sanitizer.
TRAP_ONLY_PROBES = frozenset({
    "generator-thunk-call-site-returns-void-ptr-not-carrier",
})


# Pinned minimal repros, one per open report above, used by --known-probes to
# keep the avoid list honest: when one prints `fixed`, retire its
# known_bug_slug row.
#
# A row is either (label, src) -- "fires" means the compiler rejected it,
# emitted invalid C, failed to link, or crashed -- or (label, src, expected),
# which ADDS a wrong-output arm: a program that builds and runs cleanly but
# prints something other than `expected` also counts as firing.  The 3-tuple
# form is required for any defect whose symptom is a wrong ANSWER; without it
# such a probe exits 0 and reports FIXED on a build that is still broken.
KNOWN_PROBES = [
    # generator-thunk-call-site-returns-void-ptr-not-carrier: FIXED
    # 2026-10-03, pinned by tests/fixtures/generator-thunk-call-site-carrier.
    # Kept as a regression probe.  Reduced from the nightly's case 376 at seed
    # 20261003: a generator yield of an immediately-applied closure inside a
    # generic body (the generic thunk around it turned out incidental).
    #
    # 2-tuple on purpose: the symptom is a TRAP, which run_case classifies
    # `fnptr_trap` and known_probes already counts as firing.  It is NOT a
    # wrong-answer defect, so the 3-tuple form would be wrong -- the program
    # prints the correct `true`.
    #
    # Listed in TRAP_ONLY_PROBES above, so on a box without
    # -fsanitize=function (`fnsan: UNAVAILABLE`) it reports UNKNOWN rather
    # than a FIXED it could not have observed.
    ("generator-thunk-call-site-returns-void-ptr-not-carrier",
     "(defn mk [x : bool] : (fn [] bool) (fn [] x))\n"
     "(defn thunk [B] [v : B] : (fn [] B) (fn [] v))\n"
     "(defn gbody [A] [x : A] : A\n"
     "  ((thunk (gen-unwrap (gen-next (gen [] (yield ((fn [] x)))))))))\n"
     "(defn main [] : int\n"
     "  (println ((gbody (mk true))))\n  0)\n"),
    # The tyvar-result and by-value-result rows are FIXED (stage 1 and
    # increment 2 of fn-value-fat-normalization) and pinned by
    # tests/fixtures/fn-value-fat-normalized-{,tyvar-}params/.  They stay here
    # as regression probes: --known-probes prints FIXED for both, and a future
    # regression flips them back to `fires` without waiting for a fuzz session.
    ("poly-result-hof-capturing-closure-sigbus (tyvar result)",
     "(defn run [R] [body : (fn [] R)] : R (body))\n"
     "(defn main [] : int\n"
     "  (let [k 7] (println (run (fn [] : int (+ k 1)))))\n  0)\n"),
    ("poly-result-hof-capturing-closure-sigbus (by-value struct result)",
     "(defstruct FzB [a : int])\n"
     "(defn call [f : (fn [] FzB)] : FzB (f))\n"
     "(defn main [] : int\n"
     "  (let [k 7] (println (.a (call (fn [] (FzB k))))))\n  0)\n"),
    # RESOLVED 2026-08-16 (the report's LAST row) by the CPS increment: the
    # E2a registry call sites dispatch fat (slot 0 = capturing-lambda entry
    # with an env-taking __cps twin, slot 1 = fatshim's stashed bare-fn
    # entry), and effect-annotated fn params are fat-normalized like every
    # other nominal fn param.  Kept as a FIXED regression probe; pinned by
    # tests/fixtures/effect-capturing-closure-thin-param/.
    # `Write` is declared (an undeclared row name is TUR-E0026) and main's row
    # names IO because it prints (`println` is #fx{IO}) -- the same two edits
    # that fixture got; without them the probe reads `fires (reject)`.
    ("poly-result-hof-capturing-closure-sigbus (effect row)",
     "(defeffect Write [s :cstr] :nil)\n"
     "(defn run [body : (fn [] #fx{Write} int)] #fx{Write} : int (body))\n"
     "(defn main [] #fx{Write IO} : int\n"
     "  (let [k 7] (println (run (fn [] #fx{Write} : int (+ k 1)))))\n  0)\n"),
    # (result-monad-bind-typed-boundary-miscompiles: RESOLVED 2026-07-31,
    # archived; probe retired -- pinned by
    # tests/fixtures/result-monad-bind-typed-boundary/.)
    # RESOLVED 2026-08-16 (both defects; archived) -- kept as a FIXED
    # regression probe like the two rows above, since the boundary was
    # unusually sharp.  Pinned by
    # tests/fixtures/generic-closure-return-type-app/.
    # Faithful to the report: it takes the stdlib Cons (a defstruct).  The
    # same shape over a local parametric defdata checked AND ran clean even
    # before the fix, so the trigger was narrower than "generic + type-app +
    # closure return".
    ("generic-closure-return-type-app (Defect A: checker reject)",
     "(defn pure [A] [x : A] : (fn [] (Cons A))\n"
     "  (fn [] (tcons x (tnil))))\n"
     "(defn use [A] [xs : (Cons A)] : int 0)\n"
     "(defn main [] : int (use ((pure 1))))\n"),
    # (vec-byvalue-struct-element-invalid-c: RESOLVED 2026-07-31, archived;
    # probe retired -- pinned by tests/fixtures/vec-byvalue-struct-element/
    # and tests/fixtures/map-narrow-struct-value/.)
    # (fn-typed-value-return-ascribe-miscompiles: RESOLVED 2026-07-30 by
    # fat-normalization stage 2 and archived; its matrix -- broken rows
    # included -- is pinned by tests/fixtures/fn-value-matrix-ok-rows/, so
    # the probes are retired rather than kept as permanent FIXED rows.)
    # (class-method-result-into-generic-invalid-c: RESOLVED 2026-07-31,
    # archived; probe retired -- pinned by
    # tests/fixtures/class-method-result-into-generic/.)
    # (fn-payload-in-container-undeclared-temp: RESOLVED 2026-07-31 by
    # fat-normalization stage 2 -- the parametric-defdata + FLOAT variant
    # verified by hand -- and archived; probe retired.)
    # (fn-value-carrier-fat-seam-residuals: RESOLVED 2026-07-31, archived;
    # probe retired -- pinned by tests/fixtures/fn-value-carrier-fat-seams/.)
    #
    # RESOLVED 2026-09-11, archived.  Kept as a FIXED regression probe, like the
    # rows above: the wrong-answer arm is what detects a relapse, and a
    # relapse here is silent everywhere else.  The row carries an expected
    # stdout because the defect produced a wrong ANSWER (7 for 7.1) rather than
    # a rejection -- without it this probe would report FIXED on a broken
    # build.
    ("nested-class-method-call-picks-the-first-instance",
     "(defclass JsP [a] (joinp [x : a y : a] : a))\n"
     "(definstance JsP [int]   (joinp [x y] (if (< x y) y x)))\n"
     "(definstance JsP [float] (joinp [x y] (if (< x y) y x)))\n"
     "(defn fp [^JsP A] [x : A y : A] : A (joinp (joinp x y) y))\n"
     "(defn main [] : int (println (fp 2.5 7.1)) 0)\n",
     "7.1\n"),
    # RESOLVED 2026-09-11, archived.  Kept as a FIXED regression probe.  Note
    # the probe's `fq` takes an A-typed PARAMETER: that is what makes the shape
    # specializable, and a relapse of the gate would show up here.
    ("nullary-class-method-unresolvable-over-newtype-tyvar",
     "(defclass SgP [a] (combp [x : a y : a] : a))\n"
     "(defclass MoP [a] (mzerop [] : a))\n"
     "(defopaque SumP :int)\n"
     "(definstance SgP [SumP] (combp [x y] x))\n"
     "(definstance MoP [SumP] (mzerop [] (:: 0 SumP)))\n"
     "(defn fq [^SgP A ^MoP A] [x : A] : A (combp x (mzerop)))\n"
     "(defn main [] : int (println (:: (fq (:: 3 SumP)) int)) 0)\n"),
    # RESOLVED 2026-09-26.  Found by the nightly as class_nested + gid on a
    # float leg (seeds 20260913/17/18/25), filed by none of them -- see
    # tests/fuzz-seed-corpus.txt.  A FIXED regression probe: the flat tail is
    # the minimal shape, and -4.25 printed -nan while it was open.
    ("constrained-generic-float-result-into-generic-value-converts",
     "(defclass NvP [a] (nvp [x : a y : a] : a))\n"
     "(definstance NvP [int]   (nvp [x y] x))\n"
     "(definstance NvP [float] (nvp [x y] x))\n"
     "(defn fr [^NvP A] [x : A] : A (nvp x x))\n"
     "(defn gr [A] [x : A] : A x)\n"
     "(defn main [] : int (println (gr (fr -4.25))) 0)\n",
     "-4.25\n"),
    # RESOLVED 2026-09-26.  FIXED regression row: -4.25 printed -4.
    ("let-bound-class-method-result-in-constrained-generic-truncates",
     "(defclass LbP [a] (lbp [x : a y : a] : a))\n"
     "(definstance LbP [int]   (lbp [x y] x))\n"
     "(definstance LbP [float] (lbp [x y] x))\n"
     "(defn fl [^LbP A] [x : A] : A (let [y (lbp x x)] (lbp y x)))\n"
     "(defn main [] : int (println (fl -4.25)) 0)\n",
     "-4.25\n"),
    # RESOLVED 2026-09-26.  FIXED regression row: 9.75 printed 9.  No
    # typeclass involved.
    ("let-bound-generic-call-result-in-generic-truncates",
     "(defn gl [A] [x : A] : A x)\n"
     "(defn wl [A] [x : A] : A (let [y (gl x)] y))\n"
     "(defn main [] : int (println (wl 9.75)) 0)\n",
     "9.75\n"),
    # ---- runtime seams ------------------------------------------------------
    #
    # All four are wrong-ANSWER defects, so all four MUST carry the expected
    # stdout (the 3-tuple form).  Without it each one exits 0, prints the wrong
    # number, and this probe would report FIXED on a broken build -- the exact
    # trap the comment above the table warns about.  These are the rows that
    # keep the seam avoid-list honest.
    ("session-payloads-are-int64-only (float truncates)",
     "(defn fzspawn [f : ptr<void>] : ptr<void>\n"
     "  ```c\n"
     "  pthread_t *tid = (pthread_t *)malloc(sizeof(pthread_t));\n"
     "  pthread_create(tid, NULL, tur_session_thread_wrapper, f);\n"
     "  return (void *)tid;\n"
     "  ```)\n"
     "(defn fzjoin [t : ptr<void>] : int\n"
     "  ```c\n"
     "  pthread_join(*(pthread_t *)t, NULL);\n"
     "  free(t);\n"
     "  return 0;\n"
     "  ```)\n"
     "(defn main [] : int\n"
     "  (let [[s r] (make-session (Send float Close))]\n"
     "    (let [k (fzspawn (fn [] (let [[v r2] (recv r)]\n"
     "                             (println v) (close r2))))]\n"
     "      (let [s2 (send s 7.25)] (close s2) (fzjoin k))))\n"
     "  0)\n",
     "7.25\n"),
    ("router-payloads-are-int64-only (float truncates)",
     "(defprotocol FzQ [A B]\n  (-> A B float))\n"
     "(defn fzspawn [f : ptr<void>] : ptr<void>\n"
     "  ```c\n"
     "  pthread_t *tid = (pthread_t *)malloc(sizeof(pthread_t));\n"
     "  pthread_create(tid, NULL, tur_session_thread_wrapper, f);\n"
     "  return (void *)tid;\n"
     "  ```)\n"
     "(defn fzjoin [t : ptr<void>] : int\n"
     "  ```c\n"
     "  pthread_join(*(pthread_t *)t, NULL);\n"
     "  free(t);\n"
     "  return 0;\n"
     "  ```)\n"
     "(defn fzqa [^linear ch :(Role FzQ A)] : nil\n"
     "  (let [ch (send-to ch B 7.25)] (close ch)))\n"
     "(defn fzqb [^linear ch :(Role FzQ B)] : nil\n"
     "  (let [[v ch] (recv-from ch A)] (println v) (close ch)))\n"
     "(defn main [] : int\n"
     "  (let [[a b] (make-protocol FzQ)]\n"
     "    (let [k (fzspawn (fn [] (fzqa a)))] (fzqb b) (fzjoin k)))\n"
     "  0)\n",
     "7.25\n"),
    # (generator-yield-payload-is-int64-only's and async-await-payload-is-
    # int64-only's rows retired 2026-09-17: both print `fixed` -- the reads
    # happen at the payload's declared type now.)
]


# ---------------------------------------------------------------------------
# Leg generation
# ---------------------------------------------------------------------------

FLOAT_LITS = ["0.5", "1.25", "2.75", "3.25", "7.1", "-1.5", "-4.25", "6.5",
              "7.25", "10.5"]
CSTR_POOL = ["alpha", "brave-7", "c3", "delta-x", "e", "fzz-99", "gg-hh"]


class Leg:
    """One independent value route: defns + main-body form + expected lines."""

    def __init__(self):
        self.prelude = []     # top-level forms emitted ONCE per program, deduped
        self.defs = []        # top-level defn/defstruct/... source strings
        self.body = []        # forms for main (strings)
        self.expected = []    # exact stdout lines
        self.tags = set()     # shape tags for attribution / known matching
        self.seam = None      # seam name when this is a runtime-seam leg


class Gen:
    def __init__(self, rng, idx, emit_known):
        self.rng = rng
        self.i = idx              # leg index, used to prefix every name
        self.emit_known = emit_known
        self.n_names = 0
        # --seam-matrix pins one cell at a time instead of sampling.
        self.force_scalar = None
        self.force_payload = None

    def name(self, stem):
        self.n_names += 1
        return "l%d%s%d" % (self.i, stem, self.n_names)

    # -- scalars --------------------------------------------------------------

    def pick_scalar(self, narrow=False):
        """`narrow` admits the non-word scalars (float32, int16) -- value legs
        only.  No fuzzer had ever generated a float32: gen-unwrap at float32
        read the low half of the yielded double, and a generic's float32
        spec returned its carrier's bits, with nothing to notice
        (docs/archive/generic-spec-carrier-crossings.md)."""
        pool = ["int", "int", "int", "float", "bool", "cstr"]
        if narrow:
            pool += ["float32", "int16"]
        t = self.force_scalar or self.rng.choice(pool)
        if t in ("int", "int16"):
            return t, self.rng.randint(-20, 20)
        if t in ("float", "float32"):
            return t, self.rng.choice(FLOAT_LITS)
        if t == "bool":
            return t, self.rng.choice([True, False])
        return t, "%s-%d" % (self.rng.choice(CSTR_POOL), self.rng.randint(0, 99))

    def lit(self, ty, v):
        if ty == "int":
            return str(v)
        if ty == "float":
            return v
        if ty in ("float32", "int16"):
            return "(:: %s %s)" % (v, ty)
        if ty == "bool":
            return "true" if v else "false"
        return '"%s"' % v

    def dflt(self, ty):
        """A value of `ty` no leg ever carries -- the unwrap default."""
        return {"int": "-9999", "float": "-999.5", "bool": "false",
                "cstr": '"z-dflt"', "float32": "(:: -999.5 float32)",
                "int16": "(:: -9999 int16)"}[ty]

    # -- wrappers -------------------------------------------------------------
    #
    # Each returns (typename, wrap_fn, unwrap_fn, tags) where wrap_fn/unwrap_fn
    # map an expression string to an expression string, emitting helper defns
    # into `leg`.  Wrapping and unwrapping are themselves defns, so every
    # wrapper already crosses two function boundaries typed.

    def w_none(self, leg, ty):
        return ty, (lambda e: e), (lambda e: e), {"bare"}

    def w_box(self, leg, ty, heap=False):
        tn = "FzB%d%d" % (self.i, self.n_names)
        self.n_names += 1
        leg.defs.append("(defstruct %s%s [a : %s])"
                        % (tn, " :heap" if heap else "", ty))
        w, u = self.name("bw"), self.name("bu")
        leg.defs.append("(defn %s [x : %s] : %s (%s x))" % (w, ty, tn, tn))
        # Extraction: direct field read, or through a typeclass instance --
        # the method result is where the erasure historically leaks.
        if self.rng.random() < 0.35:
            cls, meth = "FzC%d%d" % (self.i, self.n_names), self.name("m")
            leg.defs.append("(defclass %s [a] (%s [self : a] : %s))"
                            % (cls, meth, ty))
            leg.defs.append("(definstance %s [%s] (%s [self : %s] : %s (.a self)))"
                            % (cls, tn, meth, tn, ty))
            dot = "." if self.rng.random() < 0.5 else ""
            leg.defs.append("(defn %s [b : %s] : %s (%s%s b))"
                            % (u, tn, ty, dot, meth))
            tags = {"box_heap" if heap else "box", "class_extract"}
        else:
            leg.defs.append("(defn %s [b : %s] : %s (.a b))" % (u, tn, ty))
            tags = {"box_heap" if heap else "box"}
        return tn, (lambda e: "(%s %s)" % (w, e)), (lambda e: "(%s %s)" % (u, e)), tags

    def w_adt(self, leg, ty):
        tn, ctor = "FzW%d%d" % (self.i, self.n_names), None
        self.n_names += 1
        ctor = tn + "c"
        leg.defs.append("(defdata %s (%s :%s))" % (tn, ctor, ty))
        w, u = self.name("aw"), self.name("au")
        leg.defs.append("(defn %s [x : %s] : %s (%s x))" % (w, ty, tn, ctor))
        leg.defs.append("(defn %s [v : %s] : %s (match v (%s x) x))"
                        % (u, tn, ty, ctor))
        return tn, (lambda e: "(%s %s)" % (w, e)), (lambda e: "(%s %s)" % (u, e)), {"adt"}

    def w_opt(self, leg, ty):
        tn = "(Option %s)" % ty
        w, u = self.name("ow"), self.name("ou")
        dflt = self.dflt(ty)
        leg.defs.append("(defn %s [x : %s] : %s (some x))" % (w, ty, tn))
        leg.defs.append("(defn %s [o : %s] : %s (unwrap-or o %s))"
                        % (u, tn, ty, dflt))
        return tn, (lambda e: "(%s %s)" % (w, e)), (lambda e: "(%s %s)" % (u, e)), {"opt"}

    def w_res(self, leg, ty):
        tn = "(Result %s int)" % ty
        w, u = self.name("rw"), self.name("ru")
        dflt = self.dflt(ty)
        leg.defs.append("(defn %s [x : %s] : %s (ok x))" % (w, ty, tn))
        leg.defs.append("(defn %s [r : %s] : %s (if (ok? r) (ok-val r) %s))"
                        % (u, tn, ty, dflt))
        return tn, (lambda e: "(%s %s)" % (w, e)), (lambda e: "(%s %s)" % (u, e)), {"res"}

    def w_vec(self, leg, ty):
        tn = "(Vec %s)" % ty
        w, u = self.name("vw"), self.name("vu")
        leg.defs.append(
            "(defn %s [x : %s] : %s\n"
            "  (let [v (:: (vec-new) %s)]\n"
            "    (vec-push! v x)\n    v))" % (w, ty, tn, tn))
        get = "(vec-get v 0)"
        if ty != "int":
            get = "(:: %s :%s)" % (get, ty)
        leg.defs.append("(defn %s [v : %s] : %s %s)" % (u, tn, ty, get))
        return tn, (lambda e: "(%s %s)" % (w, e)), (lambda e: "(%s %s)" % (u, e)), {"vec"}

    def w_vec_box_heap(self, leg, ty):
        # Vec of :heap struct elements works; the by-value variant is the
        # known invalid-C shape (see known_bug_slug).
        bn = "FzB%d%d" % (self.i, self.n_names)
        self.n_names += 1
        leg.defs.append("(defstruct %s :heap [a : %s])" % (bn, ty))
        tn = "(Vec %s)" % bn
        w, u = self.name("hw"), self.name("hu")
        leg.defs.append(
            "(defn %s [x : %s] : %s\n"
            "  (let [v (:: (vec-new) %s)]\n"
            "    (vec-push! v (%s x))\n    v))" % (w, ty, tn, tn, bn))
        leg.defs.append(
            "(defn %s [v : %s] : %s\n"
            "  (.a (:: (vec-get v 0) %s)))" % (u, tn, ty, bn))
        return tn, (lambda e: "(%s %s)" % (w, e)), (lambda e: "(%s %s)" % (u, e)), \
            {"vec_heap_struct"}

    def w_vec_box_byvalue(self, leg, ty):
        # In the DEFAULT pool since increment 3 (2026-07-31): any-width
        # by-value struct elements ride container slots heap-boxed now.
        bn = "FzB%d%d" % (self.i, self.n_names)
        self.n_names += 1
        leg.defs.append("(defstruct %s [a : %s])" % (bn, ty))
        tn = "(Vec %s)" % bn
        w, u = self.name("yw"), self.name("yu")
        leg.defs.append(
            "(defn %s [x : %s] : %s\n"
            "  (let [v (:: (vec-new) %s)]\n"
            "    (vec-push! v (%s x))\n    v))" % (w, ty, tn, tn, bn))
        leg.defs.append(
            "(defn %s [v : %s] : %s\n"
            "  (.a (:: (vec-get v 0) %s)))" % (u, tn, ty, bn))
        return tn, (lambda e: "(%s %s)" % (w, e)), (lambda e: "(%s %s)" % (u, e)), \
            {"vec_byvalue_struct"}

    def w_opt_box(self, leg, ty):
        bn = "FzB%d%d" % (self.i, self.n_names)
        self.n_names += 1
        leg.defs.append("(defstruct %s [a : %s])" % (bn, ty))
        tn = "(Option %s)" % bn
        w, u = self.name("pw"), self.name("pu")
        dflt = self.dflt(ty)
        leg.defs.append("(defn %s [x : %s] : %s (some (%s x)))" % (w, ty, tn, bn))
        leg.defs.append("(defn %s [o : %s] : %s (.a (unwrap-or o (%s %s))))"
                        % (u, tn, ty, bn, dflt))
        return tn, (lambda e: "(%s %s)" % (w, e)), (lambda e: "(%s %s)" % (u, e)), \
            {"opt_box"}

    def w_res_box(self, leg, ty):
        bn = "FzB%d%d" % (self.i, self.n_names)
        self.n_names += 1
        leg.defs.append("(defstruct %s [a : %s])" % (bn, ty))
        tn = "(Result %s int)" % bn
        w, u = self.name("qw"), self.name("qu")
        dflt = self.dflt(ty)
        leg.defs.append("(defn %s [x : %s] : %s (ok (%s x)))" % (w, ty, tn, bn))
        leg.defs.append("(defn %s [r : %s] : %s\n"
                        "  (if (ok? r) (.a (ok-val r)) %s))"
                        % (u, tn, ty, dflt))
        return tn, (lambda e: "(%s %s)" % (w, e)), (lambda e: "(%s %s)" % (u, e)), \
            {"res_box"}

    def w_thunk(self, leg, ty):
        # defn returning `(fn [] ty)` that closes over its argument: the
        # closure-RETURN boundary.
        tn = "(fn [] %s)" % ty
        w = self.name("tw")
        leg.defs.append("(defn %s [x : %s] : %s (fn [] x))" % (w, ty, tn))
        return tn, (lambda e: "(%s %s)" % (w, e)), (lambda e: "(%s)" % e), \
            {"thunk", "closure_ret"}

    WRAPPERS = ["none", "box", "box_heap", "adt", "opt", "res", "vec",
                "vec_box_heap", "vec_box_byvalue", "opt_box", "res_box",
                "thunk"]

    def pick_wrapper(self, leg, ty):
        pool = list(self.WRAPPERS)
        which = self.rng.choice(pool)
        if which == "none":
            return self.w_none(leg, ty)
        if which == "box":
            return self.w_box(leg, ty, heap=False)
        if which == "box_heap":
            return self.w_box(leg, ty, heap=True)
        if which == "adt":
            return self.w_adt(leg, ty)
        if which == "opt":
            return self.w_opt(leg, ty)
        if which == "res":
            return self.w_res(leg, ty)
        if which == "vec":
            return self.w_vec(leg, ty)
        if which == "vec_box_heap":
            return self.w_vec_box_heap(leg, ty)
        if which == "vec_box_byvalue":
            return self.w_vec_box_byvalue(leg, ty)
        if which == "opt_box":
            return self.w_opt_box(leg, ty)
        return self.w_thunk(leg, ty) if which == "thunk" else self.w_res_box(leg, ty)

    # -- boundary crossings ----------------------------------------------------
    #
    # Each takes (leg, typename, expr) and returns (expr', tag).  The crossing
    # happens while the value is WRAPPED -- that is the whole point.

    def x_through(self, leg, tn, e):
        f = self.name("f")
        style = self.rng.random()
        if style < 0.4:
            leg.defs.append("(defn %s [v : %s] : %s v)" % (f, tn, tn))
        elif style < 0.65:
            leg.defs.append("(defn %s [v : %s] : %s (let [w v] w))" % (f, tn, tn))
        elif style < 0.85:
            c = self.rng.choice(["(= 1 1)", "(> 2 1)", "(not= 3 4)"])
            leg.defs.append("(defn %s [v : %s] : %s (if %s v v))" % (f, tn, tn, c))
        else:
            leg.defs.append("(defn %s [n : int v : %s] : %s\n"
                            "  (if (= n 0) v (%s (- n 1) v)))" % (f, tn, tn, f))
            return "(%s %d %s)" % (f, self.rng.randint(1, 3), e), "deep"
        return "(%s %s)" % (f, e), "through"

    def x_let(self, leg, tn, e):
        v = self.name("v")
        return "(let [%s %s] %s)" % (v, e, v), "let"

    def x_ascribe(self, leg, tn, e):
        return "(:: %s %s)" % (e, tn), "ascribe"

    def x_gid(self, leg, tn, e):
        f = self.name("g")
        leg.defs.append("(defn %s [A] [x : A] : A x)" % f)
        n = 2 if self.rng.random() < 0.3 else 1
        for _ in range(n):
            e = "(%s %s)" % (f, e)
        return e, "gid"

    def x_gid_let(self, leg, tn, e):
        """A generic whose body LET-BINDS another generic's result.

        Every other crossing hands a call's result straight to the next call,
        and emit re-targets a call it can see.  A `let` binding instead takes
        the call's ELABORATED type -- which for an instantiation to the
        caller's own `A` was `int`, so a float leg truncated (9.75 -> 9) and a
        by-value aggregate leg did not compile.  Both halves are fixed and in
        the default pool: this one
        (docs/archive/let-bound-generic-call-result-in-generic-truncates.md)
        and the class-method one, class_let.
        """
        f, g = self.name("g"), self.name("g")
        leg.defs.append("(defn %s [A] [x : A] : A x)" % f)
        leg.defs.append("(defn %s [A] [x : A] : A (let [y (%s x)] y))" % (g, f))
        return "(%s %s)" % (g, e), "gid_let"

    # Shapes a value typed as the generic's own `A` takes INSIDE a generic
    # body, from tests/generic-spec-matrix.py's sink table (each one proven at
    # every matrix type).  {E} is the A-typed input; each result is A.  The
    # helper names are substituted per leg.
    GBODY_SINKS = {
        "let":     "(let [{L} {E}] {L})",
        "ident":   "({ID} {E})",
        "if":      "(if ({T}) {E} {E})",
        "box":     "(.val ({BOX} {E}))",
        "some":    "(match (some {E}) (Some {L}) {L} (None) {E})",
        "vec":     "(let [{L} (vec-new)] (vec-push! {L} {E}) (vec-get {L} 0))",
        "pair":    "(pair-fst (pair {E} 0))",
        "lambda":  "((fn [{L} : A] : A {L}) {E})",
        "capture": "(let [{L} {E}] ((fn [] {L})))",
        "map":     "(map-get (map-assoc (map-new) 1 {E}) 1)",
        "gen":     "(gen-unwrap (gen-next (gen [] (yield {E}))))",
        "adt":     "(match ({WC} {E}) ({WC} {L}) {L})",
        "fnret":   "(({MK} {E}))",
        "hof":     "({APP} (fn [{L} : A] : A {L}) {E})",
    }

    def x_gbody(self, leg, tn, e):
        """The value crosses a generic `[A] x:A -> A` whose BODY moves it:
        through a vector, a map, a generic struct field, an Option match, a
        lambda, a returned closure, a generator... 1-3 of them, composed.

        Every other generic crossing hands `x` straight back.  The carrier
        crossings of 2026-09-30 (docs/archive/generic-spec-carrier-crossings.md)
        all lived in a generic body that did something with its `A`: nine
        independent miscompiles, none of which any fuzzer could generate.
        tests/generic-spec-matrix.py enumerates each shape once; this composes
        them, at the fuzzer's wrapper types, along random chains.
        """
        g = self.name("gb")
        names = {"ID": self.name("gbid"), "T": self.name("gbt"),
                 "BOX": self.name("GbBox"), "WC": self.name("GbWc"),
                 "W": self.name("GbW"), "MK": self.name("gbmk"),
                 "APP": self.name("gbapp")}
        leg.defs.append("(defn %s [A] [y : A] : A y)" % names["ID"])
        leg.defs.append("(defn %s [] : bool true)" % names["T"])
        leg.defs.append("(defstruct %s [A] [val : A])" % names["BOX"])
        leg.defs.append("(defdata %s [A] (%s A))" % (names["W"], names["WC"]))
        leg.defs.append("(defn %s [B] [v : B] : (fn [] B) (fn [] v))"
                        % names["MK"])
        leg.defs.append("(defn %s [B] [f : (fn [B] B) v : B] : B (f v))"
                        % names["APP"])
        body = "x"
        chain = []
        for _ in range(self.rng.randint(1, 3)):
            pool = list(self.GBODY_SINKS)
            # A generator inside a generator, and a yield inside a match arm,
            # are v1 rejections (the matrix's EXCLUDE list).
            if "gen" in chain:
                pool = [k for k in pool if k not in ("gen", "some")]
            k = self.rng.choice(pool)
            chain.append(k)
            loc = self.name("gbl")
            tmpl = self.GBODY_SINKS[k]
            for key, val in names.items():
                tmpl = tmpl.replace("{%s}" % key, val)
            body = tmpl.replace("{L}", loc).replace("{E}", body)
        leg.defs.append("(defn %s [A] [x : A] : A\n  %s)" % (g, body))
        return "(%s %s)" % (g, e), "gbody"

    def x_rank2_class(self, leg, tn, e):
        """The value crosses a RANK-2, dictionary-passing call: a constrained
        generic passed where `(forall [a] [(C a)] (-> a a))` is expected, whose
        body dispatches a class method through the runtime dictionary.

        No crossing had ever generated one.  That path carried three silent
        float wrong answers at once (docs/archive/
        dict-classvar-float-param-value-converted.md and its result twin): the
        carrier call value-converted the argument, the slot took a `double`
        through an int64 cast, and the result came back through
        `(int64_t)(intptr_t)` of a double.  Instance heads need a plain type
        name, so this is offered for those only.
        """
        cls, m = self.name("FzR"), self.name("rm")
        impl, use = self.name("ri"), self.name("ru")
        # The method's shape: which positions hold the class variable.  Each
        # is an identity on `x`, so the leg's oracle is unchanged.
        shape = self.rng.choice(["id", "id", "two", "extra", "opt"])
        sig, body, call = {
            "id":    ("[x : a] : a", "x", "(%s x)" % m),
            "two":   ("[x : a y : a] : a", "x", "(%s x x)" % m),
            "extra": ("[x : a n : int] : a", "x", "(%s x 3)" % m),
            "opt":   ("[x : a] : (Option a)", "(some x)",
                      "(match (%s x) (Some q) q (None) x)" % m),
        }[shape]
        isig = sig.replace(": a", ": %s" % tn).replace("(Option a)",
                                                       "(Option %s)" % tn)
        leg.defs.append("(defclass %s [a] (%s %s))" % (cls, m, sig))
        # A second instance declared BEFORE the leg's own changes which
        # instance the dispatch site's cast follows (the "representative").
        other = self.rng.choice([None, None, "int", "cstr", "float"])
        if other and other != tn:
            osig = sig.replace(": a", ": %s" % other).replace(
                "(Option a)", "(Option %s)" % other)
            leg.defs.append("(definstance %s [%s] (%s %s %s))"
                            % (cls, other, m, osig, body))
        leg.defs.append("(definstance %s [%s] (%s %s %s))"
                        % (cls, tn, m, isig, body))
        leg.defs.append("(defn %s [a] [(%s a)] [x : a] : a %s)" % (impl, cls, call))
        route = self.rng.choice(["rank2", "rank2", "direct", "generic"])
        if route == "direct":
            return "(%s %s)" % (impl, e), "rank2_class"
        if route == "generic":
            leg.defs.append("(defn %s [a] [(%s a)] [v : a] : a (%s v))"
                            % (use, cls, impl))
            return "(%s %s)" % (use, e), "rank2_class"
        leg.defs.append("(defn %s [l (forall [a] [(%s a)] (-> a a)) v : %s] : %s (l v))"
                        % (use, cls, tn, tn))
        return "(%s %s %s)" % (use, impl, e), "rank2_class"

    def x_fat_hof(self, leg, tn, e):
        f = self.name("h")
        leg.defs.append("(defn %s [^fat f : (fn [] %s)] : %s (f))" % (f, tn, tn))
        v = self.name("c")
        # let-bind first so the lambda genuinely captures a local.
        return "(let [%s %s] (%s (fn [] %s)))" % (v, e, f, v), "fat_hof"

    def x_thin_hof(self, leg, tn, e):
        f = self.name("t")
        leg.defs.append("(defn %s [f : (fn [] %s)] : %s (f))" % (f, tn, tn))
        v = self.name("c")
        return "(let [%s %s] (%s (fn [] %s)))" % (v, e, f, v), "thin_hof"

    def x_fn_field(self, leg, tn, e):
        """The value passes through a function stored in a TYPED fn field.

        No crossing had ever stored a function in a struct field.  That is
        where a bare `fn` field returned a float's BITS
        (4619004367821864960 for 6.5) and an effectful typed field aborted
        -- both 2026-09-30.  Half the time the stored function is a
        capturing lambda, which a typed field boxes fat.
        """
        st, f = self.name("FzFF"), self.name("ff")
        leg.defs.append("(defstruct %s :copy [app : (fn [%s] %s)])" % (st, tn, tn))
        if self.rng.random() < 0.5:
            leg.defs.append("(defn %s [v : %s] : %s v)" % (f, tn, tn))
            fnv = f
        else:
            k = self.name("fk")
            leg.defs.append("(def %s 1)" % k)
            fnv = "(fn [v : %s] : %s (if (= %s 1) v v))" % (tn, tn, k)
        return "(.app (make-struct %s %s) %s)" % (st, fnv, e), "fn_field"

    def x_fn_field_eff(self, leg, tn, e):
        """The same through an EFFECTFUL typed fn field: the stored function
        performs, and the handler resumes with the payload.  The E2a call
        into it spelled every argument `int64_t` (a float became its
        truncation in the wrong register) and looked the registry up by the
        field's fat box."""
        ef, st, f = self.name("FzEf"), self.name("FzFE"), self.name("fe")
        leg.defs.append("(defeffect %s [x :%s] :%s)" % (ef, tn, tn))
        leg.defs.append("(defstruct %s :copy [run : (fn [%s] %s) #fx{%s}])"
                        % (st, tn, tn, ef))
        leg.defs.append("(defn %s [v : %s] : %s (perform (%s v)))" % (f, tn, tn, ef))
        return ("(handle (.run (make-struct %s %s) %s)\n    (%s [x] k) (resume k x))"
                % (st, f, e, ef)), "fn_field_eff"

    def x_tyvar_run(self, leg, tn, e):
        # KNOWN shape (crashes with a capturing closure); --emit-known only.
        f = self.name("r")
        leg.defs.append("(defn %s [R] [body : (fn [] R)] : R (body))" % f)
        v = self.name("c")
        return "(let [%s %s] (%s (fn [] %s)))" % (v, e, f, v), "tyvar_run"

    def x_class_thru(self, leg, tn, e):
        # Typeclass pass-through: instance heads must be plain names, so this
        # crossing only applies to scalar and struct/ADT wrappers.
        cls, meth = "FzT%d%d" % (self.i, self.n_names), self.name("p")
        leg.defs.append("(defclass %s [a] (%s [self : a] : a))" % (cls, meth))
        leg.defs.append("(definstance %s [%s] (%s [self : %s] : %s self))"
                        % (cls, tn, meth, tn, tn))
        dot = "." if self.rng.random() < 0.5 else ""
        return "(%s%s %s)" % (dot, meth, e), "class_thru"

    def x_class_nested(self, leg, tn, e):
        """Nested same-class dispatch inside a constrained generic, with the
        leg's own type NOT the first-declared instance.

        Closes three gaps at once (type-confusion-detection-plan 3.1): the
        class carries TWO instances so "first declared" and "correct" can
        diverge; the method is BINARY over its own class type so one call can
        feed another; and the generic body NESTS the two calls.  All three are
        required by
        docs/archive/nested-class-method-call-picks-the-first-instance.md.

        The method projects its first argument, so nesting is an identity and
        the leg's expected value is unchanged -- resolving to the wrong
        instance therefore surfaces as BUG_wrong_output (a wrong ANSWER), not
        merely as invalid C.
        """
        n = self.n_names
        self.n_names += 1
        cls, meth = "FzN%d%d" % (self.i, n), "fzn%d%d" % (self.i, n)
        gen = "fzng%d%d" % (self.i, n)
        # The decoy must be declared FIRST and must not be the leg's own type.
        decoy = "float" if tn == "int" else "int"
        leg.defs.append("(defclass %s [a] (%s [x : a y : a] : a))" % (cls, meth))
        leg.defs.append("(definstance %s [%s] (%s [x y] x))" % (cls, decoy, meth))
        leg.defs.append("(definstance %s [%s] (%s [x y] x))" % (cls, tn, meth))
        leg.defs.append("(defn %s [^%s A] [x : A] : A (%s (%s x x) x))"
                        % (gen, cls, meth, meth))
        return "(%s %s)" % (gen, e), "class_nested"

    def x_class_let(self, leg, tn, e):
        """class_nested's shape with the inner result LET-BOUND.

        The decoy instance is declared first, as in class_nested, because the
        call on an A-typed receiver is elaborated against that carrier
        representative.  Bound by a `let`, the representative's result type
        used to become the binding's type: the float instance's double was
        stored into an int64 and the outer call dispatched on `int`.
        docs/archive/let-bound-class-method-result-in-constrained-generic-
        truncates.md.  The method projects its first argument, so the
        crossing is an identity and a wrong instance is a wrong ANSWER.
        """
        n = self.n_names
        self.n_names += 1
        cls, meth = "FzL%d%d" % (self.i, n), "fzl%d%d" % (self.i, n)
        gen = "fzlg%d%d" % (self.i, n)
        decoy = "float" if tn == "int" else "int"
        leg.defs.append("(defclass %s [a] (%s [x : a y : a] : a))" % (cls, meth))
        leg.defs.append("(definstance %s [%s] (%s [x y] x))" % (cls, decoy, meth))
        leg.defs.append("(definstance %s [%s] (%s [x y] x))" % (cls, tn, meth))
        leg.defs.append("(defn %s [^%s A] [x : A] : A (let [y (%s x x)] (%s y x)))"
                        % (gen, cls, meth, meth))
        return "(%s %s)" % (gen, e), "class_let"

    def x_class_nullary_newtype(self, leg, tn, e):
        """A NULLARY class method whose instances are over a `defopaque`
        newtype -- docs/archive/nullary-class-method-unresolvable-over-newtype-tyvar.md.

        Both halves are required: every other generated method takes `self`,
        and every other instance head is a plain struct/scalar name.  Applied
        to bare `int` legs only, since the newtype wraps `:int`; routing the
        value through it and back keeps the crossing an identity.
        """
        n = self.n_names
        self.n_names += 1
        nt, cls = "FzW%d%d" % (self.i, n), "FzV%d%d" % (self.i, n)
        zero, comb = "fzvz%d%d" % (self.i, n), "fzvc%d%d" % (self.i, n)
        gen, thru = "fzvg%d%d" % (self.i, n), "fzvt%d%d" % (self.i, n)
        leg.defs.append("(defopaque %s :int)" % nt)
        leg.defs.append("(defclass %s [a] (%s [] : a) (%s [x : a y : a] : a))"
                        % (cls, zero, comb))
        leg.defs.append("(definstance %s [%s] (%s [] (:: 0 %s)) (%s [x y] x))"
                        % (cls, nt, zero, nt, comb))
        leg.defs.append("(defn %s [^%s A] [x : A] : A (%s x (%s)))"
                        % (gen, cls, comb, zero))
        leg.defs.append("(defn %s [v : int] : int (:: (%s (:: v %s)) int))"
                        % (thru, gen, nt))
        return "(%s %s)" % (thru, e), "class_nullary_newtype"

    def crossings_for(self, tn, tags):
        # Fn-typed VALUES are fat-normalized across returns/let/ascribe and
        # HOF hops as of fn-value-fat-normalization stage 2 (2026-07-30) --
        # thunk legs take the full crossing pool minus the class/gid pair
        # (instance heads need plain names; gid over a thunk is fine and
        # included).
        if "thunk" in tags:
            return [self.x_through, self.x_let, self.x_ascribe, self.x_gid,
                    self.x_fat_hof, self.x_thin_hof, self.x_gbody]
        xs = [self.x_through, self.x_let, self.x_ascribe, self.x_gid,
              self.x_fat_hof, self.x_thin_hof, self.x_gid_let,
              self.x_fn_field, self.x_gbody]
        if tn in ("int", "float", "bool", "cstr"):
            xs.append(self.x_fn_field_eff)
        # Thin HOF over every wrapper: scalars ride the poly carrier;
        # concrete by-value/heap signatures are fat-normalized as of
        # fn-value-fat-normalization stage 1 (2026-07-30).
        # Instance heads: plain type names only.
        if not tn.startswith("("):
            xs.append(self.x_class_thru)
            xs.append(self.x_rank2_class)
            # Both F1 shapes are in the default pool: their reports were
            # resolved and archived (2026-09-11).  class_nested is also the
            # only shape that gives a class a SECOND instance declared before
            # the leg's own, which is what found
            # constrained-generic-float-result-into-generic-value-converts.
            xs.append(self.x_class_nested)
            xs.append(self.x_class_let)
            if tn == "int":
                xs.append(self.x_class_nullary_newtype)
        if self.emit_known:
            xs.append(self.x_tyvar_run)
        if FORCE_CROSSING:
            # --crossing: concentrate a run on one crossing where it applies.
            only = [x for x in xs if x.__name__ == "x_" + FORCE_CROSSING]
            return only or xs
        return xs

    # -- int mutation steps (bare int legs get arithmetic through defns) -------

    def int_mut(self, leg, e, v):
        f = self.name("i")
        k = self.rng.randint(1, 9)
        op = self.rng.choice(["+", "-", "*"])
        if op == "*":
            k = self.rng.choice([2, 3])
        leg.defs.append("(defn %s [x : int] : int (%s x %s))" % (f, op, k))
        nv = {"+": v + k, "-": v - k, "*": v * k}[op]
        return "(%s %s)" % (f, e), nv

    # -- one leg ----------------------------------------------------------------

    def leg(self):
        leg = Leg()
        ty, val = self.pick_scalar(narrow=True)
        tn, wrap, unwrap, tags = self.pick_wrapper(leg, ty)
        leg.tags |= tags
        leg.tags.add("scalar_" + ty)

        e = self.lit(ty, val)
        if ty == "int" and "bare" in tags and self.rng.random() < 0.7:
            for _ in range(self.rng.randint(1, 2)):
                e, val = self.int_mut(leg, e, val)

        e = wrap(e)
        pool = self.crossings_for(tn, tags)
        if pool:
            for _ in range(self.rng.randint(1, 3)):
                x = self.rng.choice(pool)
                e, tag = x(leg, tn, e)
                leg.tags.add(tag)
        e = unwrap(e)

        if ty in ("float", "float32"):
            leg.body.append("(println (= %s %s))" % (e, self.lit(ty, val)))
            leg.expected.append("true")
        elif ty == "bool":
            leg.body.append("(println %s)" % e)
            leg.expected.append("true" if val else "false")
        elif ty == "cstr":
            leg.body.append("(println %s)" % e)
            leg.expected.append(val)
        else:
            leg.body.append("(println %s)" % e)
            leg.expected.append(str(val))
        return leg

    def leg_res_bind(self):
        """Monad bind over Result through a typed defn boundary.  Fixed by
        consolidation increment 2 (was result-monad-bind-typed-boundary,
        archived); generated in the default rotation since."""
        leg = Leg()
        f, g = self.name("bf"), self.name("bg")
        n = self.rng.randint(1, 9)
        leg.defs.append("(defn %s [n : int] : (Result int int)\n"
                        "  (if (= n 0) (err 7) (ok n)))" % f)
        leg.defs.append("(defn %s [n : int] : (Result int int)\n"
                        "  (bind (%s n) (fn [x] (ok (* x 2)))))" % (g, f))
        leg.body.append("(let [r (%s %d)]\n"
                        "    (println (if (ok? r) (ok-val r) -1)))" % (g, n))
        leg.expected.append(str(n * 2))
        leg.tags = {"res_bind", "scalar_int"}
        return leg

    # -- runtime seams ---------------------------------------------------------
    #
    # Every CROSSING above is one where the compiler owns both ends: it emits
    # the producer and the consumer in the same pass and the value never leaves
    # its static type.  A SEAM is the other thing -- the value is PARKED in a
    # runtime data structure (a channel, a protocol router, a generator frame,
    # a future slot, a transactional cell) by one piece of emitted code and read
    # back later by another.  The runtime structure's slot has ONE C type, so
    # the payload is cast into it on the way in and out, and a plain C cast of a
    # `double` is a value conversion that TRUNCATES.
    #
    # This axis is why the seam class survived four fuzzers that all fuzz
    # floats: floats were routed across every compiler-owned boundary and
    # across no runtime-owned one.
    #
    # Each emitter returns a complete Leg.  The oracle is PRINT-THE-VALUE, not
    # the `(= e lit)` comparison the value legs use for floats, and that choice
    # is load-bearing: a seam that erases its payload to int makes `(= v 7.25)`
    # a TUR-E0042 reject, and a reject is not a failure -- the bug would be
    # generated, rejected, and counted as "the generator made an illegal
    # program".  Printing the value turns the same defect into a wrong ANSWER.
    # Every literal in FLOAT_LITS round-trips through `println` exactly as
    # written, so the predicted string is the literal itself.

    PTHREAD_PEER = (
        ";; Shared session peer.  The `ptr<void>` here is the stand-in CLAUDE.md\n"
        ";; forbids; it is reproduced because every compiled session fixture and\n"
        ";; every session-types-guide example hand-rolls exactly this, and the\n"
        ";; seam must be exercised the way callers actually reach it.\n"
        "(defn fzspawn [f : ptr<void>] : ptr<void>\n"
        "  ```c\n"
        "  pthread_t *tid = (pthread_t *)malloc(sizeof(pthread_t));\n"
        "  pthread_create(tid, NULL, tur_session_thread_wrapper, f);\n"
        "  return (void *)tid;\n"
        "  ```)\n"
        "(defn fzjoin [t : ptr<void>] : int\n"
        "  ```c\n"
        "  pthread_join(*(pthread_t *)t, NULL);\n"
        "  free(t);\n"
        "  return 0;\n"
        "  ```)")

    def payload(self, leg, kind):
        """Pick a payload and return (typename, mk_expr, read_fn, expected).

        kind is "scalar" (the value rides the seam at its own type) or "box" (a
        by-value struct rides it -- the row that has no int/pointer conversion
        in C at all).
        """
        ty, val = self.pick_scalar()
        lit = self.lit(ty, val)
        printed = {"int": lambda: str(val),
                   "float": lambda: lit,
                   "bool": lambda: "true" if val else "false",
                   "cstr": lambda: val}[ty]()
        leg.tags.add("scalar_" + ty)
        if kind == "scalar":
            leg.tags.add("payload_scalar")
            return ty, lit, (lambda e: e), printed
        bn = "FzB%d%d" % (self.i, self.n_names)
        self.n_names += 1
        leg.defs.append("(defstruct %s [a : %s])" % (bn, ty))
        leg.tags.add("payload_box")
        return bn, "(%s %s)" % (bn, lit), (lambda e: "(.a %s)" % e), printed

    def _pk(self):
        """Payload kind for a seam: sampled, or pinned by --seam-matrix."""
        return self.force_payload or self.rng.choice(["scalar", "box"])

    def seam_session(self, leg):
        """Binary session: `send` parks the payload in a TurChannel, the peer
        `recv`s it.  docs/reported/session-payloads-are-int64-only.md."""
        tn, mk, read, exp = self.payload(leg, self._pk())
        leg.prelude.append(self.PTHREAD_PEER)
        s, r = self.name("s"), self.name("r")
        s2, r2, v, k = self.name("s"), self.name("r"), self.name("v"), self.name("k")
        leg.body.append(
            "(let [[%s %s] (make-session (Send %s Close))]\n"
            "    (let [%s (fzspawn (fn [] (let [[%s %s] (recv %s)]\n"
            "                               (println %s)\n"
            "                               (close %s))))]\n"
            "      (let [%s (send %s %s)] (close %s) (fzjoin %s))))"
            % (s, r, tn, k, v, r2, r, read(v), r2, s2, s, mk, s2, k))
        leg.expected.append(exp)
        return leg

    def seam_router(self, leg):
        """Multi-party session: `send-to` parks the payload in the protocol
        router.  A DIFFERENT template from the binary seam above
        (elab_global.c's tur_router_send, not elab_sessions.c's
        tur_session_send), so fixing one does not fix the other."""
        tn, mk, read, exp = self.payload(leg, self._pk())
        leg.prelude.append(self.PTHREAD_PEER)
        n = self.n_names
        self.n_names += 1
        proto = "FzP%d%d" % (self.i, n)
        ra, rb = "fzra%d%d" % (self.i, n), "fzrb%d%d" % (self.i, n)
        a, b, k, v = self.name("a"), self.name("b"), self.name("k"), self.name("v")
        ch = self.name("ch")
        leg.defs.append("(defprotocol %s [A B]\n  (-> A B %s))" % (proto, tn))
        leg.defs.append("(defn %s [^linear %s :(Role %s A)] : nil\n"
                        "  (let [%s (send-to %s B %s)] (close %s)))"
                        % (ra, ch, proto, ch, ch, mk, ch))
        leg.defs.append("(defn %s [^linear %s :(Role %s B)] : nil\n"
                        "  (let [[%s %s] (recv-from %s A)]\n"
                        "    (println %s)\n    (close %s)))"
                        % (rb, ch, proto, v, ch, ch, read(v), ch))
        leg.body.append(
            "(let [[%s %s] (make-protocol %s)]\n"
            "    (let [%s (fzspawn (fn [] (%s %s)))] (%s %s) (fzjoin %s)))"
            % (a, b, proto, k, ra, a, rb, b, k))
        leg.expected.append(exp)
        return leg

    def seam_generator(self, leg):
        """`yield` parks the payload in the generator frame; `gen-next` reads it
        back.  docs/reported/generator-yield-payload-is-int64-only.md."""
        tn, mk, read, exp = self.payload(leg, self._pk())
        leg.prelude.append('(load "stdlib/gen.tur")')
        g, v = self.name("g"), self.name("v")
        leg.body.append(
            "(let [%s (gen [] (yield %s))]\n"
            "    (let [%s (gen-next %s)]\n"
            "      (when (gen-some? %s) (println %s))))"
            % (g, mk, v, g, v, read("(gen-unwrap %s)" % v)))
        leg.expected.append(exp)
        return leg

    def seam_await(self, leg):
        """`async` parks the thunk's result in a future slot; `await` reads it
        back.  docs/reported/async-await-payload-is-int64-only.md."""
        tn, mk, read, exp = self.payload(leg, self._pk())
        f, fut = self.name("fzc"), self.name("fut")
        leg.defs.append("(defn %s [] : %s %s)" % (f, tn, mk))
        leg.body.append("(let [%s (async %s)] (println %s))"
                        % (fut, f, read("(await %s)" % fut)))
        leg.expected.append(exp)
        return leg

    # -- seams that are already correct: the axis's positive controls ----------
    #
    # Without these a run where every seam fails cannot distinguish "the seams
    # are broken" from "the seam emitter is broken".  All three carry a payload
    # across a runtime slot and all three get it right, by three different
    # mechanisms -- so they also document what a fix looks like.

    def seam_effects(self, leg):
        """perform/resume across the fiber slot.  CORRECT since 2026-07-12: the
        direct/fiber path stores the resume value through a
        `union { double d; int64_t i; }` bit-reinterpret instead of a cast
        (docs/archive/fiber-effect-float-result-truncated.md).  This is fix
        direction 1 of the session report, already shipped elsewhere."""
        tn, mk, read, exp = self.payload(leg, "scalar")
        n = self.n_names
        self.n_names += 1
        eff, run = "FzS%d%d" % (self.i, n), "fzer%d%d" % (self.i, n)
        leg.defs.append("(defeffect %s [] :%s)" % (eff, tn))
        leg.defs.append("(defn %s [] : %s\n"
                        "  (handle (perform (%s)) (%s [] k) (resume k %s)))"
                        % (run, tn, eff, eff, mk))
        leg.body.append("(println %s)" % read("(%s)" % run))
        leg.expected.append(exp)
        return leg

    def seam_any(self, leg):
        """`any` round-trip.  CORRECT: `any` does not ride the int64 slot at
        all -- it boxes the payload with a type tag and `cast` checks the tag
        (tests/fixtures/any-box-struct/ says so in its header).  This is the
        design the broken seams want."""
        tn, mk, read, exp = self.payload(leg, self._pk())
        mkf = self.name("fzm")
        leg.defs.append("(defn %s [] : any %s)" % (mkf, mk))
        leg.body.append("(println %s)" % read("(cast (%s) %s)" % (mkf, tn)))
        leg.expected.append(exp)
        return leg

    def seam_tvar(self, leg):
        """STM transactional cell.  `tvar/read` hands back `ptr<void>` (the
        payload's type is GONE -- a typing hole), so the value is read back
        through an ascription to the payload type and PRINTED, after the CAS
        round trip.

        This seam was listed as a correct positive control, and asserted only
        the CAS.  That hid tvar-float-payload-value-converted: every payload
        crossed into the slot through `(void*)(intptr_t)v`, so a float was
        stored as its truncation -- consistently on every side, so the CAS
        still matched.  `(tvar/cas t 7.4 ...)` against 7.1 succeeded, and the
        read-back printed 3.45846e-323.  The emitted-C lint is what caught
        it; the printed read-back makes the same defect a wrong ANSWER."""
        tn, mk, read, exp = self.payload(leg, "scalar")
        tv = self.name("tv")
        leg.body.append("(let [%s (tvar/new %s)]\n"
                        "    (println (atomically (stm (tvar/write %s %s)\n"
                        "                              (tvar/cas %s %s %s))))\n"
                        "    (println (:: (atomically (stm (tvar/read %s))) %s)))"
                        % (tv, mk, tv, mk, tv, mk, mk, tv, tn))
        leg.expected.append("true")
        leg.expected.append(exp)
        leg.tags.add("oracle_roundtrip")
        return leg

    # name -> (emitter, runs_under_interpret)
    #
    # interp=False is a measured property, not caution: session/router need the
    # inline-C pthread peer and `--interpret` refuses inline-C outright, and the
    # generator seam trips a UBSan misaligned-load on TuriGen under the
    # tree-walker (a separate defect from the payload one).
    SEAMS = {
        "session":   ("seam_session",   False),
        "router":    ("seam_router",    False),
        "generator": ("seam_generator", False),
        "await":     ("seam_await",     True),
        "effects":   ("seam_effects",   True),
        "any":       ("seam_any",       True),
        "tvar":      ("seam_tvar",      True),
    }

    def leg_seam(self, only=None):
        leg = Leg()
        which = only or self.rng.choice(sorted(self.SEAMS))
        meth, _interp = self.SEAMS[which]
        leg.seam = which
        leg.tags.add("seam_" + which)
        getattr(self, meth)(leg)
        return leg


def gen_program(rng, n_legs, emit_known):
    legs = []
    for i in range(n_legs):
        g = Gen(rng, i, emit_known)
        # Bind legs run in the default rotation since the increment-2 fix.
        if rng.random() < 0.10:
            legs.append(g.leg_res_bind())
        else:
            legs.append(g.leg())
    return legs


def gen_seam_program(rng, emit_known, only=None):
    """One seam leg, alone in its program.

    Seam legs are NOT mixed with value legs.  Two reasons, both load-bearing:
    a session/router leg blocks on a rendezvous and joins a peer thread, so an
    unrelated leg's failure in the same program could be reported as a hang;
    and keeping it alone makes attribution exact without relying on the
    bisector.  The cost is that a seam case exercises one shape, which is why
    the seam pool is sampled per case rather than per leg.
    """
    g = Gen(rng, 0, emit_known)
    return [g.leg_seam(only)]


def assemble(legs):
    prelude, defs, body, expected = [], [], [], []
    for leg in legs:
        # Preludes are shared helpers (the pthread peer, a stdlib load): emit
        # each distinct one once, or the second seam leg redefines it.
        for p in leg.prelude:
            if p not in prelude:
                prelude.append(p)
        defs += leg.defs
        body += leg.body
        expected += leg.expected
    src = "\n\n".join(prelude + defs)
    src += "\n\n(defn main [] : int\n  " + "\n  ".join(body) + "\n  0)\n"
    return src, "\n".join(expected) + "\n"


# ---------------------------------------------------------------------------
# Running one case
# ---------------------------------------------------------------------------

# The shape-independent value-conversion check (tests/fconv_lint.py): a clean,
# correct run whose emitted C still converts a float<->int VALUE where no `as`
# asked for it is a finding the output oracle cannot see -- the value may
# happen to round-trip (7.0) or the conversion may sit on a path this program
# does not print.  The corpus is at zero, so this class fails the run.
_FCONV_CLANG = fconv_lint.find_clang()


def value_conversions(tur, path, env):
    if not _FCONV_CLANG or os.environ.get("TUR_FUZZ_FCONV", "1") == "0":
        return []
    r = subprocess.run([tur, "emit-c", path], capture_output=True, text=True,
                       timeout=TIMEOUT, cwd=REPO, env=env)
    if r.returncode != 0 or fconv_lint.MARK not in r.stdout:
        return []
    cpath = path + ".fconv.c"
    with open(cpath, "w") as f:
        f.write(r.stdout)
    try:
        return fconv_lint.lint_c(cpath, _FCONV_CLANG)
    finally:
        try:
            os.unlink(cpath)
        except OSError:
            pass


class Outcome:
    def __init__(self, kind, stdout="", stderr=""):
        self.kind = kind        # clean/crash/invalid_c/link/reject/timeout/other
        self.stdout = stdout
        self.stderr = stderr


def run_case(tur, path, src):
    with open(path, "w") as f:
        f.write(src)
    env = dict(os.environ)
    # Drop any ambient TUR_STDLIB_DIR so the compiler under test resolves the
    # stdlib that ships beside it (its walk-up probe), not one belonging to
    # some other turmeric install.
    #
    # This is not hypothetical, and it is expensive to diagnose. A version
    # manager whose `python3` is a shim (mise, asdf) re-exports the tool env
    # *inside* this process, so the variable can point at an unrelated
    # installed release even when the invoking shell has no such variable --
    # which means `unset` in the shell wrapper does not reach it, and a saved
    # case rerun by hand does not reproduce. The compiler's own guard does not
    # catch it either: it only checks that macros.tur is readable there, which
    # a stale-but-intact install passes.
    #
    # The symptom is a wall of Result/Option shape failures -- an old stdlib's
    # `(defstruct Result [A B] (is-ok :bool) ...)` compiled against a current
    # compiler's box layout gives `no member named 'is_ok' in
    # 'tur_result_box_t'`, or silently wrong output. It reads as a codegen bug
    # in the compiler and is not one. See
    # docs/archive/type-fuzz-src-red-on-clang-21.md.
    env.pop("TUR_STDLIB_DIR", None)
    env["ASAN_OPTIONS"] = env.get("ASAN_OPTIONS", "") or "detect_leaks=0"
    # Arm clang's function-pointer detector (tests/fuzz_arm.py): it sees a
    # mismatched indirect call whatever shape produced it, which no shape
    # list can promise.
    env, _ = fuzz_arm.armed_env(env)
    try:
        chk = subprocess.run([tur, "check", path], capture_output=True,
                             text=True, timeout=TIMEOUT, cwd=REPO, env=env)
    except subprocess.TimeoutExpired:
        return Outcome("timeout")
    if chk.returncode != 0:
        return Outcome("reject", chk.stdout, chk.stderr)
    try:
        p = subprocess.run([tur, "run", path], capture_output=True,
                           text=True, timeout=TIMEOUT, cwd=REPO, env=env)
    except subprocess.TimeoutExpired:
        return Outcome("timeout")
    if p.returncode == 0:
        conv = value_conversions(tur, path, env)
        if conv:
            ln, kind, how, text = conv[0]
            return Outcome("value_conv", p.stdout,
                           "emitted C line %d: %s %s value conversion: %s"
                           % (ln, how, kind, text))
        return Outcome("clean", p.stdout, p.stderr)
    if fuzz_arm.is_fnsan_trap(p.returncode):
        return Outcome("fnptr_trap", p.stdout, p.stderr)
    if p.returncode in (134, 138, 139) or p.returncode < 0:
        return Outcome("crash", p.stdout, p.stderr)
    blob = p.stderr + p.stdout
    if "cc invocation failed" in blob:
        if "undefined" in blob.lower() or "ld:" in blob:
            return Outcome("link", p.stdout, p.stderr)
        return Outcome("invalid_c", p.stdout, p.stderr)
    return Outcome("other", p.stdout, p.stderr)


BUG_OF = {"crash": "BUG_crash", "invalid_c": "BUG_invalid_c",
          "link": "BUG_link", "other": "BUG_toolchain_other",
          "fnptr_trap": fuzz_arm.TRAP_CLASS,
          "value_conv": "BUG_value_conversion"}

SEAM_BUG_OF = {"crash": "BUG_seam_crash", "invalid_c": "BUG_seam_invalid_c",
               "link": "BUG_seam_link", "other": "BUG_toolchain_other",
               "fnptr_trap": fuzz_arm.TRAP_CLASS,
               "value_conv": "BUG_value_conversion"}


def classify(out, expected, is_seam=False):
    if out.kind == "timeout":
        # A seam hang is a genuine defect class -- a compiled session rendezvous
        # blocks the only OS thread, which is a filed report -- but a timeout is
        # also exactly what CPU contention looks like, and CLAUDE.md is emphatic
        # that overlapping runs manufacture them.  Report it as its own class
        # without turning the run red on a timing signal.
        return "SEAM_HANG" if is_seam else "skip_timeout"
    if out.kind == "reject":
        # For a value leg a reject means the GENERATOR emitted an illegal
        # program, so it is noise.  For a seam it means the CHECKER refused this
        # payload type -- which is the honest outcome, and is fix direction 4 of
        # session-payloads-are-int64-only: whatever a seam cannot carry must be
        # rejected in the elaborator rather than passed through to cc.  It gets
        # its own class so it is counted and printed instead of being filed
        # under "the generator made an illegal program", which is precisely how
        # this defect class stayed invisible.
        return "SEAM_REJECT" if is_seam else "GEN_REJECT"
    if out.kind == "clean":
        if out.stdout == expected:
            return "ok"
        return "BUG_seam_wrong_output" if is_seam else "BUG_wrong_output"
    return (SEAM_BUG_OF if is_seam else BUG_OF)[out.kind]


def one_case(tur, workdir, idx, seed, max_legs, emit_known,
             seam_frac=0.0, seam_only=None):
    rng = random.Random(seed)
    if seam_only is not None or rng.random() < seam_frac:
        legs = gen_seam_program(rng, emit_known, seam_only)
    else:
        legs = gen_program(rng, rng.randint(1, max_legs), emit_known)
    is_seam = any(leg.seam for leg in legs)
    src, expected = assemble(legs)
    path = os.path.join(workdir, "c%06d.tur" % idx)
    out = run_case(tur, path, src)
    kind = classify(out, expected, is_seam)

    detail = None
    if kind.startswith("BUG") or kind in ("GEN_REJECT", "SEAM_REJECT",
                                          "FNPTR_TRAP"):
        # Bisect: which leg(s) fail alone?
        failing = []
        for j, leg in enumerate(legs):
            s2, e2 = assemble([leg])
            p2 = os.path.join(workdir, "c%06d_leg%d.tur" % (idx, j))
            o2 = run_case(tur, p2, s2)
            k2 = classify(o2, e2, leg.seam is not None)
            if k2 != "ok":
                failing.append((j, k2, leg, s2, e2, o2))
            try:
                os.unlink(p2)
            except OSError:
                pass
        # KNOWN downgrade: every failing leg matches an open report.
        slugs = set()
        for _j, _k, leg, _s, _e, _o in failing:
            slug = known_bug_slug(leg.tags)
            if slug:
                slugs.add(slug)
            else:
                slugs = None
                break
        if failing and slugs:
            kind = "KNOWN(" + ",".join(sorted(slugs)) + ")"
        detail = (failing, src, expected, out)
    try:
        os.unlink(path)
    except OSError:
        pass
    return kind, detail


# ---------------------------------------------------------------------------
# Self-test: prove the classifier sees each failure class.
# ---------------------------------------------------------------------------

# A program whose emitted C makes one indirect call through a function
# pointer of the wrong type.  With fnsan armed it must classify as
# its trap class (FNPTR_TRAP, or BUG_fnptr_trap under strict); unarmed, the self-test says it cannot check this arm rather
# than passing it.
FNPTR_SELF_TEST = (
    "fn-pointer mismatch trapped",
    '(defn twice [x : int] : int (* 2 x))\n'
    '(defn boom [] : int\n'
    '  ```c\n  double (*d)(double) = (double (*)(double))(void *)twice;\n'
    '  return (int64_t)d(1.5);\n  ```)\n'
    '(defn main [] : int (println (twice 1)) (println (boom)) 0)\n',
    "2\n3\n", fuzz_arm.TRAP_CLASS)

SELF_TESTS = [
    ("clean pass",
     '(defn f [x : int] : int (+ x 1))\n(defn main [] : int (println (f 4)) 0)\n',
     "5\n", "ok"),
    ("wrong output detected",
     '(defn f [x : int] : int (+ x 1))\n(defn main [] : int (println (f 4)) 0)\n',
     "6\n", "BUG_wrong_output"),
    ("crash detected",
     '(defn boom [] : int\n'
     '  ```c\n  volatile int64_t *p = 0;\n  *p = 1;\n  return 0;\n  ```)\n'
     '(defn main [] : int (println (boom)) 0)\n',
     "0\n", "BUG_crash"),
    ("type error rejected",
     '(defn f [x : int] : int "nope")\n(defn main [] : int (println (f 1)) 0)\n',
     "?\n", "GEN_REJECT"),
    # ---- seam axis plumbing -------------------------------------------------
    #
    # Three rows, because a seam axis that can only report failure is useless:
    # one seam that PASSES (so "every seam fails" cannot be the emitter being
    # broken), one that fails with a wrong answer, and one the checker refuses.
    ("seam ok (effects: a correct seam)",
     "(defeffect FzX [] :float)\n"
     "(defn fzx [] : float (handle (perform (FzX)) (FzX [] k) (resume k 7.25)))\n"
     "(defn main [] : int (println (fzx)) 0)\n",
     "7.25\n", "ok", True),
    # This row used to be the live await-float defect
    # (async-await-payload-is-int64-only), fixed 2026-09-17.  The classifier
    # still needs a seam program whose printed value disagrees with its
    # expectation, so the expectation is deliberately wrong: the program
    # prints 7.25 and the row expects 7.24.  What is under test is the
    # classification, not the compiler.
    ("seam wrong-output detected (deliberately wrong expectation)",
     "(defn fzc [] : float 7.25)\n"
     "(defn main [] : int\n"
     "  (let [fut (async fzc)] (println (await fut)))\n  0)\n",
     "7.24\n", "BUG_seam_wrong_output", True),
    # The reject arm matters most.  This is the shape that made the whole class
    # invisible: comparing an erased payload against its own literal is a
    # TUR-E0042, and under the old classifier that is GEN_REJECT -- "the
    # generator emitted an illegal program" -- which never fails a run.
    # The reject used to be the live TUR-E0042 on `(= (await fut) 7.25)`; with
    # the await typed at the thunk's result (2026-09-17) that compares two
    # floats and is accepted, so the row compares against a cstr instead -- a
    # deliberate type error, so the checker still refuses a seam program.
    ("seam reject classified apart from GEN_REJECT",
     "(defn fzc [] : float 7.25)\n"
     "(defn main [] : int\n"
     "  (let [fut (async fzc)] (println (= (await fut) \"seven\")))\n  0)\n",
     "true\n", "SEAM_REJECT", True),
]


def self_test(tur, workdir):
    ok = True
    status = fuzz_arm.armed_env(dict(os.environ))[1]
    rows = list(SELF_TESTS)
    if "ARMED" in status:
        rows.append(FNPTR_SELF_TEST)
    else:
        print("  SKIP %-28s (%s)" % (FNPTR_SELF_TEST[0], status))
    for i, row in enumerate(rows):
        label, src, expected, want = row[0], row[1], row[2], row[3]
        is_seam = row[4] if len(row) > 4 else False
        path = os.path.join(workdir, "selftest%d.tur" % i)
        out = run_case(tur, path, src)
        got = classify(out, expected, is_seam)
        status = "ok " if got == want else "FAIL"
        if got != want:
            ok = False
        print("  %s %-28s want=%-16s got=%s" % (status, label, want, got))
    return ok


def seam_matrix(tur, workdir):
    """Print the seam x payload verdict table.

    One command that shows which runtime seams can carry which payloads, so a
    fix is verified by reading a row rather than by trusting a fuzz summary.
    Deterministic (every cell is pinned, nothing sampled), so it also diffs
    cleanly across builds.
    """
    kinds = [("scalar", sc) for sc in ("int", "float", "bool", "cstr")]
    kinds.append(("box", "int"))
    hdr = ["int", "float", "bool", "cstr", "box<int>"]
    print("seam x payload verdicts (%s)" % tur)
    print("  %-11s %s" % ("seam", " ".join("%-13s" % h for h in hdr)))
    worst = 0
    for name in sorted(Gen.SEAMS):
        cells = []
        for idx, (pk, sc) in enumerate(kinds):
            g = Gen(random.Random(1234 + idx), 0, True)
            g.force_scalar, g.force_payload = sc, pk
            leg = g.leg_seam(name)
            src, expected = assemble([leg])
            path = os.path.join(workdir, "mx_%s_%s_%s.tur" % (name, pk, sc))
            out = run_case(tur, path, src)
            k = classify(out, expected, True)
            if k == "ok":
                cell = "ok"
            elif k == "BUG_seam_wrong_output":
                cell = "WRONG(%s)" % out.stdout.strip()[:9]
                worst = max(worst, 2)
            elif k == "SEAM_REJECT":
                cell = "reject"
                worst = max(worst, 1)
            else:
                cell = k.replace("BUG_seam_", "")
                worst = max(worst, 2)
            cells.append(cell)
            try:
                os.unlink(path)
            except OSError:
                pass
        print("  %-11s %s" % (name, " ".join("%-13s" % c for c in cells)))
    print("\n  ok = payload round-trips.  WRONG = built, ran, wrong ANSWER.")
    print("  reject = checker refused the payload (honest; the outcome the")
    print("           session report asks for).  invalid_c = checker accepted,")
    print("           cc refused.")
    return worst


def known_probes(tur, workdir):
    print("known-probe status (open reports the generator avoids by default):")
    status = fuzz_arm.armed_env(dict(os.environ))[1]
    fnsan_armed = status.startswith("fnsan: ARMED")
    if not fnsan_armed and TRAP_ONLY_PROBES:
        print("  (%s -- trap-only rows report UNKNOWN)" % status)
    any_fixed = False
    for i, row in enumerate(KNOWN_PROBES):
        label, src = row[0], row[1]
        expected = row[2] if len(row) > 2 else None
        path = os.path.join(workdir, "known%d.tur" % i)
        out = run_case(tur, path, src)
        fired = out.kind in ("crash", "invalid_c", "link", "reject", "other",
                            "fnptr_trap")
        how = out.kind
        # A wrong-ANSWER defect builds and runs cleanly, so out.kind is
        # "clean" and the loop above would call it FIXED on a still-broken
        # build.  A row that declares its expected stdout gets the extra arm.
        if not fired and expected is not None and out.kind == "clean" \
                and out.stdout != expected:
            fired = True
            how = "wrong_output: %r != %r" % (out.stdout, expected)
        if fired:
            verdict = "fires (%s)" % how
        elif label in TRAP_ONLY_PROBES and not fnsan_armed:
            # Not FIXED and not firing: unjudgeable here.  Deliberately does
            # NOT set any_fixed -- a caller must not read this as a closed
            # report.
            verdict = "UNKNOWN -- trap-only, needs -fsanitize=function"
        else:
            any_fixed = True
            verdict = "FIXED -- retire its known_bug_slug row"
        print("  %-62s %s" % (label, verdict))
    return any_fixed


# ---------------------------------------------------------------------------



def _progress(i, n, kind, extra=""):
    """Report a case the moment it finishes.  Every non-ok verdict prints its
    own line; ok cases print a heartbeat every 25 so a long session is
    visibly alive under a pipe or ctest -V.  Flushed, because stdout is
    block-buffered under a pipe and nothing would show until exit."""
    if kind != "ok":
        print("  case %6d/%d  %s%s" % (i, n, kind, extra), flush=True)
    elif (i + 1) % 25 == 0 or i + 1 == n:
        print("  %6d/%d done" % (i + 1, n), flush=True)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, default=200, help="cases to generate")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 1))
    ap.add_argument("--legs", type=int, default=3, help="max legs per program")
    ap.add_argument("--tur", default=os.path.join(REPO, "build", "tur"))
    ap.add_argument("--save-dir", default=None)
    ap.add_argument("--emit-known", action="store_true",
                    help="also generate shapes matching open reports "
                         "(classified KNOWN, never fail the run)")
    ap.add_argument("--seam-frac", type=float, default=0.30,
                    help="fraction of cases that route a payload through a "
                         "RUNTIME SEAM instead of a compiler-owned crossing "
                         "(0 disables the axis; default 0.30)")
    ap.add_argument("--seam", default=None, choices=sorted(Gen.SEAMS),
                    help="generate ONLY this seam (implies every case is a "
                         "seam case); for triaging one feature")
    ap.add_argument("--crossing", default=None,
                    help="offer only the crossing x_NAME wherever it applies "
                         "(e.g. rank2_class, gbody, fn_field)")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--known-probes", action="store_true")
    ap.add_argument("--seam-matrix", action="store_true",
                    help="print the seam x payload verdict table and exit "
                         "(deterministic; the one-command view of the axis)")
    args = ap.parse_args()
    global FORCE_CROSSING
    FORCE_CROSSING = args.crossing
    if args.seam:
        args.seam_frac = 1.0

    tur = os.path.abspath(args.tur)
    if not os.path.exists(tur):
        print("type_fuzz_src: no compiler at %s" % tur, file=sys.stderr)
        return 2

    workdir = tempfile.mkdtemp(prefix="type-fuzz-work-")
    try:
        if args.self_test:
            print("type_fuzz_src: self-test (%s)" % tur)
            ok = self_test(tur, workdir)
            print("type_fuzz_src: self-test %s" % ("PASS" if ok else "FAIL"))
            return 0 if ok else 1
        if args.known_probes:
            known_probes(tur, workdir)
            return 0
        if args.seam_matrix:
            # Informational, like --known-probes: prints the table, never fails.
            seam_matrix(tur, workdir)
            return 0

        save_dir = args.save_dir or tempfile.mkdtemp(prefix="type-fuzz-src-")
        os.makedirs(save_dir, exist_ok=True)

        counts, findings = {}, []

        def job(i):
            return one_case(tur, workdir, i, args.seed * 1000003 + i,
                            args.legs, args.emit_known,
                            args.seam_frac, args.seam)

        print("type_fuzz_src: %d cases, seed %d, max %d legs, %d job(s)%s%s"
              % (args.n, args.seed, args.legs, args.jobs,
                 ", emit-known" if args.emit_known else "",
                 (", seam=%s only" % args.seam) if args.seam
                 else (", seam-frac %.2f" % args.seam_frac
                       if args.seam_frac else ", seams off")))
        print("type_fuzz_src: " + fuzz_arm.armed_env(dict(os.environ))[1])
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            futs = {pool.submit(job, i): i for i in range(args.n)}
            done = 0
            for fut in as_completed(futs):
                i = futs[fut]
                kind, detail = fut.result()
                counts[kind] = counts.get(kind, 0) + 1
                if detail:
                    findings.append((kind, i, detail))
                extra = ""
                if detail and detail[0]:
                    extra = "  tags=" + " | ".join(
                        ",".join(sorted(leg.tags)) for _j, _k, leg, *_ in detail[0])
                _progress(done, args.n, kind, extra)
                done += 1
        findings.sort(key=lambda t: t[1])

        for kind, i, (failing, src, expected, out) in findings:
            safe = kind.replace("(", "_").replace(")", "").replace(",", "+")
            d = os.path.join(save_dir, "%s-%06d" % (safe, i))
            os.makedirs(d, exist_ok=True)
            with open(os.path.join(d, "input.tur"), "w") as f:
                f.write(src)
            with open(os.path.join(d, "README"), "w") as f:
                f.write("classification: %s\nseed base: %d  case: %d\n"
                        "expected stdout:\n%s\ngot (%s):\n%r\nstderr tail:\n%s\n"
                        % (kind, args.seed, i, expected, out.kind, out.stdout,
                           out.stderr[-2000:]))
                if failing:
                    f.write("\nbisected failing legs:\n")
                else:
                    f.write("\nINTERACTION: no single leg fails alone\n")
                for j, k2, leg, s2, e2, o2 in failing:
                    f.write("  leg %d: %s  tags=%s\n"
                            % (j, k2, ",".join(sorted(leg.tags))))
            for j, k2, leg, s2, e2, o2 in failing:
                with open(os.path.join(d, "leg%d.tur" % j), "w") as f:
                    f.write(";; %s  tags=%s\n;; expected:\n"
                            % (k2, ",".join(sorted(leg.tags))))
                    for ln in e2.splitlines():
                        f.write(";;   %s\n" % ln)
                    f.write(s2)

        print("\ntype_fuzz_src: %d cases" % args.n)
        for k in sorted(counts):
            print("  %-28s : %d" % (k, counts[k]))

        n_bugs = sum(v for k, v in counts.items() if k.startswith("BUG"))
        n_rej = counts.get("GEN_REJECT", 0)
        n_known = sum(v for k, v in counts.items() if k.startswith("KNOWN"))
        n_srej = counts.get("SEAM_REJECT", 0)
        n_hang = counts.get("SEAM_HANG", 0)
        print("\n  BUG classes (fail)          : %d" % n_bugs)
        print("  generator rejects (report)  : %d" % n_rej)
        n_trap = counts.get("FNPTR_TRAP", 0)
        if n_trap:
            print("  fn-pointer traps (report)   : %d   "
                  "(TUR_FUZZ_FNSAN_STRICT=0: report-only)" % n_trap)
        print("  known open reports (report) : %d" % n_known)
        # A seam reject is the elaborator refusing a payload it cannot carry --
        # the outcome the session report asks for -- so it is reported, not
        # failed.  It is still a feature-surface hole worth reading.
        print("  seam payload rejects        : %d" % n_srej)
        print("  seam hangs (timing/deadlock): %d" % n_hang)
        if findings:
            print("\n  saved to %s" % save_dir)
        elif not args.save_dir:
            shutil.rmtree(save_dir, ignore_errors=True)
        return 1 if n_bugs else 0
    finally:
        shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
