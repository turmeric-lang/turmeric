# turi call/cc reclaim fails when sizeof(Type) changes

**Severity:** low-medium (test fragility, possible memory retention). The
`tur_turi_callcc_reclaim` ctest passed or failed on a change that only grew
`Type`, a compiler struct the interpreter's evaluation of the test program
does not touch.

## Repro

Measured while landing refined-function-types-plan F1, which first added four
pointers to `Type`'s `as.fn` arm (`sizeof(Type)` 128 -> 160 bytes):

```sh
python3 tests/check-turi-callcc-reclaim.py ./build/tur
```

| `sizeof(Type)` | continuations freed (stress) | stored-continuation growth | verdict |
|---|---|---|---|
| 128 (`main`) | 2011 | +4 MB | OK |
| 160 | 1963 | +51 MB (bound 28 MB) | FAIL |
| 128 again, same feature behind one pointer | 2011 | +2 MB | OK |

The sweep-off figure was +135 MB in all three, so the cost of one image did
not change -- what changed is how many images the sweep frees.

## Root cause (hypothesis, not confirmed)

The continuation sweep (`r7k_maybe_sweep`, `src/turi/interpreter_natives.c`;
`turi_cont_gc_*`, `src/turi/eval.c`) marks conservatively from the C stack.
A frame layout change (some frame on the path holds a `Type` by value) can
leave a stale word that looks like a continuation handle in a slot the
sweep scans, and each image reaches the previous one, so one stale root
keeps a chain alive. Not verified by tracing which word is retained.

## Fix directions

- Confirm with `TUR_TURI_CONT_GC_TRACE` which root keeps the chain.
- If it is a dead slot in a long-lived frame, clear it, or scan only the live
  part of the frames the sweep must see.
- Until then, a change to `sizeof(Type)` (or to frames on the interpreter's
  eval path) is worth checking against this test locally; it runs only in the
  auxiliary CI leg.
