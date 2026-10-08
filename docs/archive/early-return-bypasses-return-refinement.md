# An early `return` bypassed a function's return refinement and `:post`

**Severity: high (soundness).** A refined return type or `:post` was
neither proved nor checked on a value leaving through `return`. The static side
"proved" the obligation from the last body form alone, so the whole-body check
was elided. Even when the check was kept, the emitted `return` jumped past it.
A caller trusts that result through its crossings, so a wrong value could
discharge a downstream obligation, such as a bounds check. Found 2026-10-03
while narrowing the early-`return` decline of
[loop-invariant-declines-more-than-soundness-requires](loop-invariant-declines-more-than-soundness-requires.md),
and fixed in the same change. **RESOLVED 2026-10-03.**

## Repro

```turmeric
(defn f [n : int] : #refine{ r : int | (>= r n) }
  (when (> n 5) (return 0))
  n)

(defn g [n : int] : int
  :post (>= result n)
  (when (> n 5) (return 0))
  n)

(defn main [] : int
  (println (f 10))     ;; printed 0 -- violates r >= 10
  (println (g 10))     ;; printed 0 -- violates result >= 10
  0)
```

`TUR_REFINE_STATS=1 tur check` reported `2 obligation(s): 2 proven`.
`--strict-refine` was clean. The emitted C for `f` was:

```c
if ((n) > (INT64_C(5))) { tur_frame_fire_chain(&__frame); return INT64_C(0); }
__t = n; ... return __t;            /* no check on either exit */
```

Nothing about this is loop-specific. The same bypass hit `definstance`
methods, whose result checks come from the instance-method path.

## Root cause

Two halves, each sufficient on its own.

1. **Static.** `elab_defn` hands `rt_return_obligation_proven` the LAST body
   form as the subject (`rt_subject`). The RT4 per-path split
   (`rt_prove_paths`) reads a `do`'s earlier forms as statements that matter
   only through `set!`. A `return` in one of them is a second exit whose value
   neither looks at, so the proof covered the fall-through path only. RT4
   inference (`rt4_infer_return`), which publishes a refinement to call sites,
   had the same blind spot.
2. **Runtime.** `rt_wrap_return_check` wraps the body as
   `(let [r body] (check p) r)`. An `EX_RETURN` inside `body` is emitted as a C
   `return` and never reaches the check.

## Fix

- **Static** (`elab_defn`): when the whole body mentions `return` outside a
  lambda (`li_mentions_return`), the return and `:post` obligations are not
  attempted. They are reported as `TUR-W0372` "could not be decided statically
  (the body can leave early through `return`); runtime check kept", an error
  under `--strict-refine`. RT4 inference is skipped too. Proving each return
  value under its own path condition would recover precision; nothing needed
  it yet.
- **Runtime** (`elab_return`): the enclosing function publishes its result
  checks as `Elab.ret_contract`, and `rt_check_returned_value` wraps each
  `(return v)` value in them. That means `:post`, then the refined return,
  then (for an instance method) the class's promise -- the same order the
  whole-body wrap nests them. Each check is elaborated in a scope of its own,
  so its result binding cannot shadow a user name after the `return`. The
  contract is published by `elab_defn` and by the instance-method path, and
  it is cleared for every nested function body: a lambda (`elab_fn`), and a
  `defclass` default method. A lambda's `return` is the lambda's.

## Pinned by

- `tests/fixtures/refine-early-return-violates-return`, `-violates-post`,
  `-violates-method`: each early return that breaks the contract now panics.
- `tests/fixtures/refine-early-return-contracts`: satisfying early returns pass.
  It also covers a user local spelled like the refinement variable after the
  return site, and a lambda's own `return` that the outer check must leave
  alone.
- `tests/fixtures/errors/refine-early-return-not-proved-strict`: the obligation
  is no longer "proved".

## Not covered

`?` and other non-local exits that do not go through `elab_return` were not
audited here. A refined return over a `Result` is unusual, but `?` lowers to an
early exit too.
