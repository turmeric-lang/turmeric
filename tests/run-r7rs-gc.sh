#!/usr/bin/env bash
# tests/run-r7rs-gc.sh -- the r7rs-gc collector (docs/archive/r7rs-gc-plan.md),
# on by default for a compiled `#lang r7rs` program since it graduated
# (2026-09-25); TUR_R7RS_GC=0 builds without it.
#
#   1. Every `#lang r7rs` fixture, built with the collector and run with
#      TUR_GC_TORTURE (a collection every N allocations; default 31), must still
#      print its expected.stdout and exit as expected.  A conservative
#      collector's one real failure is a MISSING ROOT -- memory it cannot see
#      holding the only pointer to an object -- and collecting that often turns
#      one into a crash or a wrong answer instead of a rare heisenbug.
#   2. The runtime archive's blocks: Scheme values kept only in a Turmeric
#      persistent map (a HAMT libturt_runtime.a allocates) survive a
#      collection on EVERY allocation.  Before the archive allocated through
#      the collector's hook (src/runtime/rt_alloc.h) this segfaulted: the
#      nodes were libc's, unscanned, and the values were freed under them.
#   3. Threads (docs/archive/r7rs-gc-threads-plan.md, stages A to C): a
#      program that starts threads runs them in parallel under the
#      collector, which stops them by signal to collect: a bare thread
#      start, the r7rs-threads-* fixtures and r7rs-srfi-18 under frequent
#      collections (every allocation, or every 31st for the long ones),
#      plus a lint over the release points.
#   1b. Every compiled `#lang saffron` fixture the same way: since
#      2026-09-28 the collector is a Saffron program's allocator too
#      (any-widen-stored-in-an-adt-field-has-no-owner; TUR_SAFFRON_GC=0 opts
#      out), so the same missing-root hazard applies to its `any` boxes.
#   4. Reclamation (Linux only, where `ulimit -v` binds): a loop that builds
#      and drops a million small lists runs under a 256 MiB address-space
#      limit.  With the collector it fits; the same program without it
#      (429 MB peak, docs/reported/r7rs-heap-data-never-reclaimed.md) must
#      NOT fit, or the check proves nothing and fails.  A million guards and
#      call/cc escapes must fit too (reclaim-escapes; 397 MB before nested
#      CPS entries dropped their reap registrations).
#   5. libturi's data is not a root (Linux): a `(scheme eval)` program's
#      libturi data and bss sit in tur_turi_{data,bss}, bracketed by the
#      linker, and the collector skips them.
#
# Linux/glibc and macOS (the collector is; elsewhere it is plain malloc).
#   R7RS_GC_TORTURE=N   the torture interval (default 31; 1 collects on EVERY
#                       allocation, the deep run to make before touching the
#                       collector or its roots).
#   R7RS_GC_TORTURE_SCALE=R
#                       passed on as TUR_GC_TORTURE_SCALE to every run at an
#                       interval above 1 (default 64; 0 keeps the interval
#                       fixed): a program holding a large live heap collects
#                       every live-objects / R allocations instead of every N,
#                       so it is not re-marked tens of thousands of times
#                       (docs/archive/r7rs-gc-torture-quadratic-in-live-heap.md).
#                       Small heaps -- start-up, the prelude, most fixtures --
#                       keep the N interval.  The every-allocation cases (seam,
#                       threads-run and the fixture cases at 1) are never
#                       scaled, and neither is R7RS_GC_TORTURE=1.

set -uo pipefail
cd "$(dirname "$0")/.."

TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || { echo "r7rs-gc: $TUR not built" >&2; exit 2; }
HOST="$(uname -s)"
if [ "$HOST" != "Linux" ] && [ "$HOST" != "Darwin" ]; then
    echo "r7rs-gc: SKIP (host is $HOST)"
    exit 0
fi
TUR="$(cd "$(dirname "$TUR")" && pwd)/$(basename "$TUR")"
TORTURE="${R7RS_GC_TORTURE:-31}"
SCALE="${R7RS_GC_TORTURE_SCALE:-64}"
[ "$TORTURE" = 1 ] && SCALE=0

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

fixtures=()
for d in tests/fixtures/*/; do
    d="${d%/}"
    [ -f "$d/input.tur" ] && [ -f "$d/expected.stdout" ] || continue
    case "$(basename "$d")" in
        r7rs-*) fixtures+=("$d") ;;
        *) IFS= read -r first < "$d/input.tur"
           [[ "$first" == "#lang r7rs"* ]] && fixtures+=("$d")
           # 1b: compiled Saffron fixtures (an interp-only one is not built).
           [[ "$first" == "#lang saffron"* ]] && [ ! -f "$d/requires.interp-only" ] \
               && fixtures+=("$d") ;;
    esac
done

# A hang in a threaded case is a deadlock or a cycle walk, and the two look
# nothing alike on the stack (docs/archive/r7rs-gc-threads-lifecycle-rare-hang.md
# "If it recurs").  On CI the process is gone by the time anyone looks, so a
# case that outlives its deadline has every thread's stack printed first, with
# whichever debugger the host has.
dump_stacks() {
    local pid="$1"
    if command -v gdb > /dev/null 2>&1; then
        gdb -p "$pid" -batch -ex "thread apply all bt" 2>&1 | grep -E '^(Thread|#)' | head -400
    elif command -v lldb > /dev/null 2>&1; then
        lldb -p "$pid" --batch -o "thread backtrace all" 2>&1 | head -400
    elif [ "$HOST" = Darwin ] && command -v sample > /dev/null 2>&1; then
        sample "$pid" 1 -mayDie 2>&1 | head -400
    else
        echo "(no gdb, lldb or sample here to print the stacks)"
    fi
}

# run_deadline <secs> <out> <err> <cmd...>: timeout(1)'s exit codes (124 when
# the deadline passed), but the stacks are dumped into <err> before the kill.
# Standard input is $RD_STDIN (default /dev/null).  It polls every tenth of a
# second, so a case that finishes at once is not held for a whole second:
# every fixture in section 1 goes through here.
# crash_stacks <stdin> <cmd...>: a case that DIED of a signal leaves nothing
# but its exit status -- a segfault writes no stderr -- so it is run once
# more under the debugger, with the same environment, and every thread's
# stack at the fault is printed.  (macOS r7rs-sicp-metacircular-evaluator
# and r7rs-srfi-35 exited 139 under torture on #1082 with an empty tail.)
crash_stacks() {
    local stdin="$1"; shift
    if command -v gdb > /dev/null 2>&1; then
        timeout 300 gdb -batch -ex "run < $stdin > /dev/null" -ex "thread apply all bt 30" --args "$@" 2>&1 |
            grep -E '^(Thread|#|Program|\[)' | head -200
    elif command -v lldb > /dev/null 2>&1; then
        # -k: the commands lldb runs when the process stops on a crash.
        perl -e 'alarm 300; exec @ARGV' lldb --batch -o "process launch -i $stdin -o /dev/null" \
            -k "thread backtrace all -c 30" -k "quit 1" -- "$@" 2>&1 | head -200
    else
        echo "(no gdb or lldb here to print the stacks)"
    fi
}

run_deadline() {
    local secs="$1" out="$2" err="$3" pid ticks=0
    shift 3
    "$@" < "${RD_STDIN:-/dev/null}" > "$out" 2> "$err" &
    pid=$!
    while kill -0 "$pid" 2> /dev/null; do
        if [ "$ticks" -ge $((secs * 10)) ]; then
            { echo "--- stacks at the ${secs}s deadline ---"; dump_stacks "$pid"; } >> "$err" 2>&1
            kill -9 "$pid" 2> /dev/null
            wait "$pid" 2> /dev/null
            return 124
        fi
        sleep 0.1
        ticks=$((ticks + 1))
    done
    wait "$pid"
}

one() {
    local name; name="$(basename "$1")"
    one_case "$1" > "$WORK/$name.result" 2>&1
}
one_case() {
    local dir="$1" name rc want flags args=()
    name="$(basename "$dir")"
    flags=""; [ -f "$dir/flags" ] && flags="$(cat "$dir/flags")"
    [ -f "$dir/run.args" ] && mapfile -t args < "$dir/run.args"
    local stdin=/dev/null; [ -f "$dir/input.stdin" ] && stdin="$dir/input.stdin"
    # shellcheck disable=SC2086
    if ! timeout 600 "$TUR" $flags build "$dir/input.tur" \
            -o "$WORK/$name" > "$WORK/$name.build" 2>&1; then
        echo "FAIL $name -- build failed: $(grep -m1 -i error "$WORK/$name.build" | cut -c1-160)"
        return
    fi
    # Through run_deadline, so a fixture that hangs has every thread's stack
    # in the log: on CI the process is gone by the time anyone looks, and a
    # bare "timed out" cannot tell a deadlock from a cycle walk (the macOS
    # r7rs-threads-lifecycle timeout on #956 left nothing else to go on).
    (cd "$dir" && ASAN_OPTIONS=detect_leaks=0 TUR_GC_TORTURE="$TORTURE" \
        TUR_GC_TORTURE_SCALE="$SCALE" RD_STDIN="$stdin" run_deadline 300 "$WORK/$name.out" "$WORK/$name.err" "$WORK/$name" "${args[@]}") 2> /dev/null
    rc=$?
    want=0; [ -f "$dir/expected.exit" ] && want="$(tr -d '[:space:]' < "$dir/expected.exit")"
    if [ "$rc" = 124 ]; then
        echo "FAIL $name -- timed out (>300s) under TUR_GC_TORTURE=$TORTURE TUR_GC_TORTURE_SCALE=$SCALE"
        sed -n '/^--- stacks at the/,$p' "$WORK/$name.err"
    elif { [ "$want" = nonzero ] && [ "$rc" = 0 ]; } || { [ "$want" != nonzero ] && [ "$rc" != "$want" ]; }; then
        echo "FAIL $name -- exit $rc, expected $want under TUR_GC_TORTURE=$TORTURE TUR_GC_TORTURE_SCALE=$SCALE; stderr tail:"
        # The whole tail, not one line cut at 120 columns: a panic line
        # starts with the emitted unit's path, and macOS's long $TMPDIR put
        # the message itself past the cut (saffron-class-fn-extra on #1007
        # showed only "panic at /var/folders/.../..._input_tur.c:27").
        tail -6 "$WORK/$name.err" | cut -c1-400 | sed 's/^/    /'
        if [ "$rc" -gt 128 ] && [ "$rc" != 124 ]; then
            echo "    --- stdout tail ($(wc -l < "$WORK/$name.out") of $(wc -l < "$dir/expected.stdout") expected lines) ---"
            tail -3 "$WORK/$name.out" | cut -c1-200 | sed 's/^/    /'
            echo "    --- stacks at the fault, re-run under the debugger ---"
            (cd "$dir" && ASAN_OPTIONS=detect_leaks=0 TUR_GC_TORTURE="$TORTURE" \
                TUR_GC_TORTURE_SCALE="$SCALE" crash_stacks "$stdin" "$WORK/$name" "${args[@]}") | sed 's/^/    /'
        fi
    elif ! diff -q "$WORK/$name.out" "$dir/expected.stdout" > /dev/null; then
        echo "FAIL $name -- stdout differs with the collector"
        diff "$WORK/$name.out" "$dir/expected.stdout" | head -6 | sed 's/^/    /'
    else
        echo "PASS $name"
    fi
}
export -f one one_case run_deadline dump_stacks crash_stacks
export TUR WORK TORTURE SCALE HOST

printf '%s\n' "${fixtures[@]}" | xargs -P "$(nproc)" -I{} bash -c 'one "$@"' _ {}
for d in "${fixtures[@]}"; do cat "$WORK/$(basename "$d").result"; done | tee "$WORK/results"

# 2. The runtime archive's blocks, under a collection on every allocation.
cat > "$WORK/seam.tur" <<'EOF'
#lang r7rs
(import (scheme base) (scheme write) (turmeric stdlib/map))
(define (churn i)
  (if (= i 0) 'done
      (begin (list i i i i) (churn (- i 1)))))
;; Keys are Scheme symbols: `'k` is the runtime value of the keyword `:k`,
;; and a bare `:k` in Scheme is an identifier
;; (docs/archive/r7rs-leading-colon-identifiers.md).  Turmeric's `#map{...}`
;; is not Scheme syntax (docs/archive/r7rs-turmeric-syntax-leaks.md).
(define m (map-assoc (map-assoc (map-new) 'a 1) 'k (list 1 2 3 "four" (vector 5 6))))
(churn 20000)
(define m2 (map-assoc m 's (string-append "hello" " world")))
(churn 20000)
(write (list (map-get m2 'k) (map-get m2 's) (map-count m2)))
(newline)
EOF
seam_want='((1 2 3 "four" #(5 6)) "hello world" 3)'
if ! "$TUR" build "$WORK/seam.tur" -o "$WORK/seam" > "$WORK/seam.build" 2>&1; then
    echo "FAIL seam -- build failed: $(grep -m1 -i error "$WORK/seam.build" | cut -c1-160)"
else
    seam_got="$(TUR_GC_TORTURE=1 timeout 300 "$WORK/seam" 2> "$WORK/seam.err")"; rc=$?
    if [ "$rc" != 0 ]; then
        echo "FAIL seam -- exit $rc under TUR_GC_TORTURE=1: $(tail -1 "$WORK/seam.err" | cut -c1-120)"
    elif [ "$seam_got" != "$seam_want" ]; then
        echo "FAIL seam -- values kept in a Turmeric map came back as: $seam_got"
    else
        echo "PASS seam (values kept in a Turmeric map survive a collection on every allocation)"
    fi
fi | tee -a "$WORK/results"

# 3. Threads (docs/archive/r7rs-gc-threads-plan.md, stages A to C).  A
# program that starts threads runs them in parallel under the collector,
# which stops the others by signal when it collects: the thread registry,
# the release points around every blocking call, and every thread's stack,
# registers, thread-local state, key values and allocation cache as roots.
# Each case below runs with a collection on EVERY allocation, except the two
# stage C cases, which run a collection every 31st (at every allocation they
# take minutes, and neither needs it: a lost root is lost at the first
# collection, and the fork check is about locks, not collections).
#   threads-run    a C thread started through a Turmeric module starts,
#                  joins and prints, under the collector as without it.
#   threads-share  tests/fixtures/r7rs-threads-share: a list built on the
#                  main thread crosses to a worker thread, which walks it
#                  with a Scheme procedure and hands the sum back.
#   threads-roots  tests/fixtures/r7rs-threads-roots: the worker holds the
#                  only reference to a large list on its parked stack while
#                  the main thread churns garbage through thousands of
#                  collections.
#   threads-tls    tests/fixtures/r7rs-threads-tls: two threads each read
#                  their own thread-local runtime state.
#   threads-parallel, threads-pause, threads-syscall (stage B, the
#                  stop-the-world collector): tests/fixtures/r7rs-threads-*:
#                  two threads rendezvous by spinning with no release point
#                  between them; a thread in a tight allocation loop is
#                  stopped by the other's collections thousands of times; a
#                  thread blocked in a read the collector does not wrap is
#                  stopped and resumed across hundreds of collections.
#   threads-stress, threads-lifecycle (stage C, the heap under contention):
#                  tests/fixtures/r7rs-threads-*: eight threads assoc and
#                  dissoc Scheme values in one shared Turmeric persistent map
#                  through a mutex while a ninth churns garbage and large
#                  objects, and every value is intact after; a value kept
#                  only under a pthread key survives, on the main thread and
#                  a worker; a child forked while another thread allocates
#                  flat out can allocate; detached threads leave nothing in
#                  the registry.
#   threads-dynenv, threads-fiber-dynenv: tests/fixtures/r7rs-threads-
#                  dynamic-env and -fiber-dynamic-env: the Scheme dynamic
#                  environment (handlers, wind frames, parameter bindings) is
#                  per thread and per fiber, kept in a thread-local the
#                  collector scans and a fiber's block (a collection every
#                  31st allocation, as for stage C).
#   threads-srfi18 tests/fixtures/r7rs-srfi-18: SRFI 18's threads, mutexes
#                  and condition variables (stdlib/srfi/18.scm over
#                  stdlib/r7rs/thread.tur), eight threads on one mutex
#                  allocating between turns (every 31st allocation).
#   threads-lint   every blocking libc call the stdlib and the emitter
#                  spell is one the collector's release-point macros route
#                  (src/runtime/r7gc.c); a new one that is not would be a
#                  deadlock the day a program blocked there.
cat > "$WORK/spawner.tur" <<'EOF'
(defmodule spawner
  (export spawn-one)
  (defn worker [arg : ptr<void>] : ptr<void>
    ```c
    return arg;
    ```)
  (defn spawn-raw [f : ptr<void>] : ptr<void>
    ```c
    pthread_t *t = (pthread_t *)malloc(sizeof(pthread_t));
    if (pthread_create(t, NULL, (void *(*)(void *))f, NULL) != 0) { free(t); return NULL; }
    return (void *)t;
    ```)
  (defn join-raw [t : ptr<void>] : int
    ```c
    if (!t) return 0;
    pthread_join(*(pthread_t *)t, NULL);
    free(t);
    return 1;
    ```)
  (defn spawn-one [] : int
    (join-raw (spawn-raw worker))))
EOF
cat > "$WORK/threaded.tur" <<'EOF'
#lang r7rs
(import (scheme base) (scheme write) (turmeric spawner))
(display (spawn-one)) (newline)
EOF
thread_case() {
    local tag="$1"; shift
    if ! (cd "$WORK" && env "$@" "$TUR" build threaded.tur -o "threaded-$tag") > "$WORK/threaded-$tag.build" 2>&1; then
        echo "build-failed"; return
    fi
    TUR_GC_TORTURE=1 timeout 300 "$WORK/threaded-$tag" > "$WORK/threaded-$tag.out" 2> "$WORK/threaded-$tag.err"
    echo "$?"
}
plain_rc="$(thread_case plain TUR_R7RS_GC=0)"
gc_rc="$(thread_case gc TUR_R7RS_GC=1)"
if [ "$plain_rc" != 0 ] || [ "$(cat "$WORK/threaded-plain.out" 2>/dev/null)" != 1 ]; then
    echo "FAIL threads-run -- under TUR_R7RS_GC=0 the program should start and join a thread (exit $plain_rc)"
elif [ "$gc_rc" != 0 ] || [ "$(cat "$WORK/threaded-gc.out" 2>/dev/null)" != 1 ]; then
    echo "FAIL threads-run -- under the collector the program should start and join a thread (exit $gc_rc): $(tail -1 "$WORK/threaded-gc.err" | cut -c1-120)"
elif grep -q "r7rs-gc" "$WORK/threaded-gc.err"; then
    echo "FAIL threads-run -- the collector had something to say about a thread start: $(grep -m1 r7rs-gc "$WORK/threaded-gc.err" | cut -c1-120)"
else
    echo "PASS threads-run (a thread starts, joins and prints under the collector, silently, as without it)"
fi | tee -a "$WORK/results"

fixture_case() {
    local tag="$1" dir="tests/fixtures/$2" torture="${4:-1}" scale=0 want got rc
    [ "$torture" != 1 ] && scale="$SCALE"
    if ! "$TUR" build "$dir/input.tur" -o "$WORK/$tag" > "$WORK/$tag.build" 2>&1; then
        echo "FAIL $tag -- build failed: $(grep -m1 -i error "$WORK/$tag.build" | cut -c1-160)"
        return
    fi
    TUR_GC_TORTURE="$torture" TUR_GC_TORTURE_SCALE="$scale" run_deadline 300 "$WORK/$tag.out" "$WORK/$tag.err" "$WORK/$tag"; rc=$?
    got="$(cat "$WORK/$tag.out")"
    want="$(cat "$dir/expected.stdout")"
    if [ "$rc" = 124 ]; then
        echo "FAIL $tag -- timed out (>300s) under TUR_GC_TORTURE=$torture TUR_GC_TORTURE_SCALE=$scale (a missing root can read as a hang: a freed list walked in a cycle)"
        sed -n '/^--- stacks at the/,$p' "$WORK/$tag.err"
    elif [ "$rc" != 0 ]; then
        echo "FAIL $tag -- exit $rc under TUR_GC_TORTURE=$torture TUR_GC_TORTURE_SCALE=$scale: $(tail -1 "$WORK/$tag.err" | cut -c1-120)"
    elif [ "$got" != "$want" ]; then
        echo "FAIL $tag -- expected '$want', got '$got'"
    else
        echo "PASS $tag ($3)"
    fi
}
fixture_case threads-share r7rs-threads-share "a list crosses to a worker thread and its sum comes back, under a collection on every allocation" | tee -a "$WORK/results"
fixture_case threads-roots r7rs-threads-roots "a list held only on a parked thread's stack survives the main thread's churn" | tee -a "$WORK/results"
fixture_case threads-tls   r7rs-threads-tls   "each thread reads its own thread-local runtime state" | tee -a "$WORK/results"
fixture_case threads-parallel r7rs-threads-parallel "two threads rendezvous by spinning, with no release point between them: they run at the same time" | tee -a "$WORK/results"
fixture_case threads-pause r7rs-threads-pause "a thread allocating in a tight loop is stopped by the other thread's collections, thousands of times" | tee -a "$WORK/results"
fixture_case threads-syscall r7rs-threads-syscall "a thread blocked in an unwrapped read is stopped and resumed across hundreds of collections, and the read completes" | tee -a "$WORK/results"
fixture_case threads-stress r7rs-threads-stress "nine threads on the heap at once -- eight on one shared persistent map, one churning -- and every value intact" 31 | tee -a "$WORK/results"
fixture_case threads-lifecycle r7rs-threads-lifecycle "key values are roots, a fork mid-allocation is safe, detached threads leave the registry" 31 | tee -a "$WORK/results"
fixture_case threads-dynenv r7rs-threads-dynamic-env "five threads each see only their own handlers, wind frames and parameter bindings, and the collector sees all of them" 31 | tee -a "$WORK/results"
fixture_case threads-fiber-dynenv r7rs-threads-fiber-dynamic-env "a fiber's handlers and parameter bindings move with it from thread to thread" 31 | tee -a "$WORK/results"
fixture_case threads-srfi18 r7rs-srfi-18 "SRFI 18: threads, mutexes, condition variables and eight threads on one mutex, every value intact" 31 | tee -a "$WORK/results"

# threads-lint: the blocking calls (a broad list; the stdio reads are left
# out on purpose -- a read from a FILE holds the world, docs/guides/r7rs-guide.md).
lint_missing=""
for n in pthread_join pthread_cond_wait pthread_cond_timedwait pthread_mutex_lock \
         pthread_barrier_wait pthread_exit sem_wait sem_timedwait nanosleep usleep sleep \
         poll ppoll select pselect epoll_wait epoll_pwait kevent accept accept4 connect \
         recv recvfrom recvmsg read readv pread waitpid wait waitid sigwait pause \
         flock msgrcv mq_receive; do
    # a call as written: the name, an open paren, not a struct member (`->name(`)
    if grep -rqE "(^|[^A-Za-z0-9_>.])$n\(" stdlib src/compiler/emit_module.c src/compiler/emit_dk_runtime.c \
            src/compiler/emit_expr.c src/compiler/emit_fns.c src/compiler/emit_cps_ir.c src/compiler/emit_core.c \
            --include='*.tur' --include='*.c' 2>/dev/null; then
        grep -qE "^#define $n\(" src/runtime/r7gc.c || lint_missing="$lint_missing $n"
    fi
done
if [ -n "$lint_missing" ]; then
    echo "FAIL threads-lint -- blocking call(s) spelled in the stdlib or the emitter with no release point in src/runtime/r7gc.c:$lint_missing"
else
    echo "PASS threads-lint (every blocking call the unit spells is routed through a release point)"
fi | tee -a "$WORK/results"

# 4. Reclamation under an address-space limit.  `ulimit -v` binds nothing on
# macOS, so the check would read "fits both ways" there and say nothing.
if [ "$HOST" = "Linux" ]; then
cat > "$WORK/churn.tur" <<'EOF'
#lang r7rs
(import (scheme base) (scheme write))
(define (churn i)
  (if (= i 0) 'done
      (begin (list i i i i) (churn (- i 1)))))
(write (churn 1000000))
(newline)
EOF
reclaim() {
    local tag="$1"; shift
    if ! env "$@" "$TUR" build "$WORK/churn.tur" -o "$WORK/churn-$tag" > "$WORK/churn-$tag.build" 2>&1; then
        echo "build-failed"; return
    fi
    (ulimit -v 262144; "$WORK/churn-$tag" 2>/dev/null) 2>/dev/null || true
}
with="$(reclaim gc TUR_R7RS_GC=1)"
without="$(reclaim plain TUR_R7RS_GC=0)"
if [ "$with" != "done" ]; then
    echo "FAIL reclaim -- with the collector the churn did not fit in 256 MiB (got '$with')"
elif [ "$without" = "done" ]; then
    echo "FAIL reclaim -- the churn fits in 256 MiB WITHOUT the collector, so this check bites on nothing"
else
    echo "PASS reclaim (256 MiB: fits with the collector, not without)"
fi | tee -a "$WORK/results"
# r7rs-callcc-memory-never-freed: escapes.  A `guard`, a call/cc used as an
# escape and a re-entrant call/cc each enter CPS code, and a nested CPS
# entry's registrations stayed on the reap list until the program ended: this
# loop peaked at 397 MB before `__dk_reap_drop_to`, and runs in 10 MB with it.
cat > "$WORK/escapes.tur" <<'EOF'
#lang r7rs
(import (scheme base) (scheme write))
(define keep #f)
(define (step i)
  (+ (guard (e (#t 0)) (if (= i -1) (raise 'never) 1))
     (call/cc (lambda (k) (k 1)))
     (call/cc (lambda (k) (set! keep k) 1))))
(define (spin i acc) (if (= i 0) acc (spin (- i 1) (+ acc (step i)))))
(write (spin 1000000 0))
(newline)
EOF
esc="build-failed"
if "$TUR" build "$WORK/escapes.tur" -o "$WORK/escapes" > "$WORK/escapes.build" 2>&1; then
    esc="$( (ulimit -v 262144; "$WORK/escapes" 2>/dev/null) 2>/dev/null || true)"
fi
if [ "$esc" = "3000000" ]; then
    echo "PASS reclaim-escapes (a million guards and call/cc escapes fit in 256 MiB)"
else
    echo "FAIL reclaim-escapes -- a million guards and call/cc escapes did not fit in 256 MiB (got '$esc')"
fi | tee -a "$WORK/results"
# Saffron (any-widen-stored-in-an-adt-field-has-no-owner): the same loop in
# the dynamic dialect -- rebuild a 100-cell list of `any`, map it through a
# dynamic call and fold it, 100000 times.  Measured at 20000 iterations: 84 MB
# peak without the collector, growing linearly; 6 MB with it.
cat > "$WORK/saffron-churn.tur" <<'EOF'
#lang saffron
(defdata Lst [] (Cons [hd : any tl : any]) (Nil))
(defn build [n acc] (if (= n 0) acc (build (- n 1) (Cons n acc))))
(defn lmap [f xs] (match xs (Cons h t) (Cons (f h) (lmap f t)) (Nil) (Nil)))
(defn lsum [xs acc] (match xs (Cons h t) (lsum t (+ acc h)) (Nil) acc))
(defn add-one [x] (+ x 1))
(defn spin [i total]
  (if (= i 0)
    total
    (spin (- i 1) (+ total (lsum (lmap add-one (build 100 (Nil))) 0)))))
(defn main [] : int (println (spin 100000 0)) 0)
EOF
sreclaim() {
    local tag="$1"; shift
    if ! env "$@" "$TUR" build "$WORK/saffron-churn.tur" -o "$WORK/schurn-$tag" > "$WORK/schurn-$tag.build" 2>&1; then
        echo "build-failed"; return
    fi
    (ulimit -v 262144; "$WORK/schurn-$tag" 2>/dev/null) 2>/dev/null || true
}
swith="$(sreclaim gc TUR_SAFFRON_GC=1)"
swithout="$(sreclaim plain TUR_SAFFRON_GC=0)"
if [ "$swith" != "515000000" ]; then
    echo "FAIL reclaim-saffron -- with the collector the churn did not fit in 256 MiB (got '$swith')"
elif [ "$swithout" = "515000000" ]; then
    echo "FAIL reclaim-saffron -- the churn fits in 256 MiB WITHOUT the collector, so this check bites on nothing"
else
    echo "PASS reclaim-saffron (256 MiB: fits with the collector, not without)"
fi | tee -a "$WORK/results"
else
    echo "PASS reclaim (skipped on $HOST: no address-space limit to test under)" | tee -a "$WORK/results"
fi

# 5. libturi's data is not a root (Linux): a `(scheme eval)` program links
#    libturi, whose data and bss -- ~46 MB in Debug, and nothing in them
#    points into the heap -- the collector scanned on every collection
#    (docs/archive/r7rs-gc-eval-programs-scan-libturi-data.md).  The build
#    moves them into tur_turi_{data,bss}; the linker defines the bracketing
#    symbols only for a program whose collector references them, so their
#    presence (and the span between them) pins both halves.
if [ "$HOST" = "Linux" ] && command -v nm > /dev/null 2>&1; then
    skipped=""
    if "$TUR" build tests/fixtures/r7rs-eval/input.tur -o "$WORK/libturi-skip" > "$WORK/libturi-skip.build" 2>&1; then
        declare -A brk=()
        while read -r addr _ sym; do
            case "$sym" in
                __start_tur_turi_data|__stop_tur_turi_data|__start_tur_turi_bss|__stop_tur_turi_bss)
                    brk[$sym]=$((16#$addr)) ;;
            esac
        done < <(nm "$WORK/libturi-skip")
        if [ "${#brk[@]}" -ne 4 ]; then
            skipped="missing"
        else
            skipped=$(( brk[__stop_tur_turi_data] - brk[__start_tur_turi_data]
                      + brk[__stop_tur_turi_bss]  - brk[__start_tur_turi_bss] ))
        fi
    fi
    if [ -z "$skipped" ] || [ "$skipped" = "missing" ]; then
        echo "FAIL libturi-not-a-root -- r7rs-eval has no tur_turi_{data,bss} brackets (got '${skipped:-build failed}')"
    elif [ "$skipped" -lt 1048576 ]; then
        echo "FAIL libturi-not-a-root -- the skipped span is only $skipped bytes"
    else
        echo "PASS libturi-not-a-root ($((skipped / 1048576)) MiB of libturi data and bss skipped)"
    fi | tee -a "$WORK/results"
fi

pass=$(grep -c '^PASS' "$WORK/results")
fail=$(grep -c '^FAIL' "$WORK/results")
echo
if [ "$SCALE" = 0 ]; then
    echo "r7rs-gc: $pass passed, $fail failed (torture every $TORTURE allocations)"
else
    echo "r7rs-gc: $pass passed, $fail failed (torture every $TORTURE allocations, stretched to live objects / $SCALE on a large heap; every allocation where a case says so)"
fi
[ "$fail" -eq 0 ]
