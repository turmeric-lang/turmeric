#!/usr/bin/env bash
# tests/check-jit-stack-overflow-message.sh --
# docs/archive/jit-stack-overflow-has-no-message.md: under `tur jit` a
# recursion that runs off the entry thread's stack prints `stack overflow:
# recursion too deep` -- the line a compiled `#lang r7rs` program prints --
# and still dies of the signal, in every dialect.  A fault that is not an
# overflow (a wild pointer in JIT'd code) must NOT print it.
#
# TUR_MAIN_STACK_MB=8 keeps the runaway cheap: it touches ~8 MiB, not the
# default 1 GiB.  Linux and macOS (the POSIX handler in jit_engine.c).
set -uo pipefail
cd "$(dirname "$0")/.."
TUR_REL="${TUR:-./build/tur}"
TUR="$(cd "$(dirname "$TUR_REL")" && pwd)/$(basename "$TUR_REL")"
NAME=check-jit-stack-overflow-message
if [ ! -x "$TUR" ]; then
    echo "$NAME: $TUR not built" >&2
    exit 2
fi
case "$(uname -s)" in
    Linux|Darwin) ;;
    *) echo "SKIP $NAME: Linux and macOS only"; exit 0 ;;
esac
# Same probe as tests/run-jit.sh: a non-JIT binary says so before reading the file.
probe=$("$TUR" jit /nonexistent-tur-jit-probe.tur 2>&1 || true)
case "$probe" in
    *"carries no JIT"*) echo "SKIP $NAME: $TUR carries no JIT engine"; exit 0 ;;
esac
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
MSG="stack overflow: recursion too deep"

cat > "$TMP/runaway.scm" <<'EOF'
#lang r7rs
(import (scheme base) (scheme write))
(define (down n) (+ 1 (down (+ n 1))))
(write (down 0))
(newline)
EOF
cat > "$TMP/runaway.tur" <<'EOF'
(defn down [n : int] : int (+ 1 (down (+ n 1))))
(defn main [] : int (println (down 0)) 0)
EOF
cat > "$TMP/wild.tur" <<'EOF'
(defn poke [x : int] : int
  ```c
  return *(int64_t *)(intptr_t)x;
  ```)
(defn main [] : int (println (poke 8)) 0)
EOF

fail=0
for f in runaway.scm runaway.tur; do
    # The braces take bash's own "Segmentation fault" job notice off our output.
    { TUR_MAIN_STACK_MB=8 "$TUR" jit "$TMP/$f" > "$TMP/out" 2> "$TMP/err"; } 2>/dev/null; rc=$?
    if ! grep -qF "$MSG" "$TMP/err"; then
        echo "FAIL $NAME ($f): no '$MSG' on stderr (exit $rc): $(grep -v 'warning --' "$TMP/err" | head -2)"
        fail=1
    elif [ "$rc" -le 128 ]; then
        echo "FAIL $NAME ($f): printed the message but exited $rc, not by the signal"
        fail=1
    fi
done
{ TUR_MAIN_STACK_MB=8 "$TUR" jit "$TMP/wild.tur" > "$TMP/out" 2> "$TMP/err"; } 2>/dev/null; rc=$?
if [ "$rc" = 0 ]; then
    echo "FAIL $NAME (wild.tur): a read of address 8 exited 0"
    fail=1
elif grep -qF "$MSG" "$TMP/err"; then
    echo "FAIL $NAME (wild.tur): a wild pointer was reported as a stack overflow"
    fail=1
fi

[ "$fail" = 0 ] || exit 1
echo "PASS $NAME: a runaway recursion under tur jit says so (r7rs and turmeric); a wild pointer does not"
