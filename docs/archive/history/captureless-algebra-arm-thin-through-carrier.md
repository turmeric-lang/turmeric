---
title: Captureless algebra arm returns a thin fn pointer through a fat carrier -- non-uniform closure result segfaults
category: Closure ABI uniformity -- thin/fat result at a function/match that crosses the int64 carrier
severity: Low-Medium. Blocks an environment-passing "interpreter as one cata"
  whose algebra has a captureless variable-lookup arm (a `(VarF i) (fn [env]
  env)` node). Direct structural recursion is a working alternative (shipping in
  regex/tree's `re-matches?`). `tur check` is clean; the program compiles and
  segfaults at runtime.
status: RESOLVED 2026-06-22 -- captureless fn tail leaves are now boxed
  uniformly fat in `elab_defn` (src/compiler/elab_fns.c). Regression fixture
  `tests/fixtures/hkt-cata-captureless-fn-carrier-arm`. Carved out of
  `docs/reported/hkt-cata-function-carrier-recursive-segfault.md` (its residual
  "(a) Captureless algebra arm stays thin through the carrier (Bug B)").
---

## Resolution (2026-06-22)

`elab_defn` now runs a closure-ABI uniformity pass over the function body's
tail/return leaves when the declared result type is a function type (the
closure carrier). `fn_tail_fn_leaf_kinds` classifies the leaves: a *fat* leaf
is an `EX_CLOSURE` / `EX_FN_TO_FAT` (or any boxed `TY_FN` value); a *thin* leaf
is an `EX_VAR` of non-boxed `TY_FN` -- a captureless lifted lambda or a bare
defn reference. When the body is *mixed* (at least one fat and one thin leaf),
`elab_box_thin_fn_tail_leaves` wraps each thin leaf in `EX_FN_TO_FAT`, so every
return leaf lowers to a uniform fat box `{ thunk, ... }`. A carrier-crossing
caller that fat-dispatches the result then reads a valid layout on every arm
instead of jumping into a code address.

An all-thin (uniformly captureless) body is left untouched -- it is correctly
thin-callable and must not be force-boxed. The shim only fires on the
fat/thin mix, which is the non-uniform shape that segfaults.

Walking the emitted C for the repro confirms the `VarF` arm now emits
`{ __tur_fatshim1, __fn_1059 }` rather than the bare `__t49 = __fn_1059;`.

---

# Captureless algebra arm stays thin through a fat carrier

## One-line summary

A closure-returning function (or a `match` over an HKT algebra) whose result is
the generic carrier `B = (fn ...)` boxes its *capturing* arms as fat closure
boxes `{ thunk, env... }` but emits a *captureless* arm as a bare thin function
pointer. The function's `(fn ...)` result is therefore not uniformly fat; when
the carrier-crossing caller fat-dispatches the result, the thin-pointer arm is
jumped into as if it were a fat box -> SIGSEGV.

## Minimal repro (segfaults)

```turmeric
(load "stdlib/typeclass-functor.tur")

(defdata ExprF :copy [a] (LitF :int) (VarF :int) (AddF a a))
(defdata Expr  :copy (Roll (ExprF Expr)))

(definstance Functor [ExprF]
  (fmap [c g]
    (match c
      (LitF n)   (LitF n)
      (VarF i)   (VarF i)
      (AddF x y) (AddF (g x) (g y)))))

(defn unroll-e [e : Expr] : (ExprF Expr)
  (match e (Roll l) (:: l (ExprF Expr))))

(defn cata [B] [alg : (fn [(ExprF B)] B)  e : Expr] : B
  (alg (:: (fmap (unroll-e e) (fn [c : Expr] : B (cata alg c))) (ExprF B))))

;; carrier B = (fn [int] int): an environment-passing interpreter.
(defn eval-alg [l : (ExprF (fn [int] int))] : (fn [int] int)
  (match l
    (LitF n)   (fn [env : int] : int n)       ;; captures n   -> fat box
    (VarF i)   (fn [env : int] : int env)     ;; captureless  -> THIN pointer
    (AddF f g) (fn [env : int] : int (+ (f env) (g env)))))  ;; captures f g -> fat box

(defn var [i : int] : Expr (Roll (VarF i)))

(defn main [] : int
  (do (println ((cata eval-alg (var 0)) 100))   ;; EXPECT 100; ACTUAL: SIGSEGV
      0))
```

`tur check` is clean; `tur run` segfaults.

## Root cause (confirmed in emitted C)

`eval-alg` lowers to a `void *`-returning `match`. Its capturing arms heap-box:

```c
struct __env_1054 *__t50 = malloc(sizeof(struct __env_1054));   /* LitF: fat */
__t50->__fn = (tur_thunk_int64_t_int64_t_t)__fn_1052;
...
struct __env_1066 *__t52 = malloc(sizeof(struct __env_1066));   /* AddF: fat */
__t52->__fn = (tur_thunk_int64_t_int64_t_t)__fn_1064;
```

but the captureless `VarF` arm returns the raw thin function pointer directly:

```c
__t49 = __fn_1059;        /* VarF: a bare thin fn pointer, no fat box */
```

So `eval-alg`'s `(fn ...)` result is fat on two arms and thin on one. The
fix-1/fix-3 machinery from #489 / hkt-cata-function-carrier-recursive
fat-dispatches the cata result (and the AddF inner `(f env)`/`(g env)`), so when
the value flowing through is the thin `VarF` arm, the dispatch reads a "thunk"
slot out of a code pointer and jumps to garbage.

## Fix direction

When a closure-returning function / `match` yields a fat box in any arm (or its
`(fn ...)` result feeds a carrier-typed algebra), auto-shim the captureless arms
to fat via `EX_FN_TO_FAT` at the tail/return leaves -- i.e. apply the
`result_fat` machinery at the carrier crossing rather than only on an explicit
`^fat` annotation. This is a closure-ABI uniformity change: a function whose
result type is a carrier-bound `(fn ...)` must return a *uniformly fat* value
across all of its return leaves. Affects environment-passing interpreters /
variable-lookup nodes specifically.

## Relationship to neighbouring reports

- Carved out of `hkt-cata-function-carrier-recursive-segfault.md` (residual a).
  That report's residual (b) -- the mixed fn/value carrier spec-name collision
  -- is now fixed (`tests/fixtures/hkt-cata-mixed-fn-value-carrier`,
  emit_module.c env-struct `__h<n>` disambiguator).
- Same failure mode as the matcher-as-cata Edge in
  `hkt-matcher-cata-fnarg-on-toplevel-defn-and-env-struct-collision.md`: once
  that report's Edge 2 (env-struct redefinition) was fixed by the same
  disambiguator, the regex matcher's `(LitF n) (fn [s] true)` captureless arm
  reaches exactly this thin-through-carrier segfault.
- Lineage: the closure thin/fat boundary fixes #489 and
  `docs/archive/history/arrow-thin-call-segfaults-capturing-closures.md`,
  `docs/archive/history/captureless-lambda-abi-plan.md`.
