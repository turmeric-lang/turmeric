# In an evicted function, a `shift` with no `reset` around it in that function is not abortive

**Severity: medium** (silent wrong answer). Found 2026-10-23 working
[shift0-pop-plan](../archive/shift0-pop-plan.md).

```turmeric
(defn g [xs : (Vec int) x : int] : int (+ 1 (shift (fn [v : int] (* 2 v)) x)))
(defn g0 [xs : (Vec int) x : int] : int (+ 1 (shift0 (fn [v : int] (* 2 v)) x)))
(defn main [] : int
  (let [xs (:: (vec-new) (Vec int))]
    (println (reset (+ 10 (g xs 3))))     ; 17 compiled, 6 interpreted
    (println (reset (+ 10 (g0 xs 3)))))   ; 17 compiled, 6 interpreted
  0)
```

The `(Vec int)` parameter evicts `g` and `g0` from the CPS backend
(`TUR_TRACE_EVICT=1` shows `SIG-REJECT`), so their `shift` is lowered by
`emit_effects_shift` / `emit_effects_shift0` (`src/compiler/emit_effects.c`).
Those make the shift abortive only in escape mode -- `ctx->in_shift_escape`,
set by a direct `reset` that encloses the shift in the same function
(`emit_effects_reset`). Here the `reset` is in `main`, so
`emit_shift_escape_abort` emits nothing and `f(x)` flows on as the shift's
value: `g` returns `(+ 1 6)` and `main` adds 10. `tur jit` agrees with
`tur run`.

The same gap is why a `shift0` in such a function does not pop its caller's
reset (shift0-pop-plan's change 2 only applies in escape mode): with `h0` =
`(shift0 (fn [v : int] (+ 1 (shift0 (fn [w : int] (* 1000 w)) v))) x)`,
`(reset (+ 100 (reset (+ 10 (h0 5)))))` is 5100 compiled, 5000 interpreted.
(`h0` is evicted because its shift body is not straight-line,
`shift_body_ok`.)

## Fix directions

The landing is live at run time -- `main`'s `reset` installs it, since its body
reaches a call (compiled-shift-delimited-by-root). So outside escape mode a
direct shift could do what the DK machine's root case does: when
`tur_cur_shift_reset` is set, store `f(x)` in it as bits, and abort there
(`__dk_reset_abort`, which restores the recorded DK state); a `shift0` pops the
landing around the receiver call as in escape mode. Two cautions:

- The reset's C type is not known at the shift; the value travels as the
  word's bits, as the DK root case does, and the landing reads it at its own
  type.
- A CPS `reset` (a DK prompt) between the landing and the evicted function is
  not on `tur_cur_shift_reset`, so the jump would skip it -- today a shift in
  tail position there gives the right answer by accident. That is
  [shift-abort-across-nested-dk-entries](shift-abort-across-nested-dk-entries.md)
  item 2; fix it first, or keep the old behaviour when a DK prompt is nearer.
