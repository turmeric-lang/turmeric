# A by-value recursive local lent across a mutual-recursion cycle is treated as moved, and leaks

**Severity: low (a leaked spine per call, only inside a cycle of functions
that take a by-value recursive ADT).** Filed 2026-11-28, the residue of
ordering Pass 2 so a callee with a by-value recursive ADT parameter is
elaborated before its callers (`fwd_gen_order`, `fgo_ann_is_rec_adt`,
`src/compiler/elab_toplevel.c`; pinned by
`tests/fixtures/byval-recursive-adt-forward-callee-lent`).

That ordering is what lets a caller read the callee's inferred non-retaining
/ result-alias mask (`nonretain_ptr_param_mask`, `resalias_param_mask`). A
cycle cannot be ordered: it is broken at its first such member, which still
sees the others' pass-1 forward declarations, whose masks are 0.

## Repro

```turmeric
(defdata Lst [] (Cons [hd : int tl : Lst]) (Nil))

(defn llen [xs : Lst] : int
  (match xs
    (Cons h t) (+ 1 (llen t))
    (Nil)      0))

(defn p1 [xs : Lst n : int] : int
  (if (= n 0)
    (llen xs)
    (let [ys (Cons n (Nil))]
      (+ (p2 ys (- n 1)) (llen xs)))))

(defn p2 [xs : Lst n : int] : int
  (if (= n 0)
    (llen xs)
    (let [ys (Cons n (Nil))]
      (+ (p1 ys (- n 1)) (llen xs)))))

(defn main [] : int
  (let [zs (Cons 1 (Nil))]
    (println (p1 zs 4)))     ; 5
  0)
```

Measured 2026-11-28 under `tests/run-leak-check.sh`'s flags: 48 B in 2
allocations -- `p1`'s `ys`, once per call of `p1` that recurses.

## Root cause

`p1` is elaborated first. Its call `(p2 ys ...)` reads `p2`'s forward
declaration, so `p2`'s first parameter is not known to be non-retaining, and
the call MOVES `ys` (`binding_mark_moved`, `src/compiler/elab_call.c`) where
it would have LENT it (`binding_mark_lent`). `p1`'s `let` then does not free
`ys` (`drops_local_owned`, `src/compiler/elab_forms.c`). `p2` is inferred
non-retaining afterwards, so it does not discharge its parameter either, and
nobody frees the spine. `p2`'s own `ys` is lent to the already-inferred `p1`
and is freed.

## Fix directions

- **Solve the cycle's ADT masks together**, as the fn-parameter masks
  already are (`elab_nonretain_resolve_forward`, `src/compiler/elab_fns.c`):
  every candidate bit of the group set, cleared against the others' current
  masks until nothing changes.
- **Then revisit what was decided against the 0 mask**: a move into a group
  member that turns out to lend is a lend, and the `let` whose local it was
  gets its `drops_local_owned` decision again. Both are elaboration-time
  facts, so this needs the move recorded as tentative (callee, parameter) and
  the `let` recorded for a retry after the group is solved.
- Raising a mask after the fact is otherwise safe: it only turns a callee
  from consuming into lending, and the emitter's provenance
  (`own_compute`, `src/compiler/emit_core.c`) and its lent-temporary free read
  the final masks.
