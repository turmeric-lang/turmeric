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

## Resolved 2026-10-11

Both directions landed in the DK runtime (`src/compiler/emit_dk_runtime.c`);
fixture `tests/fixtures/defer-cps-abortive-handler` carries the repro above
and fifteen more shapes, and `defer-cps-caught-panic` case 10 now expects
the abandoned defer to run (before the later panic, which does not run it
again).

**The plain abort.** When a non-tail handler case returns, `dk_perform` reads
its sub-continuation's `ncopy`: every way a case can resume or keep `k` --
`dk_invoke`, `dk_resume_into`, the copy-on-store of `(set! m k)`, the
cloneable bridge of a shift receiver -- goes through `dk_copy_range`, so an
uncopied `sub` was neither resumed nor stored and the perform's original
frames up to its handler are dead.  `__dk_abort_abandon(k, H)` fires their
unwinders then, innermost first, before the handle's continuation runs; the
walk stops at `H` by address only, since a re-opening case can already have
handed the handler group back.  A generator's stored `k` (ncopy 1) and a
multi-shot case are untouched: their copies run their own defers when they
run.  The unwinder helper (`cps_pend_fire`, emit_cps_ir.c) now saves and
restores `tur_panicking` around the fires instead of forcing it set, so the
same helper serves the panic path and the abort path; a deferred body that
panics during an abort surfaces as an ordinary panic of the perform.

**The case that abandoned itself.** A case that re-opens an enclosing effect
runs with the real enclosing chain as its continuation, so when that outer
effect aborts, the rest of the program runs from inside the outer
`dk_perform`, and the inner perform's dead frames -- reachable only through
the C stack -- would fire when it unwound, after everything else (a server
loop never gets there).  `dk_perform` now keeps a per-thread list of the
performs whose case is running on the C stack (`DKPendPerform`, one record
per non-tail perform, on its own frame).  An abort walks down it: the record
below is dead exactly when the dead chain just fired passes through the
first node beyond its handler group (`dk_case_enclosing_real`), which is
where a re-opening case's continuation frames lead; a perform handled
inside the case (a nested handle in its body, a resumed copy's marker copy)
never reaches that node, and the case goes on.  A dead record whose `sub`
was copied stops the walk: its continuation lives on in the copy, and so
may everything beneath it.  The list is stack-disciplined like the driver
landing: every longjmp landing (the trampoline drivers, the three CPS entry
wrappers, the per-fiber panic landing) resets it to what it was when the
landing was armed, the fiber swap carries the fiber's own
(`FiberBlock.dk_pend`, swapped with the reap registry), and the c2mir front
end gets a host slot (`tur_tls_dk_pend_perform_ptr`).

**Escapes.** An `escape` landing fires the records the longjmp jumped over
(`__dk_escape_abandon`): a perform whose case invoked the escape is dead
unless the case copied its `k`.  Reaching it needed one more fix: an
escape receiver's `k` (a `CONT_ESCAPE` continuation, the landing's address
as the int64 carrier) failed the CPS signature gate (`fn_sig_ok`), which
made the receiver a permanently sig-rejected function and evicted every
function sharing an effect with it -- so a `handle` inside an escape
receiver was a hard "no lowering for perform" error.  The gate now admits
it as it admits a `serial-cont` (passed by value, consumed only by a plain
runtime call); fixture cases 18 and 19 cover the receiver installing the
handle and the continuation passed to a function that does.  Found on the
way and filed separately: an `escape` written directly in `main` is an
internal error
([escape-in-main-reaches-direct-emitter](../reported/escape-in-main-reaches-direct-emitter.md)).

**A deferred body that panics.** The fire helper (`cps_pend_fire`) runs
each body in its own `do { } while (0)` with the per-call-site signal check
spelled `break` (the stackless trampoline's `panic_signal_is_break` mode),
so a body that panics leaves only its own fire and the bodies declared
before it still run, LIFO, as a direct frame runs every closure of its
chain; the panic is raised again after the last one (fixture case 17).
The split runtime needed `g_dk_pend_perform` in the generator's TLS table
(`tools/gen-runtime-split.py`) so both halves share the one host slot, as
the driver landing does.
