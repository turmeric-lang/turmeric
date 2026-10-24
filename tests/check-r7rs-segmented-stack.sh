#!/usr/bin/env bash
# tests/check-r7rs-segmented-stack.sh --
# docs/upcoming/r7rs-heap-frames-plan.md (option C): under
# --enable=r7rs-segmented-stack a compiled `#lang r7rs` program's recursion
# goes on past the thread's own stack onto segments, bounded by the
# TUR_MAIN_STACK_MB budget instead.  A fixture cannot set an environment
# variable, and reaching the segments on the default 1 GiB stack would take
# gigabytes, so this shrinks the deep-stack thread's own stack with
# TUR_SEG_BASE_MB=8 (other threads' own stacks are 8 MiB already) and runs:
#
#   1. a recursion 1e6 deep (~30 MiB of frames), which the same 8 MiB stack
#      alone could not hold -- checked by running it again without the flag;
#   2. a recursion that allocates in every frame, under collector torture, so
#      collections run while frames live on several segments (H2);
#   3. call/cc (H3): an escape from deep on a segment, a re-entry from deep on
#      a segment into a continuation captured shallow, a raise from deep caught
#      shallow, then deep again (stale segments are dropped); a continuation
#      captured ON a segment, taken as an escape and then invoked after its
#      extent, which re-enters it; and one captured at the bottom of a
#      recursion whose every frame holds a list, re-entered eleven times --
#      from the top, from deep in another recursion that reuses its segments,
#      alternately -- plainly and under collector torture;
#   4. threads (H4): four threads at once, each 1e6 deep past its own 8 MiB
#      stack, building lists collections must keep, then a continuation
#      captured deep on a worker and re-entered there -- plainly and under
#      collector torture -- and the same program without the flag, which
#      must not survive (so the run above is a test);
#   5. fibers (H4): a fiber's recursion yields at its bottom, and the thread
#      that resumed it then recurses 1e6 deep onto segments (a fiber used to
#      disarm the thread's check for the rest of the run); and a runaway on a
#      fiber's 1 MiB stack, which stops with the overflow message (a fiber's
#      stack does not grow);
#   6. TUR_MAIN_STACK_MB=16, runaway: the overflow message and a non-zero exit
#      once the budget is spent (H5);
#   7. the hatch, TUR_R7RS_SEGMENTED_STACK=0: in the compiler's environment the
#      C is byte-identical to a build without the flag; at run time no thread
#      arms (the four 1e6-deep threads die again).
#
# Linux x86-64 and arm64: elsewhere the flag compiles to no check at all.
#
# TUR_SEG_CROSS=aarch64 runs the same programs as arm64 binaries on an x86-64
# host: built monolithically with the runtime from source (--no-split-build
# --runtime=source) by aarch64-linux-gnu-gcc, static, and run under
# qemu-aarch64-static (both from the Ubuntu packages gcc-aarch64-linux-gnu and
# qemu-user-static).  It skips when either is missing; the torture runs are
# smaller there, qemu being slower.
set -uo pipefail
cd "$(dirname "$0")/.."
TUR_REL="${TUR:-./build/tur}"
TUR="$(cd "$(dirname "$TUR_REL")" && pwd)/$(basename "$TUR_REL")"
NAME=check-r7rs-segmented-stack
if [ ! -x "$TUR" ]; then
    echo "$NAME: $TUR not built" >&2
    exit 2
fi
RUNNER=()
BUILD_ENV=()
BUILD_FLAGS=()
SCALE=1
case "${TUR_SEG_CROSS:-}" in
    "") case "$(uname -s)/$(uname -m)" in
            Linux/x86_64|Linux/aarch64) ;;
            *) echo "SKIP $NAME: Linux x86-64 and arm64 only"; exit 0 ;;
        esac ;;
    aarch64)
        NAME="$NAME (aarch64 under qemu)"
        if ! command -v aarch64-linux-gnu-gcc > /dev/null || ! command -v qemu-aarch64-static > /dev/null; then
            echo "SKIP $NAME: needs aarch64-linux-gnu-gcc and qemu-aarch64-static"; exit 0
        fi
        RUNNER=(qemu-aarch64-static)
        BUILD_ENV=(CC=aarch64-linux-gnu-gcc "TUR_CC_FLAGS=-O2 -static -fwrapv")
        BUILD_FLAGS=(--no-split-build --runtime=source)
        SCALE=4 ;;
    *) echo "$NAME: TUR_SEG_CROSS=$TUR_SEG_CROSS: only aarch64" >&2; exit 2 ;;
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
;; A continuation captured while its frames are on a segment: an escape, and
;; invoked after its extent, a re-entry.
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
cat > "$TMP/reenter.scm" <<'EOF'
#lang r7rs
(import (scheme base) (scheme write) (scheme process-context))
(define (arg) (string->number (cadr (command-line))))
;; A continuation captured at the bottom of a recursion whose frames are on
;; segments, each frame holding a fresh list across the call: re-entered from
;; the top (the thread's own stack), from deep in another recursion (on
;; segments the first one's may now be reused by), and many times over.
(define saved #f)
(define (deep n)
  (if (= n 0)
      (call/cc (lambda (k) (set! saved k) 0))
      (let ((cell (list n)))
        (let ((r (deep (- n 1))))
          (+ r (car cell))))))
(define (jump-from n v) (if (= n 0) (saved v) (+ 1 (jump-from (- n 1) v))))
(define results '())
(define (go)
  (let ((v (deep (arg))))
    (set! results (cons v results))
    (let ((i (length results)))
      (cond ((< i 3) (saved i))
            ((< i 6) (jump-from (* i 100000) i))
            ((< i 12) (if (even? i) (saved i) (jump-from (* i 50000) i)))
            (else (reverse results))))))
(define s (let ((n (arg))) (quotient (* n (+ n 1)) 2)))
(write (map (lambda (r) (- r s)) (go)))
(newline)
EOF
cat > "$TMP/threads.scm" <<'EOF'
#lang r7rs
(import (scheme base) (scheme write) (scheme process-context) (srfi 18))
(define (arg) (string->number (cadr (command-line))))
;; Every thread the program starts has its own chain.  Four threads at once,
;; each recursing (arg) deep -- past the 8 MiB a thread's own stack is -- and
;; building a list half as deep that collections must keep, then a
;; continuation captured deep on a worker's segments and re-entered there.
(define (down n) (if (= n 0) 0 (+ 1 (down (- n 1)))))
(define (build n)
  (if (= n 0)
      '()
      (let ((p (list n n)))
        (let ((rest (build (- n 1))))
          (cons (+ (car p) (cadr p)) rest)))))
(define (sum l acc) (if (null? l) acc (sum (cdr l) (+ acc (car l)))))
(define (work) (+ (down (arg)) (sum (build (quotient (arg) 2)) 0)))
(define ts (map (lambda (i) (thread-start! (make-thread work))) '(1 2 3 4)))
(write (map thread-join! ts))
(newline)
(define (reenter-here)
  (let ((saved #f) (log '()))
    (define (deep n) (if (= n 0) (call/cc (lambda (k) (set! saved k) 0)) (+ 1 (deep (- n 1)))))
    (let ((v (deep (arg))))
      (set! log (cons v log))
      (if (< (length log) 3) (saved (length log)) (reverse log)))))
(write (thread-join! (thread-start! (make-thread reenter-here))))
(newline)
EOF
cat > "$TMP/fibstep.tur" <<'EOF'
;; fibstep -- a fiber's yield, for Scheme code running in one.
(defmodule fibstep
  (export fib-yield!)
  (defn fib-yield! [] : nil
    ```c
    tur_fiber_block_yield(0);
    ```))
EOF
cat > "$TMP/fibwork.scm" <<'EOF'
#lang r7rs
;; A recursion on the fiber's own stack that yields at its bottom, twice.
(define-library (fibwork)
  (export fib-part fib-depth-set! fib-result)
  (import (scheme base) (turmeric fibstep))
  (begin
    (define depth 0)
    (define result 0)
    (define (fib-depth-set! d) (set! depth d))
    (define (fib-result) result)
    (define (walk n) (if (= n 0) (begin (fib-yield!) 0) (+ 1 (walk (- n 1)))))
    (define (fib-part) (set! result (+ (walk depth) (walk depth))))))
EOF
cat > "$TMP/fibcrew.tur" <<'EOF'
;; fibcrew -- one fiber with a 1 MiB stack, resumed by the calling thread
;; until it is done; the number of resumes.
(defmodule fibcrew
  (import fibwork :refer [fib-part])
  (export fib-run)
  (defn fib-body [] : nil
    (fib-part))
  (defn fib-drive [body : ptr<void>] : int
    ```c
    FiberBlock *f = tur_fiber_block_new((void (*)(void))body, 1024 * 1024);
    int64_t n = 0;
    while (!f->done) { tur_fiber_block_resume(f, 0); n++; }
    tur_fiber_block_free(f);
    return n;
    ```)
  (defn fib-run [] : int
    (fib-drive fib-body)))
EOF
cat > "$TMP/fiber.scm" <<'EOF'
#lang r7rs
;; A fiber's stack is checked against its own bounds while it runs, and the
;; thread's check is back when it switches out -- so the thread's recursion
;; afterwards still grows onto segments.
(import (scheme base) (scheme write) (scheme process-context) (fibwork) (turmeric fibcrew))
(define (down n) (if (= n 0) 0 (+ 1 (down (- n 1)))))
(fib-depth-set! (string->number (cadr (command-line))))
(write (list 'resumes (fib-run) 'result (fib-result)))
(newline)
(write (down (string->number (caddr (command-line)))))
(newline)
EOF

build() {   # build <prog> <out> [tur flags...]
    local p="$1" o="$2"; shift 2
    if ! env "${BUILD_ENV[@]}" "$TUR" "$@" build "${BUILD_FLAGS[@]}" "$TMP/$p.scm" -o "$TMP/$o" > "$TMP/build.log" 2>&1; then
        echo "FAIL $NAME: tur build $p.scm $* failed: $(grep -m1 -i error "$TMP/build.log")"
        exit 1
    fi
}
for p in down alloc callcc late reenter threads fiber; do
    build "$p" "$p" --enable=r7rs-segmented-stack
done
build down down-plain
build threads threads-plain

fail=0
check() {   # check <label> <want-rc: 0|nonzero> <want-stdout> <want-stderr-substring|-|*> <env...> -- <exe> <args...>
    # (want-stderr: - none at all, * not checked)
    local label="$1" want_rc="$2" want_out="$3" want_err="$4"; shift 4
    local envs=()
    while [ "$1" != "--" ]; do envs+=("$1"); shift; done
    shift
    local out rc
    local exe="$1"; shift
    out="$(env "${envs[@]}" timeout 600 "${RUNNER[@]}" "$TMP/$exe" "$@" 2>"$TMP/err")"; rc=$?
    if [ "$want_rc" = 0 ] && [ "$rc" != 0 ]; then
        echo "FAIL $NAME ($label): exited $rc: $(head -2 "$TMP/err")"; fail=1; return
    fi
    if [ "$want_rc" = nonzero ] && [ "$rc" = 0 ]; then
        echo "FAIL $NAME ($label): exited 0, expected a failure"; fail=1; return
    fi
    if [ "$out" != "$want_out" ]; then
        echo "FAIL $NAME ($label): stdout '$out', expected '$want_out'"; fail=1; return
    fi
    # qemu reports the target's fatal signal on stderr itself.
    if [ "$want_err" = "*" ]; then
        :
    elif [ "$want_err" = - ]; then
        if [ -s "$TMP/err" ]; then
            echo "FAIL $NAME ($label): unexpected stderr: $(head -2 "$TMP/err")"; fail=1
        fi
    elif ! grep -qF -- "$want_err" "$TMP/err"; then
        echo "FAIL $NAME ($label): stderr lacks '$want_err': $(head -2 "$TMP/err")"; fail=1
    fi
}
small=$((300000 / SCALE))
tri() { echo $(( $1 * ($1 + 1) )); }   # sum of 2k for k = 1..n

# 1-2, 6: recursion, collections, the budget.
# Without segments the same 8 MiB stack overflows: the run below is a test.
check "8 MiB stack, no flag" nonzero "" "stack overflow: recursion too deep" \
      TUR_MAIN_STACK_MB=8 -- down-plain 1000000
check "1e6 deep past an 8 MiB stack" 0 1000000 - \
      TUR_SEG_BASE_MB=8 -- down 1000000
check "collections on segments" 0 250000500000 - \
      TUR_SEG_BASE_MB=8 TUR_GC_TORTURE=2000 TUR_GC_TORTURE_SCALE=64 -- alloc 500000
check "16 MiB budget, runaway" nonzero "" "stack overflow: recursion too deep" \
      TUR_MAIN_STACK_MB=16 -- down 100000000

# 3: call/cc.
check "call/cc across segments" 0 "found
(0 1 2 3)
(caught deep)
(caught deep)" - \
      TUR_SEG_BASE_MB=8 -- callcc 1000000
check "call/cc captured on a segment, as an escape" 0 "1
1" - \
      TUR_SEG_BASE_MB=8 -- late 1000000
check "call/cc captured on a segment, invoked late (re-entry)" 0 "1
1
2" - \
      TUR_SEG_BASE_MB=8 -- late 1000000 late
check "re-entry into a chain of segments, eleven times" 0 "(0 1 2 3 4 5 6 7 8 9 10 11)" - \
      TUR_SEG_BASE_MB=8 -- reenter 1000000
check "re-entry into a chain of segments, under torture" 0 "(0 1 2 3 4 5 6 7 8 9 10 11)" - \
      TUR_SEG_BASE_MB=8 TUR_GC_TORTURE=2000 TUR_GC_TORTURE_SCALE=64 -- reenter "$small"

# 4: threads.
check "four threads 1e6 deep, a re-entry on a worker" 0 "($(( $(tri 500000) + 1000000 )) $(( $(tri 500000) + 1000000 )) $(( $(tri 500000) + 1000000 )) $(( $(tri 500000) + 1000000 )))
(1000000 1000001 1000002)" - \
      -- threads 1000000
h=$((small / 2))
check "four threads on segments, under torture" 0 "($(( $(tri $h) + small )) $(( $(tri $h) + small )) $(( $(tri $h) + small )) $(( $(tri $h) + small )))
($small $((small + 1)) $((small + 2)))" - \
      TUR_GC_TORTURE=2000 TUR_GC_TORTURE_SCALE=64 -- threads "$small"
check "four threads 1e6 deep, no flag" nonzero "" "*" \
      -- threads-plain 1000000

# 5: fibers.
check "a fiber's recursion, then the thread's onto segments" 0 "(resumes 3 result 4000)
1000000" - \
      TUR_SEG_BASE_MB=8 -- fiber 2000 1000000
check "a runaway on a fiber's stack" nonzero "" "stack overflow: recursion too deep" \
      TUR_SEG_BASE_MB=8 -- fiber 1000000 0

# 7: the hatch.
check "TUR_R7RS_SEGMENTED_STACK=0 at run time: no thread arms" nonzero "" "*" \
      TUR_R7RS_SEGMENTED_STACK=0 -- threads 1000000
"$TUR" emit-c "$TMP/reenter.scm" > "$TMP/plain.c" 2> /dev/null
TUR_R7RS_SEGMENTED_STACK=0 "$TUR" --enable=r7rs-segmented-stack emit-c "$TMP/reenter.scm" > "$TMP/hatch.c" 2> /dev/null
"$TUR" --enable=r7rs-segmented-stack emit-c "$TMP/reenter.scm" > "$TMP/on.c" 2> /dev/null
if ! cmp -s "$TMP/plain.c" "$TMP/hatch.c"; then
    echo "FAIL $NAME (TUR_R7RS_SEGMENTED_STACK=0 to the compiler): the C differs from a build without the flag"; fail=1
elif ! grep -q "TUR_SEG_CHECK(" "$TMP/on.c" || grep -q "TUR_SEG_CHECK(" "$TMP/hatch.c"; then
    echo "FAIL $NAME (TUR_R7RS_SEGMENTED_STACK=0 to the compiler): the check is not where it should be"; fail=1
fi

[ "$fail" = 0 ] || exit 1
echo "PASS $NAME: recursion, collections, call/cc re-entry, threads and fibers run across segments; the budget stops a runaway; the hatch turns it all off"
