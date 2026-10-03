# `tur fmt` reprints sweet-expressions as s-expressions, and keeps the `#lang` header

**Severity:** medium.
**Found:** 2026-10-03, against v0.60.1.
**Impact:** formatting a sweet-expression file silently converts it to another
syntax, and leaves behind a header that contradicts the body.

## What happens

```sh
$ cat b.tur.sweet
#lang turmeric/sweet

defn double [x]
  {x * 2}

$ tur fmt --stdout b.tur.sweet
#lang turmeric/sweet
(defn double [x] (* x 2))
```

The body is reprinted as s-expressions. The `#lang turmeric/sweet` line is
preserved verbatim — deliberately, per the comment above `fmt_format_source` in
`src/main.c` ("not an s-expression ... re-emitting it from a parse would be
inventing a canonical spelling") — so the result is a file that announces the
sweet reader and contains none of it.

The same through stdin:

```sh
$ tur fmt --stdin --lang sweet < b.tur.sweet
(defn double [x] (* x 2))

(defn main [] (println (double 21)) 0)
```

## `r7rs/sweet` already does the right thing

```sh
$ tur fmt --stdin --lang r7rs/sweet < d.sscm
define (f x)
  * x 2
display(f(21))
```

Checked and returned as written — `src/main.c:8925` says so in as many words
("Checked, and kept as written"). So the two sweet readers behave differently,
and the Scheme one behaves correctly.

## Suggested fix

Either treat `turmeric/sweet` and `saffron/sweet` the way `r7rs/sweet` is
already treated — check, re-indent at most, keep as written — or decline them
out loud so a caller can tell. Silently emitting another dialect is the one
option that cannot be worked around by a client, because the output looks like
a successful format.

A `#lang`-carrying `r7rs/sweet` buffer is also not quite byte-identical: the
header survives but the blank line after it is dropped. Minor, and not
destructive, but worth folding into the same fix.

## Client-side consequence

*(Paths below are in [Trowel](https://github.com/rjungemann/trowel), a
separate repo — the editor this was found from. Included so the report
names a real consumer and so the workaround can be deleted alongside a
fix.)*

Trowel declines Format File for `turmeric/sweet` and `saffron/sweet` rather
than offering an action that destroys the buffer (`DialectIsFormattable`,
`src/editor/dialect.cpp`). That guard goes away when this does.
