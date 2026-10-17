# Needle files without a trailing newline assert nothing

**RESOLVED 2026-10-17** -- see [Resolution](#resolution-2026-10-17). The
needle loops in both harnesses read an unterminated last line.

**Severity:** medium (test coverage). 39 fixtures' expected diagnostics or
stderr are never checked, and at least one hides a real interpreter bug.

## What happens

`tests/run.sh` and `tests/run-turi.sh` read `expected.diag`,
`expected.stderr` and their `unexpected.*` mirrors one needle per line:

```sh
while IFS= read -r needle; do
    [ -z "$needle" ] && continue
    grep -F -q "$needle" "$err" || missing=1
done < "$needles"
```

`read` returns non-zero on a final line without a newline, so the loop body
never sees that line:

```
$ printf 'abc' | while IFS= read -r l; do echo "got $l"; done
$                                    # nothing
```

A one-line needle file written without a trailing newline (e.g. by
`printf`, or an editor that does not add one) therefore contributes no
needle at all:

| File | Total | Unterminated | Single-line (assert nothing) |
| --- | --- | --- | --- |
| `expected.diag` | 742 | 39 | 35 |
| `expected.stderr` | 134 | 4 | 4 |
| `unexpected.diag` / `unexpected.stderr` | 14 | 0 | 0 |

The four multi-line unterminated `expected.diag` files lose their last needle.

Under `run.sh` an `errors/` fixture still has to fail to compile, so the
damage there is "any error passes". Under `run-turi.sh` the errors pass
ignores the exit status (`|| true`), so these fixtures check nothing at all.

## Example of a hidden bug

`tests/fixtures/errors/effect-handler-borrow/expected.diag` is one
unterminated line. `tur --interpret` on that fixture prints `42` and exits 0,
skipping the borrow-capture check
([turi-skips-handler-borrow-capture-check](turi-skips-handler-borrow-capture-check.md)).
`TUR_FORCE=1 TURI_FILTER='errors/effect-handler-borrow' bash tests/run-turi.sh`
still reports `PASS`.

## Fix directions

- In every needle loop (`run.sh` errors and positive passes, `run-turi.sh`),
  use `while IFS= read -r needle || [ -n "$needle" ]; do`.
- Add a check to `tests/check-docs-lint.sh` (or a sibling script run in CI)
  that every needle file ends with a newline, or normalize the 43 files in
  the same change.
- Expect some of the 39 fixtures to start failing once their needle is read.
  Each such failure is a real divergence that was hidden.

## Resolution (2026-10-17)

Every needle loop in `tests/run.sh` (errors and positive passes, and the
`run.args` reader) and `tests/run-turi.sh` reads an unterminated last line:
`while IFS= read -r needle || [ -n "$needle" ]; do`. The 43 files were also
given their trailing newline, so an older harness reads them too.

Reading the needles surfaced what they had been hiding:

- `errors/effect-handler-borrow` under the interpreter --
  [turi-skips-handler-borrow-capture-check](turi-skips-handler-borrow-capture-check.md);
- `errors/session-project-nonprojectable`: the projection gave a role that
  cannot learn the choice a `Branch` --
  [session-projection-branches-a-role-that-cannot-learn-the-choice](session-projection-branches-a-role-that-cannot-learn-the-choice.md);
- needles that had drifted from the diagnostics they were written against,
  and were updated to the error the fixture still raises at the intended
  site: `errors/borrow-moved`, `copy-use-after-move` and `ref-use-after-move`
  (TUR-E0201, with the location), `borrow-ptr-unsafe-required` (the message
  ends `(unsafe ...)`), `hkt-multi-instance` (the needle named the enum,
  `TUR_E0020_AMBIGUOUS_DISPATCH`, not the printed `TUR-E0020`), and the
  `panic-defer` / `panic-ref` stderr, which now carries the panic's location.
