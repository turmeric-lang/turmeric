# Partially applying a `^borrow` parameter spends an affine argument

**Severity: low (a compile-time refusal of a valid program; nothing unsafe runs).**

Found 2026-10-28 revisiting the known limits of `stdlib/numvec.tur`
([simd-and-vectorization-plan](../upcoming/simd-and-vectorization-plan.md) N0).

A saturated call to a `^borrow` parameter does not spend an affine argument:
the call path rolls back the single use the variable reference recorded
(`elab_call.c`, the "Substructural affine: undo the single use" block in the
borrow-argument loop). A **partial** application into the same parameter goes
through `elab_partial_apply` (`elab_call.c`), which has no such rollback, so
the argument is spent and every later use is TUR-E0150.

## Repro

```turmeric
(load "stdlib/numvec.tur")
(defn main [] : int
  (let [a (u8vec-new 2 (as uint8 0))
        s (u8vec-set! a)]
    (s 1 (as uint8 77))
    (println (u8vec-ref a 1)))   ; TUR-E0150: affine value 'a' used more than once
  0)
```

The same shape with a full call, `(u8vec-set! a 1 (as uint8 77))`, compiles.

## Why it is not a one-line fix

Rolling the use back is the easy part. The partial application stores `a` in
its env, so the closure holds a borrow of `a`: if `s` escapes the scope (is
returned, stored, or captured by something that outlives `a`), the borrow
dangles once `a`'s scope drops it. The saturated call needs no such check
because its borrow ends when the call returns. A fix therefore needs:

- the rollback in `elab_partial_apply` for `FA_BORROW` slots; and
- a non-escape check on the resulting closure (the machinery
  `hoist_borrowed_closure_args` / `binding_escapes_impl` already uses for
  borrowed closures), refusing the partial application -- with a diagnostic
  that names the borrow -- when the closure may outlive the argument.

The partial-application env must also not drop the captured argument; it does
not today (`capture_drop_insts` is NULL for `__pap` envs), and the fix must
keep it that way.

## Workaround

Call the setter saturated: `(u8vec-set! a 1 (as uint8 77))`. A lambda is no
way round it from the owning scope -- capturing an owned `a` moves it into the
closure, which is the same TUR-E0150 on the next use. Inside a function that
takes `a` as `^borrow`, a lambda over it does borrow it (see
`tests/fixtures/closure-borrowed-drop-capture-not-dropped`).
