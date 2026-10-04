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
| `within-cutoff` | `(< (lap-total v) 60.5)` | Float (Real) |
| `panel-size` | returns `#refine{ r : int \| (= r 5) }` | in-place return obligation |
| `runner-up-ranked?` | ranked in, `r` out, over a *variable* board | non-ground (RF4) |

Without `^reflect` every one of these is an opaque symbol to the solver and
each call site keeps a runtime check (TUR-W0372). With it, every obligation
in the file proves at compile time.

```sh
tur run examples/reflected-measures/src/main.tur
tur check --strict-refine examples/reflected-measures/src/main.tur   # every contract proved
tur check --dump-reflect   examples/reflected-measures/src/main.tur  # totality verdicts
tur check --dump-refine=json examples/reflected-measures/src/main.tur
```

`build.tur` declares `:experiments ["reflected-measures"]`, so no
`--enable=` flag is needed.

## Things to try

- Put an `11` on the board passed to `certify`, or swap the first two scores
  passed to `winner`: the check fails with `TUR-E0371 ... is false for the
  value given here`.
- Change `ranked?`'s recursive call to `(ranked? b)`: the definition is
  rejected with `TUR-E0384` (fails the termination gate).
- Pass `winner` a nine-entry board: unfolding runs out of fuel (8 unfoldings
  per obligation by default) and the check fails with `TUR-W0385`.
  `TUR_REFLECT_FUEL=16` raises the budget.
