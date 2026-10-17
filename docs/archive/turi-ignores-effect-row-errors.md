# `tur --interpret` prints effect-row errors and then runs the program anyway

**RESOLVED 2026-10-17** -- see [Resolution](#resolution-2026-10-17). Pinned by
`tests/run-turi.sh`, whose errors pass now requires a non-zero exit.

**Severity:** low-medium. A program the compiler rejects runs to completion and
exits 0 under the interpreter, after printing the error.

## Repro

```turmeric
(defeffect Ask [] : int)
(defn f [] #fx{} : int (handle (perform (Ask)) (Ask [] k) (resume k 1)))
(defn g [] #fx{} : int (do (println 5) 1))
(defn main [] : int (do (println (+ (f) (g))) 0))
```

- `tur run`: TUR-E0009 (`function 'g' performs effect 'IO' but its declared
  row {} does not include it`), exit 1.
- `tur --interpret`: the same TUR-E0009 on stderr, then prints `5` and `2`,
  and exits 0.

## Root cause

`turi_eval_impl` (`src/turi/eval.c`) calls `effect_check_pass` but ignores its
return value. `tests/run-turi.sh`'s errors pass only greps stderr for the
diagnostic text, so it passes whether or not execution continues.

## Fix directions

- Stop the turn when `effect_check_pass` reports an error, as the compiled
  pipeline does.
- Have `run-turi.sh`'s errors pass also require a non-zero exit, as `run.sh`
  does.

## Resolution (2026-10-17)

`turi_eval_impl` (`src/turi/eval.c`) keeps `effect_check_pass`'s result and
stops the turn on an error, with the same cleanup as the other checks there,
so the interpreter exits 1 after TUR-E0009 as the compiled path does.
`tests/run-turi.sh`'s errors pass now requires a non-zero exit too; an
`errors/` fixture the interpreter runs to exit 0 fails with "the interpreter
exited 0 on an errors/ fixture".
