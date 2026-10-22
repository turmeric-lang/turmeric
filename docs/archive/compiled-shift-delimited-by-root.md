# A compiled `shift` that finds no reset prompt is delimited by the root: a wrong answer under a direct-emitted `reset`, and no error for a bare `shift`

**RESOLVED 2026-10-22**; see Resolution at the end.

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

## Resolution (2026-10-22)

Fix direction 1's first shape, then direction 2.

**Item 1.** A direct-emitted `reset` (`emit_effects_reset`) installs its setjmp
landing whenever its value can ride the landing and its body can reach a call,
not only for a lexical `shift`, and records the DK machine's per-thread state
in it (`__dk_reset_save`: the driver landing, the pending perform, the entry
depth, the reap mark). In `dk_run_impl`, a reset-tag shift (`k->tag !=
DK_ROOT_TAG`) that reaches its chain's root while a landing is live stores f(v)
in it -- as bits, the way the direct escape does -- marks it `pending`, and
returns as the root case did. The entry's exit (`__dk_entry_leave`, which every
direct->CPS wrapper runs) does its cleanup and then, finding the landing
pending, restores the recorded state and jumps there (`__dk_reset_abort`). So
`(reset (+ 10 (h)))` is 6: the `(+ 10 _)` never runs. Jumping at the entry's
exit rather than from inside the machine keeps the entry's root and reap list
freed (LeakSanitizer-clean over 1000 aborts); the restore covers a jump out of
nested entries, whose roots are then left behind -- 2 nodes per skipped entry
per abort, measured and filed as
[shift-abort-across-nested-dk-entries](../reported/shift-abort-across-nested-dk-entries.md).

The original repro no longer shows the bug as written: since 2026-10-22 a
`catch-unwind` with a lambda thunk lowers on the DK chain and does not evict
`main`. A `(Vec int)` parameter (SIG-REJECT) evicts the same way.

**Item 2.** With item 1 in place, a reset-tag shift that reaches the root with
no landing live has no reset around it: `tur_panic("shift used outside of any
reset boundary")`, the interpreter's message, on `tur run` and `tur jit`. The
2026-10-21 attempt panicked on item 1's first line because a direct `reset`
over a call installed no landing; it does now.

One shape is still wrong, differently: a `reset` in a CPS-emitted function (a
DK prompt) around an evicted function that calls the shifting one starts a new
DK chain for the callee, whose root is reached with no landing live -- that now
panics instead of returning f(v) as the callee's value. A CPS `reset` would
have to install a landing too; filed with the leak above, in
shift-abort-across-nested-dk-entries.

A runtime-preamble change: the 158 `expected.c` snapshots and the S2 split
runtime (`tools/gen-runtime-split.py`) were regenerated. Fixtures
`shift-in-callee-delimited-by-direct-reset` (a callee, a callee's callee, two
shifts in one body, a float result, `shift0`, a nested reset, 1000 resets in a
loop) and `shift-without-reset-panics`, on both harnesses; `tur jit` agrees.
