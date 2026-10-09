# A capturing `async` body's closure env is never freed

**Narrowed again 2026-10-09: a box-only env is dropped, and the typed spawn
drops too.** An env whose drop glue frees the box alone is now owned by the
spawn (`closure_env_drop_frees_box_only`, `src/compiler/emit_core.c`): no rc
capture, no owned `^fat` closure capture, no Drop-instance capture, and no
inline C in the body (walked exactly, through `cps_visit_children`, where
`expr_subtree_has_inline_c` reads an `await` as possible inline C). A struct
capture is a copy in the box, so it qualifies. The typed spawn
(`tur_async_fiber_via`, a float / bool / pointer body) takes the same
ownership protocol as `tur_async_fiber_closure`; it owned nothing before.
`await-in-struct-capturing-async-body` is leak-checked now, and
`tests/fixtures/async-struct-capturing-body-env-dropped` pins inline and
parking struct-capturing bodies and a typed float body, 100 spawns of each
(11,312 B in 303 allocations before, 0 now).

**Narrowed 2026-10-09 (filed the same day): a shallow env is dropped.** A
fresh capturing lambda written at the spawn, whose env drop is shallow
(`closure_env_drop_is_shallow`: scalar captures, no inline C), is now owned by
the spawn. The emitter sets `tur_async_owns_env` before
`tur_async_fiber_closure`. A body that settles inline drops its box there; a
body that parks hands the box to the park (`TurAsyncPark.own_env`, carried
through each re-park), and `__tur_async_resume` drops it once the body
settles. Pinned by `tests/fixtures/async-capturing-body-env-dropped`
(leak-checked: inline, parking every turn, and 200 spawns in a loop -- 4848 B
in 202 allocations before, 0 now).

**What is left:** a capture whose drop glue releases something -- an `rc`, an
owned `^fat` closure, a Drop instance -- since the body's result may still
reference it (a scalar result could not, which would admit those too); a
lambda bound to a local before the spawn (its `let` owns it); the
thread-backed spawn (`tur_async_thread_via`, `__tur_async_call_box`).

**Severity: low (was low-medium; a leak per spawn).** `(async (fn [] ... captured ...))`
mallocs the lambda's env box, hands it to `tur_async_fiber_closure`, and
nothing frees it afterwards: 32 bytes for a one-struct capture, every time the
spawn runs. A server that spawns a capturing task per request leaks one box
per request. Found 2026-10-09 while leak-checking
`tests/fixtures/await-in-struct-capturing-async-body`; it is independent of
the CPS work that fixture pins, since a body that never awaits leaks the same
way.

## Repro

```turmeric
(defstruct P [a : int b : int])
(defn main [] : nil
  (let [p (P 5 0)]
    (println (await (async (fn [] : int (+ (.a p) 1)))))))
```

Under `tests/run-leak-check.sh`'s flags (`-fsanitize=address`,
`detect_leaks=1`, `use_stacks=0:use_registers=0`):

```
Direct leak of 32 byte(s) in 1 object(s) allocated from:
    #1 ... in main__cps ...   <- the env box `malloc(sizeof(void *) + sizeof(struct __env_N))`
```

(The 56-byte `tur_future_new` leak in the same report is the future itself,
which this program never frees. A program that frees its futures, like the
fixtures' `drive`, shows only the env.)

## Root cause

A capturing lambda passed to `async` is boxed as a fat closure
(`{ thunk, captures... }`) at the spawn site (`emit_expr.c`, EX_ASYNC, path
(a): `fn_expr->type.as.fn.boxed`). The spawn reads the thunk from slot 0 and
calls it with the box as its env. The same lambda let-bound and called
locally is dropped at scope exit (`TUR_CLOSURE_DROP`). The box handed to a
spawn is never dropped: not when the body settles inline, and not when a
parked body is resumed and settles later.

## Fix directions

- The spawn owns the box. Free it (through the closure's own header drop,
  for its captures' drop glue) when the body settles: after `fn` returns in
  `tur_async_fiber_closure` if the body did not park, otherwise when
  `__tur_async_resume` fulfills the outer future (the park record would carry
  the box).
- A thread-backed spawn (`tur_async_thread_via` with `__tur_async_call_box`)
  needs the same at the end of the thread's run.
- Check that nothing else holds the box. A lambda built once and spawned
  twice would be the case to rule out.
