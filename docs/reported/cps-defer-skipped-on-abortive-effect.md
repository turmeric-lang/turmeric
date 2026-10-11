# A `defer` in a CPS function does not run when an abortive handler or an escape abandons its frame

**Severity: medium (cleanup skipped: leaks, and anything else a defer was
guarding).** Noted 2026-10-11 while closing
[defer-skipped-on-panic-in-cps-function](../archive/defer-skipped-on-panic-in-cps-function.md),
which fixed the panic path only.

## Repro

```turmeric
(defn note [s : cstr] : int
  ```c printf("%s\n", s); fflush(stdout); return 0; ```)
(defeffect Abort [] : int)

(defn work [] : int
  (defer (note "work: defer ran"))
  (+ 1 (perform (Abort))))

(defn main [] : int
  (handle (work) (Abort [] k) 0)   ; never resumes
  (note "done")
  0)
```

```
done                   <- "work: defer ran" is missing
```

`tests/fixtures/defer-cps-caught-panic` case 10 pins the current behaviour
(the defer does not run, and a later panic does not run it late either).

## Root cause

`work`'s block continuation -- the frame that runs its deferred body after
the perform's value is delivered -- is copied for the handler and never run
when the handler returns without resuming. Nothing fires it. In the direct
backend there is no equivalent exit (a direct function cannot be left by an
effect), so there is no direct-emitter behaviour to mirror.

The panic fix gave every such frame an unwinder (`<name>_unw`,
emit_cps_ir.c) that runs its pending defers from the frame's env, and the DK
runtime walks the chain a *panic* abandons and fires them
(`__dk_panic_abandon`, emit_dk_runtime.c). An abort is deliberately not
walked: at the moment a handler case returns without resuming, the runtime
cannot tell an abort from a continuation the case stored for later (a
generator, a backtracking search), and firing the defers of a continuation
that is resumed later would run them before their scope ends -- a
use-after-free for a `(defer (free buf))`.

## Fix directions

- When a handler case returns, its sub-continuation `sub` (dk_perform) is
  dead if nothing copied it (`ncopy == 0`) and it was not resumed in place:
  walk it with `__dk_panic_abandon`'s fire loop then. A stored `k` is a
  copy, so `ncopy` says whether the case kept one. That catches the plain
  abort (`(Abort [] k) 0`) without touching generators.
- An escape (`call/ec`, a `return` out of a colored function) abandons its
  frames at a known point too; the same fire-the-dead-chain step applies
  where the escape lands.
- Multi-shot: a continuation resumed twice runs its defers twice today (once
  per copy); that is the existing P5b semantics and is not changed by any of
  the above.
