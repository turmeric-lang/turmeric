# U5 regex matcher-as-cata: two follow-up codegen edges past #499

**Found by:** turmeric-spices Track C (U5 regex matcher refactor attempt
after #499 landed)
**Verified on:** turmeric main @ `affeb7a6` (post #499), Debug ASan build
**Severity:** Low-Medium. Blocks the spice-side matcher-as-cata landing
in `turmeric-spices/spices/regex/src/regex/tree.tur`. `re-matches?` remains
direct structural recursion. Value folds (`re-size`, `re-nullable?`,
`re->str`) and the matcher itself are unaffected.

**Status: RESOLVED 2026-06-22.** Edge 2 was fixed earlier (env-struct
disambiguator, see below). Edge 1 -- the `letrec`-self capture across a nested
closure -- is now fixed too (see the Edge 1 resolution note). The remaining
runtime residual (a captureless algebra arm crossing the int64 carrier *thin*)
is tracked separately in
`docs/reported/captureless-algebra-arm-thin-through-carrier.md`; it is the
same fat-vs-thin carrier-dispatch cluster as Bug B/Bug C of
`hkt-cata-function-carrier-recursive-segfault`, not a `letrec` capture bug.

## Context

#499 fixed the cata-application-site case where the carrier `B` has a
function-typed argument (`B = (fn [(fn [cstr] bool) cstr] bool)`). With
that fix, a minimal `cata` whose carrier takes a function-typed argument
runs end to end.

Attempting to use the now-fixed carrier shape to express the regex
backtracking matcher as `(re-cata match-alg e)` -- i.e. swap the existing
recursive `m` for an F-algebra over the matcher closure carrier -- the
codegen hits two further edges that #499 did not cover.

## Edge 1 -- `letrec`-self inside a returned nested closure in a match arm

The natural `StarF` arm wants a local `letrec` whose closure references
itself and is returned out of the algebra:

```turmeric
(StarF x) (letrec [self (fn [k : (fn [cstr] bool) s : cstr] : bool
                          (or (k s)
                              (x (fn [s2 : cstr] : bool
                                   (if (cstr-same? s2 s) false (self k s2)))
                                 s)))]
            self)
```

Emits a body that references `self_1149` from inside a nested closure
without including it in the closure's captures:

```c
__t25 = regex__tree____fn_1158((void *)(intptr_t)(self_1149),
                                (int64_t)(intptr_t)(__env___env_1155->k),
                                s2);
// error: use of undeclared identifier 'self_1149'
```

The outer letrec-bound `self` is reachable from the inner `(fn [s2] ...)`
closure body, but `collect_free_vars` is not picking it up across that
inner-closure boundary.

### Edge 1 -- FIXED 2026-06-22

Root cause: `collect_free_vars`' `EX_CALL` free-var gate
(`src/compiler/elab_core.c`) deliberately excluded *every* `letrec`/named-let
self binding -- the `is_param`/`is_match_binding`-only accept list from #499 --
so that a direct self/mutual recursive call stayed with the recursion machinery
(captureless-global lifting, or the S5 env-ptr self-call). That exclusion was
too broad: a `self` reached only from a *nested* closure is a genuine free
variable of that inner closure and must be captured by env.

Fix (mirrors #494/#496-class capture fixes, plus an S5-style emit rule):

- `src/compiler/expr.h`: new `Binding.is_letrec_binding` flag, set on every
  `letrec`/named-let group member in `elab_letrec` Pass A
  (`src/compiler/elab_forms.c`).
- `src/compiler/elab_core.c`: the `EX_CALL` gate now also accepts a `TY_FN`
  call head that `is_letrec_binding`. A new **self-exclude set** (the active
  letrec group, threaded through `Elab.letrec_self_group` and snapshotted +
  cleared at `elab_fn` entry) keeps a *direct* self/mutual call in the init's
  own top-level body out of the capture set, while a reference from a nested
  closure -- which sees an empty group -- is captured. The `EX_CLOSURE`
  transitive fold honours the same exclude set so the closure the letrec binds
  `self` to does not capture *itself* (its value is its own env pointer).
- `src/compiler/emit_expr.c`: when a nested closure's captured binding is the
  very closure currently being emitted (`captured->closure_fn_binding ==
  ctx->closure->fn->binding`), store its own env pointer in the inner env slot
  (the S5 self-*call* rule applied to a self-*capture*). A letrec member that
  turned out captureless (lifted as a directly-callable global) is stripped
  from the env struct / build / `capture_env_access`
  (`src/compiler/emit_core.c`) and reached by its C symbol instead -- globals
  are never legitimately captured elsewhere, so this is otherwise a no-op.

Regression fixture: `tests/fixtures/letrec-self-in-nested-closure` (the
real-closure-capture `walk` shape and the captureless-global `countdown`
shape). Full `bash tests/run.sh`: 1762 passed, 0 failed.

## Edge 2 -- env-struct redefinition collision when the letrec is hoisted

Workaround attempt: hoist the StarF loop to a top-level `defn` that takes
the folded child matcher `mx` explicitly:

```turmeric
(defn star-loop [mx : (fn [(fn [cstr] bool) cstr] bool)
                 k  : (fn [cstr] bool)
                 s  : cstr] : bool
  (or (k s)
      (mx (fn [s2 : cstr] : bool
            (if (cstr-same? s2 s) false (star-loop mx k s2)))
          s)))
```

`tur check` passes; `cc` reports two distinct problems on the emitted C:

1. **Wrong dispatch signature for the fn-typed parameter `mx`** at its
   call site -- the args list collapses to `(size_t, const char *)`:

   ```c
   __t36 = ((bool (*)(size_t, const char *))(intptr_t)mx)(__t38, s);
   // error: incompatible pointer to integer conversion passing 'void *'
   //        to parameter of type 'size_t'
   ```

   `mx` is `(fn [(fn [cstr] bool) cstr] bool)`; the dispatch should be
   `bool (*)(void* env, int64_t, int64_t)`. The first parameter (which is
   itself a function) is being demoted, and the second parameter (`cstr`)
   is being narrowed too.

2. **Spec collision on `__env_1068__spec__bool`** -- FIXED 2026-06-22.
   Two distinct definitions were emitted in the same TU, one with a fat
   `tur_thunk_bool_int64_t_t __fn` field, one with `int64_t __fn`:

   ```c
   // first definition
   struct __env_1068__spec__bool { tur_thunk_bool_int64_t_t __fn; int64_t alg; };
   // ...
   // second definition (redefinition error)
   struct __env_1068__spec__bool { int64_t __fn; int64_t alg; };
   ```

   The same closure environment was specialized twice with different ABIs for
   the embedded `__fn` slot (thin vs. fat) under one C name -- the carrier `B`
   lowers to the int64 carrier for both, so the env-struct name collapsed.

   **Fixed** by the same disambiguator as hkt-cata residual (b): the env-struct
   override site (`src/compiler/emit_module.c`) now appends a deterministic
   `__h<n>` suffix when the base env name collides with another spec's
   `env_name_override`, mirroring `emit_abi_clone_name`'s Gap H. With Edge 2
   resolved, the matcher-as-cata shape now *compiles*; at runtime it reaches a
   separate, still-open residual -- the captureless arm `(LitF n) (fn [s] true)`
   crosses the carrier as a thin pointer and segfaults. That is tracked by
   `docs/reported/captureless-algebra-arm-thin-through-carrier.md`.

## Minimal repro

Apply the matcher-as-cata patch to `spices/regex/src/regex/tree.tur`
(replace the existing direct-recursion `m` with an algebra over a function
carrier as sketched in
`docs/upcoming/spices/u5-regex-matcher-cata-blocker-2026-06-22.md`). Run
`tur run spices/regex/tests/tree_test.tur`. Both edges fire in the same
emit.

## Fix direction

- **Edge 1:** FIXED 2026-06-22 -- `collect_free_vars` now captures a
  `letrec`-bound name referenced from a nested closure, gated by a new
  `is_letrec_binding` flag plus a self-exclude set so direct self/mutual
  recursion still goes to the recursion machinery; emit stores the enclosing
  closure's own env pointer in the inner env slot (and strips a captureless
  member that became a global). See the "Edge 1 -- FIXED" note above.
- **Edge 2:** FIXED 2026-06-22 -- the env-struct name now carries a `__h<n>`
  disambiguator when two specs collapse to the same base name (see above). The
  redefinition is gone; what remains after it is the captureless-thin-arm
  segfault, tracked separately.

Both edges are now addressed. The matcher-as-cata shape *compiles and runs*
for the `letrec`-self-through-nested-closure case; the residual that keeps the
full spice-side `(re-cata match-alg e)` from landing is the captureless-arm
thin-carrier segfault tracked in
`docs/reported/captureless-algebra-arm-thin-through-carrier.md`. Until that
lands, the regex matcher stays direct structural recursion (currently 14/14
green at `turmeric-spices/spices/regex/tests/tree_test.tur`).
