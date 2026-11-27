# On wasm, a MIR-interpreted tail call from a function that allocated keeps its allocas and a frame

**RESOLVED 2026-11-24** in the fork as turmeric-lang/mir#22 (f4f9098, merged
as dc4be67), vendored the same day. This is the first fix direction below. On
wasm, `eval`'s frames and the `MIR_ALLOCA` blocks now come from a per-context
stack that the interpreter owns: 256 KiB chunks from the context's allocator,
with an explicit top. `MIR_INTERP_OWN_STACK` selects it, and it can be set to 1
on any target. The entry mark, the tail call's drop, `BSTART` and `BEND` read
or reset that top. A return resets it, and so does a return from `setjmp`,
which saves the top in a third reserved frame slot. Running out raises
`MIR_alloc_error` instead of corrupting memory, and a `BEND` of an address that
is not on that stack raises `MIR_invalid_insn_error`. Native targets compile the
same code as before.

The repro is pinned in the fork's `wasm-tests/run.sh`, the `wasm` job of its CI.
The tail-call chain test checks that the aggregate stays at one address and that
the linear stack pointer does not move. Against the unfixed interpreter it traps
at 100,000 calls on an 8 MiB stack. There are also tests for `bstart`/`bend` in
MIR text, `longjmp`, and frames across chunks.

**Severity:** medium for the planned wasm JIT (wasm-jit-plan; its engine is
the MIR interpreter), none today -- nothing ships the interpreter on wasm yet.
A tail-call chain through a function with any address-taken or aggregate
local grows the linear stack about 350 bytes per call. With `tur_wasm`'s
`-sSTACK_SIZE=16777216` (`src/CMakeLists.txt:2015`) the module traps after
about 24,000 calls, and a module that has trapped this way cannot be used again
(wasm-jit-plan section 5.8). Natively the same chain runs in constant stack.

Filed 2026-10-24. This is the wasm residue noted by the resolved
[mir-interp-tail-calls-grow-the-stack](../archive/mir-interp-tail-calls-grow-the-stack.md)
and in wasm-jit-plan section 5.8. Until now it was written down only in an
archived report and a plan paragraph, with no open finding to track it.

## Repro (measured, emcc 6.0.13, node 22)

Build `tools/wasm-jit-spike/spike.c` as `tools/wasm-jit-spike/run.sh` does,
with one extra native function, `sp_now`, that returns
`emscripten_stack_get_current ()`. It is passed to `MIR_load_external` and
declared in `hdr/spike.h`. Then interpret:

```c
static long long base;
struct S { long long x[4]; };
static void use (struct S *p) { p->x[1] = p->x[0]; }
int na (int n); int nb (int n);
int na (int n) { if (n == 0) return (int) (base - sp_now ()); __attribute__((musttail)) return nb (n - 1); }
int nb (int n) { __attribute__((musttail)) return na (n); }
int noalloc (int n) { base = sp_now (); return na (n); }
int wa (int n); int wb (int n);
int wa (int n) { struct S s; s.x[0] = n; use (&s); if (n == 0) return (int) (base - sp_now ()); __attribute__((musttail)) return wb (n - 1); }
int wb (int n) { __attribute__((musttail)) return wa (n); }
int withalloc (int n) { base = sp_now (); return wa (n); }
```

Stack growth, in bytes:

| n | `noalloc` | `withalloc` |
| --- | --- | --- |
| 10 | 1440 | 8784 |
| 1000 | 1440 | 705744 |
| 100000 | 1440 | `RuntimeError: memory access out of bounds` |

## Root cause

`eval` (`external/mir/mir-interp.c:1175`) allocates its frame with C `alloca`,
and so does `MIR_ALLOCA` (`:1790`). After an alloca, `frame_nregs = 0` (`:1795`)
forces the next tail call to run `bend_builtin (entry_sp)` and allocate a new
frame (`:1734`). On wasm, `bend_func` is a no-op (`mir-wasm.c:546`), so the
old frame and the allocas stay on the stack.

**A C-level `bend` cannot fix this.** I tried
`bstart = emscripten_stack_get_current`, `bend = _emscripten_stack_restore`
in a scratch copy. `bend` ran on every tail call (2020 calls for n=1000) and
the growth was unchanged. LLVM's wasm backend keeps a function's stack pointer
in a local and assumes callees leave `__stack_pointer` as they found it. So
`eval`'s next `alloca` starts from its own cached SP and writes the global
back, and the restore is lost. Natively `bend` is machine code that sets
`rsp`, and the alloca that follows reads `rsp` again. Wasm has no equivalent.
So the docs' "the interpreter cannot reset the stack pointer" is accurate: the
no-op stub is not just an omission.

## Fix directions

- On wasm (or everywhere, behind one macro), allocate `eval`'s frames and
  `MIR_ALLOCA` blocks from an interpreter-owned stack: a per-context arena
  with an explicit top. Then `bstart`/`bend` become "read / reset the arena
  top", which the interpreter controls, and a tail call can drop the caller's
  frame and allocas as it does natively. This also makes `MIR_BSTART`/`MIR_BEND`
  work on wasm. The arena needs an overflow check that reports an error rather
  than corrupting memory.
- Batch it into the fork's one open PR (CLAUDE.md "MIR fork" rule) with the
  rest of wasm-jit-plan P1's fork fixes.
- Pin it with the repro above in the spike (`withalloc` constant like
  `noalloc`).
