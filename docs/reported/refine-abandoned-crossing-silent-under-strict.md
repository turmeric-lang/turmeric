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
