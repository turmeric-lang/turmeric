# The test harnesses' pass cache ignores a fixture's expectation files

**Severity:** medium-low (test coverage). Tightening a fixture's expectations
does not re-run it: the old pass is served from the cache until
`TUR_FORCE=1`.

## What happens

`tests/run.sh` skips a fixture whose stamp matches. The stamp is built in
`stamp_key`:

```sh
echo "$(_tur_hash_file "$input")-${ec_hash}-${TUR_MTIME}-${TUR_STDLIB_HASH}-${TUR_CONFIG_HASH}"
```

It hashes `input.tur`, `expected.c`, the compiler's mtime, the stdlib and the
config. It does not hash `expected.stdout`, `expected.stderr`,
`expected.diag`, `unexpected.*`, `flags` or the `requires.*` markers. Editing
any of those (adding a needle, changing expected output, adding a flag) leaves
the stamp valid, so the fixture still reports `PASS` without running.
`tests/run-turi.sh` has its own `stamp_key` with the same blind spot.

## Fix directions

- Hash every file in the fixture directory, except the outputs the harness
  writes there, into the stamp. Or hash the specific expectation, flag and
  marker files both harnesses read.
