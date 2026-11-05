# A module `def` holding a heap value nobody reads is reported as a leak

> **RESOLVED 2026-11-05.** A file-local top-level `def` with an initializer
> is now emitted `static __attribute__((used))`
> (`src/compiler/emit_module.c`), so the C compiler keeps the global and its
> store and LeakSanitizer sees the box as reachable. Measured with
> `run-leak-check.sh`'s flags (`-O1 -fsanitize=address`, source runtime): the
> new `module-def-unread-heap-not-leaked` fixture leaked 8 bytes in 2
> allocations before and is clean after. 162 `expected.c` snapshots changed,
> each only by the attribute.

**Severity: low (a false positive that fails a leak-checked test).**
Found 2026-10-29 cleaning up after module-type-names-collide-across-modules.

## Repro

tur-dom-core's `dom/core` has

```turmeric
(defdata ^heap ^copy ParseErrorKind (Syntax) (Limit) (Refused) (Io))
(def __syntax-node (Syntax))
(def __limit-node (Limit))
```

read only by `syntax-kind`, `limit-kind`, ... A program that loads `dom/core`
and calls none of them -- in turmeric-spices, tourist's markup test rewritten
to serve its pages from a tourist server and fetch them with `http/client` --
fails `tur test` under LeakSanitizer with four `Direct leak of 4 byte(s)`
allocated from `tur_region_alloc_or_malloc` in `main`.

## Root cause

Each `def` is a `static int64_t` global stored once in
`__tur_module_def_init`. When nothing reads the global, the C compiler drops
the global and the store, so the only pointer to the constructor's box is
gone and LeakSanitizer reports a process-lifetime value as leaked. Whether it
happens depends on what else the optimizer can see, so the same module is
clean in one program and leaking in another.

## Fix directions

- Emit a module `def` global with `__attribute__((used))` (or as a
  non-`static` symbol), so its store survives and the box stays reachable.
  Every snapshot with a module `def` changes.
- Or skip the initializer of a `def` nothing reads.
