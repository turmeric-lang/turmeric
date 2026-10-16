# Compiled code evaluates a later argument's call before an earlier argument

**Severity:** medium-high (silent wrong value). When a later argument
contains a call with a side effect, the compiled program reads earlier
arguments **after** that call. The interpreter evaluates left to right.

## Repro

```turmeric
(def ^mut counter 1)

(defn bump [] : int
  (set! counter (+ counter 100))
  5)

(defn sub [a : int b : int] : int (- a b))

(defn main [] : int
  (println (+ counter (bump)))
  (set! counter 1)
  (println (sub counter (bump)))
  0)
```

| | `(+ counter (bump))` | `(sub counter (bump))` |
| --- | --- | --- |
| `tur --interpret` | 6 | -4 |
| `tur run` (and JIT) | 106 | 96 |

## Root cause

The emitter's value/statement split. `emit_value` returns a C expression and
hoists any call that can panic into a statement before it (`call_ret_note`
plus the `tur_panicking` check). In
`src/compiler/emit_expr.c:emit_value_dispatch` (calls) and
`src/compiler/emit_core.c:emit_builtin` (operators), arguments are emitted in
order:

- an earlier argument that is a plain variable or global comes back as text,
  `counter`;
- a later argument's call is hoisted into a statement that runs immediately.

The final expression then reads `counter` after `bump()` has run.

## Fix directions

- When emitting an argument list, check whether any later argument wrote
  statements into `body`. If one did, spill every earlier non-constant
  argument into a temp first. A cheap version: snapshot `body`'s length per
  argument, and re-emit earlier arguments as temps when it grows.
- Add the repro as a fixture. It runs on all three engines, so `run.sh`,
  `run-turi.sh` and `run-jit.sh` will all pin it.
