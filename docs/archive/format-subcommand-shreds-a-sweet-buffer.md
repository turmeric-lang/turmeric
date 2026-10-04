# `tur format` shreds a sweet-expression buffer

**Severity:** medium.
**Status:** RESOLVED (2026-10-03). `tur format` resolves the reader from `--lang`, the extension and the `#lang` line. See [Resolution](#resolution).
**Found:** 2026-10-03, against v0.60.1.
**Impact:** the legacy stdin formatter destroys sweet-expression source. One
token per line.

## What happens

```sh
$ cat b.tur.sweet
defn double [x]
  {x * 2}

defn main []
  println(double(21))
  0

$ tur format < b.tur.sweet
defn

double

[x]

(* x 2)

defn

main

[]

println

(double (21))

0
```

Every token on its own line, blank-separated. `tur fmt --stdin --lang sweet` on
the same input at least produces valid (if converted) source — see
[fmt-reprints-sweet-as-s-expressions](fmt-reprints-sweet-as-s-expressions.md).

The file form does the same: `tur format b.tur.sweet` shreds it identically.
**Output is stdout only** — there is no in-place mode (`usage_format`,
`src/main.c:10984`) and the file on disk is left untouched, so this destroys
source only through a client that writes the output back. Which is how it was
found.

## Why it happens

`tur format` (`cmd_format`, dispatched at `src/main.c:13687`) takes no
`--lang`, so a buffer arriving on stdin is read with the default reader. Under
the Turmeric reader each indented sweet line is a separate top-level form, and
the printer dutifully prints each one.

## Suggested fix

`tur format` is a strictly older surface than `tur fmt --stdin`, which has the
`--lang` flag and the mode matrix. Either deprecate it in favour of that, or
have it refuse input it cannot identify rather than reformatting it under an
assumed reader. A formatter that can silently destroy its input on a file type
the compiler supports is worth failing closed.

## Resolution

The reach was wider than the filing said. `cmd_format` hard-coded
`READER_TURMERIC` even when given a file path, and checked neither the
extension nor a `#lang` line, so `tur format x.scm` and a `#lang r7rs` `.tur`
file were read as plain Turmeric too -- not only stdin.

`cmd_format` now takes its reader from `--lang` if given, else the file's
extension, and sends anything that is not plain Turmeric (by extension or by
`#lang` line) through `fmt_format_document`, the path `tur fmt` uses. So a
sweet file is kept as written (see
[fmt-reprints-sweet-as-s-expressions](fmt-reprints-sweet-as-s-expressions.md))
and a Scheme file is re-indented. Plain Turmeric keeps its existing path,
including the manifest reader-macro preload. `--lang` takes the same
vocabulary as `tur fmt --lang` (one shared helper,
`fmt_reader_from_lang_name`), and the usage text points at `tur fmt`.

One case is still not detectable: a headerless sweet buffer on stdin with no
`--lang` has no extension and no directive to go on, and is read as Turmeric.
A caller piping a sweet buffer must pass `--lang`, as it must to `tur fmt
--stdin`.

Pinned in `tests/run-fmt.sh` (`format-sweet-file-kept`,
`format-stdin-lang-sweet`, `format-check-sweet-already-formatted`).

## Client-side consequence

*(Paths below are in [Trowel](https://github.com/rjungemann/trowel), a
separate repo — the editor this was found from. Included so the report
names a real consumer and so the workaround can be deleted alongside a
fix.)*

Trowel called `tur format` over stdin for Format File, so Format File on a
sweet buffer destroyed it. Now on `tur fmt --stdin --lang <reader>`.
