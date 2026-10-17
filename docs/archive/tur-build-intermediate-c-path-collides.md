# Concurrent `tur build`s of same-named files in different directories swap programs

**RESOLVED 2026-10-17** -- see [Resolution](#resolution-2026-10-17). Pinned by the `run.sh` suite itself (see below).

**Severity:** medium-high. Two builds running at once can silently produce each
other's binary.

## Repro

```sh
mkdir -p a b
printf '(defn main [] : int\n  (println "A")\n  0)\n' > a/m.tur
printf '(defn main [] : int\n  (println "B")\n  0)\n' > b/m.tur
for i in 1 2 3; do
  (cd a && tur build m.tur -o a$i) & (cd b && tur build m.tur -o b$i) & wait
done
for i in 1 2 3; do echo "a$i=$(a/a$i) b$i=$(b/b$i)"; done
```

One run printed `a3=B`: directory `a`'s binary runs directory `b`'s program.
The driver's own investigation saw 3 of 6 binaries swapped.

## Root cause

A single-file build writes its intermediate C to `/tmp/tur-build/`, at a name
from `src/main.c:stable_c_path`. That name is derived from the input path **as
typed** (`m.tur` -> `.../m_tur.c`), not its real path. Two concurrent builds
of `m.tur` from different directories write, compile and link the same
intermediate file.

## Fix directions

- Key the intermediate name on the input's `realpath` (or a hash of it), and
  include the process id or use `mkstemp` for the file actually written, then
  rename it.
- The same reasoning applies to any other `/tmp/tur-build` artifact named from
  the input.

## Resolution (2026-10-17)

`stable_c_path` (`src/main.c`) names the intermediate after the input's base
name and a hash of its **real** path: `m_tur-<16 hex>.c`. One stable name per
source file is what ccache needs, and two files with the same base name in
different directories no longer share one. (Two concurrent builds of the
*same* file still do.)

Fixing it turned up a second defect that made the first fix collide at once:
`tur_hamt_hash_xxh64` (`src/runtime/hamt.c`) never mixed in the bytes past
the last whole 32-byte stripe, or the input's length, for an input of 32
bytes or more. Every fixture path shares its first 32 bytes, so under the new
naming 9 fixtures built in parallel got 5 names, and `tests/run.sh` reported
pruned-unit TUR-W0071 failures with an empty cc log (one build's retry
truncated the other's `.err`). The tail loops now run after the stripes, and
the length is mixed in; an input under 32 bytes hashes exactly as before. See
[hamt-xxh64-ignores-bytes-past-the-last-stripe](hamt-xxh64-ignores-bytes-past-the-last-stripe.md).

The suite is the regression test for both: it builds ~1,400 `input.tur`
files in parallel, all under one directory.
