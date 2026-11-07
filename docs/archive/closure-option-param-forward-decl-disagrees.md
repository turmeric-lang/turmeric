# A capturing lambda with an `(Option int)` parameter did not build

**RESOLVED 2026-11-07, the day it was found** (while writing the fixture for
[sum-closure-payload-never-dropped](../reported/sum-closure-payload-never-dropped.md)
item 4).

**Severity was medium** (a correct program refused by cc, with no diagnostic
of ours; `tur --interpret` ran it).

## Repro

```turmeric
(defn run [k : int] : int
  (let [f (fn [o : (Option int)] : int
            (match o (Some v) (+ v k) (None) k))]
    (f (some 2))))
(defn main [] : int (println (run 1)) 0)   ; 3
```

```
error: conflicting types for '__fn_7'; have 'int64_t(void *, int64_t)'
error: incompatible type for argument 2 of '__fn_7'
```

The same for `(Result int int)`, for the lambda handed to a typed fn
parameter, wrapped in `(some ...)`, or bound by `letrec`. A captureless
lambda, and a capturing one taking a wide by-value STRUCT, built fine.

## Root cause

A closure thunk takes a parameter wider than a word as the B4 box
(`int64_t __tur_b4box_<p>`, deref-copied at entry). The definition
(`src/compiler/emit_fns.c`) and the CPS entry ask
`type_is_b4box_closure_slot`, which covers a wide by-value ADT AND an applied
by-value sum over 8 bytes (`(Option int)` is 16). Two places asked only
`type_is_wide_byval_adt`, which is the first half:

- the forward declaration (`emit_fn_forward_decls`,
  `src/compiler/emit_module.c`), which so declared the parameter by value;
- the direct call through a let-bound closure (`src/compiler/emit_expr.c`,
  the let-bound thunk call), which so passed the value into the `int64_t`
  slot.

## Fix

Both ask `type_is_b4box_closure_slot`. The call site spills an applied sum
to a local in its own frame and passes the address
(`fat_dispatch_box_arg`): the thunk only deref-copies it at entry, so no heap
box and no owner question. The wide-ADT path there is unchanged.

Pinned by `tests/fixtures/closure-option-param-b4box` (leak-checked: an
`(Option int)` and a `(Result int int)` parameter called directly, a
recursive `letrec`, through a typed fn parameter, wrapped in an Option;
every line equals `tur --interpret`, and `tur jit` agrees).
