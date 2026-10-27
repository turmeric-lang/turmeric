# `tur dap` and `tur trace` only instrument `(main)`; top-level work is invisible

**Status: RESOLVED 2026-09-02** -- see the Resolution at the end.

**Severity: medium** (no wrong answers, but the two debugging tools report
nothing for a whole shape of program, and `setBreakpoints` actively claims
otherwise -- see "The part that misleads").

**Status:** OPEN. Filed 2026-09-01 while building Trowel's debugger UI, from a
user hitting it head-on: a breakpoint set in the gutter, the program run, and
nothing happened. Found on macOS/arm64 against the staged `tur` at v0.42.1.

## Summary

Both interpreter-side debugging tools attach around the `(main)` call and
nothing else. A file whose work happens at the **top level** therefore:

- runs to completion under `tur dap` with **no `stopped` event at all** — not
  for a breakpoint, and not even for `stopOnEntry`;
- records **0 steps** under `tur trace`.

This is already known for the tracer — `docs/guides/time-travel-tracing-guide.md`
and the Try Turmeric plan both note that a program with no `main` records
nothing, and Try Turmeric works around it by checking for a top-level `main`
and calling `(main)` itself after loading a file's forms. What this report adds
is that **`tur dap` has the identical hole**, that it is silent rather than
merely empty, and that top-level code is not an exotic shape: it is what every
example in `web/tour/index.html` looks like, and what the REPL evaluates.

## Repro

Two files differing only in whether the body sits inside `main`.

`toplevel.tur`:

```turmeric
#lang turmeric

(defeffect Ask [] : int)

(defn use-ask [] : int
  (+ 1 (perform (Ask))))

(println (handle (use-ask)
  (Ask [] k) (resume k 41)))
```

`withmain.tur` — same, with the last form wrapped:

```turmeric
(defn main [] : int
  (println (handle (use-ask)
    (Ask [] k) (resume k 41)))
  0)
```

Drive `tur dap` over each with `stopOnEntry: true` **and** a breakpoint on line
6 (`(+ 1 (perform (Ask))))`, inside `use-ask`, which both files call):

```
withmain.tur  bp@6: events=['stopped']                        stopped=True
toplevel.tur  bp@6: events=['output','exited','terminated']   stopped=False
```

The tracer half is the already-documented behaviour: `tur trace toplevel.tur`
prints the program's output and summarises `0 steps`.

## The part that misleads

`setBreakpoints` answers **`verified: true`** for the top-level file:

```json
{"breakpoints": [{"verified": true, "line": 6}]}
```

An editor draws that as a bound, solid breakpoint. So the user sees a
breakpoint the adapter has confirmed, runs the program, and gets silence — which
reads as *the debugger is broken*, not as *this program cannot be debugged*.
That is the difference between a limitation and a bug, and `verified` is what
turns one into the other. Compare the tracer, which at least reports `0 steps`
and gives a client something to explain.

Even without the larger fix below, **reporting `verified: false` when the
source defines no top-level `main`** would let every client say the right thing.
`stopOnEntry` silently doing nothing is the same problem in a second place: a
client that asked to stop at entry got no stop and no error.

## What "fixed" would look like

Options, roughly in order of cost:

1. **Report honestly.** `verified: false` on breakpoints that cannot bind, and
   an `output`/error for a `stopOnEntry` that will not happen. Cheap, and it
   turns a silent failure into an explained one.
2. **Instrument top-level forms.** Attach the debugger/recorder around the
   whole load rather than around `(main)`, so top-level evaluation steps like
   any other. This is the real fix and makes `tur dap` match what a user means
   by "debug this file".
3. **Synthesize the entry.** What Try Turmeric does: if the file defines
   `main`, call it after loading; otherwise treat the top-level forms as the
   program. Note this only helps the *has-main* direction and does not by
   itself make top-level forms steppable.

(1) and (2) are independent — (1) is worth doing even if (2) lands, because a
file can still be un-debuggable for other reasons.

## Notes for whoever picks this up

- The relevant code is `dap_begin_session` / `dap_run_program` in
  `src/turi/dap.c`, which arm the debugger around the launch, and the recorder
  path in `src/turi/eval.c` that `turi_trace_begin` hooks.
- `tests/fixtures/dap/input.tur` and `tests/fixtures/dap-replay/input.tur` both
  define `main`, so the whole DAP regression suite runs on the working side of
  this line and would not catch a regression on the other side. A top-level
  fixture asserting "no stop, and here is why" would pin it.
- Downstream, Trowel now checks the source for a top-level `main` before
  launching a session and says so, sharing the rule its tracer already used.
  That is a workaround in a client, not a fix — every other DAP client
  (VS Code, nvim-dap) still gets the silent version.

## When this lands, two downstreams have workarounds to retire

Both are compensating for this defect, and both will be *wrong* rather than
merely redundant once top-level forms become steppable — they will keep
refusing or warning about programs that have by then become debuggable.

- **Try Turmeric** (`web/main.js`, `web/public/eval-worker.js`): checks for a
  top-level `main` before running `(main)` after loading the forms, and the
  Trace path reports "recorded nothing" for the no-`main` case. If (2) or (3)
  above lands, revisit that check and the message that goes with it.
- **Trowel** (`src/trace/trace_runner.{h,cpp}` `DefinesMainEntry` and its caller
  in `src/app/main_window.cpp`): warns before launching a debug session that a
  top-level file will not stop, and `TraceOutcome::NoMain` explains the same
  thing for tracing. Both messages tell the user to wrap their code in
  `(defn main [] …)`, which becomes bad advice the day this is fixed.

If (1) alone lands — honest `verified: false` — neither workaround becomes
wrong, but both can lean on the adapter's own answer instead of re-deriving it
from the source text, which is the more robust place for it to live.

## Resolution (2026-09-02)

Fix direction (2), the real one. `cmd_eval_h` (`src/main.c`, the launch path
both `tur dap` and `tur trace` go through) attached the debugger and let the
embedder install its pause handler BEFORE the user file loaded, but only
*armed* it right before `(main)` -- so a top-level program's forms ran with
the debugger idle: no entry stop, no breakpoints, `0 steps`. It now pre-scans
the file for a top-level `main` (`file_defines_main`: `(defn main` or the
sweet-exp `defn main` at a line start -- the rule Try Turmeric and Trowel
already re-derived client-side) and, when there is none, arms the debugger
around the load itself: the entry stop lands on the program's first located
form (`<top> input.tur:3`), a breakpoint inside a function the top level calls
fires, and the recorder records the steps. A file WITH a `main` keeps arming
right before `(main)`, so its definitions still load without stopping and the
existing DAP transcript (entry frame `main`) is unchanged. The wasm glue
(`turi_wasm_trace_run`) already had exactly this split; the CLI was the hole.

Direction (1) -- `verified: false` on the top-level file -- is moot: the
breakpoint the report shows as falsely verified now genuinely binds.

Pinned by `tests/fixtures/dap-toplevel` (the report's `toplevel.tur`) driven
by `tests/dap-toplevel-driver.py` from `tests/run-dap.sh` (entry stop at the
first form, breakpoint in `use-ask` line 6, output `42`, exit 0: 68/0), and
by `tests/run-trace.sh` (a top-level program records 12 steps, not 0: 36/0).
The time-travel tracing guide states the rule.

Downstreams: Try Turmeric's Run/Trace rule (load, then `(main)` if defined,
else the top-level forms are the program) is the same rule and stays. Trowel's
`DefinesMainEntry` warning before a debug session ("a top-level file will not
stop") is now wrong and should be retired; its `TraceOutcome::NoMain` likewise.
