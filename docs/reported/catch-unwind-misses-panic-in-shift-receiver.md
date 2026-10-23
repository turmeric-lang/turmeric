# Compiled, `catch-unwind` does not catch a panic in the receiver of a `shift` inside its thunk

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
