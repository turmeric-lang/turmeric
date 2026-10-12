# A capture-free lambda's fat-shim box handed to a mutually recursive CPS pair is never freed

**Severity: low (24 B per call site evaluation, not per step).** Found
2026-10-12 while leak-checking the fixture for
[cps-self-call-through-join-grows-the-stack](../archive/cps-self-call-through-join-grows-the-stack.md);
it reproduces byte for byte on the same emitted C with that change's bounce
blocks stripped, so it predates it.

## Repro

```turmeric
(defn ping [^fat f : (fn [int] int) i : int n : int acc : int] : int
  (if (>= i n) acc (pong f (+ i 1) n (+ acc (f i)))))
(defn pong [^fat f : (fn [int] int) i : int n : int acc : int] : int
  (if (>= i n) acc (ping f (+ i 1) n (- acc (f i)))))
(defn main [] : int
  (println (ping (fn [x : int] : int (mod x 7)) 0 1000 0))
  0)
```

Built with `-fsanitize=address` (`TUR_RUNTIME=source`), run with
`ASAN_OPTIONS=detect_leaks=1:use_stacks=0:use_registers=0`: one direct leak of
24 bytes, the `malloc(sizeof(void *) + 2 * sizeof(int64_t))` fat-shim box
(`__tur_fatshim1` + the bare fn) built in `main` for the lambda argument.

The same lambda handed to a SELF-recursive loop of the same shape (`sum-to`,
the report above's repro) is freed: a seven-shape program leaked exactly this
one box.

## Root cause (not pinned down)

The shim box for a `^fat` argument is released after the consuming call when
the callee is proven non-retaining for that parameter. `ping` passes `f` on
to `pong`, which passes it back, so the inference likely reads the
mutual pass-through as retention (or the fused tail-call group's member
masks are not consulted at the call site), and nothing frees the box.

## Fix directions

Resolve the non-retaining mask through the cycle (a parameter only ever
passed to a callee that is itself non-retaining for it is non-retaining), or
reap the shim box at the direct->cps entry boundary of the call that built
it, as a closure env is.
