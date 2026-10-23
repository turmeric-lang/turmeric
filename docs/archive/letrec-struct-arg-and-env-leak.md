# A `letrec` closure leaks its struct arguments, and its env when it opens an existential

**Resolved 2026-10-23: leak 2 is fixed too** (leak 1 was fixed 2026-10-21, below). The env drop was skipped by `let_binding_env_freeable`'s result gate: a closure returning a struct (`TY_ADT`) is admitted only when its body has no inline C, and `expr_subtree_has_inline_c` (`src/compiler/emit_core.c`) had no arm for `EX_EXISTS_OPEN`, `EX_EXISTS_PACK` or `EX_EXISTS_DISPATCH`, so a body that opened an existential fell to the conservative `default` and read as "may hide inline C". The int-accumulator closure passed only because an `int` result needs no such gate. The three arms walk their operands, as the other emitter-code nodes do. `tests/fixtures/letrec-open-existential-env-leak` (`requires.leak-check`) leaks 48 B without the change and is clean with it; the whole leak-check harness stays green.

**Narrowed 2026-10-21: leak 1 is fixed; leak 2 stays open.** The direct
closure-call site (`src/compiler/emit_expr.c`, the B4 branch of the thunk
call's argument loop) now spills a wide by-value struct argument into the
caller's frame and passes its address, instead of heap-boxing it with no
owner: the thunk only borrows the box (it deref-copies at entry, `emit_fns.c`
"no free here"), and the spill's `&` keeps the call off `musttail`, so the
frame outlives the call. `tests/fixtures/letrec-struct-arg-leak` (the program
below plus a non-loop self-call) runs under LeakSanitizer through
`tests/run-leak-check.sh`. What remains is leak 2, the env that is never
dropped when the body opens a captured existential and takes a struct
argument.

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
