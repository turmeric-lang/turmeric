# `defer` in a CPS-converted function does not run on a caught panic

**Severity: medium (cleanup skipped: leaks, and anything else a defer was
guarding).** Found 2026-10-10 adding `mw-recover` support to tourist
(tourist-on-stdlib-httpd-plan H4). In a function compiled directly, a `defer`
fires when a panic passes through (`tur_frame_fire_lifo` before each
`if (tur_panicking) return`). When the function is CPS-converted (it performs
an effect, or calls something that does), its `__cps` body runs the deferred
form inline at the normal exit only; the panic path returns without it.

## Repro

```turmeric
(defn note [s : cstr] : int
  ```c printf("%s\n", s); fflush(stdout); return 0; ```)

(defn boom [x : int] : int
  (if (> x 0) (panic "boom") x))

(defn direct [x : int] : int
  (defer (note "direct: defer ran"))
  (boom x))

(defeffect Ask [] : int)

(defn cps [x : int] : int
  (defer (note "cps: defer ran"))
  (+ (perform (Ask)) (boom x)))

(defn main [] : int
  (let [r1 (catch-unwind (fn [] : int (direct 1)))]
    (note (if (err? r1) "direct: caught" "direct: no panic")))
  (let [r2 (catch-unwind (fn [] : int (handle (cps 1) (Ask [] k) (resume k 1))))]
    (note (if (err? r2) "cps: caught" "cps: no panic")))
  0)
```

```
direct: defer ran
direct: caught
cps: caught            <- "cps: defer ran" is missing
```

## Where it bit

`tourist/app`'s `__tourist-handler` is CPS-converted (it reaches user closures
through the `use!` chain), so `(defer (tourist-ctx-free ctx))` there leaked the
request's ctx, its attrs and its route captures on every panicking route. The
same defer in spices/httpd's (direct) `srv-serve-view` works.

## Fix directions

The CPS emitter should register the defer frame as the direct emitter does, and
fire it on every `if (tur_panicking) return` it emits. The archived
`defer-in-generic-hof-skipped-on-caught-panic` is the same family.

## Workaround in use

tourist calls `__tourist-run` from an inline-C guard (`__tourist-run-guarded`)
that frees the ctx when `tur_panicking` is set after the call.
