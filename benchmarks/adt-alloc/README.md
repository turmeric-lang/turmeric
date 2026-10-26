# ADT allocation ceiling harness

Prices the fixes proposed in
[docs/archive/multi-variant-adts-always-heap-allocate.md](../../docs/archive/multi-variant-adts-always-heap-allocate.md)
before anyone spends compiler time on them: seven representations of the same
workload (build an n-binding substitution, walk all n, discard), so the two
proposed fixes can be costed separately, together, and against region reclamation.

```sh
gcc -O2 -D_POSIX_C_SOURCE=200809L -o /tmp/ceiling benchmarks/adt-alloc/ceiling.c -std=c99
for rep in A B C D E F G; do /tmp/ceiling $rep 16000; done
```

`A` is what the compiler emits today. Output is `rep,passes,ns_per_op,checksum`;
the checksum must match across all seven, which is what keeps a representation
from looking fast by doing less.

Results and the reading of them: [RESULTS.md](RESULTS.md). Rows `F` and `G`
(arena reclamation, with and without the by-value ABI change) were added
2026-08-25 and are the ones that matter -- `G` reaches 7.64x with no ABI change
at all.

**The harness was missing from the tree until 2026-08-25.** `.gitignore` has a
blanket `*.c` with negations for `src/ tests/ examples/ docs/ tools/` but not
`benchmarks/`, so the commit that published the original numbers landed with
only this README and the measurement could not be re-run. The negation is fixed
and `ceiling.c` is reconstructed, but note that the numbers in RESULTS.md are a
fresh measurement rather than the original's -- two of the published rows do not
reproduce.

**Run each representation in its own process, as the loop above does.** A, C and
E leak by design -- that is the thing being modelled -- so running them in
sequence lets whichever goes first hand its heap to the rest. Doing that
produced a fake 8x "degradation" that took a rewrite of the report to undo.
