# An abandoned crossing is silent under `--strict-refine`

**Severity:** low-medium (diagnostic honesty). Programs are correct: a crossing
never elides the callee's entry check, so the check runs and fires. What is
lost is `--strict-refine`'s promise that a clean check means every contract
was discharged at compile time.

## Repro

```turmeric
(defn needs-pos [x : #refine{ v : int | (> v 0) }] : int x)
(defn f [x : #refine{ v : int | (> v 0) }] : int
  (let [x -5]
    (needs-pos x)))
(defn main [] : int (println (f 3)) 0)
```

`tur check --strict-refine` exits 0. `--dump-refine=json` lists only
`argument 1 of 'f' in 'main'`; the `needs-pos` crossing is not there at all.
`tur run` panics: `Contract violated by parameter 'x' in needs-pos`.

## Root cause

A binder on the path that shadows a refined name sets `*shadowed` in
`rt_collect_path_conds` (`src/compiler/elab_fns.c`). Then
`refine_resolve_call_sites` does `if (cs_skip) continue;`, so the crossing
gets no obligation, no `TUR-W0372`, and nothing for `--strict-refine` to
promote.

That was a deliberate choice in
[crossing-shadowed-binder-false-proof](../archive/crossing-shadowed-binder-false-proof.md)
("It costs a diagnostic in a rare shape and removes a wrong one"). It is the
right call against a false proof. It does not consider strict mode, where a
missing diagnostic reads as success. Since 2026-10-17 every binder shape
reaches this path (multi-binding, `^mut` and annotated `let` bindings, and
`fn` parameters, beside the lone `let` binding and `match` binders), so the
shape is less rare than it was.

## Fix directions

- Record the skipped crossing as an Unknown obligation with a reason ("a
  binder on the path shadows `x`; runtime check kept"). It is then a
  `TUR-W0372` like any other decline, and `--strict-refine` makes it an error.
  This is the honest version of "no answer here".
- Or alpha-rename the shadowed binder for the crossing, as `rt_prove_paths`
  does for return obligations, and keep the proof.
- `tests/run-flags.sh` `refine-crossing-shadowing-binders` pins today's
  behaviour (the crossing is not proved). Extend it to expect the W0372.

## Resolution (2026-10-18)

The first fix direction, plus one change so that a shadowed crossing which
needs no facts still proves.

A crossing under a shadowing binder is still not given the path's facts:
those would speak about the outer binding, which is the false proof
crossing-shadowed-binder-false-proof removed. Instead of `continue`,
`refine_resolve_call_sites` (`src/compiler/elab_fns.c`) now hands it to
`rt_report_shadowed_crossing`. That function records each refined argument as
an obligation and decides it against an environment that has the caller's
sorts and none of its hypotheses, so nothing it proves leans on the shadowed
binding:

- **Proved:** `(let [x 5] (needs-pos 7))` still proves, under
  `--strict-refine` too.
- **A closed refutation:** `(needs-pos -5)` under a shadowing binder is the
  ordinary TUR-E0371, as it is without one.
- **Otherwise:** the verdict is `unknown`, with `decided_by` "declined: a
  binder on the path shadows a refined name". It reports TUR-W0372 wherever
  an unknown crossing does: as an error under `--strict-refine`, and as a
  warning when no runtime check backs the crossing (a `#reads` measure). An
  open counterexample is not offered, because it ignores the path.

The repro is now `error [TUR-W0372]: ... (a binder on the path to this call
shadows a refined name, so its facts are not used); runtime check kept` under
`--strict-refine`. Without the flag it is quiet, as before, since the runtime
check remains. `--dump-refine=json` lists the crossing.

Pinned by `tests/run-flags.sh` `refine-crossing-shadowing-binders`, extended
as this report asked. It checks the four shadowing bodies for an `unknown`
verdict and a W0372 under strict. It also checks that `(let [x 5] (needs-pos
7))` and the unshadowed control prove and pass `--strict-refine`.
