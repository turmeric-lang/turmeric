# Compiled: an effect capture cannot cross a `catch-unwind`

**Narrowed again 2026-10-24: the clause shape is fixed too; what is left are
thunk shapes the inline lowering does not take.** A handler case body now
admits a catch prompt (`handle_case_ok_rec`'s CT_RESET,
`src/compiler/emit_cps_ir.c`). `emit_reset` installs it on the case's
`__kont`: the real enclosing chain for a re-opening case
(`dk_case_enclosing_real`), otherwise a done node of the case's own
(`emit_lifted`, `case_has_catch`), so running the prompt chain hands the
case's value back to `dk_perform` as before. The rest of the case is lifted
past the prompt (LH_RESUME_CONT), the delimited part delivers through the
chain (`shift_mode` off there) and its panic checks name the prompt
(`g_pend_k_ok`), so a panic the resumed body raises -- coming back through
`dk_invoke`, or `__dk_discontinue` -- is caught at the prompt and the rest of
the case goes on with Err. `case_reopens` looks inside the prompt. The third
row of the table below prints `100` compiled and under `tur jit`; pinned by
`tests/fixtures/cps-catch-unwind-in-clause` (all three engines,
leak-checked) and, for `discontinue`, `discontinue-caught-in-clause`.

**Left** (none blocks [discontinue-plan](../upcoming/discontinue-plan.md)):

- A thunk that captures a local and performs -- `(fn [] : int (+ x (perform
  (Ask))))`, or in a clause `(fn [] : int (resume k (perform (Log 7))))` --
  is still refused: the lifted thunk is a closure, fails `fn_sig_ok`, and its
  effect is perm-tainted (`sig_perm`, `emit_cps_ir.c` near `fn_net_escaping`)
  although its caller inlines it and never calls it. A capture-free thunk, or
  one that captures and only resumes / discontinues, is fine. Fix direction:
  skip the perm-taint for a `cps_catch_inlined` thunk, and emit its dead body
  as a trap rather than its direct lowering -- but only when the catch that
  inlines it was CPS-emitted, or an evicted caller's direct `catch-unwind`
  would call the trap.
- A thunk whose body is typed `never` (`(fn [] : int (panic "x"))`) is not
  one word to `catch_delim_type_ok`, so the catch keeps the delegated
  lowering -- which evicts a function that also holds a `handle`.
- As before: a thunk that is not a lambda at the form or returns more than a
  word; a runtime helper's panic inside a catch prompt with no direct
  `catch-unwind` live; a catch prompt on a thread other than `main`'s.

**Narrowed 2026-10-22: the body-side shapes are fixed; a `catch-unwind` inside
a handler CLAUSE is what is left.** The false TUR-W0033 is gone on every engine
(`collect_called_thunk`, `src/passes/effect_check.c`). Compiled, a
`catch-unwind` / `catch-panic-of` whose thunk is a lambda written at the form
(nullary, no `return`, returning a machine word: `int`, `bool`, `cstr`, `nil`)
is now translated inline under a **catch prompt** on the DK chain, a CT_RESET
with `is_catch` (`build_catch`, `src/passes/cps_ir.c`; `emit_reset`,
`dk_prompt_catch`). So the first two rows of the table below now print `100`
compiled. In more detail:

- **Coloring.** The coloring walks treat such a thunk's body as the enclosing
  function's (`cps_catch_inline_thunk`, `src/passes/cps.c`), and the lifted
  thunk is marked `cps_catch_inlined`, so it no longer counts as a fn value
  whose effects escape (that made it a permanent fiber source and tainted
  every function performing the same effect).
- **Runtime.** A catch prompt boxes the delimited body's value as Ok when it
  is reached normally (`dk_run_impl`). A panic is caught wherever a chain is in
  hand (`__dk_panic_catch`): a frame returning by signal in `dk_run_impl`, a
  shift body, a CPS body's panic return (`cps_pend_hook_fire`; it consumes its
  chain, so it delivers the Err itself), the await path, and `dk_perform`.
  `__dk_panic_abandon` stops at a catch prompt that takes the panic, so the
  frames past it are not unwound. A handler case or a shift body runs outside
  the part it cut off, so its panic retags the catch prompts in between dead
  (`__dk_catch_kill`) and only a catch past the handler takes it. A copied
  continuation copies its catch prompts, so each `^multishot` copy catches on
  its own. A catch-panic-of prompt takes only a payload of its type.
- **Printing.** An uncaught panic's message is now printed when the root
  unwind ends in `main` (`tur_panic_root_print`), not where it was raised,
  since a catch prompt further up may still take it. It is also printed ahead
  of a double panic.
- **Memory.** The Ok / Err box a let-bound catch makes, and an Err's payload,
  go on the reap list when the binding provably does not escape the let (the
  direct emitter's `let_binding_box_freeable`, exported as
  `emit_let_binding_box_freeable`).
- **On the way:** `panic-with` in tail position of a CPS function was
  BODY-UNSUPPORTED (`EX_#41`); it is now delegated like `panic`.

Pinned by `tests/fixtures/cps-catch-unwind-across-perform` (eight shapes, all
three engines, leak-checked) and `cps-catch-unwind-multishot` (compiled only
then: the interpreter's fiber runtime was single-shot; all three engines since
turi-fiber-handle-defers-fire-at-the-wrong-time was fixed, 2026-10-24).

**What was left on 2026-10-22** (the clause half is fixed since 2026-10-24,
see the top). The third row: a `catch-unwind` in the handler clause around
a `resume` is still refused compiled. A handler case body has its own grammar
(`handle_case_ok_rec`, `src/compiler/emit_cps_ir.c`), it delivers its value by
returning to `dk_perform` rather than through a chain, and it admits no
CT_RESET. A catch there would need the case to hold a chain to deliver
through, as a re-opening case does. Also still outside this lowering, keeping
the old behaviour: a thunk that is not a lambda at the form, or whose value is
not one word (a float, an aggregate). Two more limits: a panic raised by a
runtime helper (a failed cast or dynamic operator) inside a catch prompt with
no direct `catch-unwind` live still aborts where it is raised, since only the
panic forms unwind (see
[compiled-uncaught-panic-skips-caller-defers](../archive/compiled-uncaught-panic-skips-caller-defers.md)).
And on a thread other than `main`'s, a catch prompt never sees a panic at all.

**Severity: medium** (as filed; low for what is left). A compile-time refusal, not a wrong answer, and
`tur --interpret` runs every shape below. It **blocked
[discontinue-plan](../upcoming/discontinue-plan.md)** on the compiled engine
(no longer, since 2026-10-24):
the plan's reason for keeping `discontinue` is that a `catch-unwind` in the
body, around the `perform`, can catch the raise and carry on, and today no
compiled program can put a `catch-unwind` between a `perform` and its handler.
Found 2026-10-22 checking what blocks that plan; reproduces on `main` (`a0a718f3`) and on #1326's head.

## Repro

```turmeric
(defeffect Ask [] : int)
(defn body [] : int
  (let [r (catch-unwind (fn [] : int (+ 1 (if (> (perform (Ask)) 0) (panic "boom") 0))))]
    (if (err? r) 100 200)))
(defn main [] : int
  (println (handle (body) (Ask [] k) (resume k 5)))
  0)
```

| | `tur run` / `tur jit` | `tur --interpret` |
| --- | --- | --- |
| `perform` inside the thunk (above) | refused: `this effect operation has no lowering here`, plus ``note: `Ask` escapes this function value`` | `100` |
| the same, with the `perform` in a helper the thunk calls | refused the same way | `100` |
| `catch-unwind` in the CLAUSE around `(resume k 5)` | refused: `no lowering here` on the body's `perform` (`TUR_TRACE_EVICT=1`: `BODY-UNSUPPORTED main unsupported form: EX_CATCH_UNWIND`); **`100` since 2026-10-24** | `100` |

All three engines also warn, wrongly, `TUR-W0033: handler clause for 'Ask' is
unreachable: the body does not perform 'Ask'` on the first two shapes.

What does work: a `catch-unwind` in a function that holds neither the
`perform` nor the `handle`, around a call that does both
(`tests/fixtures/panic-in-handler`, `defer-cps-caught-panic`).

## Root cause

- **CPS IR.** `EX_CATCH_UNWIND` / `EX_CATCH_PANIC_OF` have no rule in
  `src/passes/cps_ir.c` (they fall to "unsupported form", `cps_ir.c:586`), so a
  function holding one is evicted to the direct emitter, which lowers
  `catch-unwind` as a setjmp landing on the C stack. A `handle` in that
  function then has no lowering.
- **The thunk.** The thunk is a function value handed to the direct-emitted
  landing, and a call through it starts with no handler in scope
  (`emit_expr.c:18322`, the "escapes this function value" note). It is the same
  structural gap as
  [compiled-shift-delimited-by-root](../archive/compiled-shift-delimited-by-root.md): a
  direct-emitted delimiter around a CPS callee.
- **TUR-W0033.** `collect_effects_in_expr` (`src/passes/effect_check.c:674`)
  walks the thunk as an expression; the thunk is a `fn` literal, whose effects
  are latent, so the `perform` in its body is never counted against the
  enclosing `handle`. `catch-unwind` calls its thunk at once, so the thunk's
  body effects belong to the `catch-unwind` node.

## Fix directions

1. **A `catch-unwind` term in the CPS IR.** Lower it as a DK frame that marks a
   panic boundary, so the thunk's body is translated inline (it is called
   exactly once, at once) and a `perform` inside it captures through the
   boundary like any other frame. The DK machine already has the pieces: CPS
   frames return on `tur_panicking` (`cps_panic_check`) and abandoned frames
   fire their defers (`__dk_panic_abandon`). The boundary frame consumes
   `tur_panicking` and delivers `(Err payload)`. A multishot copy of a chain
   holding the boundary copies the boundary too.
2. **TUR-W0033 alone (small):** in `effect_check.c`, when the thunk of
   `EX_CATCH_UNWIND` / `EX_CATCH_PANIC_OF` is a `fn` literal, collect its body.
   That fixes the false warning on every engine, independent of (1).
