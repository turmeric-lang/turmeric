---
title: Stateful Refinements
category: Type Safety
description: Refining predicates over mutable state -- the `frozen` region and the `#reads w` annotation that grants congruence, why a trusted annotation is sound here, and how the guarded crossings codegen
---

# Stateful Refinements -- frozen regions and `#reads`

> **Status: shipping, unconditional.** The `frozen` region form ships today in
> the `tur-ecs` spice (`ecs/freeze`). The `#reads` annotation and the congruence
> grant it enables are **on in every build** -- there is no flag to set. (A
> lingering `--enable=refined` was a no-op through 0.37.0 and is a hard
> `TUR-E0310` from 0.38.0; delete it.) The design
> of record is
> [`docs/archive/refine-stateful-measures-plan.md`](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/refine-stateful-measures-plan.md).
> A `#reads`-refined accessor proves its guarded crossings *and* codegens
> (see [Codegen and enforcement](#codegen-and-enforcement) -- the impure
> entry contract is suppressed). Read the
> [Refinement Types guide](refinement-types-guide.md) first; this one assumes it.

## The gap this fills

A refinement predicate is only useful if the solver can reason about the terms
in it, and the load-bearing move is **congruence**: treating two occurrences of
`(alive? w e)` -- one in a guard, one in a crossing -- as the *same value*, so a
guard discharges the read it protects.

The Refinement Types guide states the rule that blocks this for mutable things:
[*"a measure must be provably pure"*](refinement-types-guide.md). A predicate
about mutable state is not pure -- `alive?` reads a generation counter, `open?`
reads a socket's state, `in-bounds?` reads a buffer's current length -- so each
occurrence gets a *distinct* opaque symbol, and:

```turmeric
(if (alive? w e)
  (get-Pos! w e)      ;; the crossing does NOT discharge -- `0 proven, 1 unknown`
  (handle-dead))
```

does not prove. And that is *correct* by default: with a `tick` that counts up,
`(- (tick) (tick))` is `-1`, not `0`, and the same reasoning applies to a world
despawned between the guard and the read. Congruence on an impure measure is a
miscompile, and the refinement design refuses it.

Stateful refinements recover the guard-discharges-the-read pattern for mutable
state **without** loosening that rule, using two pieces:

1. a **region** in which the state provably cannot change, and
2. a **declaration** of which state a measure reads,

so that *inside the region* the measure is a function of frozen state and its
pure arguments -- and therefore congruent *there*, and only there.

## Piece 1 -- the `frozen` region (ships today)

`(frozen w body...)` runs `body` while the owned value `w` is **borrowed** for
the region's extent. It lowers to an ordinary borrow held across a `let`:

```turmeric
(frozen w
  (read-only-pass w))     ;; ==  (let [_ (& w)] (read-only-pass w))
```

The point is what the borrow forbids. Declare the mutator that could change the
state to require **exclusive** access:

```turmeric
(defn despawn! [^unique ^mut w : World e : Entity] : nil ...)
```

Inside `(frozen w ...)` a borrow of `w` is live, so passing `w` as `^unique ^mut`
is rejected -- `TUR-E0200: cannot pass 'w' as ^unique ^mut -- active borrow
exists`. A despawn of `w` is a **compile error** in the region, not a runtime
check.

This is sound by construction, and needs no new machinery:

- The authority to mutate `w` is `w`'s own exclusive-mutable access. It cannot be
  forged from a shared borrow, so there is no capability to mint, hoard, or alias
  -- the failure mode a capability token would have. That holds for a `^borrow`
  **parameter** too, not only for an in-frame `(& w)`: passing one on as
  `^unique ^mut` is the same `TUR-E0200`. It is worth stating because the two
  arrive by different routes -- an in-frame borrow is visible to the frame, while
  a `^borrow` parameter's aliasing happened in the caller -- and for a while only
  the first was checked (see
  [docs/archive/borrow-param-passed-as-unique-mut-undiagnosed.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/borrow-param-passed-as-unique-mut-undiagnosed.md)).
- `frozen` *borrows*, it does not consume: `w` is usable again after the region,
  so a real `despawn!` outside the region is fine.
- Read-only accessors take `[^borrow w]` and coexist with the region borrow, so
  reads stay callable inside; only the exclusive mutator is locked out.

Requirements: the region body is a plain `let` body -- any result type, and
elaborated *inline* (not a closure), which is what lets the refinement encoder
see it.

`w` does **not** have to be an owned `^mut` local. A `^borrow` parameter
registers the region borrow just as well, on both paths: the crossing
discharges inside `(frozen w ...)` and falls back to `TUR-W0372` without it,
and a `despawn!` needing `^unique ^mut w` inside the region is still
`TUR-E0200`. So a helper that takes `[^borrow w : World]` and opens a region on
its parameter works, which is what makes regions usable below the frame that
owns the world.

> `frozen` is exported by `ecs/freeze` in `tur-ecs`. It is world-agnostic -- the
> same shape freezes a file handle, a buffer, a lock, or a transaction, given a
> mutator declared `^unique ^mut`.

## Piece 2 -- `#reads w`

The region proves the state is frozen, but the encoder still does not know that
`alive?` *depends on* that state -- `alive?`'s body is inline C, opaque to the
purity walk, so without `#reads` it is simply "impure" and gets a fresh symbol
per occurrence, region or no region (a bare impure guard inside `frozen` still
reports `0 proven, 1 unknown`).

`#reads w` supplies the missing fact. It annotates a measure with the borrowed
argument whose mutable state it reads:

```turmeric
(defn alive? [^borrow w : World e : Entity] #reads w : bool
  ;; body reads gens[idx] out of w's control block through inline C,
  ;; which is why the purity walk cannot see the dependence on w.
  ...)
```

`#reads w` names a parameter whose mutable state the body reads. A `^borrow`
parameter is the shape the annotation was designed around, and the one every
example here uses -- but it is **not** a requirement: a BY-VALUE parameter is
accepted too, and `stdlib/vec.tur`'s `vec-len` / `vec-get` rely on that, since
a container handle is passed by value. A generic (`[A]`) measure is accepted as
well. In both cases the frame is live: the measure passes the contract-position
gate and takes part in the congruence grant exactly as a `^borrow` one does.

A measure that reads more than one borrowed state names them all in one frame,
the same way `#writes` does:

```turmeric
(defn linked? [^borrow w : World ^borrow g : Grid e : Entity] #reads [w g] : bool
  ...)
```

The grant over such a frame is **conjunctive**: it applies only when *every*
named parameter is frozen at the call site. One unfrozen parameter is enough
for the measure to differ between two occurrences -- which is exactly the
crossing the grant elides -- so "any frozen" would be unsound. Freezing `w` but
not `g` above leaves `linked?` fresh-per-occurrence, and the crossing reports
`unknown` as it should.

Write one frame, not several: `#reads w #reads g` is `TUR-E0024`, as is an
empty `#reads []`. (`#writes []` *is* allowed, because "writes nothing" is a
real claim; "reads nothing" is just the absence of the annotation.)

With it, the encoder's congruence rule gains one arm:

- a call `(alive? w e)` whose callee declares `#reads w` is given a **stable**
  (congruent) symbol **when a live borrow of every named parameter is in
  scope** -- i.e. inside `(frozen w ...)`;
- and a **fresh** symbol everywhere else, exactly as an impure measure gets
  today.

Region exit, a `set!` of `w`, and the `do`-split rule all invalidate the
hypothesis -- the same three invalidation sites, sharing one predicate. So:

```turmeric
(frozen w
  (if (alive? w e)
    (get-Pos! w e)      ;; discharges: alive?(w,e) is congruent in the region
    (handle-dead)))
```

proves, and the same code *outside* a `frozen w` does not.

A measure written in **pure Turmeric** -- reading `w` through ordinary field
accesses, no inline C -- needs no `#reads`: it is already congruent by the
existing rule ("a field read is as pure as its receiver"). `#reads` is only for
measures whose dependence on the state is *invisible* to the purity walk, which
in practice means an inline-C or otherwise-opaque body.

## Why a *trusted* annotation is sound here

`#reads w` is a **promise, not a checked fact** -- the compiler cannot look into
an inline-C body to confirm the measure reads *only* `w`'s state and nothing
else. The Refinement Types guide is emphatic that a trusted purity attribute is
[forbidden](refinement-types-guide.md), for a precise reason: *"the cost of a
wrong purity claim is an elided check."*

That reason does not apply here, because the check that matters is **not
elided**. The guide's own limit spells it out:

> **[by design] A callee's entry check is never elided.** The call-site layer
> reports, it does not remove the callee's guard.

A guarded stateful read has *two* checks:

| check | who | elided by a proof? |
|---|---|---|
| the **crossing** at `(get-Pos! w e)` -- verifies `(alive? w e)` at the call site | the caller | **yes** |
| `get-Pos!`'s **own internal check** -- the `gens[idx]` compare it writes in its body before it reads | the callee | **no, ever** |

A congruence proof from `#reads` + `frozen` elides only the *crossing* check.
So a **wrong** `#reads w` -- a measure that secretly reads other mutable state
-- costs a **missed compile-time lint**: an unguarded read that should have
warned `TUR-W0372` compiles clean. It can **never** cause a use-after-free: the
callee's own internal check still runs and aborts on a dead handle at runtime.

> **The backstop is the accessor's *own* check, not an auto-generated contract.**
> Read the second row carefully: it is `get-Pos!`'s hand-written `gens[idx]`
> aliveness compare -- ordinary code in the body -- **not** a runtime contract
> synthesized from the `#refine{ x | (alive? w x) }` parameter. The compiler
> **suppresses** that entry contract for a `#reads` parameter, because the
> crossing is the enforcement point (see [Codegen and
> enforcement](#codegen-and-enforcement)). So a `#reads`-refined accessor gets
> its safety backstop **only** from the check it writes itself. A minimal
> accessor whose body is `(.n w)` with no internal guard -- like the
> `refine-stateful-guard-discharges` test fixture -- demonstrates the
> *congruence proof*, not a runtime safety net; a real accessor keeps its own
> bounds/aliveness check.

That is exactly the asymmetry the refinement design is built on -- *the cost of a
wrong claim is a **kept** check, not an elided one* -- reached by declining to
elide the safety check rather than by refusing the declaration. `#reads` buys a
**stronger compile-time signal** (the guard is now provable), never a weaker
runtime guarantee.

The corollary is a rule of thumb: **do not use `#reads` to elide the safety
check itself.** Its whole soundness argument is that the kept entry check is the
backstop. A design that elided the entry check on the strength of a `#reads`
proof would be unsound, and is out of scope by construction.

## Codegen and enforcement

Two consequences of the measure being impure shape how a `#reads`-refined
function compiles and how its crossings are enforced.

**The entry contract is suppressed.** An ordinary `#refine{ x | p }` parameter
injects a runtime *entry contract* -- a `(tur-contract-check p ...)` at the top
of the callee. For a `#reads`-measure predicate the injector **skips** it
(`rt_pred_reads_measure` in `src/compiler/elab_fns.c`): the crossing is where a
`#reads` refinement is enforced, and the safety backstop is the accessor's own
internal check (previous section).

**The purity gate accepts a `#reads` measure.** A contract predicate with side
effects is a hard error (`TUR-E0375`: "contract predicate has side effects;
predicates must be pure"), because whether the check is compiled in becomes
observable. A `#reads` measure's body is inline C, so the purity walk alone
would call it impure. But `#reads` is the claim that it only *reads*, and a read
is not observable however often the check runs. So the gate
(`rt_pred_observably_impure`) does not count a call the predicate makes directly
to a `#reads` measure. Its arguments are still walked, an impure term beside it
is still `TUR-E0375`, and so is an unannotated function that wraps one. The
other contract positions -- `:pre`, `:post`, a refined return, and a loop's
`:invariant` -- therefore accept a `#reads` measure as an ordinary **runtime**
check. Before 2026-10-03 all four were `TUR-E0375`
([reads-measure-rejected-in-invariant-and-pre](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/reads-measure-rejected-in-invariant-and-pre.md)).

**A bounded-index loop invariant is PROVED inside a frozen region.** An
invariant such as `:invariant (and (>= i 0) (<= i (vlen v)))` over a frozen `v`
discharges on entry and through the body, and both runtime checks are then
elided. It used to be **declined whole** -- the region's own `(& v)` made `v`
volatile.

What makes the bound provable is that two occurrences of `(vlen v)` denote the
same value, and the evidence for that is **not** the borrow. A container's
mutators take it BY VALUE -- `vec-push!` is `[v : (Vec A) val : A]`, `#fx{}` --
so a region does not stop a callee growing it and no borrow conflict is
reported. The evidence is a **write promise** on everything the loop hands `v`
to, and there are two kinds:

- `#reads v`, which `stdlib/vec.tur`'s `vec-len` and `vec-get` carry. The
  mutators carry `#writes [v]` instead: that asymmetry is what distinguishes a
  reader from a writer, because the signatures cannot.
- a callee the compiler knows is **pure**, which writes nothing by definition
  -- provided its result is a scalar or is itself only read. A pure function
  that RETURNS `v` hands back a handle to write through, so
  `(vec-push! (id v) 7)` withholds the grant.

Hand `v` to anything else inside the loop -- a mutator, or an unannotated
inline-C callee -- and the grant is withheld for that loop. The bound then
falls back to "proved on entry, not preserved", its re-establishment check
stays, and a body that really does grow `v` dies on that check instead of
compiling to a false proof.

Two further edges:

- **Only the marker shape is exempt from the volatile set** -- a shared borrow
  bound to a name nothing mentions. Name that borrow and use it, or take a
  borrow inside the loop, and the loop is declined again (`TUR-W0372`), because
  a shared borrow handed to an **inline-C callee does get written through**.
- **A `#reads` measure written in Turmeric is checked for writes; one written
  in inline C is trusted.** At its definition, every use of a framed parameter
  in the body must be a read. A store through it, or handing it to a callee
  whose `#writes` frame names it, is `TUR-W0383` and refuses every grant. A use
  the walk cannot vouch for -- binding it to a local, returning it, passing it
  to an unannotated function -- is silent, but that measure no longer backs
  this grant. An inline-C body cannot be seen into, so its `#reads` stays a
  trusted claim; the stdlib's are truthful.

The measure's own refined return is a separate obligation, and stays runtime
checked while its body is inline C (`(>= r 0)` is not provable from
`vec-len`).

**Enforcement of the crossing lives at compile time, under `--strict-refine`.**
Because there is no runtime contract for a `#reads` crossing, an *unproven* one
cannot fall back to a runtime check the way a pure refinement does. What happens
instead depends on the mode:

| mode | unproven `#reads` crossing |
|---|---|
| `--strict-refine` | **hard error** `TUR-W0372` -- the read must be provably guarded (this is the mode `tur-ecs` and any safety-critical use should compile under) |
| default (non-strict) | **warning** `TUR-W0372`: "no runtime fallback for an impure `#reads` measure -- the crossing must be proven (guard it inside a `frozen` region)". The crossing is trusted (elided), but you are told -- it is not silent |

Both message variants say *no runtime fallback*, not "runtime check kept": a
`#reads` crossing has no runtime contract to keep (unlike a pure refinement,
which falls back to one when unproven). The distinction from the pure path is
exactly this -- there is no safe fallback, so an unproven `#reads` crossing is
always surfaced, never silently trusted.

The practical rule: **compile `#reads`-bearing code under `--strict-refine`.**
There the guarantee is real -- an unguarded stateful read is a compile error, not
a trusted (warned) elision. Non-strict downgrades it to a warning so stateful
refinements stay incrementally adoptable, but the accessor's own internal check
remains the runtime backstop either way.

> **On testing `#reads` soundness.** Because a `#reads` crossing carries no
> runtime contract, the refinement *source fuzzer* (`tests/refine-fuzz-src.py`,
> whose `stateful` shape generates exactly this code) cannot exercise `#reads`
> *soundness*: its differential is "a check that fired with the gate off must not
> vanish with it on", and there is no gate-off check here to vanish. The fuzzer's
> `stateful` shape is **correctness** coverage (a codegen divergence or crash is
> caught); soundness is pinned by the `errors/refine-stateful-*` fixtures under
> `--strict-refine` plus a frozen-check sabotage, both compile-time. This is a
> structural property of a trusted, impure measure, not a coverage hole.

## Macros compose

Both halves of the pattern survive being wrapped in -- or generated by -- a
macro, which is what makes an ergonomic iteration surface possible:

- a macro that **splices the user's forms** (`ecs/freeze`'s `frozen` is
  `~@body`) discharges because the crossing keeps its source span
  (`rt_form_ident`);
- a macro whose quasiquote template **generates** the guard and/or the
  refined read discharges because the crossing path walk traverses macro
  expansions (`refine_note_macro_expansion`) -- the walk sees the code that
  actually elaborated, including a template-hidden `set!`, which declines
  exactly like a written one.

The shipped consumer is `ecs/refined-world`'s `for-each-alive!`: one macro
generates the tail-recursive loop, the frozen re-borrow, and the aliveness
guard, and the user's refined read -- spliced as the body -- discharges
per-entity. One composition rule to know: a macro that splices the SAME user
form twice makes the crossing's path ambiguous, and the collector declines
(conservative, never unsound).

## Where this generalizes

`#reads` is narrower than it looks in one direction and broader in another, and
both are worth knowing.

**Broader: it is not an ECS feature.** Aliveness is one instance of "a predicate
about a mutable resource, congruent in a scope where that resource is frozen."
The same `frozen` + `#reads` pair covers an open file (`(open? conn)`), a
resizable buffer (`(in-bounds? buf i)` -- the bounds-elimination case
[`loop-invariants-plan`](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/loop-invariants-plan.md) wants;
pinned by
`tests/fixtures/refine-stateful-resizable-bounds`: the guard proves inside the
region, `grow!` is `TUR-E0200` there, and without the region the read is
`TUR-W0372`), a held lock, a session in a state, a row inside a transaction. `ecs/freeze` lives in
the ECS spice only because the ECS is the first program that demanded it.

The concept has a well-established name outside Turmeric: a **`reads` clause**,
as in Dafny's `reads`/`modifies` frames or separation logic's read/write
footprints -- the standard way heap-aware verification states what a function may
touch. `#reads w` is a coarse (per-argument, not per-heap-location) `reads`
clause.

**Narrower: the *trusted* form is refinement-only.** The same read-frame
information would enable common-subexpression elimination, safe parallelization
(two operations with disjoint read/write frames may run concurrently -- which is
what `tur-ecs`'s `WriteCap`/`ReadCap` scheduler already does by hand, per
component), and reactive/incremental recomputation. But every one of those
*elides or reorders real work* on the strength of the claim, so they would need
`#reads` to be **checked**, not trusted -- and checking an inline-C body is the
wall this trusted form steps around. The trusted variant is sound precisely
because it changes nothing at runtime.

### The intended trajectory

`#reads` is introduced deliberately as the **minimal, trusted, refinement-only**
slice, shaped so a stronger version can grow from it without a rename or a
semantics break:

1. **trusted now** -- a promise, sound because the safety check is kept
   (this guide);
2. **checkable later** -- verified by the purity walk once a measure's state is
   Turmeric-visible (struct fields rather than an inline-C handle), at which
   point the annotation is a checked fact and may guard reordering/CSE;
3. **effect-row eventually** -- `#reads`/`#writes` as a real read/write effect
   row (the one `#fx{}` never was -- it "tracks algebraic effects and infers
   nothing from `set!`, a mutable global, or inline C"), subsuming the ECS's
   hand-rolled cap-based conflict detection and feeding the loop-invariant
   bounds work.

Treat today's `#reads` as step 1 of that path, not as a finished `reads`-clause
feature. **Step 2 has landed** (as the `write-frames` experiment, graduated in
0.37.0) -- `#writes w` / `#writes [a b]` declares which arguments a body may
write, and a frame on a body with no inline C is *checked* rather than
believed. See
[`checked-write-frames-plan.md`](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/checked-write-frames-plan.md).

`#reads` itself is unchanged by it: still trusted where it cannot be checked,
still refinement-only, still step 1. What a checked `#writes` frame buys today is on the
*invalidation* side -- a borrowed local no longer forces every hypothesis in
the body to be dropped when the borrow provably reaches nothing that writes.

It does **not** buy elision, and the row in the quick reference below still
says so. The plan originally proposed one (WF4) on the strength of this
guide's "the accessor's own internal check is the backstop" line; that check
is the author's own code inside an inline-C body, which no frame can license
removing -- and inline-C is exactly what the checked tier cannot see into.
WF4 is retired.

### A frame says nothing about globals

A `#writes` frame's vocabulary is **parameters**. A mutable global (`def ^mut`)
is written by name rather than passed, so a frame can neither name it nor
exclude it -- `#writes []` means "writes none of my arguments", not "writes no
storage anywhere".

Because a *checked* frame is a fact an optimization may act on, a body that
writes a global is therefore **never VERIFIED**: the verdict downgrades to
UNVERIFIED, silently. No diagnostic, because a global write is outside the
frame's vocabulary rather than outside the declared frame, and "I cannot check
this" is not "you did something wrong". The declaration still documents intent;
nothing optimizes on it.

The fact propagates through callees, including callees that receive none of
your parameters -- a call with no arguments at all can still write a global.
`--dump-write-frames` prints the verdict and the global answer as separate
columns:

```
write-frame sneaky: UNVERIFIED mask=0x0 frame=VERIFIED global=YES
```

`frame=VERIFIED global=YES` reads as "the frame itself holds, but the body
writes global state, so the frame is not a fact you may build on."

### Naming a global in the frame

A `#writes` frame **may name a mutable global**, so a body that legitimately
maintains global state can carry a checked frame instead of being declined
outright:

```turmeric
(def ^mut hits 0)

(defn bump! [] #writes [hits] : void
  (set! hits (+ hits 1)))
```

The question then becomes coverage, exactly as for parameters:

| Body | Verdict |
|---|---|
| writes only globals the frame names | VERIFIED |
| writes a global the frame does not name | `TUR-E0382`, naming the global |
| the walk cannot tell | UNVERIFIED |

Declared-but-never-written is fine -- a frame is an *upper bound* on what the
body may write, the same reading `#writes [a]` already has for a parameter the
body happens not to touch. A frame may mix the two: `#writes [a hits]`.

Naming an **immutable** global gets its own rejection rather than a generic
"not a parameter" one: it is a claim that cannot be true (`TUR-E0381`), and
saying so beats a message about parameters.

`#reads` is deliberately **not** part of this. It is the annotation that
*grants* congruence, so letting it name a global would let a promise about
mutable global state pay out in proofs -- see
[`mutable-globals-plan.md`](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/mutable-globals-plan.md)
sections 12.2 and 12.4, and the `refine-reads-frame-omits-global` fixture pair
that pins what a broken read-side promise costs.

The compiler does now **tell you** when that promise is demonstrably broken,
in either of the two ways it can break: a `#reads` measure whose body
directly reads a mutable global, or one whose body reads mutable state
rooted in a **parameter the frame omits** (a raw `array-get-unchecked` /
`ptr-deref` load, or a field read through a reference-typed receiver, whose
pointer chases back to an unframed parameter -- including inside the
`(unsafe ...)` block those loads require). Both draw `TUR-W0383` at the
definition ("`#reads w` omits mutable state the body reads"); the
omitted-parameter wording tells you the fix is `#reads [w g]`, after which
the grant requires *both* frozen at the site. Only a *demonstrable* read
warns: an inline-C body yields no evidence and stays silent (so every measure
from before mutable globals existed is unaffected), and a read behind a call
the walk cannot follow stays silent too. `tur --explain TUR-W0383` has the
full story.

Since 0.37.0 (the `checked-reads` experiment, graduated) that same evidence
also **refuses the override**: the measure encodes fresh-per-occurrence like
any unframed impure callee, the crossing that used to be proved from the
broken promise becomes `TUR-W0372` (with wording that says the frame failed,
not the region -- the "guard it inside a `frozen` region" advice would be
misleading when the region is present), and `--strict-refine` makes it a hard
error. Refusal keys on "saw a read", never "could not see", so an inline-C
measure keeps the trusted grant.

Note what refusing does **not** do. A `#reads` crossing is proof-only by
design -- there is no runtime fallback to fall back to -- so refusing buys a
diagnostic, not a check. Outside `--strict-refine` the program still compiles
and still runs the unearned crossing; what changed is that the compiler now
says so rather than staying silent. Fix the frame. See
[`trusted-refinement-claims-plan.md`](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/trusted-refinement-claims-plan.md)
(R2 for the mutable-global evidence, R4 slice 1 for the omitted-parameter
evidence -- `errors/r4-checked-reads-refuses-param-read` pins the refusal and
`refine-reads-multi-param-visible-quiet` the fixed frame, while
`refine-reads-frame-omits-global` pins the non-strict "warns, still runs"
half).

There is also a **verification** tier now, layered on the evidence tier:
a deferred footprint walk runs over each `#reads` function's
elaborated body, and `--dump-read-frames` (a diagnostic knob -- it reports
what the pass decided and changes nothing) prints one of three verdicts per
frame:

- `VERIFIED` -- the walk saw the whole body and every read of mutable state
  attributes to a frame-named parameter. This includes reads made *through*
  another verified frame: a call whose framed callee slot is fed by your own
  framed parameter is covered (the pass iterates to a fixed point, so
  definition order does not matter).
- `EXCEEDED` -- a read the frame omits, possibly reached through a verified
  callee's frame. That last shape is one the definition-site evidence scan
  cannot see (it is behind a call), so the verification pass feeds it into
  the same evidence tier itself: the finding draws `TUR-W0383` (with wording
  that names the callee-frame route) and refuses the congruence grant exactly
  like the direct findings. One finding, one warning, whichever tier saw it
  first.
- `UNVERIFIED` -- something could not be vouched for: an inline-C body, an
  indirect or unframed call, an unmodeled form. Not a finding -- the frame
  simply stays at the trusted tier it is at today.

A `VERIFIED` frame is a stamped, dumpable fact (the read-side mirror of
`#writes`'s checked bit); nothing consumes it behaviorally yet. What it
should eventually buy -- CSE, safe parallelization, incremental recompute --
is recorded in the plan, deliberately deferred until the measure layer the
verdict would speak about actually exists.

## Quick reference

| you have | you want | use |
|---|---|---|
| a pure-Turmeric predicate over struct fields | congruence | nothing -- already congruent |
| an impure (inline-C) predicate over a mutable value | congruence *in a scope where it can't change* | `frozen` + `#reads` |
| to stop a mutation for a scope | a compile error on the mutator | declare the mutator `^unique ^mut`; wrap the scope in `frozen` |
| to elide the *safety* check on a stateful read | -- | not supported, by design; the accessor's own internal check is the backstop |

## See also

- [Refinement Types guide](refinement-types-guide.md) -- the base feature;
  "a measure must be provably pure" and "a callee's entry check is never elided"
  are the two rules this guide builds on.
- [Substructural Types guide](substructural-types-guide.md) -- `^borrow`,
  `^unique ^mut`, and the `TUR-E0200` exclusive-access rule the `frozen` region
  relies on.
- [Uniqueness Types guide](uniqueness-types-guide.md) -- `^unique` semantics.
- [`refine-stateful-measures-plan.md`](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/refine-stateful-measures-plan.md)
  -- the design record, including why the capability-token approach was retired
  in favour of `frozen` and why `#reads` is trusted.
- [ECS guide](ecs-guide.md) -- the first consumer; `tur-ecs`'s `ecs/freeze`
  ships the `frozen` region, and `ecs/refined-world` ships the
  aliveness-refined accessor family plus `for-each-alive!` built on this
  guide's pattern.
