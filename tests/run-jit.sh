#!/usr/bin/env bash
# tests/run-jit.sh -- MIR JIT fixture runner (J3, jit-engine-plan section 5).
#
# Runs EVERY tests/fixtures/ through `tur jit` -- the in-process
# MIR engine (src/jit_engine.c) -- instead of compiling each to a native
# binary through cc.  The engine's own step-6 fallback to the cc path is a
# first-class outcome here: a fixture whose inline C c2mir rejects still runs
# and still must produce the expected output; it is tallied separately
# ("via cc fallback") as a signal, never a failure.
#
# Result semantics mirror tests/run.sh's run phase (expected.stdout diff +
# expected.exit, run.args, input.stdin, expected.timeout), NOT the sweep's
# (tools/jit-spike/sweep-turjit.sh scores any signal as FAIL; this harness
# honors expected.exit, so by-design-panic fixtures pass here).
#
# Usage:
#   bash tests/run-jit.sh                       # the full fixture set
#   TUR_TEST_FILTER='hamt' bash tests/run-jit.sh  # narrow by regex
#   TUR_TEST_SHARD=1/3 bash tests/run-jit.sh    # one third of the corpus
#
# Environment:
#   TUR            path to a TUR_JIT=ON tur (default: ./build-turjit/tur,
#                  falling back to ./build/tur).  A binary without the
#                  engine SKIPs the whole run (exit 0) so this harness is
#                  safe to invoke against any build.
#   TUR_TEST_JOBS  parallelism (default: cpu count, capped at 8)
#   TUR_HEADROOM_TOP  how many fixtures the closing "closest to their timeout
#                  budget" block lists (default 8).  Set it high to size the
#                  whole corpus (stress-fixture-tiering-plan T1).
#   TUR_TEST_SHARD "i/N" -- run only the i-th of N disjoint slices of the
#                  corpus, so N runners can share it.  Mirrors run.sh: the
#                  partition is round-robin by discovery ordinal, ordinals are
#                  assigned over the FULL corpus (so a filter never shifts
#                  shard membership), and the union of 1/N..N/N is exactly the
#                  unsharded run.  Happy and error fixtures round-robin on
#                  their own counters, so each shard holds within one fixture
#                  of an equal share of BOTH -- what makes shards equal-cost.
#   TUR_TEST_LIST  set to 1 to print the fixture names this invocation would
#                  run (after filter and shard) and exit 0 without running any
#   TUR_FORCE      set to 1 to skip the stamp-cache fast-path
#
# Markers (mirroring run-turi.sh's posture):
#   requires.cc               -- genuinely cc-only under the JIT; PASS-skip
#   requires.posix-apis       -- POSIX-only APIs; PASS-skip on Windows only,
#                                matching tests/run.sh
#   requires.interp           -- interpreter-owned fixture; PASS-skip
#   requires.dedicated-runner -- owned by its own ctest target; PASS-skip
#   requires.spices           -- skipped when the sibling checkout is absent
#   requires.tsan             -- skipped unless TUR_TSAN=1
#   requires.stress           -- skipped unless TUR_STRESS=1 (nightly-only
#                                full-size twin of a per-PR fixture)
#
# Like tests/run.sh, a fully green run needs a DEBUG-configured tur: the
# refine-* fixtures depend on Debug-only refinement discharge and fail on
# any Release binary, jit or not (the known "refine-* on a Release tur"
# class, findings 21.3). Expect green against build-turjit-debug/tur and
# exactly the refine set red against a Release build.

set -u
cd "$(dirname "$0")/.."

# Plan section 6: JIT harness runs mirror the interpreter harness posture --
# the program runs INSIDE the (possibly ASan) tur process, whose engine
# deliberately leaks its context on unwound errors and whose runtime keeps
# process-lifetime registrations, so LeakSanitizer noise is by design.  Opt
# back in with ASAN_OPTIONS=detect_leaks=1.
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"

# jit-linux-aborting-fixtures-time-out: under the JIT the process a fixture's
# `abort()` kills is the (sanitized, large) tur itself.  Where the kernel pipes
# core dumps to a handler (`core_pattern` starting with `|`, apport on stock
# Ubuntu), RLIMIT_CORE does not stop the dump -- ASan's disable_coredump sets
# it to 0, which a pipe ignores -- and a handler serializing several big cores
# at once was the suspected cause of aborting fixtures running past the
# budget in clusters.  A limit of exactly 1 is the kernel's one "do not pipe
# this dump" (fs/coredump.c), so set it, and keep ASan from resetting it to 0.
# A file core_pattern is left alone: there a limit of 1 would leave 1-byte
# cores in the fixture directories.
_core_pattern="$(cat /proc/sys/kernel/core_pattern 2>/dev/null || true)"
if [ "${_core_pattern#|}" != "$_core_pattern" ]; then
    if ulimit -c 1 2>/dev/null; then
        export ASAN_OPTIONS="$ASAN_OPTIONS:disable_coredump=0"
        echo "run-jit: core dumps piped to '${_core_pattern%% *}'; RLIMIT_CORE=1 so aborting fixtures skip them"
    fi
fi

# Force server fixtures to bind 127.0.0.1 instead of INADDR_ANY -- the same
# export tests/run.sh:72 makes, and it must match: the stdlib listen path
# (stdlib/httpd.tur:703) reads this env at RUN time, so a fixture behaves
# differently under a harness that omits it.
#
# httpd-new-pool-fail-drops-handler is the fixture that caught the omission.  It
# occupies 127.0.0.1:<port> and then asserts httpd-new-pool's bind of the same
# port is refused.  Without this export httpd binds 0.0.0.0 instead, and BSD's
# SO_REUSEADDR (both sockets set it) permits a wildcard bind while a SPECIFIC
# address holds the port -- so the bind SUCCEEDED and the fixture printed
# "built" instead of "refused".  Linux refuses that bind either way, which is
# why this only ever showed up on macOS and read as a JIT/BSD defect.  It is
# neither: it was harness drift from run.sh.
export TUR_BIND_LOOPBACK=1

TUR="${TUR:-./build-turjit/tur}"
[ -x "$TUR" ] || TUR=./build/tur

# TUR_TEST_LIST answers a question about the CORPUS -- which fixtures a filter
# and shard select -- and the answer does not depend on the compiler at all.
# So list mode skips every gate that asks something of the binary: the
# existence check here, the engine probe, and the native-execution smoke test.
# That is what lets tests/run-shard-partition.sh guard the partition in EVERY
# job rather than only the one configured -DTUR_JIT=ON -- the partition is a
# property of tests/fixtures/, which any commit can change.
#
# Kept as one named flag rather than repeating the condition at each gate:
# three separate copies is how the smoke test below got missed the first time,
# and a fourth gate would be just as easy to miss.
LIST_ONLY=0
if [ "${TUR_TEST_LIST:-0}" = "1" ]; then LIST_ONLY=1; fi

if [ "$LIST_ONLY" != "1" ]; then
    [ -x "$TUR" ] || { echo "run-jit: no tur binary found" >&2; exit 2; }
fi

# Capability probe -- capture, don't pipe into grep -q (pipefail SIGPIPE).
# Probed with a nonexistent input: P0 (engine-selection-plan) moved cmd_jit's
# input scan ahead of its gates, so a bare `tur jit` prints usage on EVERY
# build and no longer discriminates.  A non-JIT binary answers "carries no
# JIT engine" before touching the file; a JIT binary proceeds to (and fails)
# the compile.
if [ "$LIST_ONLY" != "1" ]; then
    probe=$("$TUR" jit /nonexistent-tur-jit-probe.tur 2>&1 || true)
    case "$probe" in
      *"carries no JIT"*)
         echo "run-jit: SKIP ($TUR carries no JIT engine; configure -DTUR_JIT=ON)"
         echo "TUR_SKIP: $TUR carries no JIT engine (configure -DTUR_JIT=ON)"
         exit 0 ;;
    esac
fi

# The engine's cc fallback links -lturi; anchor -L at the build tree the
# binary actually lives in, exactly as tests/run.sh does.
_tur_build_dir=$(dirname "$TUR")
export TUR_CC_FLAGS="${TUR_CC_FLAGS:--O2 -std=c99 -Wall -fno-strict-aliasing -L${_tur_build_dir}/src}"

PASS=0
FAIL=0
SKIP=0
FALLBACK=0
FAILED=()

if command -v getconf >/dev/null 2>&1; then
    _nproc="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
else
    _nproc=4
fi
JOBS="${TUR_TEST_JOBS:-$_nproc}"
case "$JOBS" in ''|*[!0-9]*) JOBS=4 ;; esac
if [ "$JOBS" -lt 1 ]; then JOBS=1; fi
if [ "$JOBS" -gt 8 ]; then JOBS=8; fi

TUR_FORCE="${TUR_FORCE:-0}"
STAMP_CACHE="tests/.stamp-cache-jit"

_tur_hash_file() {
    if command -v md5 >/dev/null 2>&1; then md5 -q "$1" 2>/dev/null
    elif command -v md5sum >/dev/null 2>&1; then md5sum "$1" 2>/dev/null | awk '{print $1}'
    else echo "nohash"; fi
}
# GNU first, and only an all-digit answer counts: `stat -f` on GNU means
# "filesystem status" and printed the volume's free-block counts into the
# stamp key, so on Linux the cache almost never hit (see tests/run.sh).
_tur_mtime() {
    local m
    m="$(stat -c '%Y' "$1" 2>/dev/null)"
    case "$m" in ''|*[!0-9]*) m="$(stat -f '%m' "$1" 2>/dev/null)" ;; esac
    case "$m" in ''|*[!0-9]*) m=0 ;; esac
    echo "$m"
}

# Stock macOS ships no `timeout(1)` -- Homebrew coreutils installs it as
# `gtimeout` unless the gnubin path is on PATH.  run.sh has detected this since
# T19; this harness called bare `timeout`, so on a Mac without GNU coreutils
# every fixture would have died in the runner rather than the compiler.  Mirror
# run.sh: prefer timeout, fall back to gtimeout, and run untimed if neither
# exists (a hung fixture then hangs the run, which is the pre-existing tradeoff
# run.sh already makes).
_tur_timeout_bin=""
if command -v timeout >/dev/null 2>&1; then _tur_timeout_bin="timeout"
elif command -v gtimeout >/dev/null 2>&1; then _tur_timeout_bin="gtimeout"; fi
_run_timed() {
    local secs="$1"; shift
    local _t0=$SECONDS _rc=0
    if [ "$secs" -le 0 ] || [ -z "$_tur_timeout_bin" ]; then "$@" || _rc=$?
    else "$_tur_timeout_bin" "$secs" "$@" || _rc=$?; fi
    # Record elapsed-vs-budget for the summary's "closest to their timeout"
    # block.  The point is to see a fixture CREEPING toward its own
    # expected.timeout while it still passes, rather than finding out when a
    # slow runner draw kills it: r7rs-tail-calls sat at ~32 s against a 60 s
    # bound and died twice (docs/reported/macos-jit-leg-stall-unexplained.md)
    # before anyone could see the margin was gone.
    #
    # `rkey` and `name` are run_jit_fixture's locals, reached by bash's
    # dynamic scoping -- this function is only ever called from there and
    # from the error pass, and `${rkey:-}` keeps it inert under `set -u`
    # anywhere else.  SECONDS has 1 s granularity, which is the right size
    # for budgets measured in tens of seconds.
    if [ "$secs" -gt 0 ] && [ -n "${rkey:-}" ] && [ -n "${RESULTS_DIR:-}" ]; then
        printf '%s %s %s\n' "$((SECONDS - _t0))" "$secs" "${name:-$rkey}" \
            >> "$RESULTS_DIR/$rkey.time"
    fi
    return "$_rc"
}

export TUR_MTIME="$(_tur_mtime "$TUR")"

# One hash over stdlib/, as tests/run.sh keys its stamps: the stdlib is data
# `tur` reads at elaboration time, so a stdlib-only edit changes neither the
# binary nor any fixture (docs/archive/run-sh-stamp-cache-ignores-the-stdlib.md).
# This key had no such term; the broken mtime above masked that on Linux.
_tur_hash_stdin() {
    if command -v md5 >/dev/null 2>&1; then md5 -q
    elif command -v md5sum >/dev/null 2>&1; then md5sum | awk '{print $1}'
    else echo "nohash"; fi
}
export TUR_STDLIB_HASH="$(find stdlib -type f 2>/dev/null | LC_ALL=C sort |
    while IFS= read -r _f; do printf '%s\n' "$_f"; cat "$_f"; done | _tur_hash_stdin)"

stamp_key() { echo "$(_tur_hash_file "$1")-${TUR_MTIME}-${TUR_STDLIB_HASH}"; }

stamp_check() {
    [ "$TUR_FORCE" = "1" ] && return 1
    local f="$STAMP_CACHE/$(printf '%s' "$1" | tr '/ ' '__')"
    [ -f "$f" ] || return 1
    [ "$(cat "$f")" = "$(stamp_key "$2")" ]
}

stamp_write() {
    mkdir -p "$STAMP_CACHE"
    echo "$(stamp_key "$2")" > "$STAMP_CACHE/$(printf '%s' "$1" | tr '/ ' '__')"
}

RESULTS_DIR="$(mktemp -d -t tur-jit-results.XXXXXX)"
trap 'rm -rf "$RESULTS_DIR"' EXIT

# jit-suite-reports-pass-when-the-engine-is-disabled: two aggregate checks,
# because per fixture the cc fallback is (rightly) a pass, and that let the
# engine go from "compiles 2700 programs" to "compiles zero" -- a GNU-only
# `__auto_type` in the emitter that c2mir rejects in every TU with an erasing
# ascription, i.e. every TU that loads the prelude -- while this harness
# printed a green summary and exited 0.
#
# (1) Smoke: ONE trivial program must go through the engine natively.  Costs
#     a second, needs no list, and catches the tree-wide case before 2700
#     fixtures spend ten minutes falling back.
# The first engine diagnostic in a stderr file, for a FAIL line.  CI filters
# this harness's console down to its FAIL lines, so a reason printed on a line
# of its own is lost there (it survives only in the uploaded ctest log).
_first_jit_error() {
    local _e
    _e="$(grep -E '^<tur-jit>:[0-9]+:[0-9]+: ' "$1" 2>/dev/null | grep -v -- ' warning --' | head -1)"
    [ -n "$_e" ] || _e="$(grep -m1 -E '^tur: jit: ' "$1" 2>/dev/null)"
    printf '%s' "$_e"
}

if [ "$LIST_ONLY" != "1" ]; then
_smoke_dir="$(mktemp -d -t tur-jit-smoke.XXXXXX)"
printf '(defn main [] : int (println 42) 0)\n' > "$_smoke_dir/smoke.tur"
_smoke_err="$_smoke_dir/smoke.stderr"
_smoke_out="$("$TUR" jit "$_smoke_dir/smoke.tur" 2> "$_smoke_err")"; _smoke_rc=$?
# TUR-W0071 counts too: a trivial program that needs the full-TU retry means
# the split or the pruner (src/compiler/jit_prune.h) is broken for EVERY
# program on this host -- name it once here rather than in 2700 FAIL lines.
if [ "$_smoke_rc" -ne 0 ] || [ "$_smoke_out" != "42" ] \
   || grep -qE 'TUR-W007[01]' "$_smoke_err" 2>/dev/null; then
    echo "FAIL run-jit -- the engine did not natively run a trivial program: $(_first_jit_error "$_smoke_err")"
    echo "     (rc=$_smoke_rc, stdout='$_smoke_out').  If stderr below carries TUR-W0070,"
    echo "     the engine is falling back TREE-WIDE (an emitter construct c2mir cannot"
    echo "     parse?) and every fixture 'pass' below would be the cc path, not the JIT."
    echo "     TUR-W0071: the reduced TU (split runtime / pruned program) is broken on"
    echo "     this host and every program is paying a second, full compile."
    sed 's/^/     stderr: /' "$_smoke_err" | head -12
    rm -rf "$_smoke_dir"
    exit 1
fi
rm -rf "$_smoke_dir"
fi

# (2) Ratchet: the fixtures ALLOWED to fall back are listed by NAME in
#     tests/jit-fallback-baseline.txt.  A fixture that falls back and is not
#     listed FAILs the run -- add it to the baseline in the same commit, with
#     a reviewer looking at why the engine lost it.  A listed fixture that no
#     longer falls back is reported as reclaimed (tighten the baseline; not a
#     failure, so an engine improvement never turns the suite red).  Names,
#     not a count: a count rots into a rubber-stamp as fixtures come and go.
#     Regenerate deliberately: TUR_JIT_FALLBACK_UPDATE=1 bash tests/run-jit.sh
JIT_FALLBACK_BASELINE="tests/jit-fallback-baseline.txt"

# Known latent MISCOMPILES, discovered by this harness being the first to
# COMPILE the nested typed/* fixtures (run.sh scans only tests/fixtures/*/;
# these were interpreter-covered only).  Each failed identically under gcc
# and MIR -- the defect was in the emitted C, not an engine -- so they were
# denylisted here with their reports rather than failing every run.  Add an
# entry (with a report) if the class recurs; remove it when the report is
# resolved.
#
# Both original entries are now fixed and run normally: typed/result-basic by
# the cps->direct aggregate-carrier bridge (findings 28) and
# typed-slots/cs3-nested-specialization by the CS3 nested-spec result recovery
# (findings 30).  The list is deliberately kept (empty) -- it is the mechanism
# for carrying a compile-path miscompile without hiding it.
JIT_KNOWN_MISCOMPILE="
"
jit_known_miscompile() {
    local n
    for n in $JIT_KNOWN_MISCOMPILE; do
        [ "$1" = "$n" ] && return 0
    done
    return 1
}

run_jit_fixture() {
    local dir="$1"
    local name="${dir#tests/fixtures/}"
    local rkey; rkey="$(printf '%s' "$name" | tr '/ ' '__')"
    local input

    if   [ -f "$dir/input.tur" ]; then input="$dir/input.tur"
    elif [ -f "$dir/$(basename "$dir").tur" ]; then input="$dir/$(basename "$dir").tur"
    else return; fi

    if [ -f "$dir/requires.cc" ]; then
        printf 'SKIP %s (requires.cc)\n' "$name"
        echo "SKIP" > "$RESULTS_DIR/$rkey.result"; return; fi
    # hook.sh fixtures delegate the whole build+run+normalize to a
    # fixture-owned script that invokes `tur build` itself -- the two-process
    # shape the JIT replaces.  They stay owned by run.sh.
    if [ -f "$dir/hook.sh" ]; then
        printf 'SKIP %s (hook.sh; owned by run.sh)\n' "$name"
        echo "SKIP" > "$RESULTS_DIR/$rkey.result"; return; fi
    if [ -f "$dir/requires.interp" ] || [ -f "$dir/requires.interp-only" ]; then
        printf 'SKIP %s (interpreter-owned)\n' "$name"
        echo "SKIP" > "$RESULTS_DIR/$rkey.result"; return; fi
    if [ -f "$dir/requires.dedicated-runner" ]; then
        printf 'SKIP %s (requires.dedicated-runner)\n' "$name"
        echo "SKIP" > "$RESULTS_DIR/$rkey.result"; return; fi
    # requires.posix-apis: POSIX-only APIs the fixture reaches from inline-C.
    # tests/run.sh has honoured this since the marker was introduced; this
    # runner did not, so eleven fixtures that run.sh correctly skips on
    # Windows were failing here for a reason that has nothing to do with the
    # JIT -- reactor-*, scheduler-io-park, term-raw-cooked-roundtrip.  They
    # accounted for 11 of the 61 failures in the first Windows corpus run.
    if [ -f "$dir/requires.posix-apis" ] && [ "$TUR_HOST_WINDOWS" = "1" ]; then
        printf 'SKIP %s (posix-apis; not on Windows)\n' "$name"
        echo "SKIP" > "$RESULTS_DIR/$rkey.result"; return; fi
    if [ -f "$dir/requires.spices" ] && [ ! -d "../turmeric-spices" ]; then
        printf 'SKIP %s (requires.spices; sibling checkout absent)\n' "$name"
        echo "SKIP" > "$RESULTS_DIR/$rkey.result"; return; fi
    if [ -f "$dir/requires.tsan" ] && [ "${TUR_TSAN:-0}" != "1" ]; then
        printf 'SKIP %s (requires.tsan)\n' "$name"
        echo "SKIP" > "$RESULTS_DIR/$rkey.result"; return; fi
    if [ -f "$dir/requires.stress" ] && [ "${TUR_STRESS:-0}" != "1" ]; then
        printf 'SKIP %s (requires.stress)\n' "$name"
        echo "SKIP" > "$RESULTS_DIR/$rkey.result"; return; fi
    if jit_known_miscompile "$name"; then
        printf 'SKIP %s (known miscompile -- see docs/reported/)\n' "$name"
        echo "SKIP" > "$RESULTS_DIR/$rkey.result"; return; fi

    if stamp_check "$name" "$input"; then
        printf 'PASS %s\n' "$name"
        echo "PASS" > "$RESULTS_DIR/$rkey.result"
        return
    fi

    local fixture_flags=""
    [ -f "$dir/flags" ] && fixture_flags=$(cat "$dir/flags")

    # Per-fixture timeout.  Default sits between run.sh's 10s (compiled) and
    # run-turi.sh's 15s: the program runs at native speed but pays the
    # engine's compile on top.
    local fixture_timeout=15
    if [ -f "$dir/expected.timeout" ]; then
        local _t; _t=$(tr -d '[:space:]' < "$dir/expected.timeout")
        case "$_t" in [0-9]*) fixture_timeout=$_t ;; esac
    fi

    local run_args_arr=()
    if [ -f "$dir/run.args" ]; then
        while IFS= read -r _ra; do
            [ -z "$_ra" ] && continue
            run_args_arr+=("$_ra")
        done < "$dir/run.args"
    fi

    local actual_stdout="$dir/jit.stdout"
    local actual_stderr="$dir/jit.stderr"
    local stdin_file=/dev/null
    [ -f "$dir/input.stdin" ] && stdin_file="$dir/input.stdin"

    local rc=0
    if [ "${#run_args_arr[@]}" -gt 0 ]; then
        _run_timed "$fixture_timeout" "$TUR" $fixture_flags jit "$input" \
            -- "${run_args_arr[@]}" \
            < "$stdin_file" > "$actual_stdout" 2> "$actual_stderr" || rc=$?
    else
        _run_timed "$fixture_timeout" "$TUR" $fixture_flags jit "$input" \
            < "$stdin_file" > "$actual_stdout" 2> "$actual_stderr" || rc=$?
    fi

    local fell_back=""
    grep -q 'TUR-W0070' "$actual_stderr" 2>/dev/null && fell_back=1

    local expected_exit="0"
    [ -f "$dir/expected.exit" ] && expected_exit=$(tr -d '[:space:]' < "$dir/expected.exit")

    # Report a timeout AS a timeout.  timeout(1) exits 124 when it kills the
    # child, and the partial (usually empty) stdout that leaves behind would
    # otherwise fall through to the diff below and be reported as "stdout
    # mismatch" -- a claim about the answer when the only fact is the clock.
    # run.sh and run-turi.sh each grew this check after that misreport cost a
    # triage pass (docs/archive/ci-cps-tramp-turi-timeouts-under-load.md);
    # this harness was missed, and the same misreport cost another one on
    # 2026-10-01 (see docs/reported/macos-jit-leg-stall-unexplained.md, where
    # a slow runner's killed r7rs-tail-calls read as a tail-call regression).
    # This check must stay ahead of the stdout diff.
    if [ "$rc" -eq 124 ] && [ "$expected_exit" != "124" ]; then
        echo "FAIL $name -- timed out (>${fixture_timeout}s under the JIT engine)${fell_back:+ (via cc fallback)}"
        echo "FAIL" > "$RESULTS_DIR/$rkey.result"
        return
    fi

    if [ -f "$dir/expected.stdout" ]; then
        if ! diff -u "$dir/expected.stdout" "$actual_stdout" > /dev/null 2>&1; then
            echo "FAIL $name -- stdout mismatch${fell_back:+ (via cc fallback)}"
            diff -u "$dir/expected.stdout" "$actual_stdout" | head -20 | sed 's/^/    /'
            echo "FAIL" > "$RESULTS_DIR/$rkey.result"
            return
        fi
    fi

    if [ "$expected_exit" = "nonzero" ]; then
        if [ "$rc" -eq 0 ]; then
            echo "FAIL $name -- expected nonzero exit, got 0"
            echo "FAIL" > "$RESULTS_DIR/$rkey.result"
            return
        fi
    else
        if [ "$rc" -ne "$expected_exit" ]; then
            echo "FAIL $name -- exited $rc (expected $expected_exit)${fell_back:+ (via cc fallback)}"
            [ -s "$actual_stderr" ] && tail -5 "$actual_stderr" | sed 's/^/    stderr: /'
            echo "FAIL" > "$RESULTS_DIR/$rkey.result"
            return
        fi
    fi

    # TUR-W0071 without W0070: the reduced TU (split runtime and/or the
    # pruned program, src/compiler/jit_prune.h) failed and only the full-TU
    # retry got the engine through.  The output is right, but that is the
    # one way a reference the pruner missed would otherwise pass silently
    # -- and pay two c2mir compiles.
    if [ -z "$fell_back" ] && grep -q 'TUR-W0071' "$actual_stderr" 2>/dev/null; then
        echo "FAIL $name -- passed only on the full-TU retry (TUR-W0071): $(_first_jit_error "$actual_stderr")"
        grep 'TUR-W0071' "$actual_stderr" | head -1 | sed 's/^/    stderr: /'
        echo "FAIL" > "$RESULTS_DIR/$rkey.result"
        return
    fi

    if [ -n "$fell_back" ]; then
        # Correct output through the cc fallback: a pass, tallied separately
        # so the jit-native count stays an honest signal.  Not stamped -- a
        # future engine improvement should get the chance to reclaim it.
        printf 'PASS %s (via cc fallback)\n' "$name"
        echo "PASS_FALLBACK" > "$RESULTS_DIR/$rkey.result"
        return
    fi

    stamp_write "$name" "$input"
    printf 'PASS %s\n' "$name"
    echo "PASS" > "$RESULTS_DIR/$rkey.result"
}

# Negative fixtures: the diagnostics come from the same front end as the cc
# path (compile_to_c fails before the engine is reached), so expected.diag
# must match under `tur jit` exactly as under `tur build`.
run_jit_error_fixture() {
    local dir="$1"
    local name="${dir#tests/fixtures/}"
    local rkey; rkey="$(printf '%s' "$name" | tr '/ ' '__')"

    [ -f "$dir/input.tur" ] || return
    [ -s "$dir/expected.diag" ] || return
    # requires.interp-only is checked alongside requires.interp, as the happy
    # path already does.  It was missing here, and the two markers are
    # near-homographs that do opposite things (see CLAUDE.md), so an `errors/`
    # fixture asserting an INTERPRETER diagnostic -- which is exactly what
    # requires.interp-only means for a negative fixture -- was run through
    # `tur jit` and reported `jit diagnostic mismatch` for a diagnostic no
    # compiled path ever emits.  Caught by
    # errors/turi-multishot-resume-past-fiber-body, the first such fixture.
    if [ -f "$dir/requires.cc" ] || [ -f "$dir/requires.interp" ] \
       || [ -f "$dir/requires.interp-only" ] \
       || [ -f "$dir/requires.spices" ]; then return; fi
    if [ -f "$dir/requires.posix-apis" ] && [ "$TUR_HOST_WINDOWS" = "1" ]; then
        return; fi

    if stamp_check "$name" "$dir/input.tur"; then
        printf 'PASS %s\n' "$name"
        echo "PASS" > "$RESULTS_DIR/$rkey.result"
        return
    fi

    local flags=""; [ -f "$dir/flags" ] && flags=$(cat "$dir/flags")
    local err="$dir/jit.stderr"
    # _run_timed, NOT bare `timeout` -- stock macOS ships no timeout(1) (see
    # the _tur_timeout_bin probe above).  This call site was missed when that
    # guard went in, and the failure is silent and total: `timeout` not found
    # makes the command fail, `|| true` swallows it, jit.stderr ends up with a
    # shell error instead of a diagnostic, and EVERY needle misses -- so all
    # ~400 negative fixtures report `jit diagnostic mismatch` as though the
    # compiler had stopped emitting diagnostics.  A Mac with Homebrew coreutils
    # on PATH does not reproduce it, which is why the local baseline was green
    # while macOS CI was not.
    _run_timed 15 "$TUR" $flags jit "$dir/input.tur" \
        >/dev/null 2>"$err" || true

    local missing=0 needle
    while IFS= read -r needle; do
        [ -z "$needle" ] && continue
        grep -F -q "$needle" "$err" || missing=1
    done < "$dir/expected.diag"

    if [ "$missing" -eq 0 ]; then
        stamp_write "$name" "$dir/input.tur"
        printf 'PASS %s\n' "$name"
        echo "PASS" > "$RESULTS_DIR/$rkey.result"
    else
        echo "FAIL $name -- jit diagnostic mismatch"
        echo "FAIL" > "$RESULTS_DIR/$rkey.result"
    fi
}

export TUR STAMP_CACHE RESULTS_DIR TUR_FORCE TUR_MTIME TUR_CC_FLAGS
export JIT_KNOWN_MISCOMPILE
export -f run_jit_fixture run_jit_error_fixture jit_known_miscompile
export -f stamp_check stamp_write stamp_key _tur_hash_file _tur_mtime
export -f _run_timed
export _tur_timeout_bin

shopt -s nullglob
ALL_DIRS=()
for d in tests/fixtures/*/ tests/fixtures/*/*/; do
    d="${d%/}"
    case "$d" in tests/fixtures/errors|tests/fixtures/errors/*) continue ;; esac
    [ -d "$d" ] || continue
    ALL_DIRS+=("$d")
done

# Are we producing Windows binaries?  Mirrors tests/run.sh: keyed on MSYSTEM
# rather than uname so it stays false under WSL, which runs the Linux build and
# has every POSIX API.
TUR_HOST_WINDOWS=0
case "${MSYSTEM:-}" in
  UCRT64|MINGW64|CLANG64|MINGW32) TUR_HOST_WINDOWS=1 ;;
esac
export TUR_HOST_WINDOWS

JIT_FILTER="${JIT_FILTER:-${TUR_TEST_FILTER:-}}"

# Optional sharding, so N runners can split the corpus.  Parsed exactly as
# tests/run.sh parses it (same "i/N" spelling, same clamping of a nonsense
# index) -- a second, subtly different dialect of the same variable is worse
# than no sharding at all, because CI sets it once for both harnesses.
TUR_TEST_SHARD="${TUR_TEST_SHARD:-}"
SHARD_INDEX=0
SHARD_TOTAL=1
if [ -n "$TUR_TEST_SHARD" ]; then
    case "$TUR_TEST_SHARD" in
        */*)
            shard_left="${TUR_TEST_SHARD%/*}"
            shard_right="${TUR_TEST_SHARD#*/}"
            case "$shard_left" in ''|*[!0-9]*) shard_left=1 ;; esac
            case "$shard_right" in ''|*[!0-9]*) shard_right=1 ;; esac
            if [ "$shard_right" -lt 1 ]; then shard_right=1; fi
            if [ "$shard_left" -lt 1 ]; then shard_left=1; fi
            if [ "$shard_left" -gt "$shard_right" ]; then shard_left="$shard_right"; fi
            SHARD_TOTAL="$shard_right"
            SHARD_INDEX=$((shard_left - 1))
            ;;
    esac
fi

matches_shard() {
    if [ "$SHARD_TOTAL" -le 1 ]; then
        return 0
    fi
    [ $(($1 % SHARD_TOTAL)) -eq "$SHARD_INDEX" ]
}

# Refused up front, not warned about at the end: the baseline is rewritten
# WHOLE, so regenerating it from a shard would silently delete the N-1/N of the
# names this run never exercised, and the next full run would report every one
# of them as a new fallback.  Nothing about the resulting file would look
# wrong.  Checked here rather than at the ratchet so the answer arrives before
# a shard's worth of fixtures runs, not after.
if [ "${TUR_JIT_FALLBACK_UPDATE:-0}" = "1" ] && [ "$SHARD_TOTAL" -gt 1 ]; then
    echo "FAIL run-jit -- TUR_JIT_FALLBACK_UPDATE=1 with TUR_TEST_SHARD=$TUR_TEST_SHARD" >&2
    echo "  The baseline is rewritten whole, so a shard would drop every name it" >&2
    echo "  did not run.  Regenerate from an unsharded run." >&2
    exit 2
fi

# The ordinal advances on EVERY discovered fixture, not just admitted ones, so
# shard membership is a property of the corpus rather than of the filter.  A
# filtered shard run is then a subset of the same slice an unfiltered one takes
# -- which is what makes `TUR_TEST_FILTER` usable to re-run one shard's failure.
FILTERED_DIRS=()
fixture_ordinal=0
for d in "${ALL_DIRS[@]}"; do
    name="${d#tests/fixtures/}"
    if { [ -z "$JIT_FILTER" ] || grep -E -q "$JIT_FILTER" <<< "$name"; } \
       && matches_shard "$fixture_ordinal"; then
        FILTERED_DIRS+=("$d")
    fi
    fixture_ordinal=$((fixture_ordinal + 1))
done

ERROR_DIRS=()
error_ordinal=0
for d in tests/fixtures/errors/*/; do
    d="${d%/}"; [ -d "$d" ] || continue
    name="${d#tests/fixtures/}"
    if { [ -z "$JIT_FILTER" ] || grep -E -q "$JIT_FILTER" <<< "$name"; } \
       && matches_shard "$error_ordinal"; then
        ERROR_DIRS+=("$d")
    fi
    error_ordinal=$((error_ordinal + 1))
done

# Both slices are resolved before any fixture runs, which is what lets
# TUR_TEST_LIST answer "what is in shard i/N?" without executing anything.
# tests/run-shard-partition.sh drives the partition assertions through this,
# so they test the harness's real enumeration rather than a copy of it that
# can drift.  Names only, one per line, `errors/` kept in the path.
if [ "$LIST_ONLY" = "1" ]; then
    for d in "${FILTERED_DIRS[@]+"${FILTERED_DIRS[@]}"}" \
             "${ERROR_DIRS[@]+"${ERROR_DIRS[@]}"}"; do
        echo "${d#tests/fixtures/}"
    done
    exit 0
fi

if [ ${#FILTERED_DIRS[@]} -gt 0 ]; then
    printf '%s\n' "${FILTERED_DIRS[@]}" | \
        xargs -P "$JOBS" -I{} bash -c 'run_jit_fixture "$@"' _ {} 2>/dev/null
fi
if [ ${#ERROR_DIRS[@]} -gt 0 ]; then
    printf '%s\n' "${ERROR_DIRS[@]}" | \
        xargs -P "$JOBS" -I{} bash -c 'run_jit_error_fixture "$@"' _ {} 2>/dev/null
fi

FALLBACK_NAMES=()
for rf in "$RESULTS_DIR"/*.result; do
    [ -f "$rf" ] || continue
    kind="$(cat "$rf")"
    name="$(basename "${rf%.result}" | tr '__' '/')"
    case "$kind" in
        PASS)          PASS=$((PASS + 1)) ;;
        PASS_FALLBACK) PASS=$((PASS + 1)); FALLBACK=$((FALLBACK + 1))
                       FALLBACK_NAMES+=("$name") ;;
        FAIL)          FAIL=$((FAIL + 1)); FAILED+=("$name") ;;
        SKIP)          SKIP=$((SKIP + 1)) ;;
    esac
done

echo
if [ "$SHARD_TOTAL" -gt 1 ]; then
    echo "jit fixture summary (shard $((SHARD_INDEX + 1))/$SHARD_TOTAL): $PASS passed, $FAIL failed, $SKIP skipped"
else
    echo "jit fixture summary: $PASS passed, $FAIL failed, $SKIP skipped"
fi
if [ "$FALLBACK" -gt 0 ]; then
    echo "  (of which $FALLBACK passed via the cc fallback -- TUR-W0070)"
fi

# Fixtures closest to their own expected.timeout.  A per-fixture bound that a
# slow runner draw can cross is invisible until it does, and this leg draws a
# ~1.9x slower 3-core macos-latest ~97% of the time -- so "passed" is not the
# same as "has margin".  Printed unconditionally and cheap: one awk pass over
# files the run already wrote.
#
# An array, not `ls`: nullglob is on (above), so when no fixture wrote a .time
# file -- every one skipped, or answered from the stamp cache -- the glob is
# EMPTY.  `ls` with no operand lists the cwd and succeeds, and `cat` with no
# operand then reads stdin, which hung the harness forever on a terminal or
# socket stdin.
_time_files=("$RESULTS_DIR"/*.time)
if [ "${#_time_files[@]}" -gt 0 ]; then
    echo "  closest to their timeout budget (elapsed/budget, worst attempt):"
    cat "${_time_files[@]}" 2>/dev/null \
      | awk '{ pct = ($2 > 0) ? (100 * $1 / $2) : 0
               if (pct > best[$3]) { best[$3] = pct; el[$3] = $1; bud[$3] = $2 } }
             END { for (n in best) printf "%6.0f%%  %9s  %s\n",
                                         best[n], (el[n] "s/" bud[n] "s"), n }' \
      | sort -rn | head -"${TUR_HEADROOM_TOP:-8}" | sed 's/^/   /'
fi

# The fallback ratchet (see the header above RESULTS_DIR).  Only meaningful
# on a full run: under a filter most baseline names are simply not exercised,
# so reclaimed-vs-missing cannot be told apart and NEW fallbacks alone are
# checked.  A SHARD is the same situation arriving a different way -- it sees
# 1/N of the corpus, so N-1/N of the baseline is unexercised -- and it gets the
# same treatment.  The NEW-fallback half still runs per shard and is what makes
# the ratchet hold across a sharded CI job: every name is in exactly one shard,
# so their union checks every name exactly once.
_observed="$(printf '%s\n' "${FALLBACK_NAMES[@]+"${FALLBACK_NAMES[@]}"}" | sed '/^$/d' | sort -u)"
if [ "${TUR_JIT_FALLBACK_UPDATE:-0}" = "1" ]; then
    {
        echo "# tests/jit-fallback-baseline.txt -- fixtures run-jit.sh ALLOWS to pass via"
        echo "# the cc fallback (TUR-W0070) instead of the MIR engine.  One name per line."
        echo "# A fallback not listed here FAILs the run; add it deliberately, in the same"
        echo "# commit, with the reason the engine lost it.  A listed fixture the engine"
        echo "# reclaims is reported so the line can be removed.  Regenerate:"
        echo "#   TUR_JIT_FALLBACK_UPDATE=1 TUR=./build-jit/tur bash tests/run-jit.sh"
        echo "# (jit-suite-reports-pass-when-the-engine-is-disabled)"
        printf '%s\n' "$_observed"
    } > "$JIT_FALLBACK_BASELINE"
    echo "  wrote $JIT_FALLBACK_BASELINE ($FALLBACK name(s))"
elif [ -f "$JIT_FALLBACK_BASELINE" ]; then
    _allowed="$(grep -v '^#' "$JIT_FALLBACK_BASELINE" | sed '/^$/d' | sort -u)"
    _new="$(comm -23 <(printf '%s\n' "$_observed") <(printf '%s\n' "$_allowed") | sed '/^$/d')"
    if [ -n "$_new" ]; then
        echo "FAIL run-jit -- fixture(s) fell back to cc that $JIT_FALLBACK_BASELINE does not allow:"
        printf '%s\n' "$_new" | sed 's/^/  - /'
        echo "  The engine used to compile these natively.  Fix the engine, or add the"
        echo "  name(s) to the baseline in the same commit with the reason."
        FAIL=$((FAIL + 1))
        while IFS= read -r _n; do FAILED+=("$_n (new cc fallback)"); done <<< "$_new"
    fi
    if [ -z "$JIT_FILTER" ] && [ "$SHARD_TOTAL" -le 1 ]; then
        _reclaimed="$(comm -13 <(printf '%s\n' "$_observed") <(printf '%s\n' "$_allowed") | sed '/^$/d')"
        if [ -n "$_reclaimed" ]; then
            echo "  reclaimed by the engine (remove from $JIT_FALLBACK_BASELINE):"
            printf '%s\n' "$_reclaimed" | sed 's/^/  - /'
        fi
    fi
else
    echo "  note: $JIT_FALLBACK_BASELINE is missing; run with TUR_JIT_FALLBACK_UPDATE=1 to create it"
fi
if [ $FAIL -ne 0 ]; then
    echo "failed:"
    for f in "${FAILED[@]}"; do echo "  - $f"; done
    exit 1
fi
exit 0
