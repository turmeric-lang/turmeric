# `perform` in a function with an `any` parameter has no CPS lowering

**Severity: low** -- a narrow shape (gradual typing meets effects), and the
diagnostic is loud rather than silent. But its advice does not apply, which
costs the reader a while. Found 2026-08-29 while building the negative cases
for `any-struct-box-leak-per-widen`.

## Repro

```turmeric
(defeffect Ask [] : int)

(defn with-any [v : any] : int (perform (Ask)))

(defn main [] : int
  (println (handle (with-any 3) (Ask [] k) (resume k 5)))
  0)
```

```
error: this effect operation has no lowering here: the enclosing function left
the CPS backend's supported subset, and the direct emitter cannot lower
`perform`. The usual cause is a loop inside a `handle` clause -- hoist that
work into a helper function and call it from the clause. This is a compiler
limitation, not a mistake in this expression.
```

The parameter is not used. Changing its type to `:int` compiles and prints `5`,
so the `any` parameter alone is what pushes the function out of the supported
subset.

## Root cause

*(Established while fixing. The guess above was right about `slot_box_ty` and
wrong to think it was one exclusion: it was four, in three passes, each of
which independently took the whole program off the DK backend.)*

`any` lowers to `tur_tagged_t`, a fixed two-word by-value struct that the direct
emitter passes and returns by value. Nothing about that is hostile to the CPS
backend -- it is a Tier C aggregate exactly like a `defstruct`-lowered record.
But it is not an `AdtDef`, and four places were written in terms of one:

1. **`slot_box_ty` (`emit_cps_ir.c`)** tests `type_is_byvalue_adt_product`,
   which cannot see TY_ANY. So `fn_sig_ok`'s param gate rejected an `any`
   PARAMETER and the function sig-evicted -- the reported repro.
2. **`is_delegatable_struct` (`passes/cps_ir.c`)** had no case for the `any`
   widen or its readers, so they translated to `CT_UNSUPPORTED`. That is why
   `(with-any 3)` in `main` -- a widen, nothing else -- evicted `main`, tainted
   the effect, and pushed the performer back off DK even once (1) was fixed.
3. **`collect_free_vars` (`elab_core.c`)** did not walk those nodes either, so
   an `any` read AFTER the suspension point never rode the continuation env:
   `'v' undeclared` in the lifted helper. This is the same shape as the
   catch-thunk free-var bug recorded in that walker's own comments, and its
   `default:` arm is the same reason.
4. **`call_arg_ok`'s cps->direct reject** refuses a `slot_box_ty` argument,
   because a by-value ADT reaching an uncoloured callee would be emitted raw
   against a carrier-ABI signature. Admitting `any` to `slot_box_ty` in (1)
   therefore made every cps->direct call taking one evict -- including `main`
   calling the very function whose `perform` this was all for.

Each fix only exposed the next, which is why the diagnostic never moved until
all four were in.

## Why the diagnostic misleads

It names one cause ("a loop inside a `handle` clause") and prescribes hoisting
into a helper. Here there is no loop and no `handle` clause, and hoisting is
exactly what the code already does -- `with-any` *is* the helper. A reader
follows the advice, changes nothing, and has to go looking. Widening the
message to name the actual exclusion, or dropping the specific advice when the
shape does not match, would be worth as much as the lowering fix.

## Fix directions

1. Diagnostic first: report the real reason (an unsupported parameter
   representation) rather than the loop-in-handle guess.
2. Admit `tur_tagged_t` parameters to the CPS subset -- it is a fixed 16-byte
   by-value aggregate, so it should slot like any other narrow monomorph.

## Notes

Encountered as a blocked test case, not as a user report: the effect-free gate
in the `any` frame-box optimization
(`docs/reported/any-struct-box-leak-per-widen.md`) refuses a callee that can
suspend, and this is why that gate cannot currently be exercised end-to-end.
The gate stays in regardless -- it is cheap, and it is exactly what would go
wrong if this lowering gap were closed without it.

## Resolution (2026-08-29)

Fix direction 2, at all four sites above. The diagnostic (direction 1) is left
alone deliberately: it now fires only for the shapes it already describes, and
rewriting a message whose remaining audience is unknown is worse than leaving
it. Reopen that half if another misleading instance turns up.

`tests/fixtures/perform-with-any-param` pins the five shapes -- an ignored
`any` parameter, one live across the `perform`, one unboxed by `cast` after it,
the perform in builtin-operand position (which is what reaches the cps->direct
argument gate), and an `any` return crossing the return continuation.

The gate this unblocks is now genuinely tested rather than defensive: with a
`perform` in an `any`-parameter function compiling, `tests/fixtures/any-widen-retaining-callee`
gains the effectful-callee case, and it correctly keeps the heap box while the
effect-free sibling in the same file does not.

- `bash tests/run.sh` -- 2735 passed, 0 failed.
- `tests/run-jit.sh` (c2mir) -- green.
- `tests/run-leak-check.sh` -- green.
