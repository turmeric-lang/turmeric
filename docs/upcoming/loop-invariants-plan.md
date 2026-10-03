# Plan: Loop Invariants for Refinement Types (`:invariant`)

> **Status:** LI0--LI5 **landed 2026-09-30** behind `--enable=loop-invariants`
> (prototype; `introduced` 0.57.0, `expires_at` 0.58.0 -- advisory). Moved out
> of `hold/` when executed. What shipped, and where it departs from the
> elaboration below, is recorded in [Delivered](#delivered-2026-09-30) at the
> end; the rest of this file is the plan as elaborated.
> **Last Updated:** 2026-09-30
> **Type:** Compiler / Refinement types
> **Depends on:** [refinement-types-plan.md](../archive/refinement-types-plan.md)
> (RT0--RT7 + S0--S4, all landed). The `refined` graduation this plan used to
> wait on **happened 2026-08-01** (`bb7cbef61`, shipped v0.33.0); static
> discharge is unconditional and the old experiment clock is gone.

## Goal

Let a `while` loop carry a refinement across its iterations, so that a value
built by a loop can satisfy a refined return type.

Today it cannot. A loop accumulator is `Unknown` no matter what the loop does:

```turmeric
(defn count-up [n : int] : #refine{ r : int | (>= r 0) }
  (let [^mut acc 0
        ^mut i   0]
    (while (< i n)
      (set! acc (+ acc 1))
      (set! i   (+ i 1)))
    acc))
```

The runtime check stays and the program is correct; what is lost is the proof.

---

## The decision that is already made

**Checking, not inference.** This is not an open question and was not reopened
during elaboration -- it is the founding decision of the refinement design
(see "Why checking, not inference" in the parent plan), and it is what makes
the whole feature shippable without a heavyweight solver.

So this plan is about an invariant the USER WRITES:

```turmeric
(while (< i n) :invariant (and (>= acc 0) (>= i 0))
  (set! acc (+ acc 1))
  (set! i   (+ i 1)))
```

Inferring the invariant is out of scope permanently, not deferred. A written
invariant is checking, is the same shape as the `:pre` / `:post` annotations
that already ship, and is a bounded feature rather than a multi-week analysis.

A `while` with an invariant `p` generates the standard Hoare-rule obligations:

| # | Obligation | Meaning |
|---|---|---|
| 1 | `env \|- p` | **Initiation:** `p` holds on entry, before the first test |
| 2 | `env, p, c \|- p'` | **Preservation:** the body re-establishes `p` (`p'` is `p` after the body's assignments) |
| 3 | -- | **Use:** after the loop, `p AND (not c)` is available to what follows |

Obligation 3 is what makes the feature worth anything: without it the loop
proves its own invariant and then nothing downstream can use it.

---

## Demand status -- the trigger has partially fired

The 2026-07-25 version of this file recorded zero measured demand. That is no
longer quite true:

- [ecs-refinement-typed-apis-plan.md](v1/ecs-refinement-typed-apis-plan.md)
  (on the v1 track) names user-written `while` invariants as its prerequisite
  **C3** and states "this plan is that signal". Its RE2 phase (bounds-checked
  slot access on sized worlds) is the concrete consumer: `for-each` lowers to
  a `while` over slot indices, and probe 6 there measured the crossing as
  `0 proven, 1 unknown`.
- **But** a later probe (2026-07-26, recorded in RE2) found the tail-recursion
  shape discharges the same bounds TODAY with no new syntax: the upper bound
  rides the guard as a path condition and the lower bound rides a refined
  parameter inductively. So C3 gates only the `while`/`for-each` *lowering*,
  and RE2 itself "does not start without a profile".

Net: demand exists on the v1 track but is not blocking it. This plan is
elaborated so it can start the moment RE2's profile (or any of the triggers
below) says go; it should not start before that.

### The trigger (unchanged in substance)

Start when any one of:

- RE2's profile shows the per-access bounds re-check is a real cost and the
  `while` lowering (not the recursion shape) is where the call sites live.
- A bounded-index type in `stdlib/refine.tur` gets used to walk a vector with
  a `while`.
- Two or more distinct callers ask for it, or a spice hits it in real code.

---

## Settled design decisions

Elaboration settled the six open questions of the previous version. Each
answer below is grounded in the current source, not sketched.

### 1. Syntax: `:invariant` after the condition -- and it is NOT reader work

`while` today has no keyword position: `elab_while`
(`src/compiler/elab_forms.c:3182-3214`) elaborates item 1 as the condition and
everything from item 2 on as body forms. A stray `:invariant` keyword would
fail elaboration rather than be silently absorbed -- so the syntax is
backward-compatible and purely an `elab_while` change, mirroring the
`:pre`/`:post` scan loop that `defn` already carries
(`src/compiler/elab_fns.c:6051-6071`). No reader change of any kind.

The scan accepts `:invariant <pred>` immediately after the condition only --
not interleaved with body forms -- and at most once. A second `:invariant` is
a plain diagnostic.

### 2. Storage: a third field on the `while_` payload

`EX_WHILE`'s payload is `struct { Expr *cond; Expr *body; }`
(`src/compiler/expr.h:1119`). All ~30 visitors (`elab_core.c`, `emit_*.c`,
`mono_specs.c`, `span_audit.c`, turi's `DK_WHILE` continuation at
`src/turi/eval.c:6693`) touch only `cond` and `body`, so adding
`const Form *invariant;` (the raw predicate form, kept as a `Form` exactly as
`TY_CONTRACT` keeps its predicate) is safe with zero visitor churn. Codegen
(`emit_stmt.c:5-19`) emits nothing for it; the runtime checks of LI1 are
injected as ordinary body/preamble forms during elaboration, not by the
emitter.

### 3. Which loops: `while` only, and the alternatives genuinely do not apply

- `for` is not a loop: it is the monadic-comprehension macro
  (`stdlib/macros.tur:64-104`) expanding to `.bind`/`.pure` closures.
- `loop`/`continue` exist only as the `defprotocol` session-type DSL
  (`src/compiler/elab_global.c:254-333`), not control flow.
- Nothing in the language desugars to `while`; the idiomatic functional loop
  is tail recursion, which path conditions already serve (see the RE2 probe).

So "`while` only" is not a first cut that will grow -- it is the complete
surface. A macro that *expands* to a `while` (the ECS `for-each` lowering)
gets the feature for free by emitting `:invariant` in its expansion.

### 4. Modelling the body's assignments: reuse `rt_collect_set_targets`

Obligation 2 needs `p'` = `p` after the body. The machinery exists:

- `rt_collect_set_targets` (`src/compiler/elab_fns.c:975-1021`, built for
  WF3) walks a form and returns the set of `set!`-assigned names, declining
  (return `false`) on place-expression targets (`(set! (.f s) v)`), an
  assignment symbol in non-head position, depth overflow, or more than 16
  targets. That decline list is exactly this plan's conservative posture.
- The substitution itself composes sequentially: walk the body's statements
  in order; for `(set! x rhs)`, rewrite `x -> rhs'` where `rhs'` is `rhs`
  with all *earlier* substitutions applied. The rewriting reuses
  `rt_rename_free`'s traversal shape (`elab_fns.c:2005-2037`), and the
  obligation carries the result in the existing `RefineObligation.subst` /
  `RefineSubst` fields (`src/compiler/refine_collect.h:140-150`) -- the same
  carrier call-site sibling-parameter substitution already uses.

**Decline list for the first cut** (each falls back to "invariant not proven,
runtime check kept" -- never a wrong proof):

- any `set!` through a place expression or `(@ ...)` deref (per
  `rt_collect_set_targets`);
- a `set!` to a dynamic var (`elab_forms.c:3081-3120` is a fourth `set!`
  path the scanners do not model);
- a body that borrows an assigned local (`rt_form_borrows_name`,
  `elab_fns.c:1046-1062` -- the one channel through which a callee writes a
  caller slot);
- control flow inside the body other than straight-line statements: an `if`
  whose branches assign differently is deferred to LI3+ (path-split the
  preservation obligation the same way the branching-`let`-value split works
  at `elab_fns.c:2136-2162`); first cut declines;
- nested `while` (inner loop havocs its own targets; declining is simplest
  and costs precision only);
- early exit anywhere in the body -- see next item.

### 5. Early exit: the language has no `break`, but `return` / `?` are real

There is no `break` or `continue` form (checked: neither is interned,
dispatched, or reserved; the strings appear only in the C-keyword mangler
avoid-list and the turi debugger). The early exits that DO exist are
`(return v)` (`EX_RETURN`, dispatched at `elab_call.c:2204`), the `?`
Result-propagation form, and `panic`.

A `return`/`?` inside the body leaves the loop without `(not c)` holding, so
obligation 3's post-condition is wrong for that path. First cut: a Form-level
scan for those heads (mirroring `rt_form_mentions_set`, `elab_fns.c:886-909`,
including its macro-expansion lateral hop) and **decline the post-loop fact**
when found. Obligations 1 and 2 remain sound in their presence (a body that
exits early still may not break the invariant before exiting -- preservation
over the straight-line prefix is not checked in the first cut, so decline
those too: any early-exit head declines the whole feature for that loop).
`panic` diverges rather than exits, so it alone does not decline.

### 6. `--strict-refine`: a loop with no `:invariant` stays silent, for free

The strict-mode pattern is unambiguous in `refine_discharge.c` (three sites:
`:494`, `:601`, `:617`): strict mode only raises the severity of an existing
report, never creates a report where there was none. A `while` without
`:invariant` generates no obligation, hence no report, hence no strict-mode
error -- with zero code written to ensure it. An unproven written invariant
under strict mode is an error like any other W-to-E promotion.

### 7. Gating: `--enable=loop-invariants`, annotation always parses

Per the CLAUDE.md rule, in-flight semantics ship behind `--enable=<name>`.
`refined` has graduated and its row is gone, so this needs a fresh row --
`loop-invariants` -- modelled on the live rows (`write-frames`,
`checked-reads` in `src/runtime/experiments.c`; all seven descriptor
fields, `g_opt_loop_invariants`, `expires_at` one minor line out per current
convention, i.e. set `introduced`/`expires_at` when work starts). Follow the
`#writes` precedent exactly: **the annotation always parses; the gate
withholds the acting** (obligations, runtime-check injection, post-loop
fact). `plan_path` points at this file. `experiment_warn_if_used` fires from
the `elab_while` scan when an `:invariant` is actually written.

### 8. Diagnostics: no new codes -- report through `TUR-E0371` / `TUR-W0372`

`:pre` and `:post` obligations do not mint codes; they flow through the two
verdict-carrying codes with a `what` string ("the postcondition of 'f'",
`elab_fns.c:6706`). Invariant obligations take the identical route: a refuted
or closed-false invariant is `TUR-E0371` and an undecided one is `TUR-W0372`,
with `what` = "the invariant of the while loop in 'f' (entry)" /
"... (preservation)". This keeps `tur explain` accurate (both codes' texts
already describe exactly this situation), gets `--strict-refine` promotion
for free (same discharge path), and leaves the freshly freed `0383+` band to
the sibling reflected-measures plan.

One measured caveat for the wording: the counterexample search is budgeted
(`MODEL_MAX_VARS` is 8 and `MODEL_MAX_EVALS` bounds `n_cand ** n_vars` at
131072, `refine_solver.h`; until 2026-09-05 the width cap was 3), so a wide
multi-conjunct invariant under a body substitution can still exceed the
search's budget and land on `TUR-W0372` without a model. The remedy is in LI4: split the
invariant into conjuncts and discharge each separately, so the message can
say *which* iteration-carried fact broke -- conjunct-level reporting matters
more here than counterexample translation.

---

## Runtime semantics: the invariant is a contract first

Consistent with the whole contract/refinement design ("fails toward a runtime
check", never silently unenforced -- the reasoning that produced
`TUR-W0380`), a written `:invariant p` is dynamically enforced from day one:

- `(tur-contract-check p "Loop invariant failed on entry")` immediately
  before the loop;
- the same check as the last statement of the body (re-establishment).

Static discharge then *elides* these exactly as a proven `:pre`/`:post`
elides its check (`elab_fns.c:6741-6790` is the template): obligation 1
proven elides the entry check; obligation 2 proven elides the body check.
This gives the feature value even on programs the solver declines, keeps the
turi interpreter path correct with no interpreter change (turi walks the
elaborated `Expr`, and the checks are ordinary injected forms), and makes
"the annotation quietly does nothing" impossible. The predicate must pass the
same purity gate as any contract predicate (`TUR-E0375` on an effectful
invariant) and must elaborate to `:bool`.

---

## Phases

### LI0 -- Gate, syntax, storage

**Goal:** `:invariant` parses, is stored, is validated, and does nothing else.

- `EXPERIMENTS[]` row `loop-invariants` (all seven fields), `g_opt_loop_invariants`
  in `globals.{h,c}`, `experiment_warn_if_used("loop-invariants")` from the
  scan. Annotation parses with the gate off; acting is withheld.
- `elab_while` keyword scan after the condition: at most one
  `:invariant <pred>`; the predicate form is stored on the new
  `while_.invariant` field. Duplicate/misplaced `:invariant` is a plain
  diagnostic; `:invariant` with no following form likewise.
- Predicate validation: elaborate the predicate in the loop's scope, require
  `:bool` (reusing the condition's own check shape at `elab_forms.c:3188`),
  require purity (the `TUR-E0375` path contract predicates already take).
  The validated form is kept as a `Form` for the refinement machinery; the
  elaborated `Expr` is discarded except for the LI1 checks.

**Acceptance:** fixture `refine-loop-invariant-parses` (gate on, invariant
written, program compiles and runs unchanged); `errors/loop-invariant-effectful`
(`TUR-E0375`); `errors/loop-invariant-not-bool`; gate-off fixture pinning
that the annotation parses and warns nothing.

### LI1 -- Dynamic semantics (runtime checks)

**Goal:** an `:invariant` is enforced at runtime, before any solver work.

- Inject the entry check before the loop and the re-establishment check at
  the end of the body, under the gate, during `elab_while` -- injected as
  source forms so compiled and interpreted paths agree for free.
- Panic message names the loop's source location and says entry vs.
  re-establishment.

**Acceptance:** fixture where a deliberately false invariant panics on entry;
one where it panics after the first iteration; one where a true invariant
runs clean under `--interpret` and compiled alike (the turi harness picks
this up with no interpreter change).

### LI2 -- Static discharge: initiation and preservation

**Goal:** obligations 1 and 2 are collected, discharged, and elide the LI1
checks when proven. This is the core of the plan.

- **Obligation 1 (initiation):** goal `p`, env from `rt_build_env`
  (`elab_fns.c:677-701`: parameters' refinements + `:pre`), collected at the
  loop site via `refine_collect_obligation`. `runtime_guarded = false` -- the
  invariant is the user's own claim (same "who owes the proof" logic as a
  return refinement), and the LI1 check is the fallback.
- **Obligation 2 (preservation):** hypotheses `p` and `c`; goal `p` under the
  body's composed `set!` substitution, carried in `RefineObligation.subst`.
  Assignment modelling and the decline list exactly as settled above
  (`rt_collect_set_targets` + sequential composition + borrow/dynvar/
  early-exit/nested-loop declines). A shadowing hazard does not arise here
  the way it does in the `let` splitter -- the substitution rewrites the
  *predicate*, not the env -- but the fresh-name idiom (`"%s~%u"`,
  `elab_fns.c:2113`) is available if conjunct splitting in LI4 needs it.
- **Elision:** obligation 1 proven drops the entry check; obligation 2 proven
  drops the body check. Either unproven keeps its check and reports
  `TUR-W0372` (a warning, since the runtime check backstops it).
- New `RefineStats` counters (`refine_discharge.h:30-41` precedent:
  `proven_by_path`) so `TUR_REFINE_STATS=1` shows invariant obligations
  separately.

**Acceptance:** `count-up` from the header still reports its *return*
obligation Unknown (LI3 owns that) but shows `invariant: 2 proven` under
stats; entry-check and body-check elision visible in emitted C (fixture
pins the absence of `tur_contract_check` in the loop body); a false
invariant at a closed initiation site is `TUR-E0371` with a model
(zero-variable evaluation, the `(safe-div 10 0)` shape); decline cases
(field write in body, `return` in body, borrowed accumulator) each stay
`TUR-W0372` + kept checks -- pinned as fixtures so the conservative posture
is a tested property, not an accident.

### LI3 -- The post-loop fact (obligation 3)

**Goal:** code after the loop can use `p AND (not c)` -- the phase that makes
the feature worth shipping, and the one with the only genuinely new
soundness reasoning.

- Extend `rt_prove_paths`' `do` case (`elab_fns.c:2171-2187`). Today any
  `set!` in a non-final statement declines the whole split. New rule: a
  non-final statement that is a `while` **with a proven invariant** (both
  obligations from LI2) is stepped over with havoc-and-assume:
  1. collect the loop's assigned names (already known from LI2);
  2. alpha-rename each assigned name `x` to a fresh `x~N` in the *remaining*
     statements and the goal (`rt_rename_free`), declare `x~N`
     (`refine_env_declare`) -- this is the havoc: downstream `x` is a new
     value the old hypotheses do not constrain;
  3. push `p[x~N/x] AND (not c)[x~N/x]` as hypotheses.
  A loop whose invariant is unproven, or that hit any LI2 decline, keeps
  today's behavior (the `rt_form_mentions_set` decline).
- Same havoc-and-assume in `rt_collect_path_conds`
  (`elab_fns.c:2694-2826`), which today has no `while` case at all, so
  call-site crossings *after* a proven loop gain the fact too.
- Early-exit loops were already declined wholesale in LI2, so this phase
  never sees one.

**Acceptance:** `count-up` proves end to end (`1 proven` on the return
obligation, no `TUR-W0372`); the RE2 shape proves -- a
`sized-get-at`-style callee with
`#refine{ x : Slot | (and (>= x 0) (< x n)) }` called inside
`(while (< i n) :invariant (>= i 0) ...)` discharges both bounds under
`--strict-refine`; negative control with an off-by-one guard `(< i (+ n 1))`
rejects; a sabotage run confirms the havoc is load-bearing (strip step 2 and
the `errors/` fixture asserting a staled pre-loop hypothesis must wrongly
pass, per the WF sabotage convention).

### LI4 -- Diagnostics, conjunct splitting, strict mode

**Goal:** a failed preservation names the conjunct that broke.

- Sharpen the `what` strings so entry vs. preservation is unambiguous, and
  extend the `TUR-E0371`/`TUR-W0372` `tur explain` texts with a loop-invariant
  paragraph each (no new codes; see settled decision 8).
- Split a top-level `(and ...)` invariant into per-conjunct preservation
  obligations (memoized via RT7, so cost is bounded); report the failing
  conjunct by source span: "the loop does not preserve `(>= acc 0)`", plus
  the RT6 hint machinery (`refine_hint_search`) unchanged.
- `--strict-refine` promotion needs no new work (same three-site pattern in
  `refine_discharge.c`, same codes); a loop with no invariant stays silent
  (already free, but pinned by a fixture).

**Acceptance:** `errors/loop-invariant-not-preserved` shows the conjunct in
its `expected.diag` needles; the extended `tur explain` texts print; strict-mode
fixture pair (promoted error / silent bare loop).

### LI5 -- Breadth: fixtures, fuzzer, docs, ECS handoff

**Goal:** the feature is exercised beyond its own unit tests and the shipped
docs stop saying "deferred".

- Fuzzer: teach `tests/refine-fuzz-src.py` to emit `while`+`:invariant`
  shapes (true invariants, sabotaged invariants, bodies with declined
  writes); run `--n 400` at two seeds, require 0 soundness bugs -- the
  encoder history (both prior soundness bugs lived there) makes this
  non-optional.
- Docs: rewrite the `[deferred] A while loop is not analysed` bullet in
  `docs/guides/refinement-types-guide.md:954`; add the invariant rule,
  obligations table, and decline list to
  `docs/guides/refinement-solver-internals-guide.md`; syntax-guide note.
- Hand C3 to the ECS plan: update
  `docs/upcoming/v1/ecs-refinement-typed-apis-plan.md`'s prerequisite table,
  and prototype the `for-each` macro emitting `:invariant` in its expansion
  (the macro-expansion lateral hop in the scanners already handles expanded
  forms; verify with a fixture rather than assume).
- Graduation is a separate later decision per the experiment lifecycle;
  the row's `expires_at` forces it to be taken, not when it lands.

**Acceptance:** suite green modulo known-red fixtures; fuzz clean at two
seeds; guide text updated in the same PR as the code it describes.

---

## Effort

LI0+LI1 are small and mechanical (the `:pre`/`:post` scan and check-injection
templates are copy-adjacent). LI2 is the bulk: the sequential substitution
composer is new code, though every decline it needs is already implemented.
LI3 is small in lines but carries the only new soundness argument
(havoc-and-assume) -- budget the sabotage fixtures as first-class work, not
cleanup. LI4/LI5 are routine. No solver-stage changes anywhere: S0--S3 and
the caps carry the new obligations untouched, which was the original sketch's
bet and elaboration confirmed it.

## Explicitly not in scope

- **Invariant inference** of any kind, including "guess `>= 0` and see".
  Permanent, not deferred.
- **Termination.** A refinement says nothing about whether the loop finishes.
  A non-terminating loop with a true invariant is perfectly well-typed.
- **Ranking functions / decreasing measures.** Same reason. (A measure's
  *definition* entering the logic is the separate
  [reflected-measures-plan.md](reflected-measures-plan.md), which carves out
  its own totality obligation without touching program termination.)
- **`for` / `loop` surface support.** Nothing to support -- see settled
  decision 3. Macros that expand to `while` compose for free.
- **Preservation through arbitrary body control flow** (branching bodies,
  nested loops). Declined conservatively in the first cut; the
  branching-`let` split is the template if demand appears. (Since landed for
  branching bodies, nested loops -- havocked -- and an early `return` -- its
  paths pruned; see "Left open".)

---

## References

- [refinement-types-plan.md](../archive/refinement-types-plan.md) -- the
  parent plan; "Why checking, not inference" is the constraint this one
  inherits.
- [../guides/refinement-types-guide.md](../guides/refinement-types-guide.md)
  -- the user-facing write-up; the `while` limitation is documented at
  line 954 and is what LI5 rewrites.
- [../guides/refinement-solver-internals-guide.md](../guides/refinement-solver-internals-guide.md)
  -- pipeline, staged solver, caps.
- [v1/ecs-refinement-typed-apis-plan.md](v1/ecs-refinement-typed-apis-plan.md)
  -- the demand signal (C3) and the RE2 consumer.
- [checked-write-frames-plan.md](../archive/checked-write-frames-plan.md) -- WF3's
  `rt_collect_set_targets` is the assignment model this plan reuses; a landed
  `#writes` frame would later let a call in the loop body stop declining.
- Hoare (1969) -- the while rule; Floyd-Hoare initiation/preservation/use is
  the entire logical content of this plan.

---

## Delivered (2026-09-30)

All six phases, behind `--enable=loop-invariants` (row in
`src/runtime/experiments.c`, bit `g_opt_loop_invariants`, TUR-W0060 fired from
`elab_while` when an `:invariant` is written). Code: `elab_while`
(`elab_forms.c`) for LI0/LI1; the `li_*` block and `li_prove_paths_ext` in
`elab_fns.c` for LI2/LI3; `quiet` obligations and the invariant stats line in
`refine_discharge.c`; the TUR-E0371 / TUR-W0372 explain texts in `diag.c`.

### Where it departs from the elaboration above

- **The stray-keyword premise was wrong.** Settled decision 1 said a stray
  `:invariant` "would fail elaboration rather than be silently absorbed". It
  was silently absorbed: a keyword is a value, so `(while c :invariant p ...)`
  evaluated `p` as a statement each iteration and checked nothing. The keyword
  is now rejected anywhere but directly after the condition (and at most once).
- **The body substitution is applied to the goal FORM, not carried in
  `RefineObligation.subst`** (settled decision 4). The encoder leaves a
  substituted name FREE when its image does not encode, and a free `acc` in
  `p'` is the pre-body `acc` -- which proves `p'` from `p`, the one answer
  preservation must never give by accident. A Form-level substitution that does
  not encode fails the whole goal instead (unknown, check kept).
- **Branching bodies are in, not deferred.** `if`/`when` split the body into
  paths (at most 16), one preservation obligation per path under its
  substituted branch tests; the failing path is named in the diagnostic. This
  was the most common decline in practice.
- **Initiation facts come from a position-aware walk (`li_walk`), not
  `rt_build_env` alone.** The accumulator's `let` equation is what initiation
  needs, and the WF3 whole-body filter would drop it (the loop assigns the very
  name it mentions). The walk drops a fact only when code BEFORE the point
  assigns or borrows what it names; a plain `(set! x v)` adds `x = v`; an
  enclosing loop counts as having run any number of times. Call-site crossings
  use the same walk (under the gate) when their path crosses or steps over an
  annotated loop -- that is what gives the RE2 shape both bounds.
- **Havoc renames the PAST, not the future** (LI3 step 2). Renaming the
  statements after the loop handed a second proved loop a renamed copy of its
  own form, whose invariant still spoke of `x`: two loops in a row judged the
  final value by the FIRST loop's post-fact (a live wrong proof during
  development; `errors/loop-invariant-post-havoc`'s `down-up`). Every hypothesis
  already in the environment is instead rewritten so an assigned `x` reads a
  fresh `x~liN`, and `p AND (not c)` is assumed of the current `x`.
- **More declines than listed**, each closing a channel that rebinds a local
  with no `set!` in the loop -- and each one a live miscompile before it was
  closed: a name BORROWED anywhere in the function (`(& x)`, `&mut x`: an
  alias taken before the loop reaches a writing callee inside it), and a name
  assigned where code does not run in place (a lambda's shared cell, an
  effect-handler clause, `defer`, any special form `li_head_runs_in_place`
  does not allow-list). The invariant and condition may not read a field, a
  deref or a mutable global: those heads are unresolvable, and the encoder
  treats an unresolvable head as a congruent abstract measure.
- **Reporting is `quiet` + caller-worded, `runtime_guarded = false`.** A
  refuted preservation is TUR-E0371 naming the conjunct (LI4). A refuted
  initiation is TUR-E0371 when the walk kept every fact, or when the kept facts
  refute the goal outright (`(not q)` provable -- reported as "false for the
  value given here"); an OPEN counterexample from a walk that dropped a fact is
  TUR-W0372 instead, because it may rest on what was dropped.
- **Scope of the defn-level pass.** Loops are decided by the enclosing `defn`
  (so `main` and every ordinary function) or `definstance` method, the method's
  parameter refinements serving as its entry facts. A loop in a top-level
  `(def f (fn ...))` lambda has no definition to anchor its facts to and may be
  rebound, so it is **declined**: `TUR-W0372`, both runtime checks kept, and
  counted in the stats line like any other decline (`li_decline_unanalyzed`).
  Until 2026-10-03 both forms were silently unanalysed.

### Pre-existing bugs found and fixed on the way

- **mut-param-refinement-trusted-after-set** (miscompile). A `^mut` refined
  parameter reassigned in the body kept its ENTRY refinement as a hypothesis:
  `(set! n -5) n` under `n : Nat` proved a `Nat` return, elided the check, and
  returned -5. `rt_build_env` now drops a rebound `^mut` parameter's refinement
  (and a `:pre` naming it), and the return obligation then uses the whole body.
  Pinned by `refine-mut-param-rebound-not-trusted`.
- **Int/Real literal conflict** (miscompile, found by the LI5 fuzz
  population). S3 passed an LA-entailed equality between an Int and a Real
  term to EUF, and S1 held `0` and `0.0` as distinct constants -- a
  manufactured conflict, so `(> acc 0.0)` "followed" from `acc = 0.0` whenever
  an unrelated Int was pinned to the same value. Every fuzz soundness hit was
  this. `main` fixed the same root cause independently while this branch was
  open (#972: S1 compares int/real literals by value; model search now covers
  Real), so this branch's interim S3 guard was dropped in favour of it. The
  loop-shaped repro is pinned by `errors/loop-invariant-float-entry-false`.
- **`&mut` was not a borrow to WF3.** `rt_form_borrows_name` and
  `wf_borrow_write_free` only knew `(& x)`, so `&mut x` -- the borrow a callee
  writes through -- was invisible to them. Both now treat it as a write
  channel.

### Fixtures

Happy: `refine-loop-invariant-parses`, `loop-invariant-gate-off`,
`loop-invariant-entry-panics`, `loop-invariant-body-panics`,
`loop-invariant-count-up` (the header example, all three checks elided, stats
line pinned), `refine-loop-invariant-re2` (both bounds, a post-loop crossing,
the `for-slots` macro prototype of the `for-each` lowering),
`loop-invariant-branching`, `loop-invariant-post-loop`,
`loop-invariant-strict-bare-loop`, `loop-invariant-declines`,
`refine-mut-param-rebound-not-trusted`.
Errors: `loop-invariant-effectful`, `loop-invariant-not-bool`,
`loop-invariant-misplaced`, `loop-invariant-entry-false`,
`loop-invariant-not-preserved`, `loop-invariant-post-havoc` (the sabotage
fixture: skip the rename in `li_prove_paths_ext` and it fails),
`loop-invariant-strict-unproven`, `loop-invariant-re2-off-by-one`,
`loop-invariant-unseen-writes`, `loop-invariant-float-entry-false`.

### Fuzz

`tests/refine-fuzz-src.py` gained `shape_loop` (true, sabotaged, branching and
must-decline bodies; int and float accumulators) in the default mix and
`--only-shape loop`; both legs pass `--enable=loop-invariants`.
The first two `--only-shape loop --n 400` runs (seeds 11, 23) found **12
soundness bugs -- every one the Int/Real literal conflict above**, reached
through the float accumulator beside the int counter. With it fixed (the
loop-only rows re-run after merging #972, whose Real model search roughly
doubles the refutations):

| run | cases | proven / refuted | soundness bugs | other BUG | suspicious (report-only) |
|---|---|---|---|---|---|
| `--only-shape loop --seed 11` | 400 | 715 / 732 | **0** | 0 | 22 |
| `--only-shape loop --seed 23` | 400 | 665 / 767 | **0** | 0 | 15 |
| default mix `--seed 41` (before the merge) | 400 | 441 / 416 | **0** | 0 | 4 |

The "suspicious" rows are universal refutations (TUR-E0371 over all inputs)
of programs whose `main` happens not to pass a violating argument -- the
harness's documented report-only class.

### Left open

- Graduation, on the experiment lifecycle (the row's `expires_at` forces the
  review, not the date it happens).
- ~~A nested loop that assigns still declines the outer loop; a `when` guarding
  an early `return` still declines the whole loop.~~ **Resolved 2026-10-03**
  ([archived report](../archive/loop-invariant-declines-more-than-soundness-requires.md)).
  A nested loop's assigned names are havocked on every path, so the outer loop
  declines only if one of them is read after it. A `return`'s paths are pruned
  from the body composition, so initiation and preservation are still proved,
  and only the post-loop fact is withheld (`LoopInvSite.early_return`).
  Pinned by `loop-invariant-nested-and-early-return`,
  `errors/loop-invariant-early-return-refuted`, and the two new cases in
  `loop-invariant-declines`.
- ~~A loop outside a `defn` -- in a `definstance` method or a top-level lambda --
  is registered and given both runtime checks but never analysed, and says
  nothing at any strictness level.~~ **Resolved 2026-10-03**
  ([archived report](../archive/loop-invariant-silently-unverified-outside-a-defn.md)):
  methods are analysed, and a top-level lambda is declined out loud. See the
  "Scope of the defn-level pass" note above.
- The ECS `for-each` lowering (RE2) is **no longer waiting on this plan, and
  will not be the consumer that fires the trigger above.** C3 landed, and RE2
  was re-measured 2026-10-02 (see its probe update in
  [ecs-refinement-typed-apis-plan](v1/ecs-refinement-typed-apis-plan.md)): one
  `:invariant (>= i 0)` in the expansion does discharge the bounds obligation,
  3 of 3 proven under `--strict-refine`, with three negative controls
  refuting. But the profile that gated RE2 shows the per-access bounds
  re-check is *not* a real cost, and proving it changes no emitted code at all
  -- a parameter refinement is a *hypothesis*, checked in the callee's
  prologue, and the refinement guide says a hypothesis "is still checked at
  runtime". So RE2 would consume the correctness path only, and is unstarted
  by decision. Trigger 1 above, which is phrased in terms of a profile showing
  the re-check is a real cost, cannot be fired by RE2.
- ~~**The gap that should be closed before graduating this row** was
  `reads-measure-rejected-in-invariant-and-pre`: a `#reads` measure was
  rejected in a `:invariant` by `TUR-E0375` while a parameter refinement
  accepted it, so no stdlib container's length could appear in an
  invariant.~~ **Two of its three items resolved 2026-10-03**
  ([report](../reported/reads-measure-rejected-in-invariant-and-pre.md)). First
  the CT1 gate accepted a `#reads` measure in every contract position, so the
  bounded-index walk could be *written* against a real container and
  runtime-checked (`refine-reads-measure-contract-positions`). Then items 1 and
  3 landed: a shared borrow bound to a name nothing mentions -- the frozen
  marker idiom -- no longer makes the borrowed name volatile
  (`li_inert_borrows`; the proposed blanket narrowing to `&mut` was measured
  UNSOUND, since an inline-C callee does write through a shared borrow), and
  the elision veto asks the one contract-position gate instead of the plain
  purity walk. A bounded-index invariant now discharges on ENTRY and loses that
  check -- 3 of 4 obligations, where the whole loop used to be declined
  (`refine-loop-invariant-frozen-marker-analysed`).

  **Item 2 stays open, and it is not the plumbing task the report assumed.**
  Publishing the crossing's frozen set on the loop's obligations grants
  congruence ACROSS the body, and measurably proved `(<= (vlen v) 3)` preserved
  by a body calling `(vec-push! v 7)`: a live shared borrow does not pin a
  container whose mutators take it BY VALUE (`vec-push! [v : (Vec A) ...]`,
  `#fx{}`, no `#writes`), so nothing for `TUR-E0200` to reject. Held out by
  `refine-loop-invariant-byvalue-mutation-not-proved`. A sound grant needs the
  stdlib accessors to carry `#reads`/`#writes` first, which is
  `trusted-refinement-claims-plan` R4's territory, not this plan's.

  **So the remaining graduation questions are:** whether to accept that a
  bound over a container is proved on entry but not preserved; that this row is
  `XF_LIFECYCLE_PROTOTYPE`, so going straight to always-on skips a `beta` soak;
  that `tests/fixtures/loop-invariant-gate-off` asserts the gate-OFF behaviour
  with no `TUR_LOOP_INVARIANTS=0` hatch for it to invert onto (see the
  experimental-flags guide on a harness that inverts rather than retires); and
  that no consumer currently demands it -- RE2 is unstarted by decision, per
  the bullet above.
