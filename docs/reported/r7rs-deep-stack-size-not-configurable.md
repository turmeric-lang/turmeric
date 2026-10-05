# `#lang r7rs`: the deep stack's size is fixed at 1 GiB

**Severity:** low. **Depends on PR #1082** (not on `main` when filed). After
#1082, a compiled `#lang r7rs` program runs `main` on a thread with a 1 GiB
stack on 64-bit hosts (64 MiB on 32-bit). The only setting is on or off
(`TUR_NO_DEEP_STACK=1`). There is no way to choose the size, although the JIT
already has one (`TUR_JIT_STACK_MB`). Filed 2026-10-05 reviewing #1082.

## Why it matters

- **A recursion that never ends uses the full 1 GiB of memory before it
  stops.** The 1 GiB is reserved address space, but a runaway recursion
  touches every page on its way down, so it is committed memory by the time
  the overflow message prints. On a small machine, a CI runner, or a
  container with a memory limit, that can mean swapping or the OOM killer
  instead of the message. A student running SICP with a missing base case is
  the expected case.
- **A program that needs more than 1 GiB has no way to get it,** short of
  editing the emitted C.
- **Two names for one setting.** The cc path and the JIT pick the same
  default but only the JIT lets you change it, under a JIT-specific name.

## Repro (after #1082)

```scheme
#lang r7rs
(import (scheme base) (scheme write))
(define (down n) (+ 1 (down (+ n 1))))
(write (down 0))
```

Built and run with `tur build` on Linux: peak RSS approaches 1 GiB before
`stack overflow: recursion too deep` and exit 139. Not measured when filed;
the PR states that reaching the message "commits the full 1 GiB", which is
why it has no fixture for the message.

## Where

`emit_deep_stack_runtime` in `src/compiler/emit_module.c` (#1082): the size
is `sizeof(void *) >= 8 ? ((size_t)1 << 30) : ((size_t)64 << 20)`, in both the
pthread and the Windows `CreateThread` branches. The JIT's default and its
`TUR_JIT_STACK_MB` override are in `tur_jit_execute`, `src/jit_engine.c`.

## Fix directions

- Read one environment variable, for example `TUR_STACK_MB`, in
  `tur_deep_enter`, and have the JIT honor it too, with `TUR_JIT_STACK_MB`
  kept as an alias. Keep `TUR_NO_DEEP_STACK=1` as the off switch, or make
  `TUR_STACK_MB=0` mean off and retire it.
- Optionally a build-time default (a `build.tur` key or a `tur build` flag)
  for a program that knows what it needs.
- Consider a smaller default for runaway safety (256 MiB still runs 1e6
  frames comfortably), and measure what depth SICP-style programs reach before
  choosing.
- Document the setting next to the limit in `docs/guides/sicp-guide.md`
  ("When something goes wrong") and the r7rs guide.

If the thread cannot be created with the requested size (for example under
`vm.overcommit_memory=2` or a low `RLIMIT_AS`), #1082 falls back to running
`main` where it is. A configured size should keep that fallback and say on
stderr that it happened, so that a too-large setting does not quietly turn
into the default 8 MiB stack.

See also `docs/upcoming/hold/r7rs-heap-frames-plan.md`, where this setting
becomes the cap on a stack that grows on demand.
