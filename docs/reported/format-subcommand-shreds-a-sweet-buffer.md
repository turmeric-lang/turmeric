# `tur format` shreds a sweet-expression buffer

**Severity:** medium.
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

## Client-side consequence

*(Paths below are in [Trowel](https://github.com/rjungemann/trowel), a
separate repo — the editor this was found from. Included so the report
names a real consumer and so the workaround can be deleted alongside a
fix.)*

Trowel called `tur format` over stdin for Format File, so Format File on a
sweet buffer destroyed it. Now on `tur fmt --stdin --lang <reader>`.
