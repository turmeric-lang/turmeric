# A parked async body's continuation chains are never reaped, and every later await adds to them

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
