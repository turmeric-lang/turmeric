# On the browser JIT, a Turmeric recursion overflows about 300 calls deep, not 1,700

**Severity:** medium (browser JIT, user-visible). **Cause confirmed
2026-10-29** (below): `eval`'s own `setjmp` call, since mir#17. Try Turmeric's Compile
engine said "recursion depth ~1,700"; a plain non-tail recursion overflows at
about 310 calls in Chromium's worker and about 590 under node. A program that
recurses over a 400-element list works under Interpret and stops with
`stack overflow: recursion too deep` under Compile.

Filed 2026-10-29, found doing raylib-web-playground-plan prerequisite 3
(the Asyncify spike), which bisected the depth to see whether Asyncify
changes it. It does not.

## Measured

`(defn deep [n : int] : int (if (= n 0) 0 (+ 1 (deep (- n 1)))))`, then
`(println (deep N))`, emcc 6.0.10:

| Where | Last depth that ran | First that overflowed |
| --- | --- | --- |
| Chromium (Playwright's, headless), the page's `jit-worker.js` | 300 | 325 |
| node 22, `tools/wasm-jit/run.js`, the module on `main` | 575 | 600 |
| node 22, the same module before `-sSTACK_OVERFLOW_CHECK=2` | 575 | 600 |
| node 22, relinked with `-sASYNCIFY` | between 400 and 600, as without it | |

The 1,696 in wasm-jit-plan (sections 3.1, 5.8, 7.1) and the page's note were
measured on 2026-10-23 by the P1 pre-spike, on a C function (`int sum (int
n)`) run by MIR's interpreter, before P5.

## Why -- confirmed 2026-10-29

Since P5 (turmeric-lang/mir#17) the interpreter's `eval` calls `setjmp` in its
own frame on wasm (`INTERP_SETJMP`, `external/mir/mir-interp.c:47`). Under
Emscripten's JS-based SjLj (the link passes no `-fwasm-exceptions`), LLVM then
routes every call `eval` makes through an `invoke_*` wrapper, a JavaScript
frame, and a JS frame costs the JS stack far more than a wasm one.

The experiment changed that one line, nothing else: a copy of the vendored
MIR (built with `-DTUR_MIR_SOURCE_DIR=<copy>`) whose `INTERP_SETJMP` calls a
`noinline` helper that does the `setjmp`, so `eval` itself no longer calls it.
That breaks `setjmp` for a program that uses it (the helper's frame is gone
when the `longjmp` comes), but a plain recursion never does:

| | Frames per Turmeric call at the overflow (node's trace) | node 22 | Chromium worker, fresh page per depth |
| --- | --- | --- | --- |
| `main` | 5: `eval`, **`invoke_iiiiii` (JS)**, `call_insn_execute`, the stub, `wasm_ff_dispatch` | 575 ran, 600 overflowed | 300 ran, 325 overflowed |
| `setjmp` out of `eval` | 4, all wasm, no `invoke_*` | 1,800 ran, 1,900 overflowed | 900 ran, 1,000 overflowed |

So the wrappers cost two thirds of the depth, and without them a Turmeric
recursion gets the ~1,800 frames the pre-P5 C spike measured.

## Fix directions

The `setjmp` has to stay in the frame that runs the interpreted function that
called it, so a longjmp resumes that activation; it cannot simply move to a
helper. But only a few functions call `setjmp` at all (the effect and panic
landings, the DK entry), and every other activation pays for them. So:

- **Two `eval`s.** Build the interpreter loop twice from the same source:
  `eval` with `INTERP_SETJMP` for functions whose body calls `setjmp` (MIR
  knows from the call's import at load time), and an `eval` with no `setjmp`
  for everything else, whose calls are plain wasm calls. A recursion through
  ordinary functions then gets the 1,800 frames back. This is a fork change
  (turmeric-lang/mir, one PR at a time). Asyncify still works, since the
  remaining wrappers are the kind it handled in the prerequisite-3 spike.
- Wasm-native SjLj (`-sSUPPORT_LONGJMP=wasm`) has no JS frames at all, but
  Asyncify does not support it, and the raylib playground wants Asyncify. JSPI
  (JavaScript Promise Integration), which works with wasm exceptions, would
  replace Asyncify there; not evaluated.
- Longer term, wasm-jit-plan 5.8's third response: heap-allocated interpreter
  frames, so depth is bounded by memory.
- Meanwhile the page says ~300 (`web/engine.js`).
