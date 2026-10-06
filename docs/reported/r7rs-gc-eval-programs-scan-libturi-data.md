# r7rs-gc: a `(scheme eval)` program's every collection scans `libturi`'s 46 MB of data, which holds no collector pointer

**Severity:** low-medium (run time of eval programs; CI time under torture).
The collector scans the executable's whole writable data and bss as roots
(`tur_gc_scan_data`, src/runtime/r7gc.c: `__data_start` .. `_end`). A
program that imports `(scheme eval)` links `libturi.a`, the embedded
interpreter, which brings its globals into that range. In the Debug
configuration CI uses, `libturi.a` is ASan-instrumented and its globals carry
redzones. A collection then reads ~46 MB that cannot matter: the interpreter
allocates with libc's `malloc` (only the program's own translation units are
redirected to the collector), so its globals point at libc memory, not at
this heap. Found 2026-10-05.

## Measured (Debug `tur`, `TUR_GC_TORTURE=31`)

```
size r7rs-eval binary:      text 36.0 MB   data 22.1 MB   bss 24.4 MB
size r7rs-strings binary:   text  0.7 MB   data  0.007 MB bss  0.09 MB

r7rs-eval:     96 collections, 3.68 s -> 38 ms per collection
r7rs-strings:  26 collections, 0.20 s -> ~8 ms per collection (mostly the program)
```

The eval fixtures (`docs-r7rs-guide-examples`, `r7rs-eval`,
`r7rs-features-agree`, `r7rs-srfi-64-read-eval`, ...) take 1-9 s each under
torture, almost all of it this scan. Outside torture the cost is 38 ms per
ordinary collection.

## Fix directions

1. **Scan only the data of the units that allocate on this heap.** The
   program unit, the cached prelude object and `libturt_runtime.a` (whose
   HAMT nodes come from the collector through `rt_alloc.h`) are the roots
   that matter. Bracket their data with linker-visible markers. For example,
   put each unit's mutable runtime globals in a named section
   (`__attribute__((section("tur_gc_roots")))`), and have GNU ld's automatic
   `__start_tur_gc_roots` / `__stop_tur_gc_roots` give the bounds. Mach-O has
   `getsectiondata` for the same. Then scan that section instead of the
   whole data segment. The program's own `static` data written by inline C
   would need the same attribute or a fallback. This is the real fix, but it
   needs care: a missed root is a use-after-free.
2. **Exclude `libturi`'s range.** Have `libturi.a` export
   `__turi_data_begin` / `__turi_data_end` from its first and last object (or
   a section, as above), and skip that sub-range in `tur_gc_scan_data`.
   Smaller, and safe for the same reason the interpreter's memory is not
   collected.
3. **Release `libturi` only.** An unsanitized `libturi.a` has far smaller
   globals. That changes the cost, not the shape, and CI's Debug build is
   where it hurts.

## Pinned by

Nothing yet. A check could compare per-collection time
(`TUR_GC_STATS=1`, torture) of an eval program against a plain one.
