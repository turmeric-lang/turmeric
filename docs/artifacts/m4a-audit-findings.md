---
title: M4a findings — audit + ABI-binding infrastructure
category: Planning -- ABI / Codegen rework
description: Audit results and ABI-binding scaffolding for `docs/archive/m4-typeclass-per-method-abi-plan.md` §M4a. Handoff to M4b.
---

# M4a — audit + ABI-binding infrastructure

This document records the audit results that M4b/M4c work from, plus the
minimal `EmitAbiSpecialization` extension shipped this turn. No emit/runtime
semantics change.

## Dict-slot generation site (single)

**`src/compiler/emit_stmt.c:457-533`** — the `case EX_INSTANCE_DEF` handler.

Walks the instance's methods, computes each method's C signature from
`method_impl->binding->type` (with `emit_carrier_return_override` and
`emit_inst_fn_return_carrier` overrides), and writes one `typedef struct
dict_<Class><type_suffix>` with one function-pointer field per method. Also
emits the `static dict_<Class><type_suffix> dict_<Class><type_suffix>_singleton`
filled with the `__inst_*` symbols.

`type_suffix` is computed at lines 380-450 from `inst->type_args[]`. For
TY_APP type-args it walks the head + first-arg name (e.g. `result_int` for
`(Result int)` instances). For primitive type-args it uses the kind name
(`int`, `bool`, `cstr`, …).

The slot's C type comes from `type_c_name(method_impl->param_types[j])`
(line 527). For primitive type-args this is the concrete C type
(`int64_t`/`bool`/`const char *`/…). For TY_APP with abstract TYVARs
(`(Tuple2 A B)`), `type_c_name` collapses to `int64_t` — **this is where the
carrier ABI enters**.

## Dispatch site (single fan-in path)

**`src/compiler/emit_expr.c:1313-1330`** — `case EX_DICT`. Emits either:
- `dict_<Class><type_suffix>_singleton.<method>` (dispatch-from-method-field),
- `(int64_t)(intptr_t)(&dict_<Class><type_suffix>_singleton)` (bare dict
  passed as `^&` to a constrained polymorphic fn).

**`src/compiler/emit_expr.c:1656-1764`** — `case EX_CALL` with `fn_expr` set.
When the fn_expr is the dispatch-from-method-field form above, the call
wraps it as

```c
((<ret_t> (*)(<arg_t>, ...))(intptr_t)(<dict_X_singleton.method>))(<args>)
```

(line 1749). The `(intptr_t)` cast is the load-bearing carrier-ABI handshake.
This is the cast M4 retires by making the slot's declared C type match the
real instance method signature.

## Carrier-bridge sites still firing at the dispatch boundary

Per-fixture audit (`TUR_M3_AUDIT=1`, direct `tur build` outside the harness)
confirms the 2 of 4 `emit_carrier_bridge` call sites still fire **only at the
typeclass-dispatch boundary**:

| Site | Direction | Reason |
|---|---|---|
| `emit_expr.c:2482` (matched_spec aggregate arg) | `carrier→concrete` | int64 handle from typeclass-method dispatch passed to a by-value spec arg |
| `emit_expr.c:2546` (dict_arg concrete agg) | `concrete→carrier` | by-value aggregate passed to a uniform-carrier dict slot |
| `emit_expr.c:4166` (EX_ASCRIBE int→agg) | `carrier→concrete` | `(:: (decode …) (Result int cstr))` — ascription pins by-value over a carrier producer |
| `emit_expr.c:1721` (concrete→carrier KB-021) | not observed | guarded by `needs_carrier_bridge && expr_emits_byvalue_carrier_abi`; both conditions ~always false post-M2 |

Three of the four sites are typeclass-related; one is concrete-bridge dead
code that the carrier-skip gate (m2b) already short-circuits but the
surrounding guards are still load-bearing for the `else if` chain. M4b/M4c
retires the typeclass sites; M4d also retires the dead site.

## HKT vs non-HKT classification

The `TypeClass` struct doesn't carry an `is_hkt` field directly. The
classifier is `TypeClass.type_param_kinds[i]` from `types.h`:

| Kind constant | Meaning |
|---|---|
| `KIND_STAR` (0) | `*` — concrete type (int, bool, struct, type-app) |
| `KIND_ARROW` (1) | `* -> *` — unary type constructor (vec, option) |
| `KIND_ARROW2` (2) | `* -> * -> *` — binary constructor (result) |
| `KIND_ARROW3`–`KIND_ARROW4` | higher arities |

**A typeclass is HKT iff any of its `type_param_kinds[i]` ≠ `KIND_STAR`.**
When `type_param_kinds == NULL`, treat as all-STAR (legacy default per the
`Phase HKT (v2, stub)` comment on the field).

### Stdlib class catalog

Non-HKT (all type-params KIND_STAR — M4 retires their carrier dict):

| Class | Source | Signature |
|---|---|---|
| `Eq` | `stdlib/typeclass-eq.tur:16` | `[a]` |
| `Ord` | `stdlib/typeclass.tur:15` | `[a]` |
| `Show` | `stdlib/typeclass.tur:96` | `[a]` |
| `Num` | `stdlib/typeclass.tur:206` | `[a]` |
| `Clone` | `stdlib/typeclass-clone.tur:24` / `stdlib/typeclass.tur:300` | `[a]` |
| `Hash` | `stdlib/typeclass-hash.tur:28` | `[a]` |
| `MapKey` | `stdlib/map.tur:348` | `[a]` |
| `Serializable` | `stdlib/serial.tur:154` | `[a]` |
| `Category` | `stdlib/arrow.tur:303` | `[arr]` (KIND_STAR) |
| `Arrow` | `stdlib/arrow.tur:315` | `[a]` (KIND_STAR per syntax) |
| `ArrowZero` / `ArrowPlus` / `ArrowChoice` / `ArrowLoop` / `ArrowApply` | `stdlib/arrow.tur` | `[a]` |

HKT (any type-param KIND_ARROW+ — keep carrier dict for now, retired by M6/M7):

| Class | Source | Signature | Kind |
|---|---|---|---|
| `Functor` | `stdlib/typeclass-functor.tur:19` | `[^f]` | `* -> *` |
| `Applicative` | `stdlib/typeclass-applicative.tur:12` | `[^f]` | `* -> *` |
| `Monad` | `stdlib/typeclass-monad.tur:13` | `[^m]` | `* -> *` |
| `Alternative` | `stdlib/typeclass-alternative.tur:12` | `[^f]` | `* -> *` |
| `Foldable` | `stdlib/rc.tur:83` / `stdlib/typeclass.tur:400` | `[^t]` | `* -> *` |
| `Traversable` | `stdlib/typeclass.tur:414` | `[^t]` | `* -> *` |
| `MonadError` | `stdlib/typeclass-monaderror.tur:14` | `[^m]` | `* -> *` |
| `Bifunctor` | `stdlib/typeclass-bifunctor.tur:12` | `[^^f]` | `* -> * -> *` |

The HKT carve-out gate at M4b/M4c is: `instance->typeclass->type_param_kinds[i] != KIND_STAR` for any `i`. When NULL, the kinds default to all-STAR — those instances qualify for M4. Stdlib + spice code reviewed; no class outside this list is in tree.

## `EmitAbiSpecialization` extension (shipped this turn)

`src/compiler/emit_internal.h:107` — added one field:

```c
struct TypeClassInstance *typeclass_inst;
```

NULL for ordinary defn specs and for HKT-class instance methods (kept on
the carrier ABI). Set by M4b's `emit_abi_intern_spec` call path when
`fn->binding` is an `__inst_*` method on a non-HKT class.

`memset(spec, 0, sizeof(*spec))` at `emit_module.c:686` already zero-inits
the field on every fresh spec — no further plumbing needed at this point.

M4b uses this field to:
- Skip emitting the carrier-ABI clone for instance-method specs (the dict
  singleton points at the per-instantiation spec, not the unspecialized
  carrier symbol).
- Mangle the dict struct name with the spec's type-arg tuple.

M4c uses it to:
- Pick which `dict_<Class>__<type_args>_singleton` to reference at the
  dispatch site, so the `(intptr_t)` cast can be dropped.

## Validation

- Build: clean (`cmake --build build -j` no warnings).
- Suite: unchanged (no code path touches the new field yet); 172 FAIL pre-existing baseline.

## Handoff to M4b

M4b's first concrete step: in `emit_module.c`'s `emit_abi_intern_spec` (line
655), when `fn_binding` resolves to a typeclass instance method on a non-HKT
class, populate `spec->typeclass_inst` from the instance. The current
identifier-lookup path is `fn_binding -> name -> Symbol` matching against
`__inst_<Class>_<method>` mangled forms — a small grep over emit_module's
spec-scan path locates the right plumbing point.

## Related

- [m4-typeclass-per-method-abi-plan.md](m4-typeclass-per-method-abi-plan.md)
- [../archive/history/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md](../archive/history/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md)
- `src/compiler/emit_stmt.c:451` (dict-slot emit)
- `src/compiler/emit_expr.c:1313` (EX_DICT)
- `src/compiler/emit_expr.c:1749` (`(intptr_t)` cast at indirect-call dispatch)
- `src/compiler/typeclass.h:51` (`TypeClass` struct + `type_param_kinds`)
- `src/compiler/types.h:62-66` (Kind constants)
