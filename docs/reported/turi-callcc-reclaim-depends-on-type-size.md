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

## Reproduced and traced (2026-11-05)

`sizeof(Type)` is 128 on `main` and the check passes (2010 continuations
freed, +2 MB). Growing `Type` by four pointers at the end of the struct
(`void *pad[4];` before `} Type;` in `src/compiler/types.h`, so 160 bytes)
fails it exactly as filed: 1963 freed, +49 MB against a 28 MB bound. Do it
in a `git worktree` with its own build directory, so the main build is not
disturbed.

The retained chain, from a temporary trace in `r7k_gc_word` plus each
worklist entry's parent: over a 3000-iteration stored-continuation loop the
third sweep keeps 158 images, and every one of their marks arrives in LIVE
context, never directly from an image word. The path is: image N holds a
word naming a call frame made two captures earlier (it passes `gc_word`'s
"made since the capture" filter); that frame was pushed and later scanned
in live context; one of its bindings holds continuation N-1's handle; and
image N-1 does the same to N-2. So the stale word is a dead slot naming a
frame whose activation had already ended. The frames involved are
`reclaimable` call frames that had ESCAPED, so `frame_release` never hands
them back or defers them and nothing records that they ended; another 640
escaped, non-call frames with bindings are reached the same way.

**Tried, not landed:** stamp a frame (and the frames it owns) as ended in
`frame_release`, escaped or not, and have `gc_word` skip, from an image, a
frame that ended before that image's capture, until the first restore of a
re-entrant image (a restore can complete an ended activation again). At 160
bytes this raised the stress figure from 1963 to 2003 freed, but the
memory loop still grew +48 MB. The chain moves to the non-call frames, which
are not on any owner's list and are made at a site not yet found
(`eval_frame_new_owned` under an escaped owner is not it: linking those to
their owner changed nothing). Finding that site, and stamping its frames'
end the same way, is the next step; the skip rule itself looked sound for a
program with no restore.

## Fix directions

- ~~Confirm which root keeps the chain~~ -- done 2026-11-05, above.
- If it is a dead slot in a long-lived frame, clear it, or scan only the live
  part of the frames the sweep must see.
- Until then, a change to `sizeof(Type)` (or to frames on the interpreter's
  eval path) is worth checking against this test locally; it runs only in the
  auxiliary CI leg.
