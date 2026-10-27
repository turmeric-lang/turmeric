# A serial-shift receiver's effect cannot reach a handler around the reset

**RESOLVED 2026-09-28** for a named receiver (or a non-capturing `fn`
literal, which is lifted to one) under a straight-frame context -- the
report's repro and every receiver the guides write.  The shapes still refused
moved to
[serial-receiver-effect-under-if-closure-or-leaf](../reported/serial-receiver-effect-under-if-closure-or-leaf.md).

## Resolution

Gap 1 did not need closing.  A serial reset's context is a STATIC frame list
(that is what makes it marshalable), so the continuation a shift would capture
-- those frames over a fresh prompt, exactly the chain the reset already built
before pushing the shift -- can be built directly, without the driver.  When an
effect escapes a named receiver, the reset is lowered as an ordinary colored
call instead of a shift (`recv_outward` on the CT_CLONEABLE node, cps_ir.c;
`emit_serial_outward_call`, emit_cps_ir.c):

    return recv__cps((int64_t)(intptr_t)frames, __dk_reap_node(dk_frame_resume(rest, env, __kont)));

The rest of the enclosing function is lifted as the call's continuation,
exactly as a heap join is, so gap 2 closes by construction: the receiver's
chain runs out through that rest into every handler above the reset.  The
receiver owns the frames chain, as it owned the shift body's copy.  No DK
runtime change, so no snapshot moved.

Gap 3 was two refusals, not one: `fn_sig_ok` refused the `serial-cont`
parameter (an int64 carrier word passed by value -- admitted now, like a
`^borrow` handle), and `param_name_clashes_cps` refused any parameter named
`k`, a reservation left over from when the continuation parameter was spelled
`DK *k` (it has been `__kont` since).  Every serial receiver the guides show is
`(defn recv [k : serial-cont] ...)`, so the reservation is lifted for a
`serial-cont` parameter.  Only for that one: lifting it outright moved every
colored function with a `k` parameter onto the CPS path, and a Saffron
self-applying function (`(k (- n 1) k)`) there leaked a lambda env the direct
path frees (`saffron-lambda-arg-env-freed`, caught merging `main`).

Admitting a `k : serial-cont` parameter exposed one miscompile the
reservation had masked: a CAPTURING closure receiver an effect escapes
compiled and aborted with "unhandled effect", because a closure receiver still
runs from the shift body's fresh root.  It is refused at IR build now (TUR-E0706 from the fallback), as a named
one used to be.

Pinned by `tests/fixtures/serial-shift-receiver-effect-reaches-handler` (four
shapes, every line equal to `tur --interpret`, including a handler that keeps
working after `resume`); `errors/serial-shift-receiver-effect-escapes` now
pins the `if` shape that is still refused.

## Original report

**Severity: low.** A compile-time refusal (`TUR-E0706`, naming the receiver
and the effect), not a wrong answer; `tur --interpret` runs the same program.
The residue of
[serial-shift-colored-receiver-rejected](../archive/serial-shift-colored-receiver-rejected.md),
which admitted every colored receiver and leaf that no effect escapes.

## Repro

```turmeric
(load "stdlib/serial.tur")
(defeffect Ask [] : int)
(defn page [env : cstr hole : int] : int (+ hole 1))
(defn asks [] : int (+ (perform (Ask)) 1))
(defn recv-e [k : serial-cont] : int (k (asks)))       ; Ask escapes recv-e
(defn run [] : int
  (serial-reset (page "" (serial-shift recv-e 0))))
(defn main [] : int
  (println (handle (run) (Ask [] r) (resume r 41)))    ; interpreter: 43
  0)
```

Compiled: `TUR-E0706: serial-shift receiver 'recv-e' performs an effect that
escapes it ({Ask})` (plus a "this effect operation has no lowering here" on the
`perform`, for the second reason below). Pinned as
`tests/fixtures/errors/serial-shift-receiver-effect-escapes`. The same holds for
a context callee (leaf) an effect escapes, with the generic E0706 message.

## Why -- three independent gaps, all needed

1. **The body cannot reach the outward chain.** The shift body calls the
   receiver; for its perform to find a handler it needs a `DK *` whose tail is
   the chain OUTSIDE the prompt (`P->next` in `dk_run_impl`'s `DKK_SHIFT`
   case), which the driver computes and never hands over: `DKBody` is
   `(intptr_t env, DK *sub)`. `DKK_RESUME_FRAME` is the precedent -- a node
   kind whose callback is handed `k->next` and owns delivery. The archived
   report's 2026-09-05 correction has the detail.
2. **The serial reset runs its chain standalone.** `emit_cloneable`'s serial
   arm builds `dk_shift(... frames ... dk_prompt(1, dk_done()))` and runs it
   with `x = dk_run(chain, 0)` in the middle of the enclosing function, so
   `P->next` is `dk_done()` even when the enclosing function is colored and has
   a `__kont`. It would need `emit_reset`'s shape: the reset's rest lifted
   into a resume frame chained to `cur_k`, the chain run in tail position --
   and the enclosing function would need the receiver's effects in its row so
   classification threads it rather than evicting it.
3. **A `serial-cont` parameter is not CPS-emittable.** `fn_sig_ok`
   (`src/compiler/emit_cps_ir.c`) refuses the `cont` param, so an effectful
   receiver is SIG-REJECT and its perform has no lowering at all -- before the
   reset is even considered.

## Workaround

Handle the effect inside the receiver (or in a function it calls), or
perform it outside the `serial-reset`. A receiver that only calls through
function values, or handles its own effects, is accepted.
