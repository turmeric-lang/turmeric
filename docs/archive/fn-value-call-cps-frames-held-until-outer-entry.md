# Calling a function value allocates CPS frames that a tail-recursive loop holds until it returns

**RESOLVED 2026-10-09.** The function-value half was fixed 2026-10-08 (the
report's repro, a capturing closure and a capture-free value join run flat),
the effect half 2026-10-09 for a handler case that resumes once, in tail
position -- see "Fixed 2026-10-09" at the end.  What is left, a case that
resumes non-tail or more than once, is
[effect-nontail-resume-copies-held-until-outer-entry](../reported/effect-nontail-resume-copies-held-until-outer-entry.md).

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
[closure-let-in-self-tail-loop-leaks](closure-let-in-self-tail-loop-leaks.md).

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

## Fixed 2026-10-08: the function-value half

Direction 1, but with the release taken where the frame is RUN rather than at
a backedge, and gated so it can never be early. `emit_heap_join` builds its
frame with `dk_frame_resume_join` / `dk_frame_join`, which set `join_once`.
When `dk_run_impl` reaches such a node it hands it back
(`__dk_join_release_node`) only if all of these hold:

- **It was never copied.** `dk_copy_node` now sets `copied` on every node it
  copies, so an unset flag means no captured continuation, E7 delivery or
  async park shares the node's env or will run it. This is the "capture mark"
  of the 2026-10-08 design note, made exact per node instead of a watermark.
- **It is the last entry on the reap list.** Nothing registered after it is
  still outstanding: a `handle` chain, a `perform`'s copied sub, another join
  spliced onto it, a closure env.
- **Nothing pinned DK memory** (`tur_dk_pinned`, r7rs `call/cc`).

The node is popped off the list before it is freed, so the boundary reap never
sees it. The lifted resume function then hands back its env
(`__dk_join_release_env`) right after reading its captures into locals. With
the node gone the env is the list's last entry exactly when this release
happened, and never while a copy runs, because a copied node stays registered
above its env. Every check is a pointer compare against the list's tail, so a
join that fails one is left to the boundary reap exactly as before. Popped
slots are cleared for the collector's scan of the list (`TUR_GC_ON`).

Measured on the report's repro, built at -O2 against the ASan runtime with
the quarantine off, 3e6 iterations:

| | peak RSS |
| --- | --- |
| before (the same C with the `_join` ctors and env release stripped) | 877 MB |
| after | 11 MB (the same 11 MB at 3e5: flat) |

Release counts from an instrumented build: the repro, a capturing-closure
argument and a capture-free value join (`dk_frame_join`) release one node per
turn, and one env per turn where there is an env. A loop whose callee
`perform`s releases none (every join it crosses is copied), and neither did
an async loop, because every `await` shifted, even on a fulfilled future.
Since 2026-10-08 an await on a fulfilled future takes no shift
([async-parked-body-chains-never-reaped](async-parked-body-chains-never-reaped.md)),
so an async loop's joins are released as they run (1,000,000 turns: 10 MB
flat); only the joins a pending await's park copies wait for the body to
settle. The effect loop still falls back to the boundary reap, unchanged.

Pinned by `tests/fixtures/fn-value-call-join-reclaimed` (`requires.leak-check`:
the fn-value, closure and value-join loops, and one-shot, outer multi-shot and
inner multi-shot handlers whose copies run the same joins more than once; all
ASan-clean, and the same answers under `--interpret`) and
`tests/fixtures/fn-value-call-join-reclaimed-async` (a pending await parks the
loop mid-way; leak-checked since the async report above was fixed).
Suite: `run.sh` 3623/0 (the full run's one failure was this fixture's own stale
snapshot, re-run green), `run-leak-check.sh` 126 / 0 / 3 known-open, the JIT
over the CPS, effect, multishot and async fixtures 176/0.

### What is left: the effect half

A `perform` in the loop's callee still grows the heap per iteration, and by
more than the 630 B the original note measured. Under one-shot `(resume k 1)`,
`(use1 ask-plus i)` in a tail-recursive loop peaks at 87 MB at 1e5 and 858 MB
at 1e6 (~860 B a turn). `dk_perform` copies the chain from the perform up to
the handler (marking every join on the way `copied`) and registers the copy
(`__dk_reap_keep(sub)`) above them, so neither the copy nor the originals can
come off the list early. Closing it needs the copy's lifetime proved -- for a
tail-resumed, one-shot case the copy is dead once the resume has delivered --
which is the same proof the E7 deliveries would need.

Inside an async body that parks every turn this is now the whole per-turn
cost: with the loop's joins freed at each park
([async-repeated-park-holds-frames-until-settle](async-repeated-park-holds-frames-until-settle.md)),
a turn that also performs an effect handled inside the body still keeps
~785 B until the body settles (200,000 turns: 167 MB, from 205). A parked
continuation that owns its envs (`dk_copy_range_owned`) and the per-node
copy count (`DK.ncopy`) that fix brought are pieces this proof can use.

## Fixed 2026-10-09: the effect half

The ~835 B a turn came from three things, all per perform:

- `dk_perform` copied the chain from the perform up to the handler (the
  perform's frame, the loop's join, the handler, a done node -- 480 B) and
  resumed the copy through the trampoline;
- the copy marked every original it crossed copied, so the join release could
  never hand back the loop's join or the perform's frame and their envs;
- the trampoline kept each resumed copy until the outermost entry returned
  (`__dk_reap_keep(ch)`: "a pending delivery may still reference it").

For a deep handler whose case resumes `k` exactly once, in tail position,
the copy buys nothing: the ORIGINAL chain from the perform is the
continuation -- the frames up to the handler, the handler still installed,
then the handle's own continuation and the rest of the program, which is what
the copy plus its queued H->next delivery added up to.  So:

- **`case_resumes_k_only_in_tail`** (`emit_cps_ir.c`): a tail-resume case
  whose `k` appears nowhere but as that one resume's continuation (not bound,
  passed, called or resumed with) is installed with
  `dk_handler_tail_inplace`.
- **`dk_perform` resumes in place** for such a handler under a driver: it
  hands the case the original chain, queues no delivery, and marks its head
  `inplace_head`; the trampoline then runs it without freeing or keeping it
  (its nodes have owners already).
- **The perform's frame is a join** (`dk_frame_join` / `dk_frame_resume_join`).
  Copies never run it; run in place, it and its env are handed back as it runs
  (`dk_run_impl`'s DKK_FRAME arm now releases a join's env after its node), and
  so is the loop's join beneath it.
- **A resumed park runs under a trampoline of its own.**  `__tur_async_resume`
  used to run the parked chain through `dk_invoke`, which has no trampoline
  when the resume is driven from direct-style code -- and `dk_perform`
  resumes in place (or trampolines at all) only under one, so every perform
  in a resumed async body took the copying path.  It now runs the park's own
  chain (a park is resumed once) under `__dk_drive_bounded`, marked owned
  elsewhere so the trampoline leaves it to the resume to free.
- **A `handle` hands its handler group back when it exits.** Its
  continuation frame is `dk_frame_resume_group_end`; when the ORIGINAL runs,
  never copied, `__dk_group_release` frees the group and the case envs
  registered just before it (when no handler node in it was copied either),
  and the frame function hands back its own env.  `dk_perform` now reads `H`'s
  fields before a non-tail case runs, since that case can run the handle's
  continuation itself.

**It also fixes a wrong answer.** Resuming a copy that ended at the handle
split the continuation in two: the copy, and the handle's continuation queued
on the meta stack.  When the resumed part performed an effect handled further
out and that outer case resumed non-tail, its resume came back before the
queued part had run, and the queued part then ran on the outer case's result:
`(handle (* 10 (inner)) (Out [] k) (+ 1000 (resume k 5)))` printed 10060
where `--interpret` prints 1060.  In place there is no split.  Pinned by
`tests/fixtures/effect-inner-tail-resume-under-outer` (four nested shapes, the
same output as `--interpret`; two of them were wrong before).  A tail-resume
case that does use `k` before resuming still takes the copy path and still
gets this wrong:
[effect-copy-path-tail-resume-delivers-out-of-order](../reported/effect-copy-path-tail-resume-delivers-out-of-order.md).

### Measured

Peak RSS, -O2:

| program | before | after |
| --- | --- | --- |
| the report's effect repro, `(use1 ask-plus i)`, 1,000,000 turns | 837 MB, 1.8 s | 10 MB, 0.17 s |
| the same called directly, `(ask-plus i)`, 1,000,000 turns | 837 MB, 3.7 s | 10 MB, 0.18 s |
| two effects in one handle, alternating, 200,000 turns | 193 MB | 10 MB |
| an inner handle under an outer one, 200,000 turns | 193 MB | 10 MB |
| a `handle` entered per turn, no perform, 200,000 turns | 85 MB | 10 MB |
| a `handle` entered per turn, a perform inside, 200,000 turns | 219 MB | 10 MB |
| the loop body opens a `handle` and performs an outer effect inside it | 401 MB | 10 MB |
| the loop body opens a `handle` whose case resumes non-tail | 452 MB | 352 MB |
| an async body that parks every turn and performs an effect, 200,000 turns | 205 MB | 10 MB |

`tests/fixtures/effect-loop-resumes-in-place` (leak-checked) prints the reap
list's length at turns 10 and 900 of a loop under one handle and of a loop
that enters a handle each turn: 50 / 4,500 and 5,061 / 10,401 before, 1 / 1
and 1 / 1 now.

All 304 effect and async fixtures run clean under ASan, and ten
adversarial shapes give the previous compiler's answers (nested handlers, two
in-place cases in one handle, deep non-tail recursion, case prefixes, an
aliased `k`, an inner non-tail resume, multi-shot handlers inside and around
an in-place loop, struct and two-argument payloads).  Suite: `run.sh` 3633/0,
`run-leak-check.sh` 133 / 0 / 3 known-open, `run-turi.sh` 2672/0, the JIT over
the async, CPS, effect, handle and multishot fixtures 603/0.
