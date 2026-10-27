# Partial application does not type-check captured (provided) arguments

**One-line summary:** `elab_partial_apply` binds each provided argument into a
capture binding using only `type_from_kind(arg_kinds[i])` and never checks the
provided argument's type against the parameter it fills. As a result an
under-saturated call accepts arguments that the equivalent saturated call
rejects -- including a plain `int` passed where an opaque/struct parameter is
expected.

**Severity:** Medium-High for type-safety. It is a silent "accepts ill-typed
program" hole on the partial-application path that mirrors (and is broader than)
the positional nominal-identity hole tracked in
[positional-nominal-type-identity-not-checked.md](positional-nominal-type-identity-not-checked.md).

**Status: FIXED.** Both the nominal-identity slice and the kind-level slice are
now closed. The capture loop in `elab_partial_apply` validates each provided
argument against the slot it fills whenever that slot is a struct/opaque/ADT
nominal type, emitting `TUR-E0001` on a mismatch.

---

## Minimal repro

### A. Plain `int` at an opaque slot during partial application (wrong)

```turmeric
(defopaque A :int)
(defn two [x : A y : A] : int ```c return (int64_t)(x + y); ```)
(defn mk-a [] : A ```c return 1; ```)
(defn main [] : int
  (let [f (two 5)]      ; provide a bare int where x:A is expected
    (f (mk-a))))
```

```sh
$ tur check repro.tur ; echo $?
0          # accepted -- expected a TUR-E0001 type mismatch
```

### Control: the saturated call rejects it correctly

```turmeric
(defopaque A :int)
(defn one [x : A] : int ```c return (int64_t)x; ```)
(defn main [] : int (one 5))   ; => TUR-E0001 "expected A, got int"
```

So the hole is specifically the **captured (already-provided) arguments of an
under-saturated call**. The not-yet-supplied (remaining) parameters are fine:
they are re-checked when the returned closure is later saturated, which routes
back through the saturated positional checker.

## Observed vs. expected

| Case | Observed | Expected |
|------|----------|----------|
| `int` captured at `:A` slot (pap) | accepted | `TUR-E0001` |
| `:B` captured at `:A` slot (pap), distinct opaques | **now rejected** (fixed) | rejected |
| `int` at `:A` in a saturated call | rejected | rejected (correct) |

---

## Root-cause analysis

`elab_partial_apply` (`src/compiler/elab_call.c`) builds capture bindings from
the coarse `arg_kinds` array and does not compare the provided argument's type
to the expected parameter type at all:

- `src/compiler/elab_call.c:1436` -- `TypeKind cap_kind = fn_type.as.fn.arg_kinds[...]`
- `cap_type = type_from_kind(cap_kind)` is used for the capture binding, and the
  provided `elab_args[i]` is wired into the inner call as a variable reference of
  that synthesized type. The provided arg's *own* type is never validated.

The saturated path, by contrast, runs a full arg-by-arg check with ~15
coercion escape hatches at `elab_call.c:~2035-2166`, ending in the
`TUR_E0001_TYPE_MISMATCH` emitter.

## What is already fixed

The **nominal-identity** slice of this hole (a same-kind struct/opaque/ADT
captured where a *different* same-kind nominal is expected, e.g. `:B` at `:A`)
is closed: a `type_eq`-based demotion now runs in the capture loop at
`elab_call.c:1436` and emits `TUR-E0001`. Covered by
`tests/fixtures/errors/positional-pap-opaque-mismatch` and the positive
`tests/fixtures/positional-pap-opaque-ok`. This landed alongside the positional
nominal-identity fix.

## What is now fixed (kind-level slice)

The **kind-level** slice (e.g. plain `int` -- `TY_INT` -- captured at a
`TY_STRUCT`/opaque slot) is now caught. The capture-loop check at
`elab_call.c:~1437` no longer requires the provided arg and the expected param
to share a `TypeKind`: when the slot is a struct/opaque/ADT nominal it demands
an exact `type_eq` against the recorded full type (falling back to a kind-level
compare when no full type is recorded), so a bare `int` at an opaque slot now
emits `TUR-E0001`. Covered by `tests/fixtures/errors/positional-pap-int-mismatch`.

## Proposed fix directions

1. **Run a real arg-by-arg check in the capture loop.** For each provided arg,
   reuse the saturated path's check: start from
   `arg_ok = (elab_args[i]->type.kind == cap_kind)`, apply the same escape
   hatches (`TY_TYVAR` params, `TY_PTR_VOID`/closure coercions, the
   `(Some 1.5)` poly-ADT-field path, fat-closure shims), and emit
   `TUR_E0001_TYPE_MISMATCH` when it stays false. The hatch chain at
   `elab_call.c:~2035-2166` is the reference implementation.
2. **Factor the saturated per-arg check into a shared helper** so both the
   saturated path and the partial-application capture loop call it, rather than
   duplicating the ~15 hatches (which is how the kinds drifted out of sync in
   the first place). This is the more durable fix but a larger refactor.

## How to validate a fix

- Add `tests/fixtures/errors/positional-pap-int-mismatch` (int at opaque slot
  during pap, expect `TUR-E0001`), mirroring the saturated control.
- Confirm legitimate pap coercions still work: a captureless lambda at a fat
  closure param, a poly-ADT field, a `:ptr<void>` callback slot.
- Full `tests/run.sh` -> zero `FAIL`.
