# A shift that crosses more than one DK entry: the abort leaks the skipped roots, and a CPS `reset` behind an evicted caller panics

**Severity: low** (a bounded leak per abort, and a panic where the interpreter
answers, both in shapes nothing in the suites reaches). The residue of
[compiled-shift-delimited-by-root](../archive/compiled-shift-delimited-by-root.md),
resolved 2026-10-22. Both shapes need a CPS-colored function, an evicted one
and another CPS-colored one in a row, so the shift runs on a different DK
chain than the one the delimiter lives on.

## 1. Jumping out of nested entries leaks each skipped entry's root

```turmeric
(defeffect Tick [] : nil)
(def gv (:: (vec-new) (Vec int)))
(defn h [] : int (+ 1 (shift (fn [v : int] (* 2 v)) 3)))
(defn b [v : (Vec int)] : int (+ 100 (h)))          ; evicted: a second entry for h
(defn a [] : int                                     ; CPS: the first entry
  (handle (do (perform (Tick)) (+ 1000 (b gv)))
    (Tick [] k) (resume k nil)))
(defn run [v : (Vec int)] : int                      ; evicted: a direct reset
  (let [^mut i 0 ^mut acc 0]
    (while (< i 1000)
      (set! acc (+ acc (reset (+ 10 (a)))))
      (set! i (+ i 1)))
    acc))
(defn main [] : int (println (run gv)) 0)
```

Prints 6000 on `tur run`, `tur jit` and `tur --interpret` (each reset yields
6). Under LeakSanitizer (`TUR_CC_FLAGS="-O1 -g -fsanitize=address"`,
`detect_leaks=1:use_stacks=0:use_registers=0`), and under `tur jit` in a
sanitized `tur`: **304,000 B in 2,000 allocations** -- two 152 B DK nodes per
abort.

The abort is deferred to the innermost entry's exit (`__dk_entry_leave`), which
frees that entry's root and reaps, then jumps to the landing
(`__dk_reset_abort`). An entry between that one and the reset -- `a`'s, here --
never reaches its own exit, so its root prompt and the `done` node after it are
never freed. The jump restores `__dk_entry_depth`, `g_dk_driver`, the pending
perform and the reap list, so nothing else is left behind, and with a single
entry between the reset and the shift (the common shape,
`shift-in-callee-delimited-by-direct-reset`) there is no leak at all: 1,000
aborts are clean.

A first version jumped from inside `dk_run_impl` and leaked ~3 nodes on EVERY
abort; freeing the shift's chain there (`dk_free(k)`) double-freed in a
callee's-callee shape, because the reap list frees some of those frames too.
Deferring the jump to the entry's exit is what made the single-entry case
clean.

## 2. A CPS-emitted `reset` behind an evicted caller panics

```turmeric
(def gv (:: (vec-new) (Vec int)))
(defn h [] : int (+ 1 (shift (fn [v : int] (* 2 v)) 3)))
(defn b [v : (Vec int)] : int (+ 100 (h)))          ; evicted
(defn a [] : int (reset (+ 1000 (b gv))))           ; the reset is a DK prompt
(defn main [] : int (println (a)) 0)
```

`tur --interpret`: `6`. `tur run`: `panic: shift used outside of any reset
boundary`. `a`'s reset is a prompt on `a`'s chain; the evicted `b` enters `h`
on a fresh chain whose root has no prompt, and no direct landing is live, so
the root case panics. Before 2026-10-22 the same root case returned f(v) as
`h`'s value instead, so `b` and `a` went on computing with it (a silent wrong
answer rather than a panic).

## Fix directions

1. **Free the skipped roots.** Keep a per-thread stack of entry roots (pushed by
   `__dk_enter0` and the inline entry wrappers in `emit_cps_ir.c`, popped by
   `__dk_entry_leave`), and have `__dk_reset_abort` `dk_free` the roots of the
   entries above `rs->dk_depth` before it jumps. A runtime-preamble change:
   regenerate the `expected.c` snapshots and the S2 split runtime.
2. **Give a CPS reset a landing too.** When a CPS-lowered `reset`'s body can
   reach an evicted function, have it also save and push a
   `tur_shift_reset_ctx` (as `emit_effects_reset` does) and pop it after; a
   shift reaching another chain's root then aborts to it. Its value must
   arrive as the reset's result on the DK side, which the direct landing's
   `TUR_SETJMP` does not give a CPS function for free -- the harder half.
3. Fixtures: both programs above, the first `requires.leak-check`.
