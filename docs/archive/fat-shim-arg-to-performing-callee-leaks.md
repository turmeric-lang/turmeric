# A defn passed to a `^fat` parameter of a function that performs leaks its shim

> **RESOLVED 2026-11-28.** The root cause was as guessed, plus a second reason
> it hid behind. `expr_subtree_has_inline_c` has no arm for `perform`,
> `handle` or `resume`, so it reads any body containing one as "maybe inline
> C", and the whole non-retention inference was skipped for such a function.
> The escape walk counting a `perform` as an escape then mattered only for a
> body the first check let through.
>
> - **Box mask** (fix direction 1). The new `nonretain_box_param_mask` in
>   `elab_fns.c` asks the fn-param question again with
>   `closure_binding_escapes_reaped`, where a `perform` is not an escape, and
>   with a precise inline-C scan (`nr_has_inline_c`, over the shared operand
>   enumeration). It is a greatest fixed point over self-calls, like the main
>   pass. Only two places read it:
>   - The caller's `^fat` argument shim in `elab_call.c`. The repro's shim is
>     now the shared static box: no allocation at all.
>   - The escape walk in reaped mode (`emit_core.c`), so a capturing closure
>     a CPS caller passes is reaped at its DK entry boundary.
>
>   A shim box is static, on the caller's stack, or reaped once at the DK
>   entry boundary, so a resume can never re-run its free. That is why the
>   `perform` is no hazard here, where it is for a scope-exit free inside a
>   captured continuation, the reason the strict mask counts it.
>   `EX_POLY_TO_FAT`'s stack box gained the same guard `EX_FN_TO_FAT`'s has:
>   in a DK body it takes the heap and the entry's reaper.
> - **A callee that only installs a handler**, `(handle (f 10) ...)`, gets its
>   strict mask too (`elab_infer_nonretain_fn_mask_precise`). The same strict
>   walk runs, still counting a `perform` as an escape, but over a precise
>   inline-C scan. Its handler delimits every continuation its callback
>   captures, so a direct caller frees the closure it passed. On `main` that
>   was 24 B a call.
>
> Found on the way, pre-existing, and fixed: `(let [g (do ... f)] (g 7))` with
> `^fat f` segfaulted. The `do` was typed like `f`, a thin `(fn ...)`, and a
> `let` inherits `is_fat` only from a bare variable, so `g` called the box as
> a code pointer. `elab_fn_tail_carrier_to_fat` now re-types a `do` / `let`
> whose value is fat. Pinned in `fn-value-through-if-or-match` (`fd-let`,
> `fd-call`, `fl-call`).
>
> Pinned by `fat-shim-arg-to-performing-callee` (`requires.leak-check`: a defn
> and a capturing closure under one-shot and multi-shot handlers, handler-only
> helpers called from a direct caller, and a must-keep callee that stores its
> argument). On `main` that fixture leaks 12120 B in 505 allocations; it is
> clean now. An ASan sweep (stack use-after-return on) over the 355 fixtures
> that `perform` or `handle` found no error.
>
> Not covered: a direct caller passing a closure to a callee that itself
> performs (and handles it). Filed as
> [closure-arg-to-self-handling-callee-kept](../reported/closure-arg-to-self-handling-callee-kept.md).

**Severity: low (leak, 24 B per call).** If a function performs an effect, a
named defn passed to one of its `^fat` parameters leaks the `{ shim, fn }` box
the caller builds for the call. Nothing about the callee's use of the
parameter matters: calling it and nothing else is enough.

Found 2026-11-28 while leak-checking
[carrier-fn-param-in-join-evicts-effectful-fn](../archive/carrier-fn-param-in-join-evicts-effectful-fn.md).
It reproduces on `main` at `5aebfd41`, so the join has nothing to do with it.

## Repro

```turmeric
(defmodule m
  (defeffect Ask [] : int)
  (defn a1 [x : int] : int (+ x 1))
  (defn ff [^fat f : (fn [int] int)] : int (+ (perform (Ask)) (f 10)))
  (defn main [] : int
    (println (handle (ff a1) (Ask [] r) (resume r 1000)))
    0))
```

It prints `1011`. Under ASan with `detect_leaks=1` it reports
`24 byte(s) leaked in 1 allocation(s)`, allocated in `main__cps`:

```c
void *__t204 = malloc(sizeof(void *) + 2 * sizeof(int64_t));
...
return m__ff__cps((int64_t)(intptr_t)__t2, __h0); /* cps->cps */
```

The same caller with a callee that does not perform, such as
`(defn ff [^fat f : (fn [int] int)] : int (f 10))` called inside the `handle`,
reclaims the box.

## Root cause (likely, not traced to the line)

The caller's shim is stack-allocated, or handed to the DK entry reap inside a
CPS function, only when the sink is proven non-retaining: `stack_ok` on
EX_FN_TO_FAT (`elab_call.c`), driven by the callee's `nonretain_param_mask`.
That mask comes from `closure_binding_escapes` over the callee's body
(`elab_infer_nonretain_masks`, `elab_fns.c`). That walk treats an `EX_PERFORM`
conservatively as an escape. A `perform` anywhere in the body therefore clears
the bit for every fn parameter, and the shim is heap-allocated with no owner.

## Fix directions

- For a fn parameter's non-retention bit, use `closure_binding_escapes_reaped`
  (which treats a `perform` as not an escape) when the caller would reap the
  shim at its DK entry boundary rather than free it at scope exit. That is the
  same distinction `cps_closure_env_freeable` already draws.
- Pin it with a `requires.leak-check` fixture: a defn and a capturing closure,
  each passed to a `^fat` parameter of a performing callee.
