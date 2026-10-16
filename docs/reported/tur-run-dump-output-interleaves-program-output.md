# `tur --dump-* run` interleaves the program's output into the dump

**Severity:** low. With stdout redirected, a dump's buffered text and the
program's output come out in the wrong order, mid-line.

## Repro

```
$ tur --dump-effects run hello.tur > out     # hello.tur prints "hi"
$ grep -n hi out
457:defn schema-decode-abort hi
```

The program's `hi` lands in the middle of a dump line.

## Root cause

The dump writes to `tur`'s buffered stdout. `tur run` then launches the built
program with `system()` (the `RUN_ENTRY` path in `src/main.c`) without
flushing, so the child's output reaches the file before the parent's buffered
dump text does.

## Fix directions

- `fflush(NULL)` before every `system()`/`exec` of a built program. The driver
  already notes that stdout must be flushed before running a program
  ([build-driver-internals-guide](../guides/build-driver-internals-guide.md));
  this path misses it.
