# Plan: Type-Level Row Follow-Ups

> **Status:** OPEN. Near-term task (R1) is small and demand-driven; the two
> follow-ups (R2, R3) are deliberately NOT scheduled and are recorded here so
> the reasoning survives.
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
`defstruct` params." That is false -- `src/compiler/elab_fns.c:3006-3010`
handles `^&name` in the `defn` type-param vector (L6 follow-up B) and two
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

## Current state (as verified, 2026-07-28)

**Representation.** `TY_TYPEROW`, kind `KIND_TYPEROW` (`[*]`, spelled `^&`).
`src/compiler/types.h:183-188` states the invariant plainly: a row "is never
the type of a runtime value, so it erases at codegen like `TY_TYPECLASS` /
`TY_GLOBAL`." Constructors `type_typerow` / `type_typerow_named` at
`src/compiler/types.c:3595-3620`. Order-significant, duplicates preserved, no
nest-flattening, `uint8_t` element count (255 cap).

**Algebra**, all pure compile-time C (`src/compiler/types.c:3664-3790`,
declared `types.h:1454-1470`):

| C function | Surface |
|---|---|
| `type_typerow_contains` | *(none -- internal only)* |
| `type_typerow_concat` | `(row-concat A B ...)` |
| `type_typerow_union` | `(row-union A B ...)` |
| `type_typerow_intersect` | `(row-intersect A B ...)` |
| `type_typerow_canonical` | `(row-canon R)` |
| `type_typerow_eq_perm` | *(none -- never consulted by the checker)* |

Surface dispatch at `src/compiler/elab_types.c:825-890`; head-symbol
recognition at `src/compiler/elab_fns.c:2127-2140`.

**Reader.** `#row{...}` is read **ungated** at `src/compiler/reader.c:1141-1144`
(no `-Xdata-literals` requirement -- the row dispatch sits beside the
data-literal table, not inside it; confirmed by row fixtures carrying no `cmd`
file).

**The three guards.**

1. `src/compiler/elab_toplevel.c:509` -- row literal in value expression position.
2. `src/compiler/elab_fns.c:2050-2074` -- **TUR-E0012**, a value cannot have row
   type. The guard lives in the shared `fn_type_from_form` wrapper so it fires in
   every value-type sub-position.
3. `src/passes/kind_check.c:171-173` + `check_row_type_arg_kind`
   (`elab_types.c:133-162`) -- kind discipline; a row has kind `[*]`, not an
   arrow kind, so it cannot be applied.

Guard 2 exists because of a real bug: a row in value-type position was once
silently accepted and lost its elements
(`docs/archive/history/row-type-in-value-position-loses-elements.md`).

**Typed-field rows.** `#row{id : int name : cstr}` with a parallel
`field_names[]`; `TUR-E0290` (mixed positional/named), `TUR-E0291` (duplicate
name).

**Consumers.** No `stdlib/` usage at all. Rows are exercised by fixtures
(`tests/fixtures/hkt-row-*`, `typed-field-row-*`, plus ten error fixtures) and
by four modules in the sibling spices repo: `frame`, `sqlite`, `httpd`,
`postgres`.

**Known limits.** No term-level row operations (`(k in r)` membership
explicitly deferred, `docs/archive/spices-type-features-uplift-plan.md:112-116`);
not an extensible-record system (no `{ x : int | r }`, no field insert/delete/
restrict, no `Row r` wrapper); permutation equality implemented but never
applied implicitly; lenient element resolution in some positions; `deftype`
(TY_REC) carries no per-parameter kind array, so its apply sites fall back to
arity-only checking (`elab_types.c:136-141`).

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

**Rationale.** Adding an operation is roughly an afternoon: one function in
`types.c` beside `type_typerow_intersect`, one declaration in `types.h`, one
`rop` branch in `elab_types.c:866-868`, unit coverage in
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
(`src/compiler/emit_module.c:503`), the per-method fn-ptr dict struct (`:604`),
`EX_EXISTS_PACK` (`:4343`) -- and typeclass dictionaries are already real
runtime values (`dict_<Class>_<T>_singleton`, `emit_module.c:1770,4343-4350`)
passed as parameters and captured into closures. They simply have no surface
form. So reifying rows is mostly plumbing existing witness infrastructure to a
new client.

**Why it is still a no.** The cost is not the machinery, it is the
indirection. `docs/archive/ecs-component-set-bounds-plan.md:121-123` makes
"a codegen assertion that the specialized body contains no dictionary load" a
hard *acceptance criterion* -- avoiding that indirection is the entire
justification for the ECS's structural approach over an instance-based one.
Reifying rows reintroduces exactly what that plan is built to remove.

And the demand is thin: one known consumer shape (ECS queries), zero stdlib
use. The ECS plan reaches this same fork and concludes at `:109-113`: *"Narrow
is probably right: the general feature is much larger and the ECS is the only
known consumer."*

**Revisit when.** A second independent consumer needs runtime row
discrimination AND can tolerate witness passing. Not before.

## R3 -- Follow-up (NOT scheduled): types as compile-time values

**The precise gap.** Macros cannot compute with types. The comptime evaluator
(`src/compiler/elab_macros.c`, ~1600 lines; `ct_eval_form:873`,
`ct_eval_call:492`, `ct_eval_builtin:355`) operates on surface syntax only,
because its value domain says so:

> `CtValue` (`src/compiler/elab_internal.h:1026-1071`) is a two-tag union:
> `Form *` or `CtFn *`. **There is no `Type` case.**

That is the whole answer in one line, and it is also the shape of the fix: any
type-level metaprogramming story starts by growing `CtValue` a `Type` case.

The builtin set (`elab_macros.c:356-485`) is `first`, `rest`, `second`, `nil?`,
`empty?`, `list?`, `vec?`, `symbol-name`, `type-ann?`, `type-ann-inner`,
`dot-sym`, `str->sym`, `str-append`, `cons`, `list`, `vec`, `=`, `not`. The two
type-adjacent entries (`:402-416`) only peek at a *syntactic* `: T` annotation
and hand back the inner form. There is no way to ask what type an expression
has, no type-level `eval`, no `comptime`/`const` block, no compile-time
arithmetic beyond `=` / `not`.

One internal `Type -> Form` reflector exists -- `type_to_form`
(`src/compiler/elab_typeclasses.c:111-140`), used by typeclass dispatch
synthesis to mint ascription forms. It is explicitly partial ("supports only
the kinds the dispatch synthesis needs", returns NULL otherwise, `:139`) and
has no exposed inverse. If R3 is ever built, this is the seed to generalize.

**Why it is not scheduled.** Nothing in `docs/upcoming/` proposes comptime,
staging, or type-level eval. `docs/design/tuple-variadic-vs-hlist.md:1-50`
already rates this whole direction as *"roughly as likely to ship as dependent
types -- i.e. probably not, certainly not before 1.0."* If it were built, the
right shape is a **bounded, total** type-level function form (structurally
recursive over rows, guaranteed terminating), never a general evaluator.

**Adjacent, and also closed:** dependent types (the converse -- values in
types) were deferred with a written rationale at
`docs/guides/advanced-type-system-rationale.md:387-424`: Pi types need a
dependent unification engine ("a significant research implementation problem,
not just engineering"), a proof-erasure pass, and interaction design against
GADTs/HKT/effect rows. Cost Very High, demand Low. Sized types and `StaticInt`
already cover the cases users actually reach for; `docs/guides/gadts-guide.md:577`
states the boundary.

## Explicitly out of scope

- **General runtime reflection.** The existing runtime tag is a flat `TypeKind`
  enum with no parameters and no identity -- `vec<int>` and `vec<cstr>` share a
  tag. It backs `(type-of x)` / `(is? x T)` / `(cast x T)` on `any`-typed values
  only (`src/compiler/emit_expr.c:3130-3160`). Growing it into structured type
  descriptors contradicts monomorphization, which erases types by construction.
  One documented cost is accepted as a consequence: the `RCK_OPAQUE` blind spot
  in the cycle collector is "permanent without runtime type reflection"
  (`docs/archive/gc-cycle-collection-followup-plan.md:565`). If that needs
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

- `docs/guides/hkt-guide.md` -- the new rows section (R0)
- `docs/archive/history/variadic-hkt-rows-missing.md` -- Layers 1-6, the
  original spec; **contains stale gap claims**
- `docs/archive/history/row-type-in-value-position-loses-elements.md` -- why
  guard 2 exists
- `docs/archive/ecs-component-set-bounds-plan.md` -- the narrow-vs-general
  fork, and the no-dictionary acceptance criterion
- `docs/archive/spices-type-features-uplift-plan.md:94-137` (typed-field rows),
  `:469-560` (row-typed schemas; httpd/postgres/json object-shape rows OPEN)
- `docs/guides/advanced-type-system-rationale.md:387-424` -- dependent types
