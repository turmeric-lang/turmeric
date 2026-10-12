# Self-TCO refuses every function with a `fn`-typed parameter

**Status: RESOLVED 2026-10-11** (see [Resolution](#resolution)).

**Severity: medium (stack overflow at run time).** A self-recursive loop that
threads a callback through -- the normal way to write "do this to every
element" in Turmeric -- was ordinary recursion, not a loop. It overflowed the
8 MB C stack somewhere between 10^5 and 10^6 steps, depending on frame size,
at every optimization level.

Found writing turmeric-spices' `signal/buffer`: `buffer-render` filled a
buffer by recursing over the sample index with the signal as a `^fat`
parameter, and segfaulted rendering ten seconds of 48 kHz audio (480,000
samples). The spice now uses a `while` loop.

## Repro

````turmeric
(defn put [i : int x : float] : int
  ```c
  (void)i; (void)x;
  return 0;
  ```)

(defn fill [^fat sig : (fn [float] float) k : int n : int] : int
  (if (>= k n)
    k
    (do
      (put k (sig (as float k)))
      (fill sig (+ k 1) n))))

(defn main [] : int
  (println (fill (fn [t : float] : float (* t 2.0)) 0 10000000))
  0)
````

`Segmentation fault`. A plain `(fn [int] int)` parameter and a rank-2
`(forall [a] (-> a a))` parameter fail the same way. Annotating the call
`^tailcall` reported why:

```
error [TUR-E0716]: `^tailcall` call is not in tail position: a parameter of the
enclosing function cannot be reassigned by a backedge (pass-by-pointer struct,
`fn`-typed, poly-fn, or carrier-ABI)
```

## Root cause

CF1's eligibility test, `tco_params_simple` (`src/compiler/emit_fns.c`),
returned false for any parameter whose type is `TY_FN` or that is
`is_poly_fn` (a `tur_poly_fn_t`: rank-2 and bare-`fn` parameters). The
restriction exists so the backedge's `p = tmp;` reassignment has an
unambiguous C type. But a loop almost never *changes* its callback: the self
call passes the parameter itself, and such a parameter needs no reassignment
at all -- exactly as a lifted closure thunk's env is already skipped
(`tco_env_offset`). Since the same gate also guards the tail-spine walk,
these functions got neither a backedge nor T2's checkless non-self tail
calls.

## Resolution

`tco_params_simple_ex(..., invariant_fn_ok)` admits a function value
parameter on condition the backedge keeps it. Only the CF1 self path opts in;
a T5 mutual-group jump reassigns every slot, so the group bar is unchanged.

- `tco_mark` takes a self tail call as a backedge only when it passes every
  such parameter itself (`tco_self_call_keeps_fn_params`). The HRT4
  pass-through `EX_POLY_WRAP` around a `tur_poly_fn_t` parameter is
  transparent to that test (`tco_passes_itself`). A self call that passes a
  different function stays an ordinary call: correct, just not a loop.
- `emit_tail_backedge` skips those parameters, as it skips a closure env.
- `^tailcall` on a self call that swaps a callback now says so: "it passes a
  `fn`-typed parameter something other than itself".

The function-level gate also covered T2's non-self tail calls, so a tail call
in a function that takes a callback is now emitted as `return f(...)` with no
panic check after it, as T-D2 (docs/archive/proper-tail-calls-plan.md) already
does everywhere else. That is the whole snapshot delta: 157 `expected.c` files
regenerated, every hunk the stdlib prelude's `consume` / thunk-runner (plus a
few lens helpers) losing the check after its tail call.

**Pinned by** `self-tail-call-keeps-fn-param` (a `^fat`, a bare-`fn` and a
rank-2 callback, each 10^6 deep under `^tailcall`, built at `-O0` by its
`hook.sh`, plus a callback-swapping call that must stay an ordinary call) and
`errors/tailcall-fn-param-swapped` (the new E0716 reason).

**Not covered:** a `^fat` callback whose result feeds the next iteration --
`(sum f (+ i 1) n (+ acc (f i)))` -- is lowered through the CPS backend,
which never reaches this path. Filed separately as
[cps-self-call-through-join-grows-the-stack](cps-self-call-through-join-grows-the-stack.md)
(since resolved 2026-10-12).
