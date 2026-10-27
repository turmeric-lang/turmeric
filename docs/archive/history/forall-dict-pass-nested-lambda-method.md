# forall-dict-pass: constraint method inside a nested lambda is rejected, not lowered

**Severity:** medium (expressiveness hole; guarded, so no miscompile)
**Status:** RESOLVED (primary case) 2026-07-06 -- a captureless nested mapper
that dispatches a SINGLE constraint method (the canonical van Laarhoven
`(fn [x] (show x))`) is now LOWERED: the mapper is converted into a closure that
captures the constraint's runtime dict and dispatches through the env-loaded dict
(`docs/archive/history/forall-dict-pass-nested-lambda-dispatch-plan.md`). Positive fixture
`tests/fixtures/van-laarhoven-lens-show-mapper/`. A narrow residual (a mapper
that dispatches >1 class, or a capturing mapper, or a dispatch in a deeper nested
lambda) is still guarded with TUR-E0311 -- negative fixture
`tests/fixtures/errors/forall-dict-nested-lambda-multiclass/`; tracked as future
work in `docs/upcoming/v1/forall-dict-pass-nested-mapper-general-plan.md`.
**Area:** `src/compiler/elab_call.c` (dict-clone lowering), `src/compiler/emit_core.c`
(dict-param dispatch).

## Summary

Under the (now always-on) `forall-dict-pass` machinery, a genuinely polymorphic
constrained function that is passed as a rank-2 argument is lowered to a
*dict-clone* whose body dispatches each class method through a runtime dictionary
param, one per constraint. This works when the method call sits **directly in the
clone body** (including inside a `let` binding). It does **not** work when the
method call sits **inside a nested lambda** that the clone body passes to another
call -- the canonical case being a van Laarhoven mapper:

```turmeric
;; (Functor f, Show a) => (a -> f a) -> a -> (f cstr)
(defn show-lens [^f a] [^Functor f ^Show a g : (-> a (f a)) s : a] : (f cstr)
  (fmap (g s) (fn [x : a] : cstr (show x))))   ; <-- `show` inside the mapper lambda
```

`fmap` (directly in the body) dispatches correctly through the `Functor` dict
slot. `show`, inside the `(fn [x : a] ...)` mapper, does not: the mapper is
lambda-lifted to its own top-level C function with **no dict param in scope**, so
`show x` silently mis-resolved to the carrier-representative instance (`Show int`)
regardless of the caller's chosen `a` -- e.g. `(shown-bool show-lens true)`
printed `int:1` instead of `bool:true`.

## Current behavior (the guard)

Rather than miscompile, the shape is now **rejected** at the rank-2 pass site with
a specific diagnostic:

```
TUR-E0311: forall-dict-pass: 'show-lens' dispatches a typeclass method on its
constrained type variable from inside a nested lambda; the lambda is lifted
without the runtime dictionary in scope, so the dispatch cannot be routed.
Dispatch the method directly in the function body (bind it in a `let`) instead
of inside a closure passed to another call
```

The detection lives in `dict_clone_nested_dispatch_rec` (elab_call.c): it walks
the original fn body and returns true when a call carrying a `dict_arg` whose
class is one of the fn's constraints, with a bare-`TY_TYVAR` receiver, appears
inside a lifted lambda (an inline `EX_FN` or an `EX_VAR` to an
`is_lifted_lambda` FnDef).

## Workaround

Dispatch the constraint method **directly in the body** and thread the result
into the mapper as an ordinary captured value:

```turmeric
(defn show-lens [^f a] [^Functor f ^Show a g : (-> a (f a)) s : a] : (f cstr)
  (let [tag (show s)]                          ; `show` in the body -- supported
    (fmap (g s) (fn [x : a] : cstr tag))))     ; mapper captures `tag`, no dispatch
```

This is the form exercised by the positive fixture
`tests/fixtures/van-laarhoven-lens-show/` and is a natural lens-with-`Show`
(e.g. logging the focus during the traversal).

## Root cause

The mapper lambda is lifted **once**, at elaboration of the original constrained
fn, before any dict-clone exists; the dict params are synthesized per call-site
clone (`make_dict_clone`). The shared lifted lambda has no way to name a
clone-specific dict binding, and its `tur_poly_fn_t` is currently built with a
null env, so there is no runtime channel to hand it the dict either.

## Fix directions

Make the nested lambda **capture the ambient dict** as a closure free variable:

1. In `make_dict_clone`, deep-copy the body (rather than sharing `orig->body`) so
   each clone owns its lifted lambdas, and for each nested lambda that references
   a constraint method, add the matching dict param binding to that lambda's
   `captures`.
2. Emit the mapper's `tur_poly_fn_t` env populated with the clone's dict
   pointer(s) at the construction site inside the clone body.
3. In `emit_call_name`, when the active dict-dispatch class maps to a **captured**
   (env-loaded) dict rather than a direct param, dispatch through the env load.

Once lowered, drop the TUR-E0311 guard and promote the negative fixture
`tests/fixtures/errors/forall-dict-nested-lambda-method/` to a positive one.
