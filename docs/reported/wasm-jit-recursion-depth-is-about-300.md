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

### What the lowering wraps, read off the module

`wasm-dis` of a `--profiling-funcs` link: MIR's `eval` (`$eval_548` there)
has 72 call sites. 17 are `invoke_*` wrappers; the other 55 are plain calls,
every one to a compiler-rt helper LLVM knows cannot longjmp (the `long double`
soft-float and 128-bit shift routines, `__letf2`, `__addtf3`, `__ashlti3`,
...) or to `__handle_stack_overflow`. All six `invoke_iiiiii` sites are
`call_insn_execute`, so **every call instruction the interpreter executes
goes out to JavaScript and back**; the remaining eleven are `eval`'s other
calls out (the tail-call preparation, the error function, the builtins).
This is the JS hop wasm-jit-plan 5.5's call stubs were built to remove,
put back one level up by P5's `setjmp`.

(An aside, not followed up: the tree-walker's `eval_expr_impl`, in the same
module and in `turmeric.wasm`, has 335 wrapped calls, so the Interpret engine
pays the same hop per call. Its depth is heap-bounded since C4, so only its
speed is affected.)

### It costs speed too, not only depth

`(fib 30)`, about 2.7 million non-tail calls, `run.js --repeat 5`, the
steady-state runs of each module:

| | run_ms (runs 2-5, two module instances) |
| --- | --- |
| `main` | 485, 485, 486, 551; 398, 566, 437, 508 |
| `setjmp` out of `eval` | 269, 321, 266, 302; 292, 374, 328, 272 |

About 1.7x, or 70 ns a call. P5 recorded "no loop slowdown" from the change;
the loop it measured was a self tail call, which never leaves `eval` and so
never meets a wrapper.

## Fix directions

### Why the `setjmp` cannot simply move to a helper

Under Emscripten's SjLj a `longjmp` lands in the frame that called `setjmp`,
so that frame has to be alive until the function that called it returns. A
helper that does the `setjmp` and returns is gone by the time the `longjmp`
comes (the experiment above is exactly that, and is only sound because a plain
recursion never longjmps). A helper that does the `setjmp` and then *stays*
on the stack, running the rest of the function by re-entering `eval` at the
next instruction, keeps the semantics but nests one helper per `setjmp`
executed in one activation: a loop installing an effect handler N times
(`emit_effects.c`'s `TUR_SETJMP` per `handle`, `emit_dk_runtime.c`'s per
bounce) would grow the stack N deep where today it stays flat. Not that.

### Two `eval`s (the fix; a fork change)

Only a function whose body calls `setjmp` needs its calls wrapped, and MIR can
know which those are. On wasm, build the interpreter loop twice from the one
source: `eval_sj`, today's, for those functions; `eval`, with no `setjmp` in
it, for every other function, whose calls are then plain wasm calls. As read
from `mir-interp.c`:

- **The mark.** `generate_icode` turns a call whose callee operand is an item
  reference into `IC_IMM_CALL` (`mir-interp.c:447-456`); it runs lazily, at a
  function's first call (`interp_arr_varg`), after `MIR_link`, so the
  reference's address is resolved and compares against `setjmp_addr`, the
  marker `MIR_load_external` installs on wasm (`mir.c:1988-2001`). A
  `calls_setjmp` bit goes in `struct func_desc` (`:54`).
- **The choice.** `interp_arr_varg`, `MIR_interp_arr_varg` and `interp` call
  `eval` (`:2126`, `:2159-2161`, and `interp`'s); each picks by the bit. The
  direct-threaded dispatch table is filled by a `func_desc == NULL` call
  (`:2081`), once per copy.
- **Tail calls in the activation.** `IC_TCALL`/`IC_IMM_TCALL` run a callee in
  the current `eval` (`func_desc = callee_desc`, `:1727`, `:1739`); a callee
  whose bit differs from the running copy's takes the ordinary call instead,
  one more condition in `interp_tail_callee`.
- **`setjmp` through a register.** `MIR_CALL` checks `func_addr != setjmp_addr`
  at run time (`:1679`); in the plain copy that branch becomes an error.
  c2mir emits `setjmp` as a direct call, and emcc itself refuses an indirect
  `setjmp`, so nothing that compiles today is lost.
- **Native is untouched**: the duplication sits under the same
  `__wasm32__`/`__EMSCRIPTEN__` guard as `INTERP_SETJMP`, and native calls
  `setjmp` through its address with no wrappers to save.
- **Cost.** `eval` is 11.7k of the module's 2.69M `wasm-dis` lines, about
  0.4%; a second copy adds about that.
- **Expected.** The experiment is this design minus the `setjmp` copy, so:
  depth 300 -> 900 in Chromium's worker and 575 -> 1,800 under node, and
  calls about 1.7x faster. Asyncify keeps working: the prerequisite-3 spike
  unwound through these wrappers, and the fix leaves fewer of them.
- **Procedure.** turmeric-lang/mir, one PR at a time (CLAUDE.md); nothing is
  open or branched for it. Build it against a checkout with
  `-DTUR_MIR_SOURCE_DIR` first, as the experiment was; the fork's
  `wasm-tests/` and this repo's `tests/run-jit-wasm.sh` baseline are the
  checks, and the `setjmp`-using fixtures (effects, panics, the DK entry) are
  the ones the `eval_sj` copy must keep passing.

### What that leaves

- **Four wasm frames per interpreted call** stay: `eval` ->
  `call_insn_execute` -> the exact-signature stub -> `wasm_ff_dispatch` (with
  `MIR_interp_arr` inlined) -> `eval`. `call()` goes to the foreign-call
  interface for every callee and only `wasm_ff_dispatch`, two frames later,
  notices the thunk is an interpreted function (`mir-wasm.c:493-520`).
  Recognising it in `call()` and entering `MIR_interp_arr` there drops the
  stub and dispatch frames. Those are the small ones (`eval`'s spilled locals
  are the bulk), so the gain is unmeasured; a second, smaller step.
- **Wasm-native SjLj** (`-sSUPPORT_LONGJMP=wasm`) has no JS frames at all,
  but it needs `-fwasm-exceptions`, and emcc 6.0.10 warns that `ASYNCIFY=1`
  with wasm exceptions leaves "parts of the program that mix" them
  uncompilable (`emcc.py:486`); `eval` under the playground's Asyncify is that
  part. JSPI is the replacement for Asyncify that would allow it; its browser
  support was not checked here.
- Longer term, wasm-jit-plan 5.8's third response: heap-allocated interpreter
  frames, so depth is bounded by memory.
- Meanwhile the page says ~300 (`web/engine.js`).
