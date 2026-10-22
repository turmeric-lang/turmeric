# A CPS function's self call from its own join frame grows the C stack

**RESOLVED 2026-10-12** -- see [Resolution](#resolution-2026-10-12). Pinned by
`tests/fixtures/cps-join-self-call-unoptimized`, built at `-O0` by its
`hook.sh`.

**Severity: medium (stack overflow below `-O2`).** A self-recursive loop
lowered through the CPS backend, whose self call sits in the continuation of
an earlier CPS call in the same step, makes a C call per step. It survives at
the default optimization level only because the C compiler turns those calls
into sibling calls; at `-O0` and `--debug` (`-Og`) it overflows the stack.

Found while fixing
[self-tco-refuses-fn-typed-params](../archive/self-tco-refuses-fn-typed-params.md):
that fix covers a callback loop on the direct path, and this is the same loop
when the callback's result feeds the next step.

## Repro

```turmeric
(defn sum-to [^fat f : (fn [int] int) i : int n : int acc : int] : int
  (if (>= i n)
    acc
    (sum-to f (+ i 1) n (+ acc (f i)))))

(defn main [] : int
  (println (sum-to (fn [x : int] : int (mod x 3)) 0 10000000 0))
  0)
```

`tur run` prints `9999999`. `tur run --debug` (or `TUR_CC_FLAGS=-O0
tur build`) segfaults. `^tailcall` on the self call is refused with "the
enclosing function is lowered through the CPS backend".

## Root cause

The call through the `^fat` callback colors `sum-to`, and `(f i)` is not in
tail position, so the CPS backend gives it a heap join and the self call ends
up inside that join, not in `sum-to__cps`'s own body:

```c
static int64_t sum_hyto__cps(int64_t f, int64_t i, int64_t n, int64_t acc, DK *__kont) {
    ...
    /* call f with a resume-join continuation */
    return ((int64_t (*)(void *, int64_t, DK *))__e2af)((void *)__e2ab, i,
               ... dk_frame_resume_join(sum_hyto_j0, env, __kont) ...);
}
static intptr_t sum_hyto_j0(intptr_t env, intptr_t __t3__slot, DK *__kont) {
    ...
    return sum_hyto__cps((int64_t)(intptr_t)f, __t1, n, __t2, __kont); /* cps->cps */
}
```

Each step is `sum-to__cps -> f -> dk_run -> sum-to_j0 -> sum-to__cps`, every
link a C call in tail position. The CPS self backedge
(`__tur_cps_self`, docs/archive/cps-self-tail-call-relies-on-sibling-call.md)
only applies to a self call in the function's OWN body that hands on its own
`__kont`; this one is in a different C function (the join), so nothing but the
C compiler's sibling-call optimization keeps the stack flat.

Calling the callback for its effect, with its result not feeding the call --
`(do (f i) (each f (+ i 1) n))` -- is CPS-lowered the same way. A plain
`(fn [int] int)` or rank-2 callback is not CPS-lowered at all, and since
self-tco-refuses-fn-typed-params it gets a real backedge; a `^fat` callback
loop that the CPS backend declines (one that also calls inline C, as
turmeric-spices' buffer fill did) takes the direct path and gets one too.

## Fix directions

1. **Run the join's self call as a bounce.** A join whose body ends in a
   self call to its own enclosing function could return a "call me" record
   to `dk_run` instead of calling, so the driver loops. That is the
   trampoline the r7rs mutual-tail fix uses (T5/T6 in
   docs/archive/proper-tail-calls-plan.md), applied to the
   function-to-own-join edge.
2. **Fold the join back into the function.** When the only non-tail CPS call
   in a step is the callback, and its continuation ends in the self call,
   the join could jump to `__tur_cps_self` with the delivered value instead of
   being its own C function -- the continuation is single-shot and the
   function's frame is dead by then.
3. **Do not color for a callback with an empty effect row.** Today even
   `^fat f : (fn [int] #fx{} int)` CPS-lowers `sum-to` (checked: `^tailcall`
   is still refused for the CPS body). If a callback whose type says it
   performs nothing were called directly, the loop would take the direct path,
   where the backedge already exists. That only helps callers that annotate
   the row.

## Workaround

Write the loop with `while` and `^mut` locals, as turmeric-spices'
`signal/buffer` does, or take the callback as a plain `(fn [int] int)` rather
than `^fat`, which is not CPS-lowered and now gets a backedge.

## Resolution (2026-10-12)

Fix direction 1, generalized: not only the join's self call but **every
cps->cps tail call that hands on `__kont`** -- a self call from a lifted join,
a call to a function outside a fused group, a closure's `__cps`, an r7rs named
let or internal `define` looping through a procedure variable -- is now a
proper tail call at every `-O`.  The mechanism is a stack-guarded bounce to
the trampoline driver (Cheney-on-the-MTA style), so a flat stack pays nothing
and a growing one is cut every 64 KiB.

**Runtime** (`emit_dk_runtime.c`, after `dk_tail_resume`):

- `__dk_tail_deep()` measures how far the C stack has grown below the
  trampoline driver's landing.  The landing's `tur_jmp_buf` is a local of
  that frame, so `g_dk_driver` *is* an address on it: no new per-thread
  state, nothing for the JIT's host TLS table or the fiber swap to carry.
  With no driver installed it is false.  `TUR_DK_BOUNCE_DEPTH` (default
  64 KiB, `-D`-overridable) is small against the smallest stack a CPS loop
  runs on (a 1 MiB fiber) and large against one step (~200-300 B at `-O0`).
- `__dk_bounce(node)` marks the node `inplace_head` (the driver neither frees
  nor keeps it), publishes it as the resume chain and longjmps to the
  driver, exactly as a tail resume does.

**Emitter** (`emit_cps_ir.c`, the cps->cps arm of `CT_TAILCALL`): where the
self backedge and the group jump do not apply, a `__kont`-threading tail call
to a callee whose `FnDef` is known is emitted as

```c
if (__dk_tail_deep()) {
    f_b1_env *__tb = malloc(sizeof *__tb);
    __tb->node = dk_frame_resume(f_b1, (intptr_t)__tb, __kont);
    __tb->a0 = ...; __tb->a1 = ...;
    return __dk_bounce(__tb->node);
}
return f__cps(..., __kont); /* cps->cps */
```

with a per-site thunk `f_b1(env, v, __kont)` in the helpers that reads the
arguments back, frees the env and the node, and makes the call.  The driver
runs the node; `dk_run_impl`'s `DKK_RESUME_FRAME` arm tail-calls the thunk;
the call is made from a flat stack.  The env's field types are the callee's
declared `__cps` parameter types (`emit_param_ctype`, what `emit_params`
spells), so the typed arguments store without a cast.

A new `CE.kont_tail_live` flag says where the jump is legal: a main body, and
a resume-frame helper (`LH_RESUME_CONT`) lifted from one -- the contexts
whose `__kont` is the live downstream chain.  A shift body, a handler case, a
perform-continuation frame and a loop helper that returns its exit value
clear it, and so does everything lifted out of them.  Same exclusions as the
backedge otherwise: a re-resolved or clone callee, a kont other than the
function's own, an argument with a deferred drop (the `freed after` arm needs
the call to return).

**Why the jump is safe.**  Only pass-through frames lie between a driver
landing and a CPS body or resume-frame helper: a `__kont`-threading call
exists only in those two contexts (a handler case and a shift body have no
`__kont` in scope -- a cps->cps call there evicts), the helpers run from
`dk_run_impl`'s `return k->rfn(...)` or a direct tail call, and every
consumer of a `dk_run` result -- an entry wrapper, `__dk_enter0`,
`dk_invoke` (`__dk_drive_bounded`), `__tur_async_resume`, the cloneable
bridge's `__dk_cont_fn` -- installs a landing of its own.  This is the
invariant `dk_tail_resume`'s longjmp has relied on since E7; the bounce
inherits it rather than adding one.  The two eager single-node frees a
longjmp can skip (`shift`'s node, an await's release) are the ones a tail
resume already skips.

**Fibers.**  `FiberBlock` now carries the fiber's own `dk_driver`, swapped by
`tur_fiber_block_resume` with the reap registry.  A resumed fiber used to see
the *resumer's* landing, on another stack; a bounce (or a tail resume) there
would have longjmped across stacks.  It now sees its own, or NULL until a
CPS entry on the fiber installs one.

**Measured** (gcc, x86-64; `tests/fixtures/cps-join-self-call-unoptimized`'s
program, 2,000,000 steps per shape):

| build | before | after |
| --- | --- | --- |
| `-O0` | segfault | 1.57 s, every answer the interpreter's |
| `-O2` | 0.94 s | 0.94 s |
| `-O2`, bounce forced on every call (`-DTUR_DK_BOUNCE_DEPTH=0`) | -- | 1.96 s |

The report's own `sum-to` at 10,000,000 steps, `-O2`, with and without the
guard (the same emitted C, the guarded blocks stripped): 0.55-0.70 s either
way, inside the noise.  The guarded block is two loads and a compare on the
flat path.

The fixture runs the report's repro, the callback-for-effect `do` shape, a
mutual pair through joins, a loop whose callback performs an effect handled
outside it (resumed in place), a non-tail multishot resume around a bouncing
loop (the bounce must land in the resume's own driver and the case must see
the loop's whole value), a loop stepping through another CPS function, and a
nested direct entry inside the callback.  An r7rs named let and an internal
`define` looping through a procedure variable were checked at `-O0` too.

**Verified.** `run.sh` 3755 / 0; `run-leak-check.sh` 176 / 0 / 3
known-open; `run-jit.sh` over the cps, effect, handle,
multishot, resume, async and tail fixtures 515 / 0 (10 skipped); the same
`run.sh` subset with the bounce forced on every call
(`TUR_RUNTIME=source TUR_CC_FLAGS="... -DTUR_DK_BOUNCE_DEPTH=0"`) 473 passed,
the 4 failures all `TUR_RUNTIME=source` link artifacts (`-O0` and
`TUR_REGIONS=0` builds lack `tur_set_contract_handler` /
`tur_region_note_escape` -- they fail the same way without the flag).  The
fixture's program under ASan with leaks on, bounce forced, natural at `-O1`
and absent at `-O2`: the same single 24 B leak each time, which is
pre-existing and filed as
[fat-shim-arg-to-mutual-cps-pair-leaks](fat-shim-arg-to-mutual-cps-pair-leaks.md).
The preamble change regenerated the split runtime and moved 157 `expected.c`
snapshots.
