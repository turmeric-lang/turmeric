# A value stored in a container has no owner, so what it owns is never freed

**Severity: low (a leak per stored element, only where a program frees its
container or would have).** Filed 2026-11-28 to hold the one question two
reports were left with once everything else in them was fixed:

- [byvalue-recursive-shared-copies-leak](../archive/byvalue-recursive-shared-copies-leak.md)
  (item 2): a by-value recursive ADT element's SPINE;
- [fn-param-at-type-variable-slot-boxed-per-call](../archive/fn-param-at-type-variable-slot-boxed-per-call.md)
  (the stored shape): the 32 B fat-handle box a `(fn ...)` parameter is put
  in at a type-variable slot, when that slot is `vec-push!`'s element.

Neither is a hole in an analysis. A container does not own what its elements
own, so there is nobody to free it.

## Repro

```turmeric
(defdata Lst [] (Cons [hd : int tl : Lst]) (Nil))

(defn stash [d : (fn [int] int) v : (Vec (fn [int] int))] : int
  (vec-push! v d)
  1)

(defn plus1 [x : int] : int (+ x 1))

(defn main [] : int
  (let [v (vec-of (Cons 1 (Cons 2 (Nil))))]
    (vec-free v))                ; the element's two boxes leak
  (let [fs : (Vec (fn [int] int)) (vec-new)]
    (stash plus1 fs)
    (vec-free fs))               ; the 32 B box `stash` pushed leaks
  0)
```

Measured 2026-11-28 under `tests/run-leak-check.sh`'s flags: 80 B in 3
allocations -- the 32 B box `stash` pushed, and the element's two 24 B boxes.

Pinned today by `tests/fixtures/byval-recursive-adt-shared-copy-not-freed`
(`known-leak`, 128 B in 5 allocations: its `Vec`, which it never frees, the
buffer, and the element's spine) and by
`tests/fixtures/fn-param-through-type-variable-slot` (not leak-checked: its
`via-vec` and `keep` boxes, and the `Vec`s that hold them).

## Root cause

- **A compiled `Vec` local is not freed at scope exit** -- `vec-free` is the
  explicit release, and a region reclaims a non-escaping one. A program that
  never calls `vec-free` leaks the container and everything in it, by design.
- **`vec-free` releases slots, not what an element owns.** `vec-free-o`
  (`stdlib/vec.tur`) frees the buffer and, through its `owned` bits, a wide
  by-value element's box (bit 0, `tur-vec-elem-wide?`) and an `rc` element's
  count (bit 1, `tur-vec-elem-rc?`, whose matching increment is stamped at the
  push site by `own_carry_for_arg`, `src/compiler/elab_call.c`). Nothing walks
  an element's own drop glue: a recursive element's spine (`drop_localowned_<T>`)
  or a fat closure handle (`TUR_CLOSURE_DROP`).
- **Not every element is the container's to free.** An element can be pushed
  from a value something else still owns -- a `^borrow`, a global, another
  container's element, a let-bound closure its `let` drops -- and copied out
  again by `vec-get` as a shared view. An element drop at `vec-free` is sound
  only once every way in hands the container an owner.

## Fix directions

- **Spines: drop at `vec-free`, own at the push.** Bit 2 of `owned` for an
  element type `emit_own_adt_of` accepts: `vec-free-o` runs
  `drop_localowned_<T>` on each element box before freeing it, and
  `vec-set!` / `vec-drop-last!` on the element they discard. The push site
  must then hand over an owner: a moved owned value already is one; a shared
  view gets the deep copy clone at the borrow added for consuming parameters
  (`elab_clone_shared_spine`, `clone_localowned_<T>`), which today refuses a
  type-variable parameter such as `vec-push!`'s `val`.
- **Closure handles: the same, for the boxes the container mints.** A box an
  `EX_POLY_TO_FAT` makes at the push is the container's alone and can be
  dropped by it; a handle pushed as-is (a let-bound closure, a parameter) is
  not, and closures have no clone glue, so those keep the status quo. The
  element bit then has to know which slots it minted -- or the push wraps
  every handle in a box of its own, which is the case the poly carrier
  already pays for.
- **Scope-exit release of a non-escaping `Vec` local** is the larger half: it
  is what makes either of the above matter in a program that does not call
  `vec-free`.
