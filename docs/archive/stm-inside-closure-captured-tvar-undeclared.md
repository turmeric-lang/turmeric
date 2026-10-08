# `stm` inside a lambda that captures a TVar emits C that does not compile

**Severity: medium.** Filed 2026-09-30, found executing security-audit-plan
WP5 while probing whether `tvar/write` carries a region note. Pre-existing on
`main` @ 5fd23a65 (reproduced with the WP5 changes stashed).

**Status: RESOLVED 2026-10-01.** Neither lift dropped the capture: the lambda
was lifted as captureless (`static int64_t __fn_6(void)`) because
`collect_free_vars` (`src/compiler/elab_core.c`) never saw `tv` at all. Its
main walk enumerated node kinds by hand and its `default:` arm was `break`, so
every capture under a kind it had no arm for was silently dropped -- `stm`,
`atomically`, every `tvar/*` form, `check`, `or-else`, `select`, a dynamic
`binding`, `with-handler` and `compose-handlers`. The `default:` arms of both
the pre-pass (let-bound locals) and the main walk now fall back to
`cps_visit_children`, the one enumeration over every kind with an evaluated
operand, and the pre-pass registers the names a handler value's cases and a
`select` clause bind. Measured before the fix and invalid C the same way: a
dynamic `binding` whose override is a capture (`'k_6' undeclared`) and a
`with-handler` whose case body captures (`'k_5' undeclared`).

Fixed alongside: the walk's two work stacks were a fixed 256 entries with no
bound check, so a lambda whose body is a `do` of 300 forms was an ASan
heap-buffer-overflow inside the compiler (`elab_core.c:488`) -- memory
corruption in a Release build. They grow now.

Found while pinning the `with-handler` shape: a `(with-handler (handler ...)
body)` was refused where the `handle` it means lowers (as a builtin's operand,
or in a capturing lambda). It now elaborates to that `handle`
(`elab_with_handler`). And the interpreter read `HandleExpr.shallow`
uninitialized on every resume through a handler value (UBSan invalid-bool
load, `eval.c`); it is zeroed now.

Pinned by `tests/fixtures/closure-captures-under-every-form` (a transaction in
a lambda, the same in `with-region`, `tvar/modify` and `tvar/cas` over a
capture, a dynamic `binding`, the 300-form body) and
`tests/fixtures/with-handler-literal-lowers-as-handle`. The STM shapes cannot
share a `main` with a `handle`: the CPS backend does not lower `tvar/new`, so a
`main` holding both is evicted (`TUR_TRACE_EVICT` now names the STM kinds
instead of printing `EX_#73`).

## Repro

```turmeric
(defn run [f : (fn [] int)] : int (f))
(defn main [] : int
  (let [tv (tvar/new 0)]
    (println (run (fn [] : int (do (atomically (stm (tvar/write tv 5))) 1)))))
  0)
```

`tur build` fails in cc:

```
error: 'tv_5' undeclared (first use in this function)
    tur_tvar_write(tur_stm_current_tx(), (TVar*)tv_5, (void*)(intptr_t)...);
```

`tur check` passes. The same `(atomically (stm (tvar/write tv 5)))` at the top
of `main`'s body -- not inside a lambda -- builds and runs
(`tests/fixtures/stm-cas`).

## What it looks like

The `stm` body is lifted into its own C function, and `tvar/write`'s inline-C
body is spliced in with `tv` spelled as the enclosing lambda's binding name
(`tv_5`) rather than read out of the transaction thunk's env. So the capture
is lost at one of the two lifts -- the lambda's, or `stm`'s -- when they nest.
Not bisected further.

## Why it matters beyond STM

Every region bracket is a lambda (`(with-region (fn [] ...))`), so a
transaction over a TVar created outside the bracket cannot be written inside
one at all. That is also why WP5 could not probe `tvar/write`'s store hook with
a runtime fixture: the value reaches it through an explicit `(:: node ptr)`,
which is noted at the ascription, but the bracket around it does not compile.

## Fix directions

- Find which lift drops the capture: dump the elaborated `stm` form under a
  lambda and check whether `tv` appears in the thunk's free-variable set.
- Pin with a fixture that runs a transaction inside a lambda and inside a
  `with-region`.
