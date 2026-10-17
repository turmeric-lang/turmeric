# `return` inside a CPS-lowered `while` acts as `break`

**RESOLVED 2026-10-17** -- see [Resolution](#resolution-2026-10-17). Pinned by
`errors/cps-loop-return-refused`.

**Severity:** medium-high. A silent wrong value on the default build: the
compiled program and `tur --interpret` disagree, and nothing warns.

## Repro

```turmeric
(defeffect Tick [n : int] : int)

(defn scan [] : int
  (let [^mut i 0]
    (while (< i 10)
      (when (= (perform (Tick i)) 1)
        (return (+ 100 i)))
      (set! i (+ i 1)))
    (+ 1000 i)))

(defn main [] : int
  (println (handle (scan)
             (Tick [n] k) (resume k (if (= n 3) 1 0))))
  0)
```

- `tur --interpret`: `103` (correct: the `return` leaves `scan`).
- `tur run`: `1003`. The `return` leaves only the loop, and `scan` goes on to
  compute `(+ 1000 i)` with `i = 3`.

`scan` is in the CPS set (no `[EVICT]` line under `TUR_TRACE_EVICT=1`). Its
CT-IR (`tur --dump-cps check`) shows the problem directly:

```
letcont j2147483650(__t1) =
  let __t0 = (+ 1000 i)
  (k __t0)
in
loop () ->
  ...
      then
        let __t7 = (+ 100 i)
        (k __t7)          <- the return
      ...
    else
      (k ())              <- the loop's normal exit
```

## Root cause

`src/passes/cps_ir.c`: both `cps_tail` and `cps_bind` lower `EX_RETURN` as
`cps_tail(b, value, b->retk)`, i.e. delivery to `KK_RET`. Inside a loop, the
exit arm `build_loop` builds is also an `APPCONT` to `KK_RET`. The emitter
renders the loop as a `<fn>_loop<N>__cps` helper whose `KK_RET` is the
helper's own continuation. In join mode (code follows the loop) that
continuation is the `letcont j` holding the code after the loop, so the return
and the normal exit become the same thing.

When the loop is the last form of the function (plain mode), the helper's
continuation is the function's, and `return` happens to be correct.

## Fix directions

- Give the translator a distinct "function return" continuation while it is
  inside a loop body. For example, record the outer `retk` on entry to
  `build_loop`, and have `EX_RETURN` deliver to a `KK_VAR` that the emitter
  threads into the loop helper as an extra continuation parameter.
- Or refuse it: a `return` inside a join-mode loop body becomes
  `CT_UNSUPPORTED`, which evicts the function to a located error rather than
  a wrong value.
- Add a fixture (`expected.stdout` = `103`) so `run.sh`, `run-turi.sh` and
  `run-jit.sh` all pin it.

## Resolution (2026-10-17)

Refused, per the second fix direction: `build_loop` (`src/passes/cps_ir.c`)
rejects a join-mode loop -- one with code after it -- whose body contains a
`return` (`expr_has_return`). The function then cannot be CPS-lowered, and
since its loop performs, it gets the located "this effect operation has no
lowering here" error, whose "Known unsupported shapes" list now names this one.
A wrong value became a compile error. A plain-mode loop (the last form of its
function) was already correct and still lowers. The interpreter runs the
program correctly (`103`).

Threading the function's own continuation into the loop helper (the first fix
direction) would make the shape compile; nothing needs it yet.

Pinned by `errors/cps-loop-return-refused` (`requires.compiled`: the
interpreter accepts the program).
