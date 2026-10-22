# A struct pushed onto a global Vec at the REPL dangled across the scratch rewind

**RESOLVED 2026-10-21** -- found and fixed in the same change, while making
the datalog examples run in the REPL. Pinned by
`test_vec_struct_cells_relocated` in `tests/turi/env-longlived.c`.

**Severity was:** high for the REPL (a use-after-free of interpreter values:
ASan use-after-poison, garbage on read), latent since scratch promotion went
on by default in the REPL.

## The bug

```
$ tur repl
=> (defstruct P :copy [x : int])
=> (def v (:: (vec-new) (Vec P)))
=> (vec-push! v (P 1))
==ERROR: AddressSanitizer: use-after-poison ... in collmark_value
```

The struct `(P 1)` is allocated in the env's rewindable `value_scratch` pool.
`vec-push!` stores its pointer in the Vec's malloc'd data buffer. At the eval
boundary, `turi_promote_escaping` checks and copies what the roots (the result
and the globals) reach, but the global `v` is a Vec handle -- an opaque int
carrier into malloc'd memory -- so the walk never looked inside the buffer.
The rewind then reclaimed the struct with the cell still pointing at it, and
the TR3 sweep's mark (`vec_buf_scan` -> `collmark_value`) read it.

Running the same program as a file (`tur --interpret`) was fine: one eval, no
boundary in between. The datalog examples, typed onto `(Vec Datum)`, hit it as
soon as their database lived in a `def` across prompts.

This is the carrier-opacity problem of
[tvar-cell-dangled-across-promotion-rewind](history/tvar-cell-dangled-across-promotion-rewind.md)
one level down: that fix moved the TVar CELL out of scratch, but a box's
contents are the program's values and cannot simply be malloc'd.

## The fix

Promotion is collection-aware (`src/turi/eval.c`, "Collection cells at
promotion"):

- **Pass 1** (`promo_check_coll_bufs`) runs the sweep's own marker over the
  pre-rewind roots, to find the tracked boxes they reach, then requires each
  reached box's cells to survive the rewind. A box with a relocate hook has
  its cells `promo_check`ed; a box without one (Set/Map, TVar) must hold no
  scratch pointer at all, or the rewind is declined for this cycle
  (`promo_decline_unrelocatable`), as for any other unrelocatable shape. Only
  reached boxes are asked, so a garbage box holding a stale scratch word does
  not block every later rewind -- unless the mark was incomplete (a non-empty
  Set/Map can hide a box behind an untyped entry), when every live box is.
- **Pass 2** (`promo_relocate_coll_bufs`) moves the cells of every checked box
  with a hook through the roots' forwarding table, so a value shared by a root
  and a cell stays one value.
- `TuriCollBuf` gained a `relocate` hook (`src/turi/env.h`); the Vec's
  (`vec_buf_relocate`, `src/turi/collections_native.c`) moves its struct,
  closure and string cells, identified by their per-cell tags.

The test pushes a struct with a runtime-built string field, and a bare string,
onto two global Vecs across 50 evals: every boundary still rewinds (no
declines), and every element reads back. Before the fix it stops at the first
boundary with ASan use-after-poison.

## Not covered

- `vec-new-filled` builds an UNTRACKED Vec box (its tracking slot is 0), so
  neither this walk nor teardown sees it. A struct stored into one with
  `vec-set!` at the REPL would still dangle.
- A Set/Map or TVar that holds a scratch value now declines the rewind
  instead of dangling: correct, but such a session reclaims nothing until the
  value is gone. Giving them relocate hooks would recover it.
