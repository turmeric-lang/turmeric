---
title: PR #386's let-bound source_binding alias miscompiles captureless closure-returning lambdas + with-resource macro emits wrong C type
severity: medium -- silent miscompile at clang time (-Wint-conversion fires; would be a hard runtime bug otherwise). Two fixtures regress: curried-fn-typed-param-application and with-resource-basic.
status: resolved
discovered: 2026-06-15
resolved: 2026-06-17
introduced-by: PR #386 (849a8711 "Fix generic-dict dispatch re-resolution under --interpret")
---

## Resolution (2026-06-17)

Fixed via the report's **Option 2** (the surgical, single-site fix). A new
`is_lifted_lambda` flag on `Binding` (`src/compiler/expr.h`) is set when a
captureless lambda is lifted to a global `__fn_N` helper
(`src/compiler/elab_fns.c`). The let-binding `source_binding` alias rule in
`src/compiler/elab_forms.c` now refuses to chain to a lifted-lambda helper:
`source_binding` means "the user typed a global function name as the init",
which a `__fn_N` helper is not. With the alias gone, `(f x)` flows through the
closure-dispatch protocol on the let binding (a function-pointer cast at the
call site) instead of a direct `__fn_N(...)` call whose C signature returns
the int64 carrier.

The same change fixed the `with-resource` case (the untraced root cause shared
the alias-rule lineage), so no separate investigation was needed.

Validation:

- `tests/fixtures/curried-fn-typed-param-application/` FAIL -> PASS.
- `tests/fixtures/with-resource-basic/` FAIL -> PASS.
- The constrained-generic regression #386 closes
  (`cgi-constrained-generic-dispatch`, `m5-constrained-poly-*`) stays green.
- `tests/fixtures/macro-quasiquote-unquote/expected.c` codegen snapshot
  regenerated: a captureless `add5 = (make-add 5)` now emits the
  function-pointer cast on the binding rather than a direct `__fn_881(...)`
  call -- the new behavior is correct and general (the old direct call only
  happened to compile for int-returning captureless lambdas).
- Full `bash tests/run.sh`: 1653 passed, 0 failed.

# PR #386 regressed two fixtures

## Summary

PR #386 added a let-bound-alias-to-global resolution rule in
`src/compiler/elab_call.c:1518-1535` (the "constrained-generic-as-value"
fix) plus bidirectional inference for lambda args in
`src/compiler/elab_call.c:2820+` and `src/compiler/elab_fns.c:3374+`.
The fix unblocks the rc=0 miscompile on generic-dict dispatch through a
let-bound alias, but breaks two adjacent paths:

1. **Captureless closure-returning lambdas in a let-binding.** A let
   like `(let [f (fn [a : int] : (fn [int] int) (fn [b] (+ a b)))] ((f 100) 5))`
   now emits a direct call to the lifted `__fn_N` global instead of
   going through the closure-dispatch protocol on `f`. The lifted
   `__fn_N`'s C signature returns `int64_t` (the carrier) rather than a
   function pointer, so the call result fails to type-check as
   `int64_t (*)(int64_t)`.
2. **The `with-resource` macro applied to a `(ref ...)` init.** The
   let-binding emitted by the macro types `r` as `size_t` instead of
   `void *`, causing `-Wint-conversion` on the `malloc` assignment and
   the later `free()` call.

Both reproduce on `tur build` against the in-tree fixtures with the
exact `flags` (or no flags) the test runner uses.

## Bisect

| Commit | curried-fn-typed-param-application | with-resource-basic |
|---|---|---|
| `335da8f3` (pre-#387/#386) | PASS | PASS |
| `fd3651b0` (#387 only) | PASS | PASS |
| `849a8711` (+#386)         | **FAIL** | **FAIL** |
| `4fbf5c2f` (current main)  | **FAIL** | **FAIL** |

PR #387 (carrier-source Result/Option bridge) is not implicated.

## Repro

### Curried fn

```turmeric
(defn main [] : int
  (let [f (fn [a : int] : (fn [int] int)
            (fn [b : int] : int (+ a b)))]
    (println ((f 100) 5)))   ;; => 105
  0)
```

Run: `tur build /tmp/repro.tur -o /tmp/r`. On `849a8711`+:

```
.../...input_tur.c:4494:27: error: incompatible integer to pointer
  conversion initializing 'int64_t (*)(int64_t)' (aka 'long long
  (*)(long long)') with an expression of type 'int64_t' (aka 'long long')
  [-Wint-conversion]
        int64_t (*__call_head_917)(int64_t) = __fn_914(INT64_C(100));
```

The passing emit (pre-#386) was:

```c
int64_t (*__call_head_917)(int64_t) =
    ((tur_fnptr_int64_t_int64_t_t (*)(int64_t))(intptr_t)f)(INT64_C(100));
```

The failing emit goes around `f` and calls `__fn_914` directly:

```c
int64_t (*__call_head_917)(int64_t) = __fn_914(INT64_C(100));
```

### with-resource

```turmeric
(defn main [] : int
  (with-resource [r (ref 42)]
    (drop! r)
    0))
```

Run: `tur -Xsubstructural build /tmp/repro.tur -o /tmp/r`. On `849a8711`+:

```
.../...input_tur.c:4419:20: error: incompatible pointer to integer
  conversion initializing 'size_t' (aka 'unsigned long') with an
  expression of type 'void *' [-Wint-conversion]
        size_t __t24 = malloc(sizeof(int64_t));
```

Hand-written equivalents (`(let [r (ref 42)] (do (drop! r) 0))` AND
`(let [r (ref 42)] (drop! r) 0)`) both emit `void * __t24 = malloc(...)`
and compile cleanly. **The bug only fires through the `with-resource`
macro expansion**, even though that macro expands to a plain let.

## Root cause (curried)

`src/compiler/elab_call.c:1530-1534` resolves a let-bound alias to its
`source_binding` when the source_binding is a global TY_FN. The intent
(per the PR comment) was let-bound aliases of user-named globals like
`(let [g count-it] (g box))`. The rule does NOT distinguish a
user-named global from a captureless-lambda-lifted helper.

In `src/compiler/elab_fns.c:3821-3831`, a captureless lambda gets
lifted to a global `__fn_N` and the lambda form is returned as
`EX_VAR(__fn_N)`. The let's init then has `kind == EX_VAR`, which
triggers `source_binding` propagation in
`src/compiler/elab_forms.c:790-808`. Subsequent `(f ...)` calls fire
the alias rule and emit `__fn_N(...)` directly, bypassing the
closure-dispatch-result cast that produces the correct function-pointer
type at the call site.

## Root cause (with-resource)

Not yet traced. The macro expands to `(let [r (ref 42)] (do ... ))`,
which compiles cleanly when written by hand. Suspect: the macro-emitted
`r` symbol carries metadata (perhaps a quoted `(quote r)` wrapper or
the macro hygiene path's interning) that interacts with #386's
elab_fns.c bidirectional-inference change. Worth bisecting between the
two #386 hunks (`elab_call.c` alias rule vs. `elab_fns.c` bidirectional)
to confirm which one is responsible.

## Proposed fix directions

1. **Gate the alias rule on user-named globals only.** In
   `elab_call.c:1530-1534`, additionally check that the source_binding
   is NOT a captureless-lambda-lifted helper. The cleanest signal is
   probably to check that source_binding's name doesn't begin with
   `__fn_` (the lifted-lambda naming convention) -- ugly but local. A
   better signal would be a Binding flag set by `elab_fns.c:3826` when
   it registers the lifted FN_DEF, e.g. `is_lifted_lambda`.
2. **Don't propagate `source_binding` for lifted-lambda EX_VARs.** In
   `elab_forms.c:790-808`, skip the source_binding set when init's
   binding is a captureless-lifted lambda. Same `is_lifted_lambda`
   flag needed.

Option 2 is more surgical (one site, narrowly scoped) and matches the
intent of source_binding ("user typed a global function name as the
init"). Either option still leaves the with-resource case to
investigate.

## Validation when fixed

- `tests/fixtures/curried-fn-typed-param-application/` flips
  FAIL->PASS.
- `tests/fixtures/with-resource-basic/` flips FAIL->PASS.
- `tests/fixtures/errors/constrained-generic-as-value-bakes-representative/`
  (the regression #386 closes) stays green.
- Full `bash tests/run.sh` shows no new failures.

## Cross-references

- PR #386: https://github.com/turmeric-lang/turmeric/pull/386
- `docs/archive/constrained-generic-as-value-bakes-representative.md`
  (the bug #386 fixes; archived in #386's docs hunk)
