# A CPS function's self call from its own join frame grows the C stack

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
