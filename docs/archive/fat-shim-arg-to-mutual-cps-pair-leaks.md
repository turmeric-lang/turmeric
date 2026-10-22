# A capture-free lambda's fat-shim box handed to a mutually recursive CPS pair is never freed

**RESOLVED 2026-10-22**; see Resolution at the end.

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

## Resolution (2026-10-22)

The guess in "Root cause" was right, one step more precisely: each function's
`nonretain_param_mask` is a greatest fixed point over its SELF-calls only, and
`ping` is inferred while `pong` -- defined after it -- still has mask 0 because
it has not been inferred yet, not because its body retains. So `ping`'s hand-off
read as retention, and `pong`'s back to `ping` did too. With both bits clear,
`main` could neither use the static box (the lambda captures nothing) nor free
the malloc'd one.

Fix direction 1, through the cycle (`elab_nonretain_resolve_forward`,
`src/compiler/elab_fns.c`, run after each defn's inference):

- A function whose fn-param bit was cleared by a hand-off to a function whose
  inference has not run (`Binding.nonretain_inferred` still false) waits in
  `Elab.nonretain_pending`.
- When a function is inferred, everything waiting on it -- transitively, so
  `c1 -> c2 -> c3 -> c1` comes in whole -- is solved again together with it:
  every candidate bit set, then cleared against the members' current masks and
  everyone else's final ones until nothing changes. A bit that survives is one
  no use in the group contradicts. Entries stay on the list, so a member still
  waiting on a later function is solved again when that one arrives.
- Raising a bit after a caller was elaborated leaves that caller's site exactly
  as conservative as it was; callers elaborated afterwards -- `main` here -- see
  the solved masks.

`main` now hands the capture-free lambda the static box, and a capturing one is
hoisted into a `__borrowc` let and dropped after the call.
`fat-shim-arg-to-mutual-pair-freed` (`requires.leak-check`) pins the repro, a
capturing lambda through the pair, a three-function cycle, and a pair where one
member stores `f` in a global: that one stays retained, and the stored closure
is called after the pair returns.
