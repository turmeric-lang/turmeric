# `tur --interpret`: an effect performed through a stdlib higher-order function draws a false TUR-W0033

**Severity:** low (diagnostic noise; the program runs correctly). Under
`tur --interpret`, a `handle` whose body passes an effectful lambda to a
**stdlib** higher-order function warns that its clause is unreachable. The
lambda's `perform` reaches the handler, and the program prints the right
answer. `tur check` and `tur run` do not warn. A user-defined function of the
same shape does not draw the warning either. Filed 2026-10-18 while fixing
`region-bracket-blocks-effects` (`with-region` / `bt-scope` thunks show it).

## Repro

```turmeric
(defeffect Bail [code : int] : int)

(defn fwd [A] [^fat body : (fn [] A)] : A (body))   ; same shape as with-region

(defn main [] : int
  (println (handle (match (option-map (some 1) (fn [x : int] : int (perform (Bail x))))
                     (Some v) v
                     None 0)
             (Bail [c] k) (+ c 100)))
  (println (handle (fwd (fn [] : int (+ 5 (perform (Bail 7))))) (Bail [c] k) c))
  0)
```

```
$ tur --interpret repro.tur
repro.tur:8:27: warning [TUR-W0033]: handler clause for 'Bail' is unreachable: the body does not perform 'Bail'
101
7
$ tur check repro.tur          # no warning
$ tur run repro.tur            # no warning
101
7
```

Only the `option-map` handle warns. `with-region` and `bt-scope` (stdlib) draw
it the same way. `fwd`, defined in the program, does not. Measured on the
`claude/focused-hypatia-k1mkwm` build
([turmeric-lang/turmeric#1273](https://github.com/turmeric-lang/turmeric/pull/1273)).
The code paths below are the same on `main`.

## Root cause

The interpreter runs the effect checker once per turn, over that turn's forms
only (`effect_check_pass(eval_arena, prog, eff_env)`, `src/turi/eval.c:17174`).
The stdlib was elaborated in the preload turn, so its definitions are not in
this turn's `FnIndex`.

In `collect_effects_in_expr` the callee is `fn_index_lookup(idx,
e->as.call_.fn_binding)` (`src/passes/effect_check.c:362`), which is NULL for
`option-map`. The rule that charges a fn-value argument's row to the call,
because the callee may invoke it, is guarded on that lookup:
`if (callee && callee->params && ...)` (`effect_check.c:469`). So the lambda's
`Bail` is never charged to the call. The handle's body reads as performing
nothing, and `check_unreachable_handlers_in_expr` warns.

`tur check` / `tur run` see the stdlib in the same program, so the callee is
indexed and the argument's row is charged. A user function is in the same
turn, so it is indexed too.

## Fix directions

1. **Index the callees an item reaches but this turn does not define.** Walk
   the turn's bodies, and add `fn_binding->source_fn_def` for any callee not
   already in the index. Infer their rows in the same fixpoint, but do not
   validate them (they were checked on their own turn). PR #1273 adds exactly
   this walk for separately compiled modules (`foreign_scan` in
   `effect_check.c`, gated on separate compilation). Running it for the
   interpreter's turns too, with the cross-module refusal still gated, would
   cover this.
2. **Narrower:** when the lookup fails but the binding has a `source_fn_def`,
   use it for the argument-row charge only (the parameter list is all that
   rule reads).

To pin it, the repro needs a fixture that fails on the warning under the
interpreter. `unexpected.stderr` (substrings that must not appear) is read
only by `tests/run.sh`, which runs the compiled path, where nothing warns. So
either teach `tests/run-turi.sh` to honour `unexpected.stderr` the way it
already honours `expected.stderr`, or give the fixture an `expected.stderr`
that holds exactly what the interpreter should print (nothing).

## Resolved 2026-10-21: fix direction 2

`collect_effects_in_expr` (`src/passes/effect_check.c`) charges a fn-value
argument's row through the callee's parameter list; when the index lookup
fails but the call's binding carries a `source_fn_def` (a stdlib function
elaborated on the preload turn), that definition's parameter list is used
for the charge alone. The callee's own inferred row is still read only from
the index, so nothing is re-validated across turns.

`tests/run-turi.sh` now honours `unexpected.stderr` the way `run.sh` does,
and `tests/fixtures/turi-hof-effectful-callback-no-w0033` (the repro plus a
`with-region` thunk) pins that no TUR-W0033 is printed on either engine.
