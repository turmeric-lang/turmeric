# A function with an `any` parameter passed to a `forall` slot reads garbage

**Severity:** medium -- wrong answer at an `int`, segfault at a `cstr`, no
diagnostic. Found 2026-10-22 executing
[rank2-argument-genericity-plan](../archive/rank2-argument-genericity-plan.md);
present before that change (it accepts `any` as generic, as the plan says, and
the old checker accepted everything).

## Repro

```turmeric
(defn apply-poly [f (forall [a] (-> a a)) x : int] : int (f x))
(defn ida [x : any] : any x)
(defn main [] : int (println (apply-poly ida 7)) 0)
```

Prints `0`, compiled and `tur run --interpret` alike (expected `7`). Applying
`f` to a `cstr` instead (`(defn both [f (forall [a] (-> a a)) s : cstr] ...
(println (f s))`) segfaults the compiled program.

## Root cause (compiled)

`make_poly_wrapper` (`src/compiler/elab_call.c`) types the forwarding wrapper
from the inner function, so it is emitted as

```c
static tur_tagged_t __poly_8(void *env, tur_tagged_t x) { return ida(x); }
```

while the forall carrier calls every wrapper through
`int64_t (*)(void *, int64_t)`. A 16-byte `tur_tagged_t` argument and result
travel in two registers, so the callee reads the `int64` word plus a garbage
tag, and the caller reads half of the returned struct. The interpreter's
answer is the same `0`, so its rank-2 path drops the `any` box as well
(not traced).

## Fix directions

- In the wrapper, box each carrier word into `any` before the call
  (`elab_coerce_to_any`, as `elab_poly_call` already does for an `any` slot of
  an F5 typed carrier) and unbox the result back to the carrier word -- but the
  carrier word does not say which type it holds, so the box needs the call
  site's instantiation. That is the hard part.
- Or reject an `any` parameter at a `forall` position in
  `hrt_check_rank2_arg_generic` until the wrapper can widen, and drop `any`
  from the "generic" list in hrt-guide and the `forall` forms entry.
