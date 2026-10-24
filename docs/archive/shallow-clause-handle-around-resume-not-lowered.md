# A `handle` around `resume` in a `handle-shallow` clause does not compile, and warns its clause is unreachable

**Resolved 2026-10-23, both halves.**

- **Lowering.** A handler case body may now end in a `handle` whose body is
  the case's own `resume` (`handle_case_ok_rec`'s CT_HANDLE arm,
  `src/compiler/emit_cps_ir.c`). Such a case counts as resuming into the chain
  (`term_resumes_k_any` looks into the handle's body), so it runs with the
  real enclosing chain as `__kont`; `emit_handle` installs the inner group on
  it, and the resume, being the handle body's tail, is
  `dk_resume_into(k, v, __h<N>, 1)` -- the continuation is spliced in under
  the inner group, so whatever it performs reaches that handler first. The
  repro prints 31 on all three engines; so do a deep inner handler answering
  two later performs (331), an inner handler for a different effect, an outer
  DEEP handle (its re-installed handler is nearer: 11), and an inner clause
  that works after its own resume (333). Still refused: a `handle` that is not
  the case's last form, or whose body is more than the resume.
- **TUR-W0033.** While a clause body is checked, its `k` maps to the effect
  row of the body it continues; a nested handle whose body resumes such a `k`
  counts that row as performed (`kres_visit`, `src/passes/effect_check.c`).
  A clause for an effect the rest really never performs still warns.

Pinned by `tests/fixtures/shallow-clause-handle-around-resume` (five shapes,
both engines).

## As filed

**Severity: low-medium** (compile failure on a documented pattern; the
interpreter is correct). Found 2026-10-22 writing the `handle-shallow` section
of effects-system-guide for
[missing-guides-for-language-primitives](../archive/missing-guides-for-language-primitives.md).

## Repro

```turmeric
(defeffect Ask [] : int)
(defn two-asks [] : int
  (+ (perform (Ask)) (* 10 (perform (Ask)))))
(defn main [] : int
  (println (handle-shallow (two-asks)
             (Ask [] k) (handle (resume k 1) (Ask [] k2) (resume k2 3))))
  0)
```

`tur --interpret`: `31` (the shallow clause answers the first `Ask` with 1, then
the `handle` it wrapped around `resume` answers the second with 3). `tur run`:

```
error: this effect operation has no lowering here: the enclosing function left
the CPS backend's supported subset ...
```

on both `perform`s in `two-asks` (`TUR_TRACE_EVICT=1`: `BODY-TAINT eff=1
two-asks`), plus `error: this handle has no lowering here` for every `handle`
in `main`. `stdlib/forms/effects.tur` describes this exact shape as what
`handle-shallow` is for ("this lets a clause choose what handles the rest, by
wrapping the `resume` in another `handle`").

Both engines also print a false
`warning [TUR-W0033]: handler clause for 'Ask' is unreachable: the body does not
perform 'Ask'` on the inner `(Ask [] k2)` clause: its body is `(resume k 1)`,
and the continuation it resumes performs `Ask`. The reachability check reads
the clause body syntactically and does not count a `resume` of a continuation
whose rest performs the effect.

## What works

A `handle` AROUND the whole `handle-shallow` (`effect-shallow-handler`) lowers
fine; only the handler installed inside the clause, around the `resume`, is
refused.

## Fix directions

1. Find which form `two-asks`'s taint comes from: the CPS backend must install
   the clause's `handle` around the resumed continuation (the DK chain the
   shallow clause holds), which is a handler installed around a `resume` rather
   than around a call.
2. W0033: treat a clause body that resumes the shallow continuation as able to
   perform every effect the handled body can.
3. Pin both engines with a fixture once it lowers.
