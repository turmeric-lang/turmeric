# Effect Rows — Full Enforcement Plan (ER0–ER6)

> **Status:** ER0–ER5 complete. ER6 core items complete (`try-with`, `--dump-effects`, `--lint-effects`).
> ER5 fully complete: `TUR-E0021` assigned (PR5-1 ✅), cross-module private-effect
> row filtering implemented (PR5-2 ✅), `export`/`import` effect lists implemented (PR5-3 ✅).
> ER6 advanced items (static one-shot, stdlib annotation) still planned.
>
> **Prerequisites:** Phase 19 algebraic effects complete (shift/reset substrate,
> handler dispatch runtime, `EffectRow` types, `effect_check.c` inference pass,
> row-variable substitution). All prerequisite infrastructure is in `src/effect.h`,
> `src/effect_check.c`, and `src/effect_lower.c`.
>
> **Related:** [../guides/advanced-type-system-rationale.md](../guides/advanced-type-system-rationale.md)
> (§8 Effect Types / Row Polymorphism, §1 Linear Types)
>
> **Last updated:** 2026-05-15

---

## Motivation

Phase 19 shipped `#{Effect}` annotations that are **inferred and checked** against
annotated functions. However several properties remain advisory or unimplemented:

| Property | Today | Goal |
|---|---|---|
| Annotated function performs unlisted effect | **Error** (ER0 done) | ✅ keep |
| Unannotated function performs any effect | **Silent** (open row) | ER1: `--strict-effects` opt-in |
| `#{}` pure annotation enforced | **Partial** — annotation stored, checked | ER1: fully reject `perform` |
| Row variable `#{e}` propagation | **Permissive** — stored, not unified | ER2: enforce via unification |
| Higher-order effect propagation | **Partial** — only resolved rows propagate | ER2: full row-variable binding |
| Typeclass method effect rows | **Advisory** — not checked | ER3: instance must match declared row |
| `fn []` subtype with narrower row | **Not checked** | ER4: row subtyping |
| Private effects escape module | **Not checked** | ER5: module-level visibility |
| `try-with` sugar | **Missing** | ER6: surface syntax |
| Capability field effect polymorphism | **Not implemented** | ER6 |

---

## Architecture Overview

```
src/reader.c        ─ parses #{...} → ERK_UNRESOLVED
src/elab.c          ─ elaborates defn/defeffect; stores EffectRow on FnDef
src/effect_lower.c  ─ perform → shift, handle → reset; populates EffectEnv
src/effect_check.c  ─ fixed-point inference + declared-vs-inferred check (TUR-E0009)
src/effect.h/.c     ─ EffectRow algebra (empty, concrete, var, union, unresolved)
                      EffectRowSubst for row-variable unification
src/types.h         ─ effect_row field on fn Type; subtype relation
src/typeclass.c     ─ dict passing; will carry effect rows on methods (ER3)
```

The existing inference pass (`effect_check_pass`) already:
1. Resolves `ERK_UNRESOLVED` rows after `PASS_EFFECT_LOWER` populates the env.
2. Runs a fixed-point propagation through the call graph.
3. Absorbs handled effects at `EX_HANDLE` nodes (effect re-opening).
4. Emits `TUR-E0009` when `inferred ⊄ declared`.

---

## Phase ER0 — Baseline enforcement ✅ Complete

**What was delivered in Phase 19 (already done):**

- `EffectRow` type with `ERK_EMPTY`, `ERK_CONCRETE`, `ERK_VAR`, `ERK_UNION`,
  `ERK_UNRESOLVED` kinds (`src/effect.h`).
- `effect_check_pass`: fixed-point call-graph inference; absorb at `EX_HANDLE`;
  `TUR-E0009` on mismatch (`src/effect_check.c`).
- `EffectRowSubst` / `effect_row_unify` / `effect_row_apply_subst` for row-variable
  instantiation (`src/effect.h`).
- `#{Effect ...}` and `#{e}` parsed, resolved, stored on `FnDef`.
- Fixtures: `effect-row-defn.tur`, `effect-row-poly.tur`, `effect-extern-c-row.tur`,
  `errors/effect-row-mismatch/`.
- `Unsafe` effect propagated from raw-pointer dereferences and unsafe casts.
- `is_private` / `defining_module_name` fields on `Effect` for module visibility
  (not yet enforced — see ER5).

**Exit criterion:** ✅ Annotated functions with wrong effects produce `TUR-E0009`.

---

## Phase ER1 — Pure-function enforcement and `--strict-effects`

**Goal:** Make `#{}` a hard purity guarantee, and offer an opt-in mode where
*all* unannotated functions are treated as open-row (explicitly tracked), not
silently unconstrained.

### Compiler flag

- [ ] Add `--strict-effects` flag; stored in `CompilerOptions`.
- [ ] Under `--strict-effects`, unannotated functions emit a warning (`TUR-W0030`)
  when their inferred row is non-empty. This nudges gradual annotation without
  breaking existing code.
- [ ] Under `--strict-effects`, calling an unannotated function from an annotated
  function propagates the callee's inferred row (already done) **and** treats it
  as if it were declared — i.e., generates no suppressed-warning gap.

### Pure annotation enforcement

- [ ] `#{}` (empty row) on a `defn` is already parsed as `ERK_EMPTY`; the check
  pass already validates it via `effect_row_is_subset`. Confirm round-trip works
  for closures and inner defns.
- [ ] Inner `(fn [...] ...)` literals inherit the enclosing function's row when
  no annotation is given. Currently they are unconstrained.
- [ ] `(fn [...] #{} ...)` closure annotation: emit `TUR-E0009` if the closure
  body performs any effect.
- [ ] Error message improvement: when the inferred row is empty but the declared
  row is non-empty (over-annotation), emit `TUR-W0031` ("function declares
  `#{Write}` but never performs it").

### `main` purity

- [ ] `main` has no explicit annotation; under `--strict-effects` it is treated
  as having an implicit open row (any unhandled effect leaks to the OS).
- [ ] Provide a convention: annotating `main` with `#{}` is a correctness
  assertion that all effects are handled before return. Emit `TUR-E0009` if any
  effect escapes `main`.

### Fixtures

- [ ] `effect-pure-closure.tur` — `(fn [] #{} ...)` closure rejected when body
  performs an effect.
- [ ] `effect-strict-mode.tur` — `--strict-effects` warns on unannotated effectful
  function.
- [ ] `effect-main-pure.tur` — `main [] #{}` accepted when all effects handled.
- [ ] Negative: `errors/effect-main-leak.tur` — `main [] #{}` rejected when
  effect escapes.
- [ ] Negative: `errors/effect-over-annotated.tur` — `TUR-W0031` for declared but
  never-performed effect.

**Exit criterion:** `#{}` on a `defn` or closure is enforced by the check pass;
`--strict-effects` propagates warnings to unannotated callers.

---

## Phase ER2 — Row-variable enforcement and higher-order propagation

**Goal:** Row variables (`#{e}` where `e` is lowercase) are unified at call sites
so that a higher-order function's row constraint propagates through the passed
function.

### Background

Currently `#{e}` is stored as `ERK_VAR` and handled permissively
(`effect_row_apply_subst` substitutes known bindings, but binding is only done
for resolved concrete rows). A function declared `#{e}` is treated as having an
open row.

### Row-variable unification at call sites

- [ ] When calling a function `f` with signature `(fn [g :(fn [] #{e} :T)] #{e} :R)`,
  bind the row variable `e` to the inferred row of the actual argument passed for
  `g` at each call site.
- [ ] This binding is per-call-site (not global), consistent with the existing
  `EffectRowSubst` design.
- [ ] `effect_row_unify` (`src/effect.h`) is already implemented; wire it into
  the call-site inference in `collect_effects_in_expr` for `EX_CALL` nodes.
- [ ] When a function-typed argument is a closure literal, its body is
  re-analysed in the current substitution scope (avoids false positives).

### Callee row propagation from row variables

- [ ] After unifying the row variable with the actual argument's row, apply the
  substitution to the callee's declared row and merge the result into the
  caller's inferred row.
- [ ] Example:
  ```turmeric
  ;; run-twice propagates the row of f to its caller
  (defn run-twice [f : (fn [] #{e} : int)] #{e} : int
    (+ (f) (f)))

  ;; Inferred row of the call below is #{Ask}
  (handle
    (run-twice (fn [] (perform (Ask))))
    (Ask [] k) (resume k 42))
  ```
- [ ] When the actual argument is not a closure but a named function, look up that
  function's declared or inferred row and unify.

### Constraints on row variables across defn bodies

- [ ] A `defn` annotated `#{e}` that calls `(perform (Write ...))` inside its
  body: the `Write` effect is added to `e`'s binding; this updates the caller's
  row when the defn is instantiated.
- [ ] Under `--strict-effects`, emit `TUR-W0032` ("row variable `e` is always
  concrete `{Write}`; consider replacing `#{e}` with `#{Write}`") as a hint.

### Fixtures

- [ ] `effect-row-ho.tur` — `map`-style higher-order function with row variable;
  verify caller's inferred row includes mapped function's effects.
- [ ] `effect-row-compose.tur` — two functions with different row variables
  composed; verify union propagates correctly.
- [ ] `effect-row-var-unused.tur` — row variable that is never bound stays open.
- [ ] Negative: `errors/effect-row-var-mismatch.tur` — row variable bound to
  `{Write}` but caller declares `#{}` → `TUR-E0009`.

**Exit criterion:** Higher-order functions with row-variable annotations correctly
propagate their argument's effects to callers; unification is enforced, not just
stored.

---

## Phase ER3 — Typeclass method effect rows

**Goal:** `defclass` method signatures may carry effect row annotations, and
`definstance` bodies are checked against them.

### Background

The `typeclass-effect-row.tur` fixture shows that `#{Write}` on a method is
currently advisory. The elaborator stores the annotation on the method signature's
`Type` but `typeclass.c` does not pass it to `effect_check_pass`.

### Typeclass method row annotation

- [ ] `defclass` method syntax already accepts `#{...}` after argument list (parsed
  in `src/reader.c`). Verify the annotation is stored in the `TypeClass` struct's
  method signature (`src/typeclass.h`).
- [ ] Add `effect_row` field to `MethodSig` in `src/typeclass.h` if not present.
- [ ] In `effect_check_pass`, include method bodies from `definstance` forms
  (`EX_DEFINSTANCE` nodes). The declared row for an instance method is the row
  from the typeclass's method signature.
- [ ] Emit `TUR-E0009` if an instance method body's inferred row is not a subset
  of the declared method row.

### Calling effectful methods from pure contexts

- [ ] When resolving a method call in `collect_effects_in_expr`, look up the
  method's declared row from the typeclass and merge it into the caller's row.
- [ ] This enables: a function calling `.io-show` on an `IOShow`-constrained type
  has `#{Write}` added to its inferred row automatically.
- [ ] Error: calling `.io-show` from a function declared `#{}` → `TUR-E0009`.

### Default method bodies

- [ ] If a `defclass` provides a default method body, check it against the method's
  declared effect row.

### Fixtures

- [ ] `typeclass-effect-row-enforced.tur` — instance method body that violates
  declared row produces `TUR-E0009`.
- [ ] `typeclass-effect-row-caller.tur` — calling an effectful method propagates
  the method's row to the caller.
- [ ] `typeclass-effect-row-pure-ctx.tur` — calling an effectful method from a
  `#{}` function is rejected.
- [ ] `typeclass-effect-row-default.tur` — default method body checked against
  declared row.

**Exit criterion:** Method effect rows in typeclasses are enforced in instance
bodies and propagated to callers.

---

## Phase ER4 — Effect row subtyping in function types

**Goal:** A function type `(fn [] #{Write} :nil)` is a subtype of
`(fn [] #{Write Log} :nil)`. Assignments, capability fields, and higher-order
arguments respect this.

### Subtype relation

The existing `effect_row_is_subset` implements `r1 ⊆ r2`. The missing piece is
wiring this into the function-type subtype check in `src/types.c`.

- [ ] In `type_is_subtype` (or equivalent), for `TY_FN` types: the argument rows
  are contravariant (`r2_param ⊆ r1_param`), the return row is covariant
  (`r1_ret ⊆ r2_ret`). For the common case (no row parameters), covariant
  return row is sufficient.
- [ ] Assignments: `let f : (fn [] #{Write} :nil) = g` where `g : (fn [] #{} :nil)`
  — accepted (narrow row is a subtype of wider row).
- [ ] Capability fields: a struct field typed `(fn [] #{Write} :nil)` can hold a
  function with row `#{}` (narrower satisfies wider capability).
- [ ] Higher-order argument: `(run-twice g)` where `run-twice` expects
  `(fn [] #{Write Log} :nil)` accepts `g : (fn [] #{Write} :nil)`.

### Effect row in structural type equality

- [ ] Two function types with different effect rows are **not** equal but may be
  **subtypes** of each other. The elaborator's type unification must be updated
  to accept subtype assignments.
- [ ] Codegen: no runtime change — effect rows are erased at codegen; subtyping
  is purely a compile-time check.

### Capability field effect polymorphism (partial ER6 pull-forward)

- [ ] Struct fields that hold function pointers can carry an effect-row annotation
  on the field type:
  ```turmeric
  (defstruct Runner
    [run : (fn [] #{e} : nil)])
  ```
- [ ] A `Runner` constructed with a concrete `run` function binds `e` to that
  function's row.
- [ ] Accessing `(.run r)` propagates `e`'s row to the caller.

### Fixtures

- [ ] `effect-subtype-assign.tur` — assigning a narrow-row function to a
  wide-row variable is accepted.
- [ ] `effect-subtype-ho.tur` — passing a narrow-row function to a wide-row
  higher-order parameter is accepted.
- [ ] `effect-subtype-capability.tur` — capability field accepts narrow-row
  function.
- [ ] Negative: `errors/effect-subtype-violation.tur` — assigning a wide-row
  function to a narrow-row variable is rejected.

**Exit criterion:** Function type subtyping respects effect rows; capability fields
accept narrower-row functions.

---

## Phase ER5 — Module effect visibility

**Goal:** Effects declared with `^private` are invisible outside their defining
module; cross-module effect-row checking works correctly.

### Background

The module system (M0–M7) is in place. `Effect` already has `is_private` and
`defining_module_name` fields populated by `effect_env_register` (Phase P19-6).
The elaborator already blocks cross-module `perform` and `handle` of private
effects using `diag_emit(DIAG_ERROR, ...)`. Fixtures for the core cases exist:
`tests/fixtures/module-effect-private/` (positive), `tests/fixtures/errors/module-effect-private-access/`
(negative), and `tests/fixtures/module-cross-module-effect/` (public cross-module).

### Prerequisites — remaining work before ER5 is complete

> These items were identified during an audit on 2026-05-15. Existing module
> infrastructure satisfies the heavy-lifting; only the items below are missing.

**PR5-1: Assign `TUR-E0021` diagnostic code to private-effect errors** ✅ Complete

- [x] Add `TUR_E0021_PRIVATE_EFFECT` to the `DiagCode` enum in `src/diag.h`.
- [x] Add case to `diag_code_to_string()` in `src/diag.c` (→ `"TUR-E0021"`).
- [x] Add case to `diag_code_from_string()` in `src/diag.c`.
- [x] Add entry to `diag_explanations_[]` in `src/diag.c` so `--explain TUR-E0021`
  works.
- [x] Update the two `diag_emit(DIAG_ERROR, ...)` calls in `src/elab.c` to use
  `diag_emit_with_code(..., TUR_E0021_PRIVATE_EFFECT, ...)`.
- [x] Update `tests/run-flags.sh`: added `TUR-E0021` to the `tur-explain-all-codes` loop.
- [x] Update `tests/fixtures/errors/module-effect-private-access/expected.diag` to
  assert `TUR-E0021` in the diagnostic output.

**PR5-2: Cross-module effect-row propagation filtering** ✅ Complete

- [x] `filter_cross_module_private()` helper in `src/effect_check.c` strips
  `ERK_CONCRETE` effects whose `is_private` is true and `defining_module_name`
  differs from `s_current_analysis_module`.
- [x] File-local `s_current_analysis_module` set per-function in the fixed-point
  loop from `fn->binding->defining_module_name`.
- [x] Filter applied in `EX_CALL` after merging `callee->inferred_effect_row`.
- [x] Fixture `tests/fixtures/effect-row-cross-private/` — consumer annotated
  `#{}` calls a module function that internally performs a private effect;
  accepted without TUR-E0009 because the private effect is filtered.

**PR5-3: Effect names in `export`/`import` lists** ✅ Complete

**PR5-3-A: Add `is_exported` flag to `Effect` struct** ✅ Complete

- [x] `bool is_exported` added to `struct Effect` in `src/effect.h`.
- [x] Defaults to `true` for non-private effects, `false` for `^private` ones.
- [x] Set in `effect_env_register()` as `effect->is_exported = !is_private`.

**PR5-3-B: Extend export-list parsing to accept `(effect EffectName)` forms** ✅ Complete

- [x] `elab_defmodule` export-list pass extended to accept `(effect Name)` two-element lists.
- [x] `n_exported_effects`/`exported_effects` parallel arrays on `DefModule` (`src/expr.h`).
- [x] Validation loop after body elaboration: ensures effect belongs to this module and is not private; emits `TUR-E0021` if a private effect is listed in `(export (effect ...))`.
- [x] `sym_effect` interned symbol added to `Elab` struct and initialized in `elab_init_state`.

**PR5-3-C: Enforce effect-export visibility at use sites** ✅ Complete

- [x] Visibility guards in `elab_perform` and `elab_handle` updated from `is_private` to `!is_exported && !elab_effect_is_referred(e, effect)`.
- [x] `elab_effect_is_referred()` static helper walks `e->referred_effects` (populated by `:refer [(effect Name)]` imports).

**PR5-3-D: Cross-module effect import via `:refer [(effect Write)]`** ✅ Complete

- [x] `parse_import_spec` extended to separate plain symbols from `(effect Name)` entries in the `:refer` vector; stored in `ImportSpec.refer_effect_syms`/`n_refer_effects` (`src/expr.h`).
- [x] `ElabModule.exported_effects`/`n_exported_effects` populated in `elab_load_module` from the effect env.
- [x] Import processing loop in `elab_defmodule` resolves each `(effect Name)` entry against the loaded module's `exported_effects` and appends to `e->referred_effects`.
- [x] `Elab.referred_effects`/`n_referred_effects`/`cap_referred_effects` dynamic array added.

**PR5-3 Fixtures** ✅ Complete

- [x] `tests/fixtures/effect-export-explicit/` — `(export (effect Write))` compiles cleanly and the effect is usable; `run-flags.sh` `effect-export-syntax` test passes.

### Private effect enforcement (already done via P19-6)

- [x] `(defeffect ^private ...)` syntax parsed; `is_private` and
  `defining_module_name` set in `Effect` struct (`src/elab.c` ~line 5027, `src/effect.c` ~line 317).
- [x] `perform (PrivateEffect ...)` from outside the defining module: elaborator
  emits `DIAG_ERROR` with message "effect 'X' is private to module 'Y'"
  (`src/elab.c` ~line 5228).
- [x] `handle ... (PrivateEffect [...] k) ...` from outside: same check in
  handler parsing (`src/elab.c` ~line 5384).
- [x] Fixture `tests/fixtures/module-effect-private/` — private effect used within
  defining module passes.
- [x] Fixture `tests/fixtures/errors/module-effect-private-access/` — cross-module
  access rejected.
- [x] Fixture `tests/fixtures/module-cross-module-effect/` — public cross-module
  effect round-trip works.

### Cross-module row propagation (✅ done — PR5-2)

- [x] Private effects in a callee's inferred row are filtered at call-site
  boundaries; they do not appear in callers from other modules.
- [x] Exported effects propagate normally via `effect_check_pass` fixed-point
  inference (unchanged).

### Effect names in `export`/`import` lists (✅ done — PR5-3)

- [x] `(export (effect Write))` in `defmodule` (PR5-3-B) and
  `:refer [(effect Write)]` in import specs (PR5-3-D) implemented.

### Fixtures

- [x] `module-effect-private.tur` — private effect not accessible from importing
  module; error emitted. (exists as `tests/fixtures/module-effect-private/` and
  `tests/fixtures/errors/module-effect-private-access/`)
- [x] `module-effect-public.tur` — public effect accessible cross-module.
  (exists as `tests/fixtures/module-cross-module-effect/`)
- [x] `module-effect-row-cross.tur` — private effects filtered at call-site boundary;
  consumer annotated `#{}` compiles cleanly.
  (implemented as `tests/fixtures/effect-row-cross-private/`)

**Exit criterion:** `TUR-E0021` is assigned and `--explain TUR-E0021` works (PR5-1 ✅);
private effects do not leak into cross-module inferred rows (PR5-2 ✅); effect names in
`export`/`import` lists work via PR5-3-A through PR5-3-D (PR5-3 ✅). ER5 complete.

---

## Phase ER6 — Integration, polish, and surface ergonomics

**Goal:** Round out the effect-row system with `try-with` sugar, `--dump-effects`
diagnostics, IDE support, stdlib annotation, and the remaining deferred items.

### `try-with` sugar

- [ ] `(try-with body handler)` desugars to `(reset (handle body handler))`.
  Implement as a macro in `stdlib/effects.tur` or as a special form in
  `src/reader.c`.
- [ ] `handler` is a list of `(EffectName [params...] k) body` clauses (same
  syntax as `handle`).

### `--dump-effects` flag

- [ ] Print every top-level `defn`'s inferred effect row after `effect_check_pass`.
  Format: `fn-name : #{Effect1 Effect2}` (or `#{}` for pure).
- [ ] Useful for auditing which functions are effectful, similar to `--dump-kinds`.

### `--lint-effects` flag

- [ ] Like `--lint-unsafe`, scan for functions without row annotations whose
  inferred row is non-empty and emit advisory warnings.

### Static one-shot enforcement for `resume`

- [ ] Currently dynamic: `tur_cont_resume` sets `consumed = true` and panics on
  second call. Promote to a static check.
- [ ] Mark `k` in a handler case as a move-only binding (already `TY_CONT` with
  `CK_MOVE`). The borrow checker should report use-after-move if `k` is used
  twice. Wire `EX_RESUME` and `EX_DISCONTINUE` into the borrow checker's
  move-consumption tracking.
- [ ] Negative fixture: `errors/effect-double-resume-static.tur` — caught at
  compile time, not runtime.

### `cont?` predicate

- [ ] `(cont? x)` returns `true` if `x` holds a live (non-consumed) continuation.
  Implement as `EX_CONT_PRED` (partially present in the codebase) lowered to a
  null-check on the continuation pointer.

### Stdlib effect row annotations

- [ ] Annotate all stdlib `defn`s with effect rows. Initially under
  `--strict-effects`; promoted to mandatory in a future release.
- [ ] Priority stdlib files: `stdlib/effects.tur`, `stdlib/vec.tur`,
  `stdlib/log.tur`, `stdlib/async.tur`, `stdlib/thread.tur`.

### Capability field effect polymorphism (remainder)

- [ ] Finish ER4 capability-field work: struct fields typed `(fn [] #{e} :T)`
  bind `e` at construction time and propagate at call sites.
- [ ] `(defstruct Runner [run : (fn [] #{e} :nil)])` is parametric in `e`.
  Checked against the caller's declared row when `.run` is called.

### IDE / tooling

- [ ] `--check` mode reports effect-row errors without emitting code.
- [ ] Language server hover: show inferred effect row for a `defn`.
- [ ] Inline hint: display `#{...}` annotation suggestion for unannotated
  effectful functions (under `--strict-effects`).

### Fixtures

- [ ] `try-with-basic.tur` — `try-with` sugar works for Write/Read effects.
- [ ] `try-with-nested.tur` — nested `try-with` handlers.
- [ ] `effect-dump.tur` — `--dump-effects` output matches expected rows.
- [ ] `effect-cont-pred.tur` — `cont?` returns correct values (extends existing
  `effect-cont-pred.tur`).
- [ ] `effect-double-resume-static.tur` — static use-after-move on `k`.
- [ ] `stdlib-effects-annotated.tur` — stdlib `Write`/`Read`/`Fail`/`Log`/`Abort`
  functions all have row annotations; callers propagate correctly.

**Exit criterion:** `try-with` works; `--dump-effects` / `--lint-effects` produce
useful output; static one-shot check catches double-resume at compile time;
priority stdlib functions are fully annotated.

---

## Relationship to Advanced Type System Plan

The ER0–ER6 phases are the **near-term enforcement layer** for effect rows. The
[advanced type system feasibility plan](../guides/advanced-type-system-rationale.md)
describes a longer-horizon effort (§8 — Effect Types / Row Polymorphism, phases
ET0–ET4) that extends this work to full effect polymorphism. Key alignment:

| ER Phase | Corresponding ET Phase | Notes |
|---|---|---|
| ER1 (`--strict-effects`, `#{}`) | ET0 (effect row syntax in fn types) | ER1 lays the foundation |
| ER2 (row-variable unification) | ET1 (row checking) + ET2 (effect polymorphism) | ER2 covers simple `#{e}` vars; full `forall [e]` quantification requires HRT1 |
| ER3 (typeclass method rows) | ET3 (handler typing) | ER3 checks instances; ET3 covers handler composition |
| ER4 (row subtyping) | ET1 (effect row subtyping) | ET1 will formalise the full subtype lattice |
| ER6 (static one-shot `resume`) | LT0–LT4 (Linear Types) | Static `k` enforcement is a stepping stone toward first-class linear continuations |

### Prerequisites for Full Effect Polymorphism

ER2 introduces row variables (`#{e}`) with per-call-site unification, but does **not**
support `forall [e]`-quantified effect polymorphism (e.g., a function that is
polymorphic over its effect row in the full Koka/Eff sense). That requires:

- **HRT Phase HRT1** (Rank-2 types) from the v2 roadmap — enables quantifying over
  effect rows in function signatures.

ER2 is intentionally conservative: it covers the common higher-order cases without
requiring rank-N types, and is forward-compatible with ET2 once HRT lands.

### Feature Flag

The advanced plan reserves **`-Xeffect-types`** as the umbrella flag for the full
ET0–ET4 feature set (targeting v3). Once stable, this flag subsumes `--strict-effects`
(ER1) and the row-variable work (ER2). Until then, `--strict-effects` is the operative
opt-in flag.

### Error Code Allocation

The advanced type system plan reserves **`TUR_E0250`–`TUR_E0299`** for Effect Types
errors introduced by ET0–ET4. This range is distinct from the codes used by the ER
phases:

| Error / Range | Assigned in |
|---|---|
| `TUR-E0009` (inferred ⊄ declared) | ER0 / Phase 19 |
| `TUR-W0030`–`TUR-W0032` | ER1–ER2 |
| `TUR-E0021` (private effect) | ER5 |
| `TUR_E0250`–`TUR_E0299` | ET0–ET4 (advanced plan, reserved) |

---

## Summary Roadmap

| Phase | Goal | Key Files | Status |
|---|---|---|---|
| ER0 | Baseline inference + `TUR-E0009` | `effect_check.c`, `effect.h` | ✅ Complete |
| ER1 | Pure `#{}` + `--strict-effects` | `effect_check.c`, `main.c` | ✅ Complete |
| ER2 | Row-variable unification + HO propagation | `effect_check.c`, `effect.h` | ✅ Complete |
| ER3 | Typeclass method effect rows | `typeclass.c`, `typeclass.h`, `effect_check.c` | ✅ Complete |
| ER4 | Row subtyping in function types | `effect_check.c`, `emit.c` | ✅ Complete |
| ER5 | Module effect visibility | `elab.c`, `effect_check.c`, `diag.h` | ✅ Complete (PR5-1, PR5-2, PR5-3) |
| ER6 | `try-with`, `--dump-effects`, `--lint-effects` | `effect_check.c`, `main.c` | ✅ Complete (core items) |

## Dependencies

```
ER0 (done)
  └─ ER1 (pure enforcement, flag)
       └─ ER2 (row-var unification)
            ├─ ER3 (typeclass rows)   ← also needs Phase 15 typeclasses
            └─ ER4 (subtyping)
                 └─ ER5 (modules)    ← also needs M0–M7 modules
                      └─ ER6 (polish)
```
