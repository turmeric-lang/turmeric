# Calling an effectful fn-value parameter through a `let` alias is refused

**RESOLVED 2026-10-08.** See "Fixed" at the end.

**Severity was: low.** A compile-time refusal, not a wrong answer, and calling the
parameter directly avoids it. Found 2026-10-07 while fixing
[cps-coloring-resolves-a-parameter-to-a-same-named-global](cps-coloring-resolves-a-parameter-to-a-same-named-global.md).

## Repro

```turmeric
(defeffect Ask [] :int)
(defn via-let [g : (fn [int] int) x : int] : int
  (let [f g]
    (f x)))
(defn main [] : int
  (println (handle (via-let (fn [x : int] : int (* x (perform (Ask)))) 3)
             (Ask [] k) (resume k 7)))
  0)
```

```
error: this effect operation has no lowering here: the enclosing function left
the CPS backend's supported subset ...
```

`(g x)` in place of the `let` prints `21`. `tur --interpret` runs both.

## What the trace says

`TUR_TRACE_EVICT=1` reports `[EVICT] SIG-REJECT eff=0 via-let` and
`[EVICT] SIG-TAINT eff=1 __fn_8` (the lambda). The E2 threading channel
(`param_thread_class`, `src/compiler/emit_cps_ir.c`) registers a parameter
only when every use is a call. Binding it in a `let` is a value use
(`val > 0` -> `PT_NONE`), so the parameter is never threaded, the lambda is
not threadable, and its effect taints the whole chain to the fiber path. That
path cannot lower the `perform`. Not investigated past that.

## Fix directions

- Treat a `let` that only renames a fn-value parameter (`(let [f g] ...)`
  with `f` used only as a callee) as calls through `g` in `ptc_walk`.
- Or have the elaborator substitute the alias away before CPS classification.

## Fixed (2026-10-08)

Fix direction 1, through a mechanism the translator already had.  The CPS IR
inlines a `let`-bound partial application whose every use is a saturated call
(`PapInline`, `pap_register_let`, `src/passes/cps_ir.c`): the binding is
dropped and each call is rewritten to the underlying one.  An immutable alias
of an immutable fn-valued parameter is the zero-capture case of that, so it
registers there too (`is_alias`), and a call through it is rewritten to the
same call made through the parameter (`pap_build_saturated_call` copies the
call node and names the parameter by `fn_binding`, as a direct `(g x)` does).

One predicate decides it for both sides, `cps_ir_let_fnparam_alias`:
binding `i` of the `let` renames an immutable parameter of the same
representation (a fat `is_poly_fn` parameter's signature is read from its
`poly_type`; its binding type is the `tur_poly_fn_t` carrier), and every use of
it -- in the body or in a later binding's init -- is a saturated call.  The
threading classifier (`ptc_walk`, `src/compiler/emit_cps_ir.c`) asks the same
question, so the alias's init is no longer a value-use and a call through the
alias counts as a call through the parameter; any other use of the alias
(passed as an argument, captured, stored) counts as a value-use of the
parameter, exactly as before.  `safe_to_delegate` refuses a call through a
registered alias, so a delegated subtree never names the dropped local.

Pinned by `tests/fixtures/cps-let-alias-of-effectful-fn-param`: the repro, a
non-tail call, two calls, calls under an `if`, a call from a later binding of
the same `let`, and pure callbacks through the same aliases; every line equals
`tur --interpret`.

Not covered, and unchanged: an alias of an alias (`(let [f g] (let [h f] (h
x)))`) is still refused -- only a parameter is followed -- and passing the
alias on to another higher-order function is refused, as passing the
parameter itself is.
