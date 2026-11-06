#!/usr/bin/env bash
# tests/check-sanitize-programs.sh -- spice-suites-without-libturi-run-unsanitized:
# TUR_SANITIZE_PROGRAMS=1 builds every program with -fsanitize=address,undefined,
# whether or not it links a sanitized libturi.  A program that leaks 40 B in
# inline C (and links no libturi) must report the leak under the knob, and
# build and run silent without it.  Skips (exit 0) when the C compiler cannot
# link ASan at all.
set -uo pipefail
cd "$(dirname "$0")/.."
TUR_REL="${TUR:-./build/tur}"
TUR="$(cd "$(dirname "$TUR_REL")" && pwd)/$(basename "$TUR_REL")"
[ -x "$TUR" ] || { echo "check-sanitize-programs: $TUR not built" >&2; exit 2; }
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
CC_BIN="${CC:-cc}"

printf 'int main(void){return 0;}\n' > "$WORK/probe.c"
if ! "$CC_BIN" -fsanitize=address,undefined "$WORK/probe.c" -o "$WORK/probe" >/dev/null 2>&1; then
    echo "SKIP check-sanitize-programs: $CC_BIN cannot link -fsanitize=address"
    exit 0
fi

cat > "$WORK/leaky.tur" <<'TUR'
(defn leak [] : int
  ```c
  void * volatile p = malloc(40);
  p = 0;
  return 1;
  ```)
(defn main [] : int (println (leak)) 0)
TUR

fail=0
if ! "$TUR" build "$WORK/leaky.tur" -o "$WORK/plain" > "$WORK/plain.build" 2>&1; then
    echo "FAIL check-sanitize-programs: plain build failed"; cat "$WORK/plain.build"; exit 1
fi
ASAN_OPTIONS=detect_leaks=1 "$WORK/plain" > "$WORK/plain.out" 2> "$WORK/plain.err"
if grep -q "LeakSanitizer\|AddressSanitizer" "$WORK/plain.err"; then
    echo "FAIL check-sanitize-programs: the plain build is sanitized"; fail=1
fi

if ! TUR_SANITIZE_PROGRAMS=1 "$TUR" build "$WORK/leaky.tur" -o "$WORK/san" > "$WORK/san.build" 2>&1; then
    echo "FAIL check-sanitize-programs: TUR_SANITIZE_PROGRAMS=1 build failed"; cat "$WORK/san.build"; exit 1
fi
ASAN_OPTIONS=detect_leaks=1 "$WORK/san" > "$WORK/san.out" 2> "$WORK/san.err"
if ! grep -q "40 byte(s) leaked in 1 allocation" "$WORK/san.err"; then
    echo "FAIL check-sanitize-programs: TUR_SANITIZE_PROGRAMS=1 did not catch the leak"
    head -20 "$WORK/san.err"; fail=1
fi
# LeakSanitizer _exits on a leak before stdio is flushed, so read the
# sanitized program's answer from a run with leak detection off.
ASAN_OPTIONS=detect_leaks=0 "$WORK/san" > "$WORK/san.out" 2>/dev/null
if [ "$(cat "$WORK/san.out")" != "1" ] || [ "$(cat "$WORK/plain.out")" != "1" ]; then
    echo "FAIL check-sanitize-programs: wrong output"; fail=1
fi
[ "$fail" = 0 ] && echo "PASS check-sanitize-programs"
exit "$fail"
