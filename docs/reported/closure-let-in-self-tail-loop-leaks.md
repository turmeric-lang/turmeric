# A closure let-bound in a self-tail-recursive body is never dropped

**Severity: low (leak; 24 B per iteration for a one-capture closure).** The
closure-drop glue frees a `let`-bound capturing closure at scope exit, but
when the scope ends in a self tail call that becomes a backedge, no drop is
emitted before the `goto`.

## Repro

```turmeric
(defn mk [k : int] : (fn [int] int) (fn [x : int] : int (+ x k)))
(defn loop [i : int n : int acc : int] : int
  (if (= i n) acc
    (let [f (mk i)
          r (f 1)]
      (loop (+ i 1) n (+ acc r)))))
(defn main [] : int (println (loop 0 10000 0)) 0)
```

valgrind: 10000 allocations of 24 B, all "definitely lost" at exit.

Controls that do not leak:

- the same body in a `while` loop (`(let [f (mk i)] (set! acc ...))`);
- the closure bound in a non-tail helper `(defn use1 [i] (let [f (mk i)] (f 1)))`
  -- the emitted body ends with `TUR_CLOSURE_DROP(f_14)`.

## Root cause

The emitted `loop__cps` reassigns its parameters and jumps to
`__tur_cps_self` without a `TUR_CLOSURE_DROP` for `f`. The `rc<T>` path gets
this right -- the same shape with `(rc/of i)` fires its defer frame
(`tur_frame_fire_lifo`) before the backedge -- so the closure drop is
missing from the CPS self-tail-call lowering specifically.

## Fix directions

Emit the scope's closure drops before the backedge in the CPS self-tail-call
path, as the direct path does for rc defers. If the closure flows into the
tail call's arguments it is moved, not dropped, which is the case to keep.

Found writing `docs/guides/memory-usage-guide.md` (2026-10-05).
