# A serial-shift receiver's continuation chain is never freed

**RESOLVED 2026-10-03** (fix direction 1).  See *Resolution* at the end; the
residue -- a `k` the receiver lets escape, and a `serial-cont` minted by
`bytes->serial-cont` -- is filed as
[serial-cont-escaped-or-deserialized-chain-leaks](../reported/serial-cont-escaped-or-deserialized-chain-leaks.md).

**Severity: low (leak; one chain per capture).**  Filed 2026-10-03, found
while leak-checking the fixtures for
[serial-receiver-effect-under-if-closure-or-leaf](../reported/serial-receiver-effect-under-if-closure-or-leaf.md).
Pre-existing on `main`, on the native serial lowering and the outward one alike.

## Repro

```turmeric
(load "stdlib/serial.tur")
(defn page [env : cstr hole : int] : int (+ hole 1))
(defn recv [k : serial-cont] : int (k 41))
(defn run [] : int
  (serial-reset (page "" (serial-shift recv 0))))
(defn main [] : int
  (println (run))
  (println (run))
  0)
```

Built the way `tests/run-leak-check.sh` builds (`TUR_RUNTIME=source`,
`-fsanitize=address`, `detect_leaks=1`):

```
SUMMARY: AddressSanitizer: 720 byte(s) leaked in 6 allocation(s).
Direct leak of 240 byte(s) in 2 object(s) ... in dk_new
Indirect leak of 480 byte(s) in 4 object(s) ... in dk_new
```

Three `DK` nodes per capture: the copied prompt, the `page` frame, and the
`dk_done` tail.  The existing fixtures show the same: `serial-shift-colored-receiver`
leaks 52 allocations, `cps-oracle-serial-closure-recv` 25,
`serial-shift-receiver-effect-reaches-handler` 12.  None of them carries
`requires.leak-check`, which is why nothing reports it.

## Root cause

The shift body hands the receiver a COPY of the captured chain
(`DK *__cap = dk_copy_range(subk, NULL)`, `run_skbody<N>` in the emitted C), and
the outward lowering hands it the freshly built chain (`emit_serial_outward_call`);
"the receiver owns `dv`" either way.  But a `serial-cont` is multi-shot and
marshalable: `tur_serial_cont_resume` is `dk_invoke(k, v)`, which runs a copy
and leaves `k` intact, because the receiver may resume it again, serialize it,
or store it.  So no use of `k` is a last use the compiler can see, and nothing
ever frees the chain.

## Fix directions

1. **Free on the receiver's return when `k` cannot have escaped.**  The
   receiver's body is visible; if every use of `k` is a direct
   `(k v)` / `resume-cont!` / `serialize-cont` call and `k` is not stored,
   returned or captured, the shift body (or the outward call's resume frame)
   can `dk_free` the chain after the receiver returns.  That covers every
   fixture above.
2. **Give `serial-cont` an owner type.**  A linear/affine continuation
   (consumed by `resume`, cloned explicitly) makes the last use syntactic.  A
   bigger change to a surface that is documented as aspirational
   ([serializable-continuations-aspirational-surface](serializable-continuations-aspirational-surface.md)).

Either way a fixture with `requires.leak-check` belongs with the fix.

## Resolution

Direction 1, in `src/compiler/emit_cps_ir.c`:

- **The confinement walk** (`serial_recv_confines_k` / `serial_k_use_ok`).  The
  receiver's body (a named defn's, or the closure literal's) is walked for every
  mention of its `k` parameter.  `k` is *confined* when each mention is a read
  that copies: the builtin `(k v)` elaborates to (`tur_serial_cont_resume`;
  likewise `tur_serial_cont_serialize`); a call to a
  Turmeric-bodied defn whose own parameter is confined, walked up to four calls
  deep (`serial-resume`, `workflow-suspend`, a user helper that only resumes);
  or one of the stdlib's inline-C marshalers (`serial-cont->bytes`,
  `save-cont!`), trusted by name only when the definition comes from a
  `stdlib/` file.  Anything else -- `k` stored, returned, bound by a `let`,
  captured by a lambda, handed to an indirect call or an unknown inline-C
  body -- keeps the old behaviour.  An inline-C receiver is never confined.
- **Native path.**  The shift body frees its copy after a confined receiver
  returns: `intptr_t __r = <recv>(__cap); if (!tur_async_suspended)
  dk_free(__cap); return __r;`.
- **Outward path.**  The receiver's call is the reset function's tail, so the
  chain cannot be freed after it; it is registered with the entry boundary's
  reap instead (`__dk_reap_keep(dv)`), which frees it at `__dk_enter`'s exit.
- **The receiver lambda's own box** (found on the way: 24 bytes per capture for
  a closure receiver).  The closure literal rides only the `dk_shift` node's
  env, and the chain the shift captures starts below that node, so it is dead
  once `dk_run` returns -- `TUR_CLOSURE_DROP` it there
  (`emit_cl_shift_env_drop`), on the serial and the cloneable shift alike.
- **`resume-cont!`** (`stdlib/workflow.tur`) freed nothing: it resumed (a copy
  of) the chain it had just deserialized and dropped the chain.  It now
  `dk_free`s it; the bytes stay the caller's, since they are what a caller
  resumes again.

The repro is clean (720 bytes in 6 allocations -> 0).  So are
`serial-shift-receiver-effect-reaches-handler`,
`serial-shift-receiver-effect-under-if` and
`serial-nonserial-in-scope-not-captured`, which now carry `requires.leak-check`,
and the new `serial-cont-receiver-chain-freed` holds a named receiver, a double
resume, the stdlib reader, a resuming helper, an `if` that drops `k` on one arm,
and a capturing closure receiver at zero.  Every other serial fixture leaks
less; what each still leaks is the residue filed separately -- the bytes and the
deserialized chain of a round trip the fixture spells by hand, or a `k` a
lambda captures (`serial-shift-colored-receiver`'s `helper`).

Suite: `bash tests/run.sh` 3556 passed, 0 failed; `tests/run-leak-check.sh`
121 passed, 0 failed, 3 known-open; turi and JIT on the serial/workflow
fixtures green.
