# On the browser JIT, a Turmeric recursion overflows about 300 calls deep, not 1,700

**Severity:** medium (browser JIT, user-visible). **Cause confirmed
2026-10-29** (below): `eval`'s own `setjmp` call, since mir#17. **Fixed 2026-10-30**:
`tools/wasm-jit/build-module.sh` links `setjmp` on wasm exception handling
(`-fwasm-exceptions -sSUPPORT_LONGJMP=wasm`), which removes the wrappers with
no MIR change: 300 -> 700 in the worker, 575 -> 1,100 on a main thread. The
page's note says ~700. Open here: four wasm frames per interpreted call
remain, and the Interpret module (`turmeric.wasm`) still links the JS
`setjmp`; see "What to do". JSPI stands in for Asyncify where the playground
needs a blocking loop to yield. Try Turmeric's Compile
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

### The link mode: wasm-native `setjmp` (measured 2026-10-30, recommended)

The wrappers exist because the module links with Emscripten's JS-based
`setjmp` (`SUPPORT_LONGJMP=emscripten`, the default without
`-fwasm-exceptions`). The other mode, `setjmp`/`longjmp` on wasm exception
handling, has no JS frames at all, and it needs no change to MIR. Built as an
experiment from a patched copy of `tools/wasm-jit/build-module.sh` --
`-fwasm-exceptions` on every compile (the `rt`, `engine` and `mir` groups, the
import table and the stubs) and `-fwasm-exceptions -sSUPPORT_LONGJMP=wasm` on
the link, nothing else -- emcc 6.0.10, legacy wasm EH (its default), the same
objects otherwise:

| Depth, last that ran / first that overflowed | today (JS `setjmp`) | wasm `setjmp` |
| --- | --- | --- |
| node 22 | 575 / 600 | 1,500 / 1,800 |
| Chromium 153, main thread | 575 / 600 | 1,100 / 1,200 |
| Chromium 153, Web Worker (Try's `jit-worker.js`) | 300 / 325 | 700 / 800 |

- **Calls.** `fib 30` under node 22: 444-549 ms today, 335-398 ms with wasm
  `setjmp` (about 1.4x; the `try` regions cost a little against the 1.7x of
  the no-`setjmp` experiment). **Under Chromium 153 there is no measurable
  difference** (337-450 against 350-412 ms, two instances each): that V8
  makes a wasm-to-JS-to-wasm call cheap enough that the wrappers only cost
  stack there. So the speed finding above is V8-version dependent; the depth
  finding is not.
- **Size.** 5.46 -> 5.54 MB (+1.5%).
- **Correctness.** `tools/wasm-jit/smoke/eff.tur` prints 42 (the effect
  landing is a wasm-EH `setjmp` now); `check-examples.sh`: all 19 Try examples
  right; a 715-fixture slice of the corpus (effects, handlers, panics, the DK
  entry, continuations, exits) through `tests/run-jit-wasm.sh`: 612 compiled,
  10 fell back, 54 failed, **no row better or worse than the baseline**.
- **One behaviour change.** The linear-stack check (`-sSTACK_OVERFLOW_CHECK=2`)
  still fires and still prints `Aborted(stack overflow (Attempt to set SP ...`
  on stderr, but the exception JavaScript sees is `RuntimeError: unreachable`,
  not the `Aborted(...)` error. `web/public/jit-worker.js` classifies by the
  exception's message, so it would read the stderr line instead (it already
  intercepts that line).
- **What it rules out.** Asyncify, which cannot be used with wasm exceptions
  (`emcc.py:486`). Try Turmeric's Compile engine links no Asyncify today, so
  for it this is a free change: worker depth 300 -> 700, and every call out of
  `eval` a plain wasm call. The raylib playground's blocking frame loop
  needed Asyncify; the next subsection is its replacement.

### JSPI in place of Asyncify (measured 2026-10-30)

JavaScript Promise Integration suspends the wasm stack at a suspending
import and resumes it when the promise settles, with no instrumentation of
the wasm. Emscripten's `-sJSPI -sJSPI_EXPORTS=turi_wasm_jit_run` makes that
export return a Promise and marks `emscripten_sleep` suspending (`ASYNCIFY=2`
in `src/lib/libasync.js`). On the wasm-`setjmp` build above, with
`emscripten_sleep` added to the import table as the Asyncify spike did:

- The `.wasm` is **byte-for-byte the same size** as without JSPI (5.54 MB;
  only the JS differs). Asyncify made it 8.31 MB.
- `tools/wasm-jit-spike/asyncify/while.tur` (a `while` over `^mut` state
  sleeping 16 ms a frame from inline C) and `frames.tur` **run and yield**,
  in Chromium 153 on the main thread and inside a Web Worker, printing the
  right values; `eff.tur` still prints 42.
- Depth and call speed are those of the wasm-`setjmp` build; JSPI adds
  nothing (1,100 / 1,200 main thread, 700 / 800 worker, `fib 30` within noise).
- **It needs the wasm-`setjmp` objects.** Today's objects relinked with
  `-sJSPI` fail the first sleep with `SuspendError: trying to suspend without
  WebAssembly.promising`: the `invoke_*` JS frame between `eval` and the sleep
  breaks the chain JSPI suspends. The same rule means a sleep inside a
  host-to-interpreted callback (a comparator, a frame function called through
  the table's JS closure, `mir_wasm_set_interp_entry`) would fail the same
  way; the blocking style sleeps from `main`, entered through
  `MIR_interp_arr` with no JS frame, and is fine. So an Asyncify module and a
  JSPI module are two object sets, not two links.
- **Support, from web sources on 2026-10-30** (emscripten.org is not reachable
  from this session's network, so its JSPI page was not read): Chrome ships
  JSPI by default since 137 (May 2025); Firefox has it behind a flag since 131
  and an Intent to Ship for 153; Safari 27 beta has it. node 22's V8 12.4 has
  only the older `Suspender` API, so `tests/run-jit-wasm.sh` cannot drive a
  JSPI module; Playwright's Chromium 153 can.
  Sources: [The State of WebAssembly 2025-2026](https://platform.uno/blog/the-state-of-webassembly-2025-2026/),
  [SQLite persistence on the web, May 2026](https://powersync.com/blog/sqlite-persistence-on-the-web),
  [State of WebAssembly 2026](https://devnewsletter.com/p/state-of-webassembly-2026/),
  [a project's Firefox and Safari support issue, October 2026](https://github.com/RichardAtCT/simgolf-web/issues/4).

So for the playground: JSPI on the wasm-`setjmp` build gives the blocking
style with no Asyncify cost, Chrome-first; other browsers need either the
Asyncify build (a second module) or the callback style. That choice is
raylib-web-playground-plan's, recorded there under prerequisite 3.

### What to do, in order

1. **Switch `tools/wasm-jit/build-module.sh` to wasm-native `setjmp`** (the
   four flag sites above), re-run the corpus for a new baseline, and teach
   `jit-worker.js` the `unreachable`-plus-stderr shape of the stack abort.
   No fork change; Try's Compile engine gets 700 frames in the worker.
   **Done 2026-10-30** (`SJLJ_CFLAGS` in the script; the worker notes the
   stderr line; `jit-engine.spec.js` 7/7, the examples gate, and the corpus
   baseline regenerated on the new module). Not done, and the same change
   would apply: `turmeric.wasm`, the Interpret module, whose tree-walker's
   `eval_expr_impl` carries 335 of the same wrappers (depth is heap-bounded
   there, so only its call speed under older V8s is at stake).
2. The playground decides Asyncify (second object set) against JSPI
   (Chrome-first) for the blocking style, or goes callback-first.
3. The two-`eval` fork change below is then unnecessary. It stays recorded in
   case a JS-`setjmp` link is ever needed again.
4. Longer term, heap-allocated interpreter frames (wasm-jit-plan 5.8's third
   response), since four wasm frames per interpreted call remain either way
   (`eval` -> `call_insn_execute` -> the stub -> `wasm_ff_dispatch` -> `eval`;
   recognising an interpreted thunk in `call()` before the foreign-call
   interface would drop two, the small ones).

### Two `eval`s (a fork change; needed only while the link keeps the JS `setjmp`)

If the module had to stay on Emscripten's JS `setjmp`, this is the fix inside
MIR. Only a function whose body calls `setjmp` needs its calls wrapped, and MIR
can know which those are. On wasm, build the interpreter loop twice from the one
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

Meanwhile the page says ~300 (`web/engine.js`).
