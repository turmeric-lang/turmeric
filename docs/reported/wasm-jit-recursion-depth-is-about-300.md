# On the browser JIT, a Turmeric recursion overflows about 300 calls deep, not 1,700

**Severity:** medium (browser JIT, user-visible). Try Turmeric's Compile
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

## Why, probably

Since P5 (turmeric-lang/mir#17) the interpreter's `eval` calls `setjmp` in its
own frame on wasm (`INTERP_SETJMP`, `external/mir/mir-interp.c:47`). Under
Emscripten's JS-based SjLj, LLVM then routes every call `eval` makes through
an `invoke_*` wrapper, a JavaScript frame. A trap's stack trace shows one per
interpreted call: `eval` -> `invoke_iiiiii` -> `call_insn_execute` -> stub ->
`wasm_ff_dispatch` -> `eval`. A JS frame costs the JS stack much more than a
wasm frame, so the depth fell. Not confirmed: the check is a module built from
the MIR before mir#17, with the same program.

## Fix directions

- Confirm the cause on a pre-mir#17 build (P4's module, 2026-10-28).
- If it is the wrappers: keep `setjmp` out of `eval` and in a small
  frame-owning helper that calls back into `eval` for the body, so only that
  helper's calls are wrapped. That is close to wasm-jit-plan 5.6's original
  `tur_rt_try` design, which P5 replaced. Wasm-native SjLj
  (`-sSUPPORT_LONGJMP=wasm`) has no JS frames, but Asyncify cannot be used with
  it, and the raylib playground wants Asyncify.
- Longer term, wasm-jit-plan 5.8's third response: heap-allocated interpreter
  frames, so depth is bounded by memory.
- Meanwhile the page says ~300 (`web/engine.js`).
