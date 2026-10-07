# An effectful fn-value escaped through a parameter that was never threaded

**RESOLVED 2026-10-07.** Found and fixed the same day, while testing
[cps-coloring-resolves-a-parameter-to-a-same-named-global](cps-coloring-resolves-a-parameter-to-a-same-named-global.md).

**Severity was:** high. A program that compiled cleanly aborted at run time
with `tur: unhandled effect (tag N)`. That happened when one function-value
parameter received an effectful lambda in one call and a pure function value
in another.

## Repro

```turmeric
(defeffect Ask [] :int)
(defn g [x : int] : int (+ x 1))
(defn app [h : (fn [int] int) x : int] : int (h x))
(defn main [] : int
  (println (app g 1))
  (println (handle (app (fn [x : int] : int (+ x (perform (Ask)))) 1)
             (Ask [] k) (resume k 10)))
  0)
```

Compiled: `tur: unhandled effect (tag 2)`, then SIGABRT. Expected `2` then
`11`, as `tur --interpret` prints. Deleting the `(app g 1)` line made it work,
and so did passing a second effectful lambda instead of `g`.

## Root cause

Two analyses in `src/compiler/emit_cps_ir.c` disagreed:

- **Threadability** (`fv_multi_tally`) counted the effectful lambda as
  threadable, because its only use is an argument at a parameter whose class
  (`param_thread_class`) is threadable. That class reads only how the callee
  uses the parameter.
- **Thread-param registration** (`param_is_thread_safe`) registers `h` only
  when EVERY value passed to it has a registered `__cps` entry. `g` has none,
  so `h` was not registered.

An unregistered parameter's call is safe to delegate to the direct emitter
(`safe_to_delegate`, `src/passes/cps_ir.c`), so `app`'s body became a plain
`h.fn(h.env, x)`. The lambda, still counted threadable, was neither threaded
nor routed to the fiber by the E2 taint. Its `perform` started from the fresh
root its direct entry installs.

## Fix

- A call through a fat poly-fn parameter that the `fn_cps` dispatch covers is
  never delegated (`safe_to_delegate`). That dispatch calls the value's
  DK-threading entry when it has one and its direct entry otherwise, so it is
  right for every value the parameter holds, pure or effectful. This is what
  makes the repro work.
- After registering thread params, `ensure_S` withdraws any effectful
  threadable fn-value passed to a parameter some call through which does not
  thread (`cps_ir_param_call_threads` mirrors the call lowering), then
  re-registers until nothing changes. "Effectful" means an effect that
  escapes the value (`fn_net_escaping_acc`): a lambda that handles its own
  `perform` needs no threading. A withdrawn value goes down the existing E2
  taint, so a shape the backend cannot thread is refused at compile time
  instead of aborting at run time. The same pass covers a parameter dropped
  by `cps_ir_thread_param_add`'s 256-entry cap.

Pinned by `tests/fixtures/cps-effectful-and-pure-fnval-one-param` on all three
back ends.

## Still open

Two shapes now get the compile-time refusal where they used to compile and
then abort. Both are filed as
[cps-effectful-callback-through-multi-arg-or-untyped-param](../reported/cps-effectful-callback-through-multi-arg-or-untyped-param.md):
an effectful callback through an un-annotated parameter of two or more
arguments, and one through an untyped `^fat` parameter.
