# Plan: Type-Level Row Follow-Ups

> **Status:** OPEN (re-audited 2026-10-24). R0 DONE, and since extended by a
> dedicated `docs/guides/row-types-guide.md`. R1 is demand-driven and still
> has **zero** demand: the builtin set is the original four (`row-concat` /
> `row-union` / `row-intersect` / `row-canon`), `row-diff` / `row-contains`
> are unadded (both still a kind error when written, re-probed), and no spice
> or stdlib code calls the row algebra. R2 stays NOT scheduled. R3 stays NOT
> scheduled as a general feature, but its bounded first step shipped through
> the macro-system direction (`syntax-struct-fields`, see R3). Nothing here is
> in progress; the plan is kept as the record of the rulings.
> **Type:** Compiler / Type System / Docs
> **Predecessors:** `docs/archive/history/variadic-hkt-rows-missing.md`
> (Layers 1-6), `docs/archive/history/row-type-in-value-position-loses-elements.md`
> **Related:** `docs/archive/ecs-component-set-bounds-plan.md`

## Why this document exists

An agent surveying the codebase reported that "row types are not expressible as
values" as though it were an unfinished edge. It is not -- it is a designed
boundary with three guards and a dedicated error code. The reason it reads as a
gap is that the *only* substantive row documentation lived in an **archived**
history file, and that file describes gaps which have since been closed.

That mis-read was reproduced under controlled conditions during this
investigation: a second surveying agent, reading
`docs/archive/history/variadic-hkt-rows-missing.md:362`, confidently reported
that "row-polymorphic `defn` is not wired; `^&` is only recognised on
`defstruct` params." That is false -- `src/compiler/elab_fns.c:9522-9538`
(2026-10-24 line numbers; `fn` has the twin at `:13899-13905`) handles `^&name` in the `defn` type-param vector (L6 follow-up B) and two
fixtures cover it. The doc was right when written and wrong now.

So the primary deliverable was documentation, not code. That has shipped (see
R0). This plan records what remains.

## R0 -- Document the boundary (DONE)

A "Type-Level Rows (`#row{...}`)" section was added to
`docs/guides/hkt-guide.md`, which previously had **zero** row coverage. It
covers the kind story (`^f` / `^^f` / `^&r`), both literal forms, the row
algebra, the three value-position guards with their error codes, the reasoning
behind them, and the current limits. A cross-reference was added under Known
Limitations.

*Since then (re-checked 2026-10-24):* the section is still in
`hkt-guide.md` (`:485`), and rows now also have a dedicated user-facing home,
`docs/guides/row-types-guide.md` (454 lines: kind, literal forms, `^&`, the
algebra, equality and `row-canon`, labels, erasure, worked examples, "What rows
deliberately cannot do", an error reference), indexed in
`docs/guides/README.md:94`. That is the Documentation-hygiene rule below
applied. One loose end: the `hkt-guide.md` rows section does not yet link to
`row-types-guide.md`.

## Current state (re-verified 2026-10-24; first verified 2026-07-28)

**Representation.** `TY_TYPEROW`, kind `KIND_TYPEROW` (`[*]`, spelled `^&`).
`src/compiler/types.h:181-187` states the invariant plainly: a row is "never
the type of a runtime value, so it erases at codegen like TY_TYPECLASS /
TY_GLOBAL." Constructors `type_typerow` / `type_typerow_named` at
`src/compiler/types.c:5708-5743`. Order-significant, duplicates preserved, no
nest-flattening, `uint8_t` element count (255 cap).

**Algebra**, all pure compile-time C (`src/compiler/types.c:5744-5987`,
declared `types.h:1643-1692`):

| C function | Surface |
|---|---|
| `type_typerow_contains` | *(none -- internal only)* |
| `type_typerow_concat` | `(row-concat A B ...)` |
| `type_typerow_union` | `(row-union A B ...)` |
| `type_typerow_intersect` | `(row-intersect A B ...)` |
| `type_typerow_canonical` | `(row-canon R)` |
| `type_typerow_eq_perm` | *(none -- never consulted by the checker)* |

Surface dispatch at `src/compiler/elab_types.c:1090-1165`; head-symbol
recognition at `src/compiler/elab_fns.c:7700-7710`. (`type_typerow_contains`
and `type_typerow_eq_perm` still have no caller outside `types.c`, 2026-10-24.)

**Reader.** `#row{...}` is read **ungated** at `src/compiler/reader.c:1987-1990`
(`read_row_literal`, `:1692`)
(no `-Xdata-literals` requirement -- the row dispatch sits beside the
data-literal table, not inside it; confirmed by row fixtures carrying no `cmd`
file).

**The three guards.**

1. `src/compiler/elab_toplevel.c:1002-1006` -- row literal in value expression
   position.
2. `src/compiler/elab_fns.c:7619-7643` -- **TUR-E0012**, a value cannot have row
   type. The guard lives in the shared `fn_type_from_form` wrapper so it fires in
   every value-type sub-position.
3. `src/passes/kind_check.c:171-173` + `check_row_type_arg_kind`
   (`elab_types.c:134-165`) -- kind discipline; a row has kind `[*]`, not an
   arrow kind, so it cannot be applied.

All three re-probed live against `build/tur` (v0.70.5) on 2026-10-24.

Guard 2 exists because of a real bug: a row in value-type position was once
silently accepted and lost its elements
(`docs/archive/history/row-type-in-value-position-loses-elements.md`).

**Typed-field rows.** `#row{id : int name : cstr}` with a parallel
`field_names[]`; `TUR-E0290` (mixed positional/named), `TUR-E0291` (duplicate
name).

**Consumers.** No `stdlib/` usage at all (no `#row{` under `stdlib/`,
2026-10-24). Rows are exercised by fixtures (ten `tests/fixtures/hkt-row-*`,
`typed-field-row-accept`, plus fourteen `errors/row-*` and three
`errors/typed-field-row-*` fixtures; unit coverage in
`tests/compiler/test_typerow.c` / `test_kind_row.c`) and by five modules in the
sibling spices repo: `frame`, `sqlite`, `httpd`, `postgres`, `ecs` -- all as
phantom literals only.

**Known limits.** No term-level row operations (`(k in r)` membership
explicitly deferred, `docs/archive/spices-type-features-uplift-plan.md:111-115`);
not an extensible-record system (no `{ x : int | r }`, no field insert/delete/
restrict, no `Row r` wrapper); permutation equality implemented but never
applied implicitly; lenient element resolution in some positions; `deftype`
(TY_REC) carries no per-parameter kind array, so its apply sites fall back to
arity-only checking (`elab_types.c:139-142`).

*Re-probed 2026-10-24:* the `deftype` gap still reproduces --
`(deftype Frame [^&cols] (Frame :int))` then `[x : (Frame int)]` passes
`tur check` with no diagnostic, where the same shape on a `defstruct` is
`TUR-E0012`. Permutation is still not applied implicitly
(`(Q #row{bool int})` against `(Q #row{int bool})` is `TUR-E0001`). Operating
on an abstract row variable is still rejected (`(row-union r #row{Tag})` with
`^&r`: "operand 1 must be a #row{...}"). Lenient element resolution did NOT
reproduce in the positions probed -- an unknown element is a hard error in a
signature, inside a `row-union` operand, and in a `::` ascription (L6
follow-up A); other positions not verified.

## R1 -- Near-term: add row builtins on demand

**Task.** When a row operation is genuinely needed by a consumer, add it as a
builtin next to the existing four. Do **not** build a general type-level
function facility first.

**Demand survey, 2026-08-17: zero -- nothing to add.**  Method: grepped every
consumer (`../turmeric-spices` at HEAD, all four typed modules + ecs; the
full fixture corpus; stdlib) for calls to the existing four ops, for
hand-maintained derived rows that a `row-diff` would replace, and for
wish-comments near row code; read the ECS bounds plan (the nearest named
future consumer).  Findings:

- The ONLY call sites of `row-concat`/`row-union`/`row-intersect`/`row-canon`
  anywhere are turmeric's own fixtures (`hkt-row-ops-labeled` et al.).  The
  five spice modules (frame, sqlite, httpd, postgres, ecs) use rows purely as
  PHANTOM literals in ascriptions -- `ecs/query.tur` *mentions*
  row-union/row-intersect for future query joins in a docstring, but no code
  composes rows anywhere.
- No workaround shapes exist: no module hand-maintains a row that is another
  row minus/plus elements, and no TODO/wish comment touches rows.
- The ECS component-set-bounds plan explicitly wants a STRUCTURAL `has`
  constraint solved by field lookup, not a row operation, and is itself
  gated on a profile that has not happened.

So per this plan's own rule, `row-diff` and `row-contains` stay unadded.  The
trigger to re-run this survey is a spice actually calling the row algebra --
today even the existing four ops have no consumer outside the test suite.
(The survey also finally applied the Documentation-hygiene rule below to this
plan's own motivating example: the two closed gaps in
`docs/archive/history/variadic-hkt-rows-missing.md` -- strict row-element
resolution and row-polymorphic `defn`, both re-verified live -- are now
struck in place.)

**Demand survey re-run, 2026-10-24: still zero.** Same method. The only
`row-concat`/`row-union`/`row-intersect`/`row-canon` occurrences in either
repo are turmeric's compiler, guides and fixtures (`hkt-row-ops-*`,
`hkt-row-canon-permutation`, `hkt-row-ecs-query`, the `errors/row-op-*` /
`row-canon-*` / `row-union-*` set); spices-side the sole mention is still the
`ecs/query.tur:210` docstring. No `row-diff` / `row-contains` request, TODO or
workaround anywhere, and nothing under `docs/reported/` touches rows. The two
spice commits since 2026-08-17 that touch `#row{` lines (`e063292`, `97260e8`)
only follow a diagnostic-text change and add a doc. The ECS bounds plan that
was the nearest named consumer is archived (2026-09-09), resolved negatively.
Probe: `(row-diff ...)` and `(row-contains ...)` in a `^&` argument are still a
`TUR-E0012` "cannot apply a type of kind '*'" -- neither exists.

**Rationale.** Adding an operation is roughly an afternoon: one function in
`types.c` beside `type_typerow_intersect`, one declaration in `types.h`, one
`rop` branch in `elab_types.c:1120-1124` (plus its case in the fold switch at
`:1158-1161`), unit coverage in
`tests/compiler/test_typerow.c`, one run fixture, one error fixture for a
non-row operand. Four builtins that cover real demand beat a general facility
covering hypothetical demand.

**Two candidates already implied by the code**, neither yet requested by a
consumer -- do not add them speculatively:

- `row-diff` (set difference) -- the obvious hole in the concat/union/intersect
  trio.
- `(row-contains R E)` -- `type_typerow_contains` already exists in C with no
  surface spelling. Note this returns a *proposition*, not a row, so it needs a
  decision about where a type-level boolean can appear. That makes it strictly
  larger than it looks; prefer a constraint form over a type-level bool.

**Acceptance for any new builtin.** A run fixture proving the computed row is
`type_eq` to the explicit literal it should equal; an error fixture for a
non-row operand; unit coverage of the element-level contents in
`test_typerow.c` (the run fixtures deliberately validate only parse/dispatch/
kind, not contents).

**Exit condition.** If the builtin count passes roughly eight and is still
climbing, that is the signal to consider R3 -- and by then the accumulated
builtins will have told you what its primitives should be.

## R2 -- Follow-up (NOT scheduled): do not reify rows as values before v1

**What it would mean.** A `Row r` value-wrapper letting a row be passed,
inspected, or matched at runtime.

**Cost, and a correction to the received wisdom.** The archived spec says
reifying rows "needs witnesses ... a Layer 6+ concern"
(`variadic-hkt-rows-missing.md:325-330`), which reads as though the machinery
does not exist. It does. Witness tables are already built and emitted for
existentials -- `ensure_exists_byval_witness_dict`
(`src/compiler/emit_module.c:678`), the per-method fn-ptr dict struct (`:795`),
`EX_EXISTS_PACK` (`:9491`) -- and typeclass dictionaries are already real
runtime values (`dict_<Class>_<T>_singleton`, emitted at
`src/compiler/emit_stmt.c:1381`, stored into a pack's witness table at
`emit_expr.c:20940`) passed as parameters and captured into closures. They simply have no surface
form. So reifying rows is mostly plumbing existing witness infrastructure to a
new client.

**Why it is still a no.** The cost is not the machinery, it is the
indirection. `docs/archive/ecs-component-set-bounds-plan.md:220-222` makes
"a codegen assertion that the specialized body contains no dictionary load" a
hard *acceptance criterion* -- avoiding that indirection is the entire
justification for the ECS's structural approach over an instance-based one.
Reifying rows reintroduces exactly what that plan is built to remove.

And the demand is thin: one known consumer shape (ECS queries), zero stdlib
use. The ECS plan reaches this same fork and concludes at `:208-212`: *"Narrow
is probably right: the general feature is much larger and the ECS is the only
known consumer."*

*Note, 2026-10-24:* the ECS plan was archived 2026-09-09, resolved
negatively -- the ECS profile it waited on measured twenty million dictionary
lookups inside run-to-run noise, and the structural `has` bound stays
unscheduled. So the indirection argument above is weaker than written (the
dictionary load was not measurable there), and R2's "no" now rests mainly on
the demand side: still one consumer shape, still zero stdlib use, and no
consumer has asked for runtime row discrimination.

**Revisit when.** A second independent consumer needs runtime row
discrimination AND can tolerate witness passing. Not before. (Not met,
2026-10-24.)

## R3 -- Follow-up (NOT scheduled): types as compile-time values

**The precise gap.** Macros cannot compute with types. The comptime evaluator
(`src/compiler/elab_macros.c`, ~2190 lines; `ct_eval_form:1077`,
`ct_eval_call:645`, `ct_eval_builtin:489`) operates on surface syntax only,
because its value domain says so:

> `CtValue` (`src/compiler/elab_internal.h:1648-1663`) is a two-tag union:
> `Form *` or `CtFn *`. **There is no `Type` case.**

That is the whole answer in one line, and it is also the shape of the fix: any
type-level metaprogramming story starts by growing `CtValue` a `Type` case.

The builtin set (now the `CT_BUILTIN_TABLE` X-macro, `elab_macros.c:331-351`)
is `first`, `rest`, `second`, `nil?`, `empty?`, `list?`, `vec?`,
`symbol-name`, `type-ann?`, `type-ann-inner`, `dot-sym`, `str->sym`,
`str-append`, `cons`, `list`, `vec`, `map`, `=`, `not`, `string?`. The two
type-adjacent entries (`:340-341`) only peek at a *syntactic* `: T` annotation
and hand back the inner form. There is no way to ask what type an expression
has, no type-level `eval`, no `comptime`/`const` block, no compile-time
arithmetic beyond `=` / `not`.

One internal `Type -> Form` reflector exists -- `type_to_form`
(`src/compiler/elab_typeclasses.c:131-200`), used by typeclass dispatch
synthesis to mint ascription forms. It is explicitly partial ("Supports only
the kinds the dispatch synthesis actually needs", `:132-134`; returns NULL
otherwise, `:199`) and has no exposed inverse. If R3 is ever built, this is the
seed to generalize.

*Update, 2026-10-24 -- the bounded first step shipped elsewhere.*
`docs/archive/macro-system-direction-plan.md` (archived 2026-09-09) took the
direction this section points at and ruled on it: the `CtValue` evaluator above
is **frozen** by policy (no new builtins, no `Type` case, no arithmetic), and
new macro expressiveness goes to procedural macros running the full language on
turi. Under *this* section's ruling (bounded, total, never a general
evaluator) it landed one reflection accessor, `syntax-struct-fields`
(`src/turi/macro_env.c:89-219`, native at `:109`): a macro-env-only native
returning a single-constructor record's field names as a syntax list, with no
`Type` value ever crossing into an evaluator. Pinned by
`tests/fixtures/macro-reflect-derive/`. `docs/guides/macros-guide.md:419-423`
cites R3 as the rationale for the read-direction limit. So "macros cannot ask
what type an expression has" still holds; what changed is that the bounded
accessor shape is now proven, and any further reflection grows that native
set, not `CtValue`.

**Why it is not scheduled.** Nothing in `docs/upcoming/` proposes comptime,
staging, or type-level eval (re-checked 2026-10-24). `docs/design/tuple-variadic-vs-hlist.md:1-50`
already rates this whole direction as *"roughly as likely to ship as dependent
types -- i.e. probably not, certainly not before 1.0."* If it were built, the
right shape is a **bounded, total** type-level function form (structurally
recursive over rows, guaranteed terminating), never a general evaluator.

**Adjacent, and also closed:** dependent types (the converse -- values in
types) were deferred with a written rationale at
`docs/guides/advanced-type-system-rationale.md:418-458`: Pi types need a
dependent unification engine ("a significant research implementation problem,
not just engineering"), a proof-erasure pass, and interaction design against
GADTs/HKT/effect rows. Cost Very High, demand Low. Sized types and `StaticInt`
already cover the cases users actually reach for; `docs/guides/gadts-guide.md:744`
states the boundary.

## Explicitly out of scope

- **General runtime reflection.** The existing runtime tag is a flat `TypeKind`
  enum with no parameters and no identity -- `vec<int>` and `vec<cstr>` share a
  tag. It backs `(type-of x)` / `(is? x T)` / `(cast x T)` on `any`-typed values
  only (`src/compiler/emit_expr.c:10310-10346`, the `EX_ANY_TYPE_OF` /
  `EX_ANY_IS` / `EX_ANY_CAST` cases). Growing it into structured type
  descriptors contradicts monomorphization, which erases types by construction.
  One documented cost is accepted as a consequence: the `RCK_OPAQUE` blind spot
  in the cycle collector is "permanent without runtime type reflection"
  (`docs/archive/gc-cycle-collection-followup-plan.md:641`). If that needs
  closing, do it with a per-opaque trace callback -- a value supplying its own
  behavior, not a type descriptor.

## Documentation hygiene

The root cause here was an archived doc serving as the de-facto reference. Two
habits follow, both cheap:

- When a gap listed in a `docs/archive/history/` report is closed, **strike it
  in the archived doc** rather than relying on the reader to notice the file is
  archived. A resolved report that still reads as a live gap list is worse than
  no document.
- When a feature graduates from "spec-shaped" to "in use," its user-facing home
  is a guide, not the plan that built it.

This is not a rows-specific failure. See
`docs/archive/mono-specs-header-comment-stale.md` for the same pattern in a
source comment.

## References

- `docs/guides/row-types-guide.md` -- the dedicated rows guide (user-facing
  home since R0)
- `docs/guides/hkt-guide.md` -- the rows section added by R0 (`:485`)
- `docs/archive/macro-system-direction-plan.md` -- the frozen CT evaluator and
  the bounded `syntax-struct-fields` reflection landed under R3's ruling
- `docs/archive/history/variadic-hkt-rows-missing.md` -- Layers 1-6, the
  original spec; **contains stale gap claims**
- `docs/archive/history/row-type-in-value-position-loses-elements.md` -- why
  guard 2 exists
- `docs/archive/ecs-component-set-bounds-plan.md` -- the narrow-vs-general
  fork, and the no-dictionary acceptance criterion
- `docs/archive/spices-type-features-uplift-plan.md:94-130` (typed-field rows),
  `:465-556` (row-typed schemas; httpd/postgres/json object-shape rows were
  left as per-spice follow-ups when that plan archived)
- `docs/guides/advanced-type-system-rationale.md:418-458` -- dependent types
