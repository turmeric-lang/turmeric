# Language/codegen gaps surfaced by the pure-Turmeric `logic.tur` port

> **Archived 2026-07-19** after re-verifying the resolution against the current
> tree: the GAP 1 repro prints `15` (was SIGSEGV), the GAP 2 repro prints `6`
> (was a `TUR-E0001` match-arm mismatch), and the GAP 3 Part A repro raises
> `TUR-E0295`. All five regression fixtures (`opaque-fn-carrier-dispatch`,
> `forward-ref-adt-return`, `errors/byvalue-adt-cast-to-int`,
> `errors/int-cast-to-byvalue-adt`, `box-unbox-byvalue-aggregate`) and all eight
> `logic-*` fixtures pass, and `stdlib/logic.tur` carries no `:int`-carrier goal
> erasure (`apply-goal` is `(Goal int)`; `fresh`/`fresh-impl` take `^fat`). The
> two remaining checklist boxes (optional `UState` readability reshape under GAP
> 3; a snapshot/docstring regen) are explicitly non-correctness items and are
> left as-is.
>
> **Status:** ALL RESOLVED 2026-07-13. GAP 1 **fixed** (compiler); GAP 2 **fixed**
> (compiler); GAP 3 **fixed** (compiler, Parts A + B); GAP 4 **withdrawn** (not a
> gap -- it was a missing `^fat` annotation, now applied). The `logic.tur`
> workarounds they concerned (the goal-handle `:int` carrier and the `:fn`
> callback) have been removed.
> GAP 3 is **fixed** 2026-07-13: the unsound direct `::` now raises `TUR-E0295`
> on both harnesses (Part A), and the sound bridge is `any` + `cast` -- `(:: v :any)`
> now heap-boxes correctly (it was miscompiling) and `(cast h T)` reads it back
> (Part B). All four gaps are now closed or withdrawn.
> **Reported:** 2026-07-13 (during the `stdlib/logic.tur` pure-Turmeric port).
> **Context:** `logic.tur` is now inline-C-free and runs under both harnesses,
> but getting there required four workarounds that paper over real gaps. None
> blocked v1 (the engine is correct and green), but each one made the code less
> honest than it should be -- notably forcing `:int`-carrier plumbing the
> "No Lazy `:int`" rule otherwise discourages. This report captures the gaps and
> a plan to retire the workarounds once they are fixed.

The four are independent; fix and clean up in any order. Severity is "how much
it hurt this port," not a release gate.

---

## GAP 1 -- calling an opaque HKT handle cast to a `fn` type emits a *thin* call (SIGSEGV) -- **HIGH** -- FIXED 2026-07-13

**Fix (as landed).** Two changes in `elab_ascribe` (`src/compiler/elab_types.c`),
both keyed off a new `ascribe_type_is_opaque_handle` helper:

- **Consumer side.** The existing "fat-handle ascription" rule (a `:int` /
  `:ptr<void>` carrier ascribed to a `fn` type is marked `boxed` for slot-0
  dispatch) now also fires when the *source* is an opaque newtype whose carrier
  is the int64/pointer slot -- so `(:: g (fn [Subst] Stream))` on a `(Goal int)`
  fat-dispatches instead of emitting a thin call.
- **Producer side.** A *bare, captureless* closure (`TY_FN`, unboxed) ascribed to
  an opaque handle or `:ptr<void>` is now wrapped in `EX_FN_TO_FAT` so it crosses
  as a uniform `{ thunk, env }` fat box -- mirroring the captureless-closure
  boxing already done for `TY_TYVAR` escapes. This also fixed a *latent* variant
  the report missed: a no-capture goal like `logic.tur`'s `succeed` / `fail`
  segfaulted when dispatched compiled, because it was stored as a raw code
  address and slot-0-dispatched. Both now work.

The `logic.tur` workaround (goal handles carried as `:int`) has been removed:
`apply-goal` is typed `[g : (Goal int) ...]` and the `(:: g :int)` erasures are
gone from the combinators and instances. See the cleanup checklist below.

Repro below now prints `15` (was SIGSEGV). Original report follows.

**Summary.** When a value typed as an opaque HKT handle (`(Goal A)`,
`(Box A)`, carrier `:ptr<void>`) is cast to a function type and applied, codegen
emits a **thin** function-pointer call that ignores the fat-closure env. If the
underlying closure captured anything, the call jumps into the env block and
segfaults. Casting the *same* value through its erased `:int` carrier first
takes the fat-dispatch path and works. Silent miscompile, not a diagnostic.

**Minimal repro.**

```turmeric
(defopaque Box [A] :ptr<void>)
(defn apply-box [b : (Box int)] : int ((:: b (fn [int] int)) 10))
(defn mk [cap : int] : (Box int)
  (:: (let [c cap] (fn [x] (+ x c))) :Box))   ; fat closure (captures c)
(defn main [] : int (println (apply-box (mk 5))))  ; want 15
```

`tur run` -> **Segmentation fault**. Change `apply-box`'s parameter to `b : int`
and call `(apply-box (:: (mk 5) :int))` and it prints `15`.

**Root cause (where known).** `elab_call_head_expr`
(`src/compiler/elab_call.c:984`). It sets the head temp's `closure_fn_binding` /
`type.as.fn.boxed` (the fat/slot-0 dispatch markers) only when the head is
`TY_PTR_VOID` *or* a curried/poly/tyvar-carrier `EX_CALL`. When the head is an
opaque handle ascribed to a `fn` type, `head_kind` resolves to `TY_FN` and the
`source_expr` is a plain binding ref (not `EX_CALL`), so neither branch fires;
`closure_fn_binding` stays NULL and emit falls back to a thin pointer call. The
`:int`-carrier form works precisely because the erased-carrier path is what the
existing "uniform fat box / slot-0 dispatch" logic already handles.

**Fix directions.** In `elab_call_head_expr`, when `source_expr`'s *static* type
is an opaque whose carrier is `:ptr<void>` (or otherwise an erased handle),
route it through the same fat-dispatch path as the `TY_PTR_VOID` head: set
`closure_fn_binding = expr_closure_fn_binding(source_expr)` (or mark the head
temp `boxed`). Minimum acceptable outcome: never emit a thin call for a value
whose runtime representation may be a fat box -- if fat-dispatch genuinely
cannot be determined, emit a diagnostic rather than a silent thin call.

**Workaround in the tree.** `stdlib/logic.tur`: `apply-goal [g : int ...]` and
`apply-fat`/goal combinators all carry the goal handle as its `:int` carrier and
apply via `(:: g (fn [Subst] Stream))`, with `(:: g :int)` erasures sprinkled
through `conjoined`/`disjoined`/`fresh`/`run-logic` and the four instances.

---

## GAP 2 -- forward/mutually-recursive `defn` return type defaults to `int` -- **MEDIUM** -- FIXED 2026-07-13

**Fix (as landed).** The Pass-1 forward-declaration loop in
`elaborate_program` (`src/compiler/elab_toplevel.c`) parsed a bare `: T` return
annotation for primitives only (`int` / `bool` / `void` / `nil` / `cstr` /
`ptr`); every other bare symbol -- including a user `defdata` / `defstruct` /
`defopaque` name -- fell through to the `TY_INT` default. Added an `else` branch
that looks the name up in the already-registered ADT stubs (RF0 runs first) and
forward-declares with the real result type (`type_adt(def)`, carried on
`result_full_type`). A sibling caller declared earlier now sees the callee's
actual ADT return type. The compound-return path (`(F A B)`) already did this;
this closes the bare-name case. Regression: `tests/fixtures/forward-ref-adt-return`.
Repro below now prints `6`. Original report follows.

**Summary.** A `defn` that calls a *later-defined* `defn` types that forward
reference as returning `int`, regardless of the callee's declared return type.
For a mutually-recursive pair this poisons `match`-arm unification.

**Minimal repro.**

```turmeric
(defdata T :copy (A :int) (B :T))
(defn f [x : T] : T
  (match x (A n) (g n) (B inner) (f inner)))   ; g is defined below
(defn g [n : int] : T (A (+ n 1)))
(defn main [] : int (match (f (B (A 5))) (A n) (println n) (B _) (println -1)))
```

`tur run` ->
`TUR-E0001: match: arm types are incompatible -- expected int (from earlier
arm), got adt`. The first arm `(g n)` is assumed `int`; the second arm is `T`.
Reordering so no forward reference exists (or making `f` self-recursive) fixes
it.

**Root cause (where known).** Not pinned to a line. The elaborator resolves a
call to a not-yet-elaborated `defn` with a default/placeholder result kind
(`int`) instead of its declared signature. Two-pass signature collection (bind
every top-level `defn`'s declared type before elaborating any body) would remove
the ordering dependence.

**Fix directions.** Pre-register all top-level `defn` return types in a first
pass so forward references resolve to the declared type. Until then, mutually
recursive helpers must be written self-recursively or ordered callee-first.

**Workaround in the tree (kept by choice).** `stdlib/logic.tur`: `logic-walk`
recurses only on itself via a `subst-lookup` helper returning a `Lookup` ADT.
This was originally forced by the cycle; with the fix a mutually-recursive
`walk` / `walk-var` pair would compile in any order, but the self-recursive form
is at least as clear, so it is left as-is (no code change needed).

---

## GAP 3 -- non-recursive (by-value) `defdata`/`defstruct` cannot erase to/from `:int` -- **MEDIUM** -- FIXED 2026-07-13

**Fixed.** Two parts:
- **Part A (diagnostic).** The direct `::` between a non-parametric by-value
  ADT/struct product and a one-word carrier (`:int` / `:ptr<void>`), in either
  direction, now raises `TUR-E0295` in `elab_ascribe` -- both harnesses report
  the same clear error instead of miscompiling (the `cc` error / segfault /
  garbage handle described below).
- **Part B (the bridge).** The sound way to carry a by-value aggregate through an
  erased handle already existed: `any` + `cast`. `elab_ascribe` now makes
  `(:: v :any)` *heap-box* the value (it previously relabelled it, miscompiling a
  by-value aggregate), giving a clean explicit box spelling; `(cast h T)` reads
  it back by value. No new `box`/`unbox` verbs were minted -- those names are
  already taken by `stdlib/safe.tur`'s int-cell functions.

See [`docs/upcoming/byvalue-adt-int-cast-plan.md`](../upcoming/byvalue-adt-int-cast-plan.md).
Fixtures: `tests/fixtures/errors/byvalue-adt-cast-to-int`,
`.../int-cast-to-byvalue-adt` (Part A), and
`tests/fixtures/box-unbox-byvalue-aggregate` (Part B). Original report follows.

**Summary.** The compiled path represents a non-recursive ADT/struct **by
value** (`tur_adt_P`), so there is no int handle to cast to. `(:: v :int)` and
the reverse `(:: someInt :SomeByValueAdt)` fail -- and fail as a raw `cc` type
error, not a Turmeric diagnostic. A *recursive* ADT is heap-boxed and erases
cleanly, so the fix is often "make the type recursive," which is a
representation hack rather than an intent.

**Minimal repro.**

```turmeric
(defdata P :copy (P :int :int))
(defn main [] : int (println (:: (P 3 4) :int)))
```

`tur run` ->
`error: incompatible types when initializing type 'int64_t' ... using type
'tur_adt_P'` (raw C error from the generated file).

**Root cause (where known).** By-value aggregate representation for
non-recursive ADTs (see the by-value-HKT monomorphization notes in
`src/compiler/elab_call.c`). `::` between a by-value aggregate and `:int` has no
lowering. Same class of issue bites a poly `:fn` result (a boxed `int`) fed into
a by-value struct slot: `(:: (f s) :SomeByValueAdt)` also fails.

Note both directions are unsound *and* the two harnesses disagree: `(:: v :int)`
is a `cc` error compiled but prints a garbage handle under `--interpret`, and the
reverse `(:: h :V)` segfaults compiled but throws "match: no arm matched" under
`--interpret`. Since the elaborator is shared, diagnosing it there fixes both at
once.

**Fix directions.** Planned in
[`docs/upcoming/byvalue-adt-int-cast-plan.md`](../upcoming/byvalue-adt-int-cast-plan.md):
(a) reject the direct `::` between a by-value aggregate and a one-word carrier
with a real Turmeric diagnostic (never leak a `cc` error / silent segfault), then
(b) add an explicit `box`/`unbox` bridge reusing the existing
`emit_agg_box`/`emit_agg_unbox` helpers so erased-carrier plumbing can round-trip
a by-value value without a sham recursive arm.

**Workaround in the tree.** `stdlib/logic.tur`: the fresh-var counter is folded
into the recursive `Subst` base node (`(SNil next)`) instead of a natural
`UState { subst next }` product -- chosen specifically because a non-recursive
`UState` is by-value and breaks the `:int`-carrier goal/stream plumbing. (This
also happens to be a tidy design, but the *reason* is this gap.)

---

## GAP 4 -- callback parameter typing -- **WITHDRAWN 2026-07-13 (not a gap)**

**Original claim.** A parameter declared `:fn` erases the callback's argument
types (a passed lambda's parameters infer as `int`), so `logic.tur` used `:fn`
for `fresh`'s callback and every call site had to annotate its lambda
`(fn [x : Term] ...)`. I framed the "honest" alternative -- a typed
`(fn [Term] (Goal int))` parameter -- as blocked, because such a parameter
segfaulted when handed a *capturing* closure (nested `fresh`).

**Why it's withdrawn.** That segfault was **not** a compiler gap -- it was a
missing `^fat` annotation. A `(fn [T] U)` parameter is thin-dispatched *by
design* (it expects a bare/global fn reference); a parameter that receives a
closure which may capture is declared `^fat`, exactly as the rest of stdlib does
(`^fat cmp-fn : (fn [int int] bool)` in `list.tur`, `^fat f : (fn [A] B)` in
`arrow.tur`/`either.tur`). With `^fat`, the call site boxes the argument and the
callee fat-dispatches through slot 0 -- and the typed parameter also *infers*
the lambda's argument type, so no annotation is needed at the call site:

```turmeric
;; works today (both harnesses); `x` needs no `: Term`, capturing closures OK:
(defn fresh-impl [^fat lf : (fn [Term] (Goal int)) state : Subst] : Stream ...)
(defn fresh      [^fat f  : (fn [Term] (Goal int))] : (Goal int) ...)
(run-logic 1 (fresh (fn [x] (fresh (fn [y] (inner-goal x y))))))
```

I conflated this with GAP 1 (the `::`-cast opaque-handle dispatch, a genuine
bug). They are distinct: GAP 1 needed the `elab_ascribe` fix; this only needed
the annotation that already exists.

**Resolution in the tree.** `stdlib/logic.tur`: `fresh` / `fresh-impl` now take
`^fat f : (fn [Term] (Goal int))` (the intermediate `apply-fat` `:fn` shim is
deleted), and the migrated `logic-fresh` / `logic-reify` fixtures drop their
`(fn [x : Term] ...)` annotations. No `:fn`-as-type-eraser remains for goal
callbacks; the only surviving `:fn` parameters are the genuine typeclass-method
poly continuations of `mbind` / `st-bind` / `bind-goal-raw` / `fmap-goal-raw`,
which is the correct use of `:fn`.

---

## Cleanup plan -- retire the workarounds once the gaps are fixed

Do these opportunistically as each gap lands; none is a prerequisite for the
others except where noted. The end state is a `logic.tur` whose goal/stream
plumbing is typed end-to-end with no `:int` type-erasure.

- [x] **GAP 1 fixed** (done 2026-07-13) -> `apply-goal` retyped
      `[g : (Goal int) state : Subst] : Stream`; the `(:: g :int)` erasures are
      gone from `conjoined`, `disjoined`, `run-logic`, `bind-goal-raw`,
      `fmap-goal-raw`, and the `Functor`/`Applicative`/`Monad`/`Alternative`
      instances (now `(:: ... (Goal int))`). Both harnesses green; no thin-call
      segfault.
- [x] **GAP 4 withdrawn** (done 2026-07-13) -> `fresh` / `fresh-impl` now take
      `^fat f : (fn [Term] (Goal int))` (the `apply-fat` `:fn` shim is deleted),
      and the `logic-fresh` / `logic-reify` fixtures drop the `(fn [x : Term] ...)`
      annotation. No compiler change was needed -- `^fat` is the intended idiom
      for a capturing-closure parameter.
- [ ] **GAP 3 fixed** -> optionally reintroduce an explicit
      `UState { subst next }` product if it reads more clearly than the
      counter-in-`SNil` encoding (the current encoding stays correct either way;
      this is a readability call, not a correctness fix).
- [x] **GAP 2 fixed** (done 2026-07-13) -> compiler now forward-declares a bare
      ADT return type. `logic-walk`/`subst-lookup` are left self-recursive (that
      form is at least as clear); no `logic.tur` change was needed.
- [ ] After any of the above, regenerate fixture snapshots / docstrings in the
      same change and re-run `bash tests/run.sh` + `bash tests/run-turi.sh`.

Remaining workaround anchor: the counter-in-`SNil` encoding (GAP 3) is the only
representation shaped around an open gap; the goal-handle `(Goal int)` typing and
the `^fat` callbacks are now the honest forms. GAP 2 only affected the shape of
`logic-walk`/`subst-lookup` (self-recursive rather than mutually recursive).

---

## See Also

- `stdlib/logic.tur` (the workarounds live in the "Goal application" through
  "HKT typeclass instances" sections).
- `docs/archive/history/logic-pure-turmeric-port-plan.md` -- the port plan, whose
  "Implementation notes" section records the same deviations from the caller's
  perspective.
- `src/compiler/elab_types.c` (`elab_ascribe`, `ascribe_type_is_opaque_handle`)
  -- where the GAP 1 fix landed (producer + consumer fat-boxing across an opaque
  carrier). `src/compiler/elab_call.c:984` (`elab_call_head_expr`) is the
  sibling call-head dispatch that already handled the `:ptr<void>`/carrier cases.
- `tests/fixtures/opaque-fn-carrier-dispatch/` -- GAP 1 regression fixture.
- `src/compiler/elab_toplevel.c` (`elaborate_program` Pass-1 forward-decl loop)
  -- GAP 2 fix site; `tests/fixtures/forward-ref-adt-return/` -- its regression.
