---
title: M4b handoff — instance→spec backlinks shipped, dispatch-rewrite deferred to M4c
category: Planning -- ABI / Codegen rework
description: M4b's deliverable for `docs/archive/m4-typeclass-per-method-abi-plan.md`. Two backlink fields wired across elab + emit; the dispatch-site rewrite that actually generates and routes per-instantiation specs is moved into M4c, where it slots cleanly next to the dict-singleton rewrite.
---

# M4b — instance→spec backlinks shipped

## What landed this turn

### `FnDef.owner_instance` (elab side)

`src/compiler/expr.h:407` — new field on `struct FnDef`:

```c
struct TypeClassInstance *owner_instance;
```

NULL for ordinary defns and lifted lambdas. Populated by
`elab_definstance` in `src/compiler/elab_typeclasses.c:2611` immediately
after each method's FnDef is constructed:

```c
method_impls[i] = method_fd;
method_fd->owner_instance = inst;
```

This is the cheap O(1) reverse lookup from a method FnDef to its owning
TypeClassInstance — the path emit_module.c needs to identify instance
methods without re-scanning every instance every time.

### `EmitAbiSpecialization.typeclass_inst` (emit side)

`src/compiler/emit_internal.h:107` — new field on `struct
EmitAbiSpecialization`:

```c
struct TypeClassInstance *typeclass_inst;
```

NULL for ordinary defn specs and for HKT-class instance methods (which
keep the uniform carrier ABI per Plan M6/M7). Set by
`emit_abi_intern_spec` in `emit_module.c:697-717` when `fd->owner_instance`
is non-NULL AND the class is non-HKT (no `type_param_kinds[i]` ≠
`KIND_STAR`).

The detection mirrors the M4a-shipped HKT classifier:

```c
TypeClass *tc = fd->owner_instance->typeclass;
bool is_hkt = false;
if (tc->type_param_kinds) {
    for (uint8_t i = 0; i < tc->n_type_params; i++) {
        if (tc->type_param_kinds[i] != KIND_STAR) {
            is_hkt = true; break;
        }
    }
}
if (!is_hkt) spec->typeclass_inst = fd->owner_instance;
```

`memset(spec, 0, sizeof(*spec))` at `emit_module.c:686` zero-inits the
field on every fresh spec — no further plumbing.

## What's deferred to M4c, and why

The plan originally split M4b into "per-instantiation instance-method
emit" and M4c into "per-instantiation dict singleton + dispatch
rewrite." Working through M4b empirically, the dispatch-site
re-elaboration step turned out to be tightly coupled to the dict-slot
type change — splitting them across two turns creates a window where
specs exist but the dict still names the carrier symbol, producing
*two* implementations of the same logical method and ambiguous
linker resolution.

The cleaner slice is: **the spec request + the dispatch-site rewrite
happen together in M4c**, with the M4b backlinks already in place so
the M4c surgery is local to one function rather than threading
context through the whole emit pipeline.

Specifically, M4c's first concrete step:

1. In `emit_abi_scan_expr` (`emit_module.c:763`), when visiting an
   EX_CALL with `dict_arg != NULL` and `fn_expr->kind == EX_DICT`,
   pull `inst = fn_expr->as.dict_.instance`. If `inst->typeclass`
   passes the HKT-classifier (`KIND_STAR` on every type-param),
   register a spec via `emit_abi_intern_spec` whose `arg_types[]`
   come from the call's concrete argument types (not the abstract
   tyvars on the instance method).
2. The spec gets emitted by the existing `emit_implementation` loop
   under the name `__inst_<Class>_<method>__spec__<type-arg-mangle>`,
   with its body re-elaborated against the per-instantiation param
   types — the existing per-instantiation infrastructure
   (`current_abi_specialization`, `needs_box_spill`, the M2b
   `m2b_carrier_synth` path) already handles the re-elaboration
   shape for `make-struct`-bodied instance methods.
3. In `emit_stmt.c:457-533`, the dict struct + singleton emit walks
   the per-instantiation specs collected at scan time and emits
   `typedef struct dict_<Class>__<type-args>` with typed slots
   matching `spec->arg_types[]`/`spec->result_type`, plus a
   `static dict_<Class>__<type-args> dict_<Class>__<type-args>_singleton
   = {.<method> = <spec_clone_name>}`.
4. In `emit_expr.c:1313` (EX_DICT) and `emit_expr.c:1749` (the
   `(intptr_t)` cast in the indirect-call dispatch), emit the
   per-instantiation singleton's address with no cast — the slot's
   declared type matches the spec clone's signature.

The carrier-bridge call sites in `emit_expr.c` are then either:
- guarded by `expr_emits_byvalue_carrier_abi` that returns false post-M4c
  (no by-value aggregate ever reaches the dispatch arg slot — it's a
  typed by-value param now), or
- still wrapping the few remaining HKT-class dispatch sites (Functor/
  Monad), which is correct — those keep the carrier ABI for now.

## Validation

- Build: clean.
- Suite: 172 FAIL — exact pre-existing baseline. The M4b backlinks are
  inert; nothing reads `owner_instance` or `typeclass_inst` yet.

## Related

- [m4-typeclass-per-method-abi-plan.md](m4-typeclass-per-method-abi-plan.md)
- [m4a-audit-findings.md](m4a-audit-findings.md)
- [../archive/history/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md](../archive/history/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md)
- `src/compiler/expr.h:407` — `FnDef.owner_instance`
- `src/compiler/elab_typeclasses.c:2611` — backlink population
- `src/compiler/emit_internal.h:107` — `EmitAbiSpecialization.typeclass_inst`
- `src/compiler/emit_module.c:697-717` — backlink read at intern time
