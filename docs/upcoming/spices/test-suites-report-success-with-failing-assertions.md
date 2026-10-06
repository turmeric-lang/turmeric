# Every spice's test suite reports success even when assertions fail

> **RESOLVED 2026-09-09 -- every `test/suite` file in the repo returns
> `run-all-status`; verified in both directions.**
>
> **What #60 did.** "Get all 45 spices building and testing, and make the
> suites able to fail" (`6cd1236`, 2026-08-28) converted the whole corpus in
> one pass -- 159 files under `spices/*/tests/` touched. Every test file that
> uses the `test/suite` DSL now ends `(run-all-status)`; the count on `main`
> is **129**.
>
> **What was left.** Nothing. `grep -rln "(run-all)" spices/*/tests/` still
> reports two files:
>
> - `spices/tourist/tests/router_test.tur`
> - `spices/tourist-ws/tests/route_test.tur`
>
> Both are **false positives**. Each already imports and calls
> `run-all-status`; the literal `(run-all)` the grep matches is inside the
> explanatory comment above the import, which quotes the old
> `(... (run-all) 0)` idiom this report describes. The grep in "Remaining
> work" cannot tell code from prose -- the correct check is
> `grep -L run-all-status` over the files that import `test/suite`, which
> comes back empty.
>
> **Verified in both directions** (2026-09-09, `tur` v0.46.0 built from
> source at `turmeric@e68592477`, stdlib 0.46.0), with three throwaway
> single-suite directories:
>
> | file | idiom | assertion | `tur test` |
> | --- | --- | --- | --- |
> | `pass_test.tur` | `(run-all-status)` | all pass | reports pass, **exit 0** |
> | `fail_test.tur` | `(run-all-status)` | one `assert-eq` fails | `FAIL`, **exit 1** |
> | `oldstyle_test.tur` | `(run-all)` then `0` | one `assert-eq` fails | reports **pass, exit 0** |
>
> The third row is this report's bug, still reproducible on demand -- which
> is what makes the first two meaningful rather than vacuous.
>
> **Suites re-run** (nothing went red):
>
> - `tourist`: `tur test tests` -> 18/18 assertions, exit 0.
> - `tourist-ws`: 6/6 assertions, exit 0. Needs mbedtls (transitively via
>   `ws-server` -> `tls`); on a current Apple clang the vendored mbedtls
>   build fails on `-Werror,-Wunterminated-string-initialization`, so it must
>   be configured `-DMBEDTLS_FATAL_WARNINGS=OFF`. That is a host-toolchain
>   issue in the dep, not a spice failure -- CI's Linux/macOS images build it
>   as-is.
>
> **Out of scope, noted for the record.** 142 test files with a `main` do not
> call `run-all-status` because they do not use the `test/suite` DSL at all
> (ansi, ecs, ecs-raylib, json, watch, signal, thread-pool, ...). Most roll
> their own TAP helper whose `tap-finish` / `finish` already `return 1` on
> any failure, so they were never affected. A subset of the ecs tests are
> golden-output programs that print values and end in a literal `0` -- for
> those `tur test` still only proves "compiles and does not crash", because
> nothing compares the printed output to an expectation. That is a
> **different** hole (no expected-output file) from the one this report
> describes, and it is not addressed here.
>
> - Runner: `spices/test/src/test/runner.tur` (`run-all-status`, exported in
>   `spices/test/build.tur`).
> - `spices/tourist/fixtures/*/` still call `(run-all)` on purpose: they need
>   a live listener, are not under `tests/`, and CI never runs them.


**Severity:** high (CI is green on assertion failures across the whole repo;
only *compile* failures are actually caught)
**Found:** 2026-08-18, while building `spices/secret` -- it hid a genuinely
failing assertion in that spice's hex tests, which only surfaced because the
TAP lines were being read by eye.
**Fixed in:** `spices/secret` and `spices/test` (this change). **Every other
spice is still affected.**

## Summary

`tur test` judges a test file by the exit code of the program it builds --
its own help text says so:

```
Each test compiles and runs; it passes iff both succeed (exit 0).
```

But the conventional `main` every spice uses ends in a hardcoded `0`:

```turmeric
(defn main [] : int
  (__all-tests)
  (run-all)
  0)            ;; <-- always 0, whatever happened
```

and `run-all` is `: void` -- it prints the TAP summary and returns nothing.
So a failing `it` prints `not ok`, `run-all` prints `# N passed, M failed.`,
and then the process exits 0 and `tur test` reports the file as **passed**.

Observed directly:

```
$ tur test tests
not ok 2 - encodes the digit/letter boundaries
...
5 tests, 5 passed, 0 failed
$ echo $?
0
```

The consequence is that `.github/workflows/ci.yml`'s "Run spice tests" step
has only ever verified that test files **compile and do not crash**. Any
assertion regression in any spice passes CI silently.

## Fix

`test/runner` gains `run-all-status`, which prints the same summary and
returns a process status (additive -- `run-all` is unchanged, so no existing
spice breaks):

```turmeric
(defn run-all-status [] : int
  (do
    (__runner-print (__ts 4) (__ts 0) (__ts 2))
    (if (> (__ts 2) 0) 1 0)))
```

A test file then ends:

```turmeric
(defn main [] : int
  (__all-tests)
  (run-all-status))
```

Verified in both directions in `spices/secret`: with a deliberately broken
expected value the suite now reports `5 tests, 4 passed, 1 failed` and exits
1; restored, it exits 0.

## Remaining work

`spices/secret` is converted. Every other spice still ends its `main` with a
literal `0` and needs the same two-line change:

```sh
grep -rln "(run-all)" spices/*/tests/
```

As of this report that is **119 test files across 28 spices**: c-dsl, frame,
glsl, http, httpd, json, opengl, osc, plot, plutovg, png, postgres, raylib,
regex, rtaudio, rtmidi, scscm, stats, template, tourist, tourist-session,
tourist-session-valkey, valkey, wav, ws-client, ws-core, ws-server, zlib.

This is mechanical, but it should be expected to **turn some suites red** --
that is the point, and those failures are pre-existing rather than caused by
the change. Worth doing spice-by-spice rather than in one sweep, so each red
suite can be triaged on its own.
