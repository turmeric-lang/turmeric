# Compiled code evaluates a later argument's call before an earlier argument

**Resolved 2026-10-18.** The same order bug was in three places, and all
three are fixed. Each uses one predicate, `expr_reads_mutable`
(`src/compiler/expr.c`): a read of a `^mut` binding, a field, a dereference
or a dynamic var, directly or through a pure builtin, cast or `if`. Only
scalars are spilled (`type_kind_is_order_spillable`).

- **The direct emitter.** `emit_arg_value` / `emit_arg_keep_order`
  (`emit_core.c`) emit argument i of a list. They spill it into a temp on the
  spot when it reads mutable state and a later argument is anything but a
  plain read. They are used by builtins, the regular, closure, generic,
  inline-C and constructor call paths, and set literals. The raw value is
  spilled before any bridge the caller wraps around it.
- **The elaborator's operand hoist.** `elab_hoist_control_operands`
  (`elab_call.c`) binds a constructor's call-bearing operand in a `let` ahead
  of the constructor, in a unit that declares an effect. An earlier mutable
  operand is now bound with it, in order. This was the half the report did
  not know about: it made `(P counter (bump))` wrong under `tur --interpret`
  too. The interpreter loads the stdlib's effects, so its units always
  declare one.
- **The CPS IR's ANF.** An atom was read where it was used, after every
  pending binding. `atomize_operand`, `rank2_hoist_args` and
  `delegate_call_atomized` (`src/passes/cps_ir.c`) now bind a mutable atom
  in order when a later operand of the same list is bound.

`args-evaluated-left-to-right` covers every call shape (11 lines).
`args-evaluated-left-to-right-effect-unit` covers the elaborator and CPS
halves, a constructor inside a `handle` included. Both print the same on
cc, `tur jit` and `tur --interpret`.

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
