# Eleven Try Turmeric specs have never run in CI

**Severity:** low-medium (test coverage gap; a spec nobody runs cannot fail).

Found 2026-11-06 by `tests/check-web-specs-in-ci.py`'s first run, while
resolving
[try-turmeric-specs-outside-the-ci-path-list-have-rotted](../archive/try-turmeric-specs-outside-the-ci-path-list-have-rotted.md):
that spec rotted for weeks because CI's desktop step names an explicit path
list and it was not on it. Ten more are in the same position, and nothing
says whether they pass:

`ci-api`, `docs-highlight`, `docs-restore`, `editor-highlight`, `pwa-update`,
`run-session`, `share-link`, `sw-kill`, `theme-toggle`, `toolbar-overflow`.

(`prod-smoke` is the eleventh name on the list and is not part of this gap:
it drives the live site through its own config, `npm run test:prod`.)

Each has a line in `web/tests/specs-not-in-ci.txt`, which the check requires,
so the list cannot grow silently.

## Repro

```sh
python3 tests/check-web-specs-in-ci.py   # PASS ... 11 excluded
```

## Fix directions

For each spec: add it to the desktop path list in `.github/workflows/ci.yml`
("Run broader smoke suite (desktop) -- BLOCKING"), delete its line from
`specs-not-in-ci.txt`, and fix what the PR's CI then reports -- that step is
the one place a bundle is built. Promote a few per PR, so a red one is easy to
pin. A spec that cannot run against the CI server (it needs network or a
deployed origin) keeps its line, with that reason in place of "not yet
checked".
