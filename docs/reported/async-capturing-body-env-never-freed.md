# A capturing `async` body's closure env is never freed

**Severity: low-medium (a leak per spawn).** `(async (fn [] ... captured ...))`
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
