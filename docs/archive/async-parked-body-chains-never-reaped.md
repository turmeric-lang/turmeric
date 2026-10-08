# A parked async body's continuation chains are never reaped, and every later await adds to them

**RESOLVED 2026-10-08** -- see "Fixed 2026-10-08" at the end, which also
corrects the diagnosis below: the per-await leak had nothing to do with
parking, and parking switched the reaper off for the whole thread.

**Severity: medium (unbounded memory growth in long-running async loops).**
Once an `async` body parks on a pending `await`, nothing it allocates on the
DK machine is reclaimed again. That covers the park's own copy, and the copy
and frames of every later `await`, including awaits on futures that are
already fulfilled. A long-running async loop grows by ~250 B a turn for
its whole life.

Split out 2026-10-08 from the "Also seen here" note of
[await-through-fn-value-parks-only-the-callee](../archive/await-through-fn-value-parks-only-the-callee.md),
which is fixed; this half is not. The archived
[compiled-async-heap-continuations-plan](../archive/compiled-async-heap-continuations-plan.md)
lists a narrower form as a known residual ("a parked async body leaks its
`__root` prompt"). Measured, it is not one prompt per park.

## Repro

`tests/fixtures/await-through-fn-value`, built under ASan
(`TUR_RUNTIME=source`, `-fsanitize=address`) and run with `detect_leaks=1`:

| program | leaked |
| --- | --- |
| one `async` body, one await that parks (`(+ 1 (await f))`) | 536 B in 5 allocations |
| the fixture, its `named` loop at 10 turns | 40,056 B in 436 |
| the same, `named` at 50 turns | 49,976 B in 556 |

40 more turns cost 9,920 B: **~248 B per await** after the first park. The
leak records are `dk_new` nodes (the shift's copied chains) and the
continuation envs of the awaiting callers, plus the fixture's own futures,
which it never frees.

## Where it comes from (read, not yet instrumented)

- Every entry wrapper skips its reap while `tur_async_suspended` is set
  (`if (!tur_async_suspended) { ... __dk_reap_run(); ... }`), so a body that
  parked leaves its whole reap list behind when its entry returns.
- The parked copy is resumed later from `tur_future_fulfill`'s `on_complete`
  callback, outside any entry wrapper. Nothing registered while it runs ever
  reaches an outermost `__dk_reap_run`.
- `await` always shifts to the entry root, even on a fulfilled future, so
  every later turn copies its chain again (`dk_copy_range`). The early
  release of `fn-value-call-cps-frames-held-until-outer-entry` cannot apply,
  because every join an await crosses is copied (measured: 0 releases in
  `fn-value-call-join-reclaimed-async`).

## Fix directions

1. Give the resumed park an entry boundary of its own: the `on_complete`
   resume runs under a reap scope that frees what the resumed run registered
   once it settles without re-parking (a re-park hands the list on, as the
   first park should).
2. Skip the shift for an `await` on a future that is already fulfilled:
   deliver the value in place, as the F3.1 inline path does for a body that
   never parked. That removes the per-turn copy, and with it most of the
   ~248 B.
3. Track resume completion so the first park's own registrations (the root
   prompt the plan names) are freed when the outer future is fulfilled.

`tests/fixtures/fn-value-call-join-reclaimed-async` and
`tests/fixtures/await-through-fn-value` deliberately carry no
`requires.leak-check` because of this; a fix should add it to both.

## Fixed 2026-10-08

**The diagnosis above was wrong in its main claim.** The ~248 B per await was
not about parking at all. An async body that never parks leaked exactly the
same: every `await` built a shift node, a frame node and (for a capturing
continuation) an env, and nothing ever registered them with the reaper
(`emit_await`, `src/compiler/emit_cps_ir.c`). That was the whole
LeakSanitizer-visible leak: 3 allocations an await, 248 B.

**Parking hid a second, worse problem that LeakSanitizer cannot see.** The
entry a body parked in skipped its exit altogether (`if (!tur_async_suspended)
...`), so it never left its depth. `__dk_entry_depth` then never got back to 0,
and no later outermost exit ever ran `__dk_reap_run`: after the first park,
**nothing registered by any CPS entry on that thread was freed again**,
async or not. The reap list is a global, so all of it reads as reachable and
the leak check stays green. Measured: after one park, an unrelated
`(handle (eff-loop ...))` of 100 steps left 500 entries on the list per call;
2,000 calls of a 1,000-step loop peaked at 1,674 MB.

And the shift every await took, even on a fulfilled future, resumed a copy of
the chain one C frame deeper than the last: 100,000 awaits in one async body
overflowed the stack (SIGSEGV) at -O2.

### What changed

1. **An await on a fulfilled future takes no shift** (`__tur_await_ready` /
   `__tur_await_value`, `emit_module.c`; `emit_await`). The value goes straight
   to the continuation: the function's own continuation for a trivial await,
   or the lifted frame called in place. That is what `__tur_await_body` did
   with it after copying the chain; now there is no copy and no node. The
   ready check drains runnable scheduler fibers first, as the shift body
   would, so a future resolved that way takes the fast path too. A lifted
   frame hands its env back after reading its captures
   (`__dk_join_release_env`, extended from joins to await continuations) when
   the env is the reap list's last entry: always on the fast path, never while
   a copy runs.
2. **The shift path registers what it builds** -- the shift node, the frame
   node and the env -- and once `dk_run` returns from the shift, hands the two
   nodes back if nothing was registered after them (`__dk_await_release`):
   only copies of the frame ever run, so the originals are unreachable. A
   parked body keeps only the env, which the parked copy shares.
3. **A park is owned, and reaped when it settles.** Every entry wrapper exits
   through `__dk_entry_leave` (`emit_module.c`). A body that parked hands
   what its entry registered, and its root prompt, to the park record (an
   amortized growable array, `__dk_reap_seg`) and leaves its depth like any
   exit. The owner is the entry the await parked at the depth of, recorded in
   the park (`TurAsyncPark.depth`); an entry exiting while someone else's park
   is pending keeps the old behaviour. `__tur_async_resume` runs as an entry
   of its own: it holds the park's share aside (off the list, so no last-entry
   release can take an env a copy still shares), runs the parked copy, and
   then either puts the share back and reaps it with everything the run
   registered (the body settled; the payload is a word, so the outer future's
   value never points into what was reaped), or passes it on whole to the next
   park, which appends only that turn's registrations.

### Measured

| program | before | after |
| --- | --- | --- |
| `await-through-fn-value` under ASan, its futures freed | 49,992 B leaked in 557 allocations | 0 |
| 100,000 awaits on a fulfilled future in one async body (-O2) | SIGSEGV | 10 MB peak, the same at 1,000,000 |
| the same after the body's first await parks | SIGSEGV | 10 MB peak |
| one park, then 2,000 runs of a 1,000-step effect loop | 1,674 MB peak | 10 MB |
| a loop that parks on a fresh future every turn, 400,000 turns | 118 MB, never freed | 76 MB, freed when the body settles |
| reap list at the end of `async-park-reaped-after-settle` | 9,056 entries, depth 6 | 0, depth 0 |

At -O1 under ASan the fast path still nests one C frame per await -- the
compiler does not turn the loop's `return dk_run(...)` into sibling calls
there, as it does not for any CPS loop through a join -- so the fixtures keep
their counts in the thousands.

Pinned by `tests/fixtures/await-fulfilled-future-takes-no-shift` (trivial,
capturing and branching continuations, 2,000 awaits each) and
`tests/fixtures/async-park-reaped-after-settle` (a re-park across a branch, a
nested async, a resume driven from inside an effect loop, a park on every turn,
and an effect loop after a park; it prints the reap list's length and the depth
last), both `requires.leak-check`, and by `requires.leak-check` on
`await-through-fn-value` and `fn-value-call-join-reclaimed-async`, which now
free their futures. All 34 async fixtures also run clean under ASan. Suite:
`run.sh` 3630/0, `run-leak-check.sh` 131 / 0 / 3 known-open, `run-turi.sh`
2671/0, the JIT over the async, CPS, effect and multishot fixtures 427/0 (its
six cc fallbacks fall back before the change too).

### What is left

A body that parks again and again keeps each park's frames until it settles:
~187 B a turn for the every-turn loop above (it was ~293 B, never freed). A
server loop that never settles therefore still grows. Filed as
[async-repeated-park-holds-frames-until-settle](../reported/async-repeated-park-holds-frames-until-settle.md).
