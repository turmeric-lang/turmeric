# A capturing lambda a CPS callee kept was freed at the entry boundary

**RESOLVED 2026-10-03** (found and fixed together, working
[serial-cont-escaped-or-deserialized-chain-leaks](../reported/serial-cont-escaped-or-deserialized-chain-leaks.md)
shape 1).  **Severity was high: a use-after-free** in a well-typed program
with no `unsafe`.  Two defects stacked in the same repro, and each crashed it
on its own.

## Repro

```turmeric
(defn ident [x : int] : int x)
(def ^mut saved-f : (fn [int] int) ident)
(defn keep1 [^fat f : (fn [int] int) v : int] : int
  (do (set! saved-f f) (f v)))
(defn mk [n : int] : int (keep1 (fn [x : int] : int (+ x n)) 3))
(defn main [] : int
  (println (mk 100))       ; 103
  (println (saved-f 10))   ; 110 -- was a segfault
  0)
```

## 1. The CPS backend reaped every capturing lambda in an operand position

`keep1` calls its fn argument, which colors it, and so `mk` is CPS-lowered.
`cps_bind`'s `EX_CLOSURE` arm (`src/passes/cps_ir.c`, "B8 slice-3 (probe)")
delegated a capturing lambda with `reap_env` set, unconditionally: the env was
registered with the entry boundary's reap and freed when `mk`'s entry returned
-- while `saved-f` still held it.  The let-bound path
(`cps_closure_env_freeable`) asked the escape walk first; the operand path
asked nothing.

**Fix.**  A pending item now records whether its consuming slot retains it
(`PendItem.reap_ok`, set by `atomize_call_arg` from `call_slot_nonretaining`:
a statically dispatched callee whose parameter is `^borrow` or inferred
non-retaining, `nonretain_param_mask` -- the same slots the direct emitter's
escape walk admits).  `fold_pending` names that expression in
`CpsB.reap_closure_expr`, and the `EX_CLOSURE` arm reaps only it.  Any other
capturing lambda is still delegated, without the reap: it leaks, as every
lambda did before the probe, instead of being freed live.  The leak gates
(`run-leak-check.sh` 122/0, `run-closure-env-leak.sh`,
`run-fat-shim-leak.sh`) did not move: the lambdas they hold at zero all reach
a non-retaining slot.

## 2. Storing a `^fat` parameter double-boxed it

`(set! saved-f f)` goes through `elab_fn_value_to_fat`, which decided whether
the value was thin by its TYPE.  A `^fat` parameter's variable reads thin there
(`fn.boxed` false), so the already-fat handle was wrapped in a second fat box
whose shim called the handle as a code pointer -- `(saved-f 10)` jumped into
the closure's env.  The same happened with a named function passed through the
`^fat` parameter.

**Fix.**  `elab_fn_value_to_fat` returns a variable unchanged when its BINDING
is fat -- `repr_of_binding(vb, REPR_POS_RESULT) == REPR_FAT_HANDLE` for a
parameter, `is_fat` otherwise -- the classification the tail/join walker in
`elab_fns.c` already uses.  It serves the `any` widen and the `^mut` fn cell's
init and `set!` alike.

## On the way: an annotated fn `def`'s box was malloc'd

`(def ^mut saved-f : (fn [int] int) ident)` asked for a static box
(`static_ok`), but the initializer reached the emitter as an `EX_ASCRIBE` of the
global, and the static-box guard in `emit_expr.c` only looked at a bare
`EX_VAR` -- so the box was malloc'd, and the first `set!` orphaned it (24 bytes
per such global).  The guard peels ascriptions now.

## Pinned by

`tests/fixtures/cps-closure-arg-kept-by-callee` (`requires.leak-check`): the
kept lambda is called after `mk` returns, beside a `borrow-only` control whose
lambda reaches a non-retaining slot and must still be reaped.  Suite 3557/0;
the type fuzzer (seed 3333, and `--crossing fn_field` seed 777) reports no bug
class; turi and JIT on the closure/fat/serial fixtures green.
