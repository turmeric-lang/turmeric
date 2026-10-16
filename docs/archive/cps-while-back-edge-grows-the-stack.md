# A CPS-lowered `while` loop's back-edge grows the C stack below `-O2`

**RESOLVED 2026-10-16** -- see [Resolution](#resolution-2026-10-16). Pinned by
`tests/fixtures/cps-while-back-edge-unoptimized`, built at `-O0` by its
`hook.sh`.

**Severity: medium (stack overflow below `-O2`: `--debug`, `TUR_CC_FLAGS=-O0`,
ASan builds).** The `while` sibling of the resolved
[cps-self-call-through-join-grows-the-stack](../archive/cps-self-call-through-join-grows-the-stack.md).
Found 2026-10-16: turmeric-spices' `signal` `test_buffer`, built with
`-O1 -fsanitize=address`, dies with an ASan stack-overflow 61,991 frames deep,
6,887 iterations into a 48,000-iteration loop. Reproduced on `main` at
`d60c40f6`.

## Repro

```turmeric
(defn call [^fat f : (fn [int] int) x : int] : int
  (f x))

(defn maxdiff [^fat a : (fn [int] int) ^fat b : (fn [int] int) n : int] : int
  (let [^mut k 0
        ^mut m 0]
    (while (< k n)
      (let [d (- (call a k) (call b k))]
        (if (> d m) (set! m d) (set! m m)))
      (set! k (+ k 1)))
    m))

(defn main [] : int
  (println (maxdiff (fn [x : int] : int (mod x 7)) (fn [x : int] : int (mod x 5)) 1000000))
  0)
```

| Build | Result |
| --- | --- |
| `tur run` (`-O2`) | `6` |
| `tur run --debug` | SIGSEGV (exit 139) |
| `TUR_CC_FLAGS=-O0 tur build`, n = 10,000 | `6` |
| `TUR_CC_FLAGS=-O0 tur build`, n = 100,000 | SIGSEGV (8 MiB stack) |

A loop with a single direct callback call
(`(set! acc (+ acc (f i)))`) does not reproduce. It takes the calls through
a `cps` helper, as the signal spice's `(sample sig t)` does.

## Root cause

`maxdiff` is CPS-lowered (its callbacks color it), and the `while` becomes a
loop helper. One iteration is a cycle of C tail calls through two heap joins:

```
maxdiff_loop0__cps -> call__cps(a, ..., join j1) -> ... -> dk_run -> maxdiff_j1
maxdiff_j1         -> call__cps(b, ..., join j2) -> ... -> dk_run -> maxdiff_j2
maxdiff_j2         -> return maxdiff_loop0__cps(__t1, n, m_11, b, a, __kont); /* cps-while-native back-edge */
```

At `-O2` the C compiler turns each into a sibling call and the stack stays
flat; below it, each iteration costs about nine frames.

The 2026-10-12 fix made every `__kont`-threading cps->cps tail call bounce to
the trampoline driver once the stack is deep (`__dk_tail_deep` /
`__dk_bounce`, emitted at `src/compiler/emit_cps_ir.c:9797`). The loop
back-edge is not one of those calls: `emit_continue` (`emit_cps_ir.c:11756`)
emits the bare `return <loop>__cps(..., __kont);` with no depth check. The
resolution notes that a loop helper returning its exit value clears
`kont_tail_live`, but this helper threads `__kont`, so nothing cuts the cycle.

## Fix directions

- Give `emit_continue` the same guard as the cps->cps arm: when
  `__dk_tail_deep()`, park the next-iteration values in a resume-frame thunk
  env and `__dk_bounce` to the driver. Where `kont_tail_live` is false (a
  `ret_direct` helper), find another cut, or refuse the shape loudly instead
  of growing the stack.
- Pin it like `cps-join-self-call-unoptimized`: the repro above built at
  `-O0` by a `hook.sh`, with n large enough to overflow 8 MiB unguarded.

## Resolution (2026-10-16)

Fix direction 1. `emit_continue` (`src/compiler/emit_cps_ir.c`) guards the
back-edge the way the cps->cps arm guards a `__kont`-threading tail call:

```c
if (__dk_tail_deep()) {   /* cps-while back-edge on a deep stack: bounce to the driver */
    maxdiff_loop0_b3_env *__tb = (maxdiff_loop0_b3_env *)malloc(sizeof *__tb);
    __tb->node = dk_frame_resume(maxdiff_loop0_b3, (intptr_t)__tb, __kont);
    __tb->a0 = __t1; ...
    return __dk_bounce(__tb->node);
}
return maxdiff_loop0__cps(__t1, n, m_11, b, a, __kont); /* cps-while-native back-edge */
```

The per-site thunk `<loop>_b<N>` reads the arguments back, frees the env and
the node, and re-enters the loop helper from the driver's flat stack.
`emit_loop` publishes what the thunk needs on the `CE` context
(`cur_loop_ctypes` for the helper's parameter C types, carried params then
invariants, and `cur_loop_inv_names`), next to `cur_loop_name`. The guard
applies only where `kont_tail_live` holds, with the cps->cps arm's other
exclusions: no shift, case or perform frame, and no deferred-drop argument. A
`ret_direct` helper (one that returns its exit value) clears
`kont_tail_live`, so its back-edge is emitted as before; whether that shape
can grow the stack the same way was not examined here.

The repro runs at `-O0` for 10^6 iterations (overflowed at 10^5), under
`--debug`, and unchanged at the default `-O2`. turmeric-spices' `signal`
`test_buffer`, built `-O1 -fsanitize=address`, now passes. The fixture covers
the repro (a heap-cell `^mut`, three invariants), a `float` accumulator
carried through the bounce env, and a loop whose step performs an effect
handled outside it. All three overflow at `-O0` on the unfixed compiler.

`tests/run.sh` passes, with no snapshot changes since no `expected.c` holds
a CPS `while` back-edge. Under `run-jit.sh` the `while` fixtures pass (8, 1
skipped) and so do the CPS / effect / handle ones (427, 8 skipped).
`check-emitted-float-conversions.py` reports no findings on the new fixture.
