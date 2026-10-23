# Compiled, `catch-unwind` does not catch a panic in the receiver of a `shift` inside its thunk

**Resolved 2026-10-23 -- the interpreter was the engine that was wrong.** The
compiled backend runs a shift's receiver outside the part of its reset's body
that the shift cut off, the way a handler clause runs outside its handled
body: a panic the receiver raises retags the catch prompts in between dead
(`__dk_catch_kill`, `src/compiler/emit_dk_runtime.c`) and only a catch around
the reset takes it. `tur --interpret` agreed for handler clauses
(`cps-catch-unwind-across-perform` case 7) but applied the receiver where the
shift was, under the inner `catch-unwind`. So the repro below is an uncaught
panic on both engines now, and a `catch-unwind` around the `reset` catches it
on both.

The interpreter now does what the DK machine does. A reset records the catch
stack at its entry (`TuriResetBoundary.saved_catch`), and before a shift
applies its receiver it marks the catch boundaries registered since then dead
(`catch_kill_to`, `src/turi/eval.c`) -- for a `shift0` the boundaries above
the reset it removes. A panic looks past a dead boundary (`catch_top`) and a
dead `DK_CATCH_UNWIND` lets the signal through. The abort after the receiver
unwinds every one of them, so none is revived. When the reset is on another
catch stack (a shift reaching it from inside a handle's fiber) nothing is
marked, as before.

Pinned by `tests/fixtures/shift-receiver-panic-passes-inner-catch` (five
shapes, both engines).

## As filed

**Severity: low** (an uncaught panic where the interpreter catches it; a shape
nothing in the suites reaches). Found 2026-10-23 working
[shift0-pop-plan](../archive/shift0-pop-plan.md); not investigated.

```turmeric
(defn boom [v : int] : int (do (panic "boom") v))
(defn main [] : int
  (println (reset (let [r (catch-unwind (fn [] : int (shift boom 1)))] (if (err? r) 1 0))))
  0)
```

`tur --interpret` prints `1`. `tur run` stops with `panic at ...: boom`. A
`catch-unwind` whose thunk calls `boom` directly catches it on both engines,
inside a `reset` or not. `shift0` behaves the same as `shift` here.

The thunk lambda is CPS-colored (`TUR_TRACE_EVICT=1`: `[E2-COLOR] __fn_5 ...
lambda`), so its `shift` runs on the DK machine; the receiver's panic likely
unwinds from inside `dk_run_impl` without reaching the `catch-unwind` landing.
Start there.
