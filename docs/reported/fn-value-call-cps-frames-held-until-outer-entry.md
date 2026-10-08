# Calling a function value allocates CPS frames that a tail-recursive loop holds until it returns

**Severity: medium (unbounded memory growth in long-running loops).** A
function that calls a function-typed value is emitted through its `__cps`
twin, and each such call from a CPS caller allocates two continuation frames
(~200 B). The frames are registered with the DK reaper, which frees them only
when the *outermost* direct-style entry returns. A self-tail-recursive loop
that makes such a call per iteration therefore grows by ~200 B per iteration
for its whole run.

## Repro

```turmeric
(defn add1 [x : int] : int (+ x 1))
(defn use1 [f : (fn [int] int) i : int] : int (f i))
(defn loop [i : int n : int acc : int] : int
  (if (= i n) acc (loop (+ i 1) n (+ acc (use1 add1 i)))))
(defn main [] : int (println (loop 0 100000 0)) 0)
```

| shape | allocs / call | peak heap / call |
| --- | --- | --- |
| above (tail-recursive driver) | 2 | **205 B** (grows with n) |
| same, closure `(let [f (fn ...)] (f i))` inside `use1` | 2 | 200 B (grows) |
| same `use1`, driven by a `while` loop in `main` | 2 | 0 (bounded) |
| `(fn [int] #fx{} int)` parameter, or `#fx{}` on `use1` | 2 | 205 B (no change) |

Effects show the same retention: a `perform` / `resume` pair in a
tail-recursive loop under one `handle` costs 6 allocations and ~630 B of
peak heap per iteration.

`--dump-cps-coloring` reports `use1` and `loop` as `uncolored`, yet the
emitted C has `use1__cps`, `loop__cps`, and a
`__dk_reap_node(dk_frame_resume(...))` heap join per call
(`src/compiler/emit_cps_ir.c:8778`).

## Root cause (partial)

The reap list is drained by `__dk_reap_run` only when `__dk_entry_depth`
returns to 0, i.e. when the direct-style wrapper around the outermost CPS
function returns. A `while` loop calls the direct wrapper once per
iteration, so it drains each time; a self-tail-recursive loop is itself the
CPS function, so it never does until it finishes. It is not yet clear why a
callee with an empty effect row is emitted on the CPS path at all.

## Fix directions

- Drain the reap list at a self-tail-call backedge in a CPS function: the
  frames of a finished iteration are dead there.
- Or keep effect-free fn-value calls on the direct path (the `#fx{}` rows above
  suggest the effect annotation is not consulted).

Workaround: drive the loop with `while`, or keep calls through function
values out of long-running tail-recursive loops.

Found writing `docs/guides/memory-usage-guide.md` (2026-10-05).

## Investigated 2026-10-07 (not fixed)

Two findings that narrow the fix directions, from the repro's emitted C:

- **Why `use1` is on the CPS path at all.** `cps_color_program`
  (`src/passes/cps.c`) colors any function that makes an unresolved call
  (`has_indirect`, CPS0.1 rule 3), and `--dump-cps-coloring` reports a
  different (may-capture) analysis, which is why it says `uncolored`. The rule
  does not read the callee's effect row, which is why `#fx{}` changed nothing.
  An un-annotated `(fn [int] int)` parameter really does admit an effectful
  function (`(handle (use1 ask-plus 1) (Ask [] k) (resume k 10))` prints 11),
  while a `(fn [int] #fx{} int)` parameter rejects one with TUR-E0009, so a
  closed empty row would be a sound reason to skip the coloring -- for effects.
  But the same coloring is what lets a tail call through a procedure value
  bounce (proper-tail-calls T6), so an exemption must keep tail-position
  indirect calls colored, or prove the bounce is not needed there.
- **Why direction 1 is not a one-liner.** In the repro the self call is not a
  main-body backedge: `loop__cps` hands `use1__cps` a heap join
  (`dk_frame_resume(loop_j0, ...)`), and the self call is made from the lifted
  continuation `loop_j0`. So there is no `__tur_cps_self` jump to drain at,
  and an iteration mark would have to travel in the join's env. Not every
  registered node is dead there either: `dk_run_impl` reads `k->next` after a
  `DKK_FRAME` node's function returns, so only `DKK_RESUME_FRAME` nodes (which
  it tail-calls) and the envs their continuations have already unpacked are
  free to go.

The closure half of the same retention, an only-invoked closure let-bound in
a main-body loop, is fixed:
[closure-let-in-self-tail-loop-leaks](../archive/closure-let-in-self-tail-loop-leaks.md).

## A design for direction 1, and why it was not landed (2026-10-08)

Read against the repro's emitted C.  Each iteration of `loop__cps` mallocs
the join's env, registers it (`__dk_reap_ptr`), builds the resume frame and
registers that (`__dk_reap_node(dk_frame_resume(loop_j0, env, __kont))`),
then calls `use1__cps`.  For a pure callback nothing else is registered
before `dk_run` reaches the frame and tail-calls `loop_j0`, so at the top of
`loop_j0` -- once it has unpacked its captures -- the reap list's last two
entries are exactly this frame's env and node.  Popping and freeing them
there keeps the list, and the heap, flat.

What makes it more than that check is proving nothing else can still reach
them:

- **Copies share the env.**  `dk_perform`, `shift`, `dk_invoke` and the E7
  deliveries capture through `dk_copy_range` / `dk_copy_node`, and a
  `dk_frame_resume` node has no `env_clone`, so a copy of the frame runs
  `loop_j0` on the SAME env -- possibly more than once (a multi-shot `shift`).
  Any copy taken after the frame was registered must disable the early free:
  a "capture mark" (`__dk_reap_n` at the last `dk_copy_node`) below which
  nothing is freed early would cover these.
- **`tur_dk_pinned`** (r7rs `call/cc`) already freezes all freeing.
- **A suspended async computation** keeps its live chain to resume later,
  without copying it; whether that chain is freed after the resume (which
  would then walk into a node freed early) was not established.  Nor was it
  established that no other path hands out the live chain.

A miss is a use-after-free in EMITTED code, and the fixture suite runs
emitted programs without a sanitizer, so it would pass silently.  Landing
this wants the capture mark, an audit of every path that holds a `DK *` past
the call that made it, and a `requires.leak-check` (ASan) fixture over
effects, `shift`, async and `call/cc` in such a loop.
