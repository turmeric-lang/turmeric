# Refinement Types -- Prototype Plan (RT0--RT7 + in-house solver S0--S4)

**Status: CLOSED. RT0--RT7 and S0--S4 all landed, and the `refined` experiment
graduated 2026-08-01 (v0.33.0).** Nothing here is outstanding work. The file is
archived rather than retired because it remains the **status source** for the
refinement feature and for the `tests/fixtures/refine-*` corpus -- read it as a
record, per the two-document note below. Work that builds on it and is still
open lives in [`loop-invariants-plan.md`](loop-invariants-plan.md)
(landed 2026-09-29 behind `--enable=loop-invariants`) and
[`reflected-measures-plan.md`](../upcoming/reflected-measures-plan.md)
(in progress since 2026-09-29, behind `--enable=reflected-measures`).

## How to read this file -- it is TWO documents

**This file is a journal stacked on top of a design document. They have
different reliability, and mixing them up is the mistake this note exists to
prevent.**

| Part | Where | What it is | Trust it for |
|---|---|---|---|
| **1. Engineering journal** | ["Landed so far"](#landed-so-far-2026-07-24) down to the **"Part 2 -- the original design plan"** banner (~45--1820) | Dated entries, 2026-07-24 through 2026-07-30, written as the work landed. Blockquoted throughout. Verified against the source tree. | **Status. This is the status source.** What shipped, what deviated, what was measured, what was declined and why. |
| **2. Original design plan** | the **"Part 2 -- the original design plan"** banner to the end (~1821--end) | The plan as written *before* implementation: Motivation, Background, Design Decisions, Architecture Overview, Phases RT0--RT7, S0--S4, Implementation Order, Effort Estimates, Open Questions, References. | **Reasoning only.** Why the design is shaped this way -- the solver seam, checking-not-inference, Z3-as-scaffolding, the predicate fragment. |

**Part 2 is NOT a status source.** It is retained for its reasoning, which is
still good, but it describes an intended implementation that the journal then
deviated from in several named places. Where the two disagree, **the journal
wins**. Known deviations are called out inline in Part 2 (search for
`SUPERSEDED` and `CORRECTED`), but treat any un-annotated implementation detail
in Part 2 -- file names, diagnostic assignments, stdlib contents, phase gating
-- as a proposal rather than a fact.

**Graduation (2026-08-01).** The `refined` experiment has **graduated**. Static
discharge of `#refine{...}` is now **unconditional** -- there is no gate. The
`EXPERIMENTS[]` and `LANG_LAYERS[]` rows are deleted, `g_opt_refined` is gone,
and `"refined"` moved to `GRADUATED[]` / `GRADUATED_LAYERS[]`: a lingering
`--enable=refined` is a `TUR-W0063` no-op and a lingering `#lang turmeric
refined` a `TUR-W0064` no-op, both aging out one minor line later. Targets
v0.33.0. See
[refined-graduation-plan.md](refined-graduation-plan.md). **Every "gated behind
`--enable=refined`" sentence anywhere in this file is historical.**

**Sibling plans.** Two follow-on plans build directly on this one and account
for fixtures a reader will find under `tests/fixtures/refine-*` that this plan
never mentions:

- [refine-predicate-measures-plan.md](refine-predicate-measures-plan.md)
  (`RM-B`, boolean-sorted measures -- LANDED RM-B0..RM-B3). Backs
  `refine-bool-measure`, `refine-float-measure`, `refine-measure-euf`.
- [refine-stateful-measures-plan.md](refine-stateful-measures-plan.md)
  (`RM-S`, refinements over mutable state -- LANDED as Candidate B: `#reads`
  plus the borrow-based `frozen` region. RM-S1, the epoch design, was not
  pursued). Backs the `refine-stateful-*` fixtures.

---

> ## Landed so far (2026-07-24)
>
> **RT0--RT3 and S0--S3 are implemented and on `main`'s feature branch**, gated
> behind `--enable=refined` / `#lang turmeric refined`. RT5b (`stdlib/refine.tur`)
> landed with them. The user-facing write-up is
> [docs/guides/refinement-types-guide.md](../guides/refinement-types-guide.md).
>
> | Phase | State | Where |
> |---|---|---|
> | RT0 gate + discharge bit + pipeline hook | done | `experiments.c`, `lang_layers.c`, `elab_fns.c`, `elab_toplevel.c` |
> | RT1 constraint collector + env | done (incl. call-site crossings) | `refine_collect.{c,h}` |
> | RT2 normalized VC + seam + SMT-LIB | done | `refine_vc.{c,h}`, `refine_smtlib.{c,h}` |
> | RT3 discharge chain + diagnostics | done | `refine_discharge.{c,h}` |
> | RT3 Z3 scaffold (dev-only) | done, EXERCISED against Z3 4.13 | `refine_libz3.c`, `TUR_REFINE_Z3_ORACLE` |
> | RT3 differential fuzzing vs Z3 (VC level) | done | `tests/unit/refine_fuzz.c` |
> | Source-level differential fuzzing (gate off vs on) | done | `tests/refine-fuzz-src.py` |
> | S0 trivial | done | `refine_solver_s0.c` |
> | S1 congruence closure (EUF) | done | `refine_solver_euf.c` |
> | S2 linear arithmetic | done (Fourier-Motzkin, not simplex -- see below) | `refine_solver_arith.c` |
> | S3 Nelson-Oppen | done (convex exchange; no integer case-split) | `refine_solver_no.c` |
> | S4 boolean structure | done (small-DNF cube expansion) | `refine_solver.c` |
> | RT5b stdlib refinement aliases | done | `stdlib/refine.tur` |
> | RT4 predicate propagation | done (+ declared-result propagation) | `elab_fns.c`, `refine_collect.c` |
> | RT6 error message quality | done | `refine_discharge.c` |
> | RT5a WASM confirm | done (compiles AND agrees at wasm32) | `tests/run-refine-wasm.sh` |
> | RT7 caching | within-unit memo done; persistent cache NOT done (see below) | `refine_vc.c`, `refine_discharge.c` |
>
> ### Deliberate deviations from the plan as written
>
> - **S2 is Fourier-Motzkin over exact rationals, not S2a-Bellman-Ford-then-
>   S2b-simplex.** FM is a strict superset of difference logic (so the S2a
>   coverage target -- `0 <= i`, `i < len`, `i + 1 <= n` -- is met), decides
>   conjunctions of linear constraints outright over the rationals, and is a few
>   hundred lines rather than the multi-week incremental-simplex investment. Its
>   weakness is combinatorial blow-up, which the `Unknown` escape hatch absorbs:
>   `REFINE_MAX_LA_CONSTR` bounds growth and a cap hit degrades to a runtime
>   check. Integer strict constraints are tightened (`e < 0` => `e <= -1`), which
>   is what discharges `x > 0 |= 2x > 0`. If profiling ever shows the cap biting
>   on real obligations, the Dutertre--de Moura simplex slots in behind the same
>   `la_*` interface with no change above it.
> - **S4 is not a separate stage.** The small-DNF cube expansion the plan
>   assigns to S4 is shared machinery in `refine_solver.c` that S1/S2/S3 all run
>   over, because every theory stage needs conjunctions of literals to work on.
>   The cap-to-`Unknown` behavior is as specified; no DPLL(T) engine exists or
>   is planned.
> - **`RT_INVALID` comes from a bounded counterexample search**, not from a
>   theory solver. The proving stages only ever answer Valid or Unknown -- they
>   refute the negated goal, and failing to refute proves nothing. So after the
>   chain comes back Unknown, `refine_model_search` enumerates a small candidate
>   assignment space and *evaluates* `hyps AND (not goal)` exactly; a satisfying
>   assignment is a genuine witness, which is what makes `TUR-E0371` an
>   actionable error with a model rather than a guess. It declines VCs containing
>   uninterpreted symbols (a measure has no fixed interpretation to evaluate).
> - **The `refined` experiment's `introduced`/`expires_at` are `0.31.0` /
>   `0.34.0`** (VERSION was 0.30.8 at landing).
>
> ### Bugs found and fixed on the way (all pre-existing, all in the CT layer)
>
> 1. **A `#refine{...}` parameter got no runtime check at all.** `[x : #refine{...}]`
>    reaches the elaborator wrapped in an `F_TYPE_ANN` (the `:` separator), and
>    the CT1 predicate collector only unwrapped a bare `F_CONTRACT_TYPE` -- the
>    spelling the migration away from bare braces left behind. The predicate was
>    silently dropped. Now collected from the RESOLVED TYPE instead of the Form,
>    which fixes all three spellings at once (bare brace, `: #refine{...}`, and a
>    named alias, which has no contract Form at the use site).
> 2. **A contract RETURN type did not compile.** `: #refine{ r : int | p }` left
>    `return_kind == TY_CONTRACT`, so every call site failed to type the result
>    (`got { _ : ? | ... }`). Now peeled to the base type, with the predicate
>    riding along as a postcondition.
> 3. **A refinement predicate's free names were mistaken for implicit type
>    parameters.** `form_mentions_type_param` walked the whole `F_CONTRACT_TYPE`
>    including the predicate, so `[v : int, n : int]` with a `(len v)` refinement
>    lost both parameters to type-variable inference and then failed to parse
>    their annotations. It now descends only into the base-type slot.
> 4. **`deftype` of a contract body produced a `TY_REC`**, making every use site
>    fail with `expected <rec>, got int`. A contract body now binds the name
>    directly to the contract type -- which is what makes `stdlib/refine.tur`
>    possible at all.
>
> ### Call-site crossings (landed 2026-07-24)
>
> RT1's first table row -- passing an argument where the parameter declares a
> refinement -- is now collected and discharged. `(safe-div 10 0)` is a compile
> error rather than a runtime panic.
>
> Three implementation decisions worth recording:
>
> - **Resolution is DEFERRED to after the whole unit elaborates.** At the moment
>   a call is elaborated the callee may still be a pass-1 forward declaration
>   with no predicates stamped. Because `elab_defn` *reuses* that binding rather
>   than replacing it, recording `(callee-binding, call-form)` during
>   elaboration and resolving afterwards makes the check independent of
>   definition order -- a call to a later-defined function behaves exactly like
>   a call to an earlier-defined one. Crossings are deduplicated through a hash
>   set, not a linear rescan, so an opted-in build does not go quadratic in its
>   call count.
> - **The caller's hypothesis environment is BACK-FILLED, not threaded.**
>   `elab_defn` marks the crossing range its body produced and stamps the
>   environment once the body is done. A mutable "current env" on `Elab` would
>   have to be unwound correctly through that function's many early-error
>   returns, and a stale one left behind by a failed path would hand a later
>   crossing hypotheses that do not hold there -- the exact direction the
>   soundness invariant forbids.
> - **An unproven argument is NOT an error by default; a definitely-wrong one
>   is.** The split is on whether the counterexample is *closed*: a goal with no
>   free variables evaluates to false on the values written at the site, which
>   is a defect. An open counterexample only says "not for every input", which
>   for an argument is the ordinary condition of partially-annotated code -- and
>   the callee still checks on entry. Erroring on it would have made the
>   experiment impossible to adopt incrementally. `--strict-refine` opts into
>   the stricter reading. (A function's own return refinement keeps the strict
>   treatment either way: an open counterexample there means the function's
>   claim about *itself* is false.)
>
> The callee's entry check is still always emitted. Eliding it needs
> whole-program call-graph knowledge including exported and indirect callers,
> and that is a separate piece of work with real soundness preconditions.
>
> ### RT4 predicate propagation (landed 2026-07-24)
>
> Landed with a piece the plan does not name, and which turned out to matter
> more than the templates: **propagating DECLARED return refinements into call
> results.** A call encodes as an uninterpreted term either way, but when the
> callee has a return refinement that predicate is a fact about the value the
> call produced. Asserting it is what makes refined code compose --
> `(twice (double-pos p))` goes from unknown to proved -- and without it RT4's
> inferred refinements would have had nowhere to be used. Both flow through the
> same `RefineFnResolver` hook, so `refine_collect.c` stays free of scope and
> binding knowledge.
>
> The soundness argument for assuming a declared refinement is that something
> enforces it: it was proved statically, or the runtime check that guarantees
> it would have panicked first. That is conditional, and the condition is
> checked -- when contracts are stripped (`--no-contracts`, or a release build
> without `--keep-contracts`) an unproved refinement is **not** published, and
> call sites go back to treating the result as opaque. A first cut put a
> blanket `g_no_contracts` refusal in the resolver instead; that was both
> redundant (the publish gate already knows) and wrong (it discarded INFERRED
> refinements, which are proved facts and hold regardless of any check).
>
> The template layer itself is what the plan describes: five shapes tried
> against the body, first proved one wins, nothing emitted. It is emphatically
> not inference -- no search over a recursive constraint system, just guesses
> that are individually checked, so a wrong guess costs a discarded proof
> attempt rather than a wrong answer. `tests/fixtures/errors/refine-
> unpropagated-result` is the fixture that would go green if that ever stopped
> being true.
>
> Known asymmetry: call-site crossings are resolved after the whole unit, so
> they see every callee's refinement; a function's own return obligation is
> decided inline (that is what lets its check be elided), so it only sees
> functions already elaborated. Under mutual recursion one direction may miss
> one. This can only lose a hypothesis, never add a false one.
>
> ### RT6 error message quality (landed 2026-07-24)
>
> A failing obligation now reports claim -> witness -> remedy: the predicate as
> the user wrote it (rendered through `fmt_print`, so it matches the source),
> the counterexample, and a `help:` line naming a fact that would discharge it.
>
> The hint is the plan's "second seam query" taken literally: a candidate is
> asserted as a hypothesis and the chain is asked again, so only a candidate
> that genuinely discharges the goal is offered. Two candidate families --
> comparisons against literals the VC already mentions, and relations between
> two variables. The second family is what produces `(< i n)` for an index
> obligation; no literal bound can express it, and that is the shape the plan's
> `SizedVec` motivation is about.
>
> Two guards, both learned by writing the tests:
>
> - **A contradictory candidate is rejected.** Adding `(> x 0)` when `x < 0` is
>   already known makes the hypotheses unsatisfiable, which discharges the goal
>   by ex falso -- and would have the compiler suggest constraining a variable
>   to be both negative and positive. Each candidate is checked for
>   satisfiability before it is offered.
> - **An already-valid obligation gets no hint.** The discharge pass only calls
>   the search on a failure, so this was unreachable in practice; the check
>   belongs in the function anyway, because the function is exported and should
>   be correct for any caller.
>
> `refine_hint_search` is exported rather than static specifically so the
> "never suggests a contradiction" property can be unit-tested. Substring
> matching over a fixture's stderr can confirm a hint *appears*; it cannot
> confirm a wrong one does not, and that is the property that matters.
>
> Deviation from the acceptance criteria: the plan asks for snapshots in
> `tests/fixtures/refine/error-messages/` diffed by the session-types snapshot
> mechanism. The repo's error-fixture harness matches `expected.diag`
> substrings, so the five representative shapes are asserted that way instead
> of inventing a parallel mechanism -- three new `errors/refine-messages-*`
> fixtures plus tightened assertions on the existing closed-value, unknown, and
> nonlinear fixtures.
>
> ### Indirect calls (investigated + partly landed 2026-07-24)
>
> Three shapes, three different answers:
>
> - **Alias of a global** (`(let [g safe-div] (g 10 0))`) -- already worked.
>   `elab_call` folds a let-bound alias of a global to the global before the
>   refinement hook runs, so the crossing lands on the real callee. Pinned by a
>   fixture now.
> - **Lambda with contract parameters** -- was a HARD COMPILE ERROR, not a
>   missing feature. `elab_fn` never peeled `TY_CONTRACT` to its base type (the
>   same hole the contract *return* type had), so the parameter's type stayed
>   the contract type and every call failed with `expected { _ : ? | ... }, got
>   int`. `#refine` on a lambda parameter did not compile at all. Now peeled,
>   given a CT1 entry check, and checkable at the call site.
> - **Function-typed parameter** -- genuinely higher-order. Nothing is checked
>   and nothing can be without refinements in function types, which the
>   prototype excludes. Sound (the callee's entry checks still run); documented
>   rather than papered over.
>
> Two things worth recording from the implementation:
>
> - The CT1 parameter-check injection is now ONE function shared by `defn` and
>   `fn`, rather than a second copy in the lambda path. A lambda's contract
>   parameter being silently decorative is exactly what a divergent second copy
>   produces.
> - The link from a `let` binding to its lambda's predicates is read off the
>   INIT EXPRESSION's FnDef, not the closure-binding graph. A non-capturing
>   lambda has no closure box, so `closure_fn_binding` is unset for it -- the
>   graph route would have missed the simplest case, which is the one people
>   write.
>
> ### Typeclass method dispatch (landed 2026-07-24)
>
> The same contract-peel defect, reached a THIRD time: an instance method's
> parameter annotation kept its `TY_CONTRACT` type, so the method body could
> not use the value (`'*' arg 2: expected int, got { v : int | ... }`) and a
> refined method signature did not compile. After fixing it in `defn` returns
> and then `fn` parameters, the third occurrence was the signal to stop
> patching sites: the peel now lives in one documented helper
> (`rt_peel_contract`, elab_internal.h) that names the failure mode and says to
> call it from any new annotation site. The CT1 entry-check injection is
> likewise one shared function across `defn`, `fn`, and instance methods.
>
> Instance methods parse their parameters in pass 1 and elaborate their bodies
> in pass 2, so the predicates ride on the method's BINDING -- the record that
> spans both passes, and also where a dispatch site looks for them.
>
> A statically-resolved dispatch is now checked like any other call; the
> receiver occupies slot 0 of the impl, so parameter and argument slots line up
> without special-casing. Dotted and bare forms share the resolution point and
> so are checked identically. A dispatch that stays dynamic is deliberately not
> checked -- which method runs is unknown there.
>
> One diagnostic detail worth keeping: the message names the method as the
> SOURCE writes it. A method binding carries its mangled instance symbol
> (`__inst_Scaler_scale_hyby_int`), which is not something to put in front of a
> user, so the crossing records the call form's head instead.
>
> ### Z3 oracle: actually built and run (2026-07-25)
>
> The scaffold had never been compiled. Building it against a real Z3 4.13
> found three bugs, two of them in the scaffold itself and both of the kind
> that only running it can reveal.
>
> **1. The oracle could not link at all.** `refine_libz3.c` is compiled into
> `tur_core`, which is an OBJECT library, so a per-target `PRIVATE` link of
> `z3::libz3` never reached the executables that embed `$<TARGET_OBJECTS:
> tur_core>` -- every test harness failed with undefined `Z3_*` references.
> Fixed with a directory-scoped `link_libraries`, which is exactly the
> accommodation the Windows `ws2_32` block a few lines above already makes for
> the same reason.
>
> **2. One Z3 context for the whole process silently poisoned every query.**
> The code assumed `Z3_eval_smtlib2_string` is self-contained because each call
> carries its own declarations. It is not -- assertions accumulate on the
> context. A single self-contradictory query poisons it, and since we assert
> the hypotheses AND the negated goal, a trivially-VALID obligation is exactly
> such a query. After the first one, every later query returned `unsat` and the
> oracle answered VALID to everything. As the chain tail that proves false
> obligations; as the cross-check it rubber-stamps the in-house stages, which
> is worse, because an oracle that never disagrees is not an oracle. Fixed with
> a fresh context per query.
>
> **3. `sat` was read as INVALID even for an abstracted VC.** The VC replaces
> nonlinear and measure terms with uninterpreted functions. Abstraction is
> sound in ONE direction: `unsat` of the abstraction implies `unsat` of the
> concrete obligation, so VALID transfers -- but a model that assigns an opaque
> symbol some convenient value says nothing about the real function. Z3
> correctly reports `sat` for the abstraction of `x>0, y>0 |- x*y>0`, which is
> a TRUE obligation, and the scaffold turned that into `TUR-E0371` on correct
> code. `sat` may only be read as INVALID when nothing was abstracted -- the
> same rule `refine_model_search` already followed.
>
> ### Differential coverage: what the corpus could and could not establish
>
> Compiling the whole fixture corpus with `refined` forced on under the oracle
> found zero disagreements -- but that is a much weaker statement than it
> sounds, because almost no fixture contains a `#refine`: the first 400 fixture
> files yield **3 obligations** between them. Fixture coverage is not
> differential coverage.
>
> `tests/unit/refine_fuzz.c` is the harness that actually exercises the solver:
> it generates random VCs in the supported fragment (linear integer arithmetic,
> boolean structure, and uninterpreted applications in a quarter of them), runs
> the in-house chain and Z3 on each, and fails on either direction of
> disagreement -- in-house VALID where Z3 says INVALID (a proof that is not a
> proof), or in-house INVALID where Z3 says VALID (a counterexample search
> claiming a witness it does not have). It builds only in an oracle build and
> is deterministic, so a failure reproduces from the printed seed.
>
> Results are also a fair completeness measurement, which nothing before this
> provided: on 4000 VCs the chain agreed with Z3 on 3070 and was incomplete on
> 159 (~5% of the VCs Z3 could decide). Those 159 cost a runtime check, which
> is the designed outcome.
>
> ### Retirement criteria: still not met
>
> **SUPERSEDED -- this is the 2026-07-25 reading and it is no longer current.**
> All three criteria were subsequently met and **the Z3 scaffold was deleted
> 2026-07-30 in 0.32.5**. The entry is kept as the record of where things stood
> that day; for the current state read
> ["Landed: a labelled SMT-LIB corpus, replayed without Z3"](#landed-a-labelled-smt-lib-corpus-replayed-without-z3)
> below and the **EXECUTED 2026-07-30 (0.32.5)** block under
> ["Z3 retirement criteria"](#z3-retirement-criteria) in Part 2.
>
> Two of the three criteria remain open, and the honest reading is that the
> oracle has just started doing its job rather than finished:
>
> - *Bootstrap discharged* -- the in-house chain already decides every fixture
>   the oracle does, so this one holds.
> - *Oracle trust established* -- a soak window and the labelled SMT-LIB
>   `QF_UF`/`QF_IDL`/`QF_LIA`/`QF_LRA` corpora are still not in the repo. The
>   fuzz harness is a real start; a checked-in labelled corpus that runs
>   WITHOUT Z3 present is what the criterion actually asks for.
> - *No scaffold references in shippable code paths* -- re-verified: the
>   default build's `tur` has zero `z3` symbols, zero `z3` in its link line,
>   and no `TUR_REFINE_Z3_ORACLE` string; Release + oracle still fails
>   configuration outright.
>
> ### The purity gap was a real miscompile (fixed 2026-07-25)
>
> Not a theoretical hole. Reduced to a program that the experiment silently
> broke:
>
> ```turmeric
> (defn tick [] #fx{Unsafe} : int  ```c static int64_t n = 0; return n++; ```)
> (defn always-nonneg [] #fx{Unsafe} : #refine{ r : int | (>= r 0) }
>   (- (tick) (tick)))
> ```
>
> `(- (tick) (tick))` is -1. With the gate OFF the runtime check fires and the
> program aborts, correctly. With `--enable=refined` it printed `-1` and exited
> 0: encoding a call as an uninterpreted function makes its occurrences
> congruent, both `(tick)`s became the same term `t`, the solver proved
> `t - t >= 0`, and the check was elided. Turning the experiment on turned a
> correct program into one that violates its own declared refinement -- exactly
> what the whole design forbids.
>
> The fix makes congruence OPT-IN. A call is modelled as a shared value only
> when the callee is known pure: an empty declared effect row (`#fx{}`), or a
> name that resolves to no function at all, which is an abstract measure and
> therefore a mathematical function by definition. Everything else gets a
> distinct symbol per occurrence.
>
> The cost is real and was paid deliberately: a measure written as an ordinary
> `defn` must now declare `#fx{}` to get congruence, and
> `tests/fixtures/refine-measure-euf` was updated to do so. Effect *inference*
> would answer this without annotation, but it runs after the discharge pass,
> so the declared row is the only evidence available. The direction of the
> default is what matters -- guessing "pure" wrongly elides a real check;
> guessing "impure" wrongly only leaves one in place.
>
> Worth noting what did NOT catch this: the differential fuzzer works at the VC
> level, below the encoder, so ~17,300 randomly generated VCs across six seeds
> were all clean while the compiler was miscompiling. Fuzzing the solver does
> not test the translation into it. `tests/fixtures/errors/refine-impure-not-
> congruent` pins the behaviour where the bug actually lived.
>
> ### Class/instance refinement variance (landed 2026-07-25)
>
> Closes the hole the typeclass slice opened. An instance may accept MORE than
> its class signature promises, never less -- the class signature is the
> contract callers program against, so an over-strict instance would reject an
> argument a generic caller was entitled to pass and find out at run time, in
> its own entry check. `TUR-E0374`.
>
> The obligation is `class_pred(p) |- instance_pred(p)` over a fresh parameter:
> an ordinary query through the existing seam, which is the payoff for having
> built the seam. A class parameter with NO refinement promises nothing, so any
> instance refinement on it is a strengthening -- unless the predicate is a
> tautology, and asking the solver is exactly how to tell those apart rather
> than special-casing it.
>
> Two implementation notes:
>
> - The class-signature predicates were being peeled and DISCARDED at three
>   sites. Only one of the three is the branch a spaced `k : #refine{...}`
>   annotation actually takes, and I patched the other two first -- every
>   variance check then reported "the class places no refinement", including
>   the legal direction. The test matrix (stronger / weaker / absent) is what
>   caught it; a single negative fixture would have looked like a pass.
> - The obligation is decided SILENTLY and the witness requested separately.
>   The ordinary reporting path is wrong for it: its failure is a
>   declaration-vs-declaration inconsistency with its own diagnostic, not a
>   `TUR-E0371` about a value, and `runtime_guarded` would have swallowed the
>   counterexample before the caller could see it.
>
> ### Purity, take two: the effect row was never evidence (landed 2026-07-25)
>
> The next slice on the list was "purity from effect inference". Looking into
> it did not produce that feature -- it produced the finding that the premise
> was wrong and that the fix above was **still unsound**.
>
> The effect system tracks ALGEBRAIC effects: `perform` and handlers. It infers
> nothing from `set!` (`collect_effects_in_expr` recurses into an `EX_SET`'s
> value and registers no effect for the mutation), nothing from a mutable
> global, and nothing from inline C -- `effect_check.c:696` says so in as many
> words where it suppresses `W0031` for a `#fx{Unsafe}` body containing inline
> C, "because the effect system does not infer Unsafe from it". An empty row
> is therefore not a purity claim, and no amount of *inference* would have made
> it one; inference would have produced the same empty row.
>
> The hole reproduced immediately: give `tick` a genuinely empty `#fx{}` and a
> `static` counter in a C block, and the original miscompile came straight back
> -- gate off aborts on the contract, gate on prints `-1` and exits 0.
>
> Purity is now decided by a **default-deny walk of the callee's body**
> (`rt_binding_is_pure`, `src/compiler/elab_fns.c`). Admitted: literals, reads
> of immutable bindings, `if` / `let` / `do` / `return`, the arithmetic,
> comparison and logical builtin shapes, and direct calls to functions that are
> themselves pure. Everything else -- inline C, `set!`, `perform`, deref, field
> reads, closures, indirect and rank-2-poly calls, the `println` family, and any
> callee whose body is not in hand -- is impure. A declared non-empty effect row
> remains as a cheap veto; an empty one now proves nothing on its own.
>
> Recursion is handled with a Tarjan-style open-frame rule rather than a flat
> memo: a self-call is optimistically assumed pure (impurity only ever enters
> through a concrete leaf, so the greatest fixpoint is the right one), but a
> result derived from an assumption about an OUTER frame is provisional and is
> not memoized. Without that, `f -> g -> f` where `f` is impure for a reason
> discovered *after* the call to `g` would leave `g` cached as pure. Recursive
> measures do get congruence: verified with a recursive `rlen`.
>
> Net effect on completeness: the walk is strictly more permissive than the old
> rule for ordinary code (`(* v 2)` needs no annotation now) and strictly less
> permissive for anything that lied. `tests/fixtures/errors/refine-impure-fx-
> empty` pins the new half; `refine-impure-not-congruent` still pins the old one.
>
> The general lesson, twice now: the fuzzer works at the VC level, below the
> encoder, so it is blind to every bug in the translation INTO the VC. Both
> soundness bugs found so far have lived there. Addressed by the source-level
> fuzzer below.
>
> ### Source-level differential fuzzing (landed 2026-07-25)
>
> Both soundness bugs so far lived in the encoder, and ~17,300 VC-level fuzz
> cases were clean through both of them. `tests/unit/refine_fuzz.c` starts
> below the encoder, so it cannot see anything the encoder gets wrong. That is
> not a gap in its implementation -- it is the wrong layer.
>
> `tests/refine-fuzz-src.py` starts at the top. It generates whole Turmeric
> programs, compiles and runs each one with `--enable=refined` off and on, and
> compares outcomes. **The gate-off build is the oracle** -- no Z3 required,
> which also means this harness runs anywhere the compiler does, unlike the VC
> fuzzer.
>
> Classification (BUG classes fail the run; SUSPICIOUS is report-only):
>
> | off | on | verdict |
> |---|---|---|
> | abort | clean | `BUG_soundness` -- the miscompile |
> | clean | clean, different stdout | `BUG_output_divergence` |
> | clean | abort | `BUG_new_abort` |
> | clean | reject | `SUSPICIOUS_over_refute` |
> | abort | abort / reject | agreement |
> | reject | -- | generator emitted invalid code; case dropped |
>
> Five generated shapes, because uniform random generation almost never lands
> inside the solver's fragment -- a 40-case smoke run proved 2 obligations, and
> the soundness property has teeth only where a check was actually ELIDED:
>
> - `random` -- anything goes; covers the fallback path, which must never elide.
> - `linear` -- decidable arithmetic with a bounding `:pre`; high prove rate.
> - `congruence` -- the bug shape: two occurrences of one call subtracted, over
>   a helper pool where half the members declare `#fx{}` and lie.
> - `param` -- a refined PARAMETER feeding a refined return. Entry checks are
>   never elided, so this cannot go wrong by elision on its own, but the
>   parameter predicate becomes a HYPOTHESIS for the return obligation and the
>   return check IS elided when that discharges.
> - `propagate` -- RT4 result propagation across a call-site crossing: a refined
>   result flows into a refined parameter through a caller with its own
>   refinement. Three obligations per program, all in the elaboration layer --
>   the layer both known soundness bugs lived in.
>
> With all five, ~45% of cases prove at least one obligation (up from ~24% on
> the first three).
>
> **The harness was verified to fail, twice, with different sabotages** -- both
> at n=200 seed=41, where the shipped build reports zero:
>
> - **A: congruence without evidence.** `rt_binding_is_pure` returns `true`.
>   Reproduces the two historical bugs. Caught 8/200.
> - **B: entry-check elision done wrong.** `rt_inject_param_checks` returns
>   early when `g_opt_refined`. This simulates the NEXT planned feature --
>   whole-program elision of a callee's entry check -- built without the
>   exported/address-taken analysis that makes it sound. Caught 14/200. That
>   feature was flagged as the most likely to introduce a real miscompile;
>   this is now pre-validated coverage for it, and rerunning sabotage B when it
>   lands is the acceptance test.
>
> Results on the shipped build: **3600 cases across eleven seeds, 0 soundness
> bugs, 0 other BUG classes.** ~1100 obligations proven. ~110 suspicious, of
> which I read eighteen by hand -- all legitimate universal refutations on
> programs whose specific arguments dodge the bad case.
>
> Registered as ctest `tur_refine_fuzz_src` at smoke size (60 cases, ~21s).
>
> ### Typeclass method RESULTS: two defects the fuzzer shape uncovered (2026-07-25)
>
> Extending the source fuzzer to typeclass dispatch surfaced two bugs before a
> single generated case ran -- both found while hand-writing the prototype the
> generator was going to be modelled on, and both in the CT layer, so both
> present with the gate OFF.
>
> 1. **A `defclass` method result refinement was parsed, peeled, and dropped.**
>    `(unbox [self : a, k : int] : #refine{ r : int | (>= r 0) })` type-checked,
>    read like a guarantee, and produced no check anywhere. An instance
>    returning `-9` printed `-9` and exited 0, where the identical predicate on
>    a plain `defn` panicked. `TypeClassMethod` had `param_refine_preds` but no
>    result equivalent, so there was nothing to carry the promise.
>
> 2. **A `definstance` method could not restate one.** The impl return
>    annotation was only recognised as `F_KEYWORD` or an `F_TYPE_ANN` wrapping
>    `F_SYM`/`F_KEYWORD`; an `F_CONTRACT_TYPE` payload fell through to the body
>    and came back as *"type annotation ': type' is only valid after a parameter
>    name or as a return type"*. So the check could not be written by hand
>    either -- the class could promise something no instance was permitted to
>    state.
>
> Fixed together: `TypeClassMethod` carries `return_refine_pred`/`_var`, the
> impl parser accepts a contract return, and an impl that writes a plain return
> type **inherits** the class's promise rather than silently discarding it.
>
> **Result variance runs OPPOSITE to parameter variance**, which is the part
> worth stating explicitly because it is easy to get backwards:
>
> | position | obligation | an instance may... |
> |---|---|---|
> | parameter | `class_pred(p) \|- instance_pred(p)` | demand LESS (accept more) |
> | result | `instance_pred(r) \|- class_pred(r)` | deliver MORE (promise more) |
>
> A parameter refinement is something the method DEMANDS; a result refinement
> is something it DELIVERS. Both are `TUR-E0374` with distinct messages. Tested
> across the full matrix in both positions -- weaker, stronger, identical,
> absent -- because the earlier parameter-variance bug was one a single negative
> fixture would have reported as a pass.
>
> The return-check construction was inline in `elab_defn`; it is now
> `rt_wrap_return_check` in `elab_fns.c`, shared with the instance path. Same
> reasoning as `rt_peel_contract`, which had to be reached independently three
> times before it was centralised: a second hand-rolled copy is a second place
> for a refinement to be accepted and quietly not enforced.
>
> Fixtures: `refine-class-result` (inherit + strengthen),
> `errors/refine-instance-result-weaker` (the illegal direction).
>
> The generator shape landed alongside: both variance ladders, both dispatch
> spellings (`.meth` and bare `meth`), int and float instances, ~15% of cases.
> 900 cases across three seeds after the fixes: 0 soundness bugs, 0 other BUG
> classes.
>
> One classification note worth keeping: `errors/refine-instance-result-weaker`
> classifies as `SUSPICIOUS_over_refute`, not `agree_rejected_early` -- it runs
> CLEAN with the gate off, because its single call returns a value that happens
> to satisfy the weaker promise. E0374 is a declaration-vs-declaration
> inconsistency, not a claim about the value the program produced. The bucket
> holds legitimate DECLARATION errors as well as universal value refutations.
>
> ### The third congruence door: typeclass methods (landed 2026-07-25)
>
> The same miscompile, a third time, through a route neither previous fix
> touched:
>
> ```turmeric
> (defn raw-tick [] #fx{} : int ```c static int64_t n = 0; return n++; ```)
> (defclass Ticker [a] (tickm [self : a] : int))
> (definstance Ticker [int] (tickm [self : int] : int (raw-tick)))
> (defn probe [] : #refine{ r : int | (>= r 0) }
>   (- (tickm 1) (tickm 1)))
> ```
>
> Gate off aborted. Gate on reported *1 proven*, printed `-1`, exited 0.
>
> The cause was the ABSTRACT MEASURE rule, which is correct and which I wrote:
> a name resolving to no function at all is an uninterpreted mathematical
> function, and those are congruent by definition. A typeclass method has no
> global binding under its bare name -- the dispatch is resolved elsewhere --
> so `rt_resolve_fn` returned false and the encoder happily read "abstract
> measure". The purity work hardened the path where a callee IS found and left
> the not-found path alone; that was the gap.
>
> `typeclass_env_find_method` now distinguishes the two. A method is reported
> as a known callee with `pure = false`, so each occurrence gets its own
> symbol. The class's result refinement is deliberately not published there:
> which instance runs is unknown at the encoder, and enforcement is
> per-instance.
>
> **The first fix was half a fix.** It looked the name up as interned, which
> works for the bare `(tickm 1)` and not for the dotted `(.tickm 1)` -- the
> encoder sees `.tickm`, which matches no method name. Two seeds later the
> fuzzer produced a dotted case and reported it. The lookup now strips the dot.
> Both spellings are in the fixture.
>
> Also fixed on the way: `refine_return_pred` was published on instance-method
> bindings UNCONDITIONALLY, violating the field's documented contract
> ("published only when enforced") under `--no-contracts`. Currently
> unreachable -- no dispatch resolves to that binding -- but it is a trap for
> whoever wires the propagation up, so it is now gated on
> `rt_contracts_emitted()` like the `defn` path's `rt_ret_guaranteed`.
>
> #### What this says about the fuzzer
>
> The typeclass shape added an hour earlier did NOT catch this, and neither did
> the congruence shape. `congruence` only ever repeats a call to a HELPER;
> `typeclass` never repeats a call at all. The bug lived in the gap between two
> shapes that each looked like they covered the area. `shape_congruence_method`
> closes it, and with the fix reverted it reports 4 soundness bugs in 200 cases
> where the fixed build reports 0 -- the four cases move to `agree_abort`.
>
> A fixture is `refine-typeclass-not-congruent`, and it is a RUNTIME fixture
> rather than an error fixture: nothing here is statically refutable (the
> method's result is opaque, so there is no closed counterexample), so the build
> succeeds with `TUR-W0372` and the kept check is what fires. The bug was never
> a missing diagnostic; it was a missing abort.
>
> ### Result propagation across a dispatch (landed 2026-07-25)
>
> The completeness half. A call to `(.unbox 3 4)` was fully opaque even when
> the class promised `(>= r 0)`; now the CLASS's result refinement propagates
> to the caller, so a caller's own obligation discharges from it.
>
> Publishing it without knowing which instance runs is sound *because of*
> result variance, and the reasoning is worth writing down because the obvious
> version of it is wrong:
>
> - An instance that INHERITS the class predicate is checked against it. Fine.
> - An instance that RESTATES one owes `instance_pred |- class_pred`, and that
>   check reports **only on a refutation**. An UNDECIDABLE pair emits no error
>   -- so "the variance check passed" does not mean the class promise holds.
>
> So the variance obligation's result is now recorded, and when an instance
> restates a predicate that was not PROVED to imply the class's, the class
> predicate is checked alongside the instance's own ("Class result contract
> violated"). The class promise is then enforced on every instance
> unconditionally, which is what makes publishing it at a dispatch site safe
> regardless of elaboration order or solver strength.
>
> `errors/`-style coverage for exactly that gap:
> `refine-typeclass-result-unproved` uses a nonlinear instance promise
> (`(>= (* r r) (* r 2))`), which abstracts to an uninterpreted term so
> variance answers unknown and no E0374 fires -- while `r = -5` satisfies it and
> violates the class's `(>= r 0)`. The caller's check IS elided; the injected
> class check is the only thing between that program and printing -5.
>
> Publication is gated on `rt_contracts_emitted()`, matching
> `rt_ret_guaranteed` on the `defn` path: `--no-contracts` removes what
> enforces the promise, so the fact goes with it. Verified both ways.
>
> ### An impure PREDICATE, found as output divergence
>
> The fuzzer's non-soundness properties earned their keep here. Seed 93 case
> 294 reported `BUG_output_divergence`: gate off printed `0 2 4`, gate on
> printed `0 1 2`. Both runs internally consistent; neither violated a
> refinement.
>
> The generated predicate called an impure helper AND contained a tautology
> (`(not= r (+ 2 r))`). The obligation discharged, the check was elided, and
> eliding it skipped the predicate's own side effects -- the counter no longer
> advanced. An experiment flag changed what the program printed.
>
> `rt_pred_is_impure` now refuses to report a return obligation as proven when
> the predicate calls anything not known pure. Reported as not-proven rather
> than diagnosed: the predicate is equally impure with the gate off, so turning
> the experiment on should not invent an error.
>
> The deeper issue was filed and then fixed the same day; the report is
> archived at `docs/archive/impure-refinement-predicates-accepted.md`.
> `TUR-E0375` is now emitted (see below).
>
> Fixture `refine-impure-predicate-not-elided` pins the behaviour. Note the
> impure call is written first inside the `or`: `or` short circuits, so with
> the tautology leading, the impure call is never reached and there is nothing
> to pin -- the first version of the fixture made exactly that mistake and
> passed for the wrong reason.
>
> ### TUR-E0375, and why purity had to become three-valued (landed 2026-07-25)
>
> Digging into the filed report turned a one-line diagnostic into a design
> correction.
>
> `TUR_E0375_REFINE_EFFECTFUL` was declared in `diag.h` and mapped in `diag.c`
> from the start and emitted by nothing. It is now emitted from the shared CT1
> helpers, covering all four positions a predicate can appear in -- refined
> parameter, `:pre`, `:post`, refined return -- and gate-independent, because
> the predicate is equally wrong with the experiment off.
>
> The interesting part is what it could NOT be driven by. `rt_binding_is_pure`
> is DEFAULT-DENY: anything the walk does not model -- a `match`, a struct
> field read -- reads as impure. That is right for congruence, where a wrong
> "pure" elides a real check. It is exactly wrong for a diagnostic, where a
> wrong "impure" rejects working code: a measure written with a `match` would
> have been reported as effectful.
>
> So purity is now three-valued -- `RT_P_PURE` / `RT_P_IMPURE` /
> `RT_P_UNKNOWN` -- and the two callers read the middle value in OPPOSITE
> directions:
>
> | question | wrong answer costs | reads UNKNOWN as |
> |---|---|---|
> | congruence: same value twice? | an elided check (miscompile) | impure |
> | diagnostic: does this DO something? | a rejected valid program | pure |
>
> They are not negations of each other, and collapsing them into one boolean is
> precisely how a default-deny purity test turns into false errors.
> `refine-pure-predicate-unmodelled` is the false-positive guard: a
> `match`-bodied measure in a `:post`, which must keep compiling.
>
> A hard error was affordable because it broke nothing: across 2308 fixtures
> and the whole stdlib the only impure predicate was the one written to
> demonstrate the bug. That is the measurement that justified using the E-code
> as declared rather than softening it to a warning.
>
> One fuzzer follow-on: generated predicates were calling impure helpers ~8% of
> the time, so those cases became `skip_invalid` and spent budget rediscovering
> a rule one fixture already pins. `Gen.expr` grew a `pure_only` flag for
> predicate generation; `skip_invalid` went from ~24/300 to ~2/300.
>
> ### RT5a -- the solver on wasm32 (landed 2026-07-25)
>
> Two questions, and only the second is interesting:
>
> 1. Do the refine sources COMPILE under Emscripten with the flags the real
>    `tur_wasm` target uses? All ten, clean, including `-pedantic` -- which is
>    load-bearing, since that is what caught the `__int128` the exact rational
>    arithmetic originally used.
> 2. Do they give the SAME ANSWERS at 32-bit pointers? **47 checks, 0 failures,
>    run under node.** This is the half a compile check would have missed: S2 is
>    Fourier-Motzkin over exact rationals with `__builtin_*_overflow` guards,
>    and the hash-cons table, the constant folder and the model search all key
>    off integer widths.
>
> The full `tur_wasm` module also builds and links with the refine sources in
> it, with no refine-related warnings.
>
> `tests/run-refine-wasm.sh` makes both reproducible and skips cleanly when
> emcc is absent; registered as ctest `tur_refine_wasm`.
>
> #### Two defects the build surfaced
>
> **The Z3 oracle guard did not actually guard WASM.** The top-level
> `CMakeLists.txt` refuses `TUR_REFINE_Z3_ORACLE` when
> `CMAKE_BUILD_TYPE STREQUAL "Release" OR EMSCRIPTEN`, and the comment beside
> it claims "Release and WASM builds REFUSE the option, so there is
> structurally no path by which Z3 reaches a shipped binary or the web
> bundle". `EMSCRIPTEN` is only set when cross-compiling under the Emscripten
> toolchain (`emcmake cmake`) -- and `tur_wasm` is a normal HOST configure that
> shells out to `emcc` from an `add_custom_target`. So
> `-DTUR_WASM=ON -DTUR_REFINE_Z3_ORACLE=ON` configured cleanly with the oracle
> ENABLED.
>
> No Z3 actually reached the bundle: the custom target inherits neither
> `target_compile_definitions` nor `link_libraries`, so `refine_libz3.c`
> compiled to its stub branch. But that is safety by accident of the build's
> shape, not the structural refusal the comment describes. The guard now also
> tests `TUR_WASM`; verified in all three directions (wasm+oracle fails,
> Release+oracle fails, plain wasm succeeds).
>
> **UB in the shipped web bundle, in unrelated code.** `promo_hash`
> (`src/turi/eval.c:9392`) applies the 64-bit MurmurHash3 finalizer to a
> `uintptr_t`, so at wasm32 it shifts a 32-bit value by 33. clang warns;
> `turi/eval.c` is the interpreter, which IS the web REPL. Filed as
> `docs/archive/history/wasm32-promo-hash-shift-ub.md` rather than fixed inline --
> the one-line fix changes hash values and therefore promo-map iteration
> order, which deserves its own suite run rather than riding along with an
> unrelated slice. (`hamt.c` does the same shift on a `uint64_t` and is fine.)
>
> **FIXED 2026-07-26**, archived at
> [docs/archive/history/wasm32-promo-hash-shift-ub.md](history/wasm32-promo-hash-shift-ub.md).
> The deferral reason above turned out not to apply: on LP64 the rewrite is
> bit-identical (the cast is a no-op widening), so native hash values do not
> move at all, and the promo map is a seen-set nothing iterates for output.
>
> ### RT7 -- measured first, then half-built on purpose (2026-07-25)
>
> **A crash came out before any caching did.** The benchmark written to make
> discharge expensive crashed the compiler instead: `(or (and A B) C)` in a
> return refinement sent the DNF expansion into unbounded recursion. Three
> lines was enough. Fixed separately; the fixture is
> `refine-disjunctive-goal`, and since those goals previously crashed rather
> than failed, the fix also turned on disjunctive reasoning that had never run.
>
> #### Where the time actually goes
>
> | workload | gate off | gate on (before) | gate on (after) |
> |---|---|---|---|
> | 200 easy obligations | 182 ms | 148 ms | 146 ms |
> | 600 hard obligations | 782 ms | 21 908 ms | **1 450 ms** |
>
> Two things the measurement changed:
>
> - **Easy obligations cost nothing.** A provable goal exits at the first stage
>   that proves it. Caching them is solving a problem nobody has.
> - **The cost is concentrated in obligations that do NOT discharge, and most
>   of it was not in the solver chain at all.** Memoizing the chain alone took
>   the hard workload from 21.9s to 17.6s -- backend calls fell 2400 -> 20 and
>   the wall clock barely moved. The rest was the RT6 HINT SEARCH, which runs
>   the full chain *twice per candidate* over every literal and variable pair.
>   It is decided by the VC just as the verdict is, so it went behind the same
>   key. That is the 17.6s -> 1.45s step.
>
> #### The key, and why a hash alone is not enough
>
> `refine_vc_fingerprint` hashes the VC under a canonical ALPHA-RENAMING:
> variables and uninterpreted symbols are numbered by first occurrence, not by
> source name, because `x > 0 |- x + 1 > 0` and `n > 0 |- n + 1 > 0` are the
> same question. Without renaming the memo would miss on every function that
> spells its parameter differently, which is most of them.
>
> **Every hit is confirmed with `refine_vc_equal` before the verdict is
> reused.** A 64-bit collision is unlikely, but "unlikely" is the wrong
> standard when the consequence is reusing VALID for a different obligation and
> eliding a check. Confirmation costs one structural compare on a hit.
>
> Proven load-bearing by construction, not by argument: with the confirmation
> deleted and fingerprints forced to collide, a program whose first obligation
> proves and whose second cannot prints its violating value and exits 0 instead
> of aborting. `refine-memo-distinct-obligations` pins it.
>
> #### A fuzzer blind spot this exposed
>
> The source fuzzer did **not** catch that miscompile: its generated programs
> carried ONE obligation each, and a single-obligation program cannot exercise
> a memo at all. 250 cases ran completely clean against the broken build.
> Generated programs now carry several obligations, and the same seed reports
> 9 soundness bugs against that build and 0 against the shipped one.
>
> Also worth recording as a negative result: the *first* memo sabotage --
> trusting the 64-bit hash without confirming -- is **not** detectable by
> fuzzing at any realistic scale, because it needs a hash collision. Forcing
> total collision is what made it testable. A fuzzer cannot validate a
> probabilistic failure mode; only the confirming compare can.
>
> #### Why the persistent cache is not built
>
> The plan specifies `.tur-cache/refine.db` keyed by VC hash plus compiler
> version, with per-file invalidation. That is deliberately NOT done, and the
> reason is the measurement above plus one structural fact:
>
> **An on-disk cache cannot confirm a hit.** It has no second VC to compare
> against -- only a digest. So the entire soundness argument rests on hash
> strength, exactly the thing just shown to be untestable by fuzzing. Doing it
> safely needs a wide digest (128-bit+) over a canonical serialization, plus
> the compiler version, the solver chain's identity, and every cap constant in
> `refine_solver.h` -- because changing `REFINE_MAX_CUBES` changes what is
> provable, and a cache that outlives that change hands back verdicts the
> current solver would not produce.
>
> With the within-unit memo in place the remaining prize is rebuild latency on
> a file whose obligations are unchanged, against a first-build cost that is
> now 1.9x the rest of compilation rather than 28x. That is a much smaller
> prize than it looked like before the measurement, carrying the largest
> soundness surface in the feature. Worth doing when there is a real project
> whose builds are measurably slowed by discharge -- and not before.
>
> ### RT4 branching bodies -- path splitting (landed 2026-07-25)
>
> `pred[body/r]` needs a value to substitute for `r`, and `(if c t e)` is not
> one, so any function whose body branched answered unknown and kept its check.
> That ruled out most real code. A branching body is now discharged PER PATH:
>
>     c |- pred[t/r]    and    (not c) |- pred[e/r]
>
> `(let [x v] body)` adds `x = v` and recurses. Nesting is handled to a depth
> cap. Neither arm typically proves the goal alone -- in `(if (>= x 0) x 0)`
> with goal `(>= r 0)`, `x` is not non-negative and `0` says nothing about `x`
> -- so the path condition is doing the work.
>
> Splitting is tried FIRST and SILENTLY, and can only ever prove more: on
> failure the ordinary whole-body obligation still runs and still reports in
> the ordinary place, so an unproven function looks exactly as it did before.
>
> #### The hole it opened, and closed
>
> The hypothesis `x = v` lands in the environment's single flat namespace, so a
> `let` that SHADOWS a name in scope asserts `x = <something mentioning x>`.
> `(let [x (- x 1)] x)` becomes `x = x - 1` -- false for every x -- and a false
> hypothesis proves the goal. `(defn shadowed [x : int] : #refine{ r | (>= r 0) }
> (let [x (- x 1)] x))` returned -6 and exited 0 where the gate-off build
> aborted. A live miscompile, on ordinary code, introduced by this slice.
>
> Fixed by declining to split when the bound name is already in scope; the
> whole-body obligation then answers unknown and the check survives.
> `refine-let-shadow-not-split` pins it. Alpha-renaming the binding would
> recover those bodies and is now the follow-up item.
>
> I found that by reasoning about the flat namespace, NOT by fuzzing -- the
> generator emitted no binding forms at all, so nothing it produced could reach
> it. A `branching` shape now generates `if`/`let` bodies including shadowed
> ones, and with the guard removed it reports the miscompile (1/200 at seed
> 171, 0 on the fixed build). Two slices running, the fuzzer's gaps have been
> about what it does not GENERATE rather than what it fails to check.
>
> Stats gained `proved by path splitting (N path probe(s))`. The split returns
> before the ordinary obligation is built, so without explicit accounting a
> branching body vanished from the summary entirely -- four refined functions
> reported as one obligation.
>
> ### Whole-program entry-check elision: measured, declined (2026-07-25)
>
> This was the top of the list for months on the strength of a claim nobody
> had checked -- that eliding a proved callee's entry check is a "code-size /
> perf win". It is not, on either count.
>
> **Runtime: unmeasurable.** A 50M-iteration loop whose body cc genuinely
> cannot eliminate (the callee writes a `volatile` static, so every iteration
> must run) calling a function with a `#refine{ v : int | (>= v 0) }`
> parameter:
>
> | | run 1 | run 2 | run 3 |
> |---|---|---|---|
> | entry checks ON | 125 ms | 125 ms | 126 ms |
> | `--no-contracts` | 125 ms | 125 ms | 127 ms |
>
> **Code size: zero bytes.** Same binary size with and without every contract
> check; the emitted C differs by 9 lines out of 7219.
>
> The check is a compare and a branch that predicts perfectly. There is no win
> here to collect.
>
> Two benchmarking traps worth recording, because both produced confident
> wrong answers before the volatile version:
>
> - `cc -O2` **proves the check itself** when the argument is locally derived
>   (`(step (if (>= acc 0) acc 0))`), so the elision that whole-program
>   analysis would do is already done, for free, by the C compiler.
> - `cc -O2` **closed-formed the loop**: a 200M-iteration arithmetic series ran
>   in 3 ms. The answers were correct, which is exactly why the number looked
>   plausible. Any benchmark here needs a side effect the optimizer must keep.
> - A size comparison on a 200-refined-function file showed byte-identical
>   binaries -- but that was dead-code elimination removing 199 functions
>   `main` never calls, not evidence about checks. The valid size number is
>   the one from the live loop above.
>
> Against zero measured benefit stands the largest soundness surface in the
> feature: it needs an exported/address-taken analysis, and getting it wrong
> drops a check that was protecting something. Fuzzer sabotage B already
> quantifies that -- eliding entry checks naively produces 14 soundness bugs
> in 200 generated programs.
>
> **Declined.** Revisit only with a profile from a real project showing entry
> checks in the hot path. The call-site layer stays what it is: a diagnostic
> that turns `(safe-div 10 0)` into a compile error, which is where its value
> actually was.
>
> ### Alpha-renaming shadowed let bindings (landed 2026-07-25)
>
> Path splitting declined to split a `let` that shadowed a name in scope,
> because the hypothesis `x = v` in one flat namespace would assert
> `x = <something mentioning x>`. The binding is now renamed to a fresh symbol
> and the body rewritten to use it, so `(let [x (- x 1)] x)` yields
> `x~0 = x - 1` -- correct, and correctly unprovable -- instead of a
> contradiction that proves anything. A body that rebinds the same name AGAIN
> still declines rather than rename twice.
>
> **Benefit undemonstrated.** I could not construct a program where this newly
> proves something, because every shadowing shape I tried that would have been
> provable fails to type-check with the gate OFF as well -- `let` shadowing a
> parameter is only narrowly supported in the language today, independent of
> refinements. So this is verified safe (suite, solver unit, 400 fuzz cases,
> and the shadow miscompile fixture still aborts under both gates) and its
> value is contingent on that unrelated limitation being lifted. Worth knowing
> before anyone counts it as a win.
>
> ### `match`-arm splitting (landed 2026-07-25)
>
> Each arm's expression is proved against the goal, with the pattern
> contributing NOTHING. Unlike `if`, where the path condition is a first-class
> fact, a match arm cannot supply one: the VC has no datatype theory -- no
> constructors, no field accessors -- so "c is a Green" is inexpressible.
> Asserting nothing is sound (fewer hypotheses only make a goal harder) and
> still discharges the common shape where every arm independently satisfies the
> predicate.
>
> `(match c (Red) 0 (Green) 1 (Blue) 2)` against `(>= r 0)` proves; change one
> arm to -1 and it stays unknown, keeps its check, and fires under both gates.
>
> Pattern binders are declared unconstrained, and a binder that SHADOWS a name
> in scope declines the split. One flat namespace means the arm's `r` would
> otherwise inherit an outer `r`'s hypotheses -- the same class of bug as the
> shadowed `let`, and unsound rather than merely imprecise. Unlike `let`,
> alpha-renaming is not enough here: a pattern binder's meaning comes from the
> constructor position, which nothing in the VC models, so there is no correct
> fact to rename INTO.
>
> The ceiling is now the datatype theory, not the splitting.
>
> **Superseded the same day** -- arms carry hypotheses now; see below. The
> "no correct fact to rename INTO" argument for declining a shadowing binder
> no longer holds either (a binder now has one), but the split still declines,
> because the rename would have to reach the synthesized selector facts too.
>
> ### `match` admitted to the purity whitelist (landed 2026-07-25)
>
> First of the three purity-whitelist widenings. `rt_classify_expr` now joins
> over a `match`'s scrutinee and every arm instead of answering UNKNOWN
> outright, so a measure written with a `match` is congruent. Measured on the
> same program before and after: `0 proven, 1 unknown` -> `1 proven, 0 unknown`.
>
> Pattern binders are deliberately NOT walked -- they are introduced by the
> pattern rather than evaluated, so an arm that reads one stays pure. Note this
> is the opposite of the rule for path SPLITTING, where a pattern binder that
> shadows an outer name declines the split; there the binder enters the VC's one
> flat namespace, here it is only being classified.
>
> The join is what keeps it honest: one impure arm makes the whole form impure.
> `refine-match-impure-arm` pins that with a counter in one arm -- had `match`
> been classified PURE unconditionally, the program would print -1 and exit 0,
> which is exactly the miscompile shape already pinned three times over.
>
> This widening cannot grow TUR-E0375. Congruence reads UNKNOWN as impure and
> diagnostics read it as pure -- they are not negations -- so moving a form from
> UNKNOWN to PURE adds proofs on one side and removes nothing but diagnostics on
> the other. Both remaining widenings (immutable field reads, `while` over
> provably-local state) inherit that argument.
>
> Verified: suite 2315/0, solver unit 47/0, 400 fuzz cases across two seeds with
> 0 soundness bugs.
>
> ### A datatype theory for match arms (landed 2026-07-25)
>
> This was the listed ceiling on match-arm proving, and it did not need what
> the entry assumed it needed. Adding constructors and selectors to the VC term
> language -- a new sort, new solver stages -- would have been the largest
> change in the feature. It turns out an arm does not need a datatype SORT; it
> needs a way to SAY what its selection implies, and equations over
> uninterpreted functions already say it. S1's congruence closure decides them
> as-is.
>
> So the theory is synthesized at the FORM level, before encoding. Four
> hypothesis sources, each measured against the same program before and after:
>
> | Source | Shape | Before | After |
> |---|---|---|---|
> | Record field | `(= w (.width b))` | 0 proven, 1 unknown | 1 proven |
> | Literal pattern | `(= n 0)` | 1 proven, 1 unknown | 2 proven |
> | Guard | the guard verbatim | 0 proven, 1 unknown | 1 proven |
> | Constructor tag | `(= (#dt/tag s) 2)` | 0 proven, 1 unknown | 1 proven |
>
> The field selector is the one with an everyday payoff: a getter can state a
> postcondition about the field it returns. The tag's only standalone use is
> **dead arms** -- two matches on one scrutinee pin its tag twice, the inner
> arm's hypotheses go contradictory, and its obligation holds vacuously because
> the arm cannot run. Narrow, but real, and measured rather than assumed.
>
> **The synthetic name must be unspellable.** `enc_measure` resolves a head
> name to decide purity, so if `#dt/tag` resolved to a real PURE function the
> hypothesis would assert something about THAT function -- false, and a false
> hypothesis proves the goal. A leading `#` is reader dispatch, so no source
> symbol can begin with one; the name resolves to nothing, which the encoder
> reads as an abstract measure.
>
> **Two bugs found on the way, one mine and one not.**
>
> Mine: arm hypotheses must be scoped PER ARM, not per match. Sibling arms
> assert different tags for the same scrutinee, so accumulating them puts
> `tag(s) = 0` beside `tag(s) = 1` -- contradictory, proving every arm after the
> first. This hazard did not exist while arms contributed nothing; the tag
> created it. Caught before it landed, pinned by
> `refine-match-arm-hyps-not-shared`.
>
> Not mine: arms are not fixed-width. `when` inserts a guard between a pattern
> and its body, and the old code strided in pairs -- so `when` was read as a
> pattern and the guard expression as a body. It failed safe (the misread arm
> never proved) but meant no guarded match was ever split. Walking the arm list
> is what fixes it, and it is why the guard row above went from 0 to 1.
>
> The fuzzer gained a `datatype` shape, and it immediately found a codegen
> null-deref unrelated to refinements: a `match` on a non-ADT scrutinee whose
> FIRST arm is a wildcard or bare binder reads `adt->name` through NULL, gate or
> no gate. The affected rungs are dropped from the generator. (My
> first reading blamed `when`, which was wrong -- it is decided by arm 0's
> pattern alone.)
>
> Negatives are pinned by INVERSION: `errors/refine-match-unsound-shapes` runs
> under `--strict-refine`, where an unproven obligation is a hard error. If a
> future change made a too-weak guard or a cross-scrutinee tag provable, that
> fixture would start compiling and fail. A positive fixture cannot catch that
> direction. `refine-match-datatype-theory` uses the same flag the other way, so
> a regression in any of the four sources is a compile error rather than a
> silently-kept check.
>
> Verified: suite 2319/0, solver unit 47/0, 400 fuzz cases across two seeds with
> 0 soundness bugs.
>
> ### Field-read purity, and the equality goal it uncovered (landed 2026-07-25)
>
> **Field reads.** A field read is now classified as pure as its receiver, so
> an ordinary getter is congruent -- `(- (width-of b) (width-of b))` against
> `(>= r 0)` went `0 proven, 1 unknown` -> `1 proven`. The plan's next-slice
> entry called this "immutable struct field reads", which presupposes a
> declared immutability the language does not have: there is no per-field `mut`
> marker. It does not need one. `set!` on `(.f s)` requires `s` to be bound
> `^mut`, which is the same declaration-level guarantee that already makes a
> non-mut variable read congruent, so recursing on the receiver inherits the
> right answer.
>
> Declined behind `rc`/`ref`/a borrow: there a caller can hold a `^mut` handle
> to the object the callee reads through a non-mut one and mutate between two
> calls, which is precisely the aliasing congruence assumes away. A by-value
> receiver has no second handle (`:copy` copies, a moved value leaves the
> caller nothing), so the vector closes by construction. Three congruence
> miscompiles on this feature have come from assuming an aliasing question
> away; this one is answered instead.
>
> **The equality goal.** Measuring the field-read gain needed a control, and
> the control failed: `(- (plain b) (plain b))` against `(= r 0)` was Unknown
> even with `plain` returning a constant -- while the same body against
> `(>= r 0)` proved. The predicate was the variable, not the callee.
>
> The cause is one skipped literal. An obligation is discharged by refuting
> `hyps AND (not goal)`, so an equality goal puts `r != x` in the formula, and
> `la_assert_cube` skipped a negated `VC_EQ` because a single linear constraint
> cannot express a disequality. Sound, and documented as such -- but it meant
> **no equality goal needing any arithmetic was ever provable**, and `(= r
> <expr>)` is the most natural postcondition there is. `(- (+ x 1) 1)` against
> `(= r x)` was Unknown.
>
> `a != b` over a totally ordered sort is `a < b OR b < a`. NNF now rewrites a
> negated arithmetic equality to that disjunction, KEEPING the original literal
> so EUF still sees the disequality it was already using. The rewrite must be
> an EQUIVALENCE rather than a strengthening -- we are refuting, so a stronger
> formula would be easier to refute and could prove a goal that does not hold.
> It is one: the added disjunction is implied by the literal it joins.
>
> Both directions moved. `(- (+ x 1) 1)` against `(= r x)` proves; `(+ x 1)`
> against the same goal is now REFUTED with a counterexample instead of
> Unknown, so an off-by-one that used to surface as a runtime panic is a
> compile-time `TUR-E0371`. Each disequality doubles the cube count, which
> `REFINE_MAX_CUBES` bounds; blowing the cap answers Unknown as every cap here
> does.
>
> This is the S2c "integer case-splitting on a disequality" tail the arith
> comment pointed at, except it is not integer-specific and not a tail: it is
> four lines in `nnf`, and it was gating a whole shape of postcondition.
>
> Worth recording how it was found: not by looking for it, but because a
> measurement needed a control and the control disagreed with the hypothesis.
> The `(>= r 0)` phrasing everywhere in the fixtures and the fuzzer's own
> predicate pool is why it survived this long -- `shape_congruence` picks
> `(= r 0)` one time in three and the case simply came back Unknown, which
> reads as "outside the fragment" rather than "a bug".
>
> Verified: suite 2322/0, solver unit 47/0, 400 fuzz cases across two seeds
> with 0 soundness bugs.
>
> ### Constructor axioms (landed 2026-07-25)
>
> The remaining half of the datatype theory, and the one the previous entry
> named as its follow-up. The arm hypotheses run one way -- a binder is the
> field it destructures -- and said nothing about a value just BUILT. The
> defining equation runs the other way and is asserted once per constructor
> application in the obligation, since it is universally true rather than
> path-dependent:
>
>     (= (.width (Box p 3)) p)
>
> `(.width (Box p 3))` against `(= r p)` went `0 proven, 1 unknown` ->
> `1 proven`, and so did the full round trip
> `(let [b (Box p 3)] (match b (Box w h) w))`, which needs the axiom, the `let`
> split, and the arm's field hypothesis all three.
>
> **It needed a second fix to do anything at all.** The axiom landed and
> changed nothing: still Unknown. A constructor HAS a global binding and no
> `defn` body, so the default-deny purity walk found no evidence and answered
> UNKNOWN -- which congruence correctly reads as impure. Every occurrence got
> its own symbol, so the `Box(p,3)` in the axiom and the `Box(p,3)` in the goal
> were different terms and the axiom was inert. A data constructor stores its
> arguments and runs no user code, so `rt_resolve_fn` now answers it pure
> before the binding lookup.
>
> Worth being precise about why that is sound, since congruence is where this
> feature's three miscompiles came from. Two applications of `Box` produce two
> distinct OBJECTS; they are congruent only in the sense that equal arguments
> give equal fields. That is enough because a constructor term is reached only
> through a selector -- nothing in the predicate language can observe identity.
> And it says nothing about the arguments: `(Box (tick) 3)` twice is two
> different terms, exactly as `(tick)` twice is, which
> `errors/refine-ctor-axioms-unsound` pins.
>
> Verified: suite 2324/0, solver unit 47/0, 400 fuzz cases across two seeds
> with 0 soundness bugs.
>
> ### Branching `let` values and `do` blocks (landed 2026-07-25)
>
> Picked by sweeping eight ordinary refinement shapes rather than by working
> down the next-slice list, which is how the equality goal turned up too.
> `abs`, `max`, `clamp`, and `sum-preserves-sign` all proved; four did not:
>
> | Shape | Before |
> |---|---|
> | `(let [m (if (<= a b) a b)] m)` vs `(<= r a)` | unknown |
> | `(do (println ...) (if ...))` | unknown |
> | recursive `(f (- n 1))` under `(= n 0)` | crossing unknown |
> | `while` accumulator | unknown |
>
> The first two are the same omission from two angles and both landed.
>
> **A branching `let` value** asserted `m = (if c a b)`, and an `if` is not a
> term the encoder can build, so the hypothesis was dropped and `m` entered the
> body unconstrained. The identical `if` written directly as the body proved --
> a distinction nobody writing the code would expect to matter. Splitting the
> value is the same rule as splitting the body, one level up.
>
> **A `do` block** is proved through its last form. Sound only while the
> statements cannot stale a hypothesis already in the environment -- and those
> hypotheses are about PARAMETERS, so an assignment is exactly what stales
> them. A syntactic scan for `set!`/`swap!`/`reset!` in the statements declines
> those. Deliberately over-broad: a name that merely looks like an assignment
> costs precision, missing one costs soundness.
>
> That scan was **verified load-bearing** rather than assumed. Disabling it
> makes `(do (set! x -5) x)` under a parameter refinement `x >= 0` report
> `2 proven, 0 unknown` and the program then returns -5 with no check -- a
> miscompile, pinned by `errors/refine-do-set-not-split`.
>
> **The other two are recorded, not fixed**, and both are in the guide's
> limitations now:
>
> - **A call-site crossing does not see path conditions.** Crossings resolve
>   after the whole unit, which is what lets them see every callee's
>   refinement, but a call inside a branch is then checked without the
>   condition that selected it -- so the canonical decreasing-argument
>   recursion has an unprovable crossing. Fixing it means capturing the path
>   condition at COLLECTION time, which needs a condition stack threaded
>   through expression elaboration: a real change, not a slice, and the largest
>   remaining proving gap.
> - **`while` is not analysed at all**, which is the honest description of the
>   remaining purity-whitelist item. It is not a classifier case; it needs loop
>   invariants, which the prototype excludes.
>
> Verified: suite 2326/0, solver unit 47/0, 400 fuzz cases across two seeds
> with 0 soundness bugs.
>
> ### Path conditions for call-site crossings (landed 2026-07-25)
>
> The largest remaining proving gap, closed without the change it looked like
> it needed. A crossing is collected during elaboration and discharged after
> the whole unit -- the deferral is load-bearing, it is what lets a crossing
> see every callee's refinement -- and the cost was that a call inside a branch
> was checked WITHOUT the condition that selected the branch.
>
> | Shape | Before | After |
> |---|---|---|
> | `(if (= n 0) 0 (+ 1 (f (- n 1))))`, `f : Nat -> _` | 1 proven, 2 unknown | 2 proven, 1 unknown |
> | `(if (= x 0) 0 (sdiv 10 x))`, `d : NonZero` | unknown | proven |
>
> The previous entry said this "needs the path condition captured at COLLECTION
> time, which means a condition stack threaded through expression elaboration:
> a real change, not a slice." That was wrong, and usefully so. `call_form` is
> a POINTER into the caller's body, so the conditions can be recovered
> syntactically after the fact: walk the body down to that exact node and
> collect every branch entered on the way. No `elab_*` function changes, and
> the facts are the same ones `rt_prove_paths` already synthesizes for return
> obligations. The only new state is the caller's body form, back-filled
> alongside the env it already back-fills.
>
> Three declines, each because the alternative is unsound rather than merely
> imprecise:
>
> - **A target inside an `if`'s CONDITION** gets no fact from that `if`. The
>   condition is evaluated before either branch is chosen, so neither it nor
>   its negation holds there.
> - **A body that assigns anywhere** declines path conditions outright. A
>   condition mentioning a reassigned name may no longer hold at the call --
>   the same staleness the `do`-block scan guards, reusing the same scan.
> - **A `call_form` reachable by more than one route** declines. A macro that
>   shares a node can produce that, and then there is no single path to speak
>   of.
>
> The remaining unknown in the recursion row is the function's OWN return, and
> it is the already-documented mutual-recursion ordering limitation: a
> function's return obligation is decided inline, so a self-recursive call
> cannot yet see the refinement being defined. Not a new gap.
>
> Negatives are pinned by an errors fixture rather than by inversion, because
> a crossing is `runtime_guarded` and therefore SILENT when merely unprovable.
> All three negatives are refuted outright with a counterexample under
> `--strict-refine`, so a bad path condition makes the refutation disappear and
> the fixture stop erroring. `match` arms and `let` bindings are not yet
> collected as crossing path conditions -- the natural follow-up, and now a
> small one.
>
> Verified: suite 2328/0, solver unit 47/0, 400 fuzz cases across two seeds
> with 0 soundness bugs.
>
> ### Crossing path conditions for `let` and `match` (landed 2026-07-25)
>
> The follow-up named in the previous entry, plus a defect the follow-up's own
> negative test exposed.
>
> | Shape | Before | After |
> |---|---|---|
> | `(let [y (+ x 1)] (sdiv 10 y))`, `x > 0` | unknown | proven |
> | `(match x 5 (sdiv 10 x) _ 0)` | unknown | proven |
> | `(match t (Num v) when (not= v 0) (sdiv 10 v) ...)` | unknown | proven |
>
> Constructor tags and field selectors are deliberately NOT collected. They
> arrive with pattern binders, and a binder shadowing an outer name is the
> unsound direction rather than the imprecise one -- which the next paragraph
> is about.
>
> **A pre-existing false proof, found by a negative test.** The shadow probe
> `(let [x (- x x)] (sdiv 10 x))` under `x > 0` came back PROVED. The encoder
> has one flat namespace, so the argument's `x` encodes as the same variable as
> the parameter `x` and inherits `x > 0` -- a fact about a different value.
> This predates path conditions entirely: the crossing environment has always
> carried the parameter refinements. It was never a miscompile, because a
> crossing is a diagnostic layer that never elides the callee's entry check,
> which still fires -- but `--strict-refine` accepted a program that panics,
> which is a wrong answer.
>
> Dropping just the binding's equation did not fix it: the collision is in the
> NAME, not the fact. `rt_prove_paths` handles the same collision by
> alpha-renaming, which it can because it also rewrites the body it is about to
> prove; a crossing has only a call form to leave alone, so the crossing is
> abandoned instead. Archived as
> `docs/archive/crossing-shadowed-binder-false-proof.md`.
>
> **Hypotheses are not free.** Adding the `let` equation broke
> `errors/refine-lambda-bad-arg`, which went from a compile error to Unknown.
> `(let [f (fn ...)] (f 0))` asserts `f = <lambda>`, the encoder abstracts the
> lambda to an uninterpreted symbol, and `refine_model_search` declines any VC
> containing one -- so a goal that was REFUTED with a counterexample degraded to
> Unknown. A hypothesis can never make a goal easier to prove incorrectly, but
> it can make it harder to REFUTE. Function bindings are now skipped, and the
> general hazard is recorded at the site.
>
> Two more codegen defects surfaced, both unrelated to refinements and both
> reproducing with the gate off: a guarded WILDCARD arm on a non-ADT scrutinee
> emits invalid C (`else` without `if`). Same non-ADT match lowering as the
> null-deref above, and probably one fix. (Both are guarded today by the
> `!_scrut_is_adt` disjunct in emit_expr.c's literal-match arm; the report
> these notes say was filed never was.)
>
> Verified: suite 2329/0, solver unit 47/0, 400 fuzz cases across two seeds
> with 0 soundness bugs.
>
> ### Fixing the match-lowering defects the fuzzer found (landed 2026-07-25)
>
> Not refinement work, but on the path: the two codegen defects the datatype
> fuzzer shape turned up had forced fixture shapes to be routed around and
> generator rungs to be dropped, and both reproduce with the gate off.
>
> They lived in one block and had one fix. The scalar-match path was entered
> only when SOME arm spelled a literal, so a match with only wildcard or binder
> arms fell through to the ADT path and read `adt->name` through a NULL AdtDef
> -- an `:int` has no AdtDef. And that path emitted an if/else-if chain, which
> cannot express a guard: a guarded arm may fail its guard and fall through, so
> its test is not the whole condition. A guarded wildcard emitted a bare `else`
> and the arm after it emitted a second one, producing `'else' without a
> previous 'if'` -- a program that could not be built at all.
>
> The fix is one structure: a flat sequence of `if` blocks each jumping to an
> end label on success, which is what the ADT path already did and for exactly
> this reason. It also gives a binder somewhere to be bound BEFORE a guard that
> mentions it is evaluated.
>
> Worth recording that the report was WRONG TWICE before it was right. The
> first reading blamed `when`; the second blamed arm 0's position; the actual
> gate is `_has_lit` over every arm. Both earlier readings were consistent with
> the probes run at the time -- which is the argument for widening the probe
> set before writing down a root cause, not after.
>
> The dropped `shape_datatype` rungs are restored, and a third defect noticed
> alongside was fixed in the following slice: a var pattern's binder was not in
> scope for its own guard (`docs/archive/match-var-pattern-guard-scope.md`).
> That one is elaboration rather than lowering, so it never reached the block
> above.
>
> Verified: suite 2330/0, solver unit 47/0, 400 fuzz cases across two seeds
> with 0 soundness bugs.
>
> ### Landed: a var pattern's binder is in scope for its own guard
>
> `elab_match`'s scalar path elaborated a `when` guard BEFORE creating the arm
> scope, so `(match p x when (> x 2) x _ 0)` failed with "unbound symbol 'x'".
> The ADT path had it right and said so -- "while arm scope is still live" --
> and the scalar path now matches it. A second gap closed with it: the scalar
> path never type-checked the guard, so an `:int` guard was accepted on one
> path and rejected on the other; the ADT path's bool check is now mirrored,
> and it is reachable only because the guard is elaborated somewhere that has a
> type to check.
>
> This unblocked a fuzzer rung class that had never been generated because it
> could not compile. `shape_datatype` gained five var-binder-guard rungs, now
> ~29% of that shape's samples. They are the sharpest guard rung available: the
> guard is written about the BINDER while any synthesized hypothesis is about
> the SCRUTINEE, and two of the five make the two names disagree on purpose, so
> reading the guard as a constraint on `p` would assert something false.
>
> Verified: suite 2332/0, solver units 2/2, 400 fuzz cases over seeds
> 2201/2202 plus the self-test, 0 soundness bugs. The 9 report-only
> `SUSPICIOUS_over_refute` cases were inspected -- none is a var-binder-guard
> shape.
>
>
> ### Landed: a labelled SMT-LIB corpus, replayed without Z3
>
> The one unmet Z3 retirement criterion was that the corpora be checked in and
> replayable with no solver present. Nothing could read a `.smt2` file --
> `refine_smtlib.c` is a serializer -- so this was blocked on a reader, not on
> obtaining benchmarks.
>
> `tests/unit/refine_corpus.c` is that reader plus a runner (ctest target
> `tur_refine_corpus`, ~0.1s, built unconditionally -- the builds with no Z3 are
> exactly the ones it must run in). The bridge from satisfiability to entailment
> is exact: assert everything as hypotheses and take `false` as the goal, so
> `hyps |- false` is VALID iff the benchmark is UNSAT. A `sat` benchmark
> answered VALID is then precisely the one-directional invariant broken, decided
> from the label alone.
>
> The reader SKIPS a benchmark whole rather than parsing it partially. Dropping
> an assertion only weakens hypotheses, so it cannot make the chain prove
> something it should not -- but it would quietly turn a real benchmark into a
> trivial one and report a pass for work not done. Skips are counted and
> printed, and one benchmark is deliberately outside the fragment to keep that
> path exercised. The runner also fails on an empty corpus or one where nothing
> was decided, so losing the corpus cannot read as green.
>
> Corpus: 103 benchmarks *at the time of this entry* -- 23 hand-written
> one-idea-each across QF_UF (incl.
> arity-2 and transitive congruence), QF_IDL (negative-weight cycles), QF_LIA
> (integer gap, scaling), QF_LRA (strictness where the integer version is
> unsat), QF_UFLIA (needs both theories), and boolean structure (`or`, `=>`,
> `distinct`, `let`); plus 80 generated, curated to 10 per (theory, status)
> bucket.
>
> **Every label agrees with both Z3 and cvc5**, checked by
> `tests/corpus/validate-labels.py`. Two solvers rather than one because Z3 is
> the thing being retired -- a label confirmed only by Z3 inherits whatever Z3
> gets wrong, and cvc5 is a different implementation lineage, so agreement
> between them is stronger evidence than either alone.
> That check matters more than it looks: a benchmark wrongly labelled `sat` can
> never fail and silently stops testing anything, while one wrongly labelled
> `unsat` inverts the check entirely. Both scripts (`validate-labels.py`,
> `generate-corpus.py`) are development scaffolding in exactly the way
> `refine_libz3.c` is -- neither is built, linked, or imported by the suite.
>
> **CORRECTED (2026-08-01): the corpus is now 125 benchmarks** (69 labelled
> `unsat`, 56 labelled `sat`), and the current replay is 68 unsat proved / 56
> sat correctly declined / 0 soundness failures -- the one undecided benchmark
> is the deliberately out-of-fragment skip described above. The 103 figure in
> this entry and the next paragraph is the size on 2026-07-25; it is kept as the
> dated record. `find tests/corpus/smtlib -name '*.smt2' | wc -l` is the check.
>
> Evidence, all clean: 103 committed benchmarks replayed with no Z3 (55 unsat
> proved, 47 sat correctly declined, 0 soundness failures); a 3600-benchmark
> generated soak with Z3-supplied labels across seeds 7/8/9 (2646 satisfiable,
> **0** wrongly proved); and 7000 VCs through the VC-level differential fuzzer
> against Z3 4.13 (0 soundness bugs, 0 refutation bugs). The last of these had
> not been exercised in a long while -- it only builds in an oracle build.
>
> Not done at the time of this entry: importing the **SMT-LIB benchmark
> library**, whose hosts were unreachable from the container this was written
> in. That is now a data drop -- the reader takes ordinary `.smt2` with
> `(set-info :status ...)`, skips what falls outside the fragment, and the
> runner recurses -- rather than an engineering task.
>
> **CORRECTION (2026-07-30): this is DONE and has been for a while.** The
> import lives at **`github.com/turmeric-lang/smt-lib-benchmarks`** (200
> benchmarks, 8 logics x 25, produced by `import-smtlib.py --sample 25`). It is
> a separate repo because the data is too large to check into this tree, NOT
> because it could not be obtained. Do not re-derive "the library was never
> imported" from the paragraph above -- see the Z3 retirement criteria section
> for the current state.
>
> Worth naming precisely, because this plan and the corpus README both got it
> loose at first. **SMT-LIB is a standard** (the 2.6 language, the theory and
> logic declarations -- reference documents, and where `:status` is defined) and
> **separately a benchmark library** (the labelled data). **SMT-COMP is neither**
> -- it is the annual competition, which draws problems from the library and
> publishes results and tooling. It is not a benchmark distribution, and an
> earlier revision here implied it was.
>
> The distinction is not pedantry: the package-registry search below found 38
> `.smt2` files in the Rust `smtlib` crate, and every one is a **logic
> declaration from the standard** (`(logic QF_NRA :written-by "Cesare Tinelli"
> ...)`) -- no assertions, no `:status`, nothing to solve. The crate vendored
> the reference, which is what a parser needs and a corpus does not.
>
> The registries (PyPI, npm, crates.io, the Go proxy) ARE reachable and were
> searched rather than assumed, since they would be a viable transport for
> vendored data; the benchmark library is not published through any of them.
> The search is tabulated in `tests/corpus/smtlib/README.md` so it does not get
> repeated. What the registries did yield is cvc5, which is why the labels now
> carry two independent confirmations instead of one.
>
> ### Landed: closedness is a property of the goal, not of the model
>
> A runtime-guarded crossing errors on a DEFINITE violation and stays quiet on
> a merely-unprovable one. That split is right; the measurement of it was not.
> "Closed" was `model->n == 0`, which counts every variable the VC declared --
> including the caller's own parameters, whether or not the goal mentions them.
> So the identical violation was an error in a zero-parameter caller and silent
> in a one-parameter one:
>
> ```turmeric
> (defn a []        : int (safe-div 10 0))   ; was reported
> (defn b [n : int] : int (safe-div 10 0))   ; was silent
> ```
>
> Closedness is now read off the goal (`vc_term_is_ground` over the substituted
> goal term). The change is monotone -- the old test is kept and the ground-goal
> case added -- so it can only widen what is reported. The hypothesis half is
> untouched and load-bearing: the model search still has to satisfy the
> hypotheses, which is what keeps the widened rule off a branch the path
> conditions exclude.
>
> `emit_model_note` had the same wart and went with it: it suppressed the
> counterexample line in the closed case from the model's size, so the newly
> reported cases printed `counterexample: n = -2` for a goal false regardless of
> `n`. It now takes the same `closed` flag as `emit_predicate_note`.
>
> **This strengthened the previous slice for free.** A violating argument at a
> dynamic typeclass dispatch now reports by DEFAULT rather than only under
> `--strict-refine` -- the abstract receiver was exactly the unrelated parameter
> that made those models look open. The fixture was moved off `--strict-refine`
> to pin the stronger behaviour.
>
> Blast radius, measured rather than assumed: suite 2336 -> 2338 with two added
> fixtures and ZERO pre-existing fixtures newly erroring, and the source-level
> fuzzer returned counts identical to their recorded baselines across four seeds
> and 800 cases (suspicious 4/5/4/13, 0 soundness bugs, 0 other BUG classes).
> The accepted cost is that a violating call on a reachable-but-not-exercised
> branch is now reported -- a latent bug, and already the standard applied to a
> zero-parameter caller, so this makes the treatment consistent rather than
> adopting a new posture. Seven positive/negative shapes checked by hand.
>
> Archived: [docs/archive/runtime-guarded-refutation-needs-closed-model.md](runtime-guarded-refutation-needs-closed-model.md).
>
> ### Landed: class parameter inheritance, and reading B + a lint
>
> The design question the entry below was blocked on is answered: a class
> parameter refinement **bounds instances**; it does not bind callers. Plus a
> lint for the gap that leaves.
>
> **The inheritance asymmetry was a defect, and fixing it first shrank the
> question.** An instance parameter with no annotation now inherits the class's
> refinement, mirroring the result direction. The inherited predicate is
> published on the instance method's binding -- the thing both the call-site
> crossing and the entry-check injection read -- so an omitted annotation is now
> enforced statically AND at run time, where before it was enforced nowhere.
> Writing an explicit annotation (including a bare `: int`) is how an instance
> opts out and demands less, which stays legal. Once omission inherits, most
> instances demand exactly the class predicate and the two readings stop
> differing for them.
>
> **For what remains -- an instance that EXPLICITLY demands less -- the resolved
> instance governs.** A statically-resolved dispatch knows which implementation
> runs, so it is checked against that implementation's contract, the more
> precise of the two. This rejects no working program. It is linted
> (`TUR-W0377`) because the leniency is not part of the interface and evaporates
> when dispatch goes dynamic or a stricter instance appears. Only a DEFINITE
> violation warns -- one the class predicate refutes outright, not one it merely
> cannot prove -- so an unconstrained argument stays silent.
>
> The alternative (class binding on callers) was rejected: it turns correct
> programs into errors, which is a bad thing to adopt while the experiment is
> heading for a graduate-or-shelve decision at `0.34.0`. Liskov-style contract
> variance also points the same way -- a caller that statically knows the
> implementation may use its weaker precondition.
>
> Implementation notes worth keeping. The lint carries the class predicates on
> the `RefineCallSite` rather than replacing the obligation, so what a call must
> PROVE is unchanged. They are attached keyed on `(callee, call_form)`, not on
> the index `refine_note_call_site` returns -- that function yields the current
> count when it deduplicates, which names the last crossing rather than the
> matching one, and would have linted an unrelated call. The crossing gate was
> loosened to walk a site where only the CLASS has predicates, since an instance
> that demands nothing publishes no arrays of its own and that is exactly the
> shape being linted.
>
> Verified: suite 2336/0, solver units 1/1, 200 fuzz cases at seed 4401 with 0
> soundness bugs. That seed reported 13 report-only suspicious cases against the
> usual 4-5, so seeds 2201/2202/3301 were re-run and returned 4/5/4 -- identical
> to their pre-change counts, confirming seed variation rather than a regression.
> Six negative controls checked by hand: satisfying argument, unknown argument,
> instance restating the predicate, class with no refinement, unannotated
> instance, and the violating case itself.
>
> ### Landed: a dynamic dispatch crosses into the CLASS signature
>
> The conservative half of the entry below. A dispatch on an abstract receiver
> resolves to no instance -- it falls back to an arbitrary carrier-compatible
> one, which is not necessarily the instance that runs -- so it now records its
> crossing against the CLASS method signature instead.
>
> Sound by the variance argument that already licenses result propagation, run
> backwards: `TUR-E0374` rejects an instance demanding more than its class, so
> every instance's parameter predicate is implied by the class's, making the
> class predicate the strongest demand true of every instance. It is also
> strictly stronger than the alternative -- the fallback instance's predicate is
> weaker than the class's, so checking against it would demand less than the
> contract while appearing to check it.
>
> Implemented by synthesising a `Binding` that carries the class's parameter
> predicates (`rt_class_method_refine_binding`), so `refine_resolve_call_sites`
> needed no change at all. Cached on the `TypeClassMethod` because crossings
> deduplicate on `(callee, call_form)` -- a fresh Binding per site would defeat
> that and double-report a re-elaborated call.
>
> Deliberately NOT included: the static-site half, where an instance that
> demands less still raises nothing. That one turns working programs into
> errors and needs the design question below answered first.
>
> Measuring it turned up a separate, pre-existing defect that limits how much
> the slice is worth by default: a runtime-guarded counterexample is reported
> only when the model is CLOSED, and closedness is measured by the VC's
> variable count rather than the goal's. Any caller with a parameter -- which
> includes every caller of a dynamic dispatch, the receiver being one -- has an
> open model, so the violation surfaces under `--strict-refine` rather than by
> default. Independent of typeclasses; `(safe-div 10 0)` reports in a
> zero-parameter caller and not in a one-parameter one. Filed as
> [docs/archive/runtime-guarded-refutation-needs-closed-model.md](runtime-guarded-refutation-needs-closed-model.md);
> not fixed here because it widens hard errors across every crossing, which is
> its own decision.
>
> Verified: suite 2334/0, solver units 1/1, 200 fuzz cases seed 3301 with 0
> soundness bugs. Fixtures `refine-class-param-dynamic-dispatch` (proofs pinned
> under `--strict-refine`) and `errors/refine-class-param-dynamic-violated`
> (pinned by inversion).
>
> ### Next slice
>
> Two pieces of remaining work now have their own plans:
>
> - [refined-graduation-plan.md](refined-graduation-plan.md) -- the decision and
>   the mechanical checklist for removing the gate. The clock (`expires_at`
>   `0.34.0`) makes this the only item with a deadline.
>
>   **LANDED 2026-08-01 (targeting v0.33.0).** All four preconditions closed and
>   the flip is in: static discharge is unconditional, the `EXPERIMENTS[]` and
>   `LANG_LAYERS[]` rows are deleted, `g_opt_refined` and its 15 conditionals are
>   gone, and `"refined"` sits in `GRADUATED[]` / `GRADUATED_LAYERS[]` so a
>   lingering `--enable=refined` is a `TUR-W0063` no-op and `#lang turmeric
>   refined` a `TUR-W0064` no-op for one minor line. `--enable=refined` was
>   stripped from 73 fixture `flags` files; the shim is pinned on purpose by
>   `refine-graduated-enable-noop` and `refine-graduated-lang-layer-noop`. **The
>   deadline item is closed** -- there is no longer an item in this plan with a
>   clock on it.
> - [corpus-reader-tail-plan.md](history/corpus-reader-tail-plan.md) -- the last 7
>   skips in the SMT-LIB corpus reader, with a recommendation to do one of the
>   two items and skip the other.
> - [../../archive/refined-dogfooding-plan.md](refined-dogfooding-plan.md) --
>   on hold, waiting on a real program rather than on effort. It carries a
>   tiered list of what such a program should contain, since coverage there is
>   what makes the exercise worth running, and it is the only source of the
>   compile-time figure graduation needs and the real-VC cross-check Z3
>   retirement needs.
>
> Candidates, roughly by value:
>
> - **Dynamic typeclass dispatch** -- INVESTIGATED; the entry was mis-scoped.
>   The gap is not specific to dynamic dispatch. The argument obligation is
>   built from the resolved INSTANCE, and an instance may legally demand less
>   (an unannotated instance parameter demands nothing at all), so a class
>   parameter refinement is enforced only when some instance happens to restate
>   it. The static case has the same hole as the dynamic one, and it is the more
>   common way to write an instance.
>
>   Not a soundness hole: the method's own entry check is retained, and a
>   genuinely dynamic two-instance dispatch passing a violating argument aborts
>   identically with the gate off and on. What is lost is the compile-time
>   error.
>
>   The fix is the dual of the result propagation that already ships: raise the
>   obligation against the CLASS parameter predicate at a class-method call
>   site, alongside the instance's. Sound by the same variance argument run
>   backwards -- `TUR-E0374` guarantees no instance demands more than its class,
>   so `class_pred` is the strongest demand true of every instance.
>
>   Blocked on a DESIGN question, not on machinery: is a class parameter
>   refinement a contract callers must honour, or only an upper bound on what
>   instances may demand? The first reading turns today's silent-and-correct
>   lenient-instance programs into errors -- coherent, but a behaviour change
>   that can reject existing code. The second reports only the dynamic case,
>   which is conservative and false-positive-free.
>
>   Demand is low by measurement: 13 `defclass` methods carry a parameter
>   refinement, all in fixtures and none in `stdlib/`; 49 files use `^Class`
>   constrained generics; the intersection is **zero**.
>
>   Filed with repros:
>   [docs/archive/class-param-refinement-not-demanded-of-callers.md](class-param-refinement-not-demanded-of-callers.md).
>   Guide corrected -- it previously showed `(.scale-by 3 0)` erroring without
>   noting that only holds when the instance restates the predicate.
> - **Higher-order callees** -- a function-typed parameter carries no
>   refinements in its type, so neither its body nor its callers can check the
>   eventual call. This needs refinements in function types, which the
>   prototype excludes; the callee's own entry checks still run, so only the
>   static crossing is lost.
> - ~~**Path conditions for call-site crossings**~~ -- LANDED for `if`, `let`,
>   and `match` alike, by syntactic recovery rather than the condition stack
>   this entry predicted. What is still not collected is a constructor tag or
>   field selector, because those arrive with pattern BINDERS and a binder that
>   shadows an outer name is the unsound direction.
>
>   **This entry used to say closing it "needs the crossing walk to
>   alpha-rename". That was wrong, and measuring it corrected the estimate in
>   both directions at once.** It is CHEAPER than recorded: the shadow guard
>   already exists and already runs -- the `match` branch of
>   `rt_collect_path_conds` walks a constructor pattern's binders, sets
>   `shadowed`, and `rt_push_cs_path_conds` abandons the whole crossing. A
>   binder that shadows nothing introduces a fresh name, and equating a fresh
>   name to a selector term is a definition, which needs no renaming at all.
>
>   It is also worth LESS than recorded. Three shapes were measured; two of the
>   three that look like they need it already prove:
>
>   | Shape | Today |
>   |---|---|
>   | Fact about the binder -- `(match b (Box a) (if (> a 200) (target a) 0))` | **proves** |
>   | Tag-side -- which constructor was matched | **proves** |
>   | Fact about the selector, goal about the binder -- `(if (> (.a b) 200) (match b (Box a) (target a)) 0)` | Unknown |
>
>   The first two already work because the condition and the goal mention the
>   same symbol -- the binder -- so there is nothing for a field equation to
>   connect. Only the third shape needs one, and it is the shape where the
>   programmer wrote the test on `(.a b)` OUTSIDE the match instead of on `a`
>   inside it. Moving the `if` into the arm proves it today.
>
>   **Still not now**, but for the honest reason: narrow demand, not blocked
>   machinery. Revisit if a real program writes shape three.
> - ~~**Widen the purity whitelist**~~ -- `match` and field reads landed. The
>   third item, `while` over provably-local state, is **struck**: measuring it
>   showed it is not a classifier case at all. A loop accumulator is Unknown
>   because there is no loop-invariant inference, not because `while` is
>   classified impure, so widening the classifier would change nothing.
>
>   The follow-up phrasing "needs invariants, which the prototype excludes"
>   conflates two things and is worth separating. The prototype excludes
>   INFERENCE -- that is the "Why checking, not inference" decision this whole
>   design rests on. It does not exclude a user-WRITTEN invariant, which would
>   be checking, and which is the same shape as the `:pre` / `:post`
>   annotations that already ship: `(while c :invariant p ...)` gives the
>   solver `p` on entry, requires the body to re-establish it, and hands
>   `p AND (not c)` to whatever follows. That is a bounded, in-philosophy
>   feature, not the multi-week analysis "invariants" suggests.
>
>   **Still not now.** There is no measured demand: 48 refinement fixtures and
>   `stdlib/refine.tur` contain zero loops touching a refinement, and the
>   stdlib layer is entirely scalar aliases (`Nat`, `Pos`, `Byte`, `Percent`)
>   -- not even a bounded-index type, which is the canonical loop-shaped
>   motivation. Adding surface syntax to an experiment that must graduate or be
>   shelved by `0.34.0` is also the wrong direction while the priority is
>   graduating what exists. Revisit when a real program wants it, and revisit
>   it as `:invariant`, not as inference. Path conditions for call-site
>   crossings are worth more and need no new syntax.
>
>   Placeholder plan, with the sketch and the trigger condition written down:
>   [docs/upcoming/hold/loop-invariants-plan.md](loop-invariants-plan.md).
> - ~~**A datatype theory for the VC**~~ -- LANDED IN FULL, see above, and
>   without the new sort this entry assumed it needed. Arm hypotheses and
>   constructor axioms both shipped; `(.a (Box p q))` now reduces to `p`. What
>   the VC still lacks is a way to express a datatype value's SHAPE beyond one
>   level -- nested patterns bind through composed selectors that nothing
>   emits -- but no measured program has wanted that yet.
> - ~~**Whole-program entry-check elision**~~ -- MEASURED AND DECLINED, see
>   below. It buys nothing measurable and carries the feature's largest
>   soundness surface.
>
> ---

## Part 2 -- the original design plan

> **CORRECTED -- everything below this line is the design document, not the
> status.** It was written before implementation and reconciled only where it
> was factually wrong. Read it for the *reasoning*; read the journal above for
> what actually shipped. Sections that were overtaken by events carry an inline
> `SUPERSEDED` or `CORRECTED` marker.

> **Status:** RT0--RT6 + S0--S4 landed (see "Landed so far" above). RT7 landed
> only in its within-unit half; the persistent cross-build cache is
> deliberately not built -- see the measurement below. RT0 syntax/storage is largely covered by the
> existing Contract Types (CT0--CT4) infrastructure; the `#refine{var : T | p}`
> reader is already shipped. The remaining work is the constraint-generation
> and discharge pipeline (RT1--RT4), a stdlib layer of predicate-annotated
> types (RT5--RT6), and -- the substantive new direction -- a **staged,
> in-house decision procedure (S0--S4)** that becomes the *only* shipped solver.
> Z3 is **scaffolding**: a development-time correctness oracle and a transitional
> bootstrap that is never bundled, never auto-fetched, and never linked into a
> default or release build, and that is retired outright once S0--S3 cover the
> fragment. The shipped product carries **zero heavyweight solver dependency**.
>
> **Prerequisites:** Contract Types (CT0--CT4). The `TY_CONTRACT` node,
> `F_CONTRACT_TYPE` reader tag, `#refine{...}` reader, and predicate-as-`Form*`
> storage are already in place and reused directly.
>
> **Gate:** ~~`refined` -- an `EXPERIMENTS[]` row (`--enable=refined`), surfaced
> per-file as the `#lang turmeric refined` semantic layer.~~ **SUPERSEDED
> 2026-08-01: the experiment GRADUATED and there is no gate.** Static discharge
> is unconditional; both rows are deleted and `g_opt_refined` no longer exists.
> `--enable=refined` is a `TUR-W0063` no-op and `#lang turmeric refined` a
> `TUR-W0064` no-op, aging out one minor line after v0.33.0. There was never an
> `-Xrefinements` flag; the retired `-X` surface did not come back for this. The
> "Gating" section below is retained for the reasoning behind the *choice* of
> gate, not as a description of current behavior.
>
> **Solver:** the Z3 scaffold was **retired 2026-07-30 in 0.32.5** and its
> sources deleted. The in-house S0--S4 chain is the only solver, in every build.
>
> **Sibling plans:** [refine-predicate-measures-plan.md](refine-predicate-measures-plan.md)
> (boolean-sorted measures, `RM-B`) and
> [refine-stateful-measures-plan.md](refine-stateful-measures-plan.md)
> (refinements over mutable state, `RM-S`) extend the predicate language beyond
> what this plan scopes.
>
> **Last updated:** 2026-08-01 (reconciliation pass: Part 2 corrected against
> the tree; `refined` graduation and Z3 retirement recorded). The journal in
> Part 1 was last extended 2026-07-30.

---

## Motivation

Contract types (`#refine{ x : T | p }`, CT0--CT4) verify predicates at
**runtime**. Refinement types go further: the compiler attempts to **prove**
predicates statically, emitting a runtime check only when static proof fails.
This eliminates whole classes of defensive guards that programmers currently
scatter through the codebase -- e.g. `require! (not= divisor 0)` becomes a
type-level obligation the compiler resolves at every call site.

The single fact that shapes this entire plan: **every refinement already has a
runtime-contract meaning.** Because of that, the static discharger is allowed
to answer `Unknown` on any obligation and stay sound -- the obligation simply
falls back to the runtime check it would have had anyway (or, under
`--strict-refine`, a compile error telling the user to add an annotation).
That fallback is what makes an incrementally hand-rolled solver *safe*: the
soundness obligation is one-directional (never say **valid** when it is
**invalid**; saying **unknown** is always fine), so a 40%-complete solver is
still a real, shippable feature rather than a broken one.

Goals:

- Catch division-by-zero, out-of-bounds access, and overflow at compile time
  rather than at runtime.
- Allow library authors to publish APIs with machine-checked pre/post conditions
  (e.g. `Vec` indexing, `sqrt` on non-negative floats).
- Compose with the existing substructural and session type systems without
  introducing mutual dependencies.
- Grow a small, auditable, dependency-free decision procedure that lives inside
  `tur.wasm`, so the browser playground gets real static checking at zero
  download -- and no heavyweight backend is ever fetched, in the browser or
  anywhere else.

Non-goals for this prototype:

- Full dependent types (Pi types, proof terms, eliminators).
- Universal or existential quantification inside predicates.
- Termination checking or total-correctness verification.
- Refinements that depend on algebraic effects or mutable state.
- **Refinement *inference*** (LiquidHaskell-style predicate abstraction +
  Horn-clause fixpoint). Turmeric refinements are **written, not inferred**;
  we only *check* closed implications. This deletes the single hardest layer
  of a refinement backend (see "Why checking, not inference" below). RT4 adds a
  deliberately small, template-based propagation convenience on top -- it is not
  general inference.

---

## Background: What Is Already Shipped

| Component | Location | Notes |
|---|---|---|
| `TY_CONTRACT` type node | `src/compiler/types.h:121` | Stores `{var : base | pred}` |
| `F_CONTRACT_TYPE` reader tag | `src/compiler/reader.c` | Parses `#refine{ x : T \| p }` |
| `type_contract()` constructor | `src/compiler/types.h:936` | Builds a `TY_CONTRACT` from a `Form*` predicate |
| Contract elaboration | `src/compiler/elab_types.c:1072` | Checks predicate is well-typed + pure |
| Runtime check insertion | `src/compiler/elab_core.c` | Emits C `assert` / panic for CT3 |
| `:pre` / `:post` in `defn` | `src/compiler/elab_fns.c` | Parsed and elaborated; currently runtime only |
| `EXPERIMENTS[]` registry | `src/runtime/experiments.c` | Where the `refined` row lands (RT0). **Row deleted 2026-08-01 on graduation**; the name is in `GRADUATED[]`. |
| `LANG_LAYERS[]` registry | `src/compiler/lang_layers.c` (lang-layers-plan L0) | Where the `refined` semantic layer lands (RT0). **Row deleted 2026-08-01**; the name is in `GRADUATED_LAYERS[]`. |

Refinement types reuse all of the above. The delta is:

1. A **constraint collector** that gathers `TY_CONTRACT` predicates as proof
   obligations during elaboration (RT1).
2. A **normalized VC representation** -- the neutral, backend-independent form
   every obligation is lowered to before a solver sees it (RT2).
3. A **solver seam**: the `decide(vc) -> Valid | Invalid(model) | Unknown`
   contract that both the in-house procedures and Z3 implement, and fall
   through to each other along (RT2/RT3).
4. A **discharge pass** that runs the seam and marks obligations proven (elide
   runtime check) or not (keep runtime check, optionally warn/error) (RT3).
5. A **Z3 backend** behind the seam, used as *scaffolding* -- a dev-build
   correctness oracle and a transitional bootstrap so refinement types can be
   exercised end-to-end while the in-house solver is still a stub. It is never
   part of a default or release build and is retired once S0--S3 land (RT3).
6. A **staged in-house decision procedure** (S0--S4) that decides the fragment
   with no dependency and *becomes the only shipped solver* (S0--S4).
7. A **predicate propagation** layer for simple arithmetic (RT4).
8. A **WASM integration layer**: the in-house solver compiles into `tur.wasm`
   directly and is the whole story there -- no Z3-in-WASM, no `z3-solver`
   fetch, no bundle cost (RT5a).

---

## Design Decisions

### Gating: the `refined` experiment + `#lang` layer

> **SUPERSEDED 2026-08-01 -- the gate is GONE.** `refined` graduated: static
> discharge is unconditional, both registrations described below were **deleted**
> (`EXPERIMENTS[]` row, `LANG_LAYERS[]` row), `g_opt_refined` and its 15
> conditionals are removed, and the four `experiment_warn_if_used("refined")`
> calls with them. The name moved to `GRADUATED[]` / `GRADUATED_LAYERS[]`, so
> `--enable=refined` is a `TUR-W0063` no-op and `#lang turmeric refined` a
> `TUR-W0064` no-op -- both age out one minor line after v0.33.0.
> `LANG_LAYERS[]` now holds `stringed` alone and no semantic layer.
>
> This section is kept for the *reasoning*: why the experiment mechanism was the
> right gate rather than a resurrected `-X` flag, and why the `#lang` layer had
> to point at the experiment rather than open a second enable path. That
> reasoning still governs the next in-flight feature. It no longer describes
> refinement checking. **`--strict-refine` is unaffected and still ships** -- it
> was always a diagnostic-strictness knob, never an experiment.

Refinement checking is an in-flight compiler feature, so it ships behind the
experiment mechanism, **not** a resurrected `-X` flag. Two coordinated
registrations:

1. **`EXPERIMENTS[]` row** in `src/runtime/experiments.c` with all seven
   descriptor fields populated:

   ```c
   { "refined",
     "static discharge of #refine{...} predicates (refinement types)",
     "docs/archive/refinement-types-plan.md",
     "0.<minor>.0",              /* introduced -- set at RT0 landing */
     "0.<minor+3>.0",            /* expires_at -- soft deadline; review at cut */
     XF_LIFECYCLE_PROTOTYPE,
     &g_opt_refined },
   ```

   The feature's discharge entry point reads `g_opt_refined` and calls
   `experiment_warn_if_used("refined")` so the lifecycle warning
   (TUR-W0060/W0061) fires.

2. **`LANG_LAYERS[]` row** (semantic layer, per the lang-layers plan L4) whose
   `experiment` field points at that same row. `#lang turmeric refined` is then
   *exactly* `--enable=refined` scoped to one file -- no parallel enable path. A
   project manifest that disables the `refined` experiment turns a
   `#lang ... refined` file into a **hard error** (`this file requires layer
   'refined', which is disabled by the project manifest`), never a silent
   ignore.

Enable precedence is the standard experiment chain: user file
(`~/.config/turmeric/experiments.tur`) < project manifest (`build.tur`
`:experiments`) < CLI (`--enable=refined`), with `#lang turmeric refined` acting
as the file-scoped form of the CLI enable. When `refined` is off, the elaborator
behaves exactly as with contracts alone: predicates parse and stay runtime-only,
the discharge pass does not run.

`--strict-refine` (a diagnostic-strictness knob, *not* an experiment) upgrades
`Unknown`/`Invalid` obligations from "keep the runtime check" to a hard compile
error -- for users who want a fully-discharged build with no silent runtime
fallbacks.

### Predicate Language Scope -- co-designed with the solver

The prototype targets **quantifier-free linear integer/real arithmetic with
equality and uninterpreted functions** -- SMT-LIB `QF_UF` + `QF_IDL`/`QF_LIA`/
`QF_LRA`. This is not an arbitrary cut: it is the exact fragment the in-house
solver (S0--S4) can be made *complete* on, and it is also the corner of Z3 that
the fallback ever touches.

```
pred ::= (= e e) | (not= e e) | (< e e) | (<= e e) | (> e e) | (>= e e)
       | (and pred pred ...) | (or pred pred ...) | (not pred)
       | (=> pred pred)
       | (measure e ...)             ;; named measure -> uninterpreted fn (EUF)
       | true | false

expr ::= integer-literal | float-literal
       | bound-var                   ;; the variable from #refine{ x : T | ... }
       | fn-param                    ;; a visible parameter in scope
       | (+ e e) | (- e e) | (* e const) | (/ e const)  ;; linear only
       | (mod e const)               ;; modular arithmetic
```

Two language rules -- stated as rules, not accidents -- keep the fragment on the
cheap side of the solver cliff:

- **Named measures are uninterpreted functions.** `len`, `elems`, and similar
  measures become opaque function symbols reasoned about by congruence closure
  (EUF), never unfolded. This is what makes S1 tractable.
- **Variable*variable multiplication is uninterpreted.** `(* x const)` is linear
  and stays interpreted; `(* x y)` with both variable is treated as an opaque
  term. Congruence closure still handles it soundly (it loses the arithmetic
  facts, not soundness), and any genuinely nonlinear obligation falls through to
  Z3 or to a runtime check. **We do not climb the nonlinear wall** -- no nlsat,
  ever. The encoder warns (TUR-W0373) when a predicate contains a nonlinear
  subterm.

Restricting the predicate language this tightly is a lever we hold that
LiquidHaskell-on-Haskell does not: it is *our* language, so "small **and
complete on the permitted fragment**" is an achievable target rather than a
fantasy. Language design and solver scope move together.

Higher-order predicates, recursion inside predicates, quantifier alternation,
and effect-dependent predicates are rejected at elaboration time with a
dedicated diagnostic.

### Why checking, not inference

LiquidHaskell's hard part is not *deciding* verification conditions -- it is
*inferring* the refinements in the first place (predicate abstraction plus
Horn-clause fixpoint solving over `liquid-fixpoint`). That is the machinery to
dread reimplementing. Because Turmeric makes people **write** their refinements,
that entire layer is deleted. We only answer "is this one closed implication
valid?" Checking is a decision problem; inference is a search for predicates
satisfying a recursive constraint system. Dropping inference removes the hardest
~60% of what a refinement backend does, and it is the reason a hand-rolled
solver is realistic here at all.

### The Solver Seam

Every backend -- the in-house S0--S4 procedures and Z3 alike -- sits behind one
interface and returns one of three verdicts:

```c
typedef enum { RT_VALID, RT_INVALID, RT_UNKNOWN } RefineVerdict;

typedef struct {
    RefineVerdict verdict;
    /* RT_INVALID only: a counterexample model, in normalized-VC variable terms
     * (translated to source syntax by RT6). NULL otherwise. */
    RefineModel  *model;
} RefineDecision;

/* A backend decides a single normalized VC. It MUST NOT return RT_VALID unless
 * the goal is genuinely entailed by the hypotheses; returning RT_UNKNOWN is
 * always permitted and always sound. */
typedef RefineDecision (*RefineBackend)(const RefineVC *vc, Arena *a);
```

**The soundness invariant is one-directional and absolute:** a backend may
never answer `RT_VALID` for an invalid obligation. `RT_UNKNOWN` is always a
safe answer. `RT_INVALID` should carry a model but is allowed to be a
best-effort "definitely not valid, no model."

**Fall-through chain.** The discharge pass runs backends in ascending cost and
stops at the first non-`Unknown` verdict:

```
S0 (normalize/trivial) -> S1 (EUF) -> S2 (arithmetic) -> S3 (N-O combine)
   -> [dev-only Z3 scaffold] -> RT_UNKNOWN => keep runtime check (or error under --strict-refine)
```

Early stages return `Unknown` for anything outside their competence; the next
stage picks it up. **In every default and release build the chain is the
in-house stages only** -- Z3 is not linked, so an obligation no stage can decide
falls straight to its runtime check. Z3 appears as the chain's last link *only*
in dev builds that opt into the scaffold (see below); it is a bridge, not a
destination, and it is removed entirely once S0--S3 land.

This seam is the whole reason the in-house solver can be built a stage at a time
and stopped wherever we like: adding a stage only ever *moves obligations left*
in the chain (off the Z3 scaffold or off the runtime-check fallback onto a
faster, dependency-free path). It never changes an answer, because every stage
preserves the soundness invariant.

### Backend strategy: Z3 as scaffolding, in-house as the product

The shipped solver is the in-house chain, full stop. Z3 exists only to make
building that chain fast and safe, and it is designed to disappear. Its two
scaffolding roles, and the single build option (`-DTUR_REFINE_Z3_ORACLE=ON`,
**off by default**, refused entirely for release and WASM builds) that gates
both:

1. **Correctness oracle.** In an oracle build, run *both* the in-house stage and
   Z3 on every obligation and assert agreement. If the in-house solver ever
   answers `Valid` where Z3 answers `Invalid`, that is a soundness bug, caught
   immediately (`TUR-I0379`, downgraded to `Unknown` so the build stays sound).
   Fuzz the in-house solver against generated VCs plus the labelled
   `QF_UF`/`QF_IDL`/`QF_LIA`/`QF_LRA` SMT-LIB benchmark corpora. This is the role
   that justifies Z3's existence, and it is dev-only from day one.
2. **Transitional bootstrap.** In an oracle build, Z3 is also the chain's last
   link, so the end-to-end pipeline (RT1 collector -> RT2 VC -> RT3 discharge ->
   codegen elides the check) can be tested *before* any in-house stage exists.
   This lets RT3 land and be exercised while S0--S4 are still stubs -- without
   ever putting Z3 in a shippable artifact.

Consequences that follow directly from "scaffolding, not backend":

- **No CPM auto-fetch, no bundling, no user setup.** The oracle build uses a
  **system-installed** Z3 (`find_package(Z3 4.12 CONFIG)`); if absent, the
  oracle option is simply unavailable and the build proceeds in-house-only. Z3
  is never downloaded, never statically linked into a distributed binary, and
  never added to the WASM bundle. The 15--20 MB binary-size question the
  original plan wrestled with disappears.
- **A release build discharges only what the in-house chain can.**
  Early on (RT3, before S-stages) that was little, and unhandled obligations
  fell to runtime checks -- acceptable at the time precisely because `refined`
  was an experiment-gated prototype with an `expires_at` contract. (That
  justification expired with the gate: S0--S3 all landed, coverage climbed as
  predicted, and graduation followed. An undecidable obligation still falls to
  its runtime check -- that is the permanent design, not a prototype
  concession.) It never depended on shipping Z3.
- **The browser story is unconditional, not "tail-fetches-Z3".** S0--S2 compile
  into `tur.wasm` and are the entire discharge path there. There is no
  `z3-solver`, no lazy fetch, no `SharedArrayBuffer`/`Atomics.wait` bridge.

**Retirement. DONE -- executed 2026-07-30 in 0.32.5.** Z3's bootstrap role
ended when the in-house chain could discharge the fixtures RT3 relied on; its
oracle role ended once the corpus and differential runs were trusted. The Z3
backend file, the `find_package` block, the `TUR_REFINE_Z3_ORACLE` option, the
VC-level differential fuzzer and every `#ifdef` are **deleted** -- see "Z3
retirement criteria" below for the criteria and the evidence they were met.
Nothing in the shipped compiler ever referenced them, so the retirement is
invisible to users.

The one dividing line we do **not** cross: a competitive **DPLL(T)** SAT engine
driving the theories is a months-to-years project. Decision procedures for a
fixed theory or a Nelson-Oppen combination are a solo, incremental project;
DPLL(T) is not. Explicit annotations keep the propositional structure of
individual VCs small (S4 handles that with naive case-splitting / small-DNF), so
we very likely never need to cross that line. If a VC's boolean structure is
genuinely too rich, the honest answer is `Unknown` -> runtime check.

The **blueprint** for the whole in-house architecture -- congruence closure +
simplex + Nelson-Oppen + matching -- is the pre-Z3 ESC/Java theorem prover
**Simplify** (Detlefs, Nelson, Saxe). It is a complete, documented description
of exactly the thing S0--S4 build, written before SMT solvers got big enough to
make people forget you could hand-roll one.

### Relationship to Contract Types

| | Contract Types (CT) | Refinement Types (RT) |
|---|---|---|
| Predicate syntax | `#refine{ x : T \| p }` | same |
| Verification time | Runtime always | Compile time when possible; runtime fallback |
| Solver involvement | None | Yes -- via the seam (in-house only; the Z3 scaffold was retired 2026-07-30) |
| Gate | contracts (shipped) | ~~`refined` experiment / `#lang turmeric refined`~~ **none -- graduated 2026-08-01; discharge is unconditional** |
| `TY_CONTRACT` node | Yes | Same node; adds `rt_discharged` bit |

Refinement elaboration is **additive**: if `refined` is off, the elaborator
behaves exactly as with contracts alone. The discharge pass simply does not run.

> **CORRECTED 2026-08-01.** There is no longer an "off" -- the discharge pass
> always runs. Additivity survives in the form that matters and is pinned by
> fixtures: static discharge only ever *removes* a runtime check it has proved
> redundant, so `--keep-contracts` still emits and fires the entry check
> (`refine-runtime-check-still-fires`) and `--no-contracts` still strips it
> (`refine-no-contracts-strips-runtime-check`). The two halves stayed separable;
> only the enable knob went away.

### Interaction with Existing Type System Features

| Feature | Interaction |
|---|---|
| Borrow checker | Refinements on borrowed values are checked at borrow site; no interaction otherwise |
| Linear / affine types | A linear value with a refinement still tracks linearity; refinement discharge is independent |
| Session types | No interaction in prototype |
| HKT / GADTs | Refinements on type parameters are unsupported in prototype; raise a diagnostic |
| Sized types | Sized bounds (SZ0--SZ3) are encoding targets -- `(< i n)` for a `SizedVec n a` index naturally maps to difference logic (S2) |

---

## Architecture Overview

> **CORRECTED 2026-08-01.** The listing below is now the file layout **as
> built**, not as originally proposed. Three planned files were never created,
> each for a reason the journal records; two stdlib entries were wrong. The
> deltas are spelled out under the listing so the original intent is not lost.
> Cross-check against the nine `compiler/refine_*.c` entries in
> `src/CMakeLists.txt` (~lines 186--194).

```
src/compiler/
  elab_types.c         -- RT0: attach rt_discharged flag; call constraint collector
  elab_fns.c           -- RT1: collect :pre/:post as obligations
                          RT4: template-based predicate propagation lives HERE
  refine_collect.c     -- RT1: constraint collector -- walks elaborated forms
                          RT4: declared-result propagation, path conditions
  refine_collect.h     -- RT1
  refine_vc.c          -- RT2: normalized VC representation + builder; RT7 within-unit memo
  refine_vc.h          -- RT2: RefineVC, RefineVerdict, RefineBackend seam
  refine_smtlib.c      -- RT2: normalized-VC -> SMT-LIB2 serializer
  refine_smtlib.h      -- RT2
  refine_discharge.c   -- RT3: discharge pass -- runs the stage fall-through chain
                          RT6: error message quality / counterexample rendering
  refine_discharge.h   -- RT3
  refine_solver_s0.c   -- S0: normalize + trivial discharge
  refine_solver_euf.c  -- S1: congruence closure (union-find + EUF)
  refine_solver_arith.c-- S2: Fourier-Motzkin over exact rationals (NOT Bellman-Ford + simplex)
  refine_solver_no.c   -- S3: Nelson-Oppen combination of EUF + LRA
  refine_solver.c      -- assembles the chain; ALSO the S4 small-DNF cube expansion
                          and refine_model_search (bounded counterexample search)
  refine_solver.h      -- S0--S4 shared declarations
  diag.h               -- RT3: TUR-E0370..TUR-E0379 refinement diagnostics
  types.h              -- RT0: rt_discharged bit on the TY_CONTRACT union arm

src/runtime/experiments.c -- RT0: the `refined` row  [DELETED 2026-08-01 on graduation;
                             the name now sits in GRADUATED[] as a TUR-W0063 no-op]
src/compiler/lang_layers.c -- RT0: the `refined` LANG_LAYERS row  [DELETED 2026-08-01;
                             now in GRADUATED_LAYERS[] as a TUR-W0064 no-op]

src/wasm_glue.c        -- RT5a: no change for solver (the in-house chain compiles in directly)

stdlib/
  refine.tur           -- RT5b: nine MONOMORPHIC scalar aliases --
                          Nat, Pos, NonZero, Neg, Byte, Percent,
                          NonNegFloat, PosFloat, UnitFloat
  refined.tur          -- (not an RT phase; stdlib-refinement-collections-plan R1/R2)
                          NonEmpty + BoundedIdx refinement newtypes with total
                          accessors, discharged by Option-returning smart constructors

tests/fixtures/refine-*/ -- end-to-end fixture tests (flat, not a refine/ subdirectory)
tests/unit/refine_solver.c -- RT2/S*: normalized-VC + per-stage decision-procedure tests
tests/unit/refine_corpus.c -- the labelled SMT-LIB corpus replayer (no solver linked)
tests/refine-fuzz-src.py   -- source-level differential fuzzer
```

**Files this section used to list that do NOT exist, and why:**

| Listed | Reality |
|---|---|
| `refine_solver_sat.c` (S4) | **Never created.** S4 is not a separate stage: the small-DNF cube expansion is shared machinery in `refine_solver.c` that S1/S2/S3 all run over, because every theory stage needs conjunctions of literals to work on. No DPLL(T) engine exists or is planned. See the S4 bullet under "Deliberate deviations" in the journal. |
| `refine_propagate.{c,h}` (RT4) | **Never created.** RT4 landed as template-based propagation inside the existing `elab_fns.c` / `refine_collect.c`, with no separate module. See "RT4 predicate propagation (landed 2026-07-24)" and "RT4 branching bodies -- path splitting" in the journal. |
| `refine_libz3.{c,h}` (RT3) | **Created, then DELETED 2026-07-30 in 0.32.5**, along with the `find_package(Z3)` block, the `TUR_REFINE_Z3_ORACLE` option, and `tests/unit/refine_fuzz.c` (the VC-level differential target). The scaffold did its job and was retired on schedule. |
| `stdlib/refine-vec.tur` | **Never created.** No `SizedVec` refinement module shipped. |
| `refine.tur` holding `Bounded` | **Not shipped, and a settled non-goal.** Parameterized refinement aliases do not elaborate -- see the "Not shipped: parameterized refinement aliases" note under Phase RT5b. |
| `refine.tur` holding `NonEmpty` / `Unit` | Wrong module and wrong names. `NonEmpty` (with `BoundedIdx`) lives in **`stdlib/refined.tur`**, a separate module this plan never scoped. `Unit` shipped as `UnitFloat`, a `[0.0, 1.0]` float alias, not a unit type. |

---

## Phase RT0: Infrastructure Hooks

> **SUPERSEDED 2026-08-01 -- the gate half of this phase has been UNDONE.** RT0
> registered `refined` as an experiment plus `#lang` layer; graduation deleted
> both rows, removed `g_opt_refined` and every `if (g_opt_refined)` conditional
> shown below, and dropped the `experiment_warn_if_used("refined")` calls. The
> collector and discharge pass now run **unconditionally**. What survives from
> RT0 is the durable half: the `rt_discharged` bit on `TY_CONTRACT` and the
> pipeline hook itself. Read the code blocks below with the `if (g_opt_refined)`
> guards mentally deleted.

**Goal:** Register the gate (experiment row + lang layer), add the discharge
bit, and wire the discharge pass into the pipeline. No solver yet -- all
predicates still fall through to runtime checks.

### Changes

**`types.h`** -- add `rt_discharged` to the `TY_CONTRACT` arm:

```c
struct {
    struct Type        *base_type;
    const char         *var_name;
    const struct Form  *predicate;
    bool                rt_discharged; /* RT0: true = statically proved; elide runtime check */
} contract_;
```

**`src/runtime/experiments.c`** -- add the `refined` row (all seven fields);
declare `g_opt_refined` in `globals.h`.

**`LANG_LAYERS[]`** (lang-layers plan L4) -- add the `refined` semantic-layer
row with `experiment` pointing at the `refined` experiment.

**`elab_types.c`** -- after the existing contract elaboration block:

```c
if (g_opt_refined) {
    experiment_warn_if_used("refined");
    refine_collect_obligation(e, ct, pred_form, source_loc);
}
```

**Compilation pipeline** -- after all elaboration is complete:

```c
if (g_opt_refined)
    refine_discharge_all(compilation_unit);
```

### Acceptance Criteria

- ~~`tur experiments` lists `refined`; `--enable=refined` and
  `#lang turmeric refined` both enable it; a manifest `:experiments []`
  suppresses the user file and makes a `#lang ... refined` file a hard error.~~
- ~~With `refined` off, behavior is identical to contracts-only.~~
- ~~Existing contract tests continue to pass with the gate on and off.~~

> **The criteria above were met, then retired at graduation** -- there is no
> gate left to satisfy them. They are replaced by the graduation acceptance
> criteria, pinned by fixtures: `refine-graduated-enable-noop` (a lingering
> `--enable=refined` warns `TUR-W0063` and compiles), and
> `refine-graduated-lang-layer-noop` (a lingering `#lang turmeric refined` warns
> `TUR-W0064` and compiles). The contracts-only comparison survives as
> `refine-runtime-check-still-fires` (`--keep-contracts`) and
> `refine-no-contracts-strips-runtime-check` (`--no-contracts`), which pin that
> static discharge and the runtime check stayed separable.

---

## Phase RT1: Constraint Collector

**Goal:** Implement `refine_collect.c`. This pass walks the post-elaboration
form tree and records each `TY_CONTRACT` crossing point as a **proof
obligation**.

### Obligation Record

```c
typedef struct RefineObligation {
    const Form    *predicate;   /* the p in #refine{ x : T | p } */
    const char    *var_name;    /* the x */
    Type          *base_type;   /* the T */
    SourceLoc      loc;         /* where the crossing occurs */
    RefineEnv     *env;         /* in-scope refinements (hypotheses) at this point */
    RefineVC      *vc;          /* RT2: normalized VC, filled during discharge */
    bool           discharged;  /* set by RT3 discharge pass */
    bool           proven;      /* set by RT3: true = a backend returned RT_VALID */
    RefineModel   *counterex;   /* RT3: model if a backend returned RT_INVALID */
} RefineObligation;
```

### Crossing Points to Collect

| Situation | Obligation |
|---|---|
| Passing an `expr : T` where `T` is `#refine{ x : U \| p }` | Prove `p[expr/x]` given current env |
| Returning from a `defn` with `:post r` | Prove `:post` holds for the return value |
| Struct field write where field type is `TY_CONTRACT` | Prove field predicate holds |
| Pattern-match arm narrowing a refined type | Add the predicate to the env for that arm |

### Environment

The `RefineEnv` is a lightweight chain of `(name, predicate)` pairs pushed at
each lexical boundary. Inside `(if (> x 0) <then> <else>)`, the then-branch env
gains `(x > 0)`; the else-branch gains `(not (> x 0))`.

```c
typedef struct RefineEnvEntry {
    const char           *name;
    const Form           *predicate;   /* known-true hypothesis mentioning name */
    struct RefineEnvEntry *next;
} RefineEnvEntry;

typedef struct RefineEnv { RefineEnvEntry *head; Arena *arena; } RefineEnv;
```

### Acceptance Criteria

- `tests/unit/refine_collect_test.c` drives the collector on hand-constructed
  forms and asserts the correct obligation set.
- No obligations are collected when `refined` is off.

---

## Phase RT2: Normalized VC + Solver Seam

**Goal:** Implement `refine_vc.c` (the neutral obligation representation and the
`decide` seam) and `refine_smtlib.c` (one serializer, VC -> SMT-LIB2, used only
by the Z3 backend). The in-house stages (S0--S4) consume the normalized VC
directly and never touch SMT-LIB text.

### Normalized VC

An obligation is lowered to a backend-independent structure: a set of typed
**variables**, a set of **uninterpreted function symbols** (measures + nonlinear
terms), a list of **hypotheses** (from the env), and a single **goal**. All
`!=` are desugared to `(not (= ...))`, literals folded, and nonlinear/measure
subterms replaced by fresh uninterpreted-function applications with a side table
mapping them back to source terms (for RT6 counterexample translation).

```c
typedef enum { VS_INT, VS_REAL } VCSort;

typedef struct {
    VCVar     *vars;    size_t n_vars;    /* declared constants + sorts */
    VCUFunc   *ufuncs;  size_t n_ufuncs;  /* measures / nonlinear-as-uninterpreted */
    VCTerm   **hyps;    size_t n_hyps;    /* assumed-true facts */
    VCTerm    *goal;                      /* the thing to prove */
    UFOrigin  *origins;                   /* uninterpreted-symbol -> source-term map */
} RefineVC;
```

### The Seam

`refine_vc.h` declares `RefineVerdict`, `RefineDecision`, and the
`RefineBackend` function-pointer type (see "The Solver Seam" above). The
discharge pass (RT3) owns the ordered array of backends and the fall-through
loop; individual backends are pure `RefineVC -> RefineDecision`.

### SMT-LIB2 Serializer (Z3 backend only)

`refine_smtlib.c` walks a `RefineVC` and emits:

```
(set-logic QF_UFLIA)         ; QF_UFLRA if any VS_REAL var present
(declare-const x Int) ...    ; one per VCVar
(declare-fun len (Int) Int)  ; one per VCUFunc
(assert <hyp-1>) ...         ; hypotheses
(assert (not <goal>))        ; negate goal -- unsat => valid
(check-sat)
(get-model)                  ; reached only if sat (counterexample)
```

Term encoding is the obvious structural map (`(= a b)`, `(<= a b)`, `(+ a b)`,
`(* a const)`, `(mod a const)`, etc.). Because nonlinear/measure terms are
already uninterpreted in the VC, the serializer has no special cases for them --
they are just `declare-fun` applications.

### Acceptance Criteria

- Unit tests build normalized VCs from obligations and assert their structure.
- A serializer test compares VC -> SMT-LIB output against expected fixtures.
- Nonlinear subterms surface as uninterpreted functions in the VC and emit
  `TUR-W0373`.

---

## Phase RT3: Discharge Pass + Z3 Scaffold Backend

**Goal:** Implement `refine_discharge.c` (runs the backend fall-through chain
over all obligations) and `refine_libz3.c` (the Z3 *scaffold* backend). RT3
brings the end-to-end pipeline up with Z3 as a dev-only bootstrap so the
collector -> VC -> discharge -> codegen path is testable before any in-house
stage exists -- **without** making Z3 part of any shippable artifact.

### CMake Integration -- system Z3 only, dev opt-in, never release/WASM

> **SUPERSEDED 2026-07-30 (0.32.5) -- none of this is in `src/CMakeLists.txt`
> any more.** The `TUR_REFINE_Z3_ORACLE` option, the `find_package(Z3 ...)`
> call, the `FATAL_ERROR` guard, and the `target_link_libraries(turi PRIVATE
> z3::libz3)` line were all deleted with the scaffold. Do not re-add them, and
> do not add a release-cut `grep` for `z3` on the strength of this section --
> see Open Question 4. Kept for the reasoning about *how* to isolate a dev-only
> oracle, which is a pattern worth reusing, not as a description of the build.

```cmake
# Dev-only correctness oracle + transitional bootstrap. OFF by default.
option(TUR_REFINE_Z3_ORACLE "link a system Z3 as a dev-build refinement oracle" OFF)

if(TUR_REFINE_Z3_ORACLE)
  if(CMAKE_BUILD_TYPE STREQUAL "Release" OR EMSCRIPTEN)
    message(FATAL_ERROR "TUR_REFINE_Z3_ORACLE is a dev scaffold; not allowed in Release or WASM builds")
  endif()
  find_package(Z3 4.12 CONFIG REQUIRED)   # SYSTEM install only -- never fetched/bundled
  target_link_libraries(turi PRIVATE z3::libz3)
  target_compile_definitions(turi PRIVATE TUR_REFINE_Z3_ORACLE=1)
endif()
```

Note the deliberate departures from the original plan: **no `CPMAddPackage`, no
`FetchContent`, no auto-download, no static-linking into distributables.** The
oracle uses a Z3 the developer already has (`find_package ... REQUIRED`); if it
is absent, they simply do not get the oracle build. Release and WASM builds
*refuse* the option outright, so there is no path by which Z3 reaches a shipped
binary or the web bundle.

### Z3 Scaffold Backend

`refine_libz3.c` is compiled only under `#ifdef TUR_REFINE_Z3_ORACLE`. It
implements a `RefineBackend`: serialize the VC (RT2), submit via
`Z3_eval_smtlib2_string`, map `unsat -> RT_VALID`, `sat -> RT_INVALID` (parse
the model), `unknown/error -> RT_UNKNOWN`. One `Z3_context` per compilation unit;
`Z3_push`/`Z3_pop` isolate each query. In an oracle build it serves both roles
from "Backend strategy" above: chain tail (bootstrap) and cross-check against
each in-house stage (oracle).

### Discharge Pass

```c
void refine_discharge_all(RefineObligationVec *obs, Arena *a, DiagCtx *diag);
```

For each obligation: build the normalized VC (RT2), then run the ordered backend
chain, stopping at the first non-`Unknown` verdict:

1. `RT_VALID`: set `proven = discharged = true`; set the `TY_CONTRACT`
   `rt_discharged` bit; codegen emits no runtime check.
2. `RT_INVALID`: store the model in `counterex`; emit `TUR-E0371`; keep the
   runtime check (hard error under `--strict-refine`).
3. Chain exhausted with `RT_UNKNOWN`: emit `TUR-W0372`; keep the runtime check
   (hard error under `--strict-refine`).

The chain is assembled at build time: `[S0, S1, ...]` for a normal build, with
the Z3 scaffold appended (and cross-check enabled) only under
`TUR_REFINE_Z3_ORACLE`. At the RT3 milestone specifically, no S-stage exists
yet, so an oracle build's chain is `[z3_scaffold]` and a normal build's chain is
empty (every obligation -> runtime check). That empty-chain normal build is the
honest state of the prototype until S0 lands a day later.

### New Diagnostics

| Code | Kind | Message Template |
|---|---|---|
| `TUR-E0370` | Error | `refinement predicate is ill-typed: <reason>` |
| `TUR-E0371` | Error | `refinement predicate cannot be proved statically; counterexample: <model>` |
| `TUR-W0372` | Warning | `solver returned unknown for refinement predicate at <loc>; runtime check kept` |
| `TUR-W0373` | Warning | `non-linear predicate subterm '<subterm>' treated as uninterpreted; arithmetic reasoning incomplete` |
| `TUR-E0375` | Error | `refinement predicate mentions effects; pure predicates only` |
| `TUR-E0376` | Error | `refinement on type parameter is not supported in this prototype` |

> **Note 2026-08-01:** the six codes above are accurate as shipped (see
> `src/compiler/diag.h`), including `TUR-E0376` meaning *type parameter* -- the
> claim in Open Question 6 that `TUR-E0376` rejects refined **typeclass method**
> signatures was never true even of this plan. Four codes landed after this
> table was written and belong with it:
>
> | Code | Kind | Meaning |
> |---|---|---|
> | `TUR-E0374` | Error | instance method demands more (parameter) or delivers less (result) than its class signature |
> | `TUR-W0377` | Warning | call allowed only because the resolved instance demands less than its class |
> | `TUR-E0378` | Error | refinement written inside a `(fn ...)` type |
> | `TUR-I0379` | Info | **RETIRED** with the Z3 oracle (0.32.5); enum slot reserved, never emitted |
>
> The `diag.h` comment block still says these are "only ever emitted under the
> `refined` experiment"; as of graduation they are emitted unconditionally.
> `--strict-refine` still upgrades `TUR-E0371` and `TUR-W0372` to hard errors.

### Acceptance Criteria

End-to-end fixtures in `tests/fixtures/refine/`:

```turmeric
;; refine/proved.tur -- compiles cleanly, no runtime check emitted
(defn double-pos [x : #refine{ v : int | (> v 0) }] : #refine{ r : int | (> r 0) }
  (* x 2))

;; refine/unproved.tur -- emits TUR-E0371 with counterexample
(defn wrong [x : int] : #refine{ r : int | (> r 0) }
  x)

;; refine/float.tur -- float predicate discharged via QF_UFLRA
(defn sqrt-pos [x : #refine{ v : double | (>= v 0.0) }] : double
  (sqrt x))
```

- In an oracle build (`-DTUR_REFINE_Z3_ORACLE=ON`, system Z3 present) all three
  behave correctly, establishing the end-to-end pipeline.
- In a normal build at the RT3 milestone (no S-stage yet), the same fixtures
  compile with the obligations falling to runtime checks -- and are re-run,
  expecting static discharge, as each S-stage lands. These fixtures carry a
  `requires` marker so the suite knows which build decides them statically.

---

## In-house Solver: Staged Gradient (S0--S4)

Each stage is independently shippable, prepends itself to the discharge chain,
and returns `RT_UNKNOWN` for anything outside its competence. None of them can
regress correctness (the soundness invariant + `Unknown` exit guarantee it);
each only *moves obligations left*, off the runtime-check fallback (and, while it
still exists, off the Z3 scaffold). Build them in order, stop wherever the
coverage/effort tradeoff says stop. These stages *are* the shipped solver -- Z3
is never in the chain a user runs.

Every stage lands validated against the **Z3 scaffold oracle** (see "Backend
strategy"): in an oracle build, run the stage *and* Z3 on the same VC and assert
agreement (a stage saying `Valid` where Z3 says `Invalid` is a soundness bug ->
`TUR-I0379`, treated as `Unknown`). Fuzz against generated VCs + the labelled
`QF_UF`/`QF_IDL`/`QF_LIA`/`QF_LRA` SMT-LIB benchmark corpora. The oracle is a
development harness only; it is not present in the builds these stages ship in.

### S0 -- normalize + trivial discharge

**Effort: days.** Reflexivity (`p => p`), constant folding, syntactic
entailment, interval bookkeeping over literal bounds. No real solver. Discharges
a startling fraction of demo-grade obligations (`b != 0` after a literal guard,
`0 <= 3`, a goal syntactically present among the hypotheses). Every later stage
runs on S0's normalized output.

### S1 -- congruence closure (EUF)

**Effort: 1--2 weeks.** Union-find + congruence closure decides quantifier-free
equality with uninterpreted functions -- exactly where every measure-as-opaque-
function (`len`, `elems`) and every nonlinear-as-uninterpreted term lives.
Cleanest textbook treatment: Harrison, *Handbook of Practical Logic and
Automated Reasoning* (working OCaml, mechanically portable to C); Bradley &
Manna, *The Calculus of Computation* for exposition.

### S2 -- arithmetic, easy half first

**Effort: days (difference logic) + weeks (full LRA).** Two sub-steps:

- **S2a difference logic** (`x - y <= c`): negative-cycle detection
  (Bellman-Ford) on a constraint graph. Days of work; covers most index/bounds
  reasoning (`0 <= i`, `i < len`, `i + 1 <= n`) -- the sweet spot for array-style
  refinements and the direct target for `SizedVec` indices.
- **S2b full LRA**: the Dutertre--de Moura simplex formulation specifically,
  because it is designed to be incremental and backtrackable (needed once
  theories combine in S3). A few weeks to get pivoting + bound bookkeeping right.
- **S2c integers, the long tail**: branch-and-bound / Gomory cuts on top of LRA.
  This is the first place to feel *zero shame* returning `Unknown` with a depth
  limit -- integer completeness is genuinely hard and the runtime fallback
  absorbs it. If real completeness on the integer-linear fragment is ever wanted,
  the Omega test (Pugh) and Cooper's algorithm are the references (Omega was
  built for exactly the array-index constraints refinements produce).

### S3 -- combine EUF + LRA (Nelson-Oppen)

**Effort: 1--2 weeks once S1 + S2 exist.** The theories exchange entailed
equalities. EUF and LRA are convex, so they combine deterministically; the
wrinkle is integers (non-convex), which forces case-splitting on equalities.

### S4 -- boolean structure

**Effort: days for the naive version; do not build more.** Only needed if VCs
have nontrivial propositional shape (disjunctions, nested `ite`). Naive
case-splitting / small-DNF over the theory solver is usually enough for
explicit-annotation checking. A real CDCL SAT engine driving the theories
(true DPLL(T)) is the **months-to-years line we do not cross** -- if a VC's
boolean structure is too rich for small-DNF, return `Unknown` -> runtime check.

### Where to stop

S0--S2a alone (a couple weeks) already covers the demo-grade and array-index
majority and is small enough to live in `tur.wasm`. S0--S3 is the honest
"complete on the permitted fragment minus the integer tail" target and is the
point at which the Z3 scaffold is retired (see below). S4 and S2c are the
diminishing-returns tail -- do them only if real obligations demand it, and lean
on the **runtime fallback** for the rest without apology. (Note the fallback is
now the runtime check alone: past retirement there is no Z3 to lean on, and that
is the point -- the shipped compiler answers `Unknown` and inserts a check
rather than pulling in a solver.)

### Z3 retirement criteria

**EXECUTED 2026-07-30 (0.32.5).** Both criteria below were met, and the
scaffold is gone: `refine_libz3.c`, the `find_package(Z3)` block, the
`TUR_REFINE_Z3_ORACLE` option, the `tur_refine_fuzz` VC-level differential
target, and every `#ifdef TUR_REFINE_Z3_ORACLE`. `TUR-I0379` is retired but its
enum slot stays reserved in `diag.h`, per the convention `TUR-E0700`/`E0701`
set. Post-retirement verification: `tur_refine_corpus` replays 125 labelled
benchmarks with no solver linked -- 68 unsat proved, 56 sat correctly declined,
**0 soundness failures** -- and the full suite was unchanged by the deletion.

That is expected rather than lucky: the removed code was entirely inside
`#ifdef` branches that a default build never compiled, so the shipped binary is
functionally identical to the one before it.

*The original criteria, retained as the record of what was required:* the
scaffold is deleted once all of the following hold:

- **Bootstrap discharged:** every RT3/RT5b fixture that Z3 statically decided is
  now statically decided by the in-house chain (reached incrementally; fully by
  end of S3).
- **Oracle trust established:** the differential run over generated VCs + the
  `QF_UF`/`QF_IDL`/`QF_LIA`/`QF_LRA` SMT-LIB corpora has been clean across a
  soak window, and those corpora are checked into the repo as a standing
  regression the in-house solver runs against *without* Z3 present (labels come
  from the corpus, not a live Z3).

  **Status: MET -- the mechanism exists, is green, and the soak window closed;
  the scaffold was deleted 2026-07-30.** `tests/corpus/smtlib/` holds **125**
  labelled benchmarks (69 `unsat`, 56 `sat`) and `tur_refine_corpus` (ctest
  target, ~0.1s) replays them with no solver linked.
  The satisfiability/entailment bridge is exact -- assert everything as
  hypotheses, take `false` as the goal, so `hyps |- false` is VALID iff the
  benchmark is UNSAT -- which makes a `sat` benchmark answered VALID precisely a
  break of the one-directional invariant, checkable from the label alone.

  Evidence, all clean (corpus row is the current figure; the two Z3-dependent
  rows are the final pre-retirement measurements and cannot be re-run, since
  the harnesses they used are deleted):

  | run | size | result |
  |---|---|---|
  | committed corpus, no Z3 (current) | **125** benchmarks | 68 unsat proved, 56 sat declined, 0 soundness failures |
  | committed corpus, no Z3 (2026-07-25) | 103 benchmarks | 55 unsat proved, 47 sat declined, 0 soundness failures |
  | generated soak, Z3-labelled | 3600 benchmarks (seeds 7/8/9) | 2646 sat, **0** wrongly proved |
  | VC-level differential vs Z3 4.13 | 7000 VCs | 0 soundness bugs, 0 refutation bugs |

  The one benchmark not decided in the 125-file run is the deliberately
  out-of-fragment skip, kept so the reader's skip path stays exercised.

  The record metadata for **SMT-LIB release 2025 (non-incremental benchmarks)**
  (Zenodo `16740866`, CC-BY-4.0, 90 per-logic tarballs, 4.89 GB) is committed at
  `tests/corpus/smt-lib-benchmark-data-2025.json`, and
  `tests/corpus/import-smtlib.py` turns it into a one-command import:
  md5-verified download, deterministic seeded sample per logic, attribution
  file, and a loud report when it keeps fewer than asked.

  **The SMT-LIB benchmark library import is DONE.** The sample lives in its own
  repository -- **`github.com/turmeric-lang/smt-lib-benchmarks`** -- because the
  data is too large to belong in this tree's history, not because it was
  unobtainable. It is `smtlib-2025/`, 25 benchmarks per logic across the eight
  fragment logics (**200 files**), produced by this repo's own importer
  (`python3 tests/corpus/import-smtlib.py --sample 25`, seed 1) and carrying the
  CC-BY-4.0 `ATTRIBUTION` file the licence requires:

  ```sh
  git clone https://github.com/turmeric-lang/smt-lib-benchmarks /tmp/smtlib-bench
  TUR_CORPUS_TIMEOUT=3 ./build/tur_refine_corpus /tmp/smtlib-bench/smtlib-2025
  ```

  Measured 2026-07-26: **200 / 200 parsed, 0 skipped, 0 crashes, 0 soundness
  failures.** The reader tail that used to skip 7 is closed -- see
  [corpus-reader-tail-plan.md](history/corpus-reader-tail-plan.md), now
  archived/resolved. So the parsed-coverage bar this criterion actually gates on
  is met outright.

  The in-tree `tests/corpus/smtlib/` corpus is unaffected and remains the
  standing no-network regression: the external sample is breadth, not a
  dependency of the test path.
- **No scaffold references remain in shippable code paths** (guaranteed by
  construction, since the CMake option refuses Release/WASM, but re-verified at
  deletion).

After deletion the differential harness keeps running against the checked-in
labelled corpora -- that is the durable soundness net; Z3-the-dependency was
only ever the bootstrap that seeded it.

---

## Phase RT4: Bidirectional Predicate Propagation

> **CORRECTED 2026-08-01: there is no `refine_propagate.c`.** RT4 landed, and
> landed in full (including declared-result propagation, path splitting across
> branching bodies, `match`-arm splitting, and path conditions for call-site
> crossings), but as changes to `elab_fns.c` and `refine_collect.c` -- it never
> warranted a module of its own. The design below is otherwise accurate.

**Goal:** Infer *result* refinements from *argument* refinements for simple
arithmetic, reducing annotation burden. This is a small, template-based
convenience -- **not** the general refinement inference we explicitly ruled out.

Without it:

```turmeric
(defn inc-pos [x : #refine{ v : int | (> v 0) }] : #refine{ r : int | (> r 0) }
  (+ x 1))
```

With it, the result refinement `(> r 0)` is inferred from the argument
refinement and the body, because the solver proves `(> x 0) => (> (+ x 1) 0)`.

### Approach: template-based (Liquid Types, checking-only)

A fixed set of predicate *shapes* is tried against the return expression, and
the seam checks which are implied by the argument refinements. Templates:

1. Predicates appearing on the function's parameters.
2. A small built-in vocabulary: `(> r 0)`, `(>= r 0)`, `(< r 0)`, `(<= r 0)`,
   `(not= r 0)`, `(= r <literal>)`.

First provable template wins; if none is provable and no explicit return
refinement was written, the return carries no refinement (same as today).
Conservative (may miss some) but sound (never infers a false refinement).

### Scope

- Single-expression bodies (no branching).
- Arithmetic over direct parameters.
- <= 4 refined parameters (`TUR-W0377` beyond that).

Branching bodies (path-sensitive join at merge points) are deferred.

### Acceptance Criteria

```turmeric
(defn inc-pos [x : #refine{ v : int | (> v 0) }] : int
  (+ x 1))                       ;; inferred result (> r 0)

(defn double-nonzero [x : #refine{ v : int | (not= v 0) }] : int
  (* x 2))                       ;; inferred result (not= r 0)
```

Both compile without `TUR-E0371`, with inferred refinements shown by
`tur --print-types`.

---

## Phase RT5a: WASM Integration

**Goal:** Static refinement discharge in the web REPL. Because Z3 is scaffolding,
this phase is almost entirely a *non*-integration: the **in-house solver
(S0--S2) compiles directly into `tur.wasm`** and is the entire discharge path.
There is no `z3-solver`, no JS bridge, no lazy fetch, no `SharedArrayBuffer` /
`Atomics.wait`, no COOP/COEP requirement, and no bundle-size cost. Whatever the
in-house chain returns `Unknown` on falls to a runtime check, exactly as in a
native build.

### Architecture

```
Turmeric WASM module (C/Emscripten)
  -> refine_discharge_all()
       -> [S0..S2 backends, compiled in]   (the whole chain; no network, no bridge)
       -> RT_UNKNOWN => runtime check
```

The web build is simply a normal (non-oracle) build targeting Emscripten. The
CMake option from RT3 already *refuses* `TUR_REFINE_Z3_ORACLE` under
`EMSCRIPTEN`, so there is structurally no way for Z3 to enter the WASM artifact.

### Changes

- None specific to a solver bridge. The only work is confirming the S0--S2
  sources compile clean under Emscripten (no host-only syscalls, arena sizing
  sane for the WASM heap) and that `refine_discharge_all` is reachable from the
  existing `wasm_glue.c` entry points.

### Acceptance Criteria

- The web REPL discharges `refine/proved.tur` statically with no network
  activity whatsoever; `refine/unproved.tur` surfaces `TUR-E0371`.
- The WASM bundle size delta from enabling `refined` is just the S0--S2 object
  code (target: small, single-digit KB), with **zero** third-party solver
  payload.
- No COOP/COEP headers are required for refinement checking (they may still be
  present for unrelated Emscripten threading, but discharge does not depend on
  them).

---

## Phase RT5b: Standard Library Refinement Types

**Goal:** `stdlib/refine.tur` with common predicate-annotated aliases. Can
proceed in parallel with RT5a once RT3 lands.

> **CORRECTED 2026-08-01 -- what shipped.** `stdlib/refine.tur` holds **nine**
> monomorphic aliases, all scalar: `Nat`, `Pos`, `NonZero`, `Neg`, `Byte`,
> `Percent` (int); `NonNegFloat`, `PosFloat`, `UnitFloat` (float). Note
> `PosFloat` is `(> x 0.0)` -- *strictly* positive; the non-negative one is
> `NonNegFloat`, and the sketch below had them conflated. The base type is
> `float`, not `double`.
>
> Separately, `stdlib/refined.tur` -- **not** part of RT5b and not scoped by
> this plan -- ships the `NonEmpty` and `BoundedIdx` refinement *newtypes* with
> total accessors (`ne-head`/`ne-tail`, `vec-get-checked`/`slice-get-checked`),
> discharged by `Option`-returning smart constructors rather than by the solver.
> It comes from `stdlib-refinement-collections-plan` (R1, R2).

```turmeric
;;; Nat -- non-negative integer (>= 0)
(deftype Nat #refine{ x : int | (>= x 0) })

;;; Pos -- strictly positive integer (> 0)
(deftype Pos #refine{ x : int | (> x 0) })

;;; NonZero -- integer that is not zero
(deftype NonZero #refine{ x : int | (not= x 0) })

;;; NonNegFloat -- non-negative float (>= 0.0); the domain of sqrt
(deftype NonNegFloat #refine{ x : float | (>= x 0.0) })
```

> **Not shipped: parameterized refinement aliases.** An earlier draft of this
> phase listed
> `(deftype (Bounded lo hi) #refine{ x : int | (and (>= x lo) (<= x hi)) })`.
> That form does not elaborate -- written as `(deftype (Bounded lo hi) ...)` it
> fails with `deftype name must be a symbol`, and in the accepted shape
> `(deftype Bounded [lo hi] ...)` it hits the deliberate guard at
> `src/compiler/elab_types.c:2466-2470`: *"a refinement alias takes no type
> parameters in this prototype (the predicate cannot mention them)."* This is a
> settled non-goal, tagged `[prototype]` / "Not planned" in
> [refinement-types-guide.md](../guides/refinement-types-guide.md); the
> revisit trigger lives in
> [ecs-refinement-typed-apis-plan.md](../upcoming/v1/ecs-refinement-typed-apis-plan.md).
> `stdlib/refine.tur` shipped monomorphic aliases only -- there is no `Bounded`.

> **CORRECTED 2026-08-01: `stdlib/refine-vec.tur` was NEVER CREATED.** The
> sketch below is the design intent, retained because it is still the clearest
> statement of what a `SizedVec` index refinement would look like and why S2
> arithmetic is the direct target. It is not a description of any shipped
> module. What ships instead is `BoundedIdx` in `stdlib/refined.tur`, which
> takes a different route: the bound is a *value-level* fact the smart
> constructor `bidx-of?` checks once and returns as an `Option`, not a type the
> checker tracks. The revisit trigger for the typed version lives in
> [ecs-refinement-typed-apis-plan.md](../upcoming/v1/ecs-refinement-typed-apis-plan.md).

`stdlib/refine-vec.tur` would wire the `(< i n)` index obligation through
`SizedVec` -- the direct target of S2a difference logic:

```turmeric
;;; refine-vec/get -- index a SizedVec with a statically-bounded index
(defn refine-vec/get [v : (SizedVec n a), i : #refine{ j : int | (< j n) }] : a
  (vec-unsafe-get v i))
```

`vec-unsafe-get` would be emitted only when `(< j n)` is proved statically;
otherwise the elaborator falls back to bounds-checked `vec-get`.

### Acceptance Criteria

```turmeric
(import refine)

(defn safe-div [n : int, d : NonZero] : int
  (/ n d))                       ;; no runtime divisor check emitted

(defn main [] : void
  (println (safe-div 10 2)))
```

`d : NonZero` gives `(not= d 0)`; S0 proves the obligation with no Z3 call at all.

---

## Phase RT6: Error Message Quality

**Goal:** Make refinement errors actionable by translating the counterexample
`RefineModel` (in normalized-VC variable terms) back into source syntax.

```
error[TUR-E0371]: refinement predicate cannot be proved statically
  --> src/myfile.tur:14:3
   |
14 |   (defn inc [x : int] : #refine{ r : int | (> r 0) }
   |                         ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
   |
   = counterexample: x = 0, so (+ x 1) = 1 is fine, but x may be <= -1,
     giving (+ x 1) <= 0
   note: the predicate (> r 0) is not always satisfied for arbitrary int x
   hint: constrain the parameter, e.g. x : #refine{ v : int | (>= v 0) }
```

The hint is derived by a second seam query: which additional predicate on the
input would make the goal valid (`(=> <candidate> <goal>)`). The
uninterpreted-symbol origin map (RT2) lets measure/nonlinear terms print as
their source expressions rather than `uf_3(...)`.

### Acceptance Criteria

- `tests/fixtures/refine/error-messages/` holds snapshots for 5 representative
  failing obligations, diffed by `tur run test` (session-types snapshot
  mechanism).

---

## Phase RT7: Incremental Discharge Caching (Follow-up)

> **CORRECTED 2026-08-01: only the within-unit half of RT7 shipped, and the
> persistent cache below is DELIBERATELY NOT BUILT.** There is no
> `.tur-cache/refine.db`. The within-unit memo lives in `refine_vc.c` /
> `refine_discharge.c`. The reasoning for stopping there -- a measurement of
> where discharge time actually goes, and why a VC hash alone is not a sound
> cache key -- is in the journal under "RT7 -- measured first, then half-built
> on purpose". The acceptance criteria below were never exercised.

**Goal:** Skip re-deciding obligations on unchanged files. Each obligation is
keyed by a hash of its **normalized VC** (captures predicate + hypotheses fully)
plus the compiler version. Results cache in `.tur-cache/refine.db`; a matching
hash skips the whole chain. Invalidation is per-file (conservative, safe). The
cache is always safe to delete.

### Acceptance Criteria

*(Not met -- see the correction above. Retained as the spec a future persistent
cache would have to satisfy.)*

- A second build with no source changes runs zero backend decisions
  (`TUR_REFINE_STATS=1`).
- Editing a file evicts exactly that file's obligations.
- Deleting the cache falls back to full discharge transparently.

---

## Implementation Order and Dependencies

> **CORRECTED 2026-08-01 -- this is the plan as sequenced, and it held, with two
> named substitutions.** The order below was followed and every phase landed;
> the Z3 scaffold was retired on schedule at end-of-S3 (2026-07-30, 0.32.5) and
> `refined` graduated (2026-08-01). Substitutions: **S2b simplex was never
> built** -- S2 is Fourier-Motzkin over exact rationals, which subsumes S2a and
> met the coverage target in a few hundred lines; and **S4 is not a separate
> node** -- the small-DNF expansion is shared machinery every theory stage runs
> over. See "Deliberate deviations from the plan as written" in the journal.

```
RT0  (experiment row + lang layer + discharge bit + pipeline hook)
 |
RT1  (constraint collector)      RT2  (normalized VC + solver seam + SMT-LIB serializer)
 |                                |
 +--------------------------------+
 |
RT3  (discharge pass + Z3 SCAFFOLD)  <-- pipeline testable end-to-end (dev/oracle build)
 |
 +-- S0 -> S1 -> S2a -> S2b -> S3 ==[Z3 SCAFFOLD RETIRED]==> (S2c / S4 tail)
 |        (in-house stages prepend to the chain; each moves obligations off
 |         the scaffold/runtime-fallback; by end of S3 Z3 is deleted)
 |
RT4  (predicate propagation)     RT5a (WASM: in-house in-bundle; no Z3, no bridge)
 |                                |
 |                               RT5b (stdlib types)
 +----------------+---------------+
                  |
                 RT6 (error message quality)
                  |
                 RT7 (incremental caching; follow-up)
```

RT3 stands the pipeline up with the Z3 *scaffold* (dev/oracle builds only);
normal builds discharge only what the in-house chain can. S0--S4 grow behind the
seam on their own schedule, each independently landable, each moving obligations
off the scaffold/runtime-fallback onto a faster dependency-free path. The Z3
scaffold is deleted once S0--S3 meet the retirement criteria. RT5a/RT5b
parallelize after RT3. S2c, S4, and RT7 are deferrable indefinitely.

---

## Effort Estimates

> **These are the PRE-IMPLEMENTATION estimates, kept as the record of what was
> predicted. They are not a work plan and were never revised against actuals.**
> Two rows describe work that did not happen as written: **S2b (simplex,
> 2--4 weeks)** was never built -- S2 shipped as Fourier-Motzkin instead -- and
> **RT5b's `refine-vec.tur`** was never created. Everything else landed.

| Phase | Estimated Effort | Notes |
|---|---|---|
| RT0 | 1 day | Experiment row, lang layer, bit field, pipeline hook |
| RT1 | 2 days | Collector; env chain; crossing-point detection |
| RT2 | 3 days | Normalized VC + seam; SMT-LIB serializer; unit tests |
| RT3 | 2 days | Discharge chain; Z3 scaffold backend (system Z3, dev-only); diagnostics |
| S0 | 2--3 days | Normalize + trivial discharge |
| S1 | 1--2 weeks | Congruence closure (EUF) |
| S2a | 3--5 days | Difference logic (Bellman-Ford) |
| S2b | 2--4 weeks | Simplex LRA (Dutertre--de Moura) |
| S3 | 1--2 weeks | Nelson-Oppen combine EUF + LRA |
| S2c | (tail) | Integer branch-and-bound; depth-limited Unknown |
| S4 | (tail) | Small-DNF boolean structure; no DPLL(T) |
| RT4 | 3 days | Template propagation |
| RT5a | 0.5 day | WASM: confirm S0--S2 compile under Emscripten; no bridge to build |
| RT5b | 1.5 days | `refine.tur`; `refine-vec.tur`; integration tests |
| RT6 | 2 days | Counterexample translation; hint generation |
| RT7 | 2 days | Incremental caching (follow-up) |
| Z3 removal | 0.5 day | Delete scaffold backend, `find_package`, CMake option (post-S3) |

RT0--RT3 (~8 days) stands up the end-to-end pipeline validated against the Z3
scaffold. The in-house gradient (S0--S3) is where the multi-week investment
lives *and* is what the shipped product runs on -- it is not optional, because
Z3 leaves. What *is* optional to correctness is the S2c/S4 tail: past those, an
undecided obligation is answered `Unknown` and gets a runtime check, which is
always sound.

---

## Open Questions

1. ~~**Z3 scaffold version.**~~ **MOOT -- resolved by retirement (2026-07-30,
   0.32.5).** There is no scaffold and no version question: `refine_libz3.c`,
   the `find_package(Z3 4.12 CONFIG REQUIRED)` block, and the
   `TUR_REFINE_Z3_ORACLE` option are all deleted. Nothing in the tree links or
   looks for Z3.

   *For the record, the answer while the scaffold existed:* the oracle used
   whatever system Z3 the developer had, requiring >= 4.12; an older one made
   the option unavailable rather than silently degrading, and there was never a
   version to *pin in a release*, because no release ever shipped Z3. It was
   last exercised against 4.13 and re-checked against 4.15.4 before deletion.

2. ~~**Retirement timing vs. coverage confidence.**~~ **ANSWERED: improve the
   corpus before retiring Z3.** The scaffold is not deleted on the bootstrap
   criterion alone. Deleting while the corpus still decides a small fraction of
   what it is handed would remove the live oracle before the thing meant to
   replace it has been shown to carry the load -- and the live oracle is
   precisely what would catch that mistake.

   What "improve" concretely means, in order of measured value against the
   200-benchmark external sample:

   | cause | was | now | status |
   |---|---|---|---|
   | `ite` / `xor` unsupported | 57 | 0 | **done** -- boolean `ite` is propositional structure; arithmetic `ite` lifts to a fresh variable with a top-level definition; `xor` expands directly |
   | `define-fun` unsupported | 16 | 0 | **done** -- macro expansion at read time, reusing the `let` binding stack so scoping and capture-avoidance come for free |
   | `let` / term nesting past the cap | 7 | 2 | **done** -- caps raised; both are C-stack recursion in `tr_term`, and no child crashed at the new depth |
   | `ite` branches disagreeing on sort | -- | 4 | **mostly done** -- Int/Real mixing is legal SMT-LIB, so the literal side is coerced as `/` does; the remaining four mix under a nested `ite` |

   Measured against the 200-benchmark external sample, over four rounds:

   | round | skipped | decided | soundness failures |
   |---|---|---|---|
   | before | 80 | 91 | **1** |
   | after `ite`/`xor` | 43 | 128 | 0 |
   | after sort coercion + caps | 22 | 139 | 0 |
   | after `define-fun` | **7** | **142** | 0 |

   **The bottleneck has moved from the reader to the solver.** 193 of 200
   benchmarks now parse; of the 189 carrying a `:status`, 142 are decided and
   **40 exceed the time budget**. Further coverage is no longer a reading
   problem -- it needs the chain to decide competition-grade problems faster, or
   a larger budget, and neither is free. That is the honest state to take into
   the retirement decision: the corpus now exercises the solver rather than the
   parser.

   **Measured, and NOT acted on: the compiler has no wall-clock bound on the
   solver, and does not need one.** Reading that 40 external benchmarks exceed a
   3s budget invites the conclusion that a user's refinement could hang a build.
   It cannot, and the reason is that the cost tracks input SIZE rather than
   being a blowup a small predicate can trigger:

   - over-budget benchmarks have a median size of 482KB against 28.6KB for
     decided ones, and the SMALLEST that times out is 63KB;
   - the structural bounds are real and they bite -- `REFINE_MAX_CUBES`,
     `REFINE_MAX_CUBE_LITS`, `REFINE_MAX_LA_CONSTR`, `REFINE_MAX_LA_VARS`, and
     `NO_MAX_ROUNDS` each degrade to `Unknown`, which is always safe;
   - adversarial Turmeric programs stay fast: 400 disequalities in one predicate
     (cube-expansion territory, where each disequality doubles the DNF) checks
     in 196ms, and a 120-deep `let` chain feeding a refined callee in 684ms.

   A wall-clock bound would therefore buy nothing a user can reach, while
   introducing exactly the non-determinism a compiler should not have -- the
   same program failing to prove on a loaded machine. Pinned by
   `tests/fixtures/refine-cube-expansion-bounded`, which uses the suite's own
   per-fixture timeout as the guard rather than asserting a duration.

   The bar for deletion is that the corpus decides a large majority of what it
   is handed AND has been clean across a soak window. "Decides" is the operative
   word: a benchmark that is skipped or times out contributes nothing to trust,
   which is why the runner counts and prints both rather than folding them into
   a pass.

   **CLOSED 2026-07-30 (0.32.5): the bar was met and Z3 was retired.** The
   in-tree corpus grew to 125 and the reader tail closed (200/200 external
   benchmarks parse, 0 skipped); the final input was an oracle build against
   system Z3 4.15.4 re-checking every VC the tur-ecs corpus generates, with
   verdicts identical to the in-house chain and zero `TUR-I0379`. Nothing above
   is a live prescription.

3. **In-house solver arena discipline.** S1--S3 allocate union-find nodes,
   simplex tableaux, and equality-propagation queues per obligation. Decide
   whether each obligation gets a fresh arena (simple, leak-clean under the
   compiler's ASan/LSan policy) or the solver state resets in place per query.
   Fresh-arena-per-obligation is the default unless profiling says otherwise.

4. ~~**Oracle-build isolation.**~~ **MOOT -- resolved by retirement (2026-07-30,
   0.32.5).** The isolation problem is gone with the thing being isolated.

   **Do not add the `grep` for `z3` to the release-cut checklist.** That
   prescription was explicitly scoped "until the scaffold is deleted," and the
   scaffold is deleted -- there is no dual-run mode, no `TUR_REFINE_Z3_ORACLE`
   option, and no `#ifdef` for a release cut to verify around. Reconstructing
   the check from this paragraph would be a release gate for a dependency that
   no longer exists.

   *For the record:* running both the in-house stage and the Z3 scaffold on
   every obligation was confined to `TUR_REFINE_Z3_ORACLE` dev builds, and a
   CMake `FATAL_ERROR` guard forbade the option under Release/WASM. Because
   every removed line lived inside an `#ifdef` a default build never compiled,
   the shipped binary was functionally identical before and after deletion.

5. **`:post` with mutable references.** Allow `:post` on functions taking
   `&mut T`, but reject any predicate whose free variables include an `&mut`
   parameter name (`TUR-E0378`). Free-variable check on the predicate `Form*`;
   no alias analysis.

6. ~~**Interaction with typeclasses.** Reject refined types on typeclass method
   signatures in the prototype (`TUR-E0376`). Per-instance discharge
   deferred.~~ **ANSWERED, and the opposite of what this said. Typeclasses are
   fully supported.** Nothing is rejected; `TUR-E0376` was reassigned to a
   different meaning entirely. This was the stalest paragraph in the file --
   the journal above records four separate slices that built the feature out.
   Current state:

   | Aspect | State |
   |---|---|
   | Refined types on `defclass` method signatures | **Supported**, parameters and results alike. `TypeClassMethod` carries `param_refine_preds` and `return_refine_pred`/`_var`. |
   | Refined types on `definstance` method signatures | **Supported.** An impl may restate a refinement; an impl that writes a plain return type **inherits** the class's promise rather than discarding it. |
   | Per-instance discharge | **Landed, not deferred.** A statically-resolved dispatch is checked like any other call -- the receiver occupies slot 0, so parameter and argument slots line up with no special-casing, and dotted (`.meth`) and bare (`meth`) spellings share the resolution point. |
   | Class/instance **parameter** variance | **`TUR-E0374`.** Obligation `class_pred(p) \|- instance_pred(p)`: an instance may demand LESS (accept more), never more. |
   | Class/instance **result** variance | **`TUR-E0374`, running the OPPOSITE direction.** Obligation `instance_pred(r) \|- class_pred(r)`: an instance may deliver MORE, never less. Distinct message from the parameter case. |
   | Result propagation across a dispatch | **Landed.** A class's result refinement propagates to the caller so the caller's own obligation can discharge from it. Sound because an unproved variance obligation causes the class predicate to be checked alongside the instance's own; publication is gated on `rt_contracts_emitted()`. |
   | Congruence over method calls | **Landed** -- the "third congruence door." |
   | Dynamic dispatch | Crosses into the **class** signature. The remaining gap is a design question (is a class parameter refinement a caller contract, or only an upper bound on instance demands?), not missing machinery -- and it is not a soundness hole, since the method's own entry check is retained. Measured demand is zero. See "Next slice" in the journal. |

   **`TUR-E0376` now means something else:** it is
   `TUR_E0376_REFINE_TYPE_PARAM`, "refinement on a *type parameter*
   (unsupported)" -- a different restriction with no bearing on typeclass
   methods. Do not cite this paragraph's original code assignment.

   Two adjacent diagnostics that did not exist when this question was written:
   `TUR-E0375` (refinement predicate mentions effects) and `TUR-E0378`
   (refinement written inside a `(fn ...)` type).

---

## References

- **Simplify: A Theorem Prover for Program Checking** -- Detlefs, Nelson, Saxe.
  The blueprint for the in-house architecture (congruence closure + simplex +
  Nelson-Oppen + matching), from the pre-Z3 ESC/Java era. Read this before
  writing a line of S0--S4.
- **Handbook of Practical Logic and Automated Reasoning** -- Harrison. Working
  code for congruence closure (S1) and the arithmetic decision procedures (S2).
- **The Calculus of Computation** -- Bradley & Manna. Cleanest exposition of
  EUF, LRA, and Nelson-Oppen combination (S1--S3).
- **A Fast Linear-Arithmetic Solver for DPLL(T)** -- Dutertre & de Moura. The
  incremental, backtrackable simplex formulation used in S2b.
- **Simplification by Cooperating Decision Procedures** -- Nelson & Oppen. The
  theory-combination method used in S3.
- **The Omega Test** -- Pugh; **Cooper's algorithm** -- for the S2c integer tail
  if real completeness is ever wanted.
- **Liquid Types** -- Rondon, Kawaguchi, Jhala (PLDI 2008). The template-based
  *checking* convenience in RT4 (we deliberately skip their *inference* layer).
- **Turmeric Contract Types** -- `docs/guides/contract-types-guide.md`
- **Turmeric Sized Types** -- `docs/guides/sized-types-guide.md`
- **Lang layers** -- `docs/archive/lang-layers-plan.md` (the `refined` semantic
  layer, phase L4 -- since removed; see the graduation note at the top).
- **Experiment mechanism** -- `docs/archive/history/experimental-flag-mechanism-plan.md`.

### Turmeric plans that depend on or extend this one

- **Graduating `refined`** --
  [refined-graduation-plan.md](refined-graduation-plan.md). EXECUTED
  2026-08-01, targeting v0.33.0. The record of the four preconditions, the
  dogfooding cost measurement, and the exact mechanical checklist that removed
  the gate.
- **Boolean-sorted measures (`RM-B`)** --
  [refine-predicate-measures-plan.md](refine-predicate-measures-plan.md).
  LANDED (RM-B0..RM-B3). Lifts the hard-coded `VS_INT` measure result sort in
  `refine_collect.c` so a `bool`-returning function can be a predicate atom.
  RM-B0 also uncovered a float measure **mis-sort**, a soundness bug, written up
  in
  [docs/archive/history/refine-float-measure-missort.md](history/refine-float-measure-missort.md).
  Backing fixtures: `refine-bool-measure`, `refine-float-measure`,
  `refine-measure-euf`.
- **Refinements over mutable state (`RM-S`)** --
  [refine-stateful-measures-plan.md](refine-stateful-measures-plan.md). RM-S0
  (dogfooding) done 2026-07-26 recommending Candidate B; the A-vs-B decision is
  awaiting sign-off, and RM-S1/RM-S2 do not start without it. Addresses the
  direct consequence of this plan's purity requirement: a measure must be
  provably pure to be congruent, which makes every predicate about a mutable
  world unprovable. Backing fixtures: the `refine-stateful-*` set
  (`-frozen-macro`, `-guard-discharges`, `-nonfinal-statement`,
  `-nonstrict-warns`, `-resizable-bounds`).
- **Refinement-typed ECS APIs** --
  [ecs-refinement-typed-apis-plan.md](../upcoming/v1/ecs-refinement-typed-apis-plan.md). The
  downstream consumer; carries the revisit triggers for two settled non-goals
  here (parameterized refinement aliases, and the typed `SizedVec` index bound
  that `refine-vec.tur` would have provided).
- **Refinement collections in stdlib** -- `stdlib-refinement-collections-plan`
  (R1, R2), which produced `stdlib/refined.tur` (`NonEmpty`, `BoundedIdx`).
  Distinct from RT5b's `stdlib/refine.tur` scalar aliases.
