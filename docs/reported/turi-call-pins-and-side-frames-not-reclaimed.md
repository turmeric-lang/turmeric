# `tur --interpret`: a call's tyvar/dictionary pins, and frames made off the driver's call path, are never handed back

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
| work-stack perform (9329) | the handler case frame `hf`; see [turi-effect-perform-keeps-its-continuation](turi-effect-perform-keeps-its-continuation.md) |
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

Nothing yet.
