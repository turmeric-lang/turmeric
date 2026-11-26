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
