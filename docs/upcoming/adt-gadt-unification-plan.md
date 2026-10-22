---
title: ADT / GADT Unification Plan
category: Planning
description: Fold `defgadt` into `defdata` -- one elaborator, an optional per-constructor result type, and a representation rule that keys on whether a constructor actually refines the type parameters rather than on which keyword spelled the type.
---

# ADT / GADT Unification

> **Status:** PROPOSED 2026-10-22. Not started. Not a v1 blocker; schedule
> after the v1 track or when a slice below is on the way to something that is.

## Summary

`defdata` and `defgadt` already share one representation: both produce an
`AdtDef` whose type is `TY_ADT`, with `CtorDef` constructors
(`src/compiler/types.h:275-426`). The whole difference is a `bool is_gadt`
and a per-constructor `result_type_form`. The emitter, interpreter, LSP,
borrow check, effect check and split emission treat `EX_DEFDATA` /
`EX_DEFGADT` as a pair with identical payloads and never read `is_gadt`.

What is still split is the wrong half:

- **Two elaborators.** `elab_defdata` (`src/compiler/elab_structs.c:1531`,
  ~585 lines) and `elab_defgadt` (`:2511`, ~835 lines) parse the name, the
  type-parameter vector, the copy kind, the forward stub and the MF4
  struct-shadowing rules twice.
- **The GADT twin is the poorer one.** It accepts only `:copy` / `:move`;
  `:heap`, `:linear`, `:affine` and `:no-auto-ctor` are rejected
  (`:heap` fails as "constructor must be a list form"). Record-style fields
  are positional-only by design (`ctor->is_record = false`, `:2912`), and a
  record vector is **silently accepted as one `int` field**: `(defgadt Shape
  [a] (Rect [w : int h : int] : (Shape int)))` elaborates, then
  `(Rect 3 4)` is "returns <adt>, which is not callable" and `(Rect w h)` in
  a match is "expects 1 fields, got 2". GADT constructor bindings never get
  `arg_full_types` (compare `:2064` for defdata with `:2949` for defgadt), so
  GADT constructor calls miss the TS4P1 polymorphic-constructor inference,
  and `elab_call.c:5843` excludes them from constructor-call type-argument
  collection outright.
- **Representation keys on the keyword, not on refinement.** Every by-value,
  flat-layout, niche and monomorph predicate bails on `is_gadt`:
  `type_register_adt_app` (`types.c:2227`), `type_adt_app_ctor_suffix`
  (`:2306`), `adt_sr1_sum_candidate` (`:4024`),
  `adt_app_is_byvalue_product_inner` (`:4582`), `adt_is_flat_product` and
  `adt_uses_named_layout` (`types.h:436`, `:464`). Measured 2026-10-22 on an
  identical three-constructor `Bound`: as `defdata` every constructor
  returns a by-value `tur_adt_Bound` struct; as `defgadt` every constructor
  `malloc`s a box, the value rides the int64 carrier, and the `match` never
  frees it.

The measurement matters because **six of the seven stdlib `defgadt`s are not
GADTs.** Only `Equal` (`stdlib/equal.tur:9`, `Refl : (Equal a a)`) has a
constructor whose result differs from the generic instantiation. `Bound`
(`stdlib/range.tur:46`), `GVec` (`gadt-vec.tur:16`), `Nat` (`nat.tur:16`),
`StaticInt`, `Size` and `SizedVec` (`sized.tur:20,94,178`) return the same
instantiation from every constructor and pay heap allocation for nothing.

The GADT machinery that is semantically real -- the per-arm `SkolemEnv`
(`elab_structs.c:2363`, `:4777`), the skolem-escape check (`:5117`),
reachability pruning (`gadt_ctor_reachable_from_scrutinee`, `:2338`), and the
index checks at calls, ascriptions and returns (`gadt_refine_to_index`,
`gadt_claim_disagrees`, `elab_call.c:3685`, `:3740`) -- already keys on a
constructor's `result_type_form` being present. It survives the merge
unchanged.

## Design

### Surface: one form, optional result type

`defdata` accepts an optional `: Result` on each constructor, in the position
`defgadt` already uses:

```turmeric
(defdata Expr [a]
  (Lit int : (Expr int))                    ; refined
  (IsZero (Expr int) : (Expr bool))         ; refined
  (If (Expr bool) (Expr a) (Expr a)))       ; unannotated: (Expr a)
```

- An unannotated constructor's result is the generic instantiation
  `(Name a b ...)`, exactly as today.
- An annotated constructor's result must be an application of `Name` with
  the declared arity (the existing defgadt checks, `:2808-2890`).
- Record-style constructors take the annotation after the field vector:
  `(Rect [w : int h : int] : (Shape int))`. The reader already folds
  `: type` into `F_TYPE_ANN`; the record path (`ctor_form_is_record`) learns
  to stop at it.
- `:heap`, `:linear`, `:affine`, `:no-auto-ctor` and record constructors
  work on a type with annotated constructors because there is only one
  head parser.
- **`defgadt` stays as an alias** that additionally requires every
  constructor to carry an annotation (its current error, `:2782`). No
  stdlib, fixture or spice migration is forced; `tur fmt`-style rewriting is
  out of scope. This is OCaml's design (one `type`, optional `: t` per
  constructor) rather than Haskell's two syntaxes.

### Semantics: `is_gadt` becomes derived

Replace the keyword-set flag with a definition-time predicate:

> A constructor is **refining** when its declared result is not the generic
> instantiation `(Name p1 ... pn)` of the type's own parameters in order.
> A type **is refined** when any constructor is refining.

`AdtDef.is_gadt` is renamed `is_refined` (or kept as a field name and
documented as derived; either way it is computed, never parsed). A `defgadt`
whose constructors all return `(Bound int)` is then a plain sum with a
phantom parameter -- which is what it is.

Everything today guarded by `is_gadt` splits by what it actually needs:

| Today's guard | Needs | After |
| --- | --- | --- |
| skolem env, escape check, reachability, refinement notes (`elab_structs.c` match path) | a constructor with a result form | unchanged: already per-constructor on `result_type_form` |
| exhaustiveness pruning (`:5277-5330`) | refined ctors | `is_refined`; also replace the first-type-arg `strcmp` heuristic with `gadt_ctor_reachable_from_scrutinee`, which exists and does the job properly |
| index checks at calls / ascriptions / returns (`elab_call.c:3252`, `:3293`, `:3632`, `:3687`, `:8485`) | a result form on the constructor | `result_type_form != NULL`; unrefined annotated ctors fall through harmlessly |
| ctor-call type-arg collection exclusion (`elab_call.c:5843`) | not to collect through a refined ctor | `!ctor_is_refining(ctor)` per constructor, so `If` above collects and `Lit` does not |
| by-value / flat / niche / monomorph gates (`types.c`, `types.h`) | a uniform per-instantiation layout | `!def->is_refined` |
| `kind_check.c:279` EX_DEFGADT walk | result forms | walks `EX_DEFDATA` too; the node kind merges (below) |
| `lsp_collect.c:60` | a symbol kind | `is_refined` (cosmetic) |
| `elab_toplevel.c:3223` forward stub | the head keyword | one stub path; stub `is_refined` is unknown until the body is seen, so stubs stay off the by-value path until filled (same as today's GADT stub) |

Why the representation gate is exactly "refined": a refined constructor's
field of type `a` has a different concrete type per arm, so no monomorph
`tur_adt_Expr__int` can hold an `(Expr a)` value and the type must stay on
the uniform carrier. An unrefined parametric sum has one layout per
instantiation and goes through the SR1/SR2 by-value paths like any `defdata`.

### AST: one node

`EX_DEFGADT` is removed; `elab_defgadt` returns `EX_DEFDATA`. The ~35 paired
`case EX_DEFDATA: case EX_DEFGADT:` sites and the `(kind == EX_DEFGADT) ?
defgadt_.def : defdata_.def` selectors collapse (`emit_module.c`,
`emit_expr.c`, `eval.c:13779`, `borrow_check.c:774`, `srfi_prune.c:403`,
`effect_check.c:2045`, `expr.c:591`, `lsp_collect.c:76`,
`elab_call.c:4276`).

## Slices

Each slice lands whole, with its fixtures and snapshot regen in the same PR.
No experiment gate is planned: slice 1 is a refactor behind an unchanged
surface, slice 2 adds surface without changing existing programs' meaning,
and slice 3 is the only ABI change and is small and measurable. If slice 3
has to land partially, it takes an `EXPERIMENTS[]` row per the CLAUDE.md
rule rather than an env seam.

### Slice 1 -- one elaborator, derived flag (no surface change)

- [ ] `ctor_is_refining(const AdtDef *, const CtorDef *)` in `types.c`:
      compares `result_type_form` structurally against the parameter list.
- [ ] `AdtDef.is_refined` computed at the end of constructor parsing (and
      when a forward stub is filled). `is_gadt` reads replaced per the table
      above; the field goes away.
- [ ] `elab_defgadt` reduced to: parse via `elab_defdata` with a
      `require_result_types` flag, which makes an unannotated constructor the
      existing error. The head parsing, stub handling and MF4 rules exist
      once.
- [ ] `elab_defdata`'s constructor loop accepts the `: Result` tail (both
      the bare `:` symbol and the folded `F_TYPE_ANN`), runs the defgadt
      result validations, and stores `result_type_form` / `field_forms`.
- [ ] Constructor bindings carry `arg_full_types` whenever any field has a
      full type (today's defdata rule), for annotated constructors too.
- [ ] `EX_DEFGADT` removed; paired sites collapse.
- [ ] Exhaustiveness: drop the `covered_arg0` strcmp heuristic; an uncovered
      constructor is unreachable iff `!gadt_ctor_reachable_from_scrutinee`,
      else the existing error.
- [ ] Fixtures: every existing `gadt-*` fixture passes unchanged. New:
      `errors/defgadt-record-ctor-rejected` is **replaced** by a positive
      `gadt-record-ctor` once slice 2 lands; until then the record vector in a
      `defgadt` must be a hard error, not a one-int field (the silent
      acceptance above is a defect regardless of this plan).

### Slice 2 -- optional result type on `defdata`

- [ ] `defdata` accepts `(Ctor payload ... : Result)` and
      `(Ctor [f : T ...] : Result)`; `defgadt` accepts record constructors,
      `:heap`, `:linear`, `:affine`, `:no-auto-ctor`.
- [ ] `stdlib/forms/datatypes.tur`: `defdata` docstring gains the annotated
      form; `defgadt` is documented as "defdata where every constructor is
      annotated". gadts-guide, gadts-cookbook and sum-types-guide updated;
      `tests/check-forms-documented.sh` stays green.
- [ ] Fixtures: `defdata-mixed-refined-ctors` (the `Expr` above, with `If`
      unannotated), `gadt-record-ctor`, `gadt-heap-ctor`,
      `gadt-linear-ctor`, `errors/defdata-result-wrong-head`,
      `errors/defdata-result-wrong-arity`.

### Slice 3 -- representation follows refinement

- [ ] `types.c` / `types.h` gates switch from `is_gadt` to `is_refined`.
      `Bound`, `GVec`, `Nat`, `StaticInt`, `Size`, `SizedVec` move to the
      by-value / flat paths; `Equal` stays on the carrier.
- [ ] `tests/check-emitted-float-conversions.py` and the region-escape
      fixtures (`region-escape-via-store`) re-run: a by-value `Bound` is a new
      shape for `TUR_REGION_NOTE_WORDS`.
- [ ] Snapshots regenerated in the same PR (CLAUDE.md "Fixture churn is not
      a deferral reason"). Expect the `range-*`, `sized-*`, `nat-*` and
      `gadt-vec-*` fixtures to move.
- [ ] Measure: `tur emit-c` of `stdlib/range.tur`'s callers shows no `malloc`
      in `ctor_Bound_*`; the range benchmark, if one exists under
      `bench/`, is re-run before and after.
- [ ] Rewrite the six degenerate stdlib `defgadt`s as `defdata` only if a
      reader benefits; the alias makes it optional, and leaving them is a
      live test that the alias path gets by-value.

## Risks

- **A refined type that today is by-value somewhere.** None should exist:
  every by-value path already bails on `is_gadt`, and `is_refined` implies
  `is_gadt` for every type written today. The only direction of change is
  degenerate GADTs gaining by-value.
- **ABI of degenerate GADTs across separate compilation.** `Bound` is
  `from_stdlib`; a client unit and the lib unit must agree, which the
  existing SR machinery handles for `defdata`. The `sepcomp-*` fixtures are
  the check.
- **Size-indexed types.** `SizedVec`'s `SizeTerm` capture in
  `gadt_build_skolem_env` fires on result-form heads `Static`/`Add`/`Mul`.
  A constructor with such an index is refining by the structural test
  (the result is not the bare parameter list), so the sized machinery stays
  on the carrier where it is today. Confirm with the `sized-*` fixtures.
- **Forward stubs.** A stub is `is_refined = unknown` until its body
  elaborates. Today a GADT stub already stays off the by-value path; keep
  that conservative default so a recursive field probing
  `adt_is_byvalue_product` mid-definition sees the same answer before and
  after (the `ctors[ci] == NULL` guard and its SEGV history, `:2706`).

## Non-goals

- Changing what refinement can express (dependent indices, polymorphic
  recursion inference). See gadts-guide "Current Limitations".
- Removing the `defgadt` keyword or rewriting existing sources.
- Touching `defstruct` -> record-ADT lowering (CONV-S1, graduated) or
  `defopaque`.
- `tur fmt` or any migration tooling.

## Open questions

1. Keep the field named `is_gadt` (fewer diffs, misleading) or rename to
   `is_refined` (honest, ~30 sites)? This plan assumes the rename.
2. Should an unrefined type with annotated constructors print as a GADT in
   `type-of` / LSP symbol kind? Proposed: no, it is a sum; the keyword is a
   spelling.

## References

- `docs/archive/gadts-plan.md`, `docs/archive/gadts-followup-tasks.md`
- `docs/archive/struct-adt-convergence-plan.md`,
  `docs/archive/defstruct-as-defadt-plan.md` (the precedent: one record
  surface over one ADT representation)
- `docs/archive/sum-representation-plan.md` (the by-value gates this plan
  widens)
- `docs/archive/gadt-length-index-not-enforced.md` (the index checks that
  key on `result_type_form`)
- `docs/archive/design-mf4-struct-gadt-namespaces.md`
- `docs/guides/gadts-guide.md`, `docs/guides/gadts-cookbook.md`,
  `docs/guides/sum-types-guide.md`
