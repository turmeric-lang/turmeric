#!/usr/bin/env bash
# tests/run.sh — fixture runner for tur (phase 0).
#
# Layout:
#   tests/fixtures/<name>/                 — happy-path fixture
#     input.tur (or <name>.tur)
#     expected.stdout
#     expected.c          (optional codegen snapshot)
#     expected.xfail      (optional: a named failing test -- the stdout
#                          mismatch is expected and passes; a match fails
#                          and says to delete the marker)
#
#   tests/fixtures/errors/<name>/          — negative fixture
#     input.tur
#     expected.diag       (substring(s) that must appear in stderr; one per line)
#
# Pass: exits 0 with "PASS <name>". Any failure exits 1.

set -u
cd "$(dirname "$0")/.."

# Isolate the test suite from any globally exported TUR_STDLIB_DIR (e.g. from
# a mise-managed global `turmeric` install). Otherwise the locally built
# ./build/tur loads stdlib sources from the global install path, and
# __tur_autolink__ references (e.g. src/runtime/hamt.c) resolve into a tree
# that has no such files, spuriously failing the whole suite.
unset TUR_STDLIB_DIR

# UC-3 (user-config-experiments-plan): the compiler now reads a user-level
# experiments file at $XDG_CONFIG_HOME/turmeric/experiments.tur (fallback
# $HOME/.config/turmeric/experiments.tur). A contributor whose real home
# directory carries one would otherwise see local-only experiment enables
# leak into the suite. Point XDG_CONFIG_HOME at an empty temp dir (and clear
# HOME's fallback effect by keeping XDG set) so every run sees no user file.
_TUR_EMPTY_XDG="$(mktemp -d "${TMPDIR:-/tmp}/tur-xdg-empty.XXXXXX")"
export XDG_CONFIG_HOME="$_TUR_EMPTY_XDG"
# NOTE: the EXIT trap that removes this dir is installed alongside the
# RESULTS_DIR cleanup below (a later `trap ... EXIT` would otherwise replace
# an earlier one).

# Overridable so a non-default build tree can be tested without editing this
# file -- notably Windows, where the binary is build-win/tur.exe.
TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || { echo "tests: $TUR not built; run 'make' first" >&2; exit 2; }

# Identity of the binary under test, re-checked at the end of the run.
#
# Fixtures exec $TUR directly out of the build tree, so a rebuild that lands
# mid-run swaps the compiler underneath the suite. During the link window the
# file exists but is not yet executable, and every fixture dispatched in that
# window dies with `Permission denied` -- which this harness reports as
# "build failed". A batch of those reads as a compiler regression and costs a
# full investigation before the cause (a concurrent `cmake --build`) turns up.
#
# `ls -ln` rather than stat(1): the -c/-f format flags are GNU/BSD-specific,
# while the size and mtime columns of `ls -ln` are portable enough to compare
# as an opaque string. A relink that somehow preserved both would slip through;
# that is not a case worth more machinery.
TUR_STAMP_START="$(ls -ln "$TUR" 2>/dev/null)"

# A handful of fixtures write to a literal "/tmp/..." from inline-C fopen. On
# POSIX that always exists; a compiled Windows binary resolves "/tmp" against the
# current drive (C:\tmp), which is not present by default. Create it so those
# fixtures behave the same as everywhere else. Guarded on MSYSTEM so this is a
# no-op off Windows.
case "${MSYSTEM:-}" in
  UCRT64|MINGW64|CLANG64|MINGW32) mkdir -p /c/tmp 2>/dev/null || true ;;
esac

# Are we producing Windows binaries?  Used by the requires.posix-apis skip
# below.  Keyed on MSYSTEM rather than uname so it stays false under WSL, which
# runs the Linux build and has every POSIX API.
# Exported because the fixture workers run as separate bash processes under
# xargs (see the `export -f` block below), so a plain shell variable would not
# reach them and every skip would silently no-op.
TUR_HOST_WINDOWS=0
case "${MSYSTEM:-}" in
  UCRT64|MINGW64|CLANG64|MINGW32) TUR_HOST_WINDOWS=1 ;;
esac
export TUR_HOST_WINDOWS

# A writable scratch directory for fixtures that need one, exported so the
# fixture binaries can find it.
#
# Fixtures used to write hardcoded "/tmp/..." paths.  Those are POSIX
# spellings, and the MSYS *shell* maps /tmp to %LOCALAPPDATA%\Temp -- but a
# fixture binary is a NATIVE Windows executable, so its CRT resolves "/tmp/foo"
# against the CURRENT DRIVE ROOT instead: C:\tmp, D:\tmp.  When that directory
# did not exist the open failed, the program printed nothing, and the fixture
# reported a stdout mismatch that named nothing resembling the cause.  It stayed
# invisible for as long as it did because a developer box accumulates a C:\tmp,
# so the suite was green locally and 12 red on a fresh CI runner.
#
# The harness used to paper over that by creating <drive>:\tmp.  Fixtures now
# read TUR_TEST_TMPDIR instead (falling back to /tmp when it is unset, so one
# can still be run by hand), which is portable by construction rather than by
# provisioning.  Same shape as TUR_BIND_LOOPBACK below.
TUR_TEST_TMPDIR="${TUR_TEST_TMPDIR:-$(mktemp -d 2>/dev/null)}"
if [ -z "$TUR_TEST_TMPDIR" ]; then
    echo "tests: could not create a scratch directory for fixtures" >&2
    exit 1
fi
mkdir -p "$TUR_TEST_TMPDIR" 2>/dev/null || true
# Native fixture binaries need the WINDOWS form of the path; an MSYS-style
# /tmp/... would be resolved against the drive root by their CRT, which is the
# very bug this replaces.
if [ "$TUR_HOST_WINDOWS" = "1" ] && command -v cygpath >/dev/null 2>&1; then
    TUR_TEST_TMPDIR=$(cygpath -m "$TUR_TEST_TMPDIR")
fi
export TUR_TEST_TMPDIR

# Force server fixtures to bind 127.0.0.1 instead of INADDR_ANY. On Windows this
# stops the Defender Firewall "allow this app" dialog from popping for every
# freshly-built fixture binary; elsewhere it is a harmless tightening (the
# fixtures are same-process loopback tests). The stdlib socket/httpd listen code
# reads this env at runtime and only then binds loopback.
export TUR_BIND_LOOPBACK=1

# TI8 (turi-parity-post-v1-plan): CI ratchet -- fail fast if any EX_* expression
# kind the compiler emits has no `case` arm in src/turi/eval.c and is not a
# documented carve-out (docs/artifacts/turi-carve-out.txt).  Cheap, deterministic, and
# keeps the interpreter parity gap from silently growing.  Opt out with
# TUR_SKIP_PARITY_CHECK=1 (e.g. when hacking on eval.c mid-change).
if [ "${TUR_SKIP_PARITY_CHECK:-0}" != "1" ] && command -v python3 >/dev/null 2>&1; then
    if ! python3 tools/check_turi_parity.py; then
        echo "tests: turi parity check failed (see above); aborting." >&2
        exit 1
    fi
    # Prereq 3a (turi-open-reports-prereqs.md): module-preload parity -- every
    # module the compiled path auto-loads is either in the --interpret prelude
    # or carved out with a rationale in docs/artifacts/turi-preload-carve-out.txt.  Keeps
    # the harness-flip "missing native" bucket from silently growing.
    if ! python3 tools/check_turi_native_parity.py; then
        echo "tests: turi native-parity check failed (see above); aborting." >&2
        exit 1
    fi
fi

# Self-test for the emitted-C pointer/integer ratchet below (the per-fixture
# check in run_happy).  A grep that matches nothing is indistinguishable from a
# clean corpus, so the canary asserts the pattern still fires before the suite
# leans on it.  Skipped along with the ratchet itself via TUR_SKIP_CC_WARN_CHECK=1.
if [ "${TUR_SKIP_CC_WARN_CHECK:-0}" != "1" ]; then
    if ! bash tests/check-cc-warn-ratchet.sh; then
        echo "tests: emitted-C warning ratchet self-test failed (see above); aborting." >&2
        exit 1
    fi
fi

# Standing rule from the numeric tower plan (section 1, made standing by N3):
# _Complex / <complex.h> / the __mul*c3 / __div*c3 compiler-runtime family
# never appear in generated C -- c2mir (the JIT) has no _Complex, and the
# helper family would grow the JIT's runtime symbol boundary.  The check
# itself predates this wiring as a ctest target (CMakeLists tur_no_c_complex);
# running it here too puts it on the path every `bash tests/run.sh` invocation
# takes.  Cheap (greps + a few emit-c runs); shares the ratchet opt-out.
if [ "${TUR_SKIP_CC_WARN_CHECK:-0}" != "1" ]; then
    if ! bash tests/check-no-c-complex.sh; then
        echo "tests: no-C-_Complex check failed (see above); aborting." >&2
        exit 1
    fi
    # The same JIT C11-subset rule for atomics: the emitted runtime reaches
    # them through TUR_ATOMIC_*, never a literal `__atomic_*` builtin (c2mir
    # has none, and one in the preamble sends every `tur jit` program to cc).
    if ! bash tests/check-no-atomic-builtins.sh; then
        echo "tests: no-atomic-builtins check failed (see above); aborting." >&2
        exit 1
    fi
fi

# R4 (carrier-crossing-recovery-routing-plan): the audit registry is the single
# source of truth for which carrier<->concrete crossings are routed.  A new
# chokepoint call site that forgot its audit row (or a drifted/stale registry)
# fails here.  Independent of the turi-parity ratchets above so it still runs
# when those are skipped or unrelated.  Opt out with TUR_SKIP_CROSSING_CHECK=1.
if [ "${TUR_SKIP_CROSSING_CHECK:-0}" != "1" ] && command -v python3 >/dev/null 2>&1; then
    if ! python3 tools/check_crossing_routing.py --quiet; then
        echo "tests: crossing-routing audit check failed (see above); aborting." >&2
        exit 1
    fi
fi

PASS=0
FAIL=0
FAILED=()

# Performance plan item #1: compiler cache integration for build steps.
# Opt in with TUR_USE_CCACHE=1 (enabled by default here if ccache is available).
# The generated C is written to a deterministic path (/tmp/tur-build/<name>.c)
# so that ccache can actually produce cache hits across runs.
# CCACHE_NOHASHDIR=1 prevents ccache from hashing the source file directory,
# further improving hit rates for generated files.
TUR_USE_CCACHE="${TUR_USE_CCACHE:-1}"
BUILD_CC="${CC:-cc}"
if [ "$TUR_USE_CCACHE" = "1" ] && command -v ccache >/dev/null 2>&1; then
    BUILD_CC="ccache ${BUILD_CC}"
    export CCACHE_NOHASHDIR=1
fi

# Default compiler flags for test builds.
# Override with TUR_CC_FLAGS="-O1 -std=c99" for faster (but less safe) builds.
# NOTE: -O0 causes SIGTRAP on Apple Silicon; -O1 exposes latent UB in some
#       emitted functions missing a return path — keep -O2 for safety.
# The `-L` for libturi.a is derived from where $TUR actually lives, not
# hardcoded to build/src -- otherwise a non-default build tree (Windows uses
# build-win/) can't resolve the `-lturi` autolink that reactor/async fixtures
# emit, and every one of them fails to link.
# `-Werror=implicit-function-declaration`: an undeclared call in emitted C is
# always a codegen bug (a runtime prelude the program-scan gate did not emit).
# Apple clang rejects it outright, so the macOS leg went red on six fixtures
# that loaded stdlib/serial.tur while gcc 13 on the Linux leg only warned and
# linked; promoting it to an error here makes both legs see the same thing.
# `-Wfloat-conversion`: NOT implied by -Wall (verified: 0 warnings vs 2 on a
# `long f(double d){return d;}` canary), so until this landed the float half of
# the scalar-representation class was unwatched in emitted C -- exactly the gap
# docs/archive/emitted-c-pointer-integer-warnings-unwatched.md closed for the
# pointer/integer half.  An int64 carrier reaching a double slot (or the
# reverse) is a wrong ANSWER, not a style nit: it is how
# docs/reported/nested-class-method-call-picks-the-first-instance.md turns 7.1
# into 7 with nothing else complaining.  Understood by both GCC (>= 4.9) and
# clang with the same diagnostic tag, so the ratchet's grep is portable.
# Deliberately NOT -Wconversion: that adds ~7 warnings out of the hand-written
# preamble/runtime (-Wshorten-64-to-32, -Wsign-conversion) that would have to be
# cleaned first.  Deliberately NOT -Wimplicit-int-float-conversion: clang-only,
# so it would split the Linux and macOS legs.
# Corpus swept clean at 0 hits across 2250 cc-invoking fixtures before this
# landed -- see docs/archive/type-confusion-detection-plan.md section 5.
_tur_build_dir=$(dirname "$TUR")
export TUR_CC_FLAGS="${TUR_CC_FLAGS:--O2 -std=c99 -Wall -Wfloat-conversion -Werror=implicit-function-declaration -fno-strict-aliasing -L${_tur_build_dir}/src}"

# T19: ThreadSanitizer (TSan) support.
# Set TUR_TSAN=1 to compile and run all fixtures with -fsanitize=thread.
# Fixtures whose directory contains a `requires.tsan` marker file are
# SKIPPED when TUR_TSAN is not set and run normally when it is set.
TUR_TSAN="${TUR_TSAN:-0}"
if [ "$TUR_TSAN" = "1" ]; then
    export TUR_CC_FLAGS="$TUR_CC_FLAGS -fsanitize=thread -g"
fi
export TUR_TSAN

# stress-fixture-tiering-plan: fixtures carrying a `requires.stress` marker are
# the full-size twins of per-PR fixtures (1e7 where the per-PR one runs 1e6).
# SKIPPED unless TUR_STRESS=1, which only the nightly sets.
TUR_STRESS="${TUR_STRESS:-0}"
export TUR_STRESS

# proper-tail-calls T2b: `requires.musttail` fixtures assert a depth that holds
# only where the fixture compiler honours `TUR_MUSTTAIL` -- today clang on
# x86-64 / aarch64; gcc 13 and the JIT's c2mir expand it to nothing, and a
# mutual cycle there is ordinary recursion that overflows at -O0.  Probe the
# fixture compiler once with the SAME gate the emitter writes
# (ensure_musttail_macro in src/compiler/emit_module.c -- keep the two in
# step) and PASS-skip the marker when it does not hold.
TUR_HAS_MUSTTAIL=0
_mt_probe_dir="$(mktemp -d)"
cat > "$_mt_probe_dir/p.c" <<'MTEOF'
#if defined(__clang__) && defined(__has_attribute) && !defined(__wasm__) && \
    (defined(__x86_64__) || defined(__aarch64__))
#  if __has_attribute(musttail)
#    define TUR_MUSTTAIL __attribute__((musttail))
#  endif
#endif
#ifndef TUR_MUSTTAIL
#  error no musttail
#endif
int g(int);
int f(int x) { TUR_MUSTTAIL return g(x); }
MTEOF
if $BUILD_CC -c "$_mt_probe_dir/p.c" -o "$_mt_probe_dir/p.o" >/dev/null 2>&1; then
    TUR_HAS_MUSTTAIL=1
fi
rm -rf "$_mt_probe_dir"
export TUR_HAS_MUSTTAIL

# T19: Timeout support.
# `expected.timeout` in a fixture directory sets the per-fixture timeout in
# seconds.  Default is 10.  Set to 0 to disable the timeout for a fixture.
# Uses `timeout(1)` (GNU coreutils), `gtimeout` (Homebrew coreutils on macOS),
# or a Perl alarm(2) fallback when neither is available.
_tur_timeout_bin=""
if command -v timeout >/dev/null 2>&1; then
    _tur_timeout_bin="timeout"
elif command -v gtimeout >/dev/null 2>&1; then
    _tur_timeout_bin="gtimeout"
fi
export _tur_timeout_bin

_run_timed() {
    local secs="$1"; shift
    if [ "$secs" -le 0 ] || [ -z "$_tur_timeout_bin" ]; then
        "$@"
    else
        "$_tur_timeout_bin" "$secs" "$@"
    fi
}
export -f _run_timed

# Performance plan item #3: avoid redundant emit-c work.
# "snapshot-only" means run emit-c only for fixtures that have expected.c.
# Set TUR_EMIT_C_MODE=always to force old behavior.
TUR_EMIT_C_MODE="${TUR_EMIT_C_MODE:-snapshot-only}"

# Performance plan item #2: parallel fixture execution.
# Override with TUR_TEST_JOBS=<n>; defaults to physical core count (capped at 8).
# The auto-detected default is capped at physical cores (not 2x) to avoid
# flooding syspolicyd on macOS with simultaneous new-binary executions from
# requires.compiled fixtures.  An *explicit* TUR_TEST_JOBS is an intentional
# override and is honored uncapped -- a 16/32-core CI runner should be able to
# use its cores.
if [ -n "${TUR_TEST_JOBS:-}" ]; then
    JOBS="$TUR_TEST_JOBS"
else
    if command -v getconf >/dev/null 2>&1; then
        _nproc="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
    elif command -v sysctl >/dev/null 2>&1; then
        _nproc="$(sysctl -n hw.logicalcpu 2>/dev/null || echo 4)"
    else
        _nproc=4
    fi
    JOBS=$(( _nproc ))
    # Cap the auto-detected value only; an explicit TUR_TEST_JOBS bypasses this.
    if [ "$JOBS" -gt 8 ]; then JOBS=8; fi
fi

case "$JOBS" in
    ''|*[!0-9]*) JOBS=4 ;;
esac
if [ "$JOBS" -lt 1 ]; then JOBS=1; fi

RESULTS_DIR="$(mktemp -d -t tur-tests-results-XXXXXX)"

# Sanitizer findings from `tur` ITSELF (not from the programs it emits).
#
# The Debug build compiles the compiler -fsanitize=address,undefined WITHOUT
# -fno-sanitize-recover, so a UBSan finding prints one line to stderr and
# execution continues.  This suite compares stdout, so such a line has always
# been invisible: `fat_captures_borrowed` was read uninitialized on 60 fixtures,
# on every run, for as long as it existed, and nothing ever failed
# (docs/archive/history/fat-captures-borrowed-read-uninitialized.md).
#
# Every phase below already redirects the compiler's stderr to actual.stderr, so
# scanning it costs one grep per phase.  Findings are collected rather than
# failed on: turning them into hard failures would have converted 60 silent
# findings into 60 red fixtures at once, which is how a gate gets disabled
# instead of fixed.  Set TUR_SANITIZER_GATE=1 to make a finding fail the run.
# Workers append concurrently; each line is far under PIPE_BUF, and this suite
# already relies on short appends being atomic for its progress output.
SANITIZER_LOG="$RESULTS_DIR/sanitizer.log"
: > "$SANITIZER_LOG"
export SANITIZER_LOG
trap 'rm -rf "$RESULTS_DIR" "$_TUR_EMPTY_XDG"' EXIT

# A killed or interrupted run must NEVER print a success-looking summary.
# Without this guard, a SIGINT/SIGTERM that the parent shell survives (e.g. the
# harness signals only the xargs workers, or the run is cut short for time)
# would fall straight through to the "summary: N passed, 0 failed" tally below
# and report a *partial* run as green.  Trap the catchable signals and bail with
# a loud, non-zero status; the completeness check at the very end is the backstop
# for the cases a trap cannot see (SIGKILLed workers).  _INTERRUPTED is also read
# by that final check.
_INTERRUPTED=0
_abort_on_signal() {
    _INTERRUPTED=1
    echo
    echo "ABORTED: run.sh caught a signal before finishing -- results are PARTIAL, NOT a pass." >&2
    # 130 = 128 + SIGINT(2); a conventional "interrupted" status that is clearly
    # not 0 (success) and not 1 (test failures).
    exit 130
}
trap _abort_on_signal INT TERM

# Optional regex filter for fixture names (relative path under tests/fixtures).
# Example: TUR_TEST_FILTER='^rc-auto-drop|^rc-ref-conversion$'
TUR_TEST_FILTER="${TUR_TEST_FILTER:-}"
# Optional regex of fixture names to leave OUT (applied after the filter).
# tests/run-fnsan.sh uses it for the fixtures that carry a `known.fnsan`
# marker, which it then runs on their own and requires to still trap.
TUR_TEST_EXCLUDE="${TUR_TEST_EXCLUDE:-}"

# Optional named sub-suite for faster developer feedback / CI fan-out.
# Groups are defined by file/dir presence (robust), not fragile name regexes:
#   all|<empty> -- everything (default)
#   happy       -- positive fixtures only (tests/fixtures/* minus errors/)
#   errors      -- negative fixtures only (tests/fixtures/errors/*)
#   snapshots   -- positive fixtures carrying a codegen snapshot (expected.c)
# Compose freely with TUR_TEST_FILTER (regex) and TUR_TEST_SHARD.
TUR_TEST_SUITE="${TUR_TEST_SUITE:-}"
case "$TUR_TEST_SUITE" in
    ''|all|happy|errors|snapshots) ;;
    *)
        echo "run.sh: unknown TUR_TEST_SUITE='$TUR_TEST_SUITE'" >&2
        echo "  valid: all (default), happy, errors, snapshots" >&2
        exit 2
        ;;
esac

# Admit a fixture into the current suite.  $1 = kind (happy|error); $2 = dir.
# Ordinals are still assigned over the FULL discovery order (see below), so
# suite selection composes with sharding without shifting shard membership.
suite_admits() {
    case "$TUR_TEST_SUITE" in
        ''|all)    return 0 ;;
        happy)     [ "$1" = "happy" ] ;;
        errors)    [ "$1" = "error" ] ;;
        snapshots) [ "$1" = "happy" ] && [ -f "$2/expected.c" ] ;;
        *)         return 0 ;;
    esac
}

# Optional sharding to split full suite across bounded timeout runs.
# Format: TUR_TEST_SHARD="1/8" (1-based index/total).
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

matches_filter() {
    local fixture_name="$1"
    if [ -n "$TUR_TEST_EXCLUDE" ] && [[ "$fixture_name" =~ $TUR_TEST_EXCLUDE ]]; then
        return 1
    fi
    if [ -z "$TUR_TEST_FILTER" ]; then
        return 0
    fi
    # bash's own =~, not `printf | grep`: this runs once per fixture, and the
    # pipeline spawned a subshell AND a grep each time.  On Linux that is
    # cheap enough to hide; on Windows a process launch is ~50x dearer, and
    # 2800 fixtures x 2 launches made a FILTERED run cost 2m28s before it ran
    # a single test -- the same run costs ~8s now.  Measured, not estimated.
    # (Unfiltered runs never reached the pipeline, so CI was never affected.)
    [[ "$fixture_name" =~ $TUR_TEST_FILTER ]]
}

matches_shard() {
    local ordinal="$1"
    if [ "$SHARD_TOTAL" -le 1 ]; then
        return 0
    fi
    [ $((ordinal % SHARD_TOTAL)) -eq "$SHARD_INDEX" ]
}

# Append any sanitizer findings in $1 (a phase's captured stderr) to the shared
# log, tagged with the fixture and phase.  UBSan's format is
# `<file>:<line>:<col>: runtime error: <what>`; `: runtime error:` is not
# produced anywhere in src/ and no expected.diag contains it, so this does not
# collide with a fixture's own diagnostics.
note_sanitizer() {
    local stderr_file="$1" name="$2" phase="$3"
    [ -s "$stderr_file" ] || return 0
    grep -- ": runtime error:" "$stderr_file" 2>/dev/null \
        | sed "s|^|$name\t$phase\t|" >> "$SANITIZER_LOG" || true
}

write_result() {
    local kind="$1"
    local name="$2"
    local detail="$3"
    local log_file="$4"
    local id
    id="$(printf '%s' "$kind-$name" | tr '/ ' '__')"
    {
        printf '%s\n' "$kind"
        printf '%s\n' "$name"
        printf '%s\n' "$detail"
        printf '%s\n' "$log_file"
    } > "$RESULTS_DIR/$id.result"
    # Immediately print outcome so progress is visible during parallel runs.
    # Single-line echo calls are atomic on Linux/macOS (under PIPE_BUF),
    # so lines from concurrent workers do not interleave.
    if [ "$kind" = "PASS" ]; then
        # A named failing test says so on its PASS line, so a reader of the
        # log sees the gap is still open; skips stay terse -- except a stress
        # skip, which the nightly's full-size tier greps for to prove its
        # fixtures really ran (nightly-arm64.yml).
        case "$detail" in
            *xfail*|*stress-skipped*) echo "PASS $name $detail" ;;
            *)       echo "PASS $name" ;;
        esac
    elif [ "$kind" = "FAIL" ]; then
        echo "FAIL $name${detail:+ — $detail}"
    fi
}

# ---------------------------------------------------------------------------
# Stamp-file caching (T2-C)
# After a fixture passes, record a stamp: content-hash of input.tur, of its
# expected.c snapshot, the mtime of the tur binary, and one hash over every
# file under stdlib/.  On the next run, if all four are unchanged the fixture
# is skipped without rebuilding.
# Disable with TUR_FORCE=1 or by setting TUR_STAMP_CACHE="".
# Stamps are stored in tests/.stamp-cache/ (listed in .gitignore).
#
# The stdlib hash is there because the stdlib is data the compiler reads at
# elaboration time, not code linked into `tur`: a stdlib-only edit changes
# neither the binary's mtime nor any fixture file, so without it every stamp
# stayed valid and the run reported a full green that recompiled nothing
# (docs/archive/run-sh-stamp-cache-ignores-the-stdlib.md).
#
# The config hash (TUR_CONFIG_HASH, below) is there for the same reason one
# step further out: two runs can build every fixture DIFFERENTLY from the same
# sources and the same `tur`.  TUR_PREAMBLE_SPLIT decides whether each program
# carries the whole runtime preamble or links libturt_preamble.a; CC and
# TUR_CC_FLAGS pick the compiler and its mode; TUR_REGIONS and friends change
# codegen.  None of them rebuilds `tur`, so without this an A/B of the two
# preamble paths ran the second half as pure stamp hits and reported a green
# it never earned (docs/archive/run-sh-stamp-cache-ignores-the-preamble-split-mode.md).
# What the stamp still does NOT cover: a fixture's `load` of a file outside
# stdlib/ and outside its own directory, and a C compiler upgraded in place
# under the same name AND version string.  TUR_FORCE=1 after changing either.
# ---------------------------------------------------------------------------
TUR_FORCE="${TUR_FORCE:-0}"
TUR_STAMP_CACHE="${TUR_STAMP_CACHE:-tests/.stamp-cache}"

_tur_hash_file() {
    local path="$1"
    if command -v md5 >/dev/null 2>&1; then
        md5 -q "$path" 2>/dev/null
    elif command -v md5sum >/dev/null 2>&1; then
        md5sum "$path" 2>/dev/null | awk '{print $1}'
    else
        echo "nohash"
    fi
}

# GNU first, and only an all-digit answer counts.  This used to try BSD's
# `stat -f '%m'` first -- but `-f` on GNU stat means "filesystem status": it
# printed the volume's FREE-BLOCK counts for `tur` (then failed on the file
# named `%m`), so on Linux every stamp key carried a number that moves with
# every write to the disk and the cache almost never hit.  Same shape as
# tests/turi/repl-spice-watch.sh's mtime_of.
_tur_mtime() {
    local m
    m="$(stat -c '%Y' "$1" 2>/dev/null)"
    case "$m" in ''|*[!0-9]*) m="$(stat -f '%m' "$1" 2>/dev/null)" ;; esac
    case "$m" in ''|*[!0-9]*) m=0 ;; esac
    echo "$m"
}

# Performance optimization: cache the compiler binary modification time once
# at startup so we do not spawn a redundant stat process for every single fixture.
export TUR_MTIME="$(_tur_mtime "$TUR")"

# ...and hash the stdlib once, for the same reason: one pass over the tree per
# run, not per fixture.  Sorted so the order find(1) walks in cannot change it.
_tur_hash_stdin() {
    if command -v md5 >/dev/null 2>&1; then
        md5 -q
    elif command -v md5sum >/dev/null 2>&1; then
        md5sum | awk '{print $1}'
    else
        echo "nohash"
    fi
}
export TUR_STDLIB_HASH="$(find stdlib -type f 2>/dev/null | LC_ALL=C sort |
    while IFS= read -r _f; do printf '%s\n' "$_f"; cat "$_f"; done | _tur_hash_stdin)"

# ...and the build CONFIGURATION, once per run (see the block comment above).
#
# The preamble mode is RESOLVED, not read off TUR_PREAMBLE_SPLIT: the split
# also depends on whether libturt_preamble.a was built, on the platform default,
# and on -fsanitize in TUR_CC_FLAGS (preamble_split_auto_applies, src/main.c).
# So ask `tur` -- one probe build, whose link line names -lturt_preamble exactly
# when the split engaged.  The same probe is how CI's `whole-preamble` job
# verifies its opt-out.
TUR_PREAMBLE_MODE=unknown
_pm_dir="$TUR_TEST_TMPDIR/preamble-probe"
mkdir -p "$_pm_dir"
printf '(defn main [] : int 0)\n' > "$_pm_dir/p.tur"
if _pm_out="$(TUR_SHOW_CC=1 "$TUR" build "$_pm_dir/p.tur" -o "$_pm_dir/p" 2>&1)"; then
    case "$_pm_out" in
        *-lturt_preamble*) TUR_PREAMBLE_MODE=split ;;
        *)                 TUR_PREAMBLE_MODE=whole ;;
    esac
fi
rm -rf "$_pm_dir"
export TUR_PREAMBLE_MODE

# Every TUR_* variable in the environment is a potential codegen or link knob
# (TUR_REGIONS, TUR_OPTION_NICHE, TUR_RCGC_FROM_ARCHIVE, TUR_RUNTIME, ...), so
# hash them ALL rather than a list that goes stale the next time one is added.
# Excluded: this harness's own bookkeeping and selection variables, which do not
# change how a fixture is built -- and TUR_TEST_TMPDIR, which is a fresh mktemp
# every run and would invalidate every stamp.  The C compiler is named by CC and
# by its version banner (ccache is a launcher, not a compiler, so it is not in
# the key).  An unrecognised extra variable costs a cold run; a missing one
# costs a green that never ran, so the list errs on the side of including.
TUR_CONFIG_HASH="$( {
    printf 'preamble=%s\n' "$TUR_PREAMBLE_MODE"
    printf 'cc=%s\n' "${CC:-cc}"
    "${CC:-cc}" --version 2>/dev/null | head -n 1
    env | LC_ALL=C sort | grep '^TUR_' | grep -vE \
        '^TUR_(TEST_[A-Z_]*|FORCE|STAMP_[A-Z_]*|MTIME|STDLIB_HASH|CONFIG_HASH|PREAMBLE_MODE|USE_CCACHE|VERBOSE|SHOW_CC|HOST_WINDOWS|HAS_MUSTTAIL|BIND_LOOPBACK)='
} | _tur_hash_stdin)"
export TUR_CONFIG_HASH

echo "run.sh: preamble=$TUR_PREAMBLE_MODE cc=${CC:-cc} config=$TUR_CONFIG_HASH"

stamp_key() {
    local input="$1"
    local dir
    dir="$(dirname "$input")"
    # Incorporate the expected.c snapshot hash (if present) so that regenerating
    # snapshots invalidates the stamp and forces a fresh codegen check.
    local ec_hash=""
    [ -f "$dir/expected.c" ] && ec_hash="$(_tur_hash_file "$dir/expected.c")"
    echo "$(_tur_hash_file "$input")-${ec_hash}-${TUR_MTIME}-${TUR_STDLIB_HASH}-${TUR_CONFIG_HASH}"
}

stamp_check() {
    local name="$1" input="$2"
    [ "$TUR_FORCE" = "1" ] && return 1
    [ -z "$TUR_STAMP_CACHE" ] && return 1
    local sf="$TUR_STAMP_CACHE/$(printf '%s' "$name" | tr '/ ' '__').stamp"
    [ -f "$sf" ] || return 1
    [ "$(cat "$sf")" = "$(stamp_key "$input")" ]
}

stamp_write() {
    local name="$1" input="$2"
    [ -z "$TUR_STAMP_CACHE" ] && return
    mkdir -p "$TUR_STAMP_CACHE"
    local sf="$TUR_STAMP_CACHE/$(printf '%s' "$name" | tr '/ ' '__').stamp"
    stamp_key "$input" > "$sf"
}
# ---------------------------------------------------------------------------

# A fixture directory the runner cannot run.  This used to record PASS with a
# "(no input -- skipped)" detail while printing SKIP to the live stream, so the
# loss was invisible in the summary line and in CI -- four directories holding
# 30 loose `.tur` files sat that way, three of them covered by no harness at
# all.  Every fixture dir must now declare how it runs.  See
# docs/archive/fixture-dirs-with-loose-tur-files-pass-without-running.md.
no_input_fail() {
    local dir="$1" name="$2" alt="$3"
    local log="$RESULTS_DIR/$(printf '%s' "no-input-$name" | tr '/ ' '__').log"
    {
        echo "FAIL $name -- no runnable input"
        echo "    Looked for $dir/input.tur and $dir/$alt"
        echo "    A fixture directory must declare how it runs, by holding one of:"
        echo "      input.tur (or <dirname>.tur)  -- run by this harness"
        echo "      hook.sh                       -- drives itself"
        echo "      requires.dedicated-runner     -- owned by a ctest target or"
        echo "                                       tests/run-*.sh; put the owner's"
        echo "                                       name in the marker file"
        echo "    Loose .tur files under other names are run by nothing: give each"
        echo "    its own subdirectory with input.tur + expected.stdout."
        echo "    If this dir holds only generated artifacts (actual.*, turi.stderr)"
        echo "    left behind by a deleted fixture, delete the directory."
    } > "$log"
    write_result "FAIL" "$name" "no runnable input" "$log"
}

run_happy() {
    local dir="$1"
    local name="${dir#tests/fixtures/}"

    # hook.sh: if present, delegate entirely to the fixture-supplied script.
    # The script is invoked with TUR, CC, and TUR_CC_FLAGS in the environment.
    # It should write its stdout to a file named `actual.stdout` in a temp dir
    # that the runner passes as $1, and exit 0 on success, nonzero on failure.
    if [ -f "$dir/hook.sh" ]; then
        local hook_tmp
        hook_tmp=$(mktemp -d)
        local hook_log="$hook_tmp/hook.log"
        local actual_hook_stdout="$hook_tmp/actual.stdout"
        # requires.no-leak-check: honor the marker on the hook.sh path too.
        # LeakSanitizer aborts via _exit(), which skips stdio flushing -- a
        # buffered final line (e.g. "done") would be lost and the snapshot
        # would mismatch.  Disable leak detection for the spawned program,
        # mirroring the standard runner below.
        local hook_asan="$ASAN_OPTIONS"
        if [ -f "$dir/requires.no-leak-check" ]; then
            hook_asan="${ASAN_OPTIONS:+$ASAN_OPTIONS:}detect_leaks=0"
        fi
        TUR="$TUR" CC="$BUILD_CC" TUR_CC_FLAGS="$TUR_CC_FLAGS" ASAN_OPTIONS="$hook_asan" \
            bash "$dir/hook.sh" "$hook_tmp" > "$actual_hook_stdout" 2> "$hook_log"
        local hook_rc=$?
        if [ $hook_rc -ne 0 ]; then
            { echo "FAIL $name -- hook.sh exited $hook_rc"; cat "$hook_log"; } > "$hook_log.final"
            write_result "FAIL" "$name" "hook.sh failed (exit $hook_rc)" "$hook_log.final"
            rm -rf "$hook_tmp"
            return
        fi
        if [ -f "$dir/expected.stdout" ]; then
            if ! diff -u "$dir/expected.stdout" "$actual_hook_stdout" > /dev/null; then
                { echo "FAIL $name -- stdout mismatch"; diff -u "$dir/expected.stdout" "$actual_hook_stdout" | sed 's/^/    /'; } > "$hook_log.final"
                write_result "FAIL" "$name" "stdout mismatch" "$hook_log.final"
                rm -rf "$hook_tmp"
                return
            fi
        fi
        rm -rf "$hook_tmp"
        write_result "PASS" "$name" "" ""
        return
    fi

    # Skip fixtures owned by a dedicated ctest target (e.g. eval-import
    # has its own tur_eval_import test with custom -I/-L flags).
    #
    # Checked BEFORE the input lookup, not after.  A dir owned by another
    # harness legitimately has no input.tur of its own, and the lookup below now
    # FAILS rather than reporting PASS -- so with the order reversed this marker
    # was unreachable for exactly the directories that carry it, and the silent
    # PASS answered for them instead.  17 dirs were in that state.
    if [ -f "$dir/requires.dedicated-runner" ]; then
        write_result "PASS" "$name" "(dedicated-runner-skipped)" ""
        return
    fi

    local input
    if   [ -f "$dir/input.tur" ]; then input="$dir/input.tur"
    elif [ -f "$dir/$(basename "$dir").tur" ]; then input="$dir/$(basename "$dir").tur"
    else no_input_fail "$dir" "$name" "$(basename "$dir").tur"; return; fi

    # T19: Skip fixtures requiring TSan when TSan is not active.
    if [ -f "$dir/requires.tsan" ] && [ "$TUR_TSAN" != "1" ]; then
        write_result "PASS" "$name" "(tsan-skipped)" ""
        return
    fi

    # Full-size stress twin: nightly only (TUR_STRESS=1).
    if [ -f "$dir/requires.stress" ] && [ "$TUR_STRESS" != "1" ]; then
        write_result "PASS" "$name" "(stress-skipped)" ""
        return
    fi

    # proper-tail-calls T2b: a guaranteed tail call needs a compiler that
    # honours musttail (see the TUR_HAS_MUSTTAIL probe above).
    if [ -f "$dir/requires.musttail" ] && [ "$TUR_HAS_MUSTTAIL" != "1" ]; then
        write_result "PASS" "$name" "(musttail-skipped)" ""
        return
    fi

    # Skip fixtures that load from the optional sibling turmeric-spices
    # repo when that directory isn't present. See CLAUDE.md "Optional
    # dependencies" for how to enable.
    if [ -f "$dir/requires.spices" ] && [ ! -d "../turmeric-spices" ]; then
        write_result "PASS" "$name" "(spices-skipped)" ""
        return
    fi

    # Skip fixtures whose inline-C calls a POSIX API that Windows does not have
    # and that is not worth emulating.  The marker file's contents name the API
    # and say why, per fixture -- read it before assuming a fixture is skipped
    # for a reason that still applies.  Currently: pipe() (MinGW ships _pipe,
    # and Windows select() is socket-only so the reactor could not poll a pipe
    # fd even if it compiled) and fork()/getppid().  See
    # docs/archive/windows-pipe-reactor-fixtures-do-not-build.md and
    # docs/archive/windows-posix-inline-c-gaps.md.
    if [ -f "$dir/requires.posix-apis" ] && [ "$TUR_HOST_WINDOWS" = "1" ]; then
        write_result "PASS" "$name" "(posix-apis-skipped)" ""
        return
    fi

    # turi-session-types-plan (Slice B): interpreter-only fixtures whose peer
    # runs as a `tur --interpret` async fiber over the cooperative session
    # runtime.  They are owned by tests/run-turi.sh; the compiled suite skips
    # them (a cooperative async fiber cannot rendezvous on the compiled pthread
    # session channel).  Distinct from requires.interp, which routes through the
    # compiling `tur run` path, not `--interpret`.
    if [ -f "$dir/requires.interp-only" ]; then
        write_result "PASS" "$name" "(interp-only-skipped)" ""
        return
    fi

    # T19: Read per-fixture timeout (default: 10 seconds; 0 = unlimited).
    local fixture_timeout=10
    if [ -f "$dir/expected.timeout" ]; then
        local _t
        _t=$(tr -d '[:space:]' < "$dir/expected.timeout")
        case "$_t" in [0-9]*) fixture_timeout=$_t ;; esac
    fi

    local out_dir="$dir"
    local actual_stdout="$out_dir/actual.stdout"
    local actual_stderr="$out_dir/actual.stderr"
    local actual_c="$out_dir/actual.c"
    local log_file="$RESULTS_DIR/$(printf '%s' "happy-$name" | tr '/ ' '__').log"
    local needs_codegen_check=0
    # Default: compiled. All fixtures run through tur build unless they
    # carry a requires.interp marker (reserved for future interpreter-only
    # tests).  Under TUR_TSAN=1 compiled mode is always forced.
    local needs_compiled=1

    if [ -f "$dir/expected.c" ]; then
        needs_codegen_check=1
    fi

    # requires.interp: override compiled default and use the interpreter.
    # requires.compiled is kept for documentation but is now a no-op.
    if [ -f "$dir/requires.interp" ] && [ "$TUR_TSAN" != "1" ]; then
        needs_compiled=0
    fi

    # requires.no-leak-check: run the compiled binary with LeakSanitizer
    # disabled.  Reserved for fixtures whose program intentionally registers
    # process-lifetime closures (e.g. reactor callbacks) that the caller never
    # frees -- mirroring the interpreter ASAN policy in CLAUDE.md.  The
    # compiler/codegen path itself is still leak-checked (emit-c/build above).
    local run_env=()
    if [ -f "$dir/requires.no-leak-check" ]; then
        run_env=(env "ASAN_OPTIONS=${ASAN_OPTIONS:+$ASAN_OPTIONS:}detect_leaks=0")
    fi

    # Stamp fast-path: skip if input, expected.c, and tur binary are all
    # unchanged since the last passing run.
    if stamp_check "$name" "$input"; then
        write_result "PASS" "$name" "" ""
        return
    fi

    # Read per-fixture compiler flags if present
    local fixture_flags=""
    if [ -f "$dir/flags" ]; then
        fixture_flags=$(cat "$dir/flags")
    fi

    # Read per-fixture run arguments if present (space-separated, one line).
    # For the compiled path these are passed directly to the binary.
    # For the interpreter path they are passed after -- to `tur run`.
    local run_args_arr=()
    if [ -f "$dir/run.args" ]; then
        while IFS= read -r _ra; do
            [ -z "$_ra" ] && continue
            run_args_arr+=("$_ra")
        done < "$dir/run.args"
    fi

    # Codegen phase: wrap in _run_timed.  A compiler infinite loop in emit-c was
    # previously UNTIMED -- it stalled the worker (and the whole suite) forever,
    # because the per-fixture timeout only covered running the compiled binary,
    # never the emit-c/build phases.  A hung emit-c now FAILs on timeout instead.
    if [ "$TUR_EMIT_C_MODE" = "always" ] || [ "$needs_codegen_check" -eq 1 ]; then
        _run_timed "$fixture_timeout" "$TUR" $fixture_flags emit-c "$input" > "$actual_c" 2> "$out_dir/actual.stderr"
        _emit_rc=$?
        note_sanitizer "$out_dir/actual.stderr" "$name" "emit-c"
        if [ $_emit_rc -ne 0 ]; then
            {
                if [ $_emit_rc -eq 124 ]; then
                    echo "FAIL $name — tur emit-c timed out (>${fixture_timeout}s)"
                else
                    echo "FAIL $name — tur emit-c failed"
                fi
                cat "$out_dir/actual.stderr"
            } > "$log_file"
            write_result "FAIL" "$name" "emit-c failed" "$log_file"
            return
        fi
    fi

    local rc
    if [ "$needs_compiled" -eq 1 ]; then
        # Compiled path: build a native binary and run it.
        # Spawns cc + a new executable; triggers syspolicyd on macOS.
        local exe
        exe=$(mktemp -t tur-test-XXXXXX)
        # Build phase (compiler frontend + cc/ccache): also wrap in _run_timed.
        # This was untimed too, so a hung codegen or a wedged C compiler stalled
        # the suite indefinitely.  A timeout here is now a FAIL, not a hang.
        CC="$BUILD_CC" _run_timed "$fixture_timeout" "$TUR" $fixture_flags build "$input" -o "$exe" 2> "$out_dir/actual.stderr"
        _build_rc=$?
        note_sanitizer "$out_dir/actual.stderr" "$name" "build"
        if [ $_build_rc -ne 0 ]; then
            {
                if [ $_build_rc -eq 124 ]; then
                    echo "FAIL $name — tur build timed out (>${fixture_timeout}s)"
                else
                    echo "FAIL $name — tur build failed"
                fi
                cat "$out_dir/actual.stderr"
            } > "$log_file"
            write_result "FAIL" "$name" "build failed" "$log_file"
            rm -f "$exe"
            return
        fi
        if [ -f "$dir/input.stdin" ]; then
            _run_timed "$fixture_timeout" "${run_env[@]}" "$exe" "${run_args_arr[@]}" < "$dir/input.stdin" > "$actual_stdout" 2>> "$actual_stderr"
        else
            _run_timed "$fixture_timeout" "${run_env[@]}" "$exe" "${run_args_arr[@]}" > "$actual_stdout" 2>> "$actual_stderr"
        fi
        rc=$?
        rm -f "$exe"
    else
        # Interpreter path: run via `tur run` -- no cc invocation, no new binary,
        # no syspolicyd hit.  This is the default for all fixtures that do not
        # have a requires.compiled marker.
        if [ "${#run_args_arr[@]}" -gt 0 ]; then
            if [ -f "$dir/input.stdin" ]; then
                _run_timed "$fixture_timeout" "$TUR" $fixture_flags run "$input" -- "${run_args_arr[@]}" \
                    < "$dir/input.stdin" > "$actual_stdout" 2> "$actual_stderr"
            else
                _run_timed "$fixture_timeout" "$TUR" $fixture_flags run "$input" -- "${run_args_arr[@]}" \
                    > "$actual_stdout" 2> "$actual_stderr"
            fi
        elif [ -f "$dir/input.stdin" ]; then
            _run_timed "$fixture_timeout" "$TUR" $fixture_flags run "$input" \
                < "$dir/input.stdin" > "$actual_stdout" 2> "$actual_stderr"
        else
            _run_timed "$fixture_timeout" "$TUR" $fixture_flags run "$input" \
                > "$actual_stdout" 2> "$actual_stderr"
        fi
        rc=$?
        note_sanitizer "$actual_stderr" "$name" "run"
    fi

    local expected_exit="0"
    if [ -f "$dir/expected.exit" ]; then
        expected_exit=$(tr -d '[:space:]' < "$dir/expected.exit")
    fi

    # Ratchet: pointer/integer confusion in the EMITTED C.
    #
    # -Wint-conversion / -Wincompatible-pointer-types are the C compiler saying a
    # pointer and an integer were mixed up -- exactly the boundary a language
    # whose ABI carries handles as int64_t gets wrong, and exactly the kind of
    # thing that is a WARNING here and a hard error under -Werror.  cc's output
    # already lands in $actual_stderr (the build phase writes it, the run phase
    # appends), and it used to be read only when the build FAILED; on success it
    # was discarded, so the class could reappear with nothing watching.
    #
    # Checked HERE -- after the build/run, ahead of the timeout and output
    # comparisons -- because the warning is a CAUSE and those are symptoms.
    # Behind the stdout diff it was unreachable for the fixture that needed it
    # most: a canary whose emitted C returns an int as a `const char *` segfaults,
    # so it reported "stdout mismatch" and the real reason never appeared in the
    # log.
    #
    # The corpus was sweep-verified at ZERO before this landed (2563 fixtures,
    # built with these same flags), which is what makes FAIL affordable rather
    # than a warning nobody reads.  See
    # docs/archive/emitted-c-pointer-integer-warnings-unwatched.md.
    #
    # Opt out with TUR_SKIP_CC_WARN_CHECK=1 (e.g. on a toolchain that words these
    # differently, or while landing a change that knowingly trips them).
    # type-confusion-detection-plan F0: -Wfloat-conversion joined the ratchet.
    # A double reaching an int64 slot is the float half of the same class the
    # pointer/integer half already covered, and it is the arm that produces a
    # silent wrong ANSWER rather than a crash -- 7.1 printed as 7, with the
    # fixture otherwise passing.  Swept clean at 0 across the corpus first, per
    # the procedure this ratchet was established with.
    # any-drop-inlining-warns-free-nonheap: -Wfree-nonheap-object joined the
    # ratchet.  Emitted code frees only what a widen boxed, behind a runtime
    # guard; gcc warning that a literal reaches free() means either the guard
    # got inlined away (the case that report fixed) or a real free of a
    # non-heap word, and both belong in a FAIL rather than in a build log.
    # GCC's -Wfloat-conversion is BROADER than clang's: it also covers
    # double -> float, which clang splits out as -Wimplicit-float-conversion.
    # That direction is a precision note, not the representation confusion this
    # ratchet is for -- an int carrier reaching a float slot, or the reverse --
    # and the emitter used to produce one benign instance of it (a float
    # literal closing a multi-expression `: float32` body got a `double` temp;
    # docs/archive/float32-block-temp-widens-to-double.md, fixed 2026-09-16 --
    # the exclusion is kept because it was never measured to be the ONLY
    # double->float site GCC reports across the corpus).  Failing the suite
    # on a legitimate narrowing is the "ratchet that fires on legitimate
    # narrowing is worse than none" risk this was landed with, so the
    # destination-is-float direction is excluded here rather than the flag being
    # dropped: gcc's extra coverage still shows up in the build log.
    #
    # The exclusion spans both compilers' quoting -- clang writes `to 'float'`
    # and gcc `to <U+2018>float<U+2019>` -- hence `.{1,3}` rather than a literal
    # quote.
    ccwarn_pat='\[-W(int-conversion|incompatible-pointer-types|free-nonheap-object|float-conversion)\]'
    ccwarn_skip='to .{1,3}(float|double).'
    if [ "${TUR_SKIP_CC_WARN_CHECK:-0}" != "1" ] && [ -s "$actual_stderr" ]; then
        if grep -E "$ccwarn_pat" "$actual_stderr" | grep -qvE "$ccwarn_skip"; then
            {
                echo "FAIL $name -- emitted C confuses scalar representations (pointer/integer/float), or frees a non-heap word"
                grep -E "$ccwarn_pat" "$actual_stderr" | grep -vE "$ccwarn_skip" \
                    | sed 's/^/    /'
                echo "    This is a warning to cc and a hard error under -Werror."
                echo "    Opt out for one run with TUR_SKIP_CC_WARN_CHECK=1."
            } > "$log_file"
            write_result "FAIL" "$name" "emitted C representation warning" "$log_file"
            return
        fi
        # Ratchet: function-pointer type confusion at a closure dispatch.
        #
        # clang's -fsanitize=function (part of -fsanitize=undefined since
        # clang 17, so any clang UBSan run of the suite carries it; GCC has no
        # equivalent) prints this when an indirect call goes through a pointer
        # whose type disagrees with the callee's definition -- benign on
        # SysV/AAPCS for same-slot returns, a hard fault under CFI / CET-BTI /
        # WASM call_indirect.  UBSan default is print-and-continue, so the
        # fixture PASSES while emitting it and the summary line never shows
        # the class -- which is how the reactor callback typedefs drifted
        # twice.  The corpus was sweep-verified at ZERO under clang before
        # this landed.  See
        # docs/archive/emitter-thunk-type-return-mismatch.md.
        if grep -q 'through pointer to incorrect function type' "$actual_stderr"; then
            {
                echo "FAIL $name -- indirect call through mismatched function-pointer type"
                grep 'through pointer to incorrect function type' \
                    "$actual_stderr" | sed 's/^/    /'
                echo "    UBSan continues past this, so output may still match; the type"
                echo "    confusion is the failure.  Opt out with TUR_SKIP_CC_WARN_CHECK=1."
            } > "$log_file"
            write_result "FAIL" "$name" "mismatched function-pointer dispatch" "$log_file"
            return
        fi
    fi

    # Report a run-phase timeout AS a timeout.  The emit-c and build phases
    # above already special-case rc 124; the run phase did not, so a killed
    # binary's partial stdout fell through to the diff below and was reported
    # as "stdout mismatch" -- pointing whoever reads the log at a wrong answer
    # that does not exist.  This check must stay ahead of the stdout diff.  See
    # docs/archive/ci-cps-tramp-turi-timeouts-under-load.md.
    if [ "$rc" -eq 124 ] && [ "$expected_exit" != "124" ]; then
        {
            echo "FAIL $name — timed out (>${fixture_timeout}s)"
        } > "$log_file"
        write_result "FAIL" "$name" "timed out (>${fixture_timeout}s)" "$log_file"
        return
    fi

    # expected.xfail: a NAMED FAILING TEST.  expected.stdout holds the answer
    # the language spec requires and the marker (whose contents say why) says
    # the implementation does not produce it yet -- a stdout mismatch is the
    # expected outcome and PASSES as "(xfail)"; a match is a FAIL that says to
    # delete the marker, because the gap it recorded has closed.  Only the
    # stdout diff is excused: a build failure, a crash or a timeout on such a
    # fixture is still a failure, so a regression cannot hide behind the
    # marker.  First use: r7rs-lang-plan D5's referential-transparency gap
    # (tests/fixtures/r7rs-syntax-rules-referential-transparency).  Never
    # stamped, so it re-runs every time.
    if [ -f "$dir/expected.stdout" ]; then
        if ! diff -u "$dir/expected.stdout" "$actual_stdout" > /dev/null; then
            if [ -f "$dir/expected.xfail" ]; then
                write_result "PASS" "$name" "(xfail: still fails, as expected.xfail says)" ""
                return
            fi
            {
                echo "FAIL $name — stdout mismatch"
                diff -u "$dir/expected.stdout" "$actual_stdout" | sed 's/^/    /'
            } > "$log_file"
            write_result "FAIL" "$name" "stdout mismatch" "$log_file"
            return
        elif [ -f "$dir/expected.xfail" ]; then
            {
                echo "FAIL $name — expected to fail (expected.xfail) but its stdout now matches expected.stdout"
                echo "    The gap the marker records has closed: delete expected.xfail and keep the fixture."
                sed 's/^/    marker: /' "$dir/expected.xfail"
            } > "$log_file"
            write_result "FAIL" "$name" "unexpectedly passed -- delete expected.xfail" "$log_file"
            return
        fi
    fi

    if [ "$needs_codegen_check" -eq 1 ]; then
        if ! diff -u "$dir/expected.c" "$actual_c" > /dev/null; then
            {
                echo "FAIL $name — codegen mismatch"
                diff -u "$dir/expected.c" "$actual_c" | sed 's/^/    /'
            } > "$log_file"
            write_result "FAIL" "$name" "codegen mismatch" "$log_file"
            return
        fi
    fi

    if [ "$expected_exit" = "nonzero" ]; then
        if [ "$rc" -eq 0 ]; then
            {
                echo "FAIL $name — expected nonzero exit, got 0"
            } > "$log_file"
            write_result "FAIL" "$name" "expected nonzero exit" "$log_file"
            return
        fi
    else
        if [ "$rc" -ne "$expected_exit" ]; then
            {
                echo "FAIL $name — program exited $rc (expected $expected_exit)"
            } > "$log_file"
            write_result "FAIL" "$name" "exit $rc, expected $expected_exit" "$log_file"
            return
        fi
    fi

    if [ -f "$dir/expected.stderr" ]; then
        local missing=0
        while IFS= read -r needle; do
            [ -z "$needle" ] && continue
            if ! grep -F -q "$needle" "$actual_stderr"; then
                {
                    echo "FAIL $name — expected stderr substring not found:"
                    echo "    $needle"
                } >> "$log_file"
                missing=1
            fi
        done < "$dir/expected.stderr"
        if [ $missing -ne 0 ]; then
            {
                echo "    actual stderr:"
                sed 's/^/      /' "$actual_stderr"
            } >> "$log_file"
            write_result "FAIL" "$name" "stderr mismatch" "$log_file"
            return
        fi
    fi

    stamp_write "$name" "$input"
    write_result "PASS" "$name" "" ""
}

run_negative() {
    local dir="$1"
    local name="${dir#tests/fixtures/}"
    # Same order as the happy path: a dir owned by another harness declares it
    # with the marker, and anything else with no input.tur is a failure, not a
    # silent PASS.
    if [ -f "$dir/requires.dedicated-runner" ]; then
        write_result "PASS" "$name" "(dedicated-runner-skipped)" ""
        return
    fi
    local input="$dir/input.tur"
    [ -f "$input" ] || { no_input_fail "$dir" "$name" "input.tur"; return; }

    # Skip negative fixtures that load from the optional sibling turmeric-spices
    # repo when that directory isn't present (mirrors the happy-path guard above).
    # Without this, a spices-dependent error fixture fails for any contributor
    # who has not cloned the sibling repo -- the diagnostic differs because the
    # spice import never resolves. See CLAUDE.md "Optional dependencies".
    if [ -f "$dir/requires.spices" ] && [ ! -d "../turmeric-spices" ]; then
        write_result "PASS" "$name" "(spices-skipped)" ""
        return
    fi

    # POSIX-API skip (mirrors the happy-path guard above).
    if [ -f "$dir/requires.posix-apis" ] && [ "$TUR_HOST_WINDOWS" = "1" ]; then
        write_result "PASS" "$name" "(posix-apis-skipped)" ""
        return
    fi

    # Interpreter-only skip (mirrors the happy-path guard above).  A negative
    # fixture asserting a `--interpret` diagnostic is owned by
    # tests/run-turi.sh's run_turi_error_fixture; the compiled path may reject
    # the same program for an unrelated reason, so its expected.diag is not a
    # claim about this suite.
    if [ -f "$dir/requires.interp-only" ]; then
        write_result "PASS" "$name" "(interp-only-skipped)" ""
        return
    fi

    # Per-fixture timeout (default 10s) -- negative fixtures only emit-c, but an
    # untimed front-end hang stalled the suite just like the happy path did.
    local fixture_timeout=10
    if [ -f "$dir/expected.timeout" ]; then
        local _nt; _nt=$(tr -d '[:space:]' < "$dir/expected.timeout")
        case "$_nt" in [0-9]*) fixture_timeout=$_nt ;; esac
    fi

    local log_file="$RESULTS_DIR/$(printf '%s' "neg-$name" | tr '/ ' '__').log"

    # Stamp fast-path: skip if input + tur binary unchanged since last PASS.
    if stamp_check "$name" "$input"; then
        write_result "PASS" "$name" "" ""
        return
    fi

    local neg_flags=""
    if [ -f "$dir/flags" ]; then
        neg_flags=$(cat "$dir/flags")
    fi
    _run_timed "$fixture_timeout" $TUR $neg_flags emit-c "$input" > /dev/null 2> "$dir/actual.stderr"
    local rc=$?
    if [ $rc -eq 124 ]; then
        {
            echo "FAIL $name — tur emit-c timed out (>${fixture_timeout}s)"
        } > "$log_file"
        write_result "FAIL" "$name" "emit-c timed out" "$log_file"
        return
    fi
    if [ $rc -eq 0 ]; then
        {
            echo "FAIL $name — expected error, but tur exited 0"
        } > "$log_file"
        write_result "FAIL" "$name" "expected error, got success" "$log_file"
        return
    fi

    if [ -f "$dir/expected.diag" ]; then
        local missing=0
        while IFS= read -r needle; do
            [ -z "$needle" ] && continue
            if ! grep -F -q "$needle" "$dir/actual.stderr"; then
                {
                    echo "FAIL $name — expected diagnostic substring not found:"
                    echo "    $needle"
                } >> "$log_file"
                missing=1
            fi
        done < "$dir/expected.diag"
        if [ $missing -ne 0 ]; then
            {
                echo "    actual stderr:"
                sed 's/^/      /' "$dir/actual.stderr"
            } >> "$log_file"
            write_result "FAIL" "$name" "diagnostic mismatch" "$log_file"
            return
        fi
    fi

    stamp_write "$name" "$input"
    write_result "PASS" "$name" "" ""
}

run_happy_worker() {
    run_happy "$1"
}

run_negative_worker() {
    run_negative "$1"
}

export TUR BUILD_CC RESULTS_DIR TUR_EMIT_C_MODE
export TUR_TEST_FILTER TUR_TEST_EXCLUDE
export TUR_TEST_SHARD SHARD_INDEX SHARD_TOTAL
export TUR_FORCE TUR_STAMP_CACHE
export TUR_TSAN TUR_STRESS _tur_timeout_bin TUR_MTIME TUR_STDLIB_HASH TUR_CONFIG_HASH
export -f matches_filter matches_shard write_result no_input_fail run_happy run_negative run_happy_worker run_negative_worker
export -f note_sanitizer
export -f _tur_hash_file _tur_mtime stamp_key stamp_check stamp_write _run_timed

# Happy fixtures: tests/fixtures/* except tests/fixtures/errors, PLUS the
# fixtures one level down inside a GROUP directory.
#
# A group dir (typed/, typed-slots/, recursive-types/, ...) holds fixtures
# rather than being one -- it has no input.tur of its own, so the top-level
# scan skipped both it and its children, and those children were compiled by
# NO harness (run-turi.sh scans this deep, but only interprets).  That is
# exactly where two latent miscompiles sat undisturbed until the J3 jit
# harness compiled them: docs/archive/history/typed-result-map-cps-clone-struct-assign.md
# and docs/archive/history/typed-slots-nested-specialization-float-garbage.md.  Both
# are fixed, so this scan now covers them and the class cannot re-hide.
#
# Detection is structural, not a hard-coded list, and deliberately STRICT: a
# dir is a group only when it holds NOTHING BUT subdirectories (no regular
# file of its own -- no input.tur, expected.*, build.tur, hook.sh, marker, or
# loose *.tur) AND at least one of those subdirectories carries an input.tur.
#
# Both halves are load-bearing.  A project fixture driven by build.tur/hook.sh
# rather than input.tur (workspace-ls2/, spice-resolver-ok/, reader-macros-*)
# has regular files, so the first half keeps it a fixture.  A project fixture
# whose only entry is a source dir (module-transitive-imports/src/) passes the
# first half but fails the second, because src/ has no input.tur.  A looser
# rule silently DROPPED ~34 such fixtures from the suite while still reporting
# 0 failed -- the same invisible-coverage-loss this whole change exists to
# close, so the set inclusion is asserted below rather than assumed.
shopt -s nullglob
FIXTURE_DIRS=()
for d in tests/fixtures/*/; do
    d="${d%/}"
    [ "$d" = "tests/fixtures/errors" ] && continue
    [ -d "$d" ] || continue
    # "holds no regular file of its own" via a plain glob -- BSD/macOS find has
    # no portable `-print -quit`, and this file is otherwise careful to stay
    # macOS-clean (stat -f first, sysctl for core count, gtimeout fallback).
    _is_group=0
    _has_own_file=0
    for _f in "$d"/*; do
        [ -f "$_f" ] && { _has_own_file=1; break; }
    done
    if [ "$_has_own_file" = 0 ]; then
        for sub in "$d"/*/; do
            [ -f "${sub}input.tur" ] && { _is_group=1; break; }
        done
    fi
    if [ "$_is_group" = 0 ]; then
        FIXTURE_DIRS+=("$d")            # a fixture in its own right
    else
        for sub in "$d"/*/; do          # a group dir: take its children
            sub="${sub%/}"
            [ -d "$sub" ] || continue
            [ -f "$sub/input.tur" ] || [ -f "$sub/$(basename "$sub").tur" ] || continue
            FIXTURE_DIRS+=("$sub")
        done
    fi
done

HAPPY_DIRS=()
fixture_ordinal=0
for d in "${FIXTURE_DIRS[@]}"; do
    name="${d#tests/fixtures/}"
    if suite_admits happy "$d" && matches_filter "$name" && matches_shard "$fixture_ordinal"; then
        HAPPY_DIRS+=("$d")
    fi
    fixture_ordinal=$((fixture_ordinal + 1))
done

# r7rs-prelude-library-cold-compile: the first `#lang r7rs` build on an empty
# cache compiles the whole stdlib library unit (8-15 s), and the builds that
# start alongside it wait on its lock with their own timers running.  On a
# fresh CI runner that is every run, so whichever r7rs fixture a shard reached
# first failed `tur build timed out (>10s)` -- a different one whenever adding
# fixtures reshuffled shard membership (r7rs-keyword-seed on Windows 2/3).
# Pay it once here, untimed, with the compiler and environment the fixtures
# build with.  Since 2026-10-07 every r7rs fixture that does not import
# `(scheme eval)` links this one object
# (docs/reported/r7rs-prelude-library-object-varies-with-the-program.md).  An
# `eval` program links the sanitized libturi, whose flags are part of the cache
# key, so the first of those still compiles a second object, inside its own
# timed build.
_r7rs_warm=0
for d in "${HAPPY_DIRS[@]}"; do
    _in="$d/input.tur"
    [ -f "$_in" ] || _in="$d/$(basename "$d").tur"
    [ -f "$_in" ] || continue
    _first=""
    IFS= read -r _first < "$_in" || true
    case "$_first" in "#lang r7rs"*) _r7rs_warm=1; break ;; esac
done
if [ "$_r7rs_warm" = 1 ]; then
    printf '#lang r7rs\n(import (scheme base) (scheme write))\n(display (map (lambda (x) (+ x 1)) (list 1 2)))\n(newline)\n' \
        > "$RESULTS_DIR/r7rs-warm.tur"
    CC="$BUILD_CC" "$TUR" build "$RESULTS_DIR/r7rs-warm.tur" \
        -o "$RESULTS_DIR/r7rs-warm.exe" > /dev/null 2>&1 || true
    rm -f "$RESULTS_DIR/r7rs-warm.exe" "$RESULTS_DIR/r7rs-warm.tur"
fi

# macos-sanitized-libturi-fixture-builds-hit-10s-cap: a program that imports a
# module autolinking `-lturi` (arc, httpd, reactor, turi/eval, r7rs/eval) links
# the Debug libturi.a, so the driver compiles it with
# -fsanitize=address,undefined too.  The FIRST such build of a run pays a cold
# cost (the 110 MB archive and the sanitizer runtimes into the cache, ~2x a warm
# build on Linux), and on a loaded macOS runner whichever of these fixtures came
# first -- arc-basic and arc-weak-upgrade, alphabetically -- timed out at the
# 10 s build cap.  Pay it once here, untimed, like the r7rs warm-up above, so
# every one of them builds warm.  Only when the run reaches such a fixture.
_turi_link_re='\(import (arc|httpd|reactor|turi/eval|r7rs/eval)[ )]|stdlib/(arc|httpd|reactor|turi/eval|r7rs/eval)\.tur|\(scheme eval\)'
_turi_warm=0
for d in "${HAPPY_DIRS[@]}"; do
    if grep -rqE --include='*.tur' "$_turi_link_re" "$d" 2>/dev/null; then
        _turi_warm=1; break
    fi
done
if [ "$_turi_warm" = 1 ]; then
    printf '(defmodule turi-warm\n  (import arc :refer [arc-new arc-drop])\n  (defn main [] : int (arc-drop (arc-new 1)) 0))\n' \
        > "$RESULTS_DIR/turi-warm.tur"
    CC="$BUILD_CC" "$TUR" build "$RESULTS_DIR/turi-warm.tur" \
        -o "$RESULTS_DIR/turi-warm.exe" > /dev/null 2>&1 || true
    rm -f "$RESULTS_DIR/turi-warm.exe" "$RESULTS_DIR/turi-warm.tur"
fi

HAPPY_XARGS_RC=0
if [ ${#HAPPY_DIRS[@]} -gt 0 ]; then
    HAPPY_LIST_FILE="$RESULTS_DIR/happy_dirs.list"
    printf '%s\n' "${HAPPY_DIRS[@]}" > "$HAPPY_LIST_FILE"
    # Capture xargs' exit status.  Workers always exit 0 (test failures are
    # recorded in .result files, not via exit code), so a non-zero rc here means
    # xargs or a worker was killed by a signal -- i.e. the run was interrupted.
    xargs -P "$JOBS" -I{} bash -c 'run_happy_worker "$@"' _ {} < "$HAPPY_LIST_FILE" 2>/dev/null || HAPPY_XARGS_RC=$?
fi

# Error fixtures
ERROR_DIRS=()
error_ordinal=0
for d in tests/fixtures/errors/*/; do
    d="${d%/}"
    [ -d "$d" ] || continue
    name="${d#tests/fixtures/}"
    if suite_admits error "$d" && matches_filter "$name" && matches_shard "$error_ordinal"; then
        ERROR_DIRS+=("$d")
    fi
    error_ordinal=$((error_ordinal + 1))
done

ERROR_XARGS_RC=0
if [ ${#ERROR_DIRS[@]} -gt 0 ]; then
    ERROR_LIST_FILE="$RESULTS_DIR/error_dirs.list"
    printf '%s\n' "${ERROR_DIRS[@]}" > "$ERROR_LIST_FILE"
    xargs -P "$JOBS" -I{} bash -c 'run_negative_worker "$@"' _ {} < "$ERROR_LIST_FILE" 2>/dev/null || ERROR_XARGS_RC=$?
fi

for result_file in "$RESULTS_DIR"/*.result; do
    [ -f "$result_file" ] || continue
    kind="$(sed -n '1p' "$result_file")"
    name="$(sed -n '2p' "$result_file")"
    detail="$(sed -n '3p' "$result_file")"
    log_file="$(sed -n '4p' "$result_file")"

    if [ "$kind" = "PASS" ]; then
        PASS=$((PASS + 1))
        # PASS line already printed by the worker for live progress.
    elif [ "$kind" = "FAIL" ]; then
        FAIL=$((FAIL + 1))
        FAILED+=("$name ($detail)")
        if [ -n "$log_file" ] && [ -f "$log_file" ]; then
            cat "$log_file"
        else
            echo "FAIL $name — $detail"
        fi
    fi
done

# Completeness guard -- the core invariant: a partial run is NEVER a pass.
# Every dispatched fixture writes exactly one .result file (PASS or FAIL, incl.
# every skip path).  If fewer results landed than we dispatched, or if either
# xargs reported a signal-killed worker, or a signal trap fired, then the run
# was cut short and the tally above is partial.  Refuse to print the
# success-looking "summary: N passed, 0 failed" line in that case.
DISPATCHED=$(( ${#HAPPY_DIRS[@]} + ${#ERROR_DIRS[@]} ))
RESULT_COUNT=$(find "$RESULTS_DIR" -maxdepth 1 -name '*.result' 2>/dev/null | wc -l | tr -d '[:space:]')
: "${RESULT_COUNT:=0}"

echo

# Did the compiler change underneath us? See TUR_STAMP_START above. Reported
# before the tallies so it is the first thing read, since it invalidates them.
TUR_STAMP_END="$(ls -ln "$TUR" 2>/dev/null)"
if [ "$TUR_STAMP_START" != "$TUR_STAMP_END" ]; then
    echo "WARNING: $TUR changed while this run was in progress."
    echo "  before: $TUR_STAMP_START"
    echo "  after:  $TUR_STAMP_END"
    echo "  Something rebuilt the compiler mid-run (a concurrent 'cmake --build',"
    echo "  most likely). Fixtures exec this binary directly, so any failure"
    echo "  above -- especially a batch of 'build failed' -- is an artifact of"
    echo "  the swap, not a result. Re-run with nothing else building."
    if [ $FAIL -ne 0 ]; then
        # Same principle as the completeness guard: a run that cannot be
        # trusted is not a pass and not an ordinary failure.
        echo "  $FAIL failure(s) recorded -- THIS IS NOT A VALID RUN."
        exit 2
    fi
fi

if [ "$_INTERRUPTED" -ne 0 ] \
   || [ "$HAPPY_XARGS_RC" -ne 0 ] \
   || [ "$ERROR_XARGS_RC" -ne 0 ] \
   || [ "$RESULT_COUNT" -lt "$DISPATCHED" ]; then
    echo "ABORTED: run did NOT complete -- $RESULT_COUNT of $DISPATCHED fixtures reported."
    echo "  (xargs rc: happy=$HAPPY_XARGS_RC error=$ERROR_XARGS_RC, interrupted=$_INTERRUPTED)"
    echo "  partial tally so far: $PASS passed, $FAIL failed -- THIS IS NOT A PASS."
    if [ $FAIL -ne 0 ]; then
        for f in "${FAILED[@]}"; do echo "  - $f"; done
    fi
    # 2 = incomplete/aborted; distinct from 0 (all passed) and 1 (test failures).
    exit 2
fi

# Sanitizer findings from the compiler itself.  Reported unconditionally so a
# regression in this class is visible, but only fatal under TUR_SANITIZER_GATE=1
# -- see the comment where SANITIZER_LOG is created.
SAN_COUNT=0
if [ -s "$SANITIZER_LOG" ]; then
    SAN_COUNT=$(wc -l < "$SANITIZER_LOG" | tr -d ' ')
    SAN_FIXTURES=$(cut -f1 "$SANITIZER_LOG" | sort -u | wc -l | tr -d ' ')
    echo
    echo "SANITIZER: $SAN_COUNT finding(s) from \`tur\` across $SAN_FIXTURES fixture(s)."
    echo "  These are UBSan/ASan diagnostics from the COMPILER, not from emitted programs."
    # Say which configuration is actually running.  The unconditional "they do
    # not fail the run" was a lie under TUR_SANITIZER_GATE=1 -- the run fails
    # thirty lines later -- and the advice to set the variable is noise to
    # someone who already has.
    if [ "${TUR_SANITIZER_GATE:-0}" = "1" ]; then
        echo "  TUR_SANITIZER_GATE=1: these are FATAL; the run fails below."
    else
        echo "  They do not fail the run (set TUR_SANITIZER_GATE=1 to make them fatal)."
    fi
    cut -f2- "$SANITIZER_LOG" | sed 's/value [0-9][0-9]*/value N/' \
        | sort | uniq -c | sort -rn | head -10 | sed 's/^/    /'
    # $SANITIZER_LOG lives under $RESULTS_DIR, which the EXIT trap deletes -- so
    # printing that path alone leaves a reader (and, once the gate is armed in
    # CI, a red job) pointing at a file that is already gone.  Copy it somewhere
    # durable next to the compiler under test before the trap fires.  Best-effort:
    # an unwritable build dir must not turn a report into a harness failure.
    SAN_LOG_OUT="${TUR_SANITIZER_LOG_OUT:-$(dirname "$TUR")/sanitizer-findings.log}"
    if cp "$SANITIZER_LOG" "$SAN_LOG_OUT" 2>/dev/null; then
        echo "  full log ($SAN_COUNT line(s), fixture<TAB>phase<TAB>finding): $SAN_LOG_OUT"
    else
        echo "  full log could not be saved to $SAN_LOG_OUT; dumping it here:"
        sed 's/^/    /' "$SANITIZER_LOG"
    fi
fi

echo "summary: $PASS passed, $FAIL failed"
if [ $FAIL -ne 0 ]; then
    for f in "${FAILED[@]}"; do echo "  - $f"; done
    exit 1
fi
if [ "${TUR_SANITIZER_GATE:-0}" = "1" ] && [ "$SAN_COUNT" -ne 0 ]; then
    echo "FAILED: $SAN_COUNT sanitizer finding(s) from the compiler (TUR_SANITIZER_GATE=1)."
    exit 1
fi
exit 0
