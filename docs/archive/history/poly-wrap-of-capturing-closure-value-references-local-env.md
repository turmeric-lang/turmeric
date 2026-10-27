# poly-wrap of a capturing closure value emits a file-scope wrapper that references a local env var

- **Severity:** silent-until-cc hard error (uncompilable generated C); previously
  masked because the only way to reach it crashed earlier. A miscompile of the
  closure-carrier path, not an ergonomics gap.
- **Status:** FIXED (this session). The report is retained as the change rationale.

## Summary

Passing a **capturing closure value** -- a `:ptr<void>` bound to a local, e.g.
the result of `(make-adder 10)` -- to a `:fn` poly-closure parameter (either a
hand-written `[f :fn]` defn parameter or a `:fn` typeclass-method parameter)
produced uncompilable C. The compiler routed the argument through
`make_poly_wrapper`, which emits a **file-scope** wrapper thunk whose body
statically references the closure's **local** env variable:

```c
static int64_t __poly_905(void * __poly_env_906, int64_t __poly_x0_908) {
    return __fn_898((void *)(intptr_t)(add10_904), __poly_x0_908);
    /*                                 ^^^^^^^^^ a local in main(), out of
     *                                 scope at file scope */
}
```

`cc` then fails: `'add10_904' undeclared (first use in this function)`.

## Minimal repro

```turmeric
(defclass Box [^w]
  (boxmap [wa f : fn] : int))

(defn __id_new [x] : int
  ```c int64_t *p = malloc(sizeof(int64_t)); *p = x; return (int64_t)(intptr_t)p; ```)
(defn __id_get [wa] : int
  ```c return *(int64_t*)(intptr_t)wa; ```)

(definstance Box [idbox]
  (boxmap [wa f] (__id_new (f wa))))

(defn make-adder [k : int] : ptr<void> (fn [wa : int] : int (+ (__id_get wa) k)))

(defn main [] : int
  (let [w       (__id_new 42)
        adder10 (make-adder 10)]      ;; capturing closure VALUE bound to a local
    (println (__id_get (.boxmap w adder10))))   ;; expect 52
  0)
```

- **Observed:** `cc invocation failed`, `'adder10_NNN' undeclared`.
- **Expected:** `52`.

A captureless lambda (`(.boxmap w (fn [wa : int] : int (* (__id_get wa) 2)))`)
compiled fine, because its inner thunk is a top-level function whose env is
unused -- the bug is specific to a *capturing* closure value.

## Root cause

`make_poly_wrapper` can only build a correct wrapper for a **statically named**
function (env ignored). For a runtime closure value it has no way to materialise
the env at file scope, so it baked in the local variable name.

The wrong-path selection sat in two arg-wrapping sites:

- `src/compiler/elab_call.c` -- the `:fn` (`arg_poly_fn`) parameter path.
- `src/compiler/elab_typeclasses.c` -- the `:fn` typeclass-method-param path.

Both resolved the arg to a non-NULL `inner_fn_b` (the closure value's binding,
which carries a `closure_fn_binding`) and unconditionally took the
`make_poly_wrapper` branch. The correct branch -- inline packing into
`tur_poly_fn_t` via `EX_POLY_WRAP{ is_closure = true }`, which reads the thunk
from the box's slot 0 at runtime -- was only reached when `inner_fn_b` was NULL
(a direct lambda literal).

## Fix

In both sites, when `inner_fn_b` is a capturing closure value
(`closure_fn_binding != NULL && !is_global`), route to the inline `is_closure`
packing path instead of `make_poly_wrapper`. The runtime box's slot-0 thunk is
read at the call site, so a capturing closure round-trips exactly like a
captureless one.

A second, cosmetic fix in `src/compiler/emit_expr.c` casts the closure value
through `intptr_t` in the `is_closure` emit (`void *tmp = (void *)(intptr_t)(...)`)
so a closure value carried as `int64_t` no longer triggers an int-to-pointer
warning.

## Validation

- `tests/fixtures/poly-fn-typeclass-capturing-closure/` -- focused regression.
- `tests/fixtures/comonad-capturing-closure/` -- the stdlib consumer that
  motivated the find (`stdlib/comonad.tur` `extend` migrated to a `:fn` carrier).
- `bash tests/run.sh`: `1482 passed, 0 failed` (leak detection on).
