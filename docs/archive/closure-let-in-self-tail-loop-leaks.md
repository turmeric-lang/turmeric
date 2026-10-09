# A closure let-bound in a self-tail-recursive body is never dropped

**RESOLVED 2026-10-07.** Two changes, one per half of the problem:

1. **The leak.** The loop is a CPS function (it calls a function value), and
   a CPS let is flattened into binders with no scope exit. The CPS path
   already reaped a let-bound closure *literal* at the DK entry boundary
   (`cps_closure_env_freeable`), but not the repro's shape, a call to a
   `returns_fresh_closure` function -- the one the direct emitter drops via
   `let_binding_env_freeable`'s `fresh_call`. `cps_fresh_call_env_freeable`
   (`src/passes/cps_ir.c`) now admits it, under the same escape walk and only
   when the call can be delegated whole (`safe_to_delegate`). Valgrind on the
   repro: 10,025 allocations, 10,025 frees (was 10,000 blocks definitely
   lost).
2. **Holding every turn's env until the loop returns.** A binder that is only
   ever *invoked* (`closure_binding_only_invoked`: the escape walk with the
   `^borrow` / non-retaining-parameter relaxations off) carries
   `letraw.reap_at_backedge`, and a self tail call in the main body frees it
   before the jump, after the arguments are computed:
   `__dk_reap_closure_now` takes the entry off the reap list (order kept, so no
   entry's mark moves) and drops it, so the boundary never frees it twice. It
   does nothing while `tur_dk_pinned` (a kept continuation may still reach
   it). Peak heap on the repro: 4 KB (stdout's buffer).

The case the fix directions said to keep is kept: a closure that is the tail
call's *argument* is the next turn's parameter, so it is not dropped at the
backedge; it stays on the boundary reap.

Pinned by `tests/fixtures/closure-let-in-self-tail-loop` (`requires.leak-check`,
so `tests/run-leak-check.sh` runs it under LSan): the repro, a closure
literal, a closure called only in the tail call's arguments, and one moved
into the tail call. The only snapshot change is the new runtime helper's text
in the DK preamble (157 fixtures); no fixture's program code moved.

**Not covered:** a binder in a lifted continuation (the backedge is then not
in the main body) and the mutual-recursion group jump keep the boundary reap
-- freed, but only when the outermost entry returns, the shape
[fn-value-call-cps-frames-held-until-outer-entry](fn-value-call-cps-frames-held-until-outer-entry.md)
describes for continuation frames.

**Severity: low (leak; 24 B per iteration for a one-capture closure).** The
closure-drop glue frees a `let`-bound capturing closure at scope exit, but
when the scope ends in a self tail call that becomes a backedge, no drop is
emitted before the `goto`.

## Repro

```turmeric
(defn mk [k : int] : (fn [int] int) (fn [x : int] : int (+ x k)))
(defn loop [i : int n : int acc : int] : int
  (if (= i n) acc
    (let [f (mk i)
          r (f 1)]
      (loop (+ i 1) n (+ acc r)))))
(defn main [] : int (println (loop 0 10000 0)) 0)
```

valgrind: 10000 allocations of 24 B, all "definitely lost" at exit.

Controls that do not leak:

- the same body in a `while` loop (`(let [f (mk i)] (set! acc ...))`);
- the closure bound in a non-tail helper `(defn use1 [i] (let [f (mk i)] (f 1)))`
  -- the emitted body ends with `TUR_CLOSURE_DROP(f_14)`.

## Root cause

The emitted `loop__cps` reassigns its parameters and jumps to
`__tur_cps_self` without a `TUR_CLOSURE_DROP` for `f`. The `rc<T>` path gets
this right -- the same shape with `(rc/of i)` fires its defer frame
(`tur_frame_fire_lifo`) before the backedge -- so the closure drop is
missing from the CPS self-tail-call lowering specifically.

## Fix directions

Emit the scope's closure drops before the backedge in the CPS self-tail-call
path, as the direct path does for rc defers. If the closure flows into the
tail call's arguments it is moved, not dropped, which is the case to keep.

Found writing `docs/guides/memory-usage-guide.md` (2026-10-05).
