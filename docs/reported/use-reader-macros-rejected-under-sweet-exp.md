# `#use-reader-macros` is rejected in a `#lang sweet-exp` file

**Severity:** low. Reader-macro files cannot be pulled in from a sweet-exp
source; the workaround is to write the `reader-macros/define` form inline.

## Repro

```turmeric no-check
#lang sweet-exp
#use-reader-macros "stdlib/dedent-reader.tur"
```

`tur run` reports `error: unexpected character '#' (0x23)` at the directive.
The same two lines in a plain `.tur` file work. An inline
`reader-macros/define 'dd :raw-brace '$body` line works in the sweet file.

## Root cause (suspected, not traced to a fix)

`try_consume_use_directive` (`src/compiler/reader.c`) is tried only between
top-level forms of the plain reader. Under sweet-exp the preprocessor has
already rewritten the source, so the directive line no longer sits at top level
as a bare `#...` token. Found while adding `tests/fixtures/dedent-reader-sweet`.

## Fix directions

- Have the sweet preprocessor pass a leading `#use-reader-macros` line through
  untouched, so the directive stays top-level.
- Or recognize the directive as `(#use-reader-macros "...")`.
