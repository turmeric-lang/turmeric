# Calling a function value allocates CPS frames that a tail-recursive loop holds until it returns

**Severity: medium (unbounded memory growth in long-running loops).** A
function that calls a function-typed value is emitted through its `__cps`
twin, and each such call from a CPS caller allocates two continuation frames
(~200 B). The frames are registered with the DK reaper, which frees them only
when the *outermost* direct-style entry returns. A self-tail-recursive loop
that makes such a call per iteration therefore grows by ~200 B per iteration
for its whole run.

## Repro

```turmeric
(defn add1 [x : int] : int (+ x 1))
(defn use1 [f : (fn [int] int) i : int] : int (f i))
(defn loop [i : int n : int acc : int] : int
  (if (= i n) acc (loop (+ i 1) n (+ acc (use1 add1 i)))))
(defn main [] : int (println (loop 0 100000 0)) 0)
```

| shape | allocs / call | peak heap / call |
| --- | --- | --- |
| above (tail-recursive driver) | 2 | **205 B** (grows with n) |
| same, closure `(let [f (fn ...)] (f i))` inside `use1` | 2 | 200 B (grows) |
| same `use1`, driven by a `while` loop in `main` | 2 | 0 (bounded) |
| `(fn [int] #fx{} int)` parameter, or `#fx{}` on `use1` | 2 | 205 B (no change) |

Effects show the same retention: a `perform` / `resume` pair in a
tail-recursive loop under one `handle` costs 6 allocations and ~630 B of
peak heap per iteration.

`--dump-cps-coloring` reports `use1` and `loop` as `uncolored`, yet the
emitted C has `use1__cps`, `loop__cps`, and a
`__dk_reap_node(dk_frame_resume(...))` heap join per call
(`src/compiler/emit_cps_ir.c:8778`).

## Root cause (partial)

The reap list is drained by `__dk_reap_run` only when `__dk_entry_depth`
returns to 0, i.e. when the direct-style wrapper around the outermost CPS
function returns. A `while` loop calls the direct wrapper once per
iteration, so it drains each time; a self-tail-recursive loop is itself the
CPS function, so it never does until it finishes. It is not yet clear why a
callee with an empty effect row is emitted on the CPS path at all.

## Fix directions

- Drain the reap list at a self-tail-call backedge in a CPS function: the
  frames of a finished iteration are dead there.
- Or keep effect-free fn-value calls on the direct path (the `#fx{}` rows above
  suggest the effect annotation is not consulted).

Workaround: drive the loop with `while`, or keep calls through function
values out of long-running tail-recursive loops.

Found writing `docs/guides/memory-usage-guide.md` (2026-10-05).
