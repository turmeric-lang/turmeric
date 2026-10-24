# The wasm link has no stack-overflow check, so a linear-stack overflow corrupts memory instead of stopping

**Severity:** medium for the planned wasm JIT (wasm-jit-plan); unknown for
today's `tur_wasm`, which has the same link posture. `tur_wasm` links at `-O2`
with `-sSTACK_SIZE=16777216` and no `-sSTACK_OVERFLOW_CHECK`
(`src/CMakeLists.txt:2015-2028`). At `-O1` and above that means no SP bounds
check at all: an overflow writes past the bottom of the 16 MiB stack.

Filed 2026-10-24. An agent concluded that this is not reportable: "under the
REPL module's 16 MiB stack the JS engine's limit binds first", so an unchecked
overflow only matters "if the JS limit is ever raised". That holds for one
shape only. Its suggested remedy, linking with the check on, is right, and
measured below.

## Measured (vendored MIR interpreter under emcc 6.0.13 / node 22, the `tools/wasm-jit-spike` driver, 16 MiB stack)

| Program shape | Without the check | `-sSTACK_OVERFLOW_CHECK=2` |
| --- | --- | --- |
| Plain non-tail recursion, small frames | JS `RangeError` near depth 1,800, using 2.7 of 16 MiB. The JS limit binds first, as claimed | same |
| Non-tail recursion with a 16 KiB local array | `memory access out of bounds` near depth 1,000, *before* the JS limit; the module is dead afterwards | `Aborted(stack overflow (Attempt to set SP to ...))` |
| Tail-call chain through a function with a local aggregate ([wasm-mir-interp-tail-call-keeps-allocas](wasm-mir-interp-tail-call-keeps-allocas.md)) | `memory access out of bounds` after about 24k calls | `Aborted(stack overflow ...)` |
| `printf` in a loop, 2e6 iterations ([c2mir-wasm-vararg-buffer-allocas-per-call](c2mir-wasm-vararg-buffer-allocas-per-call.md)) | `null function or function signature mismatch`: memory was corrupted before anything trapped | `Aborted(stack overflow ...)` |

So the linear stack is reachable today, by anything that grows it without
nesting a JS frame per step: large frames, allocas kept by tail calls, and
per-call va buffers. Without the check, the result ranges from a dead module to
silently corrupted state.

## Fix directions

- Link the wasm JIT engine with `-sSTACK_OVERFLOW_CHECK=2`. Binaryen then
  checks every `__stack_pointer` write, including `eval`'s dynamic allocas. The
  abort is catchable on the page, which then rebuilds the module (wasm-jit-plan
  5.8 already rebuilds after a `RangeError`). Measure what it costs on the
  spike's per-iteration benchmark before deciding to keep it on in release.
- Decide the same for today's `tur_wasm`. It runs the reader, elaborator,
  formatter and LSP on user input in the browser. Whether any of that recurses
  deeply enough on the linear stack to reach 16 MiB is not established here.
- The two sibling reports remove two of the three causes. The check is what
  turns whatever is left into an error message.
