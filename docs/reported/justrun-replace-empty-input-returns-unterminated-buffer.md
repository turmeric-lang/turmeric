# `tur run`: `replace("", from, to)` returns an unterminated, uninitialized buffer

**Severity:** low. A Justfile that calls `replace` on an empty string gets
whatever byte `malloc` handed back, then reads past the 1-byte allocation
until it happens to hit a NUL. Usually that byte is already 0 and nothing
shows. Needs a Justfile you are already running, so it is not a security
boundary, but it is a real heap over-read. Found while triaging CodeQL alert
#223 (`cpp/unbounded-write`, `justrun.c:1945`). The alert itself is a false
positive: the allocation is sized correctly. This is a different defect on
the same lines.

## Repro

```just
x := replace("", "a", "b")

show:
    @echo "[{{x}}]" | od -c | head -2
```

```sh
$ tur run show                     # v0.61.0
0000000    [   ]  \n
$ MallocScribble=1 tur run show    # macOS: fill fresh allocations with 0xAA
0000000    [ 252   ]  \n
```

`252` is octal for `0xAA`, the scribble byte. It reached the recipe's
command line.

## Root cause

`src/compiler/justrun.c:1932-1952`, the `replace` builtin. It allocates
`strlen(s) + growth + 1` bytes, then fills them in a loop guarded by
`for (const char *q = s; *q; )`. Each exit path from the loop writes a NUL
(`strcpy` of the tail, or the `if (!*q)` branch). When `s` is `""`, the guard
is false on entry, so no exit path runs and `out[0]` is never written.

## Fix

Write `*w = '\0'` after the loop, or initialize `out[0] = '\0'` before it.
One line. A Justfile-builtin test (`replace("", ...)` -> `[]`) pins it.
