#!/usr/bin/env bash
# tests/check-r7rs-segmented-stack.sh --
# docs/upcoming/r7rs-heap-frames-plan.md (option C): under
# --enable=r7rs-segmented-stack a compiled `#lang r7rs` program's recursion
# goes on past the thread's own stack onto segments, bounded by the
# TUR_MAIN_STACK_MB budget instead.  A fixture cannot set an environment
# variable, and reaching the segments on the default 1 GiB stack would take
# gigabytes, so this shrinks the thread's own stack with TUR_SEG_BASE_MB=8 and
# runs:
#
#   1. a recursion 1e6 deep (~30 MiB of frames), which the same 8 MiB stack
#      alone could not hold -- checked by running it again without the flag;
#   2. a recursion that allocates in every frame, under collector torture, so
#      collections run while frames live on several segments (H2);
#   3. call/cc: an escape from deep on a segment, a re-entry from deep on a
#      segment into a continuation captured shallow, and a raise from deep
#      caught shallow, then deep again (stale segments are dropped); and a
#      continuation captured ON a segment, which is escape-only: it escapes,
#      and invoked after its extent it stops the program with the message an
#      escape-only continuation gives;
#   4. TUR_MAIN_STACK_MB=16, runaway: the overflow message and a non-zero exit
#      once the budget is spent (H5).
#
# Linux x86-64 and arm64: elsewhere the flag compiles to no check at all.
set -uo pipefail
cd "$(dirname "$0")/.."
TUR_REL="${TUR:-./build/tur}"
TUR="$(cd "$(dirname "$TUR_REL")" && pwd)/$(basename "$TUR_REL")"
NAME=check-r7rs-segmented-stack
if [ ! -x "$TUR" ]; then
    echo "$NAME: $TUR not built" >&2
    exit 2
fi
case "$(uname -s)/$(uname -m)" in
    Linux/x86_64|Linux/aarch64) ;;
    *) echo "SKIP $NAME: Linux x86-64 and arm64 only"; exit 0 ;;
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
cat > "$TMP/alloc.scm" <<'EOF'
#lang r7rs
(import (scheme base) (scheme write) (scheme process-context))
;; Each frame holds a fresh list across its recursive call.
(define (build n)
  (if (= n 0)
      '()
      (let ((p (list n n)))
        (let ((rest (build (- n 1))))
          (cons (+ (car p) (cadr p)) rest)))))
(define (sum l acc) (if (null? l) acc (sum (cdr l) (+ acc (car l)))))
(write (sum (build (string->number (cadr (command-line)))) 0))
(newline)
EOF
cat > "$TMP/callcc.scm" <<'EOF'
#lang r7rs
(import (scheme base) (scheme write) (scheme process-context))
(define (arg) (string->number (cadr (command-line))))
(define (find-deep n k) (if (= n 0) (k 'found) (+ 1 (find-deep (- n 1) k))))
(write (call/cc (lambda (k) (find-deep (arg) k))))
(newline)
(define (reenter-test depth)
  (let ((saved #f) (count 0) (log '()))
    (define (down n) (if (= n 0) (saved count) (+ 1 (down (- n 1)))))
    (let ((v (call/cc (lambda (k) (set! saved k) 0))))
      (set! count (+ count 1))
      (set! log (cons v log))
      (if (< count 4) (down depth) (reverse log)))))
(write (reenter-test (arg)))
(newline)
(define (boom n) (if (= n 0) (raise 'deep) (+ 1 (boom (- n 1)))))
(write (guard (e (#t (list 'caught e))) (boom (arg))))
(newline)
(write (guard (e (#t (list 'caught e))) (boom (arg))))
(newline)
EOF
cat > "$TMP/late.scm" <<'EOF'
#lang r7rs
(import (scheme base) (scheme write) (scheme process-context))
;; A continuation captured while its frames are on a segment: escape-only.
(define saved #f)
(define (deep n)
  (if (= n 0)
      (call/cc (lambda (k) (set! saved k) (k 1) 0))
      (+ 0 (deep (- n 1)))))
(define (test late?)
  (let ((v (deep (string->number (cadr (command-line))))))
    (write v) (newline)
    (if (and late? (= v 1)) (saved 2) 'done)))
(test #f)
(test (pair? (cddr (command-line))))
EOF

fail=0
for p in down alloc callcc late; do
    if ! "$TUR" --enable=r7rs-segmented-stack build "$TMP/$p.scm" -o "$TMP/$p" > "$TMP/build.log" 2>&1; then
        echo "FAIL $NAME: tur build $p.scm failed: $(grep -m1 -i error "$TMP/build.log")"
        exit 1
    fi
done
if ! "$TUR" build "$TMP/down.scm" -o "$TMP/down-plain" > "$TMP/build.log" 2>&1; then
    echo "FAIL $NAME: tur build down.scm (no flag) failed: $(grep -m1 -i error "$TMP/build.log")"
    exit 1
fi

check() {   # check <label> <want-rc: 0|nonzero> <want-stdout> <want-stderr-substring|-> <env...> -- <exe> <args...>
    local label="$1" want_rc="$2" want_out="$3" want_err="$4"; shift 4
    local envs=()
    while [ "$1" != "--" ]; do envs+=("$1"); shift; done
    shift
    local out rc
    local exe="$1"; shift
    out="$(env "${envs[@]}" timeout 300 "$TMP/$exe" "$@" 2>"$TMP/err")"; rc=$?
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

# Without segments the same 8 MiB stack overflows: the run below is a test.
check "8 MiB stack, no flag" nonzero "" "stack overflow: recursion too deep" \
      TUR_MAIN_STACK_MB=8 -- down-plain 1000000
check "1e6 deep past an 8 MiB stack" 0 1000000 - \
      TUR_SEG_BASE_MB=8 -- down 1000000
check "collections on segments" 0 250000500000 - \
      TUR_SEG_BASE_MB=8 TUR_GC_TORTURE=2000 TUR_GC_TORTURE_SCALE=64 -- alloc 500000
check "call/cc across segments" 0 "found
(0 1 2 3)
(caught deep)
(caught deep)" - \
      TUR_SEG_BASE_MB=8 -- callcc 1000000
check "call/cc captured on a segment, as an escape" 0 "1
1" - \
      TUR_SEG_BASE_MB=8 -- late 1000000
check "call/cc captured on a segment, invoked late" nonzero "1
1" "continuation invoked after its call/cc prompt returned" \
      TUR_SEG_BASE_MB=8 -- late 1000000 late
check "16 MiB budget, runaway" nonzero "" "stack overflow: recursion too deep" \
      TUR_MAIN_STACK_MB=16 -- down 100000000

[ "$fail" = 0 ] || exit 1
echo "PASS $NAME: recursion, collections and call/cc run across segments, and the budget stops a runaway"
