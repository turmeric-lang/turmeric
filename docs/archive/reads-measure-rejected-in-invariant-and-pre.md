# A `#reads` loop invariant is runtime-checked but never proved, even inside `frozen`

**Severity: low-medium (completeness: a correct invariant keeps both runtime
checks).** **RESOLVED 2026-10-03**, in three steps the same day. Filed
2026-10-02, found looking for an illustrating use case for `loop-invariants`.
A bounded-index walk over a container is now PROVED inside a `frozen` region
and both of its runtime checks are elided
(`refine-loop-invariant-reads-frozen-proved`).

- **Items 1 and 3** -- see "Resolution of items 1 and 3". A frozen-region
  marker no longer declines the loop, and a proved `#reads` invariant's check
  is elided.
- **Item 2** -- see "Item 2, re-measured" and then "Resolution of item 2". The
  premise this report stated was WRONG: publishing the crossing's frozen set on
  a loop's obligations grants congruence across a body that can legally mutate
  the container, and doing it proved `(<= (vlen v) 3)` preserved by a body
  calling `(vec-push! v 7)`. What landed instead keys the grant on a WRITE
  PROMISE rather than on borrow liveness.

Two things this does NOT resolve, both separate and both still open:

- `vec-len`'s `#fx{}`-versus-purity-walk divergence under "Orthogonal,
  unchanged", which waits on `trusted-refinement-claims-plan` R4.
- The promise the grant now leans on is TRUSTED, not verified: a hand-written
  false `#reads` reaches the grant and nothing reports it --
  [reads-frame-verification-ignores-a-callee-write-frame](../archive/reads-frame-verification-ignores-a-callee-write-frame.md),
  filed from this work.

## What was fixed (2026-10-03)

`rt_diag_impure_pred` (`src/compiler/elab_fns.c`) is the sole `TUR-E0375`
emitter, and it now asks `rt_pred_observably_impure` instead of the plain purity
walk. That classifier treats a call the predicate makes **directly** to a
`#reads` measure as UNKNOWN rather than walking the measure's inline-C body:

- CT1's concern is a check whose evaluation changes state, so compiling it in or
  out is observable. Reading borrowed state is neutral however often it runs.
- `#reads` is the trusted claim that the body only reads, the same trust a
  parameter refinement already extended to it (`TUR-W0383` reports the
  violations the compiler can see).
- Only depth 0 is exempt. The measure's ARGUMENTS are still walked, an impure
  term beside the measure is still `E0375`, and so is an unannotated wrapper
  around the measure. No callee verdict is computed or memoized under the
  exemption.

`elab_loop_invariant_pred`'s hard bail uses the same classifier, so the
invariant now survives as a runtime-checked contract instead of being dropped.
The guard in `rt_inject_param_checks` still skips a `#reads` parameter's entry
check, but its comment now says why: a choice of enforcement point (the
crossing), not a purity verdict.

Pinned by:

- `tests/fixtures/refine-reads-measure-contract-positions`: all five positions
  accepted, run and passing.
- `tests/fixtures/refine-reads-measure-invariant-runtime-check`: the kept entry
  check fires at runtime.
- `tests/fixtures/errors/refine-impure-accessor-contract-positions`: the
  negative control. An unannotated inline-C accessor is `E0375` in all four
  positions, and so are a `(tick)` beside a `#reads` measure and an
  unannotated wrapper around one. That accessor is LOCAL to the fixture as of
  the item 2 resolution below -- it used to be stdlib's `vec-len`, which is
  `#reads v` now, and a control for "unannotated" must not depend on which
  stdlib functions happen to carry an annotation.

## The proof inside `frozen` -- as filed (1 and 3 since closed, 2 since re-measured)

```turmeric
(defn vlen [^borrow v : (Vec int)] #reads v : int (vec-len v))

(defn sum-all [^borrow v : (Vec int)] : int
  (let [__f (& v)]                       ;; what `(frozen v ...)` lowers to
    (let [^mut acc 0
          ^mut i   0]
      (while (< i (vlen v)) :invariant (and (>= i 0) (<= i (vlen v)))
        (set! acc (+ acc (vec-get v i)))
        (set! i (+ i 1)))
      acc)))
```

```
warning [TUR-W0372]: the invariant of the while loop in 'sum-all' is not
analysed statically: 'v' is borrowed, or assigned inside a lambda or handler
clause, in 'sum-all', so it can change with no `set!` in the loop
```

Without the region, the conjunct `(<= i (vlen v))` is `unknown` on entry and on
preservation, because each `(vlen v)` is a fresh symbol. That is correct
outside `frozen`. Three things stand between this and a proof inside it, and
each is a separate change:

1. **The region itself declines the loop.** `li_lambda_targets`
   (`elab_fns.c`) marks every name borrowed anywhere in the function as
   *volatile*, including by `(& v)`, and `li_analyze_one` declines any loop
   whose invariant mentions a volatile name. The channel it guards is real for
   `&mut` (`errors/loop-invariant-unseen-writes`). A shared `(& x)` cannot be
   written through: `(set! @r ...)` on one is a type error, measured. So the
   remaining exposure is an inline-C callee handed `(& x)`. Narrowing the
   volatile set to `&mut` borrows (or to `^mut` names) is a soundness decision
   for the feature's conservative posture, so it is not made here.
2. **Loop obligations carry no frozen set.** Call-site crossings call
   `refine_obligation_set_frozen` so the encoder can grant a `#reads` measure
   congruence (`enc_reads_args_frozen`, `refine_collect.c`). `li_obligation`
   builds its obligations without one, so even with (1) fixed, each
   `(vlen v)` would stay a fresh symbol.
3. **Elision is vetoed for any non-pure head.** `li_analyze_one` returns before
   eliding when `rt_pred_is_impure(e, s->inv)`, and a `#reads` measure is not
   `info.pure`. A proof would stand for the post-loop fact but leave both checks
   in. For a crossing, the trusted-annotation argument is in
   [stateful-refinements-guide](../guides/stateful-refinements-guide.md#why-a-trusted-annotation-is-sound-here)
   (the callee's own check is the backstop). An invariant has no such backstop,
   so eliding one on the strength of `#reads` needs its own argument.

## Orthogonal, unchanged

`vec-len` is declared `#fx{}` -- pure on the effect row -- while the refinement
purity walk calls its inline-C body impure. A reader who sees the `#fx{}` row
reasonably concludes it is pure. The real fix is the one
[trusted-refinement-claims-plan](trusted-refinement-claims-plan.md)'s
R4 is blocked on: make the measure layer hold its state in Turmeric-visible
structs instead of a malloc'd block behind inline C, so the stdlib accessors
stop being inline C. A purity allowlist of stdlib accessors would be the
stopgap.

## Relationship to graduating `loop-invariants`

The bounded-index walk can now be **written** against a real container and
**proved** inside a region, with both runtime checks elided. Before any of
this, `loop-invariants-plan` and `ecs-refinement-typed-apis-plan` could not
express it at all.

`loop-invariants-plan` names this report as the gap to close before graduating
that row, and it is closed. What remains for that decision is not a capability
gap: whether to graduate straight from `XF_LIFECYCLE_PROTOTYPE` and skip a
`beta` soak, what to do with `loop-invariant-gate-off` (which asserts the
gate-OFF behaviour and has no `TUR_LOOP_INVARIANTS=0` hatch to invert onto),
whether any consumer wants it -- RE2 is unstarted by decision -- and whether
the trust the frozen grant now rests on is acceptable while
[reads-frame-verification-ignores-a-callee-write-frame](../archive/reads-frame-verification-ignores-a-callee-write-frame.md)
is open.

The companion completeness report,
[loop-invariant-declines-more-than-soundness-requires](loop-invariant-declines-more-than-soundness-requires.md),
was resolved the same day.


## Item 2, re-measured (2026-10-03) -- the premise was wrong

**Doing what item 2 says produces an unsound proof.** `LoopInvSite` was given a
frozen set captured at `li_register_site` (where the borrow scope is still
live; the defn-level pass runs after it is gone), built exactly like
`refine_note_call_site`'s and narrowed further to `BK_IMMUT` only, and
`li_obligation` passed it to `refine_obligation_set_frozen`. The bounded-index
walk then reported `4 proven, 0 unproven` -- and so did this:

```turmeric
(defn walk [^mut v : (Vec int)] : int
  (let [__frozen (& v)]
    (let [^mut i 0]
      (while (< i 2) :invariant (<= (vlen v) 3)
        (vec-push! v 7)
        (set! i (+ i 1)))
      (vlen v))))
```

`(<= (vlen v) 3)` is false from the first iteration on, and preservation was
**proved**, with the re-establishment check elided. Nothing would have caught
it at runtime.

The reason is a gap between what a frozen set MEANS and what a loop needs:

- For a **crossing** it means "a shared borrow of `x` is live at this POINT",
  which is enough to make two occurrences of a `#reads` measure *within one
  predicate* congruent.
- A **loop** needs more: that `x` does not change **across the body**.

A live shared borrow does not give that, because **every stdlib container's
mutator takes the container by value**. `vec-push!` is
`[v : (Vec A) val : A]`, `#fx{}`, with no `#writes` frame -- so there is no
conflicting borrow for `TUR-E0200` to reject and the push is perfectly legal
inside the marker region. `stdlib/vec.tur` contains **zero** `#reads`
annotations, so neither `vec-get` nor `vec-push!` is distinguishable from the
other by anything the analysis can ask.

The existing frozen-region fixture `refine-stateful-resizable-bounds` works
because its mutator `grow!` takes the buffer **exclusively**, so the borrow
checker does lock it out -- its own comment says so. That is the shape where
the grant is sound, and `Vec` is not that shape.

So the plumbing is deliberately NOT in `li_obligation`, and
`tests/fixtures/refine-loop-invariant-byvalue-mutation-not-proved` is the
adversarial control that keeps it out: `0 proven, 2 unproven`, both checks
emitted, and the program dies on the kept one.

**What a sound version would need**, in rough order of cost:

1. Withhold the grant unless every call in the loop body that receives the
   frozen name is to a `#reads` measure covering that parameter. Checkable
   today, and sound on the existing `#reads` trust -- but it buys nothing for
   `Vec` until the stdlib accessors carry the annotation, because
   `(vec-get v i)` in the body would withhold it.
2. Annotate the stdlib container accessors (`#reads`) and their mutators
   (`#writes`, or an exclusive-taking signature). This is the real fix and is
   `trusted-refinement-claims-plan` R4's territory -- the measure layer holding
   state behind inline C is why the annotations are absent.
3. Only then is the loop's frozen grant worth plumbing, and it should be
   written against the *write* evidence rather than the borrow-liveness
   evidence, since the latter demonstrably does not imply it here.

## Resolution of items 1 and 3 (2026-10-03)

Measured on `tests/fixtures/refine-loop-invariant-frozen-marker-analysed`
against `origin/main` at `b87ca3553`:

| | baseline | after |
|---|---|---|
| `refine: invariant:` | 0 proven, 0 unproven, **1 declined** | **3 proven**, 1 unproven, 0 declined |
| entry check in emitted C | emitted | **elided** |
| re-establishment check | emitted | emitted (correctly -- see item 2) |

**Item 1 -- the region declines the loop.** The premise that a shared `(& x)`
is not a write channel was checked, and it is only half true. From Turmeric it
holds: `(set! @r 99)` on one is rejected with "cannot assign through immutable
borrow; use `&mut T` for mutation". But an **inline-C callee writes straight
through one**: a `defn poke [p : &int] : int` whose inline-C body is
`*(int64_t *)p = 99;`, called as `(poke (& x))` on a `^mut x` bound to 1,
leaves `x` at 99 -- measured, printed.

So narrowing the volatile set to `&mut` borrows, which is what this item
proposed, is **unsound**, and the report was right not to make that call. What
landed is narrower and needs no trust: a shared borrow bound by a `let` to a
name **nothing in that `let` body mentions** is exempt (`li_inert_borrows`,
matched by form identity, and only for a binding vector of exactly
`[sym (& sym)]` or `[sym : T (& sym)]` so a spec cannot be mispaired). That is
the marker idiom, and a borrow nothing can name has no call to be handed to,
so the inline-C exposure provably cannot arise. Every other shape still
declines: `loop-invariant-declines` gained `named-borrow` (the same borrow,
bound to a name that IS used) and `borrow-in-loop` (a shared borrow taken
inside the loop).

**Item 3 -- elision is vetoed for any non-pure head.** This needed no new
argument, because the gate that admits the predicate already makes it. The
elision site asked `rt_pred_is_impure` (the plain purity walk, which calls a
`#reads` measure impure because its body is inline C) while every other
contract position asks `rt_pred_observably_impure`, whose own comment states
the intent: *"One gate, so every contract position -- parameter, `:pre`,
`:post`/return, `:invariant` -- agrees."* The veto now asks that gate, on the
elaborated predicate (`LoopInvSite::pred_e`); `li_elision_observable`.

Two things make that safe rather than merely consistent. The carve-out is the
gate's and stays depth-0, so an unannotated wrapper and `(vlen (next-vec!))`
are untouched. And the shape the veto was built for cannot reach it any more:
`(>= (tick) 0)` -- the counter-bumping predicate the fuzzer found as an output
divergence -- is now a hard `TUR-E0375` in `elab_loop_invariant_pred`, before
analysis. Elision also stays **per obligation**: in the fixture above entry is
proved and loses its check while preservation is not and keeps its own, which
is the pair of facts that pins items 1 and 3 together.

Code: `li_inert_borrows` / `li_form_in_set` / `li_elision_observable` in
`src/compiler/elab_fns.c`; the `pred_e` back-fill in `elab_while`
(`src/compiler/elab_forms.c`); `pred_e` on `LoopInvSite` in
`src/compiler/elab_internal.h`.

## Resolution of item 2 (2026-10-03)

The grant is keyed on a **write promise** instead of on borrow liveness, and
the stdlib readers now carry one.

**The guard** (`li_name_reads_only`, `li_frozen_for_site`). The live shared
borrows at a loop are captured at registration as CANDIDATES
(`LoopInvSite::frozen_cands`) -- still the only point where the borrow checker
knows what is live. A candidate reaches an obligation only if every occurrence
of it in the loop's CONDITION and BODY is a direct argument at a position some
callee has promised not to write. Two promises count:

- `#reads p`, in the positions its mask names -- the trusted one.
- a callee the walk knows is PURE, in every position -- the proven one.
  `info.pure` is the same bar congruence itself is held to, and something that
  writes nothing cannot write this.

Everything else withholds, and withholding is the safe direction, so every
shape the walk cannot model answers "withhold": a bare mention, an argument to
an unannotated impure callee, a term inside a constructed value, an
overflowing walk, a computed head. The invariant itself is not walked -- it is
already gated non-observably-impure, so evaluating it cannot write.

**A promise counts only if the name still RESOLVES to the binding it was read
off**, and getting this wrong was a live miscompile for the length of one
build. `rt_resolve_fn` answers from the GLOBAL scope only, so it hands back
stdlib `vec-get`'s `#reads v` for a call that actually goes to a LOCAL binding
of that name. Measured, before the check existed:

```turmeric
(let [vec-get (fn [vv : (Vec int) ii : int] : int (do (vec-push! vv 7) 0))]
  ... (while (< i 2) :invariant (<= (vlen v) 3) (vec-get v i) ...))
```

was given the grant, `(<= (vlen v) 3)` was reported PRESERVED, the
re-establishment check was elided, and the program printed 5 -- the violated
bound, silently. With the check it is `0 proven, 2 unproven`, the check stays,
and the loop dies on it. Pinned by
`refine-loop-invariant-shadowed-reads-not-trusted`.

That is also why the whole filter runs at REGISTRATION rather than in the
defn-level pass: the loop's scope is live there, and it is the only place the
question can be asked. The cost is that a `#reads` measure defined LATER in
the file is not yet in global scope and so withholds -- conservative, and the
safe direction again.

**The annotations.** `stdlib/vec.tur`'s `vec-len` and `vec-get` carry
`#reads v`. They are the first `#reads` in stdlib, and both measured
prerequisites held: the annotation is live on a BY-VALUE parameter and on a
GENERIC function (each passes the CT1 gate in a `:pre`, which only happens
through the `#reads` carve-out), so neither needed a signature change. The
mutators deliberately carry nothing -- that asymmetry IS the discriminator,
because the signatures cannot be: `vec-get` and `vec-push!` both take the
vector by value.

Measured:

| | before | after |
|---|---|---|
| bounded-index walk | 3 proven, 1 unproven | **4 proven, 0 unproven**, both checks elided |
| body calls `vec-push!` | 0 proven, 2 unproven | unchanged -- still withheld |
| body calls `vec-set-o!` | n/a | 1 proven, 1 unproven -- withheld |
| body calls an unannotated inline-C callee | n/a | 1 proven, 1 unproven -- withheld |

Pinned by `refine-loop-invariant-reads-frozen-proved` (the positive case),
`refine-loop-invariant-frozen-grant-declines` (the setter and the opaque
callee), and `refine-loop-invariant-byvalue-mutation-not-proved` (the
adversarial `vec-push!`, whose kept check still fires at runtime).

**What this does NOT make verified.** The `#reads` half of the promise is
trusted, and a hand-written false one reaches the grant with no diagnostic:
`reads_checked` for an inline-C body is UNVERIFIED (visible in
`read-frames-dump-verdicts`, which now opens with the two stdlib rows), and the
reads verification never consults a callee's write frame. Filed as
[reads-frame-verification-ignores-a-callee-write-frame](../archive/reads-frame-verification-ignores-a-callee-write-frame.md).
The annotations added here are truthful -- both bodies dereference and return
-- and the exposure is not new in kind: a call-site crossing has consumed the
same trust since C2. It is new in CONSEQUENCE, because a crossing keeps the
callee's own entry check as a backstop and an elided invariant check has none.

Code: `li_name_reads_only` / `li_frozen_for_site` and the `frozen_cands`
capture in `src/compiler/elab_fns.c`; `frozen_cands` / `frozen_names` on
`LoopInvSite` in `src/compiler/elab_internal.h`; the two annotations in
`stdlib/vec.tur`.
