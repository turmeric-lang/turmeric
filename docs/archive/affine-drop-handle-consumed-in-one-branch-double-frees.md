# An `^affine` handle with a `Drop` instance, consumed in one `if` branch, is freed twice; consumed in both, it is rejected

**Severity:** high (memory safety: a double free from a program that compiled
with no diagnostic), plus a false rejection of the correct program beside it.
`^affine` + `Drop` is the ownership shape `stdlib/numvec.tur` uses and
documents, so this is the recommended way to write an owning handle.

The affine check does not look at branches. A handle moved into a consuming
call on one side of an `if` is still dropped at scope exit, so on that path it
is freed twice. Trying to write the program correctly -- consume it on both
sides -- is refused with TUR-E0150, "used more than once". `^linear` gets both
right: it rejects the first with TUR-E0104 and accepts the second.

Found 2026-11-26 choosing an ownership model for `spices/manifold` in
turmeric-spices; it is why that spice's handles are `^linear`. Verified on
`main` at `d4ad1b0`.

## Repro

The same 16-byte heap handle is used throughout:

```turmeric
(defopaque ^affine H :int)

(defn h-new [n : int] : H
  ```c
  #include <stdlib.h>
  int64_t *p = malloc(sizeof *p);
  *p = n;
  return (intptr_t)p;
  ```)

(defn h-free [h : H] : void
  ```c
  #include <stdlib.h>
  free((void *)(intptr_t)h);
  ```)

(defn h-peek [^borrow h : H] : int
  ```c
  return *(int64_t *)(intptr_t)h;
  ```)

;; Consumes its argument, returns a fresh handle holding one more.
(defn h-bump [h : H] : H
  ```c
  #include <stdlib.h>
  int64_t *o = (int64_t *)(intptr_t)h;
  int64_t *p = malloc(sizeof *p);
  *p = *o + 1;
  free(o);
  return (intptr_t)p;
  ```)
```

### 1. One branch consumes: compiles, then double-frees

Save the prelude plus this as `a1/main.tur`:

```turmeric
(definstance Drop [H] (drop [x] : void (h-free x)))

(defn work [early : bool] : int
  (let [h (h-new 7)]
    (if early (h-free h) nil)
    1))

(defn main [] : int
  (println (work false))
  (println (work true))
  0)
```

```sh
tur check a1/main.tur                      # no diagnostic
TUR_SANITIZE_PROGRAMS=1 tur test a1
```

```
==246==ERROR: AddressSanitizer: attempting double-free on 0x502000000030 in thread T0:
SUMMARY: AddressSanitizer: double-free ... in free
```

`(work true)` frees `h` in the branch and the scope-exit drop frees it again.

### 2. Both branches consume: rejected

Prelude plus:

```turmeric
(definstance Drop [H] (drop [x] : void (h-free x)))

(defn work [once : bool] : int
  (let [h (h-new 7)
        g (if once (h-bump h) (h-bump (h-bump h)))]
    (h-peek g)))

(defn main [] : int
  (println (work true))
  (println (work false))
  0)
```

```
error [TUR-E0150]: affine value 'h' used more than once
```

Each path uses `h` exactly once. The same program is rejected without the
`Drop` instance, so this half is the affine check alone, not the drop glue.

### Control: the same two programs with `^linear`

Change the prelude's first line to `(defopaque ^linear H :int)` and drop the
`definstance`. Program 1 is then rejected at compile time:

```
error [TUR-E0104]: linear value 'h' consumed in one branch but not the other
```

and program 2 (with an `(h-free g)` after the `h-peek`, since nothing drops it)
compiles and runs clean under LeakSanitizer.

## Root cause

Not traced to a line. The two diagnostics involved live apart:
`TUR_E0104_LINEAR_BRANCH_MISMATCH` is raised from the branch join in
`src/compiler/elab_forms.c:4381` (and `elab_structs.c:5282` for `match`),
while `TUR_E0150_AFFINE_USED_TWICE` comes from `elab_forms.c:2401`, which reads
as a straight count of uses. So the linear path has a per-branch consumed set
that is compared at the join, and the affine path appears not to. The scope-exit
drop then has no per-path "already moved" fact to consult.

## Fix directions

1. Give affine values the same branch-join treatment linear ones get: track
   "moved" per branch, and at the join either require agreement (as E0104
   does) or, since affine values may be dropped, synthesise the drop on the
   branch that did not move it. The second is what makes program 1 legal *and*
   safe, which is the behaviour an affine + `Drop` type is supposed to have.
2. Count uses per path, not per function, so program 2 is accepted.
3. Until then, say so in `stdlib/numvec.tur`'s ownership note and in
   `substructural-types-guide.md`: an `^affine` + `Drop` value must not be
   consumed inside a branch.

## Resolved 2026-11-26: fix directions 1 and 2

**Root cause, traced.** `elab_if` snapshots `is_moved` per branch and merges
it as `before || (then && else)`, so a local moved on one side read as *not
moved* after the `if`, and `elab_let` appended its scope-exit drop: the
consuming path freed it twice. The ST1 usage count that raises TUR-E0150 was
not snapshotted at all, so a use in each branch counted as two.

**What changed** (`src/compiler/elab_forms.c`, `elab_structs.c`):

- The usage count is rewound before each arm and merged as the maximum, in
  `if` and in both `match` paths. Program 2 compiles.
- `Binding.scope_drop_inst` records, when a let binding is made, the Drop
  instance its scope exit would release it through. At a join, a local that
  owes that drop and was consumed by some arms but not others is released by
  the arms that kept it (`elab_branch_with_scope_drop` appends the same
  `(defer (<Drop.drop> b))` glue to the arm), and reads as moved afterwards.
  An `if` with no else gets one. Program 1 compiles and frees once per path.
- "Consumed" is a move, or for an affine binding a use that survived (a
  `^borrow` use is un-counted by the call), which covers a branch whose own
  value is the local.
- A branch that leaves no longer votes in `if`'s merge (it already did not in
  `match`): the state after the `if` is the other branch's. A `return` arm
  that still owns the local runs the drop itself, after computing its value
  (`return_.owed_drops`, honoured by the C emitter in statement and tail
  position and by the interpreter). A `return` reached after the local was
  handed on records it (`return_.moved`), and the emitter's pending-drop pass
  skips it.
- The literal `match` path had no per-arm move state; it shares the ADT path's
  now (`ArmMoves`).
- `init_yields_fresh_owner` follows an `if` whose arms all yield fresh values,
  so the `g` of program 2 is owned and dropped.
- Handing such a local on inside a `while` body is TUR-E0005.

**Found on the way, same cause, all changed by the above** (each was run
under `TUR_SANITIZE_PROGRAMS=1` on `main` at `d4ad1b0` and on the fix):

| Shape | Before | After |
|---|---|---|
| `(when c (h-free h))` | double free | clean |
| `(if c (return 0) (h-free h))` | double free | clean |
| `(if c (return (h-take h)) nil) (h-peek h)` | TUR-E0150 | clean |
| one arm of a constructor `match` consumes | double free | clean |
| one arm of a literal `match` consumes | leak on the other arms | clean |
| `(if c (h-free h) nil) (h-peek h)` | TUR-E0150 | TUR-E0201 (use after move) |
| `(while ... (h-free h))` | double free | TUR-E0005 |

Pinned by `tests/fixtures/affine-drop-branch-paths` (25 path runs, counted
allocations and frees, also under `run-leak-check.sh`),
`affine-drop-branch-paths-trace` (no inline C, so the interpreter runs it),
and `errors/affine-drop-moved-in-loop-body` /
`errors/affine-drop-used-after-branch-move`.

**Not changed.**

- An owned Drop-opaque *parameter* is never dropped by its function, in a
  branch or otherwise: `(defn take [h : H c : bool] : int (if c (h-free h) nil) 1)`
  leaks `h` when `c` is false. Only let-bound fresh owners carry a scope-exit
  drop. A leak, not a double free; it predates this report.
- A branch that panics while still owning the local does not release it.
- A `do` that ends in `return` is not recognised as leaving (only a bare
  `(return ...)` arm is), so `(if c (do (h-free h) (return)) nil)` followed by
  a use of `h` is refused as a use-after-move although no path reaches it
  moved.
- The other scope-exit drops `elab_let` injects (`ref<T>`, `rc<T>`, owning
  fields) take the same one-decision-per-let shape and were not examined.
- CPS-lowered (colored) functions do not go through the `return` emitter that
  reads `owed_drops` / `moved`.

Each of these was measured and filed on 2026-11-27, under "Follow-ups to
#1486" in `docs/reported/README.md`. Two findings came out of that pass that
are not in the list above: the per-path `return` records are lost in any
program that contains an effect (`cps-pass-drops-return-drop-lists`, a
regression from this fix), and the `ref<T>` half of the fourth item is a
double free (`ref-moved-in-if-without-else-double-frees`).
