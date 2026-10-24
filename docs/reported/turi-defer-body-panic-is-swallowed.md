# `tur --interpret`: a panic raised in a deferred body is swallowed

**Severity: medium** (engine divergence: a panic the compiled program raises
is lost under the interpreter, and the program goes on). Interpreter only.
Found 2026-10-24 writing `defer-abortive-handler-no-inline-c`, the
inline-C-free twin of `defer-cps-abortive-handler`, whose cases 16 and 17 it
leaves out for this.

## Repro

```turmeric
(defn boom [] : int (panic "boom"))
(defn plain [] : int
  (defer (boom))
  5)
(defn main [] : int
  (let [r (catch-unwind (fn [] : int (plain)))]
    (println (if (err? r) "caught" "no panic")))
  0)
```

| | `tur run` / `tur jit` | `tur --interpret` |
| --- | --- | --- |
| a defer that panics at its scope's normal exit | `caught` | `no panic` |
| the same under an abortive handler case (`defer-cps-abortive-handler` 16) | `16: caught` | `16: no panic` |
| three defers, the middle one panics (case 17) | the other two run, then `17: caught` | the other two run, `17: no panic` |

Without any catch boundary both engines die with the panic
(`turi_runtime_panic` exits at once there); it is the signal a work-stack
boundary would take that is lost.

## Root cause

`fire_defers_to_mark` and `fire_defers_to_mark_by_scope` (`src/turi/eval.c`)
clear the signals before each deferred body and restore the saved ones at the
end -- `env->panicking = saved_panicking` -- so a panic a body raised against
a work-stack catch boundary (`turi_runtime_panic` sets the signal and
returns) is overwritten. Their header says so: "Errors inside defers are
silently discarded". A setjmp boundary (`catch-panic-of`, a black-boxed
`catch-unwind`) is longjmp'd to from inside the fire, so that shape does
reach a catch, without running the remaining defers.

## Fix directions

1. In both fire helpers, remember a panic a body raised (`env->panicking`
   set after it, not before the fire), keep firing the rest -- as a compiled
   frame runs every closure of its chain -- with `g_firing_panic_defer` set
   so a second one is the double panic it is compiled, and leave the signal
   raised at the end. Then add cases 16 and 17 back to
   `defer-abortive-handler-no-inline-c`.
2. Check the callers that fire on a NORMAL exit (`DK_CALL_RET`,
   `DK_LET_BODY`, a defer-scope `DK_DO_SEQ`) carry a raised signal on as an
   early exit from there.
