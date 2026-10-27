# CPS/ANF IR drops the shift receiver -- lossy for non-identity `(shift f body)`

**Status:** RESOLVED (Phase C3, cps-ir-to-c-backend-plan). `cps_ir_translate_fn`
now applies the receiver: `(shift k_fn body)` is translated as the CPS of the
application `(k_fn body)` delivered to the prompt (`cps_shift_body`,
`src/passes/cps_ir.c`). `--dump-cps` shows the receiver call (e.g.
`shift k'. let t1 = call __fn_N(5) in (<prompt> t1)`), and a receiver that is not
a directly-callable binding yields `CT_UNSUPPORTED` (honest fallback, never a
silent miscompile). The interim `recv_identity` guard was removed. Verified:
`(shift (fn[v]v) 10)` and `(shift (fn[v](* 2 v)) 10)` now dump differently and
the CPS backend reproduces the direct-style `10` / `20`.

**Severity (when open):** medium (latent miscompile for any IR consumer that
delivered the shift body value directly).

## Summary

`cps_ir_translate_fn` lowers `(shift f body)` / `(shift0 f body)` by translating
only `shift_.body`, **discarding the receiver `shift_.k_fn`**. The abortive
semantics of a plain Turmeric shift is `r = f(body-value)` delivered to the
enclosing `reset` (see `eval_abortive_shift`, `src/turi/eval.c:1561-1578`), but
the IR records only `body-value`. Consequently:

- `(shift (fn [v] v) x)` and `(shift (fn [v] (* 2 v)) x)` produce **identical**
  CTerms (`shift k'. (<prompt> x)`).
- Any backend that lowers `CT_SHIFT` by delivering the body value to the prompt
  computes the identity-receiver result for *all* receivers -- a silent
  miscompile for non-identity `f`.

The tree-walking interpreter and the direct-style/abortive emit path are
correct (they apply `f`); only the ANF/CPS IR is lossy.

## Minimal repro (IR level)

```turmeric
(defn a [] : int (reset (shift (fn [v] v)     10)))    ; = 10
(defn b [] : int (reset (shift (fn [v] (* 2 v)) 10)))  ; = 20 (direct-style)
```

`tur check --dump-cps` shows the `shift` bodies of `a` and `b` as the same
`shift k'. (<prompt> 10)` -- the `* 2` receiver in `b` is gone. Direct-style
runs give `10` and `20`; an IR consumer that trusts the dump would give `10`
for both.

## Root cause

`src/passes/cps_ir.c`, both `EX_SHIFT` arms (`cps_tail` ~line 255, `cps_bind`
~line 365):

```c
CTerm *t = new_term(b, CT_SHIFT);
t->as.shift.k = k;
t->as.shift.body = cps_tail(b, e->as.shift_.body, kont_prompt(...));
/* e->as.shift_.k_fn is never consumed */
```

## Interim guard (already landed)

`cps_ir.h`'s `CT_SHIFT` now carries `bool recv_identity`, set by
`shift_recv_is_identity(e->as.shift_.k_fn)` (structural check for `(fn [v] v)`).
The CPS-IR-to-C backend only lowers a `CT_SHIFT` when `recv_identity` is true and
falls back otherwise, so it never miscompiles. This is a guard, not a fix: the
IR still cannot *represent* a non-identity receiver.

## Fix directions

- Incorporate the receiver into the translation: lower `(shift f body)` as the
  CPS of applying `f` to the body value, i.e. deliver `f(body-value)` to the
  prompt. For an inline `(fn [v] E)` receiver this is `cps` of `E[v := body]`;
  for a named/opaque receiver it is a `CT_LETCALL`/`CT_TAILCALL` to `f` bound to
  the delivered value. Then `recv_identity` becomes unnecessary and backends are
  correct for all receivers.
- Until then, keep the `recv_identity` guard on every IR consumer, and consider
  having `--dump-cps` print the receiver (or a `<non-identity receiver>` note)
  so the dump is not silently wrong.
