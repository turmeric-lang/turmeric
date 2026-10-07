#!/usr/bin/env bash
# tests/check-r7rs-main-stack-size.sh --
# docs/archive/r7rs-deep-stack-size-not-configurable.md: TUR_MAIN_STACK_MB
# sizes the stack a compiled `#lang r7rs` program's main runs on (1 GiB by
# default).  A fixture cannot set an environment variable, so this builds one
# deep recursion and runs it four ways:
#
#   1. TUR_MAIN_STACK_MB=16, shallow     -- the answer, nothing on stderr
#   2. TUR_MAIN_STACK_MB=16, runaway     -- `stack overflow: recursion too deep`
#                                           and a non-zero exit, after touching
#                                           ~16 MiB rather than 1 GiB -- which
#                                           is also the only cheap way to pin
#                                           the overflow message itself
#   3. TUR_MAIN_STACK_MB=abc             -- reported and ignored; still runs
#   4. a size no host can reserve        -- reported; main runs where it is
#
# Linux and macOS (the pthread branch of emit_deep_stack_runtime).
set -uo pipefail
cd "$(dirname "$0")/.."
TUR_REL="${TUR:-./build/tur}"
TUR="$(cd "$(dirname "$TUR_REL")" && pwd)/$(basename "$TUR_REL")"
NAME=check-r7rs-main-stack-size
if [ ! -x "$TUR" ]; then
    echo "$NAME: $TUR not built" >&2
    exit 2
fi
case "$(uname -s)" in
    Linux|Darwin) ;;
    *) echo "SKIP $NAME: Linux and macOS only"; exit 0 ;;
esac
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

cat > "$TMP/down.scm" <<'EOF'
#lang r7rs
(import (scheme base) (scheme write) (scheme process-context))
(define (down n) (if (= n 0) 0 (+ 1 (down (- n 1)))))
(write (down (string->number (cadr (command-line)))))
(newline)
EOF
if ! "$TUR" build "$TMP/down.scm" -o "$TMP/down" > "$TMP/build.log" 2>&1; then
    echo "FAIL $NAME: tur build failed: $(grep -m1 -i error "$TMP/build.log")"
    exit 1
fi

fail=0
check() {   # check <label> <want-rc: 0|nonzero> <want-stdout> <want-stderr-substring|-> <env...> -- <depth>
    local label="$1" want_rc="$2" want_out="$3" want_err="$4"; shift 4
    local envs=()
    while [ "$1" != "--" ]; do envs+=("$1"); shift; done
    shift
    local out rc
    out="$(env "${envs[@]}" "$TMP/down" "$1" 2>"$TMP/err")"; rc=$?
    if [ "$want_rc" = 0 ] && [ "$rc" != 0 ]; then
        echo "FAIL $NAME ($label): exited $rc: $(head -2 "$TMP/err")"; fail=1; return
    fi
    if [ "$want_rc" = nonzero ] && [ "$rc" = 0 ]; then
        echo "FAIL $NAME ($label): exited 0, expected a failure"; fail=1; return
    fi
    if [ "$out" != "$want_out" ]; then
        echo "FAIL $NAME ($label): stdout '$out', expected '$want_out'"; fail=1; return
    fi
    if [ "$want_err" = - ]; then
        if [ -s "$TMP/err" ]; then
            echo "FAIL $NAME ($label): unexpected stderr: $(head -2 "$TMP/err")"; fail=1
        fi
    elif ! grep -qF -- "$want_err" "$TMP/err"; then
        echo "FAIL $NAME ($label): stderr lacks '$want_err': $(head -2 "$TMP/err")"; fail=1
    fi
}

check "16 MiB, shallow" 0 10000 - TUR_MAIN_STACK_MB=16 -- 10000
check "16 MiB, runaway" nonzero "" "stack overflow: recursion too deep" \
      TUR_MAIN_STACK_MB=16 -- 100000000
check "not a number" 0 10000 "ignoring TUR_MAIN_STACK_MB=abc" \
      TUR_MAIN_STACK_MB=abc -- 10000
check "unreservable" 0 1000 "cannot make a 100000000000 MiB stack for main" \
      TUR_MAIN_STACK_MB=100000000000 -- 1000

[ "$fail" = 0 ] || exit 1
echo "PASS $NAME: TUR_MAIN_STACK_MB sizes main's stack, and a bad or unreservable size is reported"
