# A closure passed to a callee that performs and handles its own effect is kept

**Severity: low (leak, 24 B per call).** A direct (non-CPS) function passes a
capturing closure to a function whose own body performs an effect and handles
it. The closure is never freed. Nothing reaches it after the call.

Found 2026-11-28 while fixing
[fat-shim-arg-to-performing-callee-leaks](../archive/fat-shim-arg-to-performing-callee-leaks.md).
It reproduces on `main` at `5aebfd41`.

## Repro

```turmeric
(defmodule m
  (defeffect Ask [] : int)
  (defn a1 [x : int] : int (+ x 1))
  (defn own [^fat f : (fn [int] int)] : int
    (handle (+ (perform (Ask)) (f 10)) (Ask [] r) (resume r 5)))
  (defn direct-caller [k : int] : int
    (let [g  a1
          cl (fn [x : int] : int (+ x k))]
      (+ (own g) (own cl))))
  (defn main [] : int
    (println (direct-caller 3))
    0))
```

It prints `34`. Under ASan with `detect_leaks=1` it reports
`24 byte(s) leaked in 1 allocation(s)` in `direct-caller`: `cl`'s env. On
`main` it reports 48 B, because `g`'s shim leaked too; that half is fixed.

## Root cause

`direct-caller` is direct-emitted, so `cl`'s let decides its free with
`let_binding_env_freeable` and the strict escape walk. The walk lets `cl`
through only if `own`'s strict `nonretain_param_mask` bit is set. That bit is
clear because `own`'s body contains an `EX_PERFORM`, which the strict walk
counts as an escape: a multi-shot resume could re-run a scope-exit free that
lies inside the captured continuation.

Here the continuation is delimited by `own`'s own `handle`, and `own` is
entered through its direct entry, so nothing of `direct-caller` lies inside
it. The box mask (`nonretain_box_param_mask`), which does not count the
`perform`, already has the bit set. It is consulted only for shims and for
reaped owners.

## Fix directions

- Let the strict walk's non-retaining-slot relaxation (`emit_core.c`) consult
  the box mask when every `perform` in the callee is handled by a `handle` in
  the callee itself: an effect row that is empty once its own handlers
  discharge it. Show that no caller-side scope-exit free can then lie in a
  captured continuation, and pin the multi-shot case under ASan.
- Or compute that "delimited" fact during the box-mask inference and store it
  beside the mask.
