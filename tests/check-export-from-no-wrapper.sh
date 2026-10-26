#!/usr/bin/env bash
# tests/check-export-from-no-wrapper.sh -- (export-from ...) must not emit a
# forwarding wrapper.
#
# The whole point of the form is that re-exporting is free: a consumer of the
# re-exporting module resolves to the DEFINING module's binding and calls its
# mangled symbol directly. A wrapper per re-exported name per hop would be a
# silent cost that nothing else would catch -- the behaviour fixture next door
# (tests/fixtures/module-export-from) prints the right numbers either way.
#
# The fixture chains chain/low -> chain/mid -> chain/hi -> input.tur, so
# `low-add` is re-exported twice. Assert that the emitted C holds exactly one
# definition of it, named for the module that DEFINED it, and no chain__mid__
# or chain__hi__ copy.
set -euo pipefail
cd "$(dirname "$0")/.."

TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || { echo "export-from-no-wrapper: $TUR not built" >&2; exit 2; }

FIX="tests/fixtures/module-export-from"
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"

c=$("$TUR" emit-c "$FIX/input.tur" -I "$FIX" 2>/dev/null)
[ -n "$c" ] || { echo "FAIL export-from-no-wrapper: emit-c produced no output"; exit 1; }

status=0

# One definition, under the defining module's mangled prefix.
defs=$(printf '%s\n' "$c" | grep -cE '^static int64_t chain__low__low_hyadd\(int64_t [a-z]+, int64_t [a-z]+\) \{' || true)
if [ "$defs" -ne 1 ]; then
    echo "FAIL export-from-no-wrapper: expected exactly 1 definition of chain__low__low_hyadd, found $defs"
    status=1
fi

# No copy under either re-exporting module's prefix, for the defn or the macro.
for sym in chain__mid__low_hyadd chain__hi__low_hyadd chain__mid__twice chain__hi__twice; do
    hits=$(printf '%s\n' "$c" | grep -n "$sym" || true)
    if [ -n "$hits" ]; then
        echo "FAIL export-from-no-wrapper: forwarding wrapper '$sym' was emitted:"
        printf '%s\n' "$hits" | sed 's/^/    /'
        status=1
    fi
done

# And the call site really does reach the original.
#
# Substring-match in bash rather than `printf ... | grep -q`. Under the
# `set -o pipefail` above, `grep -q` exits at its FIRST match and SIGPIPEs the
# still-writing `printf`, so the pipeline reports 141 -- a failure -- exactly
# when the pattern IS present. `$c` is a whole emitted C file, so the writer
# is never done first and the `!` then turns a match into a false FAIL. That
# is what "call site does not dispatch to chain__low__low_hyadd" meant in CI
# while the same assertion passed when run by hand on a smaller build.
# Reproduced 10/10 with a pattern sitting on line 1. Third time this repo has
# hit grep -q + pipefail; see docs/archive/pipefail-grep-q-false-failures.md.
if [[ "$c" != *'chain__low__low_hyadd(INT64_C(20), INT64_C(22))'* ]]; then
    echo "FAIL export-from-no-wrapper: call site does not dispatch to chain__low__low_hyadd"
    status=1
fi

[ "$status" -eq 0 ] && echo "PASS export-from-no-wrapper"
exit "$status"
