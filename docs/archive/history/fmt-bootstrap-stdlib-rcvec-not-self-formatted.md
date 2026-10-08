# `tur fmt` mislays a `defn` header when it carries a type-parameter list

**Severity:** low (cosmetic, and stable -- but it keeps `fmt-bootstrap-stdlib`
permanently red, which trains people to ignore that suite).

Surfaced as the long-standing `tests/run-fmt.sh` failure
`fmt-bootstrap-stdlib: stdlib is not self-formatted: stdlib/rcvec.tur`.
Independently confirmed as pre-existing by the agent working on
[Trowel](https://github.com/turmeric-lang/trowel) (17 passed / 1 failed both
before and after the `claude/busy-clarke-6zj9jl` formatter refactor).

## Status (2026-07-27): FIXED

`tests/run-fmt.sh` is now **18 passed, 0 failed** -- green for the first time.

Two corrections to what this report originally said, both worth recording
because the original diagnosis would have sent the next person the wrong way.

**The root cause was not a wrapping decision.** `fmt_defn` in
`src/compiler/fmt.c` assumed the parameter vector was always item 2 and did
not know a type-parameter vector could precede it. With `[A]` present, the
real parameter vector *and* the return annotation fell past the header and
were emitted as body forms, one per line. Nothing was measuring a width and
deciding to break; the header simply stopped early. The "fits well inside the
80-column budget" observation was true but irrelevant.

**The recommendation was backwards.** This report said prefer fixing the
formatter and reach for reformatting `rcvec.tur` "only to unblock". The real
answer needed both, and the second part was much larger than "one file":
fixing the formatter turned **1** dirty stdlib file into **23**, because those
23 carry the mislaid layout the bug produced. `rcvec.tur` was the only file
already written the way the formatter *should* emit.

What settled it: the formatter already lays out a **non**-type-parameterized
`defn` as header-on-one-line with the body indented two -- exactly the style
CLAUDE.md documents -- and only the type-parameterized path deviated. So the
fix makes the two consistent, and the 23 files were the ones out of step.

Resolution: fix `fmt_defn` (below), then `tur fmt stdlib/`. 23 files,
171 insertions / 339 deletions -- it removes lines rather than adding them.

## Summary

For a `defn` with a **type-parameter list** (`[A]`) followed by the ordinary
parameter vector, `tur fmt` breaks the header across three lines even when the
whole thing fits well inside the 80-column budget.

## Repro

```sh
$ ./build/tur fmt --stdout stdlib/rcvec.tur | diff stdlib/rcvec.tur -
```

```
119c119,121
< (defn rcvec-push! [A] [^borrow v : rc<RcVec> ^borrow item : rc<A>] : void
---
> (defn rcvec-push! [A]
>   [^borrow v : rc<RcVec> ^borrow item : rc<A>]
>   : void
```

The source line is **73 columns**; the second case (`rcvec-get`, line 154) is
**59**. Both are comfortably under `opts.line_width = 80`, so the split is not
a wrapping decision -- something in the layout path is treating the
type-parameter list as forcing a break.

Only these two forms in the file differ; the total diff is 12 lines.

> **Correction (see Status above).** The last sentence of that paragraph is
> wrong, and the width measurements -- while accurate -- are a red herring. The
> layout path was not "treating the type-parameter list as forcing a break"; it
> did not know type-parameter lists existed, so the parameter vector and return
> annotation were classified as body forms. Reading a width-looking symptom as
> a width decision is what made the original fix direction point the wrong way.

## Not an idempotence bug

Worth separating, because the two failures look alike from the summary line:

```sh
$ ./build/tur fmt --stdout stdlib/rcvec.tur | ./build/tur fmt --stdin \
    | diff - <(./build/tur fmt --stdout stdlib/rcvec.tur)   # identical
```

`fmt(fmt(x)) == fmt(x)` holds. The formatter is self-consistent; it simply
disagrees with the checked-in source. So `fmt-idempotence-stdlib` passes and
only `fmt-bootstrap-stdlib` fails.

## What was done

`fmt_defn` now locates the parameter vector instead of assuming index 2, via a
new `defn_params_index()` covering all three shapes:

```
(defn name [params] :ret body)                      -> params at 2
(defn name [A B] [params] :ret body)                -> params at 3
(defn name [A] [(Class A) ...] [params] :ret body)   -> params at 4
```

That mirrors the elaborator's detection in `elab_fns.c` **exactly**, and has
to keep doing so. The two-consecutive-vectors test is genuinely ambiguous in
the language -- `(defn f [x] [1 2 3])` reads as a type-parameterized defn, not
as one returning a vector literal -- so a formatter that disambiguated it
differently would reprint code as something that means something else.
Agreeing with the elaborator is the only safe rule, whatever one thinks of the
ambiguity. The constraint-vector case needs the same "looks like
`(ClassName TyVar ...)` forms" heuristic the elaborator uses, so that is
copied too.

### Verification

The stdlib reformat is whitespace-only, and was checked rather than assumed:

- `emit-c` output byte-identical before and after, on three stdlib-heavy
  fixtures.
- `tools/gendocs.py --emit-tur` output byte-identical, so docstring extraction
  is unaffected and `stdlib/docstrings.tur` needs no regeneration.
- `stdlib/docstrings.tur` itself is excluded from the reformat -- it is
  generated by `gendocs.py`, and the fmt checks already skip it because the
  formatter does not round-trip its inline-C literal bodies. `tur fmt stdlib/`
  does rewrite it, so it was reverted explicitly.
- No `tests/fixtures/*/expected.*` file cites a stdlib line number, so shifting
  them breaks no diagnostic snapshot. (`actual.*` files do, but they are
  generated and untracked.)
- Full suite 2399 passed / 0 failed; `tests/run-fmt.sh` 18/0;
  ctest `fmt|lsp|mcp|tur_repl` 10/10.

As predicted in the original report, `tests/fixtures/*/expected.c` is
untouched -- this is `tur fmt` output, not codegen.

### Left alone

The effect annotation in `(defn some [A] [x : A] #fx{Construct} : (Option A) ...)`
still sits at body indent rather than in the header, because `#fx{...}` is
neither a keyword nor a type annotation and `header_end` stops before it. That
behaviour is unchanged by this fix and is now *consistent* between
type-parameterized and plain defns, which it was not before. Worth revisiting
if the header should absorb effect annotations, but that is a separate layout
question.

## Related

`tests/run-fmt.sh` additionally failed to run its idempotence check at all on
macOS/BSD -- fixed, see
[../archive/fmt-idempotence-head-z-silently-skips-on-bsd.md](fmt-idempotence-head-z-silently-skips-on-bsd.md).
That fix also added a "checked at least one file" guard to *this* test, so a
future break in the stdlib enumeration fails here rather than passing
vacuously.
