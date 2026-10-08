# An effectful callback through a multi-argument or untyped fn parameter is refused

**Narrowed 2026-10-08: shapes 1, 3 and 4 are fixed** -- a callback of up to
eight arguments (word integers, `cstr` or `ptr<void>`), a capturing closure,
and a `bool` or unit result all thread now (see "Fixed 2026-10-08" at the
end).  **What is left:** shape 2, an untyped `^fat` parameter; and a callback
whose argument or result has neither a word's C spelling nor its register
class -- a `float` argument or result, a narrow integer or `bool` argument --
which is still refused, honestly, at compile time:

```turmeric
(defn appf [h : (fn [int] float) x : int] : float (h x))       ;; refused
(defn appb [h : (fn [bool] int) b : bool] : int (h b))         ;; refused
(defn app [^fat h x : int] : int (h x))                        ;; refused
```

A capturing closure is narrower still: word-integer arguments and an `int` or
unit result only (its dispatcher's fallback calls the env box's slot 0, which is
the lifted entry at those types; a `bool` closure's slot 0 is a widen wrapper).

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

## Fixed 2026-10-08: multi-argument, capturing, and `bool` / unit callbacks

Fix direction 1, without widening the struct.  `tur_poly_fn_t.fn_cps` stays
declared at the one-argument ABI; a slot of any other arity is stored through
a cast and called back at its own type, so every indirect call is made at the
callee's real type (`-fsanitize=function`-clean).

- **One shape question.**  `cps_ir_fncps_sig_ok` (`src/passes/cps_ir.c`) says
  whether a fn fits the slot: up to `CPS_FNCPS_MAX_ARGS` (8) arguments, each an
  `int`/`int64`, a `cstr` or a `ptr<void>` (each crosses the slot as its word,
  and the twin converts it back to the callee's own C type,
  `cps_ir_fncps_arg_ctype`), and an `int`/`int64`, `bool` or unit result.  The poly-wrap that
  FILLS the slot (`emit_expr.c`, EX_POLY_WRAP) and the analysis that counts a
  value as threaded only when the slot will be filled (`arg_fat_has_fn_cps`,
  `emit_cps_ir.c`) both ask it, where each used to hard-code "one int argument,
  int result" separately.  The call side (`fncps_param_call_ok`) takes N word
  arguments; the IR builder atomizes them all (`fncps_atomize_args`).
- **Named fns and captureless lambdas.**  `ensure_poly_wrap_cps_thunk`
  (`emit_module.c`) emits the `__poly_N__cps` twin at the fn's arity and result
  (`void` / `bool` forward declarations; a unit result delivers 0).  The
  one-argument `int` twin is byte-identical to before, so no snapshot moved.
- **Capturing closures.**  The closure arm of EX_POLY_WRAP appends a
  `__tur_fncps_env<N>` dispatcher (`ensure_fncps_env_dispatch`) for an
  effectful lambda: it keys the direct->CPS registry on the env box's slot 0 --
  the lifted entry, which a threadable capturing lambda registers against its
  env-taking `__cps` twin -- so it does not depend on knowing which lambda built
  the box; a miss makes the call an empty slot would have made.  Which lambda
  a poly-wrap packs is answered once, `emit_poly_wrap_fncps_closure`, for both
  the fill and `arg_fat_has_fn_cps`.
- **Emission.**  `fncps_slot_call` and `fncps_direct_call` (`emit_cps_ir.c`)
  spell the threaded and the direct call at any arity, in tail position and in
  the heap join.

Pinned by `tests/fixtures/cps-effectful-callback-pointer-args` (`cstr` and
`ptr<void>` arguments, named and lambda, tail and non-tail),
`tests/fixtures/cps-effectful-callback-multi-arg` (named fns and
lambdas, arities 0-3, `bool` and unit results, tail and non-tail, pure values
through the same parameters), `cps-effectful-capturing-closure-callback`
(literals and a let-bound closure, arities 0-2, `int` and unit results) and
`cps-effectful-closure-through-unannotated-param` (the refusal fixture this
report's earlier state pinned in `errors/`, now a passing fixture printing
16).  Every line equals `tur --interpret`, and both fixtures' programs also run
clean under clang's `-fsanitize=function`.

Found on the way, pre-existing and filed separately:
[cps-effectful-closure-returned-through-empty-row-aborts](cps-effectful-closure-returned-through-empty-row-aborts.md)
-- an effectful closure returned by a call, `(app1 (adder 3) 1)`, still
compiles and aborts with `unhandled effect`, as it did before this change.
