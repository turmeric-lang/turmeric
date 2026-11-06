# `tur --interpret`: a call's tyvar/dictionary pins, and frames made off the driver's call path, are never handed back

**Narrowed 2026-11-06: the `reset`/`shift` capture frames are handed back.**
The `let` frames `ts_capture_and_run` makes while it walks a
`cloneable-reset` / `serial-reset` body to its shift are call frames now
(`eval_frame_new_call`), released with `frame_release` where they used to go
to the no-op `eval_frame_free`: after the capture, or in
`serial_receiver_resume` once the deferred receiver has run. A closure over
one (the receiver, a context call's fn) marks it escaped and it is kept; a
call/cc capture since defers it to the sweep; a reified `TsFrame` holds values,
never frames. Measured on a Debug `tur --interpret` (ASan quarantine off):
200,000 resets with a two-binding `let` in the context grew peak RSS by 54 MB
and now by 27 MB. The rest is the `TuriCont` and its frame array, which
`turi_val_alloc` keeps for the life of the run (the continuation is handed to
the receiver as a handle, with no owner to say it is done) -- that is
continuation lifetime, not a side frame, and this report's list is now empty
of frames. Pinned by `tests/fixtures/turi-reset-capture-frames-released`
(both engines: a receiver closure over a context `let`, called after 1000
further resets have reused the free list).

**Narrowed 2026-11-06: both handler case frames are handed back.** The
work-stack perform's escaping-`k` case frame (`hf`) is a call frame now,
released when its prompt delivers the case body's value (`DK_PROMPT`, through
the continuation's `case_frame`); the fiber path's (`eval_handle_inner`) is
released when its synchronous body returns. Both are skipped by
`frame_release` when a closure over them escaped or a capture since may run
them again. Left: the `reset`/`shift` capture frames. See "Fixed 2026-11-06"
at the end.

**Narrowed 2026-10-09: the defer snapshot is fixed.** `EX_DEFER`'s snapshot
is a call frame now (`eval_frame_new_call`, parentless), and both places a
`DeferItem` fires release it after its body (`frame_release`, a no-op if the
body captured it -- a closure over the snapshot escapes it, as any capture
does). A function with a `defer` called 300,000 times grew a Debug `tur` by
30 MB and now by none; `tests/check-turi-frame-reclaim.py`'s fifth program
pins it, and `tests/fixtures/defer-body-names-a-global` runs a defer body that
captures its snapshot in a closure called after the defer fired. Left: the
fiber handler case frame, `reset`/`shift` capture frames and the work-stack
perform's `hf`.

**Narrowed 2026-10-07: section 1 is fixed, and `EX_EXISTS_OPEN` from
section 2.** A released frame's `TyvarBind` and `DictBind` chains go onto
free lists beside `frame_free` / `binding_free` (`env->tyvar_free`,
`env->dict_free`, emptied with them at a scratch reset), and every pin site
takes its node from them (`tyvar_bind_alloc` / `dict_bind_alloc`,
`src/turi/eval.c`). The check the fix asked for holds: each site prepends a
fresh node to its own frame's chain, `frame_record_abi` copies a caller's pin
by value, and lookups copy the `Type` out, so nothing points at a released
frame's nodes. `EX_EXISTS_OPEN`'s frame is now `eval_frame_new_owned`: its
body runs synchronously, the same shape as a `let` frame.

Measured: a generic function called 300,000 times grew a Debug `tur`'s peak
RSS by 51 MB (ASan quarantine off) and now by none; `r7rs-srfi-14` (Release)
peaks at 42 MB, from 48. Pinned by the third program in
`tests/check-turi-frame-reclaim.py`. **Still open:** the rest of section 2 --
the fiber handler case frame, `reset`/`shift` capture frames, the work-stack
perform's `hf`
([turi-effect-perform-keeps-its-continuation](../archive/turi-effect-perform-keeps-its-continuation.md))
and the defer snapshot -- each of which needs a proof that its continuation
or `DeferItem` is done with it.

**Severity:** low (interpreter memory). These are what the 2026-10-05
call-frame reclamation
([turi-call-frames-never-reclaimed](../archive/turi-call-frames-never-reclaimed.md))
left in the pool on purpose, to keep that change small. Neither is a large
share of any r7rs fixture today: the pins are 5% of `r7rs-srfi-14`'s peak.
Both grow with step count, though, so a long enough loop finds them. Found
2026-10-05.

## 1. Tyvar and dictionary pins on call frames

Every driven call of a generic or constrained function hangs per-call records
off its new frame:

- `TyvarBind` nodes from `frame_record_abi` (src/turi/eval.c:6721),
  `frame_pin_hkt_tyvars_from_args` (6827) and
  `frame_pin_bare_tyvars_from_args` (6885);
- `DictBind` nodes from the dict-clone bind and `frame_bind_constraint_dicts`
  (7062).

`frame_release` empties `f->tyvars` and `f->dicts` but leaves the nodes in
`value_scratch`, so each such call leaves them behind even when its frame is
reused.

Measured: `frame_record_abi` is 4.95% (6.6 MB) of `r7rs-srfi-14`'s 132 MB peak
(massif, Release `tur --interpret`).

**Fix:** free lists for `TyvarBind` and `DictBind`, alongside
`env->frame_free` and `env->binding_free`. `frame_release_one` pushes a
frame's chains onto them, and the pin sites pop from them. One check first:
`frame_record_abi` can copy a pin found by `frame_lookup_tyvar` on the
**caller's** chain into the callee. It copies the `Type` by value and does
not share the node, which is what makes releasing per frame safe.

## 2. Frames made off the driver's call path

`eval_frame_new_call` and `eval_frame_new_owned` cover the driver's call
frames, its `let`/`letrec` frames and `eval_match_resolve_with`'s arm
frames. These sites still use plain `eval_frame_new`, so their frames are
never released:

| site | what |
| --- | --- |
| `eval_handle_inner` (eval.c:3279) | the fiber effect path's handler case frame |
| `ts_capture_and_run` (4115) | `reset`/`shift` capture frames |
| work-stack perform (9329) | the handler case frame `hf`; see [turi-effect-perform-keeps-its-continuation](../archive/turi-effect-perform-keeps-its-continuation.md) |
| `EX_EXISTS_OPEN` in `eval_expr_impl` (12849) | the opened existential's frame |
| `EX_DEFER` (12035) | the defer's value snapshot (parentless) |
| closure/tyvar wrapper frames (11619, 13153, 16595) | parentless frames a closure captures; these escape by construction |

**Fix:** for the first four, use `eval_frame_new_owned` where the frame's
lifetime is provably bounded by the enclosing activation. That holds for
`EX_EXISTS_OPEN` (its body is evaluated synchronously), and for a handler
case frame whose `k` does not escape the clause. The defer snapshot can be
released when its `DeferItem` fires. The wrapper frames are captured, so
they stay.

## Pinned by

`tests/check-turi-frame-reclaim.py`: its third program (a generic function
called 300,000 times, the tyvar pins) and its fifth (a `defer` called
300,000 times, the snapshot). The three frames still open have no check yet.

## Fixed 2026-11-06: the handler case frames

- **The work-stack perform's `hf`** (the clause of a handler whose `k` is not
  a one-shot `(resume k v)`). `k` names the handled BODY's frames, not the
  case frame, so the case frame lives exactly as long as the case body runs,
  which ends when the prompt that ran it (`st[pidx]`, `pend_wc` = the
  continuation) delivers. It comes from `eval_frame_new_call` now, the
  continuation records it (`case_frame`, which the one-shot path already
  used), and `DK_PROMPT` releases it there and clears the pointer, so a
  second pop of a copied prompt does nothing. `frame_release` declines a
  frame that escaped -- a closure over a case parameter, a closure over a
  `let` the case frame owns, a capture of the case body's frames by a
  perform inside it all mark it -- and defers one made before a call/cc
  capture.
- **The fiber path's case frame** (`eval_handle_inner`). Its body runs on the
  same C frame, synchronously; it is a call frame released when the body
  returns, with the same two guards.

Measured on a Debug `tur --interpret`, ASan quarantine off: 200,000 handles
whose clause is `(let [a 5] (+ a (resume k 1)))` peaked at 673 MB, against
723 MB before. The rest of that growth is the escaping continuation itself
(its pool-owned slice copy and `k` value), which
[turi-effect-perform-keeps-its-continuation](../archive/turi-effect-perform-keeps-its-continuation.md)
fixed only for the one-shot clause.

Pinned by `tests/fixtures/turi-multishot-case-frame-released`
(`requires.interp-only`): closures over a case parameter and over a case
`let` read their own values after the free list has been reused, and 1000
handles with a non-trivial clause sum right. `run-turi.sh` passes in full.
