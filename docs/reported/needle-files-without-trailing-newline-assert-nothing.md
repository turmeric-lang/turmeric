# Needle files without a trailing newline assert nothing

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
