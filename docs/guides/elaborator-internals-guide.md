---
title: Elaborator Internals
category: Compiler Internals
description: How the elaborator turns reader Forms into the typed Expr IR -- the Elab state and scope model, the two-pass top-level driver and the synthesized main, call-site-local type inference, the Expr and Type data model, what the elaborator records for later passes, incremental sessions, macros, and the checklist for adding a form
tags:
  - elaborator
  - compiler-internals
  - type-inference
  - scopes
  - macros
---

# Elaborator Internals

This guide is for contributors working in `src/compiler/elab_*.c`: the stage
that takes the reader's `Form` trees and produces the typed `Expr` IR every
later pass reads. It describes the code as it stands. The pipeline map is
[compiler-internals.md](compiler-internals.md); this guide stops where the
`EX_PROGRAM` leaves `elaborate_program`.

Three neighbouring areas have their own guides and are only summarized here:

- typeclass declarations, instances and dispatch:
  [typeclass-internals-guide.md](typeclass-internals-guide.md);
- module loading, imports and separate compilation:
  [separate-compilation-internals-guide.md](separate-compilation-internals-guide.md)
  and [module-system-guide.md](module-system-guide.md);
- effect rows: [effect-rows-internals-guide.md](effect-rows-internals-guide.md).

> **Trust the code over the header comments.** The elaborator is about 79,000
> lines across 17 files, and many comments name the phase that introduced a
> field rather than what it does today. `types.h` still says `TY_STRUCT` is
> "see as.struct_ for StructDef"; no `Type` has carried `TY_STRUCT` since the
> structdef retirement, and the `struct_` union arm is gone. When a comment
> and this guide disagree, read the code. When the code disagrees with this
> guide, fix the guide.

---

## The short version

1. **One entry point.** `elaborate_program` (a wrapper over
   `elaborate_program_session`, `elab_toplevel.c`) takes the whole form array
   -- stdlib first, program after -- and returns one `EX_PROGRAM`, or `NULL`
   after any error.
2. **Rewrite, then two passes.** Loads are spliced in, `#lang r7rs` is
   lowered, type names are stubbed, and loose statements are folded into a
   synthesized `main`. Pass 1 forward-declares every `defn`; pass 2
   elaborates each form, deferring a few that need something defined later.
3. **Dispatch is by head symbol.** `elab_call` tries special forms, then
   macros, then bindings, then typeclass methods.
4. **Inference is call-site-local.** Each call binds the callee's type
   variables from its own arguments (`CallTypeBinding`, 16 slots), types its
   result from them, and records them in `call_.abi_bindings` for the
   emitter. Unannotated parameters default to the carrier type.
5. **Lambdas are lifted as they are elaborated** into file-scope `EX_FN_DEF`s
   (`__fn_N`), prepended to the program at the end.
6. **The IR carries decisions.** Call nodes and bindings are stamped with
   ownership masks, drop hints, dictionaries and specialization bindings that
   later passes read instead of recomputing.

---

## Source map

| File | Role |
|---|---|
| `elab.h` | Public API: `elaborate_program`, `ElabSession` and `elaborate_program_session`, the macro-time turi env accessors, the LS2 resolver and used-modules contexts |
| `elab_internal.h` | The `Elab` struct, `Scope`, `MacroDef`, the deferred-site tables, every cross-file prototype |
| `elab_toplevel.c` | `elaborate_program_session` (the driver), `elab_form` (dispatch on the `Form` tag), pass 1 (`elab_pre_declare_toplevel_defn`), the main fold, the pass-2 retry and deferral machinery, `any` casts |
| `elab_call.c` | `elab_call` / `elab_call_inner` (special-form, macro and call dispatch), `elab_call_fn_inner` (the ordinary call path, about 4,000 lines), type-binding collection and instantiation, partial application, the `any` and fat-closure bridges |
| `elab_fns.c` | `elab_defn`, `elab_fn` (lambda lifting and closure construction), `elab_def`, `elab_extern_c`, the inferred ownership masks, contracts and loop invariants |
| `elab_forms.c` | `let`, `let*`, `letrec`, `if`, `do`, `while`, `case`, `set!`, `defer`, `return`, `?`, generators |
| `elab_core.c` | `elab_init_state`, scopes, `binding_new`, move and linear state, `collect_free_vars`, `elab_register_file_def`, the return-position conflict predicates, `elab_lookup_macro` |
| `elab_types.c` | `type_expr_from_form` (annotation -> `Type`), `defkind`, `defalias`, `defrec`, `deftype`, `::`, `pack`/`open`, `default-of`, and the removed `type-app` (`elab_reject_type_app_form`, which every annotation resolver calls) |
| `elab_structs.c` | `defstruct` (lowered to a record `defdata`), `defopaque`, `defdata` and the deprecated `defgadt` (both `elab_adt_def`; `elab_defgadt` only adds TUR-D0005 and requires every constructor annotated), `match`, `make-struct`, `with`, `&`, `&mut` |
| `elab_typeclasses.c` | `defclass`, `definstance`, `elab_method_call`, return-directed dispatch. See [typeclass-internals-guide.md](typeclass-internals-guide.md) |
| `elab_effects.c` | `reset`/`shift`/`shift0`, cloneable and serial continuations, `call/cc`, `escape`, `defeffect`, `perform`, `handle`, handler values, `resume` |
| `elab_macros.c` | `defmacro`, `defmacro*`, the compile-time evaluator, quasiquote, `gensym` |
| `elab_module.c` | `load`, `import`, `defmodule`, `export`, and `elab_lookup_sym` (visibility and qualified names) |
| `elab_memory.c` | `ref`, `lref`, `@`, `drop!`, `rc`/`weak`, GC primitives |
| `elab_concurrent.c` | `thread-spawn`, `async`/`await`, `select`, panics, STM |
| `elab_unsafe.c` | `unsafe`, pointer operations, casts, raw memory, `c-call`, `call-ptr`, `dlopen` |
| `elab_dynvars.c`, `elab_sessions.c`, `elab_global.c`, `elab_reflect.c` | Dynamic vars, session types, multi-party protocols, the `^reflect` totality gate |
| `expr.h`, `expr.c` | `Binding`, `FnDef`, `Closure`, `ExprKind`, `struct Expr`, `expr_new` |
| `types.h`, `types.c` | `TypeKind`, `struct Type`, `type_eq`, `type_print`, the C-spelling table `TY_SIMPLE_REPR_ROWS` |
| `src/passes/cps.c` | `cps_visit_children`, the one shared enumeration of an expression's evaluated operands |

---

## Pipeline position

Elaboration is the first entry of `src/driver/pipeline.c:core_passes[]`:

```
PASS_ELABORATE -> PASS_KIND_CHECK -> PASS_EFFECT_ENV
  -> PASS_EFFECT_ROW_INFER -> PASS_CPS -> PASS_BORROW_CHECK
```

Kind checking, effect-row inference and borrow checking are separate passes
over the finished IR. The elaborator does its own move, linear and
uniqueness checks inline, though, because they depend on scope state that no
longer exists after it returns.

`elaborate_program` is called from the driver's `run_core_pass` (every compiling command,
plus `tur check`, `tur lsp` and `tur mcp`) and from the playground
(`src/web/wasm_glue.c`, `wasm_lsp.c`). The interpreter (`src/turi/eval.c`,
`repl.c`) calls `elaborate_program_session`. It evaluates the same
`EX_PROGRAM` the emitter compiles, so a change to the IR shape is a change to
both back ends; see
[turi-interpreter-internals-guide.md](turi-interpreter-internals-guide.md).

---

## The `Elab` struct

`Elab` (`elab_internal.h`) is one large struct passed to every elaborator
function. Most of its roughly 600 lines are cached interned symbols. The rest
falls into these groups:

| Group | Fields | Notes |
|---|---|---|
| Memory and names | `arena`, `st`, `next_id`, `next_stdlib_id`, `ids_stdlib`, `next_implib_id`, `ids_implib`, `next_gensym_id` | Draw ids through `elab_fresh_id`, never `next_id++`. Stdlib and imported-library forms take ids from their own counters (bases `ELAB_STDLIB_ID_BASE`, `ELAB_IMPLIB_ID_BASE`) so their mangled names do not move with the size of the program, which the split-build C cache depends on ([r7rs-programs-compile-slowly](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/r7rs-programs-compile-slowly.md)) |
| Scope | `scope` (current), `global`, `fn_entry_outer_scope`, `letrec_self_group` | See the next section |
| Cached symbols | `sym_let`, `sym_defn`, `sym_caret_linear`, `kw_heap`, ... (several hundred) | Interned once in `elab_init_state`; special-form dispatch compares pointers |
| Registries | `adt_defs`, `macros`, `typeclass_env`, `effect_env`, `type_alias_*`, `dynvar_entries`, `global_protocols`, `loaded_modules` | Grow for the whole unit; freed at return unless a session keeps them |
| Lifted output | `file_scope_defs`, `n_file_scope_defs` | Lambdas, instance methods and other definitions minted mid-expression. Prepended to the program at the end |
| Current-definition context | `fn_body_depth`, `current_fn_name`, `cur_fn_constraints`, `cur_fn_constraint_param_mask`, `definstance_depth`, `cur_hkt_constraint_*`, `cur_lifetime_ctx`, `in_match_arm`, `g2_skolem_env` | Saved and restored around each body |
| Signature type variables | `sig_tyvars[32]`, `sig_tyvar_kinds[32]`, `n_sig_tyvars` | Every named type variable of the enclosing signatures, outermost first. Treated as rigid by call inference (see [Rigid variables](#rigid-variables-sig_tyvars)) |
| Expected-type channel | `expected_type`, `assoc_hint_*` | A one-slot downward hint (see [Inference](#type-inference-is-call-site-local)) |
| Dialect flags | `toplevel_dynamic`, `toplevel_scheme`, `toplevel_stmt`, `in_callcc_receiver` | Set per top-level form; macro-expanded forms carry other files' spans, so span checks alone are not enough |
| Module state | `current_module_name`, `current_module`, `has_defmodule`, `module_*_dir(s)`, `in_imported_module`, `separate_compilation`, `macro_expansion_stack` | See the module guides |
| Macro state | `macro_expand_depth`, `macro_call_site_span`, `macro_site_top`, `macro_env`, `user_macros` | `macro_site_top` lets a diagnostic or panic name the macro use rather than the template |
| Deferred whole-unit work | `noinst_pending`, `bare_fat_*`, `pending_reset_nodes`, `refine_obs`, `refine_call_sites`, `wf_frame_sites`, `rf_reads_sites`, `reflect_sites`, `loop_inv_sites` | Sites recorded during pass 2 and resolved after it |
| Session watermarks | `turn_continues_session`, `turn_start_n_*` | Which definitions came from an earlier REPL turn and may be replaced |
| Mode flags | `sandboxed`, `in_stdlib_load`, `strict_unknown_types`, `make_struct_*`, `unsafe_*` | |

`elab_init_state` memsets the whole struct before filling it, so a new field
defaults to zero. Every malloc'd registry has to be freed in three places:
both teardown branches of `elaborate_program_session` and
`elab_session_free`.

---

## Scopes and bindings

### The scope chain

A `Scope` is a growable array of `Binding *` plus a parent pointer.
`scope_add` appends; `scope_lookup` walks the chain innermost first and
returns the **newest** binding of a name in the first scope that has one. So
shadowing is just "a later `scope_add` of the same name wins".

Once a scope reaches `SCOPE_INDEX_MIN` (32) bindings, `scope_add` maintains a
hash from name to its newest binding, with `prev[]` links to older bindings
of the same name. In practice only the global scope gets one; it holds every
stdlib and program definition.

Body forms push a stack-allocated scope and pop it on every exit path:

```c
Scope inner;
scope_init(&inner, e->scope);
e->scope = &inner;
/* ... scope_add(&inner, b) for each binder, elaborate the body ... */
e->scope = inner.parent;
scope_free(&inner);
```

A missed pop on an early `return NULL` leaves `e->scope` pointing at a dead
stack frame. Grep for the pattern when you add an exit path.

### Bindings

`binding_new` (`elab_core.c`) arena-allocates a zeroed `Binding` and stamps
its `id`, its `scope_depth` (the length of the live scope chain, which the
borrow-escape check compares), `defining_module_name`, and `is_from_stdlib` /
`is_from_implib` for a global made during a stdlib or imported-library
window.

`struct Binding` (`expr.h`) has well over 100 fields. Most are notes that a
later decision reads. The groups:

| Group | Examples |
|---|---|
| Identity | `name`, `type`, `id`, `span`, `is_global`, `is_param`, `is_mut`, `is_exported`, `defining_module_name` |
| Provenance | `is_synthesized`, `synth_kind` (`SYNTH_LAMBDA`, `SYNTH_INSTANCE_METHOD`, ...), `diag_label`, `is_lifted_lambda`, `source_fn_def`, `defn_form` |
| Substructural state | `is_moved`, `moved_at`, `moved_owning`, `lent_to_nonretaining`, `is_linear`/`is_linear_consumed`, `is_unique`/`is_unique_consumed`, `is_affine`, `is_relevant`, `is_borrow`, `usage_state`, `alias_state` |
| Closure plumbing | `closure_fn_binding` (a closure value's lifted thunk), `returns_closure_fn_binding`, `returns_boxed_closure`, `source_binding` (an immutable alias of a global fn) |
| Inferred callee facts | `nonretain_param_mask`, `nonretain_ptr_param_mask`, `nonretain_sum_param_mask`, `resalias_param_mask`, `returns_fresh_closure`, `returns_fresh_sum_box`, `fresh_sum_via_param_mask` |
| Refinement and frames | `refine_param_preds`, `refine_return_pred`, `reads_*`, `writes_*`, `reflect_*` |
| Emitter notes | `emit_byvalue_carrier_abi`, `emit_carrier_holds_byval`, `emit_carrier_holds_ptr`, guarded by `emit_note_epoch` |

The emitter notes are written during emission, not elaboration. Read them
through `emit_note(b)`, which clears notes left by an earlier emission in the
same process (`tur lsp` emits many programs).

### How a name resolves

A bare symbol in expression position goes through `elab_form`'s `F_SYM` arm
to `elab_lookup_sym` (`elab_module.c`):

1. `scope_lookup` in the current chain. A hit is checked for visibility: a
   binding owned by another module must be exported, unless the module is
   under `tur/`, whose names are implicitly visible everywhere. A deprecated
   binding warns, or errors under `--Werror=deprecated`.
2. Otherwise, only for a name containing `/`: self-qualified
   (`mymod/name`), then an import alias (`alias/name`), then a full module
   name. These produce their own diagnostics.
3. Otherwise `NULL`, and the caller reports TUR-E0003 with a Levenshtein
   suggestion drawn from every scope in the chain.

A list's head symbol goes through `elab_call_inner` instead, in this order:

1. special forms, compared by symbol pointer (`def`, `let`, `if`, ...
   `defn`, `fn`, `extern-c`);
2. macros (`elab_lookup_macro`);
3. `elab_lookup_sym`, as above;
4. typeclass methods: if no binding was found, return-directed dispatch
   (`elab_try_return_dispatch`); then bare-name dispatch, which rewrites
   `(name x ...)` to `(.name x ...)` for `elab_method_call`. Bare-name
   dispatch also wins over a free `defn` of the same name when the method's
   class is user-defined and the receiver's type has an instance (TUR-W0039
   flags that clash);
5. otherwise the ordinary call path, `elab_call_fn` ->
   `elab_call_fn_inner`.

So a special form beats a macro, and a macro beats a local binding of the
same name: `(let [when f] (when true 2))` still expands the `when` macro (and
then fails, because the expansion is `nil`-typed).
`tur_warn_if_shadows_special_form` (TUR-W0042) warns when a `defn` or
`defmacro` takes a special form's name.

### Move and linear state

The elaborator checks moves as it goes, on the `Binding`:

- `binding_mark_moved` sets `is_moved` and the sticky `moved_owning`;
- `binding_mark_lent` sets `is_moved` and the sticky `lent_to_nonretaining`
  (the callee's mask says it keeps nothing, so ownership stays here);
- `binding_check_not_moved` reports TUR-E0005 with a "moved here" note.

Both marks poison later uses identically. Only the scope-exit drop reads the
two sticky flags to decide whether to free.

Branches are path-sensitive. `elab_if` snapshots every visible binding's
move state (`move_state_snapshot_bindings`) and linear state, elaborates the
`then` arm, restores, elaborates the `else` arm, then merges:

```c
is_moved = before || (then && else);
```

So a value moved in only one arm is **not** moved after the `if`. Linear
values are stricter: consumed in one arm and not the other is an error.
`match` does the same per arm (`elab_structs.c`).

---

## The top-level driver

`elaborate_program_session` runs these steps in order. Each is gated so a
program that does not need it takes no extra work.

| Step | What happens | Where |
|---|---|---|
| 1. State | Restore a live session (copy the struct, re-anchor `scope` to `&global`, reset per-file fields) or `elab_init_state` | `elaborate_program_session` |
| 2. Load dedup | Register the auto-loaded stdlib files' paths, so a later explicit `(load "stdlib/x.tur")` is a no-op | `load_dedup_register` |
| 3. Load expansion | Splice every top-level `(load "path")` depth-first, in source order. The stdlib boundary index is re-anchored to the expanded stream | `load_expand_forms` |
| 4. Scheme lowering | Lower `#lang r7rs` core forms onto Turmeric forms. See [r7rs-implementation-guide.md](r7rs-implementation-guide.md) | `scheme_lower_program` |
| 5. Type stubs | Pre-register every top-level `defstruct`, `defdata` (or `defgadt`) and `defopaque` name as an empty `AdtDef` with a global binding, so types can refer to each other in any order | the "Phase RF0" loop |
| 6. Main fold | Fold loose statements into a synthesized `main` (below) | inline |
| 7. Pass 1 | Forward-declare every top-level `defn` | `elab_pre_declare_toplevel_defn` |
| 8. `defmodule` position | A `defmodule` must be the first form of its source file | inline |
| 9. Pass 2 | Elaborate each form; defer and retry some (below) | `tl_elab_slot`, `tl_flush` |
| 10. Drain | Retry the deferred forms | `fwd_gen_order_drain` |
| 11. Whole-unit checks | Instance-less retries, superclass resolution, cross-function `reset` wrapping, `#[used]` module force-loads, the lazy `^fat` sweep | `elab_noinst_retry`, `elab_typeclass_superclasses_finish`, `elab_wrap_resets_for_crossfn_resume`, `elab_force_load_module`, `elab_sweep_bare_fat_lazy` |
| 12. Prepend | `file_scope_defs` go in front of the program items | inline |
| 13. Unit diagnostics | Exported C-name collisions, TUR-W0039 (free `defn` shares a class method's name) | inline |
| 14. Refinement | Write and read frames, `^reflect` sites, call-site crossings, discharge | `wf_resolve_write_frames`, `refine_resolve_call_sites`, `refine_discharge_all` |
| 15. Teardown | Snapshot the constructor census, then free the registries or hand them back to the session | `ctor_census_snapshot` |

If `rc != 0` at the end, the result is `NULL` even when the error came from a
retried form.

### The main fold

A program with no `main` and loose top-level statements gets one:

```
(defn sq [x : int] : int (* x x))      (defn sq [x : int] : int (* x x))
(println (sq 7))                  =>   (def k 3)
(def k 3)                              (defn main [] : int
(println (+ k 1))                        (do (println (sq 7))
                                             (println (+ k 1))
                                             0))
```

The fold exists so a top-level `handle` reaches the CPS/DK backend like one
in a user `main` ([cps-toplevel-synthesized-main-bypasses-dk](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/cps-toplevel-synthesized-main-bypasses-dk.md)).
It is conservative. It aborts, leaving the forms as they were, when any
user-region form is:

- not a list, or headed by `do`, `?` or `return`;
- a statement containing a nested `def`;
- a macro call whose template cannot be shown to expand to a statement
  (`macro_form_stmt_safe`);
- a risky handle shape (`fold_stmt_is_risky`) or an unhandled `perform`;
- a `def` with a non-trivial initializer that follows a statement (its
  initializer would otherwise run before the statement;
  [toplevel-def-initializers-run-before-toplevel-expressions](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/toplevel-def-initializers-run-before-toplevel-expressions.md)).

It never runs under the interpreter (`g_interpret_mode`), where it would
make `tur eval` return the `main` closure instead of the expression's value,
nor under separate compilation. When it aborts, the emitter makes the
top-level statements statements of one synthesized `int main()`, at their
source positions (`emit_module.c`). In `tur emit-c` output for the program
above, `main` holds `int64_t __ps_202 = (sq(INT64_C(7)));` and the two
`printf` calls, then returns 0.

### Pass 1: forward declarations

`elab_pre_declare_toplevel_defn` hand-parses each `(defn ...)` form, without
elaborating it, and adds a global binding whose type is a `TY_FN` built
from:

- the arity and the scalar parameter kinds (`fwd_decl_scan_params`), skipping
  `^` markers and the type-parameter and constraint vectors;
- the return kind, if the annotation is a simple name;
- a full parametric result type where it can be built shallowly
  (`fwd_shallow_result_app`);
- full parameter types in a dynamic (Saffron or Scheme) file
  (`elab_fwd_param_full_types`).

It also pre-declares one `def` shape, `(def ^mut x : any ...)`
(`elab_pre_declare_any_mut_def`). Nothing else is forward-declared.

The forward declaration is deliberately incomplete. A compound parameter
type is the `TY_INT` placeholder, and **an unannotated return is `TY_INT`**
in a typed file (`TY_ANY` in a dynamic one). Pass 2's `elab_defn` replaces
the binding's type with the real one, but a caller elaborated earlier would
already have been typed against the placeholder, so such a callee counts as
lossy for the ordering below and its callers wait for its body
([forward-call-to-unannotated-return-typed-int](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/forward-call-to-unannotated-return-typed-int.md)).

### Pass 2: order, deferral and retry

Pass 2 visits the forms in order through `tl_elab_slot`, which sets
`toplevel_stmt`, `toplevel_dynamic`, `toplevel_scheme` and `in_stdlib_load`
around `elab_form`. Two mechanisms change the order:

- **Instance ordering (symptom A).** A `defn` whose body calls a class method
  whose only instance is declared later cannot resolve. When a `definstance`
  appears at or after the current position (`inst_from[]`), the `defn` is
  elaborated under a diagnostic capture frame (`diag_push_capture`). If it
  fails, `file_scope_defs` is rolled back, the slot is marked deferred, and it
  is retried after every other form, this time without a capture frame, so a
  real error reports for real
  ([typeclass-method-resolution-ignores-the-class](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/typeclass-method-resolution-ignores-the-class.md)).
- **Lossy callees (`FwdGenOrder`).** A forward declaration loses a generic
  callee's type parameters, a function-typed parameter's carrier flag, and an
  inferred return type (`fgo_return_unannotated`). So a `defn` that calls such
  a callee before its definition waits for it. A cycle of them (an
  unannotated mutually recursive pair) is broken at its first member.
  `fwd_gen_order_should_defer` parks it, and `tl_flush` elaborates the waiting
  `defn`s a later form names before that form. The scan is over unexpanded
  forms, so a call introduced by a macro is not seen
  ([forward-call-to-generic-callee-typed-as-placeholder](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/forward-call-to-generic-callee-typed-as-placeholder.md)).

Between forms, the loop resets `has_defmodule` at every source-file boundary
and, after the last stdlib form, promotes `tur/` module bindings and macros to
"stdlib pre-module" status (`defining_module_name = NULL`). The interpreter's
incremental path can move that boundary, which is why `elab_lookup_sym` and
`elab_lookup_macro` also honour `tur/` directly.

### What `EX_PROGRAM` looks like

`prog->as.program.items` holds, in order:

1. every `file_scope_defs` entry: lifted lambdas (`__fn_N`), instance methods
   (`__inst_<Class>_<method>_<type>`), class defaults, poly wrappers
   (`__poly_N`), partial-application thunks, dictionary clones, and the
   definitions of loaded modules;
2. one item per top-level form, in form order. A `defn` is `EX_FN_DEF`, a
   `def` is `EX_DEF`, a type definition is `EX_DEFDATA` (`defgadt` too),
   `defclass` is `EX_TYPECLASS_DEF`, `definstance` is `EX_INSTANCE_DEF`, a
   `defmodule` is `EX_DEFMODULE`, and a `defmacro` or `import` is an
   `EX_NIL_LIT` placeholder.

`out_n_file_scope_defs` reports how many items came from step 1, which the
interpreter uses to tell them apart from load-expanded forms. The program's
own `type` is `TYPE_NIL`.

---

## From a form to an `Expr`

`elab_form` switches on the `Form` tag:

| Form | Becomes |
|---|---|
| `F_NIL`, `F_BOOL` | `EX_NIL_LIT`, `EX_BOOL_LIT` |
| `F_INT` | `EX_INT_LIT` typed by its suffix (`7i32` is `TY_INT32`; `#\A` is the `Char` opaque when it is in scope). An `f32`/`f64` suffix makes `EX_FLOAT_LIT` |
| `F_FLOAT`, `F_STR` | `EX_FLOAT_LIT`, `EX_CSTR_LIT` |
| `F_KEYWORD` | `EX_SYM_LIT` (a runtime `:Sym`) |
| `F_SYM` | `EX_VAR`, through `elab_lookup_sym`. A `^mut` that was moved into a shared cell is rewritten to a cell read first |
| `F_VEC`, `F_MAP_LITERAL`, `F_SET_LITERAL` | Rewritten to a builder call (`vec-of`, `hamt-of`, ...) and re-elaborated |
| `F_QUOTE`, `F_QUASIQUOTE` | Expanded to forms and re-elaborated |
| `F_CBLOCK` | Inline C (`EX_INLINE_C`) |
| `F_LIST` | `elab_call` |
| `F_TYPE_ANN` outside a binder | An error |

Many forms are elaborated by **rewriting to another form and recursing**:
dynamic-file argument widening wraps arguments in `(:: x any)`, `when` is a
macro, `(Circle :radius 2.0)` reorders to a positional call, and a contract
check is rewritten to `tur-contract-check-at`. When you trace an elaboration,
expect the `Form` you are looking at to be a rewrite of the one in the file.
`TUR_DUMP_MATCH_LOWER=1` prints the rewrite `match` produces.

### Lambda lifting

`elab_fn` (`elab_fns.c`) never produces `EX_FN`. A lambda becomes:

- **without captures:** a global `Binding` `__fn_N` (`is_lifted_lambda`), an
  `EX_FN_DEF` registered with `elab_register_file_def`, and an `EX_VAR` naming
  it at the use site;
- **with captures:** the same `EX_FN_DEF` with an extra leading `void *` env
  parameter, plus an `EX_CLOSURE` at the use site whose `Closure` lists the
  captured bindings (found by `collect_free_vars`) and names the env struct
  `__env_N`.

Because lifting happens during elaboration, every later pass sees lambdas as
ordinary top-level functions. That is why CPS coloring can treat a lifted
lambda as a call-graph node. `elab_register_file_def` does nothing for a
definition elaborated inside an imported module under separate compilation;
that module's own translation unit emits it.

---

## The data model

### `Expr`

`struct Expr` (`expr.h`) is `kind`, a **by-value** `type`, a `span`, three
drop-hint bools outside the union (`any_drop_after`, `sum_box_drop_after`,
`sum_box_drop_after_dyn`), and a union of per-kind payloads. Allocate nodes
with `expr_new(arena, kind, type, span)`, which zeroes the payload.

`ExprKind` has 120 enumerators. By family:

| Family | Kinds |
|---|---|
| Literals | `EX_NIL_LIT`, `EX_BOOL_LIT`, `EX_INT_LIT`, `EX_FLOAT_LIT`, `EX_CSTR_LIT`, `EX_SYM_LIT`, `EX_SET_LIT`, `EX_DEFAULT_OF` |
| Variables and binding | `EX_VAR`, `EX_LET`, `EX_LETREC`, `EX_DEF`, `EX_SET` |
| Control | `EX_IF`, `EX_DO`, `EX_WHILE`, `EX_RETURN`, `EX_DEFER`, `EX_MATCH` |
| Functions and calls | `EX_FN_DEF`, `EX_CLOSURE`, `EX_CALL`, `EX_BUILTIN`, `EX_EXTERN_C`, `EX_INLINE_C`, `EX_CONS_LIST` (a `& rest` argument list) |
| Function-value bridges | `EX_POLY_WRAP`, `EX_FN_TO_FAT`, `EX_POLY_TO_FAT`. See [value-representations-guide.md](value-representations-guide.md) |
| Type operations | `EX_ASCRIBE`, `EX_CAST`, `EX_REINTERPRET`, `EX_UNION_INJECT`, `EX_EXISTS_PACK`, `EX_EXISTS_OPEN`, `EX_EXISTS_DISPATCH` |
| Dynamic (`any`) | `EX_ANY_TYPE_OF`, `EX_ANY_CAST`, `EX_ANY_IS`, `EX_DYN_OP`, `EX_DYN_CALL`, `EX_DYN_FIELD`, `EX_DYN_METHOD` |
| Data definitions and access | `EX_DEFDATA` (`defdata` and `defgadt`; there is no `EX_DEFGADT`), `EX_MAKE_STRUCT`, `EX_GET_FIELD`, `EX_SET_FIELD` |
| Typeclasses | `EX_TYPECLASS_DEF`, `EX_INSTANCE_DEF`, `EX_DICT` |
| Ownership | `EX_REF`, `EX_DEREF`, `EX_SET_DEREF`, `EX_RC_*` (7 kinds), `EX_REF_FROM_RC`, `EX_WEAK`, `EX_WEAK_UPGRADE`, `EX_WEAK_PRED`, `EX_REF_PRED`, `EX_BORROW_IMMUT`, `EX_BORROW_MUT` |
| Panics | `EX_PANIC`, `EX_PANIC_WITH`, `EX_CATCH_UNWIND`, `EX_CATCH_PANIC_OF`, `EX_PANIC_PAYLOAD_*` (5 kinds) |
| Delimited control | `EX_RESET`, `EX_SHIFT`, `EX_SHIFT0`, `EX_CALLCC`, `EX_CLONEABLE_RESET`, `EX_CLONEABLE_SHIFT`, `EX_SERIAL_RESET`, `EX_SERIAL_SHIFT` |
| Effects | `EX_DEFECT`, `EX_PERFORM`, `EX_HANDLE`, `EX_HANDLER_LIT`, `EX_WITH_HANDLER`, `EX_COMPOSE_HANDLERS`, `EX_RESUME`, `EX_DISCONTINUE`, `EX_CONT_PRED` |
| Concurrency | `EX_ASYNC`, `EX_AWAIT`, `EX_SELECT`, `EX_STM`, `EX_ATOMICALLY`, `EX_RETRY`, `EX_CHECK`, `EX_OR_ELSE`, `EX_TVAR_*` (6 kinds) |
| Generators, dynamic vars | `EX_GEN`, `EX_YIELD`, `EX_GEN_NEXT`, `EX_GEN_DONE`, `EX_GEN_UNWRAP`; `EX_DEFDYNAMIC`, `EX_DYNVAR_READ`, `EX_DYNVAR_BINDING`, `EX_DYNVAR_SET` |
| Structure | `EX_PROGRAM`, `EX_DEFMODULE` |

Two kinds are never built by the elaborator. `EX_FN` is only copied by
`cps.c` if one is already present. `EX_CPS_CONT_APP` is
built by nothing; `emit_value` reports an internal error if it sees one, and
it is the one kind the interpreter carves out. Both still have arms in most
switches. `EX_DEFECT` means `defeffect`, not "defect".

### `Type`

`Type` (`types.h`) is passed and stored **by value**; its variable-length
parts live behind pointers. Every `Type` has a `kind`, `copy_kind` and
`substruct` (the ownership and substructural discipline), up to four
`lifetimes`, `hkt_kind` (a `uint16_t` `Kind`: 0 is `*`, N an N-ary arrow,
`KIND_TYPEROW` and `KIND_ROW` sentinels), `c_num_spelling`, and a union `as`
with one arm per structured kind.

`TypeKind` has 61 enumerators. By family:

| Family | Kinds | Payload arm |
|---|---|---|
| Unknown, unit, bottom | `TY_UNKNOWN` (not yet resolved), `TY_NIL`, `TY_NEVER` | -- |
| Scalars | `TY_BOOL`, `TY_INT`, `TY_FLOAT`, `TY_CSTR`, `TY_SYM`, `TY_INT8`..`TY_INT64`, `TY_UINT8`..`TY_UINT64`, `TY_FLOAT32`, `TY_FLOAT64` | -- |
| Raw pointers | `TY_PTR_VOID`. A typed `ptr<T>` is also `TY_PTR_VOID`, with `as.ptr.inner` set | `ptr` |
| Functions | `TY_FN` | `fn` |
| Owning and borrowed | `TY_REF`, `TY_LREF`, `TY_RC`, `TY_WEAK`, `TY_REF_IMMUT` (`&T`), `TY_REF_MUT` (`&mut T`) | `ref`, `rc`, `ref_borrow` |
| Nominal data | `TY_ADT` (every `defdata`, `defstruct`, `defgadt` and `defopaque`; `AdtDef.has_result_types` / `is_refined` replaced `is_gadt`), `TY_STRUCT` (retired; see below) | `adt_` |
| Type-level structure | `TY_APP`, `TY_TYVAR`, `TY_FORALL`, `TY_EXISTS`, `TY_REC`, `TY_UNION`, `TY_INTERSECTION`, `TY_TYPEROW`, `TY_CONTRACT` | `app`, `tyvar_`, `forall_`, `rec`, `union_`, `intersection_`, `typerow_`, `contract_` |
| Dynamic | `TY_ANY` (`tur_tagged_t` at run time) | -- |
| Control | `TY_CONT`, `TY_CLONEABLE_CONT`, `TY_HANDLER`, `TY_EXCEPTION`, `TY_GENERATOR` | `cont`, `handler_`, `exn`, `generator_` |
| Typeclasses | `TY_TYPECLASS`, `TY_TYPECLASS_INST` | `typeclass`, `typeclass_inst` |
| Sessions | `TY_SESSION`, `TY_SEND`, `TY_RECV`, `TY_CLOSE`, `TY_CHOOSE`, `TY_BRANCH`, `TY_SESSION_REC`, `TY_TIMEOUT`, `TY_SESSION_PAIR`, `TY_SESSION_RECV_PAIR`, `TY_SESSION_OFFER`, `TY_GLOBAL`, `TY_ROLE` | `session_`, `global_`, `role_` |
| Compile-time only | `TY_DYNVAR`, `TY_SYNTAX` | `dynvar_` |

Things that are not what they look like:

- **`TY_STRUCT` is retired as a `Type` kind.** `defstruct` lowers to a
  single-variant record ADT (`AdtDef.from_struct_lowering`).
  `type_has_concrete_codegen_layout` and `type_c_name` still have rows for it,
  and the runtime `any` box reports such a value as a struct
  (`any_box_tag_for_type`).
- **Aliases are not uniform.** `typekind_from_symbol` maps the names `int64`
  and `float64` to `TY_INT` and `TY_FLOAT`, but `i64` and `f64` (and the
  literal suffixes) to the distinct kinds `TY_INT64` and `TY_FLOAT64`. Both
  spell `int64_t`/`double` in C, but `type_eq` tells them apart, so a plain
  `3` is rejected for an `i64` parameter. The `defn` and `fn` return ladders
  spell `int64` / `float64` out as `int` / `float` aliases; until 2026-10-17
  they rejected them as return types
  ([int64-float64-return-annotation-rejected](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/int64-float64-return-annotation-rejected.md)).
- **`TY_TYVAR` is every named type variable**, not only GADT ones as its
  comment says. `tyvar_.name` is interned; `open_slot` marks a variable that
  nothing fixed (a constructor's free slot, or a callee's renamed-apart result
  variable); `assoc_of`/`assoc_arg` mark an unreduced associated-type
  projection.
- **`TY_APP` is curried.** `(Result int cstr)` is
  `app(app(Result, int), cstr)`. `app.hole_pos_p1` (hole index plus one, zero
  meaning none) encodes a hole-headed partial application such as
  `(Result _ cstr)`. Use `type_app_has_hole` and `type_app_fill_hole`, never
  the field directly.
- **`TY_FN` keeps two views of each parameter.** `arg_kinds[i]` is a
  `uint8_t` kind for fast checks. `arg_full_types[i]` is the full type, or
  `NULL` for a monomorphic scalar. The result likewise has `result_kind` and
  `result_full_type`. Per-parameter flags (`FA_LINEAR`, `FA_BORROW`, `FA_FAT`,
  `FA_POLY_FN`, ...) are packed into `arg_flags[i]`; read and write them only
  through `FN_ARG_FLAG`/`FN_ARG_SET`. The arrays come from a process-lifetime
  arena (`tur_fn_args_alloc`) and are shared between copies, so allocate
  fresh arrays before changing one, as `call_instantiate_type` does. Other
  fields: `effect_row` (`NULL` means empty), `boxed` (a fat closure value),
  `cfnptr` (a bare C function pointer), `is_variadic`/`rest_*`,
  `result_borrow_arg`, `param_type_forms`, `result_type_form`.

Construct types with the helpers (`type_from_kind`, `type_fn`, `type_adt`,
`type_app`, `type_tyvar_named`, `type_union_build`). The fixed C spelling of
each simple kind is the `TY_SIMPLE_REPR_ROWS` table in `types.c`. For
representations and bridges see
[value-representations-guide.md](value-representations-guide.md) and
[type-erasure-guide.md](type-erasure-guide.md).

---

## Type inference is call-site-local

There is no global Hindley-Milner pass and no unification variable that
outlives an expression. Types flow **bottom-up**: each subexpression is
elaborated first and its `Expr.type` is settled when it returns (two
exceptions are under
[per-call bindings](#3-per-call-type-variable-bindings)). Three mechanisms
supply what bottom-up typing cannot (defaults, the expected-type channel,
per-call bindings), and two checks keep generic bodies honest (rigid
variables, return-position conflicts).

### 1. Defaults for what was not written

- An unannotated parameter gets the carrier default,
  `saffron_default_param_kind`: `TY_INT` in a typed file, `TY_ANY` in a
  dynamic one. `Binding.type_is_carrier_default` records that the type is
  "one untyped word", not a claim that the value is an integer
  ([perform-does-not-typecheck-its-arguments](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/perform-does-not-typecheck-its-arguments.md)).
- An unannotated `defn` return is inferred from the body in pass 2
  ([inferred-return-defaults-inconsistently](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/inferred-return-defaults-inconsistently.md)),
  but the pass-1 forward declaration says `TY_INT` (see [Pass 1](#pass-1-forward-declarations)).
- **Implicit type variables (GS4).** An unknown name in a parameter or return
  annotation becomes a named `TY_TYVAR`. The KB-026 pass in `elab_defn` then
  keeps it only if it is declared in a type-parameter vector, is a kind
  variable or ADT parameter, or occurs in at least two type positions. A lone
  parameter occurrence is demoted to a `CK_MOVE` type variable. A lone return
  occurrence is "unsupported return type keyword". Setting
  `e->strict_unknown_types` turns the fallback into an error; see
  [unknown-type-annotation-becomes-implicit-tyvar](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/unknown-type-annotation-becomes-implicit-tyvar.md).

So a higher-order parameter must be annotated. This fails, because `f`
defaults to `int`:

```
$ tur run twice.tur          ; (defn twice [f x] (f (f x)))
twice.tur:1:19: error: 'f' is not a function or continuation
```

### 2. The expected-type channel

`Elab.expected_type` is a one-slot, opt-in downward hint. It is pushed by:

- `(:: e T)`;
- a typed `let` binder and a typed `def`;
- a `defn` or `fn` body, from the declared return type;
- the other arm of an `if` (`sibling_ty`);
- a `match` result;
- a lambda argument whose parameter has a concrete function type.

`elab_call_fn_inner` saves and clears it before elaborating arguments
(`saved_expected_return`), so it applies only to the outermost call.

The only bidirectional case is a lambda argument. Against an expected
`(fn [Box] int)`, `elab_fn` types an unannotated parameter from the expected
parameter type, but only when that type is an ADT or a type application. A
primitive expected type is deliberately left alone, so `(fn [s] ...)` passed
where `(fn [cstr] cstr)` is expected still has `s : int` (a carrier
default).

### 3. Per-call type-variable bindings

A call to a generic function does the following in `elab_call_fn_inner`:

1. Allocate `CallTypeBinding type_bindings[16]`. `CallTypeBinding` is a local
   alias of `AbiTypeBinding` (`expr.h`): `{ const char *name; Type type; }`.
2. Elaborate each argument. When the parameter's declared full type mentions
   a type variable, call
   `call_collect_type_bindings(param_full_type, arg->type, bindings, &n)`;
   other parameters get the ordinary kind and `type_eq` checks.
   - **`TY_TYVAR`:** the first binding of a name wins, and later arguments are
     checked against it with `type_eq`. A *provisional* binding yields: one to
     an `open_slot` variable, or to a variable that is not the enclosing
     signature's, is replaced by a later concrete type or signature variable
     ([open-arg-first-binds-call-result-as-int](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/open-arg-first-binds-call-result-as-int.md),
     [declared-tyvars-are-not-rigid](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/declared-tyvars-are-not-rigid.md)).
   - **`TY_APP`:** the head and argument are matched recursively on a scratch
     copy (so a failure leaves nothing half-bound). If that fails and the
     expected head is a type variable, it retries with a hole-headed partial
     application, so `(m a)` can match `(Result int cstr)` with
     `m := (Result _ cstr)`. A bare `TY_ADT` argument matches a `TY_APP`
     parameter when the heads agree (KB-022).
   - **`TY_FN`:** structural, over `arg_full_types` and `result_full_type`. A
     fat closure that arrives as `TY_PTR_VOID` is accepted unbound.
   - **`TY_REF_IMMUT`/`TY_REF_MUT`:** bind through `target_tyvar` or
     `target_full`.
   - **A rank-2 `TY_FORALL` parameter:** the caller's outer variables are
     collected from inside it by `call_collect_forall_outer_bindings`; the
     forall's own variables are wildcards.
   - **Anything else:** `type_eq`.
   - **A refined-ADT argument** (`(Refl)` for `eq : (Equal A B)`): the value
     fixes no index of its own, but every constructor's result annotation
     spells some positions alike (`Refl : (Equal a a)`), so after the loop
     `call_bind_refined_index_equations` binds whichever of `A`/`B` is still
     unbound to what the other is. `(conv (Refl) 7.1)` against `[eq : (Equal A
     B) x : A] : B` then reads `B := float`
     ([coerce-result-and-type-app-retirement-plan](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/coerce-result-and-type-app-retirement-plan.md)).
3. After the loop, if the call sits under a **ground** expected type and the
   callee is a global, bind the result's remaining variables from it. Further
   arms handle a bare-variable result in a generic body and a few other
   shapes. A non-ground expectation (the enclosing body's own `(Map K V)`) is
   not used.
4. Compute the call's type with `call_instantiate_type(result_full_type,
   bindings)`. A result variable that nothing bound and that collides with an
   enclosing signature variable is **renamed apart** to `callee.A` and marked
   `open_slot`, so `(ok x)` inside `[A B E]` is not `(Result B B)`
   ([generic-call-result-leaks-callee-tyvar-names](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/generic-call-result-leaks-callee-tyvar-names.md)).
5. Record the bindings on the node: `call_.abi_bindings`,
   `call_.n_abi_bindings`. The emitter specializes the callee from them (see
   [monomorphization-abi-guide.md](monomorphization-abi-guide.md)).

Two follow-ups change already-elaborated arguments:

- **Open-argument grounding.** An argument that is itself a return-only
  generic call (`(none)`, `(vec-new)`) and was checked before the sibling
  that fixed its variable is grounded afterwards: its own `abi_bindings` are
  recorded and its `type` is replaced.
- **Forward element inference (S4).** When a sibling fixes a variable, a
  *local* `let` binding passed at a generic slot has its `Binding.type`
  rewritten (`(let [v (vec-new)] (vec-push! v 7.1) ...)` makes `v` a
  `(Vec float)`). When the initializer is a call still open in the same
  variables (`(let [o (mk)] (unwrap-or o 7))` for a nullary
  `(defn mk [A] [] : (Option A) ...)`), that call is grounded with it, as an
  open argument is: the binding's `let_init` points at it, and its
  `abi_bindings` and `type` are recorded. Without that, a by-value result
  such as `(Option int)` was declared by value and initialized from the
  carrier word (archived:
  [let-bound-nullary-generic-retyped-invalid-c](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/let-bound-nullary-generic-retyped-invalid-c.md)).
  An initializer of another shape is not rewritten, so the rewrite is still
  only safe for it when its representation does not depend on the variable;
  a heap pointer such as `Vec` qualifies.

All of this is per call. Nothing flows sideways between sibling calls, and
nothing flows from a later use back to an earlier one except through the S4
rewrite above.

### Rigid variables (`sig_tyvars`)

`elab_defn` and `elab_fn` push every named variable of their signature onto
`sig_tyvars` (`fn_collect_sig_tyvars`) and pop on exit; `elab_definstance`
pushes the instance's constraint variables (the `A` of `[(Eq A)]`). The array was introduced so a GADT match arm
could tell a quantified result from an escaping skolem (KB-025). Call
inference now also uses it as the set of **rigid** variables of the body
being elaborated: `ng_tyvar_in_sig` decides whether a variable in an argument
is the body's own abstract type (rigid, kept abstract and specialized per
instantiation) or a callee's leftover (provisional).
`rigid_tyvar_conflict` (`elab_core.c`) reports a rigid variable meeting a
different rigid variable or a ground type; in return position that is
`RET_CONFLICT_RIGID_TYVAR`, reported as TUR-E0709.

### Carrier-aware return checking

Return types are not unified; they are checked. `return_position_conflict`
(`elab_core.c`) runs the return-position predicates in a fixed order and
reports the first conflict as a `ReturnConflict` (nominal, register class,
pointer/scalar both directions, bool/integer, carrier aggregate, `nil` body,
rigid tyvar). The `ReturnClass` argument decides how strict to be:

- `RET_CLASS_COMMITTED`: a concrete `defn`; everything is checked.
- `RET_CLASS_CARRIER_FN`: a generic or `#fx{Unsafe}` `defn`; the reverse
  pointer/scalar direction is the deliberate carrier bridge and is allowed.
- `RET_CLASS_CARRIER_METHOD`: an instance method.

A float never rides the int64 carrier, so the register-class check is
symmetric for every class. An integer-literal body under a float return is
widened in place first (`rc_widen_int_literal_to_float_return`).

### Worked failures

The first binding wins, and there is no numeric widening:

```
(defn pick [A] [x : A y : A] : A x)
(println (pick 7.1 3))

error [TUR-E0001]: function 'pick' arg 2: expected float, got int
```

Argument 1 bound `A := float`; argument 2 is checked against that binding.
Swapping the arguments reports "expected int, got float".

Typing is bottom-up, so an outer operator does not inform an inner generic:

```
(defn mk [A] [] : (Option A) (none))
(println (+ (unwrap-or (mk) 1) 0.5))

error [TUR-E0042]: mixed-width numeric arithmetic: '+' arg 2 is float, expected int
```

`unwrap-or`'s `A` was bound to `int` by the literal `1` before `+` saw the
`0.5`. Writing `1.0` binds `A := float` and the program prints `1.5`. When no
argument can say it, a typed binder does, because it pushes the expected type
onto `(mk)`: `(let [o : (Option float) (mk)] ...)`.

---

## What the elaborator records for later passes

Later passes read these instead of recomputing them. Keep them consistent
when you build or rewrite nodes.

| Where | Field | Meaning | Read by |
|---|---|---|---|
| `call_` | `abi_bindings`, `n_abi_bindings` | Type-variable substitution at this call | `emit_module.c` (specialization), `emit_cps_ir.c`, turi |
| `call_` | `dict_arg` | An `EX_DICT` naming the selected instance's dictionary | the emitter, `cps_ir.c`, `mono_specs.c`, `srfi_prune.c`, turi |
| `call_` | `dict_fwd_clone`, `dict_fwd_params`, `dict_fwd_n` | In a constrained generic, which dict clone of the callee to call when the caller is itself emitted as a dict clone | `emit_expr.c` |
| `call_` | `ctor`, `size_index`, `gadt_form` | A constructor call's `CtorDef`, and the inferred size or GADT index (elaboration-only) | the emitter and `cps.c` read `ctor` |
| `call_` | `site`, `forwards_site` | Where a panic in the callee should point: the macro use, or the call through a partial application | `emit_core.c`, `emit_cps_ir.c`, turi |
| `call_` | `is_poly_call`, `poly_arg_mask`, `poly_agg_arg_mask` | Calls through a rank-2 parameter, and which arguments are poly carriers or boxed aggregates | `emit_expr.c`, `cps_ir.c` |
| `call_` | `ptr_sig` | `call-ptr`'s explicit C signature | `emit_expr.c`, turi (JIT FFI) |
| `call_` | `wants_tailcall` | The author wrote `^tailcall` | `emit_fns.c` verifier (TUR-E0716) |
| `Expr` | `any_drop_after`, `sum_box_drop_after`, `sum_box_drop_after_dyn` | This argument produces a box no one else owns and the callee keeps nothing, so free it after the call | `emit_expr.c`, `emit_stmt.c` |
| `defer_` | `is_drop_glue` | Compiler-synthesized scope-exit drop, which may run before a tail call | `emit_fns.c` |
| `set_` | `release_old` | Release the rc value being overwritten | `emit_stmt.c` |
| `reinterpret_` | `source_kind`, `target_kind`, `retain` | A carrier crossing; `retain` says whether it mints a strong reference | the emitter |
| `poly_wrap_` | `wrapper_binding`, `dict_clone_binding`, `is_closure`, `boxes_aggregate`, `carrier_erased_*` | How a function value is packed for a rank-2 or `:fn` sink | `emit_expr.c`, turi |
| `Binding` | `nonretain_param_mask`, `nonretain_ptr_param_mask`, `nonretain_sum_param_mask`, `resalias_param_mask` | Inferred once per `defn`: which parameters the body never keeps | `elab_call.c` (lend instead of move), `emit_core.c`, `cps_ir.c` |
| `Binding` | `returns_fresh_closure`, `returns_fresh_sum_box`, `fresh_sum_via_param_mask` | The result is a fresh allocation the caller owns | the emitter |
| `Binding` | `moved_owning`, `lent_to_nonretaining` | How the binding stopped being usable | scope-exit drops |
| `FnDef` | `constraints`, `dict_clone_*`, `owner_instance` | Typeclass constraints, dictionary clones, and the instance a method belongs to | the emitter, the typeclass machinery |

**Regions.** No `Expr` field is region-specific. The elaborator's part is
one rewrite: a typed node passed to an inline-C callee's erased `:int`
parameter (or, for a Turmeric-bodied callee, a `:heap` node) is wrapped in an
`EX_ASCRIBE` to `TYPE_INT`, so the emitter's ascription arm notes the word.
Constructors are exempt. See `elab_call_fn_inner` near
`call_arg_is_region_node_word`,
[stdlib-region-store-hooks-unswept](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/stdlib-region-store-hooks-unswept.md)
and [regions-internals-guide.md](regions-internals-guide.md).

**Written after elaboration.** Some fields on the same nodes are owned by
later stages and are not set by the elaborator:

- `call_.is_tail_self_call`, `is_tail_call`, `tail_group_idx` (`emit_fns.c`
  `tco_mark`);
- `FnDef.cps_colored`, `may_await`, `async_reachable` (`cps_color_program`);
- `FnDef.may_capture`, `is_cps` and `cloneable_shift_.live_captures`
  (`cps_transform`);
- `FnDef.inferred_effect_row` (`effect_check.c`; the elaborator leaves it
  `NULL`);
- `rc_clone_.elide` and `rc_drop_.elide` (`rc_elision_analyze_fn`, run from
  `emit_fns.c`; the elaborator writes `false`);
- the `Binding.emit_*` notes (the emitter).

---

## Incremental sessions (`ElabSession`)

`ElabSession` is `struct Elab` itself, heap-allocated by `elab_session_new`.
Only the interpreter uses one (`src/turi/eval.c`, `TuriEnv.elab_session`, and
`src/turi/repl.c`); the compiler, `tur lsp` and the playground elaborate the
whole program every time. With a session, `elaborate_program_session`:

- **on entry** copies the session into its local `Elab`, re-anchors `scope`
  to `&global`, resets the per-file fields (`has_defmodule`, the current
  module) and takes the `turn_start_n_*` watermarks;
- **during the call** lets a definition below a watermark -- one from an
  earlier turn -- be replaced without a redefinition error
  (`elab_prior_turn_global` and friends);
- **on exit** copies the state back instead of freeing it, after clearing the
  per-call working sets. Refinement obligations are still discharged.

The caller hands over only the new forms. Nodes live in the arena of the call
that made them, so every such arena must outlive the session; the interpreter
keeps them all. After a failed call the session must be discarded (partial
definitions may be in its scope); the interpreter replays the committed turns
into a fresh one (`elab_session_replay`, muted) and falls back to
whole-program elaboration if that fails. Design:
[turi-incremental-elaboration-design](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/turi-incremental-elaboration-design.md)
and [playground-session-hygiene-plan](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/playground-session-hygiene-plan.md).

---

## Macros at elaboration time

There is no separate expansion pass. A macro call is expanded when
`elab_call_inner` reaches it, after the special forms and before binding
lookup, and the resulting `Form` is elaborated in place. So:

- **Order matters.** A macro is visible from its `defmacro` onwards in
  pass 2. Pass 1 does not register macros.
- **Visibility** follows modules (`elab_lookup_macro`): a macro is visible if
  it is referred, owned by no module, owned by the current module, under
  `tur/`, or owned by a module on `macro_expansion_stack`. The last rule lets
  an exported macro use its module's private helpers.
- **Depth** is capped at `ELAB_MAX_MACRO_EXPANSION_DEPTH` (256), with an
  earlier stop when the C stack runs low. Ordinary deep nesting is guarded
  separately by `elab_call`'s stack check (TUR-E0712; `TUR_STACK_MB`).
- **Diagnostics** inside an expansion are attributed to the outermost macro
  use (`macro_call_site_span`, `diag_set_expansion_site`), with a trailing
  "in expansion of macro" note. `call_.site` carries the use site to runtime
  panics.

`elab_macros.c` has two kinds (`MacroKind`):

- `MACRO_TEMPLATE` (`defmacro`): parameters are substituted into the body
  (`substitute_params`). If the template needs it, the result goes through the
  small compile-time evaluator (`ct_eval_form`, `CtEnv`). A `^syntax`
  parameter is passed as data
  ([macro-args-elaborated-before-expansion](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/macro-args-elaborated-before-expansion.md)).
- `MACRO_PROCEDURAL` (`defmacro*`): the body is compiled to a function in a
  capability-denied, fuel-limited turi env (`elab_macro_env_get`,
  `src/turi/macro_env.c`), created lazily and disposed with the elaborator
  ([macro-system-direction-plan](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/macro-system-direction-plan.md)).

User-facing behaviour is in [macros-guide.md](macros-guide.md).

---

## Checklist: adding a special form or an `EX_*` kind

**A special form that lowers to existing kinds** (the cheaper choice; prefer
it):

1. Cache its symbol in `Elab` and intern it in `elab_init_state`.
2. Dispatch it in `elab_call_inner`, next to its relatives. Dispatch order is
   the precedence order, so a special form shadows any macro or binding of
   that name. Add the name to the `reserved_special_forms_` table behind
   `tur_name_is_reserved_special_form`, so a `defn` or `defmacro` of that name
   gets TUR-W0042.
3. Write `elab_<form>` in the file for its family. Push and pop scopes on
   every exit path. Save and restore any `Elab` context field you set.
4. If the form binds names, decide how `collect_free_vars` sees them; that
   walk decides closure captures.
5. If it is a definition or a module directive, check the main fold (any
   `def*` head is treated as a definition) and pass 1.
6. Add a fixture under `tests/fixtures/`, and check the interpreter with
   `tur interpret f.tur` or `tests/run-turi.sh`.

**A new `ExprKind`** also needs every walker that enumerates kinds. Most
switches have a `default:` arm, so the C compiler will **not** list the
places for you. Grep for a neighbouring kind -- `grep -rn 'case EX_SHIFT0'
src/` today finds:

| Area | Files |
|---|---|
| The IR itself | `expr.h` (enum and payload), `expr.c` (`expr_print`) |
| Elaborator walks | `elab_core.c` (`collect_free_vars`, which decides closure captures; the consumption walks `is_binding_consumed` and `is_field_consumed`) and the `switch (... ->kind)` walks in `elab_fns.c`, `elab_call.c`, `elab_typeclasses.c` (ownership masks, escape analysis) |
| Shared operand enumeration | `src/passes/cps.c:cps_visit_children`. The CPS coloring and the effect checker fall back to it, so a kind with operands must be enumerated here or those walks will not see its children |
| Core passes | `src/passes/cps.c`, `cps_ir.c`, `effect_check.c`, `borrow_check.c`, `rc_elision.c`, `srfi_prune.c` |
| Emitter | `emit_expr.c` (`emit_value`), `emit_stmt.c`, `emit_module.c` (preamble feature scans), `emit_cps_ir.c` |
| Interpreter | `src/turi/eval.c` |
| Tooling | `src/compiler/span_audit.c` (`tur audit-spans`) |

The most recently added kind, `EX_DEFAULT_OF`, appears in `expr.c`,
`emit_expr.c`, `emit_stmt.c`, `borrow_check.c`, `cps_ir.c`, `srfi_prune.c` and
`turi/eval.c`. Use that as the minimum. A kind that is a control operator also
needs `cps_control_walk` (the coloring seed) and the CT-IR translator; see
[cps-dk-internals-guide.md](cps-dk-internals-guide.md). Then run
`python3 tools/check_turi_parity.py`, which checks that `turi/eval.c` handles
every kind (today: "119/120 EX_* kinds handled, 1 carved out, 0 gaps"). A kind
the interpreter cannot run belongs in `docs/artifacts/turi-carve-out.txt`; see
[turi-parity-guide.md](turi-parity-guide.md).

---

## Debugging elaboration

There is **no IR dump**. `expr_print` exists in `expr.c` but nothing calls
it; to see an elaborated tree, call it from a debugger or a temporary
`fprintf`. The useful tools are:

| Tool | Shows |
|---|---|
| `tur check f.tur` | Elaboration and the core passes, without emitting C |
| `tur expand f.tur` | Every macro expansion outside the stdlib preload, as a form (example below) |
| `tur emit-c --emit-abi-trace f.tur` | One stderr line per generic call: which specialization the emitter chose from `abi_bindings` |
| `tur --dump-kinds check f.tur` | Kinds after the kind-check pass (global flag, before the subcommand) |
| `tur --dump-effects check f.tur` | The inferred effect row of each `defn` |
| `tur --dump-mono-specs ...`, `--dump-sizes` | By-value HKT specializations; sized-GADT indices |
| `tur --timings check f.tur` | Time per phase, with `elaborate`, `macro-expand` and `typeclass` split out |
| `TUR_DUMP_MATCH_LOWER=1` | The form a `match` is rewritten to before elaboration |
| `TUR_REPR_CENSUS=1` | The representation chosen for each fn-typed parameter |
| `TUR_DEBUG_RESOLVER=1` | Module search-path breadcrumbs |
| `tur audit-spans f.tur` | Elaborates, then lists breakpoint-eligible nodes (top-level forms, `defn`s, `let`s, calls) that lack a usable span |
| `python3 tests/check-emitted-float-conversions.py f.tur` | Float/int value conversions in the emitted C. An int-typed call of a float function shows up here |

`tur expand` and `--emit-abi-trace` on a two-line program (trimmed):

```
$ tur expand exp1.tur
;; when @ exp1.tur:3:3
(if (> 3 2) (do (println (pick 7.1 2.5))))

$ tur emit-c --emit-abi-trace exp1.tur 2>&1 >/dev/null | grep pick
abi-trace 3:26 pick concrete-clone pick__spec__double_double_double
```

When a type looks wrong, the usual causes, in order of likelihood:

1. a forward declaration (the callee is defined below the caller);
2. a carrier default (an unannotated parameter or an `any` file);
3. a first-wins binding from an earlier argument;
4. a rewrite: the form you are reading is not the form being elaborated.

A clean `tur check` with a failing `cc` usually means the IR's types and the
recorded notes disagree. The emitter guides start there:
[emitter-architecture-guide.md](emitter-architecture-guide.md) and
[diagnosing-the-compiler-guide.md](diagnosing-the-compiler-guide.md).

---

## Bugs found while writing this guide

Each was reproduced against the compiler this guide describes and filed in
[`docs/reported/`](https://github.com/turmeric-lang/turmeric/blob/main/docs/reported/README.md). When one is fixed and archived,
update the passage above that describes it and drop it from this list.

None open: the one filed here,
[let-bound-nullary-generic-retyped-invalid-c](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/let-bound-nullary-generic-retyped-invalid-c.md),
is fixed and archived.

## Maintenance

Update this guide in the same PR when you:

- add or remove an `ExprKind` or `TypeKind`, or change a family above;
- add a step to `elaborate_program_session`, or change pass 1, the main fold
  or the pass-2 deferral rules;
- change `call_collect_type_bindings`, the expected-type channel, or the
  rigid-variable rules;
- add a field to the "recorded for later passes" table, or change who reads
  one;
- change the dispatch order in `elab_call_inner`;
- add a debugging flag or environment variable that inspects elaboration.

Cite functions, not line numbers. Regenerate the example outputs with
`tur run`, `tur expand` and `tur emit-c` when their shape changes.
