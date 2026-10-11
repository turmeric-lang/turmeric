# A cell-carried `^mut` initialized by a call is never given its cell

**Status: RESOLVED 2026-10-11** (see [Resolution](#resolution)).

**Severity: medium (C compile failure).** In a function lowered through the
CPS backend, a `^mut` that a `while` loop carries in a shared heap cell, and
whose initializer is anything but a literal or variable, was declared and
stored as a plain value. The loop helper takes the cell pointer, so cc
rejected the program:

```
error: incompatible type for argument 2 of 'crossings_loop2__cps'
note: expected 'double *' but argument is of type 'double'
```

Found writing turmeric-spices' signal guide: a helper that counts zero
crossings of a signal, sampling it once before the loop and once per step.

## Repro

```turmeric
(defn sample [^fat sig : (fn [float] float) t : float] : float
  (sig t))

(defn walk [^fat y : (fn [float] float)] : int
  (let [^mut k    0
        ^mut prev (sample y 0.0)]
    (while (< k 3)
      (let [v (sample y 1.0)]
        (set! prev v))
      (set! k (+ k 1)))
    0))

(defn main [] : int
  (walk (fn [t : float] : float (* t 2.0))))
```

Three ingredients, all needed: the function is CPS-lowered (here by the call
through a `^fat` callback), the loop carries `prev` in a cell rather than as a
loop parameter (here because it is only written; an assignment under `when`
does it too), and `prev`'s initializer is a call or a primitive. A literal
initializer, or a mutable the loop reads (so it rides as a loop parameter),
compiled.

## Root cause

The promotion to a cell (`loop_cell_scan` / `byref_scan`,
`src/compiler/emit_cps_ir.c`) runs over the finished CPS term, and only
`CT_LETVAL` knows about it: its binder declaration spells the cell pointer and
its emit allocates the cell. A mutable whose initializer is a call or a
primitive is introduced by `CT_LETCALL`, `CT_LETRAW` or `CT_LETPRIM` -- or,
when the call is itself a CPS call, by the parameter of the join it returns to
(`walk_j0`'s slot). All of those declared `double prev` and stored the value;
every use, including the loop entry, read `prev` as the cell.

## Resolution

`cell_init_rewrite`, run per render right after the scans, rewrites each such
binder to bind a fresh `__cell_init<N>` temporary followed by a `CT_LETVAL` of
the mutable from it, so the one path that already allocates the cell does it
for every binder form (call, raw expression, primitive, join slot, perform and
await results). A binder that was not promoted is untouched, so no other
codegen moves: the full suite needed no snapshot regeneration for this half.

**Pinned by** `cps-mut-cell-bound-by-call`: a CPS-call, a direct-call and a
primitive initializer, each read back after the loop, and the zero-crossing
counter that found it.
