# An effectful callback through a multi-argument or untyped fn parameter is refused

**Severity: medium.** A compile-time refusal of a correct program; `tur
--interpret` runs it. Until 2026-10-07 these compiled and then aborted at run
time with `unhandled effect`. The fix for
[cps-effectful-fnval-escapes-through-unthreaded-param](../archive/cps-effectful-fnval-escapes-through-unthreaded-param.md)
turned that into this honest refusal. Annotating the parameter's row
(`(fn [int int] #fx{Ask} int)`) avoids it.

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
call is a direct call. The registry path that threads an effectful-row call
(`via_registry`) is chosen only when the parameter's declared row is
non-empty (`call_is_effectful_fnvalue`), so it never applies here.

## Fix directions

- Widen the `fn_cps` slot's ABI to N word-class arguments, the way
  `e2a_cast` already spells the registry's twins.
- Or let an empty-row call through a REGISTERED thread param take the
  registry path too, when the analysis has seen an effectful value flow in.
