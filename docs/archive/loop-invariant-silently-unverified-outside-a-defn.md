# `:invariant` is silently unverified outside a `defn`

**RESOLVED 2026-10-03.** Both forms now get a verdict:

- **`definstance` methods are analysed** (direction 2). The instance-method
  path in `elab_typeclasses.c` records `li_start` before elaborating the body,
  then calls `li_analyze_method_loops` (`elab_fns.c`) once the body is built,
  before any contract wraps it. That is `li_analyze_loops` with the method's
  parameter refinements as entry facts. On the repro the stats line now reads
  `8 proven, 0 unproven, 1 loop(s) declined`: four from the `defn`, four from
  the method, and the lambda declined.
- **A top-level lambda is declined out loud** (directions 1 and 3).
  `li_decline_unanalyzed`, called from `elab_toplevel.c` before the crossings
  resolve, sweeps every `LoopInvSite` no definition analysed. It emits the
  standard `TUR-W0372`: "it is not inside a `defn` or a `definstance` method (a
  top-level lambda is not analysed)". It also counts the loop in the stats
  line's `declined` column. A site that is a re-elaboration of an
  already-decided loop reuses that verdict instead (`li_reuse_prior`, shared
  with `li_analyze_loops`). Declining is the permanent answer for the lambda,
  and the plan's scope note now says so.

Pinned by:

- `tests/fixtures/loop-invariant-definstance-method`: proved, both checks
  elided, and the stats line asserted under `--strict-refine`.
- `tests/fixtures/errors/loop-invariant-definstance-refuted`: a false method
  invariant is now `TUR-E0371`. Before the fix it compiled clean.
- `tests/fixtures/errors/loop-invariant-top-level-lambda-strict`: the lambda's
  decline, an error under `--strict-refine`.

The original filing follows.


**Severity: medium (silent loss of the whole feature, with no diagnostic at
any strictness level).** A `while` carrying an `:invariant` inside a
`definstance` method or a top-level lambda keeps both runtime checks and gets
**no static analysis and no diagnostic** -- not a proof, not a refutation, not
even the `TUR-W0372` "is not analysed statically" notice every other
unanalysable loop gets. The annotation looks accepted, costs runtime checks on
every iteration, and verifies nothing. Filed 2026-10-02, found reviewing the
`loop-invariants` row against its `expires_at` at the v0.59.0 cut. Listed as a
scope note under "Where it departs from the elaboration above" in
[loop-invariants-plan](loop-invariants-plan.md); this is the
measured version.

## Minimal repro

Three identical loops, one per definition form:

```turmeric
(defclass Counted [A]
  (tally [a : A] : int))

(defn tally-defn [n : int] : #refine{ r : int | (>= r 0) }
  (let [^mut acc 0
        ^mut i   0]
    (while (< i n) :invariant (and (>= acc 0) (>= i 0))
      (set! acc (+ acc 1))
      (set! i   (+ i 1)))
    acc))

(definstance Counted [int]
  (tally [a : int] : int
    (let [^mut acc 0
          ^mut i   0]
      (while (< i a) :invariant (and (>= acc 0) (>= i 0))
        (set! acc (+ acc 1))
        (set! i   (+ i 1)))
      acc)))

(def tally-lambda
  (fn [n : int] : int
    (let [^mut acc 0
          ^mut i   0]
      (while (< i n) :invariant (and (>= acc 0) (>= i 0))
        (set! acc (+ acc 1))
        (set! i   (+ i 1)))
      acc)))

(defn main [] : int
  (println (tally-defn 3))
  (println (tally 3))
  (println (tally-lambda 3))
  0)
```

```sh
TUR_REFINE_STATS=1 ./build/tur --enable=loop-invariants check repro.tur
./build/tur --enable=loop-invariants --strict-refine check repro.tur
./build/tur --enable=loop-invariants emit-c repro.tur | grep -c 'Loop invariant failed on entry'
```

Observed:

```
refine: invariant: 4 proven, 0 unproven, 0 loop(s) declined
```

Four is the `defn`'s own count (two conjuncts x initiation + preservation).
The other two loops contribute nothing to any column -- not `proven`, not
`unproven`, not `declined`. `--strict-refine` adds no diagnostic either: the
only warning on the whole file is the `TUR-W0060` experiment notice. And
`emit-c` carries exactly **2** entry checks: the `definstance` method's and the
lambda's, the `defn`'s having been elided by its proof.

So the two unanalysed loops are paying the runtime cost that the feature's
value proposition is to remove, while the stats line reports a clean sheet.

Expected: at minimum the same `TUR-W0372 ... is not analysed statically`
these loops would get from any other decline, with a reason naming the form
("a loop in a `definstance` method is not analysed"). Better, analyse them.

## Root cause

`li_analyze_loops` (`src/compiler/elab_fns.c:5536`) has exactly one call site:
`src/compiler/elab_fns.c:10625`, inside `elab_defn`. The range bookkeeping it
depends on is set up there too (`li_start = e->n_loop_inv_sites`,
`elab_fns.c:9978`).

`elab_while` (`src/compiler/elab_forms.c:4657`) registers a `LoopInvSite` for
every annotated loop regardless of enclosing form, and emits both runtime
checks from the site. So a loop outside a `defn` is registered, is given its
runtime checks, and is then never visited by the analysis that would discharge
them. Nothing reports the omission because the decline machinery
(`li_decline`) is inside the pass that never runs.

The silence is the defect more than the missing analysis is. A decline is a
contract the feature already honours everywhere else: it tells you the
invariant is runtime-only and why.

## Fix directions

1. **Cheapest, and worth doing regardless of the rest:** make the gap loud.
   After the top-level pass, walk `e->loop_inv_sites[]` for any site with
   `analyzed == false` and emit the standard `TUR-W0372` with a reason naming
   the enclosing form. The site array already records every annotated loop, so
   this is a sweep over existing state, and it makes the stats line's
   `declined` column honest. It also means the next form that falls outside
   the pass (a future definition form) is self-reporting rather than silent.

2. **`definstance` methods:** a method body is elaborated with its own
   parameter bindings and return type, which is what `li_analyze_loops` needs
   (`params`, `n_params`, the parameter predicates, the declared result).
   Driving the same call from the instance-method elaboration path looks like
   the same shape as `elab_defn`'s, with the method's own range markers.
   Note the plan's own caveat applies: `TUR-W0031` is not reported on an
   instance method because its effect row is its class method's -- the
   analogous question here is whether a method's invariant should be
   analysable when the class declares the signature. It should: the invariant
   is written in the instance's body, not inherited from the class.

3. **Top-level lambdas** (`(def f (fn ...))`) are the harder half, because
   there is no enclosing `defn` to anchor the parameter predicates to and the
   lambda may be re-bound. Declining them explicitly (direction 1) may be the
   right permanent answer; if so, say that in the plan rather than leaving it
   to the stats line.

## Related

The effect system had a sibling blind spot -- `defmodule` members were never
effect-row checked
([module-members-skip-effect-row-checking](../archive/module-members-skip-effect-row-checking.md)),
because `effect_check_pass` iterated top-level `EX_FN_DEF` items only.  Fixed
2026-10-02 by walking one flattened item list (module bodies and top-level
`do` forms spread in place), which may be the shape to copy here.

**The two are not the same gap**, which is worth recording so nobody assumes
one fix covers both. Measured here: a `defn` inside a `(defmodule ...)` body
*does* reach `li_analyze_loops`.

```turmeric
(defmodule m
  (export tally-mod)
  (defn tally-mod [n : int] : int
    (let [^mut acc 0
          ^mut i   0]
      (while (< i n) :invariant (and (>= acc 0) (>= i 0))
        (set! acc (+ acc 1))
        (set! i   (+ i 1)))
      acc)))
(defn main [] : int (println (tally-mod 3)) 0)
```

gives `refine: invariant: 4 proven, 0 unproven, 0 loop(s) declined` and
`emit-c` carries zero entry checks -- analysed and elided, the same as at top
level. A module member is still an `elab_defn` call; a `definstance` method
and a lambda body are not. So this report's scope is exactly those two forms.
