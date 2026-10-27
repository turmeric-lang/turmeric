# Codegen: a by-value struct/ADT local with an owning field leaks it at scope exit

**Status:** RESOLVED on the direct path. `elab_let` now injects a scope-exit
auto-drop for every by-value ADT/record local carrying an owning `rc`/`ref`
field, mirroring the existing bare-`rc` auto-drop. See "Resolution" below.

**CPS follow-up (now tracked):** that injected `(defer (rc/drop (.f o)))` has no
CT-IR lowering, so a COLORED function carrying such a local evicts to the
whole-function fallback (`BODY-UNSUPPORTED ... EX_DEFER`). Lowering it under CPS
-- the missing generalization of O1-b, which only lowered the `ref` shape -- is
`docs/archive/cps-backend-owning-autodrop-lowering-plan.md`. (This closes the
"not currently tracked anywhere" note below for the CPS side.)

**Severity:** medium (memory leak; no crash / miscompile). Mainline codegen --
not CPS-backend-specific. Likely part of the deferred owning-pointer lifecycle
work (graduation-gate "item 4"), but not currently tracked anywhere.

## Summary

A local binding of a by-value struct / record-ADT that has drop glue (an owning
`rc` / `ref` / `weak` field, i.e. `AdtDef.needs_drop_glue == true`) is never
released at scope exit. A *bare* `rc` local IS auto-dropped (a `(defer (drop!
r))` is injected, lowering to `rc_strong_decrement`), but an `rc` nested inside a
by-value struct local is not -- the generated `drop_glue_tur_adt_<T>` is emitted
but never called, so the field's control block and payload leak.

## Minimal repro

```turmeric
(defstruct Own [r : rc<int> tag : int])
(defn f [] : int
  (let [o (make-struct Own :r (rc/of 7) :tag 9)]    ; o owns an rc<int>
    (.tag o)))                                      ; o does not escape
(defn main [] : int (println (f)) 0)
```

`tur emit-c` for `f`:

```c
static int64_t f() {
    int64_t __t181;
    {
        int64_t *__t182 = (int64_t *)malloc(sizeof(int64_t));
        *__t182 = INT64_C(7);
        RcControlBlock *__t183 = rc_cb_alloc(0, 3, NULL);   // refcount = 1
        __t183->value = __t182;
        tur_adt_Own o_1267 = ctor_Own(__t183, INT64_C(9));
        __t181 = (int64_t)(o_1267).tag;
    }                                                       // <-- no release of o's rc
    return __t181;                                          // __t183 + __t182 leak
}
```

`drop_glue_tur_adt_Own` is *defined* in the output but never *called*. Contrast a
bare `rc` local, which gets `struct __defer_env {...}` + `rc_strong_decrement` at
scope exit.

## Root cause

`src/compiler/elab_forms.c` (elab_let, the scope-exit auto-drop injection near
lines 960-1000) counts and injects `(defer (drop! b))` only for bindings whose
`type.kind == TY_REF` (linear refs). There is no arm for a by-value ADT local
whose `AdtDef.needs_drop_glue` is set, so such a local's fields are never
released. The drop glue itself is generated (`emit_adt_byval_drop_glue`,
`src/compiler/emit_module.c`), so only the *invocation* at scope exit is missing.

(The bare-`rc` auto-drop is injected through a separate path, which is why an
`rc` local is released but the same `rc` nested in a by-value struct local is
not.)

## Fix directions

Extend the elab_let auto-drop injection to also fire for a local binding whose
type is a by-value ADT (`TY_ADT` / `TY_APP`) with `needs_drop_glue`, mirroring
the existing `TY_REF` count + inject loops: inject a scope-exit
`drop_glue_tur_adt_<T>(&local)` (or the equivalent `(defer (drop! local))` that
lowers to it), guarded by the same moved / explicitly-consumed / consumed-by-use
filters so it does not double-drop a value that escapes or is passed to a
consuming call.

## Resolution

`src/compiler/elab_forms.c` (`elab_let`) gained a new scope-exit auto-drop
injection immediately after the existing bare-`rc` injection, gated by a
file-local helper `elab_byval_drop_adt(Type)`.  The helper mirrors the emit-side
predicate for `emit_adt_byval_drop_glue`: a non-`:heap`, `needs_drop_glue`,
single-ctor product laid out by value (`adt_is_byvalue_product` for a bare
`TY_ADT`; `adt_app_is_byvalue_product` for a concrete `TY_APP` monomorph).

For each such local, one scope-exit `defer` is injected per owning field, reusing
the existing drop nodes so no new expr kind / pass plumbing was needed:

- `rc` field -> `(defer (rc/drop (.f o)))`  (lowers to `rc_strong_decrement` +
  `rc_free_queue_drain`, exactly what `drop_glue_tur_adt_<T>` emits).
- `ref`/`lref` field -> `(defer (drop! (.f o)))`  (lowers to `free`, matching the
  glue).

The injection is guarded by the same moved / consumed / consumed-by-use filters
the bare-`rc` path uses (`binding_moved_during_init`, `Binding.is_moved`,
`is_binding_consumed`), so a value that escapes -- returned in tail position,
moved into a consuming call, or explicitly dropped -- is not double-dropped.
Wrapping a by-value local in `rc/of` is separately rejected by the uniqueness
checker (TUR-E0202), so that double-free path cannot arise.

The defer reuses the ordinary defer/frame machinery, so the release fires on
early-return paths too, and the local is captured into the defer env by value
(the copied aggregate shares the same `RcControlBlock *`, so the decrement lands
on the live block).

Intentionally out of scope (unchanged, matching the shallow `drop_glue`):

- `weak` fields -- non-owning (they carry no payload); there is no scope-exit
  `weak`-decrement primitive to reuse, and the reported defect is about *owning*
  fields.  The `rc/of` drop-glue path still decrements them via
  `rc_weak_decrement`.
- Nested by-value aggregate fields (a struct field that is itself a by-value
  drop-gluey ADT).  `emit_adt_byval_drop_glue` is itself shallow (it releases
  only direct `rc`/`ref`/`weak` fields), so the local-drop mirrors it exactly;
  deep nested release is a separate, pre-existing gap.
- By-value ADT **parameters** with owning fields.  Parameter ownership is a
  distinct injection site (function-body elaboration, not `elab_let`) with its
  own caller/callee double-free considerations; this report is specifically the
  *local* case.

Regression fixture: `tests/fixtures/byval-adt-local-owning-field-drop/` -- a
by-value `Own` local that does not escape; its `rc` field's strong count returns
to 1 at scope exit (would stay at 2 = leak without the fix).

## Relationship to the CPS backend

The `^borrow` owning-capture slice
(`docs/reported/cps-backend-owning-capture-multishot-double-free.md`, partially
resolved) relies on a `^borrow` value being read-only and never dropped by the
callee -- which is a type-system guarantee independent of this leak. This leak is
orthogonal: it is about an *owned* local (not a borrow) failing to be released,
in the ordinary direct emitter.
