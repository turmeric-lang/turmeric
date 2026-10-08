# `fmt-idempotence-stdlib` silently tests nothing on macOS/BSD (`head -z`)

**Severity:** medium (a green test that checks zero files is worse than a red
one -- the coverage is gone and nothing says so).

Reported from the consumer side by the agent working on
[Trowel](https://github.com/turmeric-lang/trowel) while running the suite on
macOS. Confirmed here by reading the script and checking `head`'s options.

## Status (2026-07-27): FIXED

`head` is gone from the pipeline -- the sample is bounded by a counter inside
the loop -- and both stdlib-walking checks now fail rather than pass when the
enumeration yields nothing.

Verified by shadowing `head` with a stand-in that rejects `-z` the way BSD
does, and counting the `tur fmt` invocations the idempotence check actually
made:

| | old | new |
|---|---|---|
| `fmt --stdout` calls under BSD `head` | **0** | **20** |
| reported result | `PASS`, `17 passed, 1 failed` | `PASS`, `17 passed, 1 failed` |

The second row is the point. Both print the same summary a healthy Linux run
prints, which is why nobody noticed: macOS and Linux agreed on the numbers
while disagreeing completely on whether anything had been tested.

And with the enumeration itself broken (a `find` stand-in that prints
nothing):

| | old | new |
|---|---|---|
| result | `18 passed, 0 failed` | `16 passed, 2 failed` |

Note the old script's **greenest possible outcome was the one where it tested
nothing** -- greener than a real run, which carries the known `rcvec.tur`
failure. That inversion is what the new guard removes.

Fixed in `tests/run-fmt.sh`; no product code was involved.

## Summary

`tests/run-fmt.sh:297` feeds a NUL-separated file list through `head -z`:

```sh
done < <(find stdlib -name '*.tur' -not -name 'docstrings.tur' -print0 | head -z -n 20)
```

`-z` / `--zero-terminated` is a **GNU coreutils extension**. BSD `head`, which
is what ships on macOS, has no such option: it errors with
`head: illegal option -- z` and exits nonzero.

## Why this is worse than a plain failure

The failure is swallowed rather than surfaced. Trace it:

1. `head -z` fails, writing to stderr, and produces **no stdout**.
2. The `while IFS= read -r -d '' f` loop therefore never executes its body.
3. `IDEMPOTENT_FAIL` stays `0` -- it is only ever set inside that body.
4. The script reaches `if [ "$IDEMPOTENT_FAIL" -eq 0 ]; then pass "$NAME"; fi`.

So on macOS the test reports **PASS while checking zero files**. Formatter
idempotence -- `fmt(fmt(x)) == fmt(x)` over the stdlib -- has no coverage at
all there, and the summary line says everything is fine.

## Repro

On any BSD/macOS host, or on Linux with a BSD `head` on PATH:

```sh
$ find stdlib -name '*.tur' -print0 | head -z -n 20
head: illegal option -- z
usage: head [-n lines | -c bytes] [file ...]
$ echo $?
1
```

Then note that `tests/run-fmt.sh` still prints `PASS fmt-idempotence-stdlib`.

## What was done

Both parts of the original fix direction, plus one extension.

`head` is dropped and the sample bounded by `IDEMPOTENT_SEEN` inside the loop.
That is portable, and it removes the failure mode rather than relocating it.

The `pass` is now guarded by "at least one file was checked". Applied to
**both** stdlib-walking checks, not just the reported one: `fmt-bootstrap-stdlib`
directly above it has the identical shape -- it passes by default when its
accumulator stays empty -- so it would have gone silent in exactly the same way
on the next enumeration break. It is why the second table above shows the old
script scoring 18/0.

A scan of `tests/run-fmt.sh` for other GNU-only constructs (`sed -i`,
`readlink -f`, `grep -P`, `stat -c`, `find -printf`, `sort -z`, `xargs -d`, ...)
turned up nothing else, so `head -z` was the only one. `mktemp -d` appears
seven times without a template; that is left alone, since the consumer's macOS
run reached 17 passed / 1 failed, which it could not have done had those
failed.

## Related

`tests/run-fmt.sh` also carries a genuine pre-existing failure,
`fmt-bootstrap-stdlib` -- see
[fmt-bootstrap-stdlib-rcvec-not-self-formatted.md](fmt-bootstrap-stdlib-rcvec-not-self-formatted.md).
The two are independent; this one is portability, that one is a real
formatter/stdlib disagreement.
