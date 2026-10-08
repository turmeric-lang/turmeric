#!/usr/bin/env bash
# check-r7gc-slot-of.sh -- the collector's divide-free slot index is exact.
#
# src/runtime/r7gc.c finds the slot a word points into with
# `(off * inv) >> 48`, inv = 2^48 / size + 1 (tur_gc_slot_of), instead of
# `off / size`: the mark phase does it for every candidate word, and the
# divide was the costliest instruction in it.  This checks the identity for
# every size class the table declares, at every offset in a 64 KiB chunk.
# A class added to the table is checked with no change here.
set -euo pipefail
cd "$(dirname "$0")/.."
sizes="$(sed -n '/tur_gc_class_size\[TUR_GC_NCLASS\] = {/,/};/p' src/runtime/r7gc.c | sed 1d \
         | tr -c '0-9\n' ' ' | tr -s ' ' '\n' | grep -E '^[0-9]+$' | grep -vx 0 | tr '\n' ',')"
chunk="$(grep -oE 'define TUR_GC_CHUNK +\(\(uintptr_t\)1 << [0-9]+' src/runtime/r7gc.c | grep -oE '[0-9]+$')"
[ -n "$sizes" ] && [ -n "$chunk" ] || { echo "FAIL check-r7gc-slot-of: could not read the class table"; exit 1; }
d="$(mktemp -d)"; trap 'rm -rf "$d"' EXIT
cat > "$d/t.c" <<C
#include <stdint.h>
#include <stdio.h>
static const uint64_t sizes[] = { ${sizes} };
int main(void) {
    unsigned n = 0;
    for (unsigned c = 0; c < sizeof sizes / sizeof sizes[0]; c++) {
        uint64_t sz = sizes[c], inv = ((uint64_t)1 << 48) / sz + 1;
        for (uint64_t off = 0; off < ((uint64_t)1 << ${chunk}); off++)
            if (((off * inv) >> 48) != off / sz) {
                printf("FAIL check-r7gc-slot-of: size %llu offset %llu\n",
                       (unsigned long long)sz, (unsigned long long)off);
                return 1;
            }
        n++;
    }
    printf("PASS check-r7gc-slot-of (%u classes, every offset in a 2^${chunk} chunk)\n", n);
    return 0;
}
C
${CC:-cc} -O2 -o "$d/t" "$d/t.c"
"$d/t"
