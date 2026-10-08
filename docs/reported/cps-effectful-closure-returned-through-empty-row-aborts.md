# An effectful closure returned through an empty-row fn type compiles, then aborts with `unhandled effect`

**Severity: medium.** A miscompile class, not a refusal: a correct program
builds and then aborts at run time; `tur --interpret` runs it. It needs an
effectful closure to cross a function boundary as a VALUE whose declared type
has an empty effect row, which the default (lenient) effect checker accepts.
Found 2026-10-08 while widening the `fn_cps` slot for
[cps-effectful-callback-through-multi-arg-or-untyped-param](cps-effectful-callback-through-multi-arg-or-untyped-param.md);
it reproduces identically on the compiler before that change.

## Repro

```turmeric
(defeffect Ask [] :int)
(defn ask [] : int (perform (Ask)))
(defn app1 [h : (fn [int] int) x : int] : int (h x))
(defn adder [m : int] : (fn [int] int) (fn [x : int] : int (+ m x (ask))))
(defn main [] : int
  (println (handle (app1 (adder 3) 1) (Ask [] k) (resume k 10)))
  0)
```

```
$ tur run repro.tur
tur: unhandled effect (tag 2)
Aborted
$ tur --interpret repro.tur
14
```

## Root cause

The threading analysis (`emit_cps_ir.c`) follows an effectful fn-value from
where it is WRITTEN to the parameter it is passed to: a named fn, a lambda
literal, or a let / `__borrowc` temp of one (`arg_fnval_binding`).  Only then
is the lambda put in the threadable set (registered with the direct->CPS
registry), and only then does `fnval_withdraw_walk` check that every call
through the receiving parameter threads.

`(adder 3)` is none of those: the argument is a call, so `arg_fnval_binding`
answers NULL and the lambda inside `adder` is never considered.  It is not
threadable, so it is not registered; the poly-wrap at the call site resolves no
lambda behind the `__borrowc` temp (its `hoist_closure_fn_binding` is NULL, as
it holds a call result), so the fat value carries no `fn_cps` slot; and
`app1`'s call through `h` takes the direct `.fn` path from a fresh root, where
the `perform` finds no handler.  Nothing refuses it, because the type the
value travels at -- `(fn [int] int)`, `adder`'s declared result -- says it
performs nothing.

`--strict-effects` does not catch it either: it warns (`TUR-W0030`) that the
anonymous function in `adder` performs `{Ask}` with no row annotation -- and,
wrongly, that `adder` itself does, although `adder` only returns the closure --
but nothing reports the effectful value returned at the empty-row type, and the
build succeeds.

## Fix directions

1. **Refuse it.**  An effectful lambda that escapes as a value through anything
   but a threadable argument position -- returned, stored in a container --
   taints the functions that call through a value of its type, as an
   un-threaded callee does today.  Coarse, but it turns the abort into an
   honest TUR refusal.
2. **Thread it.**  Make the returned lambda threadable (registered) and let the
   closure arm of EX_POLY_WRAP give a call result's fat value the env-box
   dispatcher it already gives a literal (`emit_poly_wrap_fncps_closure`,
   `ensure_fncps_env_dispatch`): it keys on the box's slot 0 at run time, so it
   does not need to know which lambda built the box.  What is missing is the
   registration and the analysis seeing the value at all.
3. **Close the checker hole.**  The lenient checker could infer the closure's
   row into `adder`'s result type, so the parameter's empty row is a mismatch
   it can report.
