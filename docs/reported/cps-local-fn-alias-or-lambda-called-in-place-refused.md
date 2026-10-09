# An effectful function called through a local name is refused

**Severity: medium.** This is a compile-time refusal of a correct program,
not a wrong answer: the build fails with "this effect operation has no
lowering here", and `tur --interpret` runs the program. It hits ordinary code:
a local helper lambda, a `letrec` loop that performs, or a short local name for
a named function. Found 2026-10-09 while fixing
[effect-row-lost-under-match-cast-letrec](../archive/effect-row-lost-under-match-cast-letrec.md).
It predates that fix; until then each shape also drew a false TUR-W0033.

## Repro

```turmeric
(defeffect Ask [] :int)
(defn via-adt [n : int] : int (+ n (perform (Ask))))

;; 1. a let-bound captureless lambda, called in place
(defn l1 [] : int (let [g (fn [n : int] : int (+ n (perform (Ask))))] (g 3)))
;; 2. a letrec loop that performs
(defn l2 [] : int
  (letrec [f (fn [n : int] : int (if (= n 0) (perform (Ask)) (f (- n 1))))]
    (f 3)))
;; 3. a let alias of a named function
(defn l3 [] : int (let [g via-adt] (g 2)))

(defn main [] : int
  (println (handle (l1) (Ask [] k) (resume k 10)))   ; 13
  (println (handle (l2) (Ask [] k) (resume k 10)))   ; 10
  (println (handle (l3) (Ask [] k) (resume k 10)))   ; 12
  0)
```

Each of the three is refused on its own. Calling `via-adt` directly, or
passing the lambda straight to a higher-order function, works.

## Root cause

`TUR_TRACE_EVICT=1` shows the lambda (`__fn_N`), or `via-adt`, as
`SIG-TAINT`. The `let` init is a VALUE use of that function. The E2
threadability tally (`fv_multi_tally`, `src/compiler/emit_cps_ir.c`) counts
a value use as escaping unless it is an argument to a threading parameter.
So the function is not threadable, it becomes a permanent fiber source, and
its effects taint everything that performs them.

A call through the local name is not resolved to the function it holds,
either. The CPS IR sees a call through a fn value with an empty declared row.

The `letrec` shape has two more gaps. A captureless member is lifted to a
global `__fn_N`: `elab_letrec` marks the binding global and records the
lambda as its `source_binding`. But the CPS IR has no `EX_LETREC` arm
(`--dump-cps` prints `unsupported form: EX_LETREC` for `l2`). And
`callee_fndef` does not follow `source_binding`, so the member's own
self-call `(f (- n 1))` is translated as a delegated direct call
(`<owning-op ?>`), not a cps->cps call. Nothing compiles that way today,
because the taint refuses the program first. But anything that lifts the
taint has to resolve this self-call too, or the recursion would run the
`perform` off the continuation.

The effect pass had the same blind spot and now follows the local name
(`src/passes/effect_check.c`, the EX_CALL arm). It uses `widen_fn_alias` for
an immutable alias of a global fn (shapes 1 and 3), `source_binding` for a
captureless `letrec` member (shape 2), and `closure_fn_binding` for a local
closure, whose row is all it reads.

## Fix directions

- Extend the alias machinery that already serves a fn PARAMETER
  (`cps_ir_let_fnparam_alias` / `PapInline.is_alias`, `src/passes/cps_ir.c`;
  [cps-let-alias-of-effectful-fn-param-refused](../archive/cps-let-alias-of-effectful-fn-param-refused.md))
  to a global fn and a lifted captureless lambda. The binding is dropped and
  each saturated call is rewritten to a direct call. Three things must follow
  the same predicate:
  - the use tally, so the init is no longer a value use;
  - the call-graph and effect crediting in `ensure_S`, so the rewritten call
    counts as a call to the target;
  - `safe_to_delegate`.
  Mind the pr-386 note on `Binding.is_lifted_lambda`: a lambda that RETURNS a
  closure is callable only through the closure protocol, so leave it out.
- `letrec`: translate a group whose members are all lifted globals as a
  `let` of aliases (the case above), with `callee_fndef` following
  `source_binding`. A CAPTURING member (a loop that reads an outer local) is
  a closure calling itself. That needs its env-taking `__cps` twin (the E2a
  registration a threadable capturing lambda already gets) and a self-call
  that threads `__kont`, which is a bigger piece.
