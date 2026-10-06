---
title: Track C U5 (HKT recursion for ASTs) — feasibility after recent turmeric fixes
category: Spice-uplift feasibility analysis
status: UNBLOCKED — defdata applied-field blocker fixed by turmeric #483 (+#482); U5 AST targets are now doable. json target remains moot (yyjson-backed). Minor annotation-level edges remain.
reported-by: turmeric-spices Claude (Track C, branch claude/track-c-u5-turmeric-2miu28)
verified-on: turmeric 0.22.0, main @ 99cc8b3 (post #483 applied-type defdata fields, #482 by-value parametric struct fields); prototype + generic-cata re-verified @ 97dcd86 (post #487 / gap G6 generic-cata carrier fix), built from source (build-release)
---

# Track C U5 — can it be done after the recent turmeric fixes?

**Short answer: yes, the blocker is fixed.** The `defdata` applied-field
limitation reported on 2026-06-21 was resolved the same day by turmeric
**#483** ("Allow applied type constructors in defdata constructor fields",
main @ `99cc8b3`), together with **#482** (by-value parametric struct field
layout). The single-param `defdata` kind wart is gone too. The full
recursion-schemes pattern U5 needs — typed sum functors, `Functor`/`fmap`,
a **by-value typed `Fix`**, and a recursive `cata` over container-typed
children — now compiles **and runs**.

The first-listed U5 target, `json`, remains **moot** for an unrelated
reason: the spice is yyjson-backed and has no Turmeric-level recursive IR
to collapse.

U5 plan reference:
`docs/archive/spices-type-features-uplift-plan.md`
§ "Phase U5 -- HKT recursion for ASTs" (targets: json, c-dsl, glsl,
scscm, regex, template).

All claims below were checked against a from-source `tur` 0.22.0
(`cmake --build build-release`) on turmeric main @ `99cc8b3`.

---

## What now works (verified — checks AND runs)

1. **By-value typed `Fix`.** `(defdata Fix [^f] (Roll (f (Fix f))))` is
   accepted (was rejected pre-#483). No more int-carrier `Roll :int`
   indirection required.

2. **Sum functor + `Functor` instance, single param.**
   `(defdata ExprF [a] (LitF :int) (AddF a a) (MulF a a))` with
   `(definstance Functor [ExprF] (fmap [c g] (match c ...)))` checks clean.
   The earlier `TUR-E0012` kind-`*` error on single-param `defdata` is
   fixed — **no phantom-param / partial-app-head hack needed**.

3. **Recursive `cata` over a by-value `Fix`, in pure Turmeric.** An
   arithmetic evaluator (`AddF`/`MulF` children as sub-nodes) folded
   `(mul (add (lit 1) (lit 2)) (lit 10))` to `30`.

4. **Container-typed children (the U5 json `Arr`/`Obj` shape).** A node
   `(defdata JsonF [a] (JNumF :int) (JArrF (Vec a)))` with a `Functor`
   instance, wrapped in the by-value `Fix`, folded a nested array
   `[10, 20, [5, 7]]` to `42` via a `cata` that sums all numbers.

So **every U5 AST target is unblocked**: c-dsl `Expr`/`Stmt`, glsl, scscm
`SExpr`, regex `Re`, template node IR — including nodes whose children sit
in a container, not just bare recursive positions.

---

## Residual edges (annotation-level, not blockers)

1. **Use `Vec`/`Map`, not the `list`/`option` aliases, for container
   children.** `(JArrF (Vec a))` and `(... (Map str a))` are accepted;
   `(JArrF (list a))` / `(... (option a))` fail with `TUR-E0012: cannot
   apply a type of kind '*' as a type constructor`, because `list` and
   `option` are macro-aliases (nested `tcons` / tagged structs), not
   arrow-kinded type constructors. The U5 plan writes the json node as
   `Arr (list a) | Obj (map str a)`; re-spell those as `Vec`/`Map`.

2. **Matching `(Roll layer)` does not refine the layer's recursive type
   var.** After `(match e (Roll layer) ...)`, `layer` infers with its
   recursion slot unresolved (children come back as `int`). Add one
   ascription per fold: `(let [l (:: layer (ExprF (Fix ExprF)))] (match l
   ...))`. With it, the fold checks and runs.

3. **Generic `vec-get` returns the element at the int carrier.** When a
   child lives in a `(Vec (Fix F))`, ascribe the read:
   `(:: (vec-get xs i) (Fix F))`. One ascription per child access.

4. **Bare 0-arg named type as a field is still rejected.** `(Cx C)` (a
   non-applied named type) still triggers the old "field type must be a
   keyword" message. Recursion through `(Fix F)` / applied forms sidesteps
   this, so it rarely bites; worth a follow-up if a spice wants flat
   mutually-recursive node types without `Fix`.

None of these block U5; they are the verbose-`cata` ergonomics the plan
already anticipated (P4 `match-fix` sugar would erase #2/#3).

---

## The json target is still moot

Independent of the (now-fixed) compiler blocker: the `json` spice is
**100% yyjson-backed**. `spices/json/src/json/{parse,emit,encode,decode,patch}.tur`
are inline-C wrappers over `yyjson_*`; there is **no Turmeric-level
recursive node IR**. `emit.tur` is a single `yyjson_write` call, not the
"manual recursion in `json__emit.c`" the U5 plan describes — that premise
is stale. There is nothing in the json spice to convert to a `cata`; the
recursion lives in yyjson's C, which we should not replace with a slower
pure-Turmeric tree walk. **json should be dropped from U5** (or the plan
re-scoped to note the yyjson backend already subsumes the goal). The
json-shaped functor itself is now expressible (verified above) — it just
has no home in the current json spice.

---

## Recommendation, per U5 target

| Target   | Status | Notes |
|----------|--------|-------|
| json     | **Drop / moot** | yyjson-backed; no Turmeric IR to collapse |
| c-dsl    | **Doable now** | `Expr`/`Stmt` via by-value `Fix` + `cata` |
| glsl     | **Doable now** | same shape as c-dsl |
| scscm    | **Doable now** | `SExpr` via by-value `Fix` |
| regex    | **Doable now** | `ReF a = Lit c \| Alt a a \| Concat a a \| Star a` |
| template | **Doable now** | node IR + container children via `Vec`/`Map` |

Suggested first landing: **regex** or **scscm** — smallest ASTs, children
mostly in bare recursive positions, so the residual ascriptions are
minimal. Ship the verbose `cata` first; P4 `match-fix` sugar is a later
ergonomics pass.

---

## Prototype landed: `regex/tree` (U5 proof-of-concept)

`spices/regex/src/regex/tree.tur` + `spices/regex/tests/tree_test.tur` are a
working U5 prototype (regex is PCRE2-backed with no Turmeric IR, so this is a
new self-contained recursion-schemes surface, not a refactor of the
bindings). It ships:

- `ReF a` sum functor + `Functor [ReF]` instance,
- a by-value fixed point `Re = Roll (ReF Re)` (enabled by #483),
- a generic `re-cata` (cata-via-`fmap`),
- value folds `re-size` / `re-nullable?` / `re->str`, each a plain F-algebra
  over the single `re-cata`, plus a backtracking `re-matches?`.

**14/14 tests green** against from-source `tur` @ `97dcd86`.

Updated 2026-06-21 (post #487): the generic-`cata` miscompile this prototype
first surfaced was tracked as gap **G6** and is now **fixed** — `re-cata` runs
correctly at int / bool / cstr carriers, so all three value folds go through it
(no more direct-recursion fallback). The closure-capture gap that also blocked
the matcher was fixed alongside G6. Verified directly: `(= 4 (re-cata size-alg
e))` → true; `re-cata null-alg (Alt L Empty)` → true; `re-cata str-alg` →
`((a|b))*c`; returned-closure-capturing-closures → correct.

**One narrow edge remains** (matcher only): `re-cata` does not yet thread a
**function-typed carrier** `B`. The "NFA is one cata" form uses a matcher
closure `(fn [k s] bool)` as the carrier; that type-checks but `(re-cata
match-alg e)` comes back as `int` (`error: expression in call head has type
int, which is not callable`). So `re-matches?` stays direct structural
recursion. This is the next thing to file if the function-carrier cata is
wanted.

---

## Status of the turmeric reports

- Blocker filed 2026-06-21 ("`defdata` constructor fields reject applied type
  constructors") — **RESOLVED by #483**; secondary single-param kind issue
  resolved too.
- Generic catamorphism via `Functor` `fmap` miscompile (boxed `int`/`bool`
  results; `cstr`-carrier segfault) — **RESOLVED**: tracked as gap G6, fixed by
  #487 (spec-selection by result type + per-carrier cloning of the recursive
  `fmap` closure). Report archived at
  `docs/archive/history/hkt-fmap-cata-carrier-miscompile.md`.
- The closure-capture codegen gap (returned closure capturing `let`-bound
  folded closures) — **RESOLVED** alongside G6.
- **NEW, narrow:** `re-cata` does not thread a function-typed carrier `B` (see
  the matcher note above). Not yet filed; only matters for the closure-carrier
  "NFA is one cata" form, which has a clean direct-recursion alternative.

The layer `(:: ...)` and `vec-get` ascriptions remain annotation-level and do
not warrant a report.
