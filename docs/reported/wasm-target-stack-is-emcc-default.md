# `tur build --target wasm` runs the program on emcc's 64 KiB default stack, unchecked, with no tail calls

**Severity:** medium-high -- silent memory corruption class on the web
target, no diagnostic. Found 2026-10-23 measuring recursion for
[wasm-jit-plan](../upcoming/wasm-jit-plan.md); the AOT target is
[wasm-spices-plan](../upcoming/wasm-spices-plan.md)'s.

## What happens

Three things the native build gives a program, the wasm build does not,
and nothing says so:

1. **The stack is emcc's default, 64 KiB.** `tur build --target wasm` links
   with `-O2 -std=c99 -Wall -fno-strict-aliasing -s WASM=1`
   (`src/driver/cmd_build.c`, `cc_flags`) plus what
   `pkg_web_compose_link_flags` composes from the `:web` block
   (`src/compiler/pkg.c:4448`): `EXIT_RUNTIME`, `MODULARIZE`, `EXPORT_NAME`,
   `INITIAL_MEMORY`, the GL/audio/asyncify/thread flags. No `-sSTACK_SIZE`
   anywhere (`TUR_SHOW_CC=1` shows the line). emcc 6.0's default is
   `STACK_SIZE = 64*1024` (`emscripten/src/settings.js:113`). The REPL
   module sets 16 MiB (`src/CMakeLists.txt`, `tur_wasm`); the AOT program
   gets 64 KiB.
2. **Overflow is unchecked.** `STACK_OVERFLOW_CHECK` defaults to 0. The
   linear stack overflows into whatever sits below it; the program then
   dies somewhere else, or does not.
3. **The 1 GiB entry thread is compiled in and silently does nothing.**
   `emit_deep_stack_runtime` (`src/compiler/emit_module.c:14647`) is guarded
   `#if !defined(_WIN32) && !defined(__MIRC__)`, so the wasm program carries
   it: `pthread_create` with a 1 GiB stack, which under a non-`-pthread`
   emcc build fails (and under `:threads true` would ask for a 1 GiB wasm
   thread stack). By design "a failure to make the thread is not an error
   -- main just runs where it is", so `main` runs on the 64 KiB stack with
   no line on stderr.
4. **No tail calls.** `TUR_MUSTTAIL` is defined empty under `__wasm__`
   (`emit_module.c:10996`), so a mutual tail-call cycle recurses. clang does
   honour `musttail` on wasm with `-mtail-call`, and every evergreen browser
   ships the tail-call feature. See also
   [jit-musttail-expands-to-nothing](jit-musttail-expands-to-nothing.md).

## Repro

A frame that lives in linear memory (1 KiB per level, live across the
recursion), 200 deep, compiled by emcc 6.0.13 and run under node 22. This
is plain C rather than Turmeric because a Turmeric `defn`'s locals are wasm
registers, so a 100k-deep `(+ 1 (f (- n 1)))` hits the JS engine's frame
limit (between 10,000 and 30,000 frames, a `RangeError`) before it touches
the linear stack; an inline-C body with a local array, a by-value
aggregate, or anything `alloca`-shaped is what reaches it.

| build | 200 levels |
|---|---|
| default (what `tur build --target wasm` passes) | `RuntimeError: memory access out of bounds` -- after writing 136 KiB below the stack base |
| `-sSTACK_OVERFLOW_CHECK=2 -sASSERTIONS=1` | `Aborted(stack overflow (Attempt to set SP to 0x00001350, with stack limits [0x000013e0 - 0x000113e0]))` -- the 64 KiB is visible in the limits |
| `-sSTACK_SIZE=16MB` | `rec(200) = 444663` |

`tools/wasm-jit-spike/` has the emcc command lines; the C is ten lines
(`rec` with `volatile char buf[1024]`, `noinline`, a non-associative use of
the result so LLVM cannot turn it into a loop).

For comparison the same non-tail Turmeric recursion natively: `tur run`,
`tur jit` and `tur interpret` all return at 1,000,000 deep (the 1 GiB
entry thread, and the tree-walker's heap work-stack).

Tail calls: a `noinline` mutual recursion 50 million deep with
`__attribute__((musttail))` runs to completion with `-mtail-call` and dies
with `RangeError: Maximum call stack size exceeded` without it.

## Fix directions

- Compose `-sSTACK_SIZE` in `pkg_web_compose_link_flags`, default 16 MiB
  like `tur_wasm`, with a `:web :stack <bytes>` key; put the same default
  in the bare `cc_flags` for a project with no `:web` block.
- `-sSTACK_OVERFLOW_CHECK=1` whenever the build is not `--release` (it is a
  bounds compare on `sp` moves; the cost is small and the alternative is
  the first row above).
- Guard `emit_deep_stack_runtime` with `!defined(__EMSCRIPTEN__)` too, or
  have it set nothing and say nothing on that target; the stack is a link
  flag there, not a thread.
- Lift the `__wasm__` exclusion from `TUR_MUSTTAIL` behind `-mtail-call`,
  composed from a `:web :tail-calls true` default. If a pre-2023 browser
  matters it is one key to turn off.
- Longer term, [r7rs-heap-frames-plan](../upcoming/r7rs-heap-frames-plan.md)
  names "a target without a 1 GiB stack" as what lifts its hold; this
  target is one.
