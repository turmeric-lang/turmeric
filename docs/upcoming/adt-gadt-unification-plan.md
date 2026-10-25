---
title: ADT / GADT Unification Plan
category: Planning
description: Fold `defgadt` into `defdata` -- one elaborator, an optional per-constructor result type, a representation rule that keys on whether a constructor actually refines the type parameters rather than on which keyword spelled the type, and retirement of the `defgadt` keyword through the deprecation band.
---

# ADT / GADT Unification

> **Status:** SLICES 1-4 LANDED 2026-10-22; one item open -- removing
> `defgadt` (a hard error) at the next major version once the TUR-D0005
> warning has shipped in two minor releases. Decisions taken 2026-10-22 (see
> "Decisions"): rename the flag to `is_refined`; an unrefined type reports as
> a sum; `defgadt` is retired through the TUR-D deprecation band. Where the
> build departed from the text below, the slice's checklist says so under
> *As built*. Not a v1 blocker.

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

## Decisions (2026-10-22)

1. **Rename the flag.** `AdtDef.is_gadt` becomes `AdtDef.is_refined`, a value
   computed at the end of constructor parsing, never parsed from the keyword.
   About 30 read sites move (listed under "Semantics").
2. **An unrefined type reports as a sum.** The keyword is a spelling. What
   is observable today is small: `type-of` on an `any`-widened value prints
   the type's name for both forms (`B1` / `B2`; probed), the LSP symbol kind
   (`lsp_collect.c:60`) and the "GADT" wording in diagnostics and docs. The
   LSP kind follows `is_refined`; diagnostics say "refined constructor" /
   "sum" rather than "GADT" where they name the type's kind. The real
   `range.tur` consequence is representational, not cosmetic -- see
   "range.tur under slice 3".
3. **Retire `defgadt`.** It is kept through slices 1-3 as an alias so the
   merge is a refactor, then warned on (slice 4) and removed at a later
   major version. Stdlib, fixtures and guides migrate in slice 4.

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
- **`defgadt` is an alias through slices 1-3** that additionally requires
  every constructor to carry an annotation (its current error, `:2782`).
  Slice 4 deprecates it. This is OCaml's design (one `type`, optional `: t`
  per constructor) rather than Haskell's two syntaxes.

### Semantics: `is_refined` is derived

Replace the keyword-set flag with a definition-time predicate:

> A constructor is **refining** when its declared result is not the generic
> instantiation `(Name p1 ... pn)` of the type's own parameters in order.
> A type **is refined** when any constructor is refining.

A `defgadt` whose constructors all return `(Bound int)` is then a plain sum
with a phantom parameter -- which is what it is.

*As built (slice 1):* the per-constructor test above is `ctor_is_refining`,
and it is what call-site type-argument collection reads. The type-wide
`is_refined` cannot be "any constructor is refining", because by that test
`(Inclusive int : (Bound int))` refines -- `int` is not `A` -- and so would
every type the Summary calls degenerate. It is: **a type is unrefined iff one
instantiation is shared by every constructor, and each of its arguments is
the parameter in its own position or a closed type** (no type variable in
it). `Bound`'s `(Bound int)`, `SizedVec`'s `(SizedVec Size int)` and a generic
`(Box a)` are unrefined; `Refl : (Equal a a)` (its second argument is a
variable that is not `b`) and `Lit : (Expr int)` beside `IsZero : (Expr
bool)` are refined. A type with no parameters is never refined.

Everything today guarded by `is_gadt` splits by what it actually needs:

| Today's guard | Needs | After |
| --- | --- | --- |
| skolem env, escape check, reachability, refinement notes (`elab_structs.c` match path) | a constructor with a result form | unchanged: already per-constructor on `result_type_form` |
| exhaustiveness pruning (`:5277-5330`) | refined ctors | `is_refined`; also replace the first-type-arg `strcmp` heuristic with `gadt_ctor_reachable_from_scrutinee`, which exists and does the job properly |
| index checks at calls / ascriptions / returns (`elab_call.c:3252`, `:3293`, `:3632`, `:3687`, `:8485`) | a result form on the constructor | `result_type_form != NULL`; unrefined annotated ctors fall through harmlessly |
| ctor-call type-arg collection exclusion (`elab_call.c:5843`) | not to collect through a refined ctor | `!ctor_is_refining(ctor)` per constructor, so `If` above collects and `Lit` does not |
| by-value / flat / niche / monomorph gates (`types.c`, `types.h`) | a uniform per-instantiation layout | `!def->is_refined` |
| `kind_check.c:279` EX_DEFGADT walk | result forms | walks `EX_DEFDATA` too; the node kind merges (below) |
| `lsp_collect.c:60` | a symbol kind | `is_refined` (decision 2) |
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

### range.tur under slice 3

`stdlib/range.tur` is the one stdlib module whose code depends on `Bound`
being a carrier word. `Range` is not a Turmeric type: it is a hand-rolled C
`struct { int64_t lower; int64_t upper; }` (`range.tur:16`, `:235`), and
`range-new` / `range-lower` / `range-upper` (`:233-262`) are inline-C bodies
that store and return a `Bound`-typed value as that `int64_t`. Twenty-six
stdlib files and fixtures load `range.tur`, and 24 fixtures observe range
output.

Once `Bound` is by value that inline C does not compile. Probed 2026-10-22
with the same three functions over a `defdata :copy Bound`:

```
error: incompatible types when assigning to type 'int64_t' from type 'tur_adt_Bound'
error: incompatible types when returning type 'int64_t' but 'tur_adt_Bound' was expected
```

So slice 3 must carry a `range.tur` rewrite in the same PR, and it is the
rewrite the module wanted anyway: `Range` becomes a record,
`(defstruct Range :heap [lower : Bound upper : Bound])` (the module's own
header comment calls it a "heap struct"), `range-new` is the constructor,
`range-lower` / `range-upper` are field reads, and the `r : int` parameters
of the predicates (`range-contains?`, `range-encloses?`, ... `:509-616`)
become `r : Range`. That also retires a CLAUDE.md "lazy `:int` stand-in"
(`r -- int64_t pointer to a Range`) and the three `range-abort-*` helpers
stay as they are. `range-bound.tur`'s compatibility shims are unaffected.
The `Show` / `Ord` / `Eq [Bound]` instances (`:121` onward) are pure
Turmeric and need no change.

One other stdlib body holds a `defgadt` value as a word from C:
`sized-vec-from-list` (`stdlib/sized.tur:570`) calls the generated
`ctor_SVNil()` / `ctor_SVCons(...)` from inline C, keeps the result in an
`int64_t`, and returns it as `: SizedVec`. Once `SizedVec` is by value the
constructors return a struct and that body stops compiling the same way.
It is a list fold; slice 3 rewrites it in Turmeric (`SVCons` over a
right-to-left walk of the cons list) rather than teaching C about the
by-value layout. `nat.tur`, `gadt-vec.tur`, `equal.tur` and
`sized-handle-existential.tur` have no inline C that takes or returns their
GADT.

## Slices

Each slice lands whole, with its fixtures and snapshot regen in the same PR.
No experiment gate is planned: slice 1 is a refactor behind an unchanged
surface, slice 2 adds surface without changing existing programs' meaning,
slice 3 is the only ABI change and is small and measurable, and slice 4 is
a deprecation. If slice 3 has to land partially, it takes an `EXPERIMENTS[]`
row per the CLAUDE.md rule rather than an env seam.

### Slice 1 -- one elaborator, derived flag (no surface change)

- [x] `ctor_is_refining(const AdtDef *, const CtorDef *)` in `types.c`:
      compares `result_type_form` structurally against the parameter list.
- [x] `AdtDef.is_gadt` renamed `is_refined`, computed at the end of
      constructor parsing (and when a forward stub is filled). Reads move per
      the table above. *As built:* the old flag split in two.
      `has_result_types` (some constructor is annotated -- every `defgadt`
      is) gates the def-level index machinery (`gx_gadt_head`,
      `gadt_refine_to_index`, the call-site retype) and the match path's
      skolem setup, and holds the representation gates until slice 3.
      `is_refined` is the derived predicate (see "Semantics" below for the
      pinned-instantiation rule). Both are set conservatively while an
      annotated definition is parsed and on a `defgadt` stub.
- [x] `elab_defgadt` reduced to: parse via `elab_defdata` with a
      `require_result_types` flag, which makes an unannotated constructor the
      existing error. The head parsing, stub handling and MF4 rules exist
      once. *As built:* `elab_adt_def(e, call, spelled_defgadt)`; the head
      takes keywords and the parameter vector in either order. One
      consequence of the merged MF4 rule: a type no longer supersedes an
      auto-loaded **stdlib** struct (`(defgadt Pair ...)`), it is the
      defdata "already defined by an auto-loaded stdlib module" error --
      superseding skipped the stdlib type at C emission under the stdlib code
      built on it. `errors/gadt-kind-error` renamed its `Pair` to `Twin`.
- [x] `elab_defdata`'s constructor loop accepts the `: Result` tail (both
      the bare `:` symbol and the folded `F_TYPE_ANN`), runs the defgadt
      result validations, and stores `result_type_form` / `field_forms`.
      (Behind the `defgadt` spelling only until slice 2.) Fields now resolve
      through `resolve_ctor_field` for both spellings.
- [x] Constructor bindings carry `arg_full_types` whenever any field has a
      full type (today's defdata rule), for annotated constructors too.
- [x] `EX_DEFGADT` removed; paired sites collapse.
- [x] Exhaustiveness: drop the `covered_arg0` strcmp heuristic; an uncovered
      constructor is unreachable iff `!gadt_ctor_reachable_from_scrutinee`,
      else the existing error. `errors/gadt-refine-exhaustive` now annotates
      its scrutinee `(Expr int)`: `BoolLit` is dropped by the scrutinee's
      index, `Add` is the missing arm.
- [x] Diagnostics and `lsp_collect.c` wording follow decision 2.
- [x] Fixtures: every existing `gadt-*` fixture passes unchanged. New:
      `errors/defgadt-record-ctor-rejected` is **replaced** by a positive
      `gadt-record-ctor` once slice 2 lands; until then the record vector in a
      `defgadt` must be a hard error, not a one-int field (the silent
      acceptance above is a defect regardless of this plan).

### Slice 2 -- optional result type on `defdata`

- [x] `defdata` accepts `(Ctor payload ... : Result)` and
      `(Ctor [f : T ...] : Result)`; `defgadt` accepts record constructors,
      `:heap`, `:linear`, `:affine`, `:no-auto-ctor`. *As built:* `:affine`
      is new on `defdata` too (it was never in the head parser; `type_adt`
      already honoured `AdtDef.is_affine`), and `:linear` with `:affine` is an
      error. An unannotated constructor of a type with annotated ones gets the
      generic `(Name a ...)` written out as its `result_type_form`, so the
      index machinery sees one shape: `(If (IsZero (Lit 0)) (Lit 7) (Lit 9))`
      infers `(Expr int)` from its branches. The phantom-parameter `:heap`
      base constructor (`emit_flush_dead_base_ctor_traps`) assumed the named
      single-record layout; it now picks the constructor by name, stores its
      tag when the layout has one, and writes through `adt_field_member_path`.
- [x] `stdlib/forms/datatypes.tur`: `defdata` docstring gains the annotated
      form; `defgadt` is documented as "defdata where every constructor is
      annotated; deprecated, see slice 4". gadts-guide, gadts-cookbook and
      sum-types-guide updated; `tests/check-forms-documented.sh` stays green.
      (The cookbook's recipes are rewritten with the slice 4 migration.)
- [x] Fixtures: `defdata-mixed-refined-ctors` (the `Expr` above, with `If`
      unannotated), `gadt-record-ctor`, `gadt-heap-ctor`,
      `gadt-linear-ctor`, `errors/defdata-result-wrong-head`,
      `errors/defdata-result-wrong-arity`. Also
      `errors/defdata-mixed-refined-index-mismatch` (the unannotated `If`
      over `(Expr bool)` branches is refused where `(Expr int)` is wanted),
      `errors/gadt-linear-ctor-reuse` and
      `errors/defdata-affine-annotated-reuse`.
      `errors/defgadt-record-ctor-rejected` is gone, as planned.

### Slice 3 -- representation follows refinement

- [x] `types.c` / `types.h` gates switch from `is_gadt` to `is_refined`.
      `Bound`, `GVec`, `Nat`, `StaticInt`, `Size`, `SizedVec` move to the
      by-value / flat paths; `Equal` stays on the carrier. *As measured:*
      only the **non-parametric** ones move -- `StaticInt` (a flat product),
      `Nat` and `Size` (recursive sums, by value since RM4). A parametric
      type's bare name is the uniform carrier whatever its constructors say
      (`adt_sr1_sum_candidate` requires `n_type_params == 0`; probed with a
      plain `(defdata Bnd :copy [A] (Incl int) ...)`, which mallocs too), so
      `GVec [a]` and `SizedVec [n a]` keep the carrier for bare uses; only an
      applied `(GVec int)` now gets a by-value monomorph. `Nat` and `Size`
      also needed `(Nat)` -- a type applied to no arguments, the spelling
      their result annotations use -- to read as the type `Nat` in field
      position (`resolve_ctor_field`); as an application it was resolved as
      an inline field and kept the type off the by-value path (and in a
      `defdata` it failed outright in a match arm: "type expression must be
      a symbol or (ctor arg ...) application").
      Two elaboration rules moved with the representation, because an
      unrefined annotated type's applied form is now a by-value monomorph
      while its bare name is the carrier: the def-level index machinery
      (`gx_gadt_head`, `gadt_refine_to_index`, the call-argument retype)
      keys on `is_refined`, not `has_result_types` -- retyping a bare value
      to `(Tok int)` through an ascription claimed a representation it did
      not have (`(defdata Tok [a] (ITok int : (Tok int)))` passed to a
      `(Tok int)` parameter was a C type error) -- and every constructor of
      an unrefined type collects type arguments like a defdata one, so
      `(ITok 5)` is built at the monomorph exactly as its unannotated twin
      is. Only a refining constructor of a refined type skips collection.
- [x] `range.tur` rewrite per "range.tur under slice 3", same PR.
      `range-bound.tur` docstrings lose the "int64-encoded GADT cell"
      wording. *As built:* `Bound` would have stayed on the carrier for the
      reason above, so it lost its parameter: `(defdata Bound :copy
      (Inclusive int) (Exclusive int) (Unbounded))`. `[A]` was pinned to
      `int` by every constructor and never used
      (range-gadt-typeclass-migration-plan lists parameterising `Bound` as a
      non-goal); `(Bound int)` no longer names it, and no code in the tree
      or in turmeric-spices wrote that. `Range` is `(defstruct Range :copy
      [lower : Bound upper : Bound])` -- `:copy` and by value rather than the
      `:heap` sketched above: every caller reuses a range, and a by-value
      record of two by-value sums allocates nothing. Every `r : int` in
      range.tur / range-bound.tur / range-string.tur is `r : Range`.
      `range-intersection` and `range-gap` call their abort helper as a
      `when` guard, since the helper's `:int` cannot share an `if` with a
      `Range`.
- [x] `sized.tur`'s `sized-vec-from-list` rewritten in Turmeric (same
      section), same PR. *Not needed:* `SizedVec` is parametric and its bare
      name stays on the carrier, so `ctor_SVNil()` / `ctor_SVCons(...)` still
      return the `int64_t` that inline body keeps; it compiles and runs as
      before (`sized-vec-basic`).
- [x] `tests/check-emitted-float-conversions.py` and the region-escape
      fixtures (`region-escape-via-store`) re-run: a by-value `Bound` is a new
      shape for `TUR_REGION_NOTE_WORDS`. Zero float findings on the range,
      sized, nat and new slice 2 fixtures. A by-value `Bound` / `Range` holds
      only integers; `region-escape-via-store` gains case 12, an annotated
      recursive sum (`Peano`) whose boxed field is built inside a bracket and
      pushed into an outer vec, on both `TUR_REGIONS` arms
      (`tests/run-regions-seam.sh` green).
- [x] Snapshots regenerated in the same PR (CLAUDE.md "Fixture churn is not
      a deferral reason"). Expect the `range-*`, `sized-*`, `nat-*` and
      `gadt-vec-*` fixtures to move. *As measured:* none of those carries an
      `expected.c`, and no snapshot moved -- the full suite (4080 fixtures)
      was green with none regenerated.
- [x] Measure: `tur emit-c` of a `range.tur` caller shows no `malloc` in
      `ctor_Bound_*`; the range benchmark, if one exists under `bench/`, is
      re-run before and after. *Measured 2026-10-22:* `ctor_Bound_Inclusive`
      returns a `tur_adt_Bound` by value and `range_new` a `tur_adt_Range`
      (both were `int64_t` over a `malloc`). There is no range benchmark; a
      2M-iteration loop building a `closed-open-range`, an `at-least-range`
      and their `range-span`, Release build: **0.47-0.58 s and 437 MB peak
      RSS before, 0.12-0.13 s and 1.2 MB after** (every endpoint and range
      was a never-freed malloc).

### Slice 4 -- retire `defgadt`

- [x] `TUR-D0005: defgadt is deprecated; write defdata with a : Result on
      each constructor` at the form's head, in the TUR-D band (`diag.c`
      "Deprecation band", precedent TUR-D0002/D0003 for the legacy effect-row
      spellings, which carried two minor releases). `tur explain TUR-D0005`
      shows the rewrite. Under `--Werror=deprecated` it is an error. (The
      definition is still elaborated under `--Werror=deprecated`, so its uses
      report nothing further.)
- [x] Migrate the seven stdlib `defgadt`s to `defdata`; `equal.tur`'s
      `Equal` keeps its annotation and stays refined, the other six drop
      theirs (every constructor returned the generic instantiation). *As
      built:* only the parameterless ones' annotations were the generic
      instantiation -- `Nat`, `StaticInt` and `Size` drop theirs and write
      their self-references bare (`(Succ Nat)`). `GVec`'s `(GVec int)` and
      `SizedVec`'s `(SizedVec Size int)` are not generic -- dropping them
      would make `GVNil` an `(GVec a)` -- so both keep them and are simply
      unrefined. `Bound` went in slice 3.
- [x] Migrate the 78 fixtures that spell `defgadt`, keeping one
      `errors/defgadt-deprecated` fixture that pins TUR-D0005, and the
      `gadt-*` fixture names (they test refinement, not the keyword). *As
      built:* 81 fixture files. `errors/gadt-syntax-error` keeps `defgadt`:
      it pins defgadt's own rule (an unannotated constructor is an error).
      `defgadt-deprecated-warns` pins the warning and that the program still
      runs; `errors/defgadt-deprecated` pins the error under
      `--Werror=deprecated`.
- [x] Guides: the 19 docs files and `tools/genguides.py` / `tools/gendocs.py`
      keyword lists. gadts-guide's "Your First GADT" becomes "Refined
      constructors" over `defdata`. `stdlib/forms/datatypes.tur`'s `defgadt`
      entry becomes the deprecation note. *As built:* the tools' keyword
      lists keep `defgadt`, which still parses; the Try Turmeric tour and
      landing page, `turi`'s REPL docstrings and the CHANGELOG are updated
      too.
- [ ] Removal: `defgadt` becomes a hard error with the same rewrite hint at
      the next major version after the warning has shipped in two minor
      releases. Tracked here, cut by the release skills' CHANGELOG step.

## Risks

- **A refined type that today is by-value somewhere.** None should exist:
  every by-value path already bails on `is_gadt`, and `is_refined` implies
  `is_gadt` for every type written today. The only direction of change is
  degenerate GADTs gaining by-value.
- **`range.tur` inline C.** Covered above; it is a hard C error, not a
  silent change, so it cannot slip through. The rewrite is in slice 3's PR.
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
- **Spices.** No spice spells `defgadt` (checked 2026-10-22), so slice 4
  has no downstream migration.

## Non-goals

- Changing what refinement can express (dependent indices, polymorphic
  recursion inference). See gadts-guide "Current Limitations".
- Touching `defstruct` -> record-ADT lowering (CONV-S1, graduated) or
  `defopaque`.
- A `tur fmt` rewrite of `defgadt` to `defdata`; the migration is by hand
  and the TUR-D0005 hint is the tooling.

## References

- `docs/archive/gadts-plan.md`, `docs/archive/gadts-followup-tasks.md`
- `docs/archive/struct-adt-convergence-plan.md`,
  `docs/archive/defstruct-as-defadt-plan.md` (the precedent: one record
  surface over one ADT representation)
- `docs/archive/sum-representation-plan.md` (the by-value gates this plan
  widens)
- `docs/reported/parametric-sum-stays-on-carrier.md` (slice 3's open
  remainder: why `GVec` and `SizedVec` stay on the carrier)
- `docs/archive/gadt-length-index-not-enforced.md` (the index checks that
  key on `result_type_form`)
- `docs/archive/design-mf4-struct-gadt-namespaces.md`
- `docs/archive/history/defgadt-copy-and-shared-bounds.md` (why `Bound` is
  a `:copy` GADT in the first place)
- `docs/guides/gadts-guide.md`, `docs/guides/gadts-cookbook.md`,
  `docs/guides/sum-types-guide.md`
