# Calling an effectful fn-value parameter through a `let` alias is refused

**Severity: low.** A compile-time refusal, not a wrong answer, and calling the
parameter directly avoids it. Found 2026-10-07 while fixing
[cps-coloring-resolves-a-parameter-to-a-same-named-global](../archive/cps-coloring-resolves-a-parameter-to-a-same-named-global.md).

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
