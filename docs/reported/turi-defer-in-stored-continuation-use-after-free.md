# `tur --interpret`: resuming a stored `k` whose body has a pending `defer` is a heap-use-after-free

**Severity:** high (memory safety, interpreter only). Under `tur --interpret` a
handler case that stores its continuation and returns, followed by a
`(resume saved v)` after the `handle` has exited, reads freed memory when the
performing function has a `defer` pending. On the Debug `tur` that is an ASan
abort; on a Release one the read is undefined (not measured). The compiled
program is correct, and so is the interpreter when the body has no `defer`.

Found 2026-10-17 while graduating `repl-jit-inline-c`
([aot-compiled-repl-plan](../upcoming/aot-compiled-repl-plan.md) C1). With
the inline-C JIT on, `tests/fixtures/defer-cps-abortive-handler` runs past its
first `note` call (an inline-C `cstr -> int` body the interpreter used to
refuse) and reaches case 4, which is this bug. The JIT has nothing to do with
it: the repro below has no inline-C. The fixture is in `run-turi.sh`'s inline-C
carve-out, so no suite runs it under the interpreter.

## Repro

```turmeric
(defeffect Ask [] : int)

(defn w4 [] : int
  (defer (println "w4: defer ran"))
  (+ 1 (perform (Ask))))

(defn main [] : int
  (let [^mut saved 0]
    (println (handle (w4) (Ask [] k) (do (set! saved k) 0)))
    (println "before resume")
    (println (resume saved 5)))
  0)
```

```
$ tur run repro.tur            # compiled: correct
0
before resume
w4: defer ran
6
$ tur --interpret repro.tur    # Debug tur
==...==ERROR: AddressSanitizer: heap-use-after-free ...
    #0 eval_drive_ex src/turi/eval.c:10694
freed by thread T1 here:
    #1 eval_handle src/turi/eval.c:3864
previously allocated by thread T1 here:
    #1 eval_handle src/turi/eval.c:3784
```

Delete the `(defer ...)` line and `--interpret` prints `0`, `before resume`,
`6`.

## Root cause

`TURI_TRACE_FIBER_FALLBACK=1` says which runtime the `handle` takes:

```
turi: note: handle at 9:14 falls back to the single-shot fiber effect runtime:
the form at 4:3 (EX kind 22) is not work-stack capturable
```

The form at 4:3 is the `defer`. `ws_capturable` refuses it, so the `EX_HANDLE`
arm of `eval_drive_ex` (`src/turi/eval.c`, "fiber fallback") calls
`eval_handle`. That allocates the `TuriEffectCont` (`eval.c:3784`), runs the
body on its own fiber stack, dispatches the case, and then unconditionally
`munmap`s the body stack and `free`s the cont (`eval.c:3860`, `3864`) when the
case returns. The case had already stored `k`, a value that points at that
cont, in `saved`. The later `resume` reads `k.as_cont->ws` at `eval.c:10694`,
which is freed memory.

The fiber runtime is documented as single-shot. Even so, a stored `k` resumed
once, after its handle returns, is a single shot, and it is exactly what the
compiled program and the work-stack runtime support (the scheduler idiom). So
the fiber path frees a continuation that is still reachable.

## Fix directions

1. Make `defer` work-stack capturable, so this shape never reaches the fiber
   runtime. That is the root fix and also removes the single-shot limit for
   it.
2. Until then, the fiber path must not free a continuation a case may have
   kept. Either keep the cont (and its stack) alive while a value refers to it
   -- a reference count on `TuriEffectCont`, or marking it escaped when a case
   binds `k` to anything other than an immediate `resume` -- or mark it dead
   at `eval.c:3864` and make the resume path return a clean error ("a fiber
   continuation was resumed after its handle exited") instead of reading it.
   An error is acceptable; a use-after-free is not.

Pin it with a fixture that is NOT inline-C (so `run-turi.sh` runs it): the
repro above, with `expected.stdout` from the compiled program.
