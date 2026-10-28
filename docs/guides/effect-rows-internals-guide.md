---
title: Effect Rows (internals)
category: Compiler Internals
description: How effect rows are represented, parsed, inferred and checked -- the EffectRow kinds, the EffectEnv registry, the elaborator's effect checks, the effect-row pass and its fixpoint, capabilities and Unsafe, --dump-effects, and where codegen reads the rows
tags:
  - effects
  - type-checking
---

# Effect Rows (internals)

This guide is for contributors working on how the compiler **types** effects:
how an `#fx{...}` annotation becomes a data structure, how a function's row is
inferred from its body, which diagnostics check rows against each other, and
which later stages read the result. How effects are **lowered** to C --
coloring, the CT-IR, the DK machine -- is the
[CPS and DK internals guide](cps-dk-internals-guide.md). The user-level view
is the [effects system guide](effects-system-guide.md), in particular its
[Effect Rows](effects-system-guide.md#effect-rows-typed-effects) and
[Capability effect tags](effects-system-guide.md#capability-effect-tags-capability)
sections. Type-level `#row{...}` rows are an unrelated feature
([row types guide](row-types-guide.md)).

> **Trust the code over the header comments.** Some comments in this area
> predate the current design: the `EffectRow` header comment in `effect.h`
> still describes "Phase 19 v1". Where a comment and this guide disagree, read
> the code; where the code disagrees with this guide, fix the guide.

---

## The short version

1. **Parse.** The elaborator reads every `#fx{...}` into an `ERK_UNRESOLVED`
   row: a list of symbol names plus the span they came from. It never builds a
   resolved row itself.
2. **Elaborate.** `elab_effects.c` types `defeffect`, `perform`, `handle` and
   friends. Its checks are scope- and arity-based, not row-based: TUR-E0008
   (top-level unhandled perform), TUR-E0021 (private effect), TUR-E0014
   (cloneable capture without `Clone`), handler result and arity checks.
3. **Register.** `PASS_EFFECT_ENV` builds a fresh `EffectEnv` holding the
   builtin `Unsafe` and the five compiler-known capabilities. The tree is
   untouched.
4. **Infer and check.** `PASS_EFFECT_ROW_INFER` runs
   `src/passes/effect_check.c:effect_check_pass`. It registers every
   `defeffect`, resolves each declared row (TUR-E0026 on an unknown name),
   infers every function's row with a whole-program fixpoint, and checks
   declared against inferred (TUR-E0009, TUR-W0030/31/32/33, TUR-E0254).
5. **Consume.** CPS coloring, the CT-IR translator and the emitter read
   `FnDef.inferred_effect_row` and the declared row to decide whether a
   function can perform anything at run time.

---

## Source map

| File | Role |
|---|---|
| `src/passes/effect.h`, `effect.c` | `EffectRow`, the `ERK_*` kinds, `Effect`/`EffectConstructor`, the `EffectEnv` registry, row algebra (merge, subset, remove, unify, substitution), builtin `Unsafe` and capability registration |
| `src/passes/effect_check.c`, `.h` | The effect-row pass: env population, row resolution (TUR-E0026), inference fixpoint, all row diagnostics, `--dump-effects` |
| `src/compiler/elab_effects.c` | `defeffect`, `perform`, `handle`/`handle-shallow`, handler values, `with-handler`, `compose-handlers`, `resume`, the continuation forms, `warn_legacy_fx_row` |
| `src/compiler/elab_fns.c` | Row annotations on `defn` (`elab_defn`) and `fn` literals (`elab_fn`); `#fx{Unsafe}` raising `unsafe_depth` for the body |
| `src/compiler/elab_types.c` | Rows inside fn types, `(fn [int] #fx{e} int)` (`type_expr_from_form`), and the handled row of `handler<...>` types |
| `src/compiler/elab_structs.c`, `elab_typeclasses.c` | Rows on fn-typed record fields and on class methods (`parse_typeclass_method`) |
| `src/compiler/elab_unsafe.c` | `(unsafe ...)` (`elab_unsafe`) and the raw-pointer builtins it gates |
| `src/compiler/elab_call.c` | The call-site `#fx{Unsafe}` gate (`elab_call_fn_inner`) |
| `src/compiler/elab_core.c` | `effect_row_contains_symbol`: name lookup on a row that may still be unresolved |
| `src/compiler/reader.c` | `read_fx_row` and the `#{...}` / `@{...}` legacy spellings (provenance tags) |
| `src/compiler/builtins.c` | `BuiltinSpec.effect` (`"IO"` on every `println` row), `builtin_effect_for_name` |
| `src/driver/pipeline.c`, `src/driver/cli.c` | `core_passes[]` and `run_core_pass`; the effect flags (`GLOBAL_FLAGS[]`) |
| `src/turi/eval.c` | The interpreter's own call of `effect_check_pass` (`turi_eval_impl`, step 6b) |

---

## Pipeline position

| Stage | What happens to rows |
|---|---|
| `PASS_ELABORATE` | Annotations parsed to `ERK_UNRESOLVED`; elaborator effect checks; `#fx{Unsafe}` gating. Effects are registered in the **elaborator's** `EffectEnv` (`e->effect_env`). |
| `PASS_EFFECT_ENV` | `run_core_pass` allocates `ctx->effect_env` -- a **second, empty** env -- and registers `Unsafe` and `IO`/`FS`/`Net`/`Proc`/`Rand`. |
| `PASS_EFFECT_ROW_INFER` | `effect_check_pass` re-registers each `EX_DEFECT` into that env, resolves, infers, checks. A non-zero return stops the build; `--dump-effects` runs only after a zero return. |
| `PASS_CPS` and emission | Read the rows; never write them. |

Every resolved row therefore points at `Effect` objects owned by the
**pass** env, not the elaborator's. Nothing compares the two envs' pointers
today because the elaborator only ever builds `ERK_UNRESOLVED` rows (and
`effect_row_contains_symbol` compares names). Keep it that way: an
`ERK_CONCRETE` row built during elaboration would hold pointers the pass's
`effect_row_contains` can never match.

The interpreter keeps one **session** env (`env->effect_env`) across REPL
turns, so a `defeffect` from the stdlib preload turn still resolves a later
turn's `#fx{Bt}`; `effect_redefine` lets a later turn redeclare an effect in
place.

---

## Representation

### `EffectRow`

`struct EffectRow` is a tagged union (`src/passes/effect.h`):

| Kind | Payload | Printed by `effect_row_print` | Meaning |
|---|---|---|---|
| `ERK_EMPTY` | -- | `{}` | no effects |
| `ERK_CONCRETE` | `Effect **effects`, `uint8_t n_effects` | `{Ask, Log}` | a set of resolved effects |
| `ERK_VAR` | `var_name` | `{e}` | a row variable |
| `ERK_UNION` | `left`, `right` | `({Ask} \| {e})` | built by `effect_row_merge` when either side is not concrete |
| `ERK_UNRESOLVED` | `sym_names`, `span`, `unknown_reported` | `#{Ask e}` | the parser's output; must be resolved before inference |

Points worth knowing:

- **A `NULL` row and `ERK_EMPTY` are interchangeable** for the row
  predicates in `effect.c`: `effect_row_is_empty(NULL)` is true. On a binding, `NULL` means *not
  annotated*; `#fx{}` builds `ERK_EMPTY` (`effect_row_unresolved` returns
  `effect_row_empty` for zero names). Only the checks distinguish them.
- **There is no "open row" kind.** `#fx{Ask e}` resolves to a `ERK_UNION` of
  `{Ask}` and the variable `e`. A row is "open" exactly when a `ERK_VAR`
  appears in it.
- **Name case decides the kind.** `row_name_is_effect` treats a name whose
  first character is not `a`-`z` as an effect and anything else as a row
  variable. `effect_row_resolve` and `effect_row_unknown_names` share it, so a
  name cannot be dropped without being reported.

### `Effect` and the `EffectEnv`

An `Effect` carries its name, one `EffectConstructor` (effects are 1:1 with
their single operation), visibility (`is_private`, `defining_module_name`),
an `^extends` `parent`, and `is_capability`. The constructor keeps parameter
and result `TypeKind`s, the full `Type`s when they are aggregates or fns
(Tier C), and `resumable_payload_param` for the multishot payload form.

`EffectEnv` is a flat array searched linearly by interned-symbol pointer
(`effect_env_lookup`). `effect_env_register` is idempotent: a second
registration of a name returns the existing effect unchanged.

### From `#fx{...}` to a row

1. **Reader.** `read_fx_row` reads `#fx{...}` as an `F_MAP` form tagged
   `PROV_FX_EXPLICIT`. Bare `#{...}` and `@{...}` are tagged
   `PROV_FX_LEGACY`/`PROV_FX_AT_LEGACY`; `warn_legacy_fx_row` turns those into
   TUR-D0002/TUR-D0003 the first time a site consumes one.
2. **Elaborator.** Each annotation site hands the `F_MAP` to
   `elab_fx_row_from_form` (`elab_effects.c`), which keeps the `F_SYM` items,
   reports any other item as TUR-E0026 at the item, and calls
   `effect_row_unresolved(arena, names, n, span)`:

   | Annotation | Parsed in | Stored on |
   |---|---|---|
   | `(defn f [..] #fx{..} : T ...)` | `elab_defn` | `Binding.type.as.fn.effect_row` |
   | `(fn [..] #fx{..} : T ...)` | `elab_fn` | the lifted lambda's binding |
   | `(fn [A] #fx{e} R)` as a type | `type_expr_from_form` | that `TY_FN` type (a parameter's type, for instance) |
   | `[run : fn #fx{E}]` record field | `elab_structs.c` | `CtorField.effect_row` |
   | a `defclass` method signature | `parse_typeclass_method` | `TypeClassMethod.effect_row` |
   | `handler<...>` / handler literal | `elab_types.c`, `elab_handler_lit` | `TY_HANDLER`'s `handled_row` (never resolved; compared by name) |

   **Only symbols survive.** A keyword, string or number inside the braces is
   dropped without a diagnostic, so `#fx{:FS}` is `#fx{}` (see
   [Known gaps](#known-gaps-and-open-reports)).
3. **Pass.** `effect_check_pass` resolves every row it can reach (below).
   `FnDef.inferred_effect_row` is always `NULL` coming out of elaboration
   ("must be NULL; effect_check_pass reads this").

### Row algebra and its conservative answers

The helpers in `effect.c` answer "unknown" generously. Read these before
trusting a result:

| Function | Notable behaviour |
|---|---|
| `effect_row_contains(row, eff)` | Also true when `eff` is a **descendant** of a row element (`^extends`), so a declared `#fx{IO}` covers a performed `Write`. False for `ERK_VAR` and `ERK_UNRESOLVED`. |
| `effect_row_is_subset(r1, r2)` | **True whenever either side is an `ERK_VAR`**, and for any kind pair it does not handle. A declared row variable therefore admits every effect. |
| `effect_row_merge` | Dedups two concrete rows into one; anything else becomes an `ERK_UNION` node. |
| `effect_row_remove(row, name)` | Concrete rows only, by **exact name** -- removing a parent does not remove its children. Unions and variables pass through unchanged. |
| `effect_row_unify` | Binds a variable (first binding wins, occurs check); every concrete/empty/union pair "succeeds" -- subset checking is left to `effect_row_check_declared`. `EffectRowSubst` holds at most 32 bindings and silently drops the rest. |
| `effect_row_is_runtime_pure` | True for empty rows and rows of only `^capability` effects; false for variables and unresolved rows. The codegen gates use this, not `effect_row_is_empty`. |
| `effect_row_without_capabilities` | Strips capability effects, for the lints that ask what a body *performs* (W0032, W0033). |

---

## Elaboration-time checks

These run while the tree is built and know nothing about inferred rows. They
are about scope, arity and the shape of handlers.

| Check | Where | Diagnostic |
|---|---|---|
| A `perform` outside any `defn`/`fn` body (`fn_body_depth == 0`) with no enclosing `handle` case of that name | `elab_perform`, using `push_handled_effect`/`is_effect_handled`; `elab_handle_impl` pre-scans its case headers before elaborating the body | `[TUR-E0008]` |
| `perform` or a handler clause naming a `^private` effect of another module, not `:refer`'d | `elab_perform`, `elab_handle_impl`, `elab_handler_lit` (and `elab_module.c`) | TUR-E0021 |
| `perform` argument count, or a clause's parameter count, differs from the `defeffect` | `elab_perform`, `elab_handle_impl` | TUR-E0002 |
| A `perform` argument disagrees with the declared parameter: primitive kind, a scalar word into a pointer-shaped slot, or aggregate shape (after the `any` unbox/widen seams) | `elab_perform` (`perform_primitive_norm`, `perform_param_is_pointer_shaped`, `perform_arg_shape_mismatch`) | TUR-E0001 |
| Clause result type differs from the handled body's (or from clause 0), compared by `TypeKind`; a clause ending in `resume` is skipped | `elab_handle_impl` | TUR-E0252 |
| `^multishot` clause captures a unique/linear binding; `^linear` `k` not consumed | `elab_handle_impl`, `elab_handler_lit` | TUR-E0500, TUR-E0100 |
| `compose-handlers` of two handlers with overlapping handled rows | `elab_compose_handlers` (`effect_row_collect_names`) | TUR-E0251 |
| A binding captured by a reified (`cloneable-`/resuming) continuation has no `Clone` instance and is not an admissible owning kind | `check_cloneable_capture_precise`, from `elab_reset` and `elab_cloneable_reset` | TUR-E0014 |
| A call of an `#fx{Unsafe}` function with `unsafe_depth == 0` | `elab_call.c:elab_call_fn_inner` | (no code) `unsafe function 'f' requires an enclosing (unsafe ...)` |
| An inline-C block outside an Unsafe context, under `--lint-inline-c-unsafe` | `elab_toplevel.c:elab_form_impl` (`F_CBLOCK`) | TUR-W0036 |

Two details:

- **TUR-E0008 is not a registered code.** It is emitted with `diag_emit`,
  and the code is part of the message text, so it prints as
  `error: [TUR-E0008]: ...` rather than `error [TUR-E0008]: ...`, and
  `tur explain TUR-E0008` answers "no explanation". Inside a function body
  there is no static unhandled-perform check: an effect that escapes an
  un-annotated `main` builds, and aborts at run time with
  `tur: unhandled effect: Ask (tag 2)`. Only a declared row on `main` catches it
  statically (TUR-E0009; `errors/effect-main-leak`).
- **TUR-E0014** fires per captured binding, at the reset:

  ```
  e0014.tur:5:5: error [TUR-E0014]: captured binding 'foo' does not implement Clone (required by cloneable-shift)
  ```

  An owning one-word handle (`rc<T>`, a `^heap` ADT or struct) or an owning
  single-constructor by-value product is admitted without `Clone`
  (`owning_multishot_admissible`); `TY_NIL`, `TY_FN` and cloneable
  continuations are always admitted.

---

## The effect-row pass (`effect_check.c`)

### Items

`effect_check_items` flattens the program the way the emitter does:
`(defmodule ...)` bodies and top-level `(do ...)` forms are spread in place,
recursively. Every lambda has been lifted to a top-level `EX_FN_DEF` by now,
so "every item" includes `__fn_N` lambdas, instance methods and class default
bodies.

### Steps

1. **Populate the env (Step -1).** Register every `EX_DEFECT` (or
   `effect_redefine` one already present), then set `parent` from
   `parent_name`, then copy `is_capability` -- three loops over the items.
   (Source order still matters, but earlier: `elab_defeffect` rejects an
   `^extends` parent it has not seen yet.)
2. **Resolve declared rows (Step 0).** For each `EX_FN_DEF`: its binding's row
   and every `TY_FN` parameter's row. Then record-field rows (`EX_DEFDATA`,
   which `defgadt` also elaborates to), class method rows, and the copy of
   each class method's row onto its instance impls and default body. Every
   resolution goes through `resolve_declared_row`, which reports unknown names
   **once per row object** (`unknown_reported`) and then calls
   `effect_row_resolve`.
3. **Index (Step 1).** An `FnIndex` -- an open-addressed hash from `Binding *`
   to `FnDef *` that doubles past half full -- over every `EX_FN_DEF`. The first
   `FnDef` added for a binding wins. (It used to be a fixed 1024-slot array;
   `errors/effect-row-past-1024-fns` pins the growth.)
4. **Infer (Step 2).** Repeat until no row changes: for every function, set
   `s_current_analysis_module`, take a fresh `EffectRowSubst`, and recompute
   `collect_effects_in_expr(body)` from the empty row. A row "changed" when it
   is not a mutual subset of the previous one.
5. **Validate (Step 3).** Per function: TUR-E0009, TUR-W0031, and under
   `--strict-effects` TUR-W0032 and TUR-W0030. Then annotated closures, the
   call-site check (3b) and unreachable clauses (3c).

The inferred row of every function is written to `FnDef.inferred_effect_row`;
resolved declared rows are written back in place onto the bindings, fields
and methods they came from. Nothing else is mutated.

### The inference walk

`collect_effects_in_expr(a, e, row, idx, env, subst)` returns `row` plus
what `e` may perform. Nested `EX_FN_DEF`s are skipped (they are items of
their own); an `EX_CLOSURE` literal's body **is** walked, so a function that
builds an effectful lambda is charged with the lambda's effects.

| Node | Contribution |
|---|---|
| `EX_PERFORM` | the effect, looked up by name |
| `EX_CALL` to an indexed function | its inferred row (after substitution, with other modules' `^private` effects filtered by `filter_cross_module_private`), plus `Unsafe` and every **capability** in its *declared* row |
| `EX_CALL` through a local closure, a `let` alias of a global fn, or a lifted `letrec` member | the lifted function's row, via `closure_fn_binding`, `widen_fn_alias`, `source_binding` |
| `EX_CALL` through an unindexed fn binding (a parameter) | its declared row, if concrete after substitution |
| `EX_CALL` with a fn-valued argument | ER2: a row-variable parameter is unified with the argument's row (TUR-E0254 on an occurs failure). An **un-annotated** or row-variable fn parameter is charged with the argument's row at the call, because the callee may call it |
| `(.run obj)` on a record field / dictionary dispatch | the field's or the class method's declared row |
| `EX_BUILTIN` | `spec->effect` (`IO` for `println`); Saffron's dynamic `println` via `builtin_effect_for_name` |
| `EX_DEREF`, `EX_BORROW_*` of a `ptr<void>` | `Unsafe` |
| `EX_HANDLE` | body row minus each case's effect (**deep** handlers only; `handle-shallow` keeps it), plus every case body's row |
| `EX_WITH_HANDLER` | body row minus the effects of a handler **literal** or composition; a handler held in a variable removes nothing |
| `EX_HANDLER_LIT`, `EX_COMPOSE_HANDLERS` | their case bodies (re-opened effects) |
| Saffron dynamic nodes, `any` widens, `EX_ASCRIBE` | their operands |
| everything else | the operands from `cps_visit_children` (`default` arm) |

Inline C is never inferred as `Unsafe`, and neither is an `extern-c` call.
`#fx{Unsafe}` on an inline-C function is justified by its annotation (W0031
is suppressed for it).

### Validation

| Code | Function | Fires when |
|---|---|---|
| TUR-E0026 | `resolve_declared_row` | an uppercase row name no `defeffect` declares; hints for `stdlib/effects.tur` names, moved attributes (`Construct`/`ByVal`/`NonExhaustive`), and a did-you-mean within edit distance 2 |
| TUR-E0009 | `effect_row_check_declared` | the inferred row is not a subset of a declared row; one error per offending effect |
| TUR-E0009 | `check_call_site_rows_in_expr` (ER4) | a fn argument's non-empty row is not a subset of an indexed callee's concrete or empty parameter row |
| TUR-E0254 | `collect_effects_in_expr` (ER2) | a row variable would be bound to a row containing itself. Printed once per call site, though every sweep re-walks the call (`e0254_first_report`), and it fails the pass |
| TUR-W0031 | `effect_row_check_over_annotated` | a declared, non-capability effect is never inferred (nor any descendant of it); skipped for instance methods |
| TUR-W0030 | `effect_row_check_unannotated` | `--strict-effects`: no declared row, non-empty inferred row; not for forwarding wrappers |
| TUR-W0032 | `effect_row_check_var_always_concrete` | `--strict-effects`: the declared row is a bare variable and the inferred row, minus capabilities, is concrete and non-empty |
| TUR-W0033 | `check_unreachable_handlers_in_expr` | a `handle` clause's effect is not in the body's row (capabilities stripped); the `(unsafe ...)` marker handle is exempt |

`-Werror=strict-effects` (which implies `--strict-effects`) makes W0030 and
W0032 errors through `strict_effects_level`. `--lint-effects` is a deprecated
alias (TUR-W0050). An un-annotated function is never checked against
anything; only W0030 looks at it.

Real output, from `build/tur check`:

```
e0009.tur:4:7: error [TUR-E0009]: function 'pure-print' performs effect 'IO' but its declared row {} does not include it
w0033.tur:7:30: warning [TUR-W0033]: handler clause for 'Log' is unreachable: the body does not perform 'Log'
e0026.tur:2:15: error [TUR-E0026]: unknown effect 'Write' in effect row: no defeffect declares it ('Write' is declared in stdlib/effects.tur, which is not autoloaded; add (load "stdlib/effects.tur"))
rowvar.tur:2:7: warning [TUR-W0032]: function 'sneaky' declares row variable '#fx{e}' but always performs concrete effects #fx{Ask}; consider replacing '#fx{e}' with '#fx{Ask}'
```

Note what the last one implies. `sneaky` is
`(defn sneaky [x : int] #fx{e} : int (+ x (perform (Ask))))`: it declares
only a row variable and performs `Ask`, and **without** `--strict-effects`
the program checks clean, because `effect_row_is_subset` treats a variable as
a superset of everything.

### `--dump-effects`

`effect_check_dump_effects` prints one line per flattened `EX_FN_DEF`:

```
defn <name> : #<row>
```

where `<row>` is the **declared** row when there is one, else the inferred
row, rendered by `effect_row_print`. It runs only when the pass returned 0, so
a program with a TUR-E0009 prints nothing. It lists the autoloaded prelude
(about 480 lines before your first function), instance methods
(`__inst_Eq_eq_qu_int`) and lifted lambdas (`__fn_N`). For this program:

```turmeric
(defeffect Ask [] : int)
(defeffect Log [msg : cstr] : nil)
(defn ask-plus [n : int] : int (+ n (perform (Ask))))
(defn logged [n : int] #fx{Ask Log} : int
  (do (perform (Log "hi")) (ask-plus n)))
(defn twice [f : (fn [int] #fx{e} int) x : int] #fx{e} : int (f (f x)))
(defn mixed [x : int] #fx{Ask e} : int (perform (Ask)))
(defn printer [x : int] : int (do (println x) x))
(defn handled [] : int (handle (ask-plus 1) (Ask [] k) (resume k 41)))
```

`tur --dump-effects check f.tur | tail -n 6` prints, in source order:

```
defn ask-plus : #{Ask}
defn logged : #{Ask, Log}
defn twice : #{e}
defn mixed : #({Ask} | {e})
defn printer : #{IO}
defn handled : #{}
```

The fixture `effect-dump` and `tests/run-flags.sh` pin the format.

---

## Capabilities and `Unsafe`

Both are ordinary `Effect`s with properties the pass special-cases.

**Registration.** `effect_env_register_builtin_unsafe` and
`effect_env_register_builtin_capabilities` run in `PASS_EFFECT_ENV`, in the
interpreter's session env, and in the elaborator. The capabilities come from
the `k_builtin_caps[]` table in `effect.c`: `IO`, then `FS`, `Net`, `Proc`,
`Rand` with `parent = IO`. A `defeffect` of one of those names is accepted
only if it matches the built-in exactly (`elab_defeffect`, via
`effect_builtin_capability`) -- that is how `stdlib/effects.tur` re-declares
them. Any other `defeffect ... ^capability` (`Bt` in the autoloaded
`stdlib/trail.tur`) sets `is_capability` through `EffectDef.is_capability`.

**How a capability differs from an algebraic effect:**

| | Algebraic effect | `^capability` |
|---|---|---|
| Enters a row from | `perform` in the body | the callee's **declared** row, or `BuiltinSpec.effect` |
| Can be handled | yes | it is never performed, so no clause ever runs |
| TUR-W0031 when declared and unused | yes | never |
| Counts for W0032 / W0033 | yes | stripped |
| Counts as runtime effect for codegen | yes | no (`effect_row_is_runtime_pure`) |

**`Unsafe`** has the same "propagates from the declared row" rule (the
`find_unsafe_effect` merge in the `EX_CALL` arm), plus an elaboration-time
gate and a discharge:

1. Calling an `#fx{Unsafe}` function requires `unsafe_depth > 0`
   (`elab_call_fn_inner`). A function that itself declares `#fx{Unsafe}` raises
   `unsafe_depth` for its own body (`fn_declared_unsafe` in `elab_defn`).
2. `(unsafe body...)` (`elab_unsafe`) raises `unsafe_depth` while elaborating
   the body and wraps it in an `EX_HANDLE` with one `Unsafe` case,
   `(resume k nil)`, and `is_unsafe_marker = true`. Because it is a deep
   handle, the inference walk removes `Unsafe` from the body's row: the block
   **discharges** the effect, so a safe wrapper over an unsafe primitive has an
   empty row. That is decision "Option A" in the
   [effect-row honesty plan](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/effect-row-honesty-plan.md#3-decision-option-a-for-fxunsafe).
3. W0033 skips the marker's clause. CPS coloring does **not** skip it:
   `cps_control_walk` seeds on every `EX_HANDLE`, so a function containing an
   `(unsafe ...)` block is colored, and so is every caller:

   ```
   $ TUR_CPS_CENSUS=1 tur emit-c unsafe.tur 2>&1 >/dev/null
   cps-census uncolored - - - unsafe.tur poke
   cps-census colored control - - unsafe.tur safe-poke
   cps-census colored callee - - unsafe.tur caller
   ```

   (`poke` is the `#fx{Unsafe}` inline-C primitive, `safe-poke` wraps it in
   `(unsafe ...)`, and both `safe-poke` and `caller` have the row `#{}`.) The
   cost is contained further down: the CT-IR translator whole-body-delegates a
   marker handle (`cps_ir.c:safe_to_delegate`), `cps_expr_installs_handle` and
   `cps_expr_contains_effect_op` ignore it, and the emitter runs its body in
   place (`emit_effects.c`).

---

## Where rows are consumed downstream

All of these read; none write. The details are in the
[CPS guide](cps-dk-internals-guide.md).

| Reader | Asks | Uses |
|---|---|---|
| `cps.c:cps_force_color_eff_fnval_args` | a `TY_FN` parameter or constructor field has a non-empty row | colors the lambda passed there, so the E2 registry has a `__cps` twin to thread |
| `cps.c:cps_fn_has_leftover_effect` | declared or inferred row non-empty | seeds a function that uses `with-handler` but leaves effects over |
| `cps_ir.c:fn_effect_may_escape`, `local_lambda_effectful` | not runtime-pure (a missing inferred row counts as effectful) | serial receivers; the local closure rewrite |
| `cps_ir.c:call_is_effectful_fnvalue` | a fn value's row is not `ERK_EMPTY` | eviction of a call it cannot thread |
| `emit_cps_ir.c:callee_effect_free`, `serial_closure_recv_escapes` | runtime-pure (falls back to the inferred row for a `#fx{e}` callee) | whether threading a fn value into a callee can matter |
| `emit_expr.c` (poly-wrap `fn_cps` twin), `emit_effects.c`, `emit_module.c` | runtime-pure | whether a fn value needs a CPS entry |

Coloring itself seeds on control operators, not rows; the two `cps.c` reads
above are the only places a row changes the colored set, and both test
`effect_row_is_empty`, not runtime purity.

### An empty row does not mean pure

The most important caveat for anyone optimizing with rows. A fn **type**
with an empty or missing row says nothing about the **value** in it:

- An un-annotated fn parameter accepts an effectful argument; the pass charges
  the argument's row to the *call site*, not to the parameter's type.
- A `let`-bound lambda's binding has a `TY_FN` type with no row; only the
  lifted `FnDef` carries `inferred_effect_row`.
- `inferred_effect_row` exists only on `FnDef`s the pass indexed (the
  flattened top-level items, which include lifted lambdas and the
  elaborator's partial-application thunks). A `NULL` row reads as "never
  inferred": `fn_effect_may_escape` and its siblings treat that as effectful,
  `callee_effect_free` as not effect-free.

The performance audit priced the obvious optimization -- keep a call through
a fn value with an empty row direct -- and found it unsound: seven fixtures
stop compiling ([R-1 census](https://github.com/turmeric-lang/turmeric/blob/main/docs/artifacts/perf-audit-2026-10/wp2-runtime-hot-paths.md#r-1-census----how-much-is-cps-colored-and-why)).
Rows that hold for function values (inference at lambdas, checking at HOF
parameters) are item 6 of its fix list. Until that lands, decide that
something cannot perform with `effect_row_is_runtime_pure` on a **`FnDef`'s**
rows, never with `effect_row_is_empty` on a type.

### The effect-row honesty plan

[effect-row-honesty-plan](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/effect-row-honesty-plan.md) is done and
archived. W0 (attributes out of `#fx{}`), W1 (TUR-E0026), W2 (compiler-known
capabilities), W3 (guide corrections) and W4 (`IO` on `println`, the
runtime-pure gates) are described above. W5 -- whether `--strict-effects`
should default on -- was answered no: the flag stays off by default.

---

## Flags and tools

| Flag | Effect |
|---|---|
| `--dump-effects` | the per-`defn` row listing above |
| `--strict-effects` | TUR-W0030 and TUR-W0032 |
| `-Werror=strict-effects` | those two as errors; implies `--strict-effects` |
| `--lint-effects` | deprecated alias of `--strict-effects` (TUR-W0050) |
| `--lint-inline-c-unsafe` | TUR-W0036 |
| `--timings` | the pass's time is charged to `PROF_EFFECT_ROW` |

Negative fixtures live under `tests/fixtures/errors/effect-*` (for example
`effect-row-mismatch`, `effect-row-occurs`, `effect-subtype-violation`,
`effect-handle-unreachable`, `effect-row-module-member`) plus
`unknown-effect-in-row`, `println-in-pure-fn` and the `*-capability-*`
fixtures. Their `expected.diag` lines are substrings, so they do not pin how
many times a diagnostic prints.

---

## Known gaps and open reports

- **A declared row variable admits anything.** `effect_row_is_subset` returns
  true for any `ERK_VAR`, so `#fx{e}` never produces TUR-E0009; only
  `--strict-effects` (W0032) notices a concrete body.
- **Effect hierarchy is one-directional.** A declared parent covers a
  performed child, but a `handle` clause for a parent removes nothing from the
  row (`effect_row_remove` matches names exactly).
- **`with-handler` over a handler variable** removes nothing from the row;
  the handled effect over-propagates (comment on `remove_handler_effects`).
- **Registered but never emitted:** TUR-W0034, TUR-E0250 and TUR-E0253 have
  `tur explain` text and no emit site. TUR-E0008 is the reverse: emitted, but
  not a registered code.
- **Fixed-size buffers.** `EffectRowSubst` silently drops bindings past 32,
  and `effect_row_collect_names` truncates at 32 names (a handler's handled
  set, compared and printed by name). `effect_row_merge` and
  `effect_row_remove` size their buffers from their inputs, and a row's count
  is 32-bit, since
  [effect-row-merge-overflows-past-32-effects](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/effect-row-merge-overflows-past-32-effects.md)
  (a 33rd effect wrote past a 32-entry stack array); the six row parsers no
  longer drop non-symbol items either
  ([effect-row-non-symbol-items-dropped](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/effect-row-non-symbol-items-dropped.md)).
- **`(unsafe ...)` colors.** The marker handle is a coloring seed, so a safe
  wrapper and all its callers are CPS-colored although nothing can be
  performed (see [Capabilities and `Unsafe`](#capabilities-and-unsafe)).
- [effect-tags-collide-past-128-effects](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/effect-tags-collide-past-128-effects.md)
  -- the emitter's effect tag table, downstream of rows but keyed on the same
  effects.
- [cps-effectful-callback-through-multi-arg-or-untyped-param](https://github.com/turmeric-lang/turmeric/blob/main/docs/reported/cps-effectful-callback-through-multi-arg-or-untyped-param.md)
  -- threading through un-annotated (empty-row) fn parameters.
- [serial-receiver-effect-under-if-closure-or-leaf](https://github.com/turmeric-lang/turmeric/blob/main/docs/reported/serial-receiver-effect-under-if-closure-or-leaf.md)
  -- a consumer of `fn_effect_may_escape`.

Closed reports that explain current code:
[capability-effect-tag-silently-resolves-to-empty-row](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/capability-effect-tag-silently-resolves-to-empty-row.md),
[module-members-skip-effect-row-checking](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/module-members-skip-effect-row-checking.md),
[strict-effects-and-lint-effects-are-indistinguishable](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/strict-effects-and-lint-effects-are-indistinguishable.md),
[strict-effects-w0030-names-synthesized-lambdas](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/strict-effects-w0030-names-synthesized-lambdas.md).

Related internals guides: [compiler internals](compiler-internals.md),
[elaborator internals](elaborator-internals-guide.md),
[diagnostics internals](diagnostics-internals-guide.md).

---

## Bugs found while writing this guide

Each was reproduced against the compiler this guide describes and filed in
[`docs/reported/`](https://github.com/turmeric-lang/turmeric/blob/main/docs/reported/README.md). When one is fixed and archived,
update the passage above that describes it and drop it from this list.

None open.

## Maintenance

Update this guide in the same PR when you:

- add an `ERK_*` kind, an `Effect` field, or a new place a row is parsed or
  stored;
- add a node arm to `collect_effects_in_expr` that changes what a body
  contributes, or a new step in `effect_check_pass`;
- add, retire or re-code an effect diagnostic;
- make a codegen gate read rows (say which predicate it uses);
- close an item in [Known gaps](#known-gaps-and-open-reports) or the honesty
  plan.

Regenerate the example output with `build/tur` rather than editing it by hand.
Cite functions, not line numbers.
