# `tur_hamt_hash_xxh64` ignores every byte past the last 32-byte stripe

**RESOLVED 2026-10-17** (filed and fixed the same day) -- see
[Resolution](#resolution-2026-10-17). Pinned by the `run.sh` suite, which
builds ~1,400 fixtures whose paths share their first 32 bytes.

**Severity:** medium. Any two inputs of 32 bytes or more that agree on their
whole 32-byte stripes hash the same, whatever follows and however long they
are. The function keys more than HAMT buckets:

- the intermediate-C name of `tur build <file>` (`stable_c_path`), once that
  was keyed on the input's real path -- the way this was found;
- the r7rs library-unit object and emission caches under
  `$TMPDIR/tur-build/prelude/`, keyed by the unit's text, so two units that
  differ only after their last whole stripe could share a cached object;
- the split-runtime artifact hash cmd_jit compares against;
- interned runtime symbols (`symbols.c`) and every HAMT map keyed by a string
  of 32+ bytes, where it is a performance cliff (one bucket) rather than a
  wrong answer, since the HAMT compares keys on collision.

## Repro

Before the fix, with `stable_c_path` naming the file after a hash of the real
path:

```sh
for f in borrow-defer borrow-deref tce1-vec-bool json-reader-escape \
         module-macro-refer r7rs-srfi-60 exg4-exists-clone exg4-pack-into-fn \
         cloneable-owning-consuming-recursive; do
  (TUR_SHOW_CC=1 tur build tests/fixtures/$f/input.tur -o /tmp/x-$f 2>&1 |
     grep -o '/tmp/tur-build/input[^ ]*\.c') &
done; wait
```

Nine fixtures, five distinct names. `tests/run.sh` then reported a handful of
"pruned unit failed to compile" (TUR-W0071) failures with an empty cc log,
different ones each run.

## Root cause

`src/runtime/hamt.c:tur_hamt_hash_xxh64` (a minimal, non-standard xxHash64)
had two branches. The `len >= 32` branch consumed whole 32-byte stripes and
then went straight to the finalizer; the 8-, 4- and 1-byte tail loops sat in
the `len < 32` branch only. The input length was never mixed in on the long
branch either.

## Resolution (2026-10-17)

The tail loops run after the stripe loop for every input, and the long branch
adds the total length to the accumulator, as xxHash64 does. An input under 32
bytes takes exactly the operations it took before, so its hash -- and the
iteration order of a map with short string keys -- is unchanged. There is a
single implementation; the compiler and the runtime link the same function, so
compile-time symbol hashes and run-time ones still agree. The split-runtime
artifacts were regenerated for the new recorded hash.

Found fixing
[tur-build-intermediate-c-path-collides](tur-build-intermediate-c-path-collides.md).
