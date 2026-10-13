# A join that runs a lexical cloneable reset and then a `handle` names an undeclared `__kont`

**Fixed 2026-10-12** in the same change that found it. It was found while
writing the leak fixture for
[lexical-shift-receiver-cont-never-freed](lexical-shift-receiver-cont-never-freed.md).
It reproduced on `main`. The interpreter was right; the compiled path did
not build.

**Severity was medium.** A valid program failed to compile with a C error.
It was loud, never a wrong answer.

## Repro

```turmeric
(defeffect Tick [] int)
(defn use1 [k : cont] : int (k 1))
(defn s2 [i : int] : int (cloneable-reset (+ i (cloneable-shift use1 0))))
(defn tick [c : int] : int (+ c (perform (Tick))))
(defn main [] : nil
  (println (s2 7))
  (let [c (cloneable-reset (+ 1 (cloneable-shift use1 0)))]
    (println (handle (tick c) (Tick [] k) (resume k 41)))))
```

`tur --interpret` printed `8` and `42`. `tur run` failed in cc:

```
error: '__kont' undeclared (first use in this function)
static intptr_t main_j0(intptr_t env, intptr_t __t4__slot) {
    ...
    DK *__h2 = ... dk_frame_resume_group_end(main_hk2, 0, __kont) ...;
```

The first statement's colored call makes the rest of `main` a join.

## Root cause

Three predicates decide whether a heap join must be a resume-frame that
receives `__kont`, rather than a value-only frame `(env, value)`:

- `jbody_has_delim` (a nested `handle` / `reset`);
- `jbody_has_perform`;
- `jbody_has_cps_tailcall`.

All three are in `src/compiler/emit_cps_ir.c`, and none of them descended
into `CT_CLONEABLE` (an inline cloneable reset) or `CT_CALLCC` (a call/cc
landing). Both of those emit their body inline, after the reset. So a
`handle` behind one went unseen, and the join was lifted without the
continuation the handle threads.

## Fix

All three walkers now descend into `t->as.cloneable.body` and
`t->as.callcc.body`. An outward serial reset lifts its body instead of
running it inline. There, walking into the body only promotes the join to a
resume-frame, which is valid from either position.

`tests/fixtures/cps-join-after-cloneable-reset-kont` pins three shapes, each
followed by a `handle`: a named non-escaping receiver, an escaping one, and a
call/cc landing.
