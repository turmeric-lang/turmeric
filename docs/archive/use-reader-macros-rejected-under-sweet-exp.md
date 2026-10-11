# `#use-reader-macros` is rejected in a `#lang sweet-exp` file

**Status:** RESOLVED 2026-10-11 (see [Resolution](#resolution)).

**Severity:** low. Reader-macro files cannot be pulled in from a sweet-exp
source; the workaround is to write the `reader-macros/define` form inline.

## Repro

```turmeric no-check
#lang sweet-exp
#use-reader-macros "stdlib/string-reader.tur"
```

`tur run` reports `error: unexpected character '#' (0x23)` at the directive.
The same two lines in a plain `.tur` file work. An inline
`reader-macros/define 'dd :raw-brace '$body` line works in the sweet file.

## Root cause (suspected, not traced to a fix)

`try_consume_use_directive` (`src/compiler/reader.c`) is tried only between
top-level forms of the plain reader. Under sweet-exp the preprocessor has
already rewritten the source, so the directive line no longer sits at top level
as a bare `#...` token. Found while writing the first `#dedent` fixture.

## Fix directions

- Have the sweet preprocessor pass a leading `#use-reader-macros` line through
  untouched, so the directive stays top-level.
- Or recognize the directive as `(#use-reader-macros "...")`.

## Resolution

The suspected cause was right. The t-expression preprocessor
(`sweet_analyze_top`, `src/compiler/reader.c`) treated the directive line as a
two-element line and wrapped it: `(#use-reader-macros "...")`. The plain reader
consumes the directive only between top-level forms
(`try_consume_use_directive`), so it met `#` inside a list.

Fix direction 1: a line at column 0 that starts with `#use-reader-macros`
(as a whole word, not a GROUP line) is left unwrapped, so it reaches the plain
reader as a top-level directive. Nothing else about the file's layout changes.

**Pinned by** `sweet-use-reader-macros` (a `#lang sweet-exp` file pulling two
`reader-macros/define` macros from a sibling file).
