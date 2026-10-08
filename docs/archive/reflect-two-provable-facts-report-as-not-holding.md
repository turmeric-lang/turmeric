# Two provable `^reflect` facts report as "does not hold for every input"

**RESOLVED 2026-10-03.** All three parts:

- **Shared: the note.** `emit_predicate_note` (`refine_discharge.c`) now
  takes the verdict. A refutation keeps "does not hold for every input here"
  (or "is false for the value given here" when closed). An unknown says
  "could not be proved here, which is not evidence that it fails". This
  improves every `TUR-W0372` in the language. Two fixtures pinned the old
  wording on an unknown: `errors/refine-unpropagated-result` and
  `refine-nonlinear-warn`.
- **Half 1.** `enc_cmp` (`refine_collect.c`) encodes an equality between two
  propositions as the operand itself against a `true`/`false` literal (or its
  negation), and otherwise as the implication pair, the same `iff` that
  `rf_def` already used for a Bool measure's own equation.
  `reflect-bool-equality-goal` proves all four spellings under
  `--strict-refine`. `errors/reflect-bool-equality-refuted` is the control: a
  wrong one is still refuted with its counterexample.
- **Half 2.** `rt_collect_path_conds` gives a crossing in a constructor arm the
  tag fact and the Int-sorted record selector equations, spelled as
  `rt_prove_paths` spells them. `reflect-crossing-in-match-arm` proves
  `via-crossing` under `--strict-refine`. Three guards come with it:
  1. The facts are added only when the callee's predicate mentions a
     reflected measure (`rt_pred_mentions_reflected`).
  2. They are added only for an arm that connects, transitively, to a
     variable the argument mentions (`RtRel`). That keeps the regression this
     report predicted from happening: a ground, false argument in an arm
     stays `TUR-E0371` with a model
     (`errors/reflect-crossing-ground-false-in-arm`). Measured before the
     guard: it degraded to unknown.
  3. Any path fact mentioning a name rebound below the level that collected
     it is dropped (`rt_cs_push` / `rt_rebinds_mentioned`). `_` binders and a
     binder repeated within one pattern are skipped.

  The third guard fixed a live pre-existing bug on the way. Two nested
  `let`s of the same name put `x = -5` and `x = 1` into one flat namespace,
  and the contradiction "proved" `(needs-pos x)` under `--strict-refine`
  (`errors/refine-crossing-shadowed-let`). The callee's entry check still
  caught it at runtime, so it was a wrong verdict, not a miscompile.

The original filing follows.


**Severity: low-medium (completeness, but the diagnostic text asserts
something false about the user's code).** Two independent shapes where a
reflected measure's fact is provable but comes back unknown. Neither is a
wrong compile -- the runtime check is kept, so the program is correct -- but in
both the note reads `the predicate ... does not hold for every input here`,
which is a claim that the code is wrong. It is not: in half 1 the predicate is
a tautological restatement of one that proves, and in half 2 the identical
fact proves ten lines above. Filed 2026-10-02, found reviewing the
`reflected-measures` row at the v0.59.0 cut. Both are noted as limitations in
[reflected-measures-plan](../upcoming/reflected-measures-plan.md); this is the
measured version with repros.

Run both repros with `--enable=reflected-measures`.

## Half 1 -- `(= r true)` is unknown where bare `r` proves

```turmeric
(defdata Lst [] (Cons [hd : int tl : Lst]) (Nil))

(defn ^reflect all-pos? [xs : Lst] : bool
  (match xs
    (Nil) true
    (Cons h t) (and (> h 0) (all-pos? t))))

;; Proves, silently.
(defn bare [xs : Lst] : #refine{ r : bool | r }
  (all-pos? (Cons 1 (Nil))))

;; The same proposition, spelled as an equality.
(defn eq-true [xs : Lst] : #refine{ r : bool | (= r true) }
  (all-pos? (Cons 1 (Nil))))
```

Observed -- one diagnostic, on `eq-true` only:

```
warning [TUR-W0372]: solver returned unknown for the refinement on the
return value of 'eq-true'; runtime check kept
note: the predicate (= r true) does not hold for every input here
```

### Root cause

An equality between two propositions is an atom the cube expansion cannot see
inside. The encoder already knows this and works around it for the measure's
*own* equation -- `refine_collect.c:938`:

```c
/* A Bool measure is a proposition, and its equation is an `iff`.  Spelled
 * as two implications rather than `(= p q)`: the solver's cube expansion
 * splits an implication natively, while an equality between propositions
 * is an opaque atom it cannot see inside (the `all-pos?` fixture stayed
 * Unknown under `=` and proves under the pair). */
if (t->sort == VS_BOOL)
    return vc_mk2(E->vc, VC_AND,
                  vc_mk2(E->vc, VC_IMPLIES, app, t),
                  vc_mk2(E->vc, VC_IMPLIES, t, app));
return vc_mk2(E->vc, VC_EQ, app, t);
```

A *user-written* `(= r true)` goes through the ordinary goal path and gets the
plain `VC_EQ`, so it hits exactly the limitation this code comments on.

### Fix directions

- Apply the same rewrite on the goal side: when a `VC_EQ` has two `VS_BOOL`
  operands, build the implication pair instead. That is the fix the comment
  above already argues for, applied one level out, and it is sound in the same
  way (`p = q` and `(p => q) and (q => p)` are the same proposition).
- `(= r true)` specifically also admits a cheaper normalization -- an equality
  against a boolean literal is the operand itself (or its negation), so
  `(= r true) -> r` and `(= r false) -> (not r)` before encoding. Worth doing
  anyway as a readability-neutral simplification, but it does not cover
  `(= p q)` between two non-literal propositions, which the first direction
  does.

## Half 2 -- the same RF4 fact proves at a return obligation, not at a crossing

```turmeric
(defdata Lst [] (Cons [hd : int tl : Lst]) (Nil))

(defn ^reflect sorted? [xs : Lst] : bool
  (match xs
    (Nil) true
    (Cons h t) (match t
                 (Nil) true
                 (Cons h2 _) (and (<= h h2) (sorted? t)))))

(defn needs-sorted [ys : #refine{ v : Lst | (sorted? v) }] : int 1)

;; Return obligation: proves.  (This is tests/fixtures/reflect-nonground-arm.)
(defn via-return [xs : #refine{ v : Lst | (sorted? v) }] : #refine{ r : bool | r }
  (match xs
    (Nil) true
    (Cons h t) (match t
                 (Nil) true
                 (Cons h2 t2) (sorted? t))))

;; Crossing: the SAME fact, needed as needs-sorted's precondition.
(defn via-crossing [xs : #refine{ v : Lst | (sorted? v) }] : int
  (match xs
    (Nil) 0
    (Cons h t) (match t
                 (Nil) 0
                 (Cons h2 t2) (needs-sorted t))))
```

```sh
TUR_REFINE_STATS=1 ./build/tur --enable=reflected-measures check repro.tur
./build/tur --enable=reflected-measures --strict-refine check repro.tur
```

Observed: `4 obligation(s): 3 proven, 0 refuted, 1 unknown`, and under
`--strict-refine` the one unknown is named:

```
error [TUR-W0372]: solver returned unknown for the refinement on argument 1
of 'needs-sorted' in 'via-crossing'; runtime check kept
note: the predicate (sorted? v) does not hold for every input here
```

`via-return` proves the identical `sorted?(t)` from the identical arm
hypotheses. The difference is only where the fact is needed.

### Root cause

RF4 selects a `match` arm from a tag fact `(= (#dt/tag s) k)` in the
hypotheses. For a **call-site crossing**, those facts are never collected.
`rt_collect_path_conds` (`src/compiler/elab_fns.c:4131`) walks the caller's
body to the call and, on a `match`, records only:

- the arm's `when` guard, if any (`elab_fns.c:4255`), and
- for a *literal* pattern, `(= scrut lit)` (`elab_fns.c:4259`).

A constructor pattern contributes no tag fact and no field-selector
equations, so RF4's selector has nothing to match against and the measure
stays opaque. The omission is deliberate and the plan says why: a pattern
binder that shadows an outer name would inherit that name's hypotheses in the
flat namespace -- there is a shadow veto at `elab_fns.c:4247` that bails out of
the whole walk when it sees one.

The plan also records the reason it was not simply extended: every
tag/selector fact is a ufunc, and `refine_model_search` declines any VC
carrying one, so pushing them into crossing VCs would turn currently-refuted
crossings (`TUR-E0371` *with a counterexample*) into unknown ones. That
trade was judged to cost more than it gains, and RF6.1 was done first.

### Fix directions

RF6.1 has since landed, which changes that calculus: `refine_model_search`
now runs on a VC whose every ufunc is a constructor or a reflected measure
(`RefineVC::reflect_model_ok`). Tag and selector symbols are constructor-side
facts, so the question is whether they can be admitted under the same
`reflect_model_ok` predicate rather than counting as general ufuncs. If they
can, the objection above is gone and the extension is:

1. Emit the arm's tag fact and binder selector equations in
   `rt_collect_path_conds`' `match` branch, reusing the forms
   `rt_prove_paths` already builds for a return obligation so the two paths
   agree syntactically (RF4 compares modulo the binder equations via
   `rf_canon`, so they must be spelled the same way).
2. Keep the existing shadow veto at `elab_fns.c:4247` -- it is doing real
   work and is orthogonal.
3. Gate on `reflect_model_ok` staying true for the resulting VCs, and pin it:
   a fixture whose crossing is genuinely false must still report `TUR-E0371`
   with a model, not degrade to unknown. That is the regression this ordering
   was protecting against, so it is the one to test.

A narrower alternative, if the model-search interaction does not hold up:
collect tag/selector facts only when the crossing's VC already mentions a
reflected measure. That confines any term growth and any model-search
degradation to obligations that cannot be decided without the facts anyway.

## Shared: the note is wrong in both halves

Independent of either fix, `the predicate ... does not hold for every input
here` is the wrong sentence for an unknown. It is correct for a **refutation**
(`TUR-E0371`, where a model exists and the predicate demonstrably fails). For
`TUR-W0372` the solver has said it cannot decide, which is not evidence either
way, and the current wording sends a user looking for a bug in code that is
provably fine. Something like "could not be proved here (the solver returned
unknown); the runtime check is kept" would be true of both halves, and of
every other unknown.

This is cheap and worth separating from the two completeness fixes -- it
improves every `TUR-W0372` in the language, reflected or not.
