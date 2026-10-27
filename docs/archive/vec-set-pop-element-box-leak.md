# vec-set! overwrite / vec-pop! drop orphan a multi-word element box -- RESOLVED

**Severity:** low (process-lifetime; no correctness impact under normal use;
not test-gated -- compiled programs are not run under LSan in `tests/run.sh`).

## Status -- RESOLVED

A multi-word by-value struct/ADT `Vec` element is heap-boxed (the int64 `data[i]`
slot holds a `malloc`'d box pointer -- see
`docs/upcoming/v2/collection-multiword-element-boxing-plan.md`). `vec-free`
already releases every element box (see
`docs/archive/multiword-value-and-vec-element-boxes-leak.md`); the two mutation
paths that orphaned a box are now handled:

- **`vec-set!` overwrite -- RESOLVED.** `vec-set!` is now a macro forwarding
  `(vec-set-o! v i val (tur-vec-elem-wide? v))`.  For a wide by-value element the
  helper `free`s the old `data[i]` box before storing the new value; for a
  single-word element (`tur-vec-elem-wide?` folds to 0) the slot is overwritten
  directly.  Verified LSan-clean on a `Vec[Point]` overwrite (only the unrelated
  process-lifetime `show`-string alloc remains).
- **`vec-pop!` -- RESOLVED via a new primitive + documented contract.** Freeing
  the box *inside* `vec-pop!` is wrong: the returned carrier IS the box pointer,
  which the caller's `(:: (vec-pop! v) T)` deref-copies, so an internal free
  would be a use-after-free.  So `vec-pop!` keeps transferring the box out (its
  docstring now spells out the ownership contract), and a new **`vec-drop-last!`**
  (macro -> `vec-drop-last-o!`) removes the last element AND frees its box for the
  "remove without keeping" case -- the leak-free replacement for a discarded
  `(vec-pop! v)`.  Verified LSan-clean.

### Known inherent limitation (not a leak of the collection)

The *consume-and-drop* pattern on a wide element, `(:: (vec-pop! v) T)`, still
leaks that one box: reading a > 8 byte value out of the int64 carrier requires a
live box pointer for the ascription to deref, and after the value is copied out
there is no handle left to free.  This is a property of the carrier ABI (a
produced wide value is a box the reader derefs), not of `Vec` ownership.  Callers
that must remove-and-inspect a wide last element should `vec-get` then
`vec-drop-last!` (the box stays owned by the Vec until the drop frees it) rather
than `vec-pop!`.

## Verification

```turmeric
(load "stdlib/typeclass.tur")
(defstruct Point :copy [x : int y : int])
(defn main [] : int
  (let [v (vec-of (Point 1 2) (Point 3 4) (Point 5 6))]
    (vec-set! v 0 (Point 9 9))   ;; old box at data[0] freed
    (vec-drop-last! v)           ;; (Point 5 6) box freed
    (println (show (.x (:: (vec-get v 0) Point))))  ;; 9
    (vec-free v)                 ;; remaining boxes freed
    0))
```

`tur emit-c` -> compile `-fsanitize=address,undefined` against
`build/src/libturi.a`, run `ASAN_OPTIONS=detect_leaks=1`: no Point-box leak.
Scalar Vecs (`int`/`cstr`/`float`) thread `boxed=0` and are uncorrupted.  Both
suites green (2122/0 compiled, 1592/0 interpreter); interpreter parity holds via
`native_vec_set_o` / `native_vec_drop_last_o` (which ignore the flag -- the
tree-walker owns elements as TuriStruct pointers, never C boxes).

## Related

- `docs/archive/multiword-value-and-vec-element-boxes-leak.md` -- the resolved
  `vec-free` element-box release.
- `stdlib/vec.tur` (`vec-set!`/`vec-set-o!`, `vec-drop-last!`/`vec-drop-last-o!`,
  `vec-pop!` ownership note).
- `src/turi/collections_native.c` (`native_vec_set_o`, `native_vec_drop_last_o`).
