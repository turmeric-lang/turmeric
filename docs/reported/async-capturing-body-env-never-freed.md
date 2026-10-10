# A capturing `async` body's closure env is never freed

**Narrowed a fifth time 2026-10-09: the future of an awaited fresh spawn is
freed too.** `(await (async ...))` names its `TurFuture` nowhere else, and
no stdlib call frees a raw spawn future (`future-free` is the typed
Promise/Future cell's), so every such expression leaked its 56 bytes
(`tur_future_new`, the calloc this report's measurements set aside as "the
program never frees"). The await owns that future now: the emitter marks it
at the await (`__tur_await_own`, `src/compiler/emit_module.c`; the direct
emitter's `EX_AWAIT` and the CPS `emit_await`, through a `fresh_fut` flag
`build_await` sets when the awaited expression is a spawn written there),
and whichever reader takes its value frees it right after --
`tur_await_future`, `__tur_await_value` on the CPS fast paths,
`__tur_await_body` when the shift finds it done, and the park's
`__tur_async_resume` (the value arrives as its argument; `tur_future_fulfill`,
its caller, touches the future no further). A thread-backed future is joined
before any reader reads. A future the program holds (`(let [f (async ...)]
...)`, the fixtures' `drive`) stays the program's. Pinned by
`tests/fixtures/await-fresh-spawn-future-freed` (leak-checked: the three
CPS await paths, a struct capture, a captureless body, a float payload, 200
spawns from a loop, and `main`'s direct-style awaits).

Found on the way, under `tur jit` and any hosted split-runtime build --
which on Linux is the DEFAULT cc path too (cc-path-preamble-split-plan;
`tests/run-leak-check.sh` forces `TUR_RUNTIME=source`, the one mode that
links no host runtime, which is why it never saw this): none of the env
drops this report records ever happened there. `tur jit` on
`async-capturing-body-env-dropped` reported 202 leaked blocks of 24 B where
the source-runtime path reports none. The spawn's ownership flag never reached
the runtime: `tur_async_owns_env` and `tur_async_direct_body` are
thread-locals the preamble declares with the same selector as the others,
but neither was in the split generator's TLS table
(`tools/gen-runtime-split.py`), which is what makes the RUNTIME half read a
thread-local through the host accessor (`src/runtime/tur_tls.c`). So the
hosted program wrote the accessor's slot and the runtime half read a native
thread-local of its own -- the exact mismatch the generator's comment
describes for `tur_current_fiber`. The same held for
`tur_async_direct_body`, which the await-depth refusal reads to place an
async body's root. Both rows are in the table now; `tur jit` on the async
fixtures reports no leak and the refusal fixture still refuses.

**Narrowed a fourth time 2026-10-09: a lambda bound to a local first.**
`(let [body (fn [] ... k ...)] (await (async body)))` leaked the box (32 B a
spawn): the let could not drop it -- the body may still be running, parked --
and the spawn owned nothing. When the local's only use is that one spawn
(every mention of the binding is the spawn's `fn_expr`: not under a `while`,
where a spawn per turn would each drop the same box; not inside a closure;
and no `perform` in the let, where a multi-shot resume could run the spawn
twice), the direct emitter's let marks it (`Binding.spawn_owns_env`,
`let_mark_spawn_owned`, `src/compiler/emit_expr.c`) and the spawn owns the
box as it does a lambda written at the spawn (`async_spawn_owns_env`).
Pinned by `tests/fixtures/async-let-bound-body-env-dropped` (leak-checked:
inline, parking, a struct capture, 200 spawns from a recursive loop) and
`async-let-bound-body-env-kept` (the same local spawned twice, from a
`while`, and from a lambda that captured it: kept, right answers). A let the
CPS backend lowers is not marked, so that shape still leaks there.

**Narrowed a third time 2026-10-09: the thread-backed spawn drops its box,
and a session-driving body qualifies.** `tur_async_thread_via` (a body that
drives a session endpoint runs on its own OS thread) owned nothing; it now
takes the same `tur_async_owns_env` flag, carries the box on its
`TurAsyncThreadArg`, and the thread drops it once the body has settled and
the future is written. The flag, and `tur_async_direct_body` beside it, are
thread-local now (`emit_rt_tls`, accessors in `src/runtime/tur_tls.c`): each
is set and read by one thread between a spawn site and its spawn, and a
thread-backed body spawning must not read another thread's. And the inline-C
test that kept such a body out -- `(let [[n r] (recv r)] ...)` holds the
session op's `tur_session_recv(__TUR_VAL_0__)` block -- asks the exact
question now: does an inline-C block NAME one of the closure's captures
(`closure_body_inline_c_touches_env`), which is the one way C gets an lvalue
into the box; a block with no captures cannot. The same test replaces the
whole-body one in the CPS local-closure predicates. Pinned by
`tests/fixtures/async-on-thread-body-env-dropped` (leak-checked: a typed
session body 50 times in a loop, and an untyped one);
`session-async-peer-on-thread` leaks only the four futures it never frees
(320 B in 8 allocations before, 224 B in 4 now).

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
lambda bound to a local that is spawned more than once, or in a let the CPS
backend lowers; an inline-C block in the body that names a capture.

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

(The 56-byte `tur_future_new` leak in the same report was the future itself,
which this program could not free; since the fifth narrowing above the
await frees it. A program that holds its futures, like the fixtures'
`drive`, frees them itself and shows only the env.)

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
