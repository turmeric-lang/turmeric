# `tur --interpret`: a mismatched `catch-panic-of` re-raising out of a fiber body corrupts the driver stack

**Severity: medium** (an ASan stack-buffer-overflow, interpreter only; the
compiled program prints the right thing). Found 2026-10-22 testing the CPS
`catch-unwind` lowering. It reproduces on the commit before any of that work
(`f06fa11f`'s `src/turi/eval.c`).

## Repro

```turmeric
(defeffect Ask [] : int)
(defn body [] : int
  (defer (println "body: defer ran"))
  (let [r (catch-panic-of :int (fn [] : int (let [v (perform (Ask))] (if (> v 0) (panic "boom") v))))]
    (if (err? r) 100 200)))
(defn main [] : int
  (println (handle (body) (Ask [] k) (resume k 5)))
  0)
```

`tur run`: `panic at ...: boom`, `body: defer ran`, abort. `tur --interpret`:

```
panic at ...: boom
ERROR: AddressSanitizer: stack-buffer-overflow ...
    #0 eval_drive_ex   (the DRIVE_PUSH write into the inline driver stack)
```

All of these run correctly: the same program with `catch-unwind` (which takes
the panic), the same program without the `defer` in `body`, and a mismatched
`catch-panic-of` outside any `handle`.

## What is involved

The `defer` and the `catch-panic-of` around the `perform` both send the
`handle` to the interpreter's fiber runtime (`ws_capturable` refuses both). The
`catch-panic-of` does not take a `cstr` payload and re-raises. That re-raise
leaves the body's fiber while a work-stack driver frame is live on it, and a
driver then writes past its inline stack. The likely mechanism is a
`longjmp` / `host_exit_jmp` crossing from the fiber stack to a landing on
another stack, leaving a driver's `len` / `cap` stale. Not yet traced.

## Fix directions

1. Trace the re-raise path (`turi_runtime_panic` / the catch-boundary
   machinery around eval.c:3351) for a jump that leaves a fiber, and make a
   panic leave the fiber the way a finished body does: set `done` with the
   error as the body's result, and swap back to the handler side.
2. The root fix the related report names
   ([turi-fiber-handle-defers-fire-at-the-wrong-time](turi-fiber-handle-defers-fire-at-the-wrong-time.md)):
   make `defer` and `catch-unwind` / `catch-panic-of` work-stack capturable,
   so these shapes never reach the fiber runtime.
