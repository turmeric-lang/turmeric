# `parse_shell_array` spins forever on a value that consumes nothing

**RESOLVED 2026-10-03** -- found and fixed in the same change. The guard is in
`parse_shell_array`, the reproducer is pinned as
`tests/fuzz/seeds/fuzz_justfile/shell-array-comment-hang.bin`, and the
`realloc`-failure leak next to it is fixed too. Kept for the paper trail
because this is the **second** time the same defect was found in this file, and
the first fix did not generalize.

**Severity when open: medium.** `tur run` on a Justfile whose `set shell :=`
array contains a `#`, `\n` or `\r` where a value is expected looped without
bound, allocating one empty string and doubling one pointer array per turn,
until the process was killed or the allocator failed. A denial of service on
untrusted Justfile input, not a memory-safety defect: nothing was written out
of bounds, and the loop made no progress to corrupt.

Found by the nightly `Fuzz parsers (libFuzzer)` job, 2026-10-01
(`fuzz_justfile`), reported to Sentry as `TURMERIC-CI-2`
(event `57cb91361b5d41738287d760c54a6fa5`).

## Root cause

`parse_shell_array` advanced its cursor only by whatever `parse_value`
consumed, and never checked that it consumed anything:

```c
const char *p = s + 1;
while (*p && *p != ']') {
    while (*p == ' ' || *p == '\t' || *p == ',') p++;   /* skip */
    if (*p == ']' || !*p) break;                       /* break */
    const char *end;
    char *val = parse_value(p, &end);
    p = end;                                           /* no guard */
    if (*n_out >= cap) {
        cap *= 2;
        arr = (char **)realloc(arr, (size_t)cap * sizeof(char *));
        if (!arr) return NULL;
    }
    arr[(*n_out)++] = val;
}
```

`parse_value` returns **zero progress** -- it sets `*end == p` -- whenever,
after its own leading space/tab skip, it is looking at `#`, `\n` or `\r`: the
bare-word loop stops on each of those without advancing, and `*end` is then
still `start`.

```c
    /* Bare word */
    const char *start = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && *p != '#')
        p++;
    if (end) *end = p;
    return jr_strndup(start, (size_t)(p - start));
```

None of the three was handled by the caller: the skip covers only space, tab
and comma, and the break covers only `]` and NUL. So `p` stalled, the guard
conditions stayed false, and each turn appended one empty `jr_strndup` result
and eventually doubled `cap`. Both faces of that were observed:

* **The allocation.** `arr` doubles until the request is 2 GB:
  `ERROR: libFuzzer: out-of-memory (malloc(2147483648))` at
  `parse_shell_array`. 2 GB of `char *` is 2^28 entries, so the loop had turned
  ~268 million times -- it was unbounded, not merely slow.
* **The time.** CI, with less RSS headroom, hit its watchdog first:
  `libFuzzer: timeout after 15 seconds`, stack
  `jr_strndup <- parse_shell_array`.

`realloc` failing was also mishandled: it returned `NULL` having already lost
the old `arr`, leaking it and every string in it, while `*n_out` was left
non-zero for the caller to index.

## This was the same defect as the one already fixed next door

The identical shape had already been found by this same fuzzer in the
recipe-dependency argument loop and fixed there, with a comment that names the
mechanism exactly:

```c
char *arg = parse_value(p, &end);
/* parse_value consumes nothing at a '#' or '\r', which this
 * loop did not stop on: `(dep #` spun here forever, growing
 * args by one empty string per turn -- `tur run --list` on
 * such a Justfile hung until it ran out of memory (found by
 * tests/fuzz/fuzz_justfile). */
if (end == p) { free(arg); break; }
p = end;
```

Its regression seed is `tests/fuzz/seeds/fuzz_justfile/dep-args-comment-hang.bin`.
The one-line guard was never applied to `parse_shell_array`, which is the only
other loop in the file that advances by `parse_value`'s span. **That is the
lesson worth keeping: the first fix was written as a fix to one loop, not to
the `parse_value`-returns-nothing contract, so the sibling kept the bug for as
long as it took the fuzzer to reach it a second way.**

## Repro

CI's reproducer is pinned as
`tests/fuzz/seeds/fuzz_justfile/shell-array-comment-hang.bin` (22882 bytes,
from the Sentry attachment on `TURMERIC-CI-2`). Before the fix it hung
deterministically:

```sh
build-fuzz/fuzz/fuzz_justfile tests/fuzz/seeds/fuzz_justfile/shell-array-comment-hang.bin
```

The triggering line in it is at offset 14864:

```
set shell := [# Default: configure (if needed) then debug build
```

Note that line **in isolation does not reproduce**, which is why the seed is
the unreduced artifact rather than a tidy one-liner: surrounding context
decides how far the logical-line scanner (which honors `#` as a comment only at
bracket depth 0) extends the unterminated `[`, and therefore whether a
`#`/newline ends up inside the span `parse_shell_array` walks. Three
hand-written one-liners that looked like they should trigger it did not.
Reducing it further is a nicety, not a prerequisite.

## The fix

1. **The guard**, mirroring the sibling loop:

   ```c
   char *val = parse_value(p, &end);
   if (end == p) { free(val); break; }
   p = end;
   ```

2. **The reproducer as a seed**, as the workflow's own note asks, so the
   `ctest -R '^fuzz_justfile'` replay covers it.

3. **The `realloc` loss**: the new pointer goes to a temporary, and on failure
   the collected strings and the old array are freed and `*n_out` is zeroed
   rather than leaked.

## Still open after this

A third instance is not ruled out by inspection alone. The two known sites were
found one at a time, by the same fuzzer finding the same shape twice. Anything
that advances a cursor by `parse_value`'s span wants the same guard; a sweep
for `parse_value(` feeding a loop cursor is cheap insurance.
