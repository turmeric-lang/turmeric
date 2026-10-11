# A lambda with a type-variable parameter, passed to an HKT method in a generic defn, is never emitted

**Severity: medium (cc failure on valid code).** **RESOLVED 2026-10-11** --
see [Resolution](#resolution). Filed 2026-10-11 while
executing [parsec-guide-plan](../archive/parsec-guide-plan.md). Reproduces on
`main` at v0.64.0.

## Repro

```turmeric
(defn w3 [A] [o : (Option A)] : (Option A)
  (bind o (fn [v : A] : (Option A) (some v))))

(defn main [] : int (println (unwrap (w3 (some 5)))) 0)
```

```
nl4_tur.c: In function 'w3__spec__tur_adt_Option__int_tur_adt_Option__int':
error: '__poly_8' undeclared (first use in this function)
```

The same shape with the `Parser` monad (`(bind p (fn [v : A] : (Parser A)
(pure v)))` inside `(defn w1 [A] ...)`) fails at link time with `undefined
reference to '__poly_802'`, and a nested continuation capturing such a
parameter (`(do-m v p vs (many q) (pure (list-cons v vs)))` with `v : A`)
fails with `'__fn_N' undeclared`.

A lambda whose parameter is typed by the enclosing signature's type variable,
handed to a dictionary-dispatched method (`bind`, `fmap`) inside a generic
`defn`, is referenced from the per-instantiation clone but its body is never
emitted. The same lambda passed to an ordinary higher-order `defn` compiles.

## Effect on parsec

`parsec-guide-plan` P8 types an un-annotated `do-m` binder from its
receiver -- `(do-m c (item) ...)` binds `c : Char`. Inside a generic `defn`
that would type binders with the enclosing tyvar (`v : A`) and walk straight
into this defect, so the inference is restricted to GROUND element types
(`elab_method_call`, src/compiler/elab_typeclasses.c): a generic parser's
binders keep the old int-carrier default. Lifting that restriction is the
test for a fix here.

## Fix direction

Find where the poly-wrapped lambda (`__poly_N` / `__fn_N`) of a generic
`defn`'s body is lifted: it is emitted for the carrier base or skipped, but the
spec clone re-elaborates the call and references a fresh name. Either emit the
lifted lambda per spec, or have the clone reuse the base's lifted lambda.

## Investigated 2026-10-11 (not fixed)

Two layers, and the undeclared name is only the first:

1. **The wrapper is skipped.** `__poly_8` is a file-level `EX_FN_DEF`
   (`make_poly_wrapper_ex`, elab_call.c). It is generic -- its lambda is typed
   by `A` -- and a generic function nothing calls through the carrier is
   dropped by `emit_abi_fn_skip_generic`. The clone passes it as a VALUE
   (`(tur_poly_fn_t){ NULL, __poly_8 }`), and `emit_abi_scan_expr` has no
   `EX_POLY_WRAP` case at all, so the use is never noted. Noting it there
   (inside a specialization, `emit_abi_note_carrier_call` on
   `poly_wrap_.wrapper_binding`) gets the wrapper emitted: `int64_t
   __poly_8(void *, int64_t)`, which calls the lambda's by-value spec.
2. **The `bind` spec and the wrapper then disagree on the result ABI.** With
   the wrapper present, cc fails one step later: the clone dispatches to
   `__inst_Monad_bind_Option__spec__tur_adt_Option__int_int64_t_int64_t`
   (receiver at the carrier), which reads the continuation's result as
   `int64_t` and assigns it to its by-value `tur_adt_Option__int` merge
   temp. The non-generic `(defn w4 [o : (Option int)] ...)` picks
   `..._tur_adt_Option__int_tur_adt_Option__int_int64_t` instead, and its
   `__poly_8` returns `tur_adt_Option__int`: both sides by value. So the fix is
   in which spec the generic clone selects (or the wrapper it mints), not only
   in emitting the wrapper; step 1 alone just moves the error.

## Resolution

Both layers, plus the two neighbours they exposed, and then the P8 restriction
the report named as the test. Each piece is narrow to a STATIC HKT dispatch
(concrete receiver head) whose element is the enclosing defn's own kind-`*`
signature variable.

1. **Elab attaches the element bindings** (`elab_typeclasses.c`). The HKT
   binding collector skips every tyvar actual, so `(bind o k)` with
   `o : (Option A)` bound only `m -> Option` and attached nothing; the clone
   could recover just `b` from its own result. A second, `sig_ok` pass
   (`m7_collect_tyvar_bindings_x`, `m7_is_star_sig_tyvar`) binds `a -> A`,
   `b -> A` symbolically, which `emit_abi_register_call` composes through the
   enclosing spec (`A -> int`) into the same `bind` spec a ground call picks.
   The call's result, the def-less `(? ?)` shell before, is committed as
   `(Option A)` so a generic consumer (`(unwrap-or (bind ...) d)`) accepts it.
2. **The same-method re-dispatch no longer short-circuits**
   (`emit_module.c`). With a typed result, the emit-time re-dispatch recovered
   the class var, landed on the very method the call names, and took its "note
   a carrier call and return" exit -- every element type on the carrier `bind`.
   That exit is skipped when re-dispatch changes nothing and elab attached
   element bindings.
3. **The rank-2 wrapper is cloned per specialization.** `__poly_N` forwards to
   a lambda typed by `A`, so it is generic. `emit_abi_scan_fn_values` now
   clones it like any generic fn value (`emit_abi_scan_one_fn_value`) and the
   pack site names the clone (`emit_fn_value_spec_clone`). Elab keeps `A` on
   the wrapper's parameter for a lifted lambda, so the clone takes the element
   the way the by-value `bind` spec passes it: natively (a `double`), except a
   by-value aggregate, which crosses as a word and is dereffed back
   (`poly_agg_arg_mask`, gated on the resolved type). Where no clone is needed
   (a carrier `Parser`), the base wrapper is noted -- in a specialization by
   the fn-value scan, in an emitted carrier body by the
   `emit_abi_carrier_relay_walk` fixpoint, which also now walks into
   `EX_POLY_WRAP` to reach a closure handed to a rank-2 parameter.
4. **Nested capturing continuations** (`(bind o (fn [v : A] ... (bind q (fn
   [w : A] ... v))))`, the `'__fn_N' undeclared` shape). The outer spec links
   only the closures its own body passes; `emit_abi_link_nested_passed_closures`
   links the ones a passed-closure clone passes in turn, recursively.
5. **P8 lifted.** `do-m` binders on a static dispatch are now typed at the
   signature variable too: `(do-m v p vs (many q) ...)` with `p : (Parser A)`
   binds `v : A`, `vs : (List A)`. The parsec guide's "generic `defn` binds as
   `int`" limit is gone.

Also fixed on the way: `emit_abi_scan_fn_values` read its bindings out of the
specializations array while interning into it (a heap-use-after-free once one
argument could intern two clones); it copies them first.

**Pinned by** `generic-hkt-lambda-tyvar-param` (Option: `int`, `float`,
`cstr`, a wide struct, `None`, a generic consumer, nested `bind` and `fmap`
continuations) and `parsec-generic-do-m-binder` (Parser `bind`, a nested
continuation, and an inferred generic `do-m`). Both fail to compile on the
previous compiler. gcc, clang and the JIT agree.
