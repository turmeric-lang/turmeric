# reflected-measures -- a scoreboard with provable contracts

A small judged-competition scoreboard that exercises the `reflected-measures`
experiment (`^reflect`, [plan](../../docs/upcoming/reflected-measures-plan.md)).

Each API function states its contract with a recursive measure over the board:

| Function | Contract | Measure kind |
|---|---|---|
| `average` | `(> (size v) 0)` -- no division by zero | Int |
| `podium` | `(>= (size v) 3)` | Int |
| `certify` | `(valid? v)` -- every score in 0..10 | Bool |
| `winner` | `(ranked? v)` -- best first | Bool |
| `within-cutoff` | `(< (slowest v) 20.75)` -- every lap under the bound | Float (Real), comparisons only |
| `panel-size` | returns `#refine{ r : int \| (= r 5) }` | in-place return obligation |
| `runner-up-ranked?` | ranked in, `r` out, over a *variable* board | non-ground (RF4) |
| `podium` on a `let`-bound board | `(>= (size v) 3)` | the binding's equation selects the arm |

Without `^reflect` every one of these is an opaque symbol to the solver and
each call site keeps a runtime check (TUR-W0372). With it, every obligation
in the file proves at compile time. CI checks that claim (`tests/run-flags.sh`,
`example-reflected-measures`).

A Float measure proves through **comparisons**, not arithmetic. A float `+`
rounds in double, so `(+ t (lap-total r))` is an opaque term to the solver and
a summing measure proves only at the empty round. `slowest` selects with `>`,
which double computes exactly, so it proves at any depth the fuel reaches.

```sh
tur run examples/reflected-measures/src/main.tur
tur check --strict-refine examples/reflected-measures/src/main.tur   # every contract proved
tur check --dump-reflect   examples/reflected-measures/src/main.tur  # totality verdicts
tur check --dump-refine=json examples/reflected-measures/src/main.tur
```

`build.tur` declares `:experiments ["reflected-measures"]`, so the commands
above need no `--enable=` flag. `tur build <file>`, `tur compile` and
project-mode `tur run` do not read it yet
([report](../../docs/reported/build-file-and-run-project-ignore-manifest-experiments.md));
pass `--enable=reflected-measures` to those.

## Things to try

- Put an `11` on the board passed to `certify`, or swap the first two scores
  passed to `winner`: the check fails with `TUR-E0371 ... is false for the
  value given here`.
- Change `ranked?`'s recursive call to `(ranked? b)`: the definition is
  rejected with `TUR-E0384` (fails the termination gate).
- Pass `winner` a nine-entry board: unfolding runs out of fuel (8 unfoldings
  per obligation by default) and the check fails with `TUR-W0385`.
  `TUR_REFLECT_FUEL=16` raises the budget.
- Write `within-cutoff`'s contract over a reflected sum instead,
  `(< (lap-total v) 60.5)` with `^reflect` on `lap-total`: the check stays
  (`TUR-W0372`), because a float `+` is opaque to the solver.
