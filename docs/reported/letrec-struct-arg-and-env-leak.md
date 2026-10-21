# A `letrec` closure leaks its struct arguments, and its env when it opens an existential

**Severity: low-medium (leak per call, not per program).** Found 2026-10-21
running tur-plot under LeakSanitizer (plot-renderer-typeclass-plan P4): its
two `letrec` folds over a `(Vec (exists ...))` leaked on every `plot` call,
and were rewritten as top-level recursive `defn`s, which allocate nothing.

Two leaks, both in emitted C, both invisible to `tests/run.sh` (fixture
programs run with `detect_leaks=0`; see CLAUDE.md's leak-detection policy).

**1. A by-value struct argument passed to a `letrec` closure is boxed and never
freed.**

```turmeric
(defstruct Acc :copy [lo : float  hi : float])
(defn widen [a : Acc  x : float] : Acc
  (make-struct Acc (if (< x (.lo a)) x (.lo a)) (if (> x (.hi a)) x (.hi a))))
(defn span-letrec [n : float] : Acc
  (letrec [go (fn [x : float  acc : Acc] : Acc
                (if (>= x n) acc
                  (go (+ x 1.0) (widen acc x))))]
    (go 0.0 (make-struct Acc 0.0 0.0))))
(defn main [] : int
  (do (println (.hi (span-letrec 10.0)))
      0))
```

Built with `TUR_CC_FLAGS="-O1 -g -fsanitize=address"` and run with
`ASAN_OPTIONS=detect_leaks=1`: `Direct leak of 16 byte(s) in 1 object(s)`,
from `span_hyletrec`. The call site does

```c
tur_adt_Acc *__t214 = (tur_adt_Acc *)malloc(sizeof(tur_adt_Acc));
*__t214 = __ps_213;
... __fn_13((void *)(intptr_t)(go_10), 0.0, (int64_t)(intptr_t)(__t214));
```

and the closure copies the value out (`tur_adt_Acc acc = *(tur_adt_Acc *)
(intptr_t)__tur_b4box_acc;`) without freeing the box. Here the self-call is a
loop, so it is one box per outside call; in tur-plot's fold
(`(go (+ i 1) (bbox-union acc (open (vec-get rs i) [a v] (bounds v))))`) the
self-call was not turned into a loop, and every iteration boxed again.

**2. The closure's env is never dropped when its body opens a captured
existential and it takes a struct argument.**

The same fold, with the accumulator widened by
`(open (vec-get rs i) [a v] (size v))` over a captured
`rs : (Vec (exists [a] [(Sized a)] a))`, also leaks the 32-byte env per call:
the enclosing function mallocs it (`malloc(sizeof(void *) + sizeof(struct
__env_25))`) and emits no `TUR_CLOSURE_DROP(go_...)`. The same closure with an
`int` accumulator emits the drop; so does the struct-accumulator closure
without the `open`.

## Fix directions

1. Free the argument box where the closure unboxes it (or pass the struct by
   pointer to a caller-owned temporary, as a direct `defn` call passes
   `&acc`).
2. Find why the `open` + struct-argument combination skips the env drop at
   the end of the `letrec` scope.
3. A `requires.leak-check` fixture for each (`tests/run-leak-check.sh`).
