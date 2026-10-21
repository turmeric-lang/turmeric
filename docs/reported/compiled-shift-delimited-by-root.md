# A compiled `shift` that finds no reset prompt is delimited by the root: a wrong answer under a direct-emitted `reset`, and no error for a bare `shift`

**Severity: medium** (silent wrong answer). Found 2026-10-21 working
continuation-forms-gaps (it was item 2 there).

## 1. A direct-emitted `reset` around a CPS callee gives the wrong answer

```turmeric
(defn h [] : int (+ 1 (shift (fn [v : int] (* 2 v)) 3)))
(defn main [] : int
  (println (reset (h)))           ; 6 compiled, 6 interpreted
  (println (reset (+ 10 (h))))    ; 16 compiled, 6 interpreted
  (let [r (catch-unwind (fn [] : int 0))] (println (err? r)))
  0)
```

The `catch-unwind` is only there to evict `main` from the CPS backend; any
evicting form does. An abortive `shift` discards its whole context up to the
`reset`, so the second line should be `(* 2 3)` = 6.

What happens: `main` is emitted by the direct emitter, where a `reset` is a
setjmp landing (`tur_shift_reset_ctx`, `emit_effects.c`). A landing is installed
only when the body reaches a `shift` lexically (`reset_body_has_base_shift`);
`(reset (+ 10 (h)))` has none, so nothing is installed. `h` is CPS-colored, so its
`shift` is a `dk_shift(1, ...)` run on the DK machine from `h`'s own entry
(`__dk_enter0`), whose chain holds no reset prompt. `dk_run_impl`
(`emit_dk_runtime.c`, `case DKK_SHIFT`) then takes the root as the delimiter
(`to_root`) and hands the receiver's value back as `h`'s return value. Only
`(+ 1 _)` inside `h` is discarded; `(+ 10 _)` in `main` still runs.

## 2. A `shift` with no `reset` at all is not an error

```turmeric
(defn h [] : int (+ 1 (shift (fn [v : int] (* 2 v)) 3)))
(defn main [] : int
  (println (reset (h)))
  (println (h))
  0)
```

`tur run` prints `6` and then exits with status 6: the bare `(h)`'s shift is
delimited at the root, here `main`'s entry, so the receiver's value becomes
`main`'s result and the exit code. `tur --interpret` stops with `shift used
outside of any reset boundary`.

## What was tried

2026-10-21: a panic in `dk_run_impl` when a reset-tag (non-`DK_ROOT_TAG`) shift
reaches the root while no `tur_cur_shift_reset` is live. Reverted: it turned the
correct first line of item 1 into a panic, because the direct `reset` over a call
never installs that context. The root delimiting is load-bearing for item 1's
first line, and wrong for its second.

`DK_ROOT_TAG` shifts (`await`) reach the root on purpose and are not this.

## Fix directions

1. Make a direct-emitted `reset` a real delimiter for a callee's `shift`. Two
   shapes:
   - always install the landing (not only for a lexical `shift`), and have a
     reset-tag DK shift that reaches the root with a landing live store
     `f(x)` in it and longjmp there, the way `emit_shift_escape_abort` does for a
     direct `shift` -- the landing's result slot is an `int64_t` word, so a float
     result travels as its bits, as the `call/cc` landing does;
   - or enter the reset's body through the DK machine (`dk_prompt(1, ...)`) when
     it can reach a colored function, so the shift finds a real prompt.
   The first is smaller and keeps evicted functions direct.
2. With that in place, a reset-tag shift that still reaches the root has no
   reset around it: make it the interpreter's error (`shift used outside of any
   reset boundary`), a runtime-preamble change (regenerate the snapshots and the
   split runtime).
3. Fixtures: both programs above on `tur run`, `tur jit` and `--interpret`, plus
   the same with `shift0` (see the shift0 plan, which depends on this).
