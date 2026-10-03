# `:run` on a sweet file invokes `main` but leaves no definitions behind

**Severity:** low-medium.
**Found:** 2026-10-03, against v0.60.1.
**Impact:** "run this file, then poke at it" works for `.tur` and not for
`.tur.sweet`.

## What happens

Same program, two spellings:

```sh
$ cat both.tur
(def both-x 7)
(defn main [] (println "main ran") 0)

$ printf ':run both.tur\nboth-x\n' | tur repl
main ran
=> 7
```

```sh
$ cat both.tur.sweet
def both-x 7

defn main []
  println("main ran")
  0

$ printf ':run both.tur.sweet\nboth-x\n' | tur repl
main ran
             <-- `both-x` is unbound
```

`main` runs in both cases. The top-level definition survives into the session
only for the `.tur` file.

`load` is the complement, for both spellings: the definitions land and `main`
is never invoked.

| | `.tur` | `.tur.sweet` |
|---|---|---|
| `:run` | `main` runs, defs persist | `main` runs, **defs do not** |
| `load` | defs persist, `main` not run | defs persist, `main` not run |

## History

This looks like the residue of a defect that was worse. Against v0.42.2, `:run`
on a `.tur.sweet` printed `;; run:` then `;; ready` and did nothing at all —
`cmd_run` built a fresh env, `turi_eval_file` saw a non-default reader in the
extension and called `turi_env_reset_to_prelude`, discarding the preload nobody
re-ran, and the elaboration error was dropped on the floor. `repl.c`'s own
comment above `repl_preload_stdlib_and_natives` names that hazard. The program
runs now; the session accumulation is the half still missing.

## Suggested fix

Have `:run` accumulate a non-default-reader file's top-level forms into the
session the way it does for the default reader — or, if that is not possible
because accumulated source cannot be re-read under the session's reader, say so
rather than succeeding quietly.

## Client-side consequence

*(Paths below are in [Trowel](https://github.com/rjungemann/trowel), a
separate repo — the editor this was found from. Included so the report
names a real consumer and so the workaround can be deleted alongside a
fix.)*

Trowel's Run Buffer used to route sweet buffers through `:reset` + `load` to
work around the v0.42.2 defect, which meant sweet programs were *defined and
never run* — the very bug Trowel had fixed for `.tur`. It now uses `:run` for
every dialect and pins this gap with a smoke test
(`test_run_sweet_buffer_does_not_leave_its_definitions_behind`), which starts
failing when this is fixed.
