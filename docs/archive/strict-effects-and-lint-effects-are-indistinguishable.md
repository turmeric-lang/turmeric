# `--strict-effects` and `--lint-effects` emit the same warning, and neither can become an error

**Severity: low (flag taxonomy / stale comment).** Two flags document
themselves as a strict/advisory pair, but for `TUR-W0030` they emit the same
diagnostic through duplicated code, and the "strict" one has no promotion path
-- no `-Werror` covers it and `diag.c` discards warning severity when
computing exit status. The in-tree comment asserting the distinction is wrong.
Filed 2026-10-01 while measuring whether `--strict-effects` could default on.

**Status: RESOLVED 2026-10-02** -- one flag, with a real promotion path; see
[Resolution](#resolution).

**Original status: OPEN.** Cosmetic today. It matters because the distinction is the
stated reason both flags exist, and anyone deciding whether to default one on
will read the comment and believe a promotion mechanism is there.

## The claim, and what is actually there

`src/passes/effect_check.c:1557-1558`:

```c
/* --- ER6: --lint-effects: advisory warnings for unannotated effectful functions.
 * Behaves like --strict-effects (TUR-W0030) but is never promoted to an error. */
```

"Never promoted to an error" implies `--strict-effects` is. It is not:

- Both paths call `diag_emit_with_code(DIAG_WARNING, ..., TUR_W0030_...)` with
  a byte-identical format string (`effect_check.c:1511-1526` and
  `:1558-1578`).
- `src/compiler/diag.c:110` is `if (level == DIAG_WARNING) return false;` --
  warnings never contribute to a failing exit status, unconditionally.
- The only `-Werror=` flags in the driver are `deprecated` and
  `inline-c-narrow-params` (`src/main.c:11299-11320`). Neither covers W0030,
  and there is no general `-Werror`.

So the only behavioural difference between the two flags is that
`--strict-effects` additionally runs `effect_row_check_var_always_concrete`
(TUR-W0032, `effect_check.c:1507`). Measured over `tests/fixtures/` plus
`stdlib/`: W0030 fires 358 times, **W0032 once**. For practical purposes the
flags are the same flag.

## Minimal repro

```turmeric
(defeffect Bang [] :nil)
(defn boom [] : int (do (perform (Bang)) 0))
(defn main [] : int 0)
```

```sh
./build/tur --strict-effects check repro.tur ; echo "strict rc=$?"
./build/tur --lint-effects   check repro.tur ; echo "lint   rc=$?"
```

Both print the same `TUR-W0030` line; both exit 0.

## Fix directions

Pick one of two coherent stories; the current state is neither.

1. **Make `--strict-effects` strict.** Give W0030 a promotion path (a
   `--Werror=strict-effects`, or have `--strict-effects` itself emit
   `DIAG_ERROR`), leaving `--lint-effects` as the advisory form. This is what
   the names and the comment already promise, and it is the version that would
   make the flag worth defaulting on later. Needs the W0030 message fixed
   first -- see
   [strict-effects-w0030-names-synthesized-lambdas](strict-effects-w0030-names-synthesized-lambdas.md)
   -- since promoting an unactionable warning to an error is worse than
   leaving it a warning.
2. **Retire `--lint-effects`.** If W0030 is to stay advisory, one flag is
   enough; deprecate the alias (TUR-W0050 already has machinery for a retired
   flag) and delete the duplicated block.

Either way: **delete or correct the comment at `effect_check.c:1558`**, and
de-duplicate the two emit sites into one helper so a future message change
cannot touch only one of them.

## Resolution

**Fixed 2026-10-02 by taking both halves of the two stories that are not in
tension: one flag, and that flag can be made strict.** That is gcc's shape --
`-Wfoo` warns, `-Werror=foo` fails -- and the shape this driver already uses
for `-Werror=deprecated` and `-Werror=inline-c-narrow-params`.

- **`-Werror=strict-effects`** (also `--Werror=strict-effects`) promotes
  everything `--strict-effects` reports -- `TUR-W0030` and `TUR-W0032` -- to
  `DIAG_ERROR`, so the effect pass returns non-zero and the build fails. It
  implies `--strict-effects`, as gcc's `-Werror=<x>` implies `-W<x>`. The
  codes keep their `W` (the output reads `error [TUR-W0030]`), so a script
  grepping for the code finds it either way.
- **`--lint-effects` is a deprecated alias for `--strict-effects`** and prints
  `TUR-W0050` saying so. It is accepted rather than removed so existing
  scripts keep working; the worker path (`wk_apply_flags`, fixture `flags`
  files) accepts it silently, as it does the retired `-X` flags.
- **One emit site.** `g_lint_effects` and the ER6 block are gone;
  `effect_row_check_unannotated` in `src/passes/effect_check.c` is the only
  place `TUR-W0030` is emitted, so a message change cannot touch one copy and
  not the other. The wrong comment went with the block.

Why not "make `--strict-effects` itself emit errors": it would silently turn
every existing `--strict-effects` user's warnings into a failing build, under a
code whose prefix says warning. An opt-in promotion keeps the advisory form
for exploration and gives CI the gate.

The promotion was made after
[strict-effects-w0030-names-synthesized-lambdas](strict-effects-w0030-names-synthesized-lambdas.md)
was fixed in the same change, as this report asked: an error that names
`__fn_38` would have been worse than the warning.

**Pinned by** `tests/run-flags.sh`: `lint-effects-alias` (the alias compiles,
prints `TUR-W0050`, and reports `TUR-W0030` exactly once),
`strict-effects-werror` (fails with `error [TUR-W0030]`),
`strict-effects-werror-clean` (an annotated program still compiles under
`-Werror`). Documented in `compiler-flags-guide.md` and the effects guide's
flags table; `tur explain TUR-W0030` mentions both.

**Still open, deliberately:** whether `--strict-effects` should default on.
That is the plan's W5 and its recommendation stands -- re-measure after W4
tags `println`, because every number so far was taken with printing invisible
to the effect system.
