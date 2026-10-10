# r7rs-gc: a `(scheme eval)` program's every collection scans `libturi`'s 46 MB of data, which holds no collector pointer

**RESOLVED 2026-10-09 (Linux).** Fix direction 2, by sections: libturi's
data and bss are moved into sections of their own when the archive is built,
and the collector skips them. See "Resolution" at the end.


**Narrowed 2026-10-08: the scan is ~4.6x cheaper; what it scans is unchanged.**
`tur_gc_scan` passed every word to `tur_gc_mark_word`, a call that re-read the
heap's bounds from the collector state for each one, so the per-word cost was
the call and two reloads -- not the memory.  The bounds cannot move while the
world is stopped (marking allocates its stack with `tur_gc_os`, never on the
heap), so the scan reads them once per range and tests each word inline,
calling into the marker only for a word inside the heap.  Measured on Linux
x86-64, Debug `tur`, `TUR_GC_TORTURE=31`, same collection counts before and
after:

| program | collections | without torture | before | after | per collection |
| --- | --- | --- | --- | --- | --- |
| `r7rs-eval` | 96 | 0.32-0.36 s | 1.84 s | 0.68 s | 15.8 ms -> 3.4 ms |
| `r7rs-srfi-64-read-eval` | 172 | 0.33 s | -- | 0.97 s | 3.7 ms |
| `docs-r7rs-guide-examples` | 343 | 0.28 s | -- | 1.65 s | 4.0 ms |
| `r7rs-strings` (no libturi) | 26 | 0.004 s | -- | 0.004 s | ~0 |

`tests/run-r7rs-gc.sh`: 249 passed, 0 failed.  The ~46 MB of libturi data and
bss are still scanned on every collection; fix directions 1 and 2 below are
what would stop that, and remain open.

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

## Resolution (2026-10-09)

- **The build** (`src/CMakeLists.txt`, a POST_BUILD step on `libturi`, Linux
  with an `objcopy`): every member's `.data`, `.data.rel.local` and
  `.data.rel` become `tur_turi_data`, and `.bss` becomes `tur_turi_bss`. Both
  names are C identifiers, so GNU ld and lld define
  `__start_tur_turi_{data,bss}` / `__stop_...` for a program that references
  them. (`.data.rel.ro*` lies below `__data_start`, outside the scan already;
  the TLS sections are not scanned.)
- **The collector** (`tur_gc_scan_data`, `src/runtime/r7gc.c`) declares the
  four symbols weak and scans `__data_start .. _end` minus those two ranges.
  A program that does not link libturi, or a libturi built without the step,
  reads them as null and scans as before.

Why this is safe, checked rather than assumed: nothing in libturi's globals
can point into the collector's heap. The interpreter allocates with libc, and
the `(scheme eval)` bridge (`stdlib/r7rs/eval.tur`) hands it datum TEXT and
integer ids for host values and procedures, never a heap address.

Measured (Linux, Debug `tur`, `TUR_GC_TORTURE=31`), `r7rs-eval`: 96
collections, 0.73-0.84 s before, 0.32-0.47 s after -- the same as the program
without torture; 42 MiB of the 46 are skipped (the brackets in the linked
binary: `tur_turi_data` 20 MB, `tur_turi_bss` 24 MB). Same output.
`tests/run-r7rs-gc.sh` 250 passed (249 plus the new check), and the libturi
embedding ctests (`tur_eval_*`) pass against the rewritten archive.

Pinned by `tests/run-r7rs-gc.sh` check 5, `libturi-not-a-root`: it builds
`r7rs-eval` and requires the four brackets (defined only when the collector
references them) and more than 1 MiB between them.

Not done: macOS. Its scan walks the writable Mach-O segments, and the same
move would need `__DATA,<section>` names plus `getsectiondata`; the Debug
cost there is the same 46 MB and is left as it was.
