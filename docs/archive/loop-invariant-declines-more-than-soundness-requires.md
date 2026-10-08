# `:invariant` declines two shapes more broadly than soundness requires

**RESOLVED 2026-10-03.** Both halves, each narrowed to what its reason
actually covers:

- **Half 1 (nested loop).** `li_compose` no longer declines on reaching a
  nested `while`. `li_compose_nested_loop` collects the names the nested loop
  assigns and sets each one's image to a havoc sentinel on every path. A later
  READ of a havocked name -- by a statement after it, or by the invariant at
  the end of the body -- is caught in `li_subst`, which declines naming it
  ("'j' is assigned by a nested loop and read after it"). In the invariant,
  only that conjunct's preservation goes unproved. So the inner-counter shape
  proves all four obligations. An outer name the nested loop assigns costs only
  the conjuncts that mention it. A nested loop that can `return`, mutates a
  cell, or writes through a place still declines, as does a name it assigns
  that is neither an outer local nor bound inside it.
- **Half 2 (early `return`).** `return` is now the one exit the composer
  models. `li_early_exit` takes a mode, so `?`, continuations and the rest still
  decline as before. A `(return v)` statement empties the path set: the paths
  through it leave the function and owe no re-establishment. Initiation and
  preservation are decided on what remains. The site records
  `early_return`, and the post-loop fact is withheld from everything after the
  loop: `li_proven` now requires `!early_return`, while the in-loop use goes
  through the new `li_inductive`. Pruning matters for correctness, not only
  precision. `early-bound` in the new fixture would be refuted if the
  `return` path fell through (i = 100, then i + 1 > 100).

Pinned by:

- `tests/fixtures/loop-invariant-nested-and-early-return`: 10 of 10 proved,
  no loop check emitted, under `--strict-refine`.
- `tests/fixtures/errors/loop-invariant-early-return-refuted`: a body that
  really breaks the invariant beside an early `return` is now `TUR-E0371`
  rather than a silent decline.
- `tests/fixtures/loop-invariant-declines`: its `early` case moved to a
  `return` inside a nested loop, which still declines. It gained the
  nested-loop decline path the filing noted had no test (`nested-read`).

While testing half 2, an unrelated and more serious gap turned up: an early
`return` bypasses a function's return refinement and `:post` entirely, both
statically and at runtime. It is not loop-specific. Filed as
[early-return-bypasses-return-refinement](early-return-bypasses-return-refinement.md).

The original filing follows.


**Severity: low (completeness, not soundness -- both runtime checks are kept,
so the program is correct, just unverified).** The `loop-invariants`
experiment's decline list is deliberately conservative, and the posture is a
tested property (`tests/fixtures/loop-invariant-declines`). Two of its
declines are broader than the reason behind them: in both, obligations that do
not depend on the declined channel are abandoned along with the one that does.
Filed 2026-10-02, found reviewing the `loop-invariants` row against its
`expires_at` at the v0.59.0 cut. Both halves are listed under "Left open" in
[loop-invariants-plan](loop-invariants-plan.md); this report is the
measured version with repros.

Run every repro below with `--enable=loop-invariants`.

## Half 1 -- a nested loop assigning an inner-local declines the outer loop

```turmeric
(defn nested [n : int] : int
  (let [^mut acc 0
        ^mut i   0]
    (while (< i n) :invariant (and (>= acc 0) (>= i 0))
      (let [^mut j 0]
        (while (< j 2)
          (set! j (+ j 1))))
      (set! acc (+ acc 1))
      (set! i   (+ i 1)))
    acc))
```

```sh
TUR_REFINE_STATS=1 ./build/tur --enable=loop-invariants check repro.tur
```

Observed:

```
warning [TUR-W0372]: the invariant of the while loop in 'nested' is not
analysed statically: the loop body contains a nested loop that assigns;
both runtime checks kept
refine: invariant: 4 proven, 0 unproven, 1 loop(s) declined
```

`j` is bound by the inner `let`, assigned only by the inner loop, unreadable
after it, and named by neither the outer invariant nor the outer condition. It
cannot affect `acc` or `i`. Delete the inner loop (or make it non-assigning --
a `println` body) and the identical outer loop proves all four obligations.

This is the common nested-iteration shape: any `while` over a 2D index, or any
inner accumulation loop, declines the outer invariant.

## Half 2 -- an early `return` declines initiation and preservation too

```turmeric
(defn early [n : int] : int
  (let [^mut acc 0
        ^mut i   0]
    (while (< i n) :invariant (and (>= acc 0) (>= i 0))
      (when (> i 100) (return -1))
      (set! acc (+ acc 1))
      (set! i   (+ i 1)))
    acc))
```

Observed: `the loop can leave early through 'return'; both runtime checks
kept`, and the loop contributes `0 proven`. The identical loop without the
`when` proves 4. `emit-c` carries both the entry check and the
re-establishment check for `early` and neither for the control.

The reason given is sound for **obligation 3 only**. On a `return` path the
loop condition may still hold, so the post-loop fact `p AND (not c)` is not
available -- correct. But obligation 1 (initiation: `p` holds on entry) and
obligation 2 (preservation: the body re-establishes `p`) say nothing about how
the loop exits. Both are provable here, and both are discarded.

## Root cause

- **Half 1:** `src/compiler/elab_fns.c:5169` -- `li_compose`'s statement walk
  declines on any `while` head it reaches. It is reached only for statements
  that assign, because `elab_fns.c:5057` returns early for a statement with no
  `set!` anywhere in it (`rt_form_mentions_set`). So the trigger is "the inner
  loop assigns *something*", with no test of whether what it assigns is
  visible to the outer loop's invariant, condition, or post-loop use. The
  decline is correct for an inner loop that assigns an OUTER name; it fires
  identically when every assigned name is inner-local.
- **Half 2:** `src/compiler/elab_fns.c:5412` builds the reason string, and the
  decline is taken for the whole `LoopInvSite` rather than being recorded as
  "obligation 3 unavailable". The three obligations are generated together
  from one site verdict, so there is no path today that keeps 1 and 2 while
  dropping 3.

## Fix directions

**Half 1.** Before declining, ask whether the nested loop's assigned-name set
intersects the names the outer analysis cares about -- the outer invariant's
free names, the outer condition's, and anything live after the outer loop. The
walk that answers this already exists: `li_collect_set_targets` /
`rt_collect_set_targets` produce exactly that set, and `li_walk` already
reasons about a name being assigned by code at a position. If the
intersection is empty, treat the nested loop the way a non-assigning statement
is treated at `elab_fns.c:5057` -- skip it. If it is non-empty, decline as
today and say which name forced it (the current message names none, which is
the first thing a user hits).

A cheaper first cut, if the liveness half is awkward: skip a nested loop all
of whose assigned names are bound by a `let` *inside* the outer loop body.
That covers the `(let [^mut j 0] (while ...))` shape above without needing
liveness at all, and it is the shape that actually occurs.

**Half 2.** Split the site verdict so a declined channel drops only the
obligations that rest on it. Keep initiation and preservation, drop the
post-loop fact, and reword the message to say so ("the post-loop fact is not
available: the loop can leave early through `return`; the entry and
re-establishment checks are still proved"). The runtime consequence is
visible and good: `early`'s two runtime checks become zero, and only a
post-loop return refinement keeps its check.

Note this interacts with `li_prove_paths_ext`'s havoc rename (LI3 step 2):
with obligation 3 dropped there is no `p AND (not c)` to assume, so the
havoc still has to run for the assigned names -- the post-loop environment
must not inherit the pre-loop equations either way.

## Fixture gaps found while filing

`tests/fixtures/loop-invariant-declines` covers six declines -- place write,
early `return`, `&mut` borrow, lambda cell, handler clause, shadowing `let`.
It does **not** cover the nested-loop decline at `elab_fns.c:5169`, which is
therefore an implemented, reachable decline path with no test. Whichever way
half 1 is resolved, that case belongs in the fixture (as a decline today, or
as a proof plus a genuinely-outer-assigning decline afterwards).
