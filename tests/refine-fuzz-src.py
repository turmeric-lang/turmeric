#!/usr/bin/env python3
"""tests/refine-fuzz-src.py -- SOURCE-LEVEL differential fuzzer for refinement
types.

Why this exists
---------------
`tests/unit/refine_fuzz.c` generates normalized VCs directly and compares the
in-house solver's verdicts against Z3.  It is a good solver fuzzer and a
useless compiler fuzzer: it starts *below* the encoder, so every bug in the
translation from Turmeric source into a VC is invisible to it.  Both soundness
bugs found in the refinement work so far lived exactly there, and ~17,300
randomly generated VCs across six seeds were clean the whole time.

This harness starts at the top instead.  It generates whole Turmeric programs,
compiles and runs each one TWICE -- once with static discharge suppressed and
once normally -- and compares what actually happened.

Refinement types graduated in v0.33.0, so the reference leg is no longer "the
experiment is off"; it is `TUR_REFINE_NO_DISCHARGE=1`, an env-only test seam
that makes every obligation decline so nothing is elided.  See run_gate() for
why no shipping flag can stand in for it.

TERMINOLOGY: "gate-off" and "gate-on" survive throughout this file as the names
of the two legs.  There is no gate any more -- read them as "reference leg,
discharge suppressed" and "discharge leg, normal build".  The names were kept
deliberately rather than renamed across ~15 sites in a graduation change set.

The property that matters
-------------------------
With discharge SUPPRESSED, every refinement is a runtime contract check.  A
program that violates its own refinement aborts.  With discharge ON, a proved
obligation lets that check be elided.  So:

    reference leg aborted on a contract  ==>  discharge leg must NOT run to
                                              completion

Any case where the reference leg aborts and the discharge leg exits 0 is a
MISCOMPILE: discharge changed a program that was caught into one that silently
returns a value violating its own declared refinement.  That is the single
thing the design forbids, and it is precisely what both known bugs did.

Three weaker properties are checked alongside it:

  * both gates ran clean  ->  stdout must be identical (elision must not change
    observable behaviour)
  * gate-off ran clean    ->  gate-on must not introduce an abort
  * gate-off ran clean    ->  gate-on rejecting it at compile time is
    SUSPICIOUS but not automatically wrong.  `TUR-E0371` is universally
    quantified over the function's inputs while the program only exercises the
    arguments it happens to pass, so a correct refutation can coexist with a
    clean run.  These are counted and saved for triage, never failed on.

Proving the fuzzer can fail
---------------------------
A fuzzer nobody has seen fail is a fuzzer that reports zero because it is
broken.  Two one-line sabotages, both verified to be caught at n=200 seed=41
where the shipped build reports zero:

  A. Congruence without evidence -- in `rt_binding_is_pure` (elab_fns.c),
     replace `return rt_pure_binding(&c, b);` with `return true;`.
     Reproduces the two historical soundness bugs.  Caught: 8/200.

  B. Entry-check elision done wrong -- at the top of `rt_inject_param_checks`,
     add `if (g_opt_refined) return body;`.  This SIMULATES the next planned
     feature (whole-program elision of a callee's entry check) implemented
     without the exported/address-taken analysis that would make it sound.
     Caught: 14/200.  Worth rerunning when that feature is actually built.

    cmake -S . -B build-unsound -DCMAKE_BUILD_TYPE=Debug
    cmake --build build-unsound -j
    python3 tests/refine-fuzz-src.py --tur ./build-unsound/tur --n 200 --seed 41

`--self-test` runs a cheaper check of the plumbing (exit-code capture, stdout
capture, classification) against the pinned regression fixtures.

What the generator can and cannot see (solver-integer-tail-plan, Phase 4)
------------------------------------------------------------------------
A differential fuzzer reports only on the shapes its population contains.
Until 2026-09-05 this one emitted `+ - *` by a constant, `/` by a literal
inside helper bodies, comparisons, and at most TWO parameters -- so the three
swept populations (this generator, the corpus, the in-tree fixtures) were
blind to `mod`, to squares, and to any counterexample search wider than three
variables, and nine ordinary day-one probes found four undecided shapes none
of them had ever exercised.  `shape_integer` (int mode) now generates exactly
those: `(mod e k)` and `(/ e k)` with a literal divisor under the axioms the
encoder asserts for C's truncating `/` and `%`, `(* e e)` under the square
sign law, and 3-5 parameter functions whose refinements the bounded model
search has to refute across every parameter.  `expr()` also reaches `mod` and
a square at low weight, so they show up inside the other shapes' arithmetic.

The differential property is exactly the right check for the axioms: a wrong
sign in the `mod` remainder clause would not show up as a corpus label, it
would PROVE a refinement the runtime violates -- a `BUG_soundness` here.
That is why the shape's rungs include refinements that are false only for
NEGATIVE dividends, and why `main`'s literal arguments range over both signs.

Reflected measures at density (reflected-measures RF3/RF4)
---------------------------------------------------------
`shape_reflect` is the population the `reflect_if` / `reflect_lie` helper
kinds could not be: recursive `^reflect` measures over a list, unfolded at
literals deep enough to cross the fuel limit, RF4 arm selection from a refined
list parameter, a sabotaged sibling per rung, and the RF1/RF2 rejections.
`--only-shape reflect` runs it alone, without random helpers or extra targets,
either of which would reject the case before the measure is reached.  Its
first run found a soundness bug outside reflection entirely: a `float` record
field's selector was declared Int, which made every VC that mentioned one
contradictory (docs/archive/refine-float-field-selector-declared-int.md).

Usage
-----
    python3 tests/refine-fuzz-src.py [--n 300] [--seed 1] [--jobs 4]
                                     [--mode int|float|both]
                                     [--tur ./build/tur] [--save-dir DIR]
                                     [--only-shape SHAPE] [--self-test]

Exit status is 1 if any BUG class fired, 0 otherwise.  Suspicious cases alone
never fail the run.
"""

import argparse
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor, as_completed

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fuzz_arm  # noqa: E402  (tests/fuzz_arm.py)

# ---------------------------------------------------------------------------
# Program generation
# ---------------------------------------------------------------------------

CMP_OPS = ["<", "<=", ">", ">=", "=", "not="]

# Every helper the generator can emit, tagged with whether calling it twice
# with equal arguments really does produce equal results.  `truly_pure` is the
# GROUND TRUTH, not what the compiler thinks -- the whole point is to include
# helpers that look pure and are not.
#
#   kind          arity  truly_pure  what it is
#   pure_plain      2      yes       ordinary arithmetic, no annotation
#   pure_fx         2      yes       same, but declares #fx{}
#   pure_rec        1      yes       recursive -- exercises the cycle rule
#   impure_c        1      NO        declares #fx{} and counts up in inline C
#   impure_c0       0      NO        nullary counter (the original repro shape)
#   reflect_if      1      yes       `^reflect` with an if-body: its equation is
#                                    ADMITTED by unfolding (reflected-measures)
#   reflect_lie     1      yes       `^reflect` on a non-total body: must be a
#                                    hard TUR-E0384, never an admitted equation
#
# RM-B: bool-returning helpers live in their own pool, because they are the
# only ones that can appear as a predicate ATOM rather than inside an
# arithmetic expression.  The same liar/honest split applies -- a `bool`
# measure over a `#fx{}` helper that toggles is exactly the trap the `int`
# ones set, reached through the sort the encoder used to be unable to spell.
#
#   pure_bool       1      yes       an honest comparison
#   impure_bool_c   1      NO        declares #fx{} and toggles in inline C


class Gen:
    def __init__(self, rng, mode, only=None):
        self.rng = rng
        self.only = only            # --only-shape: generate just this shape
        self.mode = mode            # "int" or "float"
        self.helpers = []           # list of (name, kind, arity)
        self.bool_helpers = []      # RM-B: bool-returning, usable as an atom

    # -- type-dependent leaves ------------------------------------------------

    @property
    def ty(self):
        return "int" if self.mode == "int" else "float"

    def lit(self):
        if self.mode == "int":
            return str(self.rng.randint(-6, 6))
        # Non-zero fractional parts on purpose: an integral float literal
        # cannot show truncation or int/float divergence.
        return self.rng.choice(
            ["0.5", "1.25", "2.75", "3.25", "7.1", "-1.5", "-4.25", "6.5"])

    def nonzero_lit(self):
        if self.mode == "int":
            return str(self.rng.choice([-4, -3, -2, -1, 1, 2, 3, 4]))
        return self.rng.choice(["1.25", "2.5", "-3.5", "4.0"])

    # -- helpers --------------------------------------------------------------

    def gen_helpers(self, n):
        """Emit n helper defns.  Returns (source_lines, helper_specs)."""
        lines = []
        kinds = ["pure_plain", "pure_fx", "pure_rec", "impure_c", "impure_c0",
                 "reflect_if", "reflect_lie"]
        for i in range(n):
            kind = self.rng.choice(kinds)
            name = "h%d" % i
            if kind == "pure_plain":
                body = self.expr(2, ["a", "b"])
                lines.append("(defn %s [a : %s, b : %s] : %s\n  %s)"
                             % (name, self.ty, self.ty, self.ty, body))
                self.helpers.append((name, kind, 2, True))
            elif kind == "pure_fx":
                body = self.expr(2, ["a", "b"])
                lines.append("(defn %s [a : %s, b : %s] #fx{} : %s\n  %s)"
                             % (name, self.ty, self.ty, self.ty, body))
                self.helpers.append((name, kind, 2, True))
            elif kind == "pure_rec":
                # Structurally decreasing on the int path; on the float path a
                # plain guard, since float recursion is not the interesting bit.
                if self.mode == "int":
                    lines.append(
                        "(defn %s [a : int] #fx{} : int\n"
                        "  (if (<= a 0) 0 (+ 1 (%s (- a 1)))))" % (name, name))
                else:
                    lines.append(
                        "(defn %s [a : float] #fx{} : float\n"
                        "  (if (<= a 0.0) 0.0 (* a 0.5)))" % name)
                self.helpers.append((name, kind, 1, True))
            elif kind == "reflect_if":
                # reflected-measures RF3: a `^reflect` measure whose defining
                # equation the encoder ADMITS by unfolding.  Non-recursive with
                # an `if`, so it unfolds at ANY argument (no arm to select) and
                # exercises the proposition-shaped encoding
                #   (c => f(t) = a) and (not c => f(t) = b).
                # Both legs see the same flag; the reference leg suppresses
                # discharge, so an unfolding that is WRONG (an equation that
                # does not hold of the compiled body) shows up as a
                # BUG_soundness exactly like a bad congruence grant would.
                body_a = self.expr(1, ["a"], pure_only=True)
                body_b = self.expr(1, ["a"], pure_only=True)
                lines.append(
                    "(defn ^reflect %s [a : %s] : %s\n"
                    "  (if (%s a %s) %s %s))"
                    % (name, self.ty, self.ty,
                       self.rng.choice([">", "<=", "="]), self.lit(),
                       body_a, body_b))
                self.helpers.append((name, kind, 1, True))
            elif kind == "reflect_lie":
                # reflected-measures RF1: `^reflect` on a function that is NOT
                # total must be a hard TUR-E0384 (the program does not compile
                # on either leg -> skip_invalid), never an admitted equation.
                # If the gate ever let this through, `f(a) = 1 + f(a)` would be
                # an inconsistent hypothesis and every obligation in the
                # program would "prove" -- the soundness check catches that.
                lines.append(
                    "(defn ^reflect %s [a : %s] : %s\n"
                    "  (%s %s (%s a)))"
                    % (name, self.ty, self.ty, "+", self.nonzero_lit(), name))
                self.helpers.append((name, kind, 1, True))
            elif kind == "impure_c":
                # Declares an EMPTY effect row and is not remotely pure.  This
                # is the shape that broke the compiler twice.
                if self.mode == "int":
                    lines.append(
                        "(defn %s [a : int] #fx{} : int\n"
                        "  ```c\n  static int64_t s = 0;\n"
                        "  s += a + 1;\n  return s;\n  ```)" % name)
                else:
                    lines.append(
                        "(defn %s [a : float] #fx{} : float\n"
                        "  ```c\n  static double s = 0.0;\n"
                        "  s += a + 1.5;\n  return s;\n  ```)" % name)
                self.helpers.append((name, kind, 1, False))
            else:  # impure_c0
                if self.mode == "int":
                    lines.append(
                        "(defn %s [] #fx{} : int\n"
                        "  ```c\n  static int64_t n = 0;\n"
                        "  return n++;\n  ```)" % name)
                else:
                    lines.append(
                        "(defn %s [] #fx{} : float\n"
                        "  ```c\n  static double n = 0.0;\n"
                        "  n += 1.25;\n  return n;\n  ```)" % name)
                self.helpers.append((name, kind, 0, False))
        return lines

    def gen_bool_helpers(self, n):
        """RM-B: emit n bool-returning helpers, half of them liars.

        A bool measure is declared VS_BOOL now, which means congruence closure
        relates two occurrences of `(b p0)` -- and that is only valid when `b`
        really is pure.  The toggling inline-C variant makes `(or (b p0) (not
        (b p0)))` FALSE at runtime, so a compiler that grants congruence on the
        strength of the `#fx{}` annotation elides a check that would have
        fired.  Same sabotage as the int helpers, new sort.

        A liar reaching PREDICATE position is normally stopped one layer
        earlier, by TUR-E0375 (both gates reject it, so the case lands in
        skip_invalid).  That is the point: the property being fuzzed is that
        E0375 and the congruence denial TOGETHER keep it sound.  If E0375 ever
        stopped firing, these cases would flow through to the encoder and the
        soundness check would have to catch them."""
        lines = []
        for i in range(n):
            name = "b%d" % i
            if self.rng.random() < 0.5:
                lines.append("(defn %s [a : %s] #fx{} : bool\n  (%s a %s))"
                             % (name, self.ty, self.rng.choice([">", ">=", "<", "<="]),
                                self.lit()))
                self.bool_helpers.append((name, "pure_bool", 1, True))
            else:
                lines.append(
                    "(defn %s [a : %s] #fx{} : bool\n"
                    "  ```c\n  static int64_t n = 0;\n"
                    "  (void)a;\n  n++;\n  return n %% 2 == 0;\n  ```)"
                    % (name, self.ty))
                self.bool_helpers.append((name, "impure_bool_c", 1, False))
        return lines

    def bool_atom(self, scope):
        """A bool-measure application, usable wherever a proposition is."""
        name, _kind, arity, _pure = self.rng.choice(self.bool_helpers)
        args = " ".join(self.expr(1, scope, pure_only=True) for _ in range(arity))
        return "(%s%s%s)" % (name, " " if args else "", args)

    # -- expressions ----------------------------------------------------------

    def expr(self, depth, scope, pure_only=False):
        """An arithmetic expression over `scope` (a list of variable names).

        `pure_only` restricts helper calls to the truly-pure pool.  Predicates
        must set it: a contract predicate that calls an effectful function is
        a `TUR-E0375` error, so generating one produces a program that does not
        compile and the case is dropped.  That cost ~8% of the fuzz budget
        rediscovering a rule one fixture already pins."""
        if depth <= 0 or self.rng.random() < 0.30:
            if scope and self.rng.random() < 0.6:
                return self.rng.choice(scope)
            return self.lit()
        r = self.rng.random()
        if r < 0.45:
            op = self.rng.choice(["+", "-", "*"])
            return "(%s %s %s)" % (op, self.expr(depth - 1, scope, pure_only),
                                   self.expr(depth - 1, scope, pure_only))
        if r < 0.55:
            # Divisor is a nonzero literal: a division trap would abort both
            # gates identically, which is noise rather than signal.
            return "(/ %s %s)" % (self.expr(depth - 1, scope, pure_only),
                                  self.nonzero_lit())
        if self.mode == "int" and r < 0.59:
            # Phase 4: `mod` by a literal, so the encoder's div/mod axioms
            # (enc_divmod_axioms) get a population inside ordinary arithmetic
            # rather than only in shape_integer's hand-built rungs.  Int only:
            # `mod` is not defined on floats.
            return "(mod %s %s)" % (self.expr(depth - 1, scope, pure_only),
                                    self.nonzero_lit())
        if self.mode == "int" and r < 0.62:
            # Phase 4: a SQUARE -- the same subexpression on both sides, which
            # is what enc_nonlinear's structural-equality test recognizes.  The
            # operand is generated ONCE and repeated textually; two independent
            # draws would be an ordinary nonlinear product, which stays
            # abstracted.
            sq = self.expr(depth - 1, scope, pure_only)
            return "(* %s %s)" % (sq, sq)
        pool = [h for h in self.helpers if h[3]] if pure_only else self.helpers
        if r < 0.95 and pool:
            name, _kind, arity, _pure = self.rng.choice(pool)
            args = " ".join(self.expr(depth - 1, scope, pure_only)
                            for _ in range(arity))
            return "(%s%s%s)" % (name, " " if args else "", args)
        return self.expr(depth - 1, scope, pure_only)

    # -- predicates -----------------------------------------------------------

    def atom(self, scope):
        op = self.rng.choice(CMP_OPS)
        return "(%s %s %s)" % (op, self.expr(2, scope, pure_only=True),
                               self.expr(1, scope, pure_only=True))

    def pred(self, depth, scope):
        # RM-B: a bool-returning measure IS a proposition, so it belongs in the
        # atom position alongside a comparison rather than under one.
        if self.bool_helpers and self.rng.random() < 0.15:
            return self.bool_atom(scope)
        if depth <= 0 or self.rng.random() < 0.55:
            return self.atom(scope)
        r = self.rng.random()
        if r < 0.4:
            return "(and %s %s)" % (self.pred(depth - 1, scope),
                                    self.pred(depth - 1, scope))
        if r < 0.8:
            return "(or %s %s)" % (self.pred(depth - 1, scope),
                                   self.pred(depth - 1, scope))
        return "(not %s)" % self.pred(depth - 1, scope)

    # -- whole program --------------------------------------------------------
    #
    # Three shapes, because uniform random generation almost never lands inside
    # the solver's fragment: a 40-case smoke run proved 2 obligations, and the
    # soundness property only has teeth on a case where a check was actually
    # ELIDED.  `linear` and `congruence` are built to be decidable often --
    # sometimes truly, sometimes falsely, which is the point.

    def _target(self, params, ret_pred, pre, body):
        sig = ", ".join("%s : %s" % (p, self.ty) for p in params)
        return ("(defn target [%s] : #refine{ r : %s | %s }%s\n  %s)"
                % (sig, self.ty, ret_pred,
                   "\n  :pre " + pre if pre else "", body))

    def _main(self, params):
        calls = []
        for _ in range(self.rng.randint(1, 3)):
            args = " ".join(self.lit() for _ in params)
            calls.append("  (println (target%s%s))" % (" " if args else "", args))
        return "(defn main [] : int\n%s\n  0)" % "\n".join(calls)

    def shape_random(self):
        """Anything goes.  Mostly lands outside the fragment; that is coverage
        of the fallback path, which must never elide."""
        params = ["p%d" % i for i in range(self.rng.randint(0, 2))]
        return (params,
                self._target(params,
                             self.pred(2, params + ["r"]),
                             self.pred(1, params) if params and self.rng.random() < 0.5 else None,
                             self.expr(3, params)))

    def shape_linear(self):
        """Linear arithmetic over one or two params: a body the encoder fully
        understands, a `:pre` that bounds the inputs, and a goal relating `r`
        to them.  S2 decides most of these outright."""
        params = ["p%d" % i for i in range(self.rng.randint(1, 2))]
        p0 = params[0]
        k = self.lit()
        body = self.rng.choice([
            p0,
            "(+ %s %s)" % (p0, k),
            "(- %s %s)" % (p0, k),
            "(* %s 2)" % p0 if self.mode == "int" else "(* %s 2.0)" % p0,
            "(+ %s %s)" % (p0, params[-1]),
        ])
        lo, hi = sorted([self.lit(), self.lit()])
        pre = self.rng.choice([
            "(>= %s %s)" % (p0, lo),
            "(and (>= %s %s) (<= %s %s))" % (p0, lo, p0, hi),
            "(< %s %s)" % (p0, hi),
            None,
        ])
        rhs = self.rng.choice([self.lit(), p0, "(+ %s %s)" % (p0, self.lit())])
        ret_pred = "(%s r %s)" % (self.rng.choice(CMP_OPS), rhs)
        if self.rng.random() < 0.25:
            ret_pred = "(and %s (%s r %s))" % (
                ret_pred, self.rng.choice(CMP_OPS), self.lit())
        return params, self._target(params, ret_pred, pre, body)

    def shape_congruence_method(self):
        """The bug shape aimed at a TYPECLASS METHOD.

        A method has no global binding under its bare name, so it used to fall
        through to the abstract-measure rule -- "a name that resolves to no
        function at all is an uninterpreted mathematical function, therefore
        congruent" -- and pick up congruence for free even when the instance
        was wired to a counter.  The plain `congruence` shape could not reach
        this: it only ever repeats a call to a HELPER, and `typeclass` never
        repeats a call at all.  The bug lived in the gap between two shapes
        that each looked like they covered it."""
        impure = [h for h in self.helpers if not h[3]]
        pure = [h for h in self.helpers if h[3]]
        pool = impure if (impure and self.rng.random() < 0.7) else (pure or impure)
        if not pool:
            return self.shape_congruence()
        hname, _k, harity, _p = self.rng.choice(pool)
        inner = "(%s%s)" % (hname, " self" * harity)
        z = self._zero()
        call = self.rng.choice(["(.tickm %s)", "(tickm %s)"]) % ("1" if self.mode == "int" else "1.25")
        lines = [
            "(defclass Ticker [a]\n  (tickm [self : a] : %s))" % self.ty,
            "(definstance Ticker [%s]\n  (tickm [self : %s] : %s\n    %s))"
            % (self.ty, self.ty, self.ty, inner),
            "(defn target [] : #refine{ r : %s | %s }\n  (- %s %s))"
            % (self.ty, self.rng.choice(["(>= r %s)" % z, "(= r %s)" % z,
                                         "(<= r %s)" % z]), call, call),
        ]
        return [], "\n\n".join(lines)

    def shape_congruence_bool(self):
        """RM-B: the congruence trap, spelled with a BOOL measure.

        `(or (b p0) (not (b p0)))` is a tautology exactly when the two
        occurrences of `(b p0)` denote the same value -- i.e. exactly when `b`
        is pure.  Half the bool helpers toggle, so half of these are false at
        runtime, and a compiler that grants congruence on the `#fx{}`
        annotation alone elides the check that would have caught it.

        The int shape puts its trap in the SUBJECT (`(- (h p) (h p))`); this
        one puts it in the PREDICATE, which is the only place a bool measure
        can appear and therefore a path the int shapes never exercise."""
        name, _kind, arity, truly_pure = self.rng.choice(self.bool_helpers)
        params = ["p0"] if arity >= 1 else []
        call = "(%s%s)" % (name, " p0" * arity)
        ret_pred = self.rng.choice([
            "(or %s (not %s))" % (call, call),
            "(=> %s %s)" % (call, call),
            "(= %s %s)" % (call, call),
        ])
        _ = truly_pure   # ground truth the compiler must work out for itself
        return params, self._target(params, ret_pred, None, "p0" if params
                                    else self._zero())

    def shape_congruence(self):
        """The bug shape.  Two occurrences of the SAME call, subtracted, with a
        refinement that holds iff the callee is genuinely pure.  Half the
        helpers available here are liars."""
        if self.bool_helpers and self.rng.random() < 0.35:
            return self.shape_congruence_bool()
        name, _kind, arity, truly_pure = self.rng.choice(self.helpers)
        params = ["p0"] if arity >= 1 else []
        # Every argument is `p0`, so the two occurrences are textually and
        # semantically identical -- which is exactly what congruence claims.
        # Saturate the call: under-applying would build a closure and the
        # program would not compile.
        call = "(%s%s)" % (name, " p0" * arity)
        body = self.rng.choice([
            "(- %s %s)" % (call, call),
            "(- (+ %s %s) (* 2 %s))" % (call, call, call) if self.mode == "int"
            else "(- (+ %s %s) (* 2.0 %s))" % (call, call, call),
        ])
        zero = "0" if self.mode == "int" else "0.0"
        ret_pred = self.rng.choice([
            "(>= r %s)" % zero, "(= r %s)" % zero, "(<= r %s)" % zero])
        # truly_pure is ground truth the generator knows and the compiler must
        # work out for itself; it is not written into the program.
        _ = truly_pure
        return params, self._target(params, ret_pred, None, body)

    def _zero(self):
        return "0" if self.mode == "int" else "0.0"

    def bound(self, var):
        """A simple, decidable bound on `var` -- the kind of hypothesis a
        refined parameter is actually written to supply."""
        z, k = self._zero(), self.lit()
        return self.rng.choice([
            "(>= %s %s)" % (var, z),
            "(> %s %s)" % (var, z),
            "(<= %s %s)" % (var, k),
            "(and (>= %s %s) (<= %s %s))" % (var, z, var, self.lit()),
        ])

    def shape_param(self):
        """A refined PARAMETER feeding a refined return.

        Entry checks are never elided, so this shape cannot go wrong by
        elision on its own -- but the parameter predicate becomes a HYPOTHESIS
        for the return obligation, and the return check IS elided when that
        obligation discharges. If the hypothesis were ever assumed without the
        check that backs it, this is the shape that would show it."""
        pp = self.bound("v")
        body = self.rng.choice([
            "p0",
            "(+ p0 %s)" % self.lit(),
            "(* p0 2)" if self.mode == "int" else "(* p0 2.0)",
            "(- p0 %s)" % self.lit(),
        ])
        rp = "(%s r %s)" % (self.rng.choice(CMP_OPS),
                            self.rng.choice([self._zero(), self.lit(), "p0"]))
        return (["p0"],
                "(defn target [p0 : #refine{ v : %s | %s }] : #refine{ r : %s | %s }\n  %s)"
                % (self.ty, pp, self.ty, rp, body))

    def shape_propagate(self):
        """RT4 result propagation across a call-site crossing: a refined result
        flows into a refined parameter, through a caller that has its own
        refinement. Three obligations per program, all in the elaboration
        layer -- the layer both known soundness bugs lived in."""
        inner_pp = self.bound("v")
        inner_rp = "(%s r %s)" % (self.rng.choice([">", ">=", "<", "<="]),
                                  self.rng.choice([self._zero(), self.lit()]))
        inner_body = self.rng.choice(
            ["x", "(+ x %s)" % self.lit(),
             "(* x 2)" if self.mode == "int" else "(* x 2.0)"])
        outer_pp = self.bound("w")
        fwd = self.rng.choice(["y", "(+ y %s)" % self.lit()])
        lines = [
            "(defn inner [x : #refine{ v : %s | %s }] : #refine{ r : %s | %s }\n  %s)"
            % (self.ty, inner_pp, self.ty, inner_rp, inner_body),
            "(defn outer [y : #refine{ w : %s | %s }] : %s\n  (inner %s))"
            % (self.ty, outer_pp, self.ty, fwd),
            "(defn target [p0 : %s] : %s\n  (outer p0))" % (self.ty, self.ty),
        ]
        return ["p0"], "\n\n".join(lines)

    def shape_typeclass(self):
        """Typeclass method dispatch and refinement variance.

        Parameters and results vary in OPPOSITE directions, so both ladders are
        generated: for a parameter an instance may demand LESS than the class
        (legal) but not more (`TUR-E0374`); for a result it may deliver MORE
        (legal) but not less. Both dispatch spellings -- dotted `(.m x k)` and
        bare `(m x k)` -- resolve to the same instance method and must be
        checked identically."""
        z = self._zero()
        # (class_pred, instance_pred, legal?) -- an instance demanding MORE of a
        # parameter than the class promises is the illegal direction.
        cp, ip = self.rng.choice([
            ("(>= v %s)" % z, "(>= v %s)" % z),   # identical
            ("(> v %s)" % z,  "(>= v %s)" % z),   # instance demands less: legal
            ("(>= v %s)" % z, "(> v %s)" % z),    # instance demands more: E0374
        ])
        # Result ladder, the other way round: promising LESS than the class is
        # the illegal direction.  The last entry is the one that matters most:
        # a NONLINEAR instance promise makes `instance |- class` abstract to an
        # uninterpreted term, so the variance check answers unknown and emits
        # no error -- while a caller is still handed the class promise. Only
        # the extra class check injected on the instance keeps that sound, so
        # this rung is what exercises it.
        use_ret = self.rng.random() < 0.6
        nonlin = ("(>= (* r r) (* r 2))" if self.mode == "int"
                  else "(>= (* r r) (* r 2.0))")
        c_rp, i_rp = self.rng.choice([
            ("(>= r %s)" % z, None),              # instance inherits
            ("(>= r %s)" % z, "(>= r %s)" % z),   # restated verbatim
            ("(>= r %s)" % z, "(> r %s)" % z),    # delivers more: legal
            ("(>= r %s)" % z, "(>= r -5)" if self.mode == "int"
                              else "(>= r -5.5)"),  # delivers less: E0374
            ("(>= r %s)" % z, nonlin),            # weaker, but undecidably so
        ])
        cls_ret = (" : #refine{ r : %s | %s }" % (self.ty, c_rp)) if use_ret else " : %s" % self.ty
        if use_ret and i_rp:
            inst_ret = " : #refine{ r : %s | %s }" % (self.ty, i_rp)
        else:
            inst_ret = " : %s" % self.ty
        body = self.rng.choice([
            "(* self k)", "(+ self k)",
            "(- self k)", "(+ (* self k) 1)" if self.mode == "int"
                          else "(+ (* self k) 1.25)"])
        call = self.rng.choice(["(.meth p0 %s)", "(meth p0 %s)"]) % self.lit()
        # Give the caller its own refinement half the time: that is what makes
        # RT4 propagate the CLASS promise across the dispatch and elide the
        # caller's check, which is the path where a wrong promise turns into a
        # wrong answer rather than just a missing hypothesis.
        if use_ret and self.rng.random() < 0.5:
            tgt_ret = "#refine{ r : %s | %s }" % (
                self.ty, self.rng.choice(["(>= r %s)" % z, "(> r %s)" % z]))
        else:
            tgt_ret = self.ty
        lines = [
            "(defclass Cls [a]\n  (meth [self : a, k : #refine{ v : %s | %s }]%s))"
            % (self.ty, cp, cls_ret),
            "(definstance Cls [%s]\n  (meth [self : %s, k : #refine{ v : %s | %s }]%s\n    %s))"
            % (self.ty, self.ty, self.ty, ip, inst_ret, body),
            "(defn target [p0 : %s] : %s\n  %s)" % (self.ty, tgt_ret, call),
        ]
        return ["p0"], "\n\n".join(lines)

    def extra_targets(self):
        """Additional refined functions, so a program carries SEVERAL
        obligations rather than one.

        This is not padding. The RT7 within-unit memo only engages when a
        second obligation is decided, and a one-obligation program cannot
        exercise it at all: a build with the memo's confirmation step deleted
        -- which is a real miscompile, `refine-memo-distinct-obligations` pins
        it -- ran 250 generated cases completely clean, because none of them
        asked two questions."""
        out, names = [], []
        z = self._zero()
        for i in range(self.rng.randint(1, 3)):
            nm = "t%d" % i
            names.append(nm)
            pred = self.rng.choice([
                "(>= r %s)" % z, "(> r %s)" % z, "(<= r %s)" % z,
                "(not= r %s)" % z, "(= r p)",
            ])
            pre = self.rng.choice(["(>= p %s)" % z, "(<= p %s)" % self.lit(), None])
            body = self.rng.choice(["p", "(+ p %s)" % self.lit(),
                                    "(* p 2)" if self.mode == "int" else "(* p 2.0)"])
            out.append("(defn %s [p : %s] : #refine{ r : %s | %s }%s\n  %s)"
                       % (nm, self.ty, self.ty, pred,
                          "\n  :pre " + pre if pre else "", body))
        return out, names

    def shape_branching(self):
        """Bodies that are `if` / `let`, which RT4 discharges PER PATH.

        The `let` rungs deliberately include a binding that SHADOWS the
        parameter.  Path splitting adds `x = v` to a single flat namespace, so
        `(let [x (- x 1)] x)` asserts `x = x - 1` -- false for every x, and a
        false hypothesis proves anything, eliding a check on a function that
        really does return a violating value.  That was a live miscompile and
        no other shape here can reach it: nothing else generates a binding
        form at all."""
        z = self._zero()
        one = "1" if self.mode == "int" else "1.0"
        body = self.rng.choice([
            "(if (>= p %s) p %s)" % (z, z),
            "(if (> p %s) p (- %s p))" % (z, z),
            "(if (>= p %s) p (if (< p %s) (- %s p) %s))" % (z, z, z, z),
            "(let [q (* p 2)] q)" if self.mode == "int" else "(let [q (* p 2.0)] q)",
            "(let [q (+ p %s)] (if (>= q %s) q %s))" % (one, z, z),
            # shadowing rungs
            "(let [p (- p %s)] p)" % one,
            "(let [p (+ p %s)] (if (>= p %s) p %s))" % (one, z, z),
        ])
        pred = self.rng.choice(["(>= r %s)" % z, "(> r %s)" % z,
                                "(<= r %s)" % z, "(>= r p)"])
        return (["p"],
                "(defn target [p : %s] : #refine{ r : %s | %s }\n  %s)"
                % (self.ty, self.ty, pred, body))

    def shape_datatype(self):
        """`match` bodies, which now carry HYPOTHESES from the arm selection.

        Every other shape's facts come from the goal's own arithmetic. These
        come from the arm that was selected -- a literal pattern's `(= p 0)`, a
        guard's condition verbatim, a constructor's tag, a record binder's
        field. Each is a fact synthesized by the compiler rather than written
        by the program, so each is a chance to assert something FALSE, and a
        false hypothesis proves the goal and elides the check. Nothing else
        here generates a `match` at all.

        The constructor rungs are deliberately unprovable-but-generated: with
        no constructor axioms in the VC, `(.a (FzBox p p))` does not reduce to
        `p`, so they cover the hazard without expecting a proof. What they DO
        exercise is arms accumulating hypotheses about one scrutinee.

        Only int mode -- a float scrutinee cannot carry an integer literal
        pattern, and the guard/tag rungs are where the risk lives anyway."""
        if self.mode != "int":
            return self.shape_branching()
        z = self._zero()
        body = self.rng.choice([
            # literal patterns: `(= p <lit>)` is the fact each arm gets
            "(match p 0 %s 1 %s _ p)" % (z, z),
            "(match p 0 0 1 1 _ %s)" % z,
            "(match p 0 (- p 1) 1 p _ %s)" % z,
            # guards: a necessary condition for the arm, never a sufficient one
            "(match p 0 %s _ when (>= p %s) p _ %s)" % (z, z, z),
            "(match p 0 %s _ when (> p 100) p _ %s)" % (z, z),
            "(match p 0 when (>= p %s) %s _ p)" % (z, z),
            # Leading wildcards and guarded wildcards, restored: both used to
            # break codegen on a non-ADT scrutinee (a NULL AdtDef deref and an
            # `else` with no `if`) regardless of the gate.
            "(match p _ when (>= p %s) p _ %s)" % (z, z),
            "(match p _ when (> p 100) p _ %s)" % z,
            "(match p _ %s)" % z,
            # Var patterns whose guard mentions the BINDER, not the scrutinee.
            # These could not be compiled at all until the binder was brought
            # into scope for its own guard, so they were never generated. They
            # are the sharpest guard rung available: a hypothesis synthesized
            # about `p` from a condition written about `x` is a fact about the
            # wrong name, and the last two make the two names disagree on
            # purpose -- `x` is shadowed or rebound, so reading the guard as if
            # it constrained `p` asserts something false.
            "(match p x when (>= x %s) x _ %s)" % (z, z),
            "(match p x when (> x 100) x _ %s)" % z,
            "(match p 0 %s x when (>= x %s) x _ %s)" % (z, z, z),
            "(match (- p p) x when (>= x %s) x _ %s)" % (z, z),
            "(let [x (- %s p)] (match p x when (>= x %s) x _ %s))" % (z, z, z),
            # constructor tag + record selector
            "(let [b (FzBox p p)] (match b (FzBox x y) x))",
            "(let [b (FzBox p p)] (match b (FzBox x y) (if (>= x %s) x %s)))" % (z, z),
            # two matches on one scrutinee: the tag facts must agree or the
            # inner arm is dead -- the one place a contradiction is CORRECT
            "(match p 0 %s _ (match p 0 %s _ p))" % (z, z),
        ])
        pred = self.rng.choice(["(>= r %s)" % z, "(> r %s)" % z,
                                "(<= r %s)" % z, "(>= r p)", "(= r p)"])
        return (["p"],
                "(defdata FzBox (FzBox [a : int b : int]))\n\n"
                "(defn target [p : %s] : #refine{ r : %s | %s }\n  %s)"
                % (self.ty, self.ty, pred, body))

    def shape_stateful(self):
        """C2 (#reads): a measure congruent inside a `frozen` region.

        Emits a `FzWorld`, an impure `#reads w` aliveness measure (inline-C, so
        the purity walk denies it -- congruence comes ONLY from the `#reads`
        grant), and a reader whose entity parameter carries a
        `#refine{ x | (fz-alive? w x) }` crossing. `target` guards the read with
        the measure inside a `(let [_ (& w)] ...)` frozen region, so the
        crossing DISCHARGES from the guard and gate-on elides it.

        What this shape does and does NOT test:

          * DOES: that a #reads program compiles and RUNS identically under both
            gates. Before the entry-check-suppression fix an impure #refine
            param was TUR-E0375 at its definition and could not codegen at all;
            this shape is the end-to-end regression guard for that path. A
            codegen divergence (gate-on returns a different value) or a crash on
            one gate is caught here as output-divergence / a gate-on abort.

          * Does NOT: exercise the soundness differential. A #reads measure is
            impure by construction, so gate-off carries NO runtime check for it
            (the entry contract is unemittable and is suppressed) -- there is no
            gate-off abort for a gate-on elision to contradict. #reads soundness
            is therefore a COMPILE-TIME property (a wrong crossing proof is a
            missed static error, never a runtime miscompile) and is pinned by
            tests/fixtures/errors/refine-stateful-* under --strict-refine plus
            the targeted frozen-check sabotage in the plan, not by this harness.

        int mode only (the world field and entity id are integers)."""
        if self.mode != "int":
            return self.shape_random()
        # fz-alive?(w, e) := e >= thr -- deterministic, a pure READ, but written
        # in inline-C so the purity walk denies it and #reads is what grants
        # congruence. A negative entity id takes the guard's else branch.
        thr = self.rng.choice([0, 1, -1])
        nval = self.rng.choice([1, 7, 42])
        defs = (
            "(defstruct FzWorld [n : int])\n\n"
            "(defn fz-alive? [^borrow w : FzWorld e : int] #reads w : bool\n"
            "  ```c\n"
            "  (void)w; return e >= %d;\n"
            "  ```)\n\n"
            "(defn fz-read! [^borrow w : FzWorld e : #refine{ x : int | (fz-alive? w x) }] : int\n"
            "  (.n w))\n\n"
            "(defn target [p : int] : int\n"
            "  (let [^mut w (FzWorld %d)]\n"
            "    (let [__frz (& w)]\n"
            "      (if (fz-alive? w p)\n"
            "        (fz-read! w p)\n"
            "        -1))))"
            % (thr, nval)
        )
        return (["p"], defs)

    def shape_integer(self):
        """Phase 4 of solver-integer-tail-plan: the integer tail's own shapes.

        Each rung is one of the nine probes that plan's section 1.3 found the
        solver could not decide before its Phase 1 -- `mod` and `/` by a
        literal, a square, a function wide enough that the counterexample
        search's old three-variable cap declined it -- generated as a family
        rather than a fixed instance, so the axioms are exercised on every
        divisor, sign and width the family reaches.

        The rungs deliberately mix TRUE refinements (proven, so the check is
        elided -- the elision is what the differential can then contradict)
        with refinements that are false only on one side of zero.  A `mod`
        remainder is non-negative for a non-negative dividend and non-positive
        for a negative one (C truncation, which is what `mod` compiles to); a
        refinement that claims `(>= r 0)` unconditionally is therefore false
        exactly when `main` passes a negative literal, and `main`'s literals
        range over both signs.  An encoder that asserted the SMT-LIB floor
        semantics instead would prove such a refinement and elide the check
        that catches it: BUG_soundness, the one class this harness exists for.

        The wide rungs have 3-5 parameters.  With a `:pre` bounding every
        parameter the goal is a genuine multi-variable LA proof (la_vars);
        without one it is false and the bounded model search has to find the
        witness across every parameter -- which is what the `model vars` /
        `model evals` rows of the cap sweep report, and what nothing in any
        swept population reached while the generator stopped at two.

        Int mode only: `mod` is not defined on floats and the square law is
        the only piece that would carry over."""
        if self.mode != "int":
            return self.shape_linear()
        z = "0"
        r = self.rng.random()
        if r < 0.22:
            # -- parity through mod.  Even in, even out (or provably odd).
            k = self.rng.choice([2, 3, 4])
            pre_res = self.rng.choice([0, 1])
            body, want = self.rng.choice([
                ("(+ p0 %d)" % k,           pre_res),            # residue kept
                ("(* p0 %d)" % (k + 1),     pre_res),            # k+1 == 1 (mod k)
                ("(+ p0 %d)" % (k * 2),     pre_res),
                ("(+ p0 1)",                (pre_res + 1) % k),  # residue moved
                ("(- p0 1)",                (pre_res - 1) % k),
            ])
            # `want` is the residue under the FLOOR reading.  Under C
            # truncation a negative dividend's remainder is <= 0, so with
            # pre-residue 0 (which admits negative multiples of k) the
            # "residue moved" rungs are false for every negative argument --
            # a real trap for an encoder that asserted floor semantics.
            if self.rng.random() < 0.25:
                # A wrong claim about the residue outright.
                want = (want + 1) % k
            pp = "(= (mod v %d) %d)" % (k, pre_res)
            rp = "(= (mod r %d) %d)" % (k, want)
            return (["p0"],
                    "(defn target [p0 : #refine{ v : int | %s }] : #refine{ r : int | %s }\n  %s)"
                    % (pp, rp, body))
        if r < 0.44:
            # -- `/` and `mod` by a literal, with the sign of the dividend
            # either bounded by the parameter or left free.
            k = self.rng.choice([-4, -3, -2, 2, 3, 4])
            ak = abs(k)
            op = self.rng.choice(["/", "mod"])
            body = "(%s p0 %d)" % (op, k)
            sign = self.rng.choice(["nonneg", "neg", "free"])
            pp = {"nonneg": "(>= v %s)" % z, "neg": "(< v %s)" % z, "free": None}[sign]
            # `holds`: true for every argument the entry check admits, under
            # C truncation (quotient toward zero, remainder with the sign of
            # the dividend, |r| <= |k|-1).  `breaks`: false for SOME admitted
            # argument -- at zero, or on one side of it -- so `main`'s
            # two-signed literals reach both a passing and an aborting run.
            # The labels describe the intent; the harness does not consume
            # them, it checks that discharge never disagrees with the runtime.
            neg_p0 = "(- %s p0)" % z
            if op == "/":
                if k > 0:
                    holds = {
                        "nonneg": ["(>= r %s)" % z, "(<= r p0)",
                                   "(and (>= r %s) (<= r p0))" % z],
                        "neg":    ["(<= r %s)" % z, "(>= r p0)"],
                        "free":   ["(or (>= r %s) (< p0 %s))" % (z, z)],
                    }[sign]
                else:
                    holds = {
                        "nonneg": ["(<= r %s)" % z, "(>= r %s)" % neg_p0],
                        "neg":    ["(>= r %s)" % z, "(<= r %s)" % neg_p0],
                        "free":   ["(or (<= r %s) (< p0 %s))" % (z, z)],
                    }[sign]
                breaks = ["(< r p0)", "(not= r %s)" % z,
                          "(>= r %s)" % z if k < 0 or sign == "neg" else "(< r %s)" % z]
            else:
                holds = {
                    "nonneg": ["(>= r %s)" % z, "(<= r %d)" % (ak - 1),
                               "(and (>= r %s) (<= r %d))" % (z, ak - 1)],
                    "neg":    ["(<= r %s)" % z, "(>= r %d)" % (1 - ak)],
                    "free":   ["(and (>= r %d) (<= r %d))" % (1 - ak, ak - 1),
                               "(or (>= r %s) (< p0 %s))" % (z, z)],
                }[sign]
                # The SMT-LIB floor/Euclidean reading would make the first of
                # these true; C truncation makes it false for a negative p0.
                breaks = ["(>= r %s)" % z, "(< r %d)" % (ak - 1),
                          "(not= r %s)" % z]
            rp = self.rng.choice(holds if self.rng.random() < 0.7 else breaks)
            if pp:
                return (["p0"],
                        "(defn target [p0 : #refine{ v : int | %s }] : #refine{ r : int | %s }\n  %s)"
                        % (pp, rp, body))
            return ["p0"], self._target(["p0"], rp, None, body)
        if r < 0.60:
            # -- squares: `(* e e)` with e a parameter or an offset of one.
            e = self.rng.choice(["p0", "(- p0 1)", "(+ p0 %s)" % self.lit()])
            body = self.rng.choice([
                "(* %s %s)" % (e, e),
                "(+ (* %s %s) 1)" % (e, e),
                "(- (* %s %s) p0)" % (e, e),      # not a square law: abstract
            ])
            rp = self.rng.choice([
                "(>= r %s)" % z,                  # true for the first two rungs
                "(> r %s)" % z,                   # false at the root
                "(>= r p0)",                      # x*x >= x needs more than the sign law
            ])
            return ["p0"], self._target(["p0"], rp, None, body)
        # -- wide: 3-5 parameters, the counterexample search's population.
        n = self.rng.randint(3, 5)
        params = ["p%d" % i for i in range(n)]
        terms = list(params)
        self.rng.shuffle(terms)
        body = terms[0]
        for t in terms[1:]:
            body = "(%s %s %s)" % (self.rng.choice(["+", "+", "-"]), body, t)
        if self.rng.random() < 0.5:
            # Bounded: every parameter non-negative, the goal a genuine LA
            # proof (or refutation, when a `-` slipped in) over all of them.
            pre = " ".join("(>= %s %s)" % (p, z) for p in params)
            pre = "(and %s)" % pre if n > 1 else pre
            rp = self.rng.choice(["(>= r %s)" % z, "(>= r %s)" % terms[0],
                                  "(> r -1)"])
            return params, self._target(params, rp, pre, body)
        # Unbounded: false, and the witness needs every parameter.
        rp = self.rng.choice(["(> r %s)" % z, "(>= r %s)" % z, "(not= r %s)" % z,
                              "(> r %s)" % terms[-1]])
        return params, self._target(params, rp, None, body)

    def shape_loop(self):
        """loop-invariants-plan LI5: a `while` carrying a written `:invariant`.

        Every rung is built so the property has teeth in both directions:

          * TRUE invariants the analysis should prove, so checks really are
            elided -- initiation, preservation (straight-line and branching
            bodies, a body `let`), and the post-loop fact the return
            refinement then leans on;
          * SABOTAGED invariants -- off by one, the wrong direction, a random
            predicate -- which are false at runtime, so the reference leg
            aborts and the discharge leg must NOT run clean;
          * bodies the analysis must DECLINE (an early `return`, a `&mut`
            borrow handed to a writing callee, a lambda cell) whose invariant
            is false -- if a decline ever regressed into a proof, the elided
            check is exactly what the soundness property catches.

        The loop counter is always an int; the accumulator has the mode's
        type, so float mode exercises the sort bookkeeping of the locals."""
        z = self._zero()
        T = self.ty
        one = "1" if self.mode == "int" else "1.5"
        if self.mode == "int":
            bound = self.rng.choice(["p", "3", "5", "(+ p 2)"])
            init = self.rng.choice([z, z, "p", self.lit()])
            scope = ["acc", "i", "p"]
        else:
            bound = self.rng.choice(["3", "5"])
            init = self.rng.choice([z, z, self.lit()])
            scope = ["acc"]
        pre_lines = []

        def step():
            if self.mode == "int":
                return self.rng.choice([one, "i", self.lit(), "(* i 2)"])
            return self.rng.choice([one, self.lit(), "(* %s 2.0)" % one])

        kind = self.rng.choice(["plain", "plain", "when", "if", "let", "twice",
                                "return", "mutborrow", "lambda"])
        if kind == "plain":
            body = "(set! acc (+ acc %s))" % step()
        elif kind == "when":
            body = "(when (> i 1) (set! acc (%s acc %s)))" % (
                self.rng.choice(["+", "-"]), step())
        elif kind == "if":
            body = "(if (> acc %s) (set! acc (- acc %s)) (set! acc (+ acc %s)))" % (
                self.lit(), step(), step())
        elif kind == "let":
            body = "(let [d %s] (set! acc (+ acc d)))" % step()
        elif kind == "twice":
            body = "(set! acc (+ acc %s))\n      (set! acc (- acc %s))" % (
                step(), step())
        elif kind == "return":
            body = "(when (> i 100) (return acc))\n      (set! acc (- acc %s))" % one
        elif kind == "mutborrow":
            pre_lines.append("(defn zap [r : &mut %s] : int (set! @r (- %s %s)) 0)"
                             % (T, z, one))
            body = "(zap &mut acc)"
        else:  # lambda
            body = "(bump)"

        invs = ["(>= acc %s)" % z,
                "(and (>= acc %s) (>= i 0))" % z,
                "(> acc %s)" % z,                 # off by one on entry
                "(<= acc %s)" % z,                # the wrong direction
                "(>= acc %s)" % init,
                self.pred(1, scope)]              # anything goes
        if self.mode == "int":
            invs.append("(and (>= i 0) (<= i %s))" % bound)
        inv = self.rng.choice(invs)
        ret = self.rng.choice(["(>= r %s)" % z, "(> r %s)" % z, "(<= r %s)" % z]
                              + (["(>= r p)"] if self.mode == "int" else []))
        binds = "[^mut acc %s\n        ^mut i   0" % init
        if kind == "lambda":
            binds += "\n        bump (fn [] : int (set! acc (- acc %s)) 0)" % one
        binds += "]"
        tail = self.rng.choice(["acc", "acc", "(+ acc %s)" % z])
        target = ("(defn target [p : %s] : #refine{ r : %s | %s }\n"
                  "  (let %s\n"
                  "    (while (< i %s) :invariant %s\n"
                  "      %s\n"
                  "      (set! i (+ i 1)))\n"
                  "    %s))" % (T, T, ret, binds, bound, inv, body, tail))
        return ["p"], "\n\n".join(pre_lines + [target])

    # Dyadic float literals: every sum of a handful of them is exact in double,
    # so a reflected `fz-sum` is never a float-rounding question.  That one is
    # docs/reported/float-proofs-assume-exact-reals.md, a separate known gap,
    # and it must not show up here as noise.
    EXACT_FLOATS = ["0.5", "1.25", "2.75", "3.25", "-1.5", "-4.25", "6.5"]

    def shape_reflect(self):
        """reflected-measures RF3/RF4: recursive `^reflect` measures over a list.

        `reflect_if` / `reflect_lie` put scalar, non-recursive measures into
        other shapes' programs; they never reach the encoder work RF3 and RF4
        added (reflect-fuzz-never-reaches-the-rf3-rf4-encoder).  This shape does:

          * recursive unfolding through a `match` -- `fz-len`, `fz-sum`,
            `fz-allpos?`, `fz-sorted?` (a nested match) -- at constructor-headed
            literals of depth 0..11, so the default fuel of 8 is crossed in
            both directions;
          * a scalar `p` at the head of the list, so the unfolded equation still
            has a variable in it;
          * RF4: a match over a list PARAMETER whose refinement is a reflected
            measure, so the arm is selected from a tag fact;
          * a SABOTAGED sibling for each rung -- a refinement the measure does
            not satisfy (off by one, a zeroed head) -- so a wrong unfolding is a
            BUG_soundness, not a silent agreement;
          * a non-exhaustive and an impure `^reflect` body, which RF2 and RF1's
            purity half must REJECT (skip_invalid on both legs)."""
        T = self.ty
        z = self._zero()
        is_int = self.mode == "int"

        def elem(pos=None):
            if is_int:
                v = self.rng.randint(1, 6) if pos else self.rng.randint(-6, 6)
                return str(v)
            pool = [x for x in self.EXACT_FLOATS if not pos or not x.startswith("-")]
            return self.rng.choice(pool)

        def num(s):
            return int(s) if is_int else float(s)

        def lit_of(v):
            return str(v) if is_int else repr(float(v))

        def lst(elems, head=None):
            items = ([head] if head else []) + elems
            out = "(FzNil)"
            for e in reversed(items):
                out = "(FzCons %s %s)" % (e, out)
            return out

        pre = ["(defdata FzLst [] (FzCons [hd : %s tl : FzLst]) (FzNil))" % T,
               "(defn ^reflect fz-len [xs : FzLst] : int\n"
               "  (match xs\n    (FzNil) 0\n    (FzCons h t) (+ 1 (fz-len t))))",
               "(defn ^reflect fz-sum [xs : FzLst] : %s\n"
               "  (match xs\n    (FzNil) %s\n    (FzCons h t) (+ h (fz-sum t))))" % (T, z),
               "(defn ^reflect fz-allpos? [xs : FzLst] : bool\n"
               "  (match xs\n    (FzNil) true\n"
               "    (FzCons h t) (and (> h %s) (fz-allpos? t))))" % z,
               "(defn ^reflect fz-sorted? [xs : FzLst] : bool\n"
               "  (match xs\n    (FzNil) true\n"
               "    (FzCons h t) (match t\n"
               "                   (FzNil) true\n"
               "                   (FzCons h2 _) (and (<= h h2) (fz-sorted? t)))))"]
        kind = self.rng.choice(["len_ground", "len_param", "sum", "allpos",
                                "rf4", "rf4", "sorted_rf4", "nonexh", "impure"])
        ret_ty = "int"
        pre_cond = None
        if kind == "len_ground":
            d = self.rng.randint(0, 11)
            body = "(fz-len %s)" % lst([elem() for _ in range(d)])
            pred = self.rng.choice(["(= r %d)" % d, "(= r %d)" % (d + 1),
                                    "(>= r 0)", "(> r %d)" % d])
        elif kind == "len_param":
            d = self.rng.randint(0, 10)
            body = "(fz-len %s)" % lst([elem() for _ in range(d)], head="p")
            pred = self.rng.choice(["(= r %d)" % (d + 1), "(= r %d)" % d,
                                    "(> r 0)", "(< r %d)" % (d + 1)])
        elif kind == "sum":
            d = self.rng.randint(0, 9)
            rest = [elem() for _ in range(d)]
            s = sum(num(x) for x in rest)
            body = "(fz-sum %s)" % lst(rest, head="p")
            off = 1 if is_int else 0.5
            pred = self.rng.choice(["(= r (+ p %s))" % lit_of(s),
                                    "(= r (+ p %s))" % lit_of(s + off),
                                    "(>= r p)", "(> r p)"])
            ret_ty = T
        elif kind == "allpos":
            d = self.rng.randint(0, 9)
            rest = [elem(pos=self.rng.random() < 0.8) for _ in range(d)]
            body = "(if (fz-allpos? %s) 1 0)" % lst(rest, head="p")
            pred = self.rng.choice(["(= r 1)", "(= r 0)", "(>= r 0)"])
            if self.rng.random() < 0.5:
                pre_cond = "(> p %s)" % z
        elif kind in ("rf4", "sorted_rf4"):
            # A list PARAMETER refined by a measure, and a match over it: the
            # arm is selected from the tag fact (RF4).  The honest body returns
            # what the refinement promises; the sabotaged one zeroes (or
            # bumps) the head, so the measure no longer holds of what it
            # unfolds -- the return refinement is false at runtime.
            m = "fz-allpos?" if kind == "rf4" else "fz-sorted?"
            honest = self.rng.random() < 0.5
            if kind == "rf4":
                tail = "(%s t)" % m if honest else "(%s (FzCons (- h h) t))" % m
            else:
                tail = ("(match t (FzNil) true (FzCons h2 t2) (%s t))" % m if honest
                        else "(%s (FzCons (+ h %s) t))" % (m, "100" if is_int else "100.5"))
            pre.append("(defn inner [xs : #refine{ v : FzLst | (%s v) }]"
                       " : #refine{ r : bool | r }\n"
                       "  (match xs\n    (FzNil) true\n    (FzCons h t) %s))" % (m, tail))
            if kind == "rf4":
                rest = [elem(pos=True) for _ in range(self.rng.randint(0, 4))]
            else:
                vals = sorted(num(elem()) for _ in range(self.rng.randint(0, 4)))
                rest = [lit_of(v) for v in vals]
            body = "(if (inner %s) 1 0)" % lst(rest, head="p")
            pred = self.rng.choice(["(= r 1)", "(>= r 0)"])
            if self.rng.random() < 0.5:
                pre_cond = "(> p %s)" % z
        elif kind == "nonexh":
            # RF2: `^non-exhaustive` is an unchecked promise the ordinary match
            # checker accepts, so only the reflection gate stands between it and
            # an admitted equation.  (A plain non-exhaustive match never got that
            # far: it is a compile error before any gate runs.)
            pre.append("(defn ^reflect fz-ne [xs : FzLst] : int\n"
                       "  (match ^non-exhaustive xs (FzCons h t) (+ 1 (fz-ne t))))")
            body = "(fz-ne %s)" % lst([elem()], head="p")
            pred = "(= r 1)"
        else:  # impure
            # RF1's purity half: a measure whose body runs inline C.
            pre.append("(defn fz-tick [] #fx{} : int\n"
                       "  ```c\n  static int64_t n = 0;\n  return n++;\n  ```)")
            pre.append("(defn ^reflect fz-imp [xs : FzLst] : int\n"
                       "  (match xs\n    (FzNil) (fz-tick)\n"
                       "    (FzCons h t) (+ 1 (fz-imp t))))")
            body = "(fz-imp %s)" % lst([elem()], head="p")
            pred = "(= r 1)"
        target = ("(defn target [p : %s] : #refine{ r : %s | %s }%s\n  %s)"
                  % (T, ret_ty, pred, "\n  :pre " + pre_cond if pre_cond else "", body))
        return ["p"], "\n\n".join(pre + [target])

    def program(self):
        # `--only-shape reflect` runs the reflected population at density: no
        # random helpers (a `reflect_lie` helper rejects the whole program on
        # both legs) and no extra targets (a refuted one does too).  Each would
        # waste the case before the measure under test is reached.  Mixed runs
        # keep both, so the reflect slice there shares the usual neighbours.
        dense = self.only == "reflect"
        lines = [] if dense else self.gen_helpers(self.rng.randint(1, 3))
        lines += [] if dense else self.gen_bool_helpers(self.rng.randint(1, 2))
        r = self.rng.random()
        if self.only:
            params, target = getattr(self, "shape_" + self.only)()
        elif r < 0.04:
            params, target = self.shape_random()
        elif r < 0.07:
            # reflected-measures RF3/RF4, carved out of shape_random's slice
            # the way shape_loop was.  `--only-shape reflect` runs it at
            # density.
            params, target = self.shape_reflect()
        elif r < 0.15:
            # loop-invariants-plan LI5: carved out of shape_random's slice, so
            # every other shape keeps the population it had.
            params, target = self.shape_loop()
        elif r < 0.33:
            params, target = self.shape_linear()
        elif r < 0.49:
            params, target = self.shape_congruence()
        elif r < 0.60:
            params, target = self.shape_param()
        elif r < 0.66:
            params, target = self.shape_propagate()
        elif r < 0.74:
            params, target = self.shape_typeclass()
        elif r < 0.80:
            params, target = self.shape_congruence_method()
        elif r < 0.86:
            params, target = self.shape_branching()
        elif r < 0.91:
            params, target = self.shape_datatype()
        elif r < 0.96:
            params, target = self.shape_integer()
        else:
            params, target = self.shape_stateful()
        lines.append(target)
        extra, extra_names = ([], []) if dense else self.extra_targets()
        lines += extra
        main = self._main(params)
        calls = "\n".join("  (println (%s %s))" % (n, self.lit()) for n in extra_names)
        main = main.replace("\n  0)", "\n" + calls + "\n  0)")
        lines.append(main)
        return "\n\n".join(lines) + "\n"


# ---------------------------------------------------------------------------
# Running one case
# ---------------------------------------------------------------------------

TIMEOUT = 60

STATS_RE = re.compile(
    r"refine: (\d+) obligation\(s\): (\d+) proven, (\d+) refuted, (\d+) unknown")


class Outcome:
    """What one `tur run` did.  `kind` is one of clean / abort / reject / timeout."""

    def __init__(self, kind, stdout="", proven=0, refuted=0):
        self.kind = kind
        self.stdout = stdout
        self.proven = proven
        self.refuted = refuted


def run_gate(tur, path, refined):
    """Compile+run one leg.  `refined=False` is the REFERENCE leg: no obligation
    is discharged, so every refinement keeps its runtime check.

    Until refinement types graduated (v0.33.0) that leg was spelled "omit
    --enable=refined".  Graduation made the flag a no-op, which would have made
    both legs identical builds and this whole harness vacuous while it kept
    reporting PASS.  TUR_REFINE_NO_DISCHARGE is the replacement -- an env-only
    test seam, deliberately not a CLI flag or an experiment.  No shipping flag
    reconstructs this leg: --no-contracts emits NO checks, --keep-contracts
    emits checks MINUS the elided ones, and the elided set is exactly what the
    miscompile property below is about.
    """
    # Both experiments are enabled on both legs.
    # reflected-measures: the `^reflect` helper kinds need the gate on both
    # legs (the reference leg still suppresses discharge, so the equations
    # are asserted into VCs that then decide nothing).
    # loop-invariants-plan LI5: a generated `:invariant` is ACTED on in both
    # legs (always on since loop-invariants graduated) -- runtime checks in the
    # reference leg, checks minus proofs in the discharge leg.
    cmd = [tur, "--enable=reflected-measures", "run", path]
    env = dict(os.environ)
    # See the note in tests/type-fuzz-src.py: a shimmed `python3` (mise, asdf)
    # can re-export another install's TUR_STDLIB_DIR inside this process, which
    # silently compiles the tree's compiler against a foreign stdlib. Strip it
    # so the compiler under test uses the stdlib beside it.
    env.pop("TUR_STDLIB_DIR", None)
    env["TUR_REFINE_STATS"] = "1"
    if not refined:
        env["TUR_REFINE_NO_DISCHARGE"] = "1"
    # The generated programs are tiny and self-contained; leak checking the
    # spawned binary is not what this harness is measuring.
    env["ASAN_OPTIONS"] = env.get("ASAN_OPTIONS", "") or "detect_leaks=0"
    # Arm clang's function-pointer detector (tests/fuzz_arm.py).
    env, _ = fuzz_arm.armed_env(env)
    try:
        p = subprocess.run(cmd, capture_output=True, text=True,
                           timeout=TIMEOUT, cwd=REPO, env=env)
    except subprocess.TimeoutExpired:
        return Outcome("timeout")

    proven = refuted = 0
    m = STATS_RE.search(p.stderr)
    if m:
        proven, refuted = int(m.group(2)), int(m.group(3))

    if p.returncode == 0:
        return Outcome("clean", p.stdout, proven, refuted)
    # A trapped mismatched function-pointer call.  Without this arm it fell
    # through to "reject" and classify() dropped it as skip_invalid.
    if fuzz_arm.is_fnsan_trap(p.returncode):
        return Outcome("fnptr_trap", p.stdout, proven, refuted)
    # SIGABRT (134) and SIGSEGV (139) both mean the program was built and then
    # died; a contract violation is the 134 case.
    if p.returncode in (134, 139) or p.returncode < 0:
        return Outcome("abort", p.stdout, proven, refuted)
    return Outcome("reject", p.stdout, proven, refuted)


# Classification.  BUG_* fail the run; SUSPICIOUS is reported only.
BUGS = ("BUG_soundness", "BUG_output_divergence", "BUG_new_abort",
        "BUG_fnptr_trap")


def classify(off, on):
    if off.kind == "fnptr_trap" or on.kind == "fnptr_trap":
        return fuzz_arm.TRAP_CLASS     # report-only unless strict
    if off.kind == "timeout" or on.kind == "timeout":
        return "skip_timeout"
    if off.kind == "reject":
        # The generator emitted something that does not compile at all.  Not a
        # refinement result; drop the case.
        return "skip_invalid"

    if off.kind == "clean":
        if on.kind == "clean":
            return ("agree_clean" if off.stdout == on.stdout
                    else "BUG_output_divergence")
        if on.kind == "abort":
            return "BUG_new_abort"
        return "SUSPICIOUS_over_refute"

    # off.kind == "abort": the program violates its own refinement.
    if on.kind == "clean":
        return "BUG_soundness"
    if on.kind == "abort":
        return "agree_abort"
    return "agree_rejected_early"


def one_case(tur, workdir, idx, seed, mode, only=None):
    rng = random.Random(seed)
    src = Gen(rng, mode, only).program()
    # Gate-off and gate-on need DISTINCT paths: `tur run` derives its
    # intermediate .c name from the source path, so a shared name would let two
    # workers (or the two gates) collide in /tmp/tur-build.
    base = os.path.join(workdir, "c%06d" % idx)
    off_path, on_path = base + "_off.tur", base + "_on.tur"
    for p in (off_path, on_path):
        with open(p, "w") as f:
            f.write(src)
    off = run_gate(tur, off_path, False)
    on = run_gate(tur, on_path, True)
    for p in (off_path, on_path):
        try:
            os.unlink(p)
        except OSError:
            pass
    return classify(off, on), src, off, on


# ---------------------------------------------------------------------------
# Self-test: does the plumbing classify the pinned fixtures correctly?
# ---------------------------------------------------------------------------

SELF_TESTS = [
    # (fixture, expected classification)
    ("tests/fixtures/errors/refine-impure-fx-empty/input.tur",
     "agree_rejected_early"),
    ("tests/fixtures/errors/refine-impure-not-congruent/input.tur",
     "agree_rejected_early"),
    ("tests/fixtures/refine-proved/input.tur", "agree_clean"),
    ("tests/fixtures/refine-measure-euf/input.tur", "agree_clean"),
    ("tests/fixtures/refine-class-result/input.tur", "agree_clean"),
    # Runs CLEAN with the gate off -- its single call returns 7, which satisfies
    # the instance's weaker promise -- and is rejected with the gate on, because
    # E0374 is a declaration-vs-declaration inconsistency rather than a claim
    # about the value this program produced. That is `SUSPICIOUS_over_refute` by
    # definition, and pinning it here documents that the bucket holds legitimate
    # DECLARATION errors too, not only universal value refutations.
    ("tests/fixtures/errors/refine-instance-result-weaker/input.tur",
     "SUSPICIOUS_over_refute"),
]


def self_test(tur):
    ok = True
    for rel, want in SELF_TESTS:
        path = os.path.join(REPO, rel)
        off = run_gate(tur, path, False)
        on = run_gate(tur, path, True)
        got = classify(off, on)
        status = "ok " if got == want else "FAIL"
        if got != want:
            ok = False
        print("  %s %-52s want=%s got=%s (off=%s on=%s)"
              % (status, rel.split("/")[-2], want, got, off.kind, on.kind))
    # The two regression fixtures must ALSO show that gate-off actually aborts
    # -- that is the half of the property the classification alone does not
    # pin, since `reject` under both gates would look the same.
    for rel, _ in SELF_TESTS[:2]:
        off = run_gate(tur, os.path.join(REPO, rel), False)
        if off.kind != "abort":
            print("  FAIL %s: gate-off should abort, got %s" % (rel, off.kind))
            ok = False
    return ok


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
    ap.add_argument("--n", type=int, default=300, help="cases to generate")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 1))
    ap.add_argument("--mode", choices=["int", "float", "both"], default="both")
    ap.add_argument("--tur", default=os.path.join(REPO, "build", "tur"))
    ap.add_argument("--save-dir", default=None,
                    help="where to write failing cases (default: a temp dir)")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--only-shape", default=None,
                    help="generate only this shape (e.g. `loop` -- the "
                         "loop-invariants-plan LI5 population)")
    args = ap.parse_args()

    tur = os.path.abspath(args.tur)
    if not os.path.exists(tur):
        print("refine_fuzz_src: no compiler at %s" % tur, file=sys.stderr)
        return 2

    if args.self_test:
        print("refine_fuzz_src: self-test (%s)" % tur)
        ok = self_test(tur)
        print("refine_fuzz_src: self-test %s" % ("PASS" if ok else "FAIL"))
        return 0 if ok else 1

    save_dir = args.save_dir or tempfile.mkdtemp(prefix="refine-fuzz-src-")
    os.makedirs(save_dir, exist_ok=True)
    workdir = tempfile.mkdtemp(prefix="refine-fuzz-work-")

    counts = {}
    findings = []
    total_proven = total_refuted = 0

    def job(i):
        mode = args.mode
        if mode == "both":
            mode = "int" if (i % 2 == 0) else "float"
        return one_case(tur, workdir, i, args.seed * 1000003 + i, mode,
                        args.only_shape)

    print("refine_fuzz_src: %d cases, seed %d, mode %s, %d job(s)"
          % (args.n, args.seed, args.mode, args.jobs))
    print("refine_fuzz_src: " + fuzz_arm.armed_env(dict(os.environ))[1])
    try:
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            futs = {pool.submit(job, i): i for i in range(args.n)}
            done = 0
            for fut in as_completed(futs):
                i = futs[fut]
                kind, src, off, on = fut.result()
                counts[kind] = counts.get(kind, 0) + 1
                total_proven += on.proven
                total_refuted += on.refuted
                if kind.startswith("BUG") or kind.startswith("SUSPICIOUS"):
                    findings.append((kind, i, src, off, on))
                # skip_* verdicts are routine here (a generated program the
                # checker rejects); only BUG/SUSPICIOUS earn their own line.
                _progress(done, args.n,
                          kind if kind.startswith(("BUG", "SUSPICIOUS")) else "ok")
                done += 1
        findings.sort(key=lambda t: t[1])
    finally:
        shutil.rmtree(workdir, ignore_errors=True)

    for kind, i, src, off, on in findings:
        d = os.path.join(save_dir, "%s-%06d" % (kind, i))
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "input.tur"), "w") as f:
            f.write(src)
        with open(os.path.join(d, "README"), "w") as f:
            f.write("classification: %s\nseed base: %d  case: %d\n"
                    "gate-off: %s\n  stdout: %r\n"
                    "gate-on : %s\n  stdout: %r\n"
                    % (kind, args.seed, i, off.kind, off.stdout,
                       on.kind, on.stdout))

    print("\nrefine_fuzz_src: %d cases" % args.n)
    for k in sorted(counts):
        print("  %-28s : %d" % (k, counts[k]))
    print("  %-28s : %d proven, %d refuted"
          % ("obligations decided", total_proven, total_refuted))

    n_bugs = sum(counts.get(b, 0) for b in BUGS)
    n_susp = counts.get("SUSPICIOUS_over_refute", 0)
    print("\n  SOUNDNESS bugs              : %d" % counts.get("BUG_soundness", 0))
    print("  other BUG classes           : %d" % (n_bugs - counts.get("BUG_soundness", 0)))
    print("  suspicious (report-only)    : %d" % n_susp)
    if findings:
        print("\n  saved to %s" % save_dir)
    elif not args.save_dir:
        shutil.rmtree(save_dir, ignore_errors=True)

    if total_proven == 0:
        print("\n  WARNING: nothing was proven in any case -- the solver never "
              "engaged, so a clean run means very little.")
    return 1 if n_bugs else 0


if __name__ == "__main__":
    sys.exit(main())
