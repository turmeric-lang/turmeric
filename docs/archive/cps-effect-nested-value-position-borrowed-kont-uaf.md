# effect-nested: value-position nested handle -- borrowed-`__kont` use-after-free (RESOLVED)

**STATUS: RESOLVED.** `effect-nested` DK-lowers -- `get-val` and the synthesized
`main` both emit `__cps`, zero `eff=1`, zero `tur_effect_perform` call sites,
output `52`.  The value-position nested handle no longer rides the fiber.

## The shape

```turmeric
(defeffect Val [] : int)
(defn get-val [] : int (perform (Val)))
(println (handle
  (+ (get-val)                       ; operand 0 -- outer handler resumes 10
     (handle (get-val)               ; operand 1 -- inner handle, value position
       (Val [] k) (resume k 42)))    ; inner resumes 42
  (Val [] k) (resume k 10)))         ; => 10 + 42 = 52
```

The inner `handle` sits in a VALUE position (operand 1 of `+`), so the outer
handle's heap-join frame (`main_j1`, `emit_heap_join`) reifies `(+ __t2 <inner>)`
and its body installs the inner handle.  The inner handle's continuation
(`main_hk2`) computes `__t2 + __t3` and must deliver the sum to the enclosing
continuation (`main_hk0`, which prints).

## Root cause (TWO gaps, second is a use-after-free)

1. **`main_j1` had no `__kont`.**  It was lifted as an `LH_PERFORM_CONT` DKFrame
   `(env, value)`, but its body needs `__kont` to hand to the inner handle's
   continuation env (`main_hk2_env->__k = __kont`).  `needs_kont`
   (`jbody_has_cps_tailcall || jbody_has_perform`) did not detect a nested
   handle/reset in the jbody, so `__kont` was undeclared -- a COMPILE error.

2. **The naive `jbody_has_delim` fix compiled but HUNG (the real bug).**  Adding
   `jbody_has_delim` (a nested `CT_HANDLE`/`CT_RESET` in the jbody) to `needs_kont`
   lifts `main_j1` as an `LH_RESUME_CONT` resume-frame, which RECEIVES its downstream
   chain as the `__kont` parameter -- so it compiles.  But a RESUME_FRAME's `__kont`
   is the DRIVER-OWNED chain (`dk_run_impl` passes `k->next`), which the E7 driver
   `dk_free`'s right after the frame yields.  `main_j1` stashes that borrowed
   `__kont` into `main_hk2_env->__k`, an env that is read MUCH LATER -- when
   `main_hk2` is delivered off the meta-stack, after the yield.  By then the chain
   is freed, so `dk_run(__kont, 52)` in `main_hk2` walks a freed node whose garbage
   `->next` self-cycles -> infinite loop.  (Confirmed by instrumenting
   `dk_run_impl`: `[run kind=-566131371 tag=2 k=<same addr>]` repeating.)

## The fix

- **`jbody_has_delim`** (emit_cps_ir.c): a heap-join jbody containing a nested
  `CT_HANDLE`/`CT_RESET` sets `needs_kont` (flag-gated on `g_opt_cps_tramp_resume`),
  lifting the join as an `LH_RESUME_CONT` resume-frame with `__kont`.
- **`CE.borrowed_kont`** (emit_cps_ir.c): set when emitting an `LH_RESUME_CONT`
  frame body.  `emit_cont_env`, when it captures the frame's `__kont` into a
  reset/handle continuation env, COPIES it
  (`__dk_reap_keep(dk_copy_range((const DK *)__kont, NULL))`) instead of aliasing.
  The copy is reaped at the outermost entry boundary like every other delimited DK
  chain.  The value-position nested handle then delivers its result to the enclosing
  continuation exactly once (via the meta-stack), with no use-after-free.
- **`fold_stmt_is_risky`** (elab_toplevel.c): the value-position rejection
  (`n_handle >= 2 && fold_handle_in_value_position`) is removed; only the
  `set!`-escaping-mutable shape (`effect-capture-k`) remains carved out.  The
  now-dead `fold_handle_in_value_position` helper is deleted.

## Why the copy is sound (memory safety -- the important part)

At the value-position join point the borrowed `__kont` is the enclosing HANDLER
markers terminated by `dk_done` (skippable in `dk_run`).  Copying it yields a fresh,
NULL-terminated, self-contained chain that `main_hk2`'s `dk_run(__kont, sum)` walks
(handlers skipped -> value returned) and that the reaper `dk_free`'s once.  The
value routing to `main_hk0` (print) rides the E7 meta-stack delivery, independent of
what the copied `__kont` contains, so correctness does not depend on the copy's
shape -- only on it staying VALID.

Verified: repro + fixture output `52`; ASan clean (no use-after-free / double-free /
leak on the emitted program); flag-off byte-identical (140/140 `expected.c`
snapshots); flag-on soundness sweep clean (167 matched + 3 documented known-benign);
full suite 2203/0 under the Debug ASan build (compiler path leak-checked).

## Context

The last value-position residual of the synthesized-main fold
(docs/reported/cps-toplevel-synthesized-main-bypasses-dk.md).  Only
`effect-capture-k` (by-reference mutable capture -- a `set!` writes the captured
continuation into an outer `^mut`, resumed after the handle exits) remains on the
fiber; that needs a real DK feature (heap-cell by-ref mutable capture).
