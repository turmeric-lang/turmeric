# An effectful callback through a multi-argument or untyped fn parameter is refused

**Severity: medium.** A compile-time refusal of a correct program; `tur
--interpret` runs it. Until 2026-10-07 these compiled and then aborted at run
time with `unhandled effect` (on `main` too). The fix for
[cps-effectful-fnval-escapes-through-unthreaded-param](../archive/cps-effectful-fnval-escapes-through-unthreaded-param.md)
turned that into this honest refusal. Annotating the parameter's row
(`(fn [int int] #fx{Ask} int)`) avoids it.

Four shapes, all through an un-annotated (empty-row) fn parameter:

- a callback of two or more arguments (the repro below);
- an untyped `^fat` parameter, `(defn app [^fat h x : int] : int (h x))`;
- a CAPTURING effectful closure, `(let [m 5] (app (fn [x : int] : int (+ m
  (perform (Ask)))) 1))`;
- an effectful callback whose result is not `int`, e.g.
  `(fn [int] void)` with `(fn [x : int] : void (println (+ x (perform
  (Ask)))))`.

## Repro

```turmeric
(defeffect Ask [] :int)
(defn app2 [h : (fn [int int] int) x : int] : int (h x x))
(defn main [] : int
  (println (handle (app2 (fn [x : int y : int] : int (+ x y (perform (Ask)))) 1)
             (Ask [] k) (resume k 10)))
  0)
```

```
error: this effect operation has no lowering here: the enclosing function left
the CPS backend's supported subset ...
```

Expected `12`. The same happens with an untyped `^fat` parameter,
`(defn app [^fat h x : int] : int (h x))`, given an effectful lambda.

## Root cause

A call through an un-annotated (empty-row) fn parameter carries the caller's
continuation only through the fat value's `fn_cps` slot, and that slot's ABI
is one int-class argument (`fncps_param_call_ok`, `src/passes/cps_ir.c`:
"Restricted to the single-int-arg `tur_poly_fn_t.fn_cps` ABI"). Any other
call is a direct call. And the slot is FILLED (EX_POLY_WRAP, `emit_expr.c`)
only for a global fn of one `int` argument and an `int` result, so a
capturing closure or a non-`int` result reaches the call with an empty slot
and takes the direct `.fn` path from a fresh root. The registry path that threads an effectful-row call
(`via_registry`) is chosen only when the parameter's declared row is
non-empty (`call_is_effectful_fnvalue`), so it never applies here.

## Fix directions

- Widen the `fn_cps` slot's ABI to N word-class arguments and any result
  kind, the way `e2a_cast` already spells the registry's twins, and fill it
  for a capturing closure (an env-taking twin, as the registry's fat dispatch
  already calls).
- Or let an empty-row call through a REGISTERED thread param take the
  registry path too, when the analysis has seen an effectful value flow in.
