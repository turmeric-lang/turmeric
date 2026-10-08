#!/usr/bin/env bash
# tests/run-turi.sh -- interpreter (turi) fixture runner.
#
# Runs EVERY tests/fixtures/ through `tur --interpret` (the tree-walking turi
# interpreter, src/turi/eval.c) instead of compiling each to a native binary.
# Fixtures that require compilation (requires.compiled), whose features the
# interpreter deliberately does not implement (requires.tur-only), or whose
# body contains a user inline-C (```c) block (a permanent TI7 carve-out) are
# PASS-skipped; see the W5-flip block further down for the full skip rules.
#
# TI8 history (turi-parity-post-v1-plan): this harness used to invoke `tur run`,
# which COMPILES and runs a native binary -- so an earlier allowlist never
# actually exercised src/turi/eval.c (the now-resolved blocker report
# docs/archive/history/turi-harness-compiles-instead-of-interpreting.md).  It now
# uses `--interpret`, and the full allowlist -> denylist flip (TI8.b/W5) has
# LANDED: the hand-curated TURI_FIXTURES_DEFAULT is gone and the harness defaults
# to run-everything-minus-markers.  The record of how the ~933-fixture gap was
# driven to zero lives in
# docs/archive/history/turi-harness-flip-reconciliation.md and
# docs/archive/history/turi-interpreter-gap-closure-plan.md.
#
# Usage:
#   bash tests/run-turi.sh                  # run the default turi fixture set
#   TURI_FILTER='borrow' bash tests/run-turi.sh   # run only matching fixtures
#
# Environment:
#   TUR              path to the tur binary (default: ./build/tur)
#   TURI_FILTER      optional additional grep -E pattern to narrow the run
#                    (TUR_TEST_FILTER is accepted as an alias for parity with
#                    tests/run.sh -- see KB-002 in docs/archive/history/known-bugs.md)
#   TUR_TEST_JOBS    parallelism (default: cpu count, capped at 8)
#   TUR_FORCE        set to 1 to skip stamp-cache fast-path
#   TUR_TURI_SHARD   run a round-robin slice, "i/N" 1-based (default: all)
#   TURI_TEST_LIST   set to 1 to print the fixture names this invocation would
#                    run, one per line, and exit without running any

set -u
cd "$(dirname "$0")/.."

# This harness runs every fixture through the tree-walking turi/eval
# interpreter, which intentionally never frees its closures or registered
# natives (they live for the process lifetime). LeakSanitizer (enabled with
# ASan on Linux) would otherwise flag that design choice as a leak. Default to
# detect_leaks=0 to mirror the ctest policy (turi_fixture_tests); opt back in
# with ASAN_OPTIONS=detect_leaks=1 bash tests/run-turi.sh. See
# docs/asan-debug-leaks-plan.md.
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"

TUR="${TUR:-./build/tur}"
LIST_ONLY=0
if [ "${TURI_TEST_LIST:-0}" = "1" ]; then LIST_ONLY=1; fi
# List mode answers a question about the CORPUS -- which fixtures a shard would
# run -- so it must not require a built interpreter.  tests/run-shard-partition.sh
# calls it from the non-JIT `test` job, where $TUR may be absent entirely.
if [ "$LIST_ONLY" = "0" ]; then
    [ -x "$TUR" ] || { echo "run-turi: $TUR not built; run 'just build' first" >&2; exit 2; }
fi

# Optional sharding, spelled and CLAMPED exactly as tests/run.sh and
# tests/run-jit.sh do it: 1-based "i/N", a nonsense index snaps into range
# rather than erroring, and a total of 1 or less means "not sharded".
#
# TUR_TURI_SHARD, not TUR_TEST_SHARD: turi_fixture_tests runs inside the 160-test
# `aux` ctest part, and tools/ci/collect-suite-timings.py tags timing rows from
# the job's ENVIRONMENT rather than per test -- so TUR_TEST_SHARD there would
# label all 160 other suites as replicas of a slice they never ran.  One variable
# per harness keeps every call site correct without a rule to remember; see
# tests/shard_util.py.
TUR_TURI_SHARD="${TUR_TURI_SHARD:-}"
SHARD_INDEX=0
SHARD_TOTAL=1
if [ -n "$TUR_TURI_SHARD" ]; then
    case "$TUR_TURI_SHARD" in
        */*)
            shard_left="${TUR_TURI_SHARD%/*}"
            shard_right="${TUR_TURI_SHARD#*/}"
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

PASS=0
FAIL=0
SKIP=0
FAILED=()

if command -v getconf >/dev/null 2>&1; then
    _nproc="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
elif command -v sysctl >/dev/null 2>&1; then
    _nproc="$(sysctl -n hw.logicalcpu 2>/dev/null || echo 4)"
else
    _nproc=4
fi
JOBS="${TUR_TEST_JOBS:-$_nproc}"
case "$JOBS" in ''|*[!0-9]*) JOBS=4 ;; esac
if [ "$JOBS" -lt 1 ]; then JOBS=1; fi
if [ "$JOBS" -gt 8 ]; then JOBS=8; fi

TUR_FORCE="${TUR_FORCE:-0}"
STAMP_CACHE="tests/.stamp-cache-turi"

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

# Performance optimization: cache the compiler binary modification time once
# at startup so we do not spawn a redundant stat process for every single fixture.
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
    local sf="$STAMP_CACHE/$(printf '%s' "$1" | tr '/ ' '__').stamp"
    [ -f "$sf" ] && [ "$(cat "$sf")" = "$(stamp_key "$2")" ]
}
stamp_write() {
    mkdir -p "$STAMP_CACHE"
    local sf="$STAMP_CACHE/$(printf '%s' "$1" | tr '/ ' '__').stamp"
    stamp_key "$2" > "$sf"
}

RESULTS_DIR="$(mktemp -d -t turi-tests-results-XXXXXX)"
trap 'rm -rf "$RESULTS_DIR"' EXIT

# ---------------------------------------------------------------------------
# W5 flip (turi-interpret-flip-residual / turi-interpreter-gap-closure-plan):
# this harness no longer carries an allowlist.  It runs EVERY tests/fixtures/*
# through `tur --interpret`, minus (a) the requires.{compiled,tur-only,
# dedicated-runner,spices,tsan} marker skips and (b) fixtures whose body
# contains a user inline-C (```c) block -- a permanent TI7 carve-out the
# tree-walking interpreter cannot run (auto-detected, no per-fixture marker).
# tests/fixtures/errors/* are negative fixtures handled by the dedicated
# error-diag pass below, not this positive pass.  The historical allowlist
# (TURI_FIXTURES_DEFAULT) and its O(1) lookup set are gone; the record of how
# the gap was driven to zero lives in
# docs/archive/history/turi-harness-flip-reconciliation.md.
# ---------------------------------------------------------------------------

# TI8.b/W2: true if the fixture body contains a user inline-C (```c) block -- a
# permanent TI7 carve-out the interpreter does not run.
fixture_has_inline_c() {
    local dir="$1"
    local f
    if   [ -f "$dir/input.tur" ]; then f="$dir/input.tur"
    elif [ -f "$dir/$(basename "$dir").tur" ]; then f="$dir/$(basename "$dir").tur"
    else return 1; fi
    grep -q '```c' "$f" 2>/dev/null && return 0
    # The carve-out is about whether the PROGRAM contains inline-C, not whether
    # that inline-C is spelled in the fixture file.  A fixture that reaches it
    # through `(load "stdlib/<mod>.tur")` -- e.g. the rc/weak fixtures, whose
    # inline-C lives in the loaded module -- is just as unrunnable under the
    # tree-walking interpreter, but the own-file grep above says otherwise and
    # the fixture then runs and fails on the first unsupported call.  Follow one
    # level of `load`, which is all any fixture uses.
    #
    # A loaded stdlib module whose EVERY inline-C body has a native override
    # under --interpret is not a carve: its ```c blocks never run there.
    # stdlib/session.tur is one (session-spawn / session-join are
    # turi_eval_register_builtins natives), and it is what lets one session
    # fixture source run under both run.sh and this harness.  Add a module here
    # only when all of its inline-C is native-backed.
    local loaded
    while IFS= read -r loaded; do
        [ -n "$loaded" ] || continue
        [ -f "$loaded" ] || continue
        case "$loaded" in
            stdlib/session.tur) continue ;;
        esac
        grep -q '```c' "$loaded" 2>/dev/null && return 0
    done < <(sed -n 's/.*(load "\([^"]*\)").*/\1/p' "$f" 2>/dev/null)
    return 1
}

# W5 flip: a small set of fixtures carry a ```c block yet DO run correctly under
# --interpret, because their inline-C ops are backed by native overrides
# (wk_register_* in src/main.c) or handled by the simple inline-C evaluator
# (try_exec_simple_inline_c in src/turi/eval.c).  The mechanical inline-C carve
# would skip these and silently drop genuine interpreter coverage, so they are
# kept explicitly and run (checked before the carve).  Keep this list tight: an
# entry belongs here only if it is verified to interpret correctly.
TURI_INLINEC_RUN="
inline-c-binop
map-multiword-struct-key
set-multiword-struct-element
clone-list
clone-option
clone-pair
generic-relay-aggregate-result
tuplen-struct-param-passing
typed/slice-basic
contract-ffi
seq-core-from-list
seq-transform-filter-map
hamt-lowering-basic
inline-c-bool-return-prints
"
while IFS= read -r _fx; do
    _fx="${_fx#"${_fx%%[![:space:]]*}"}"; _fx="${_fx%"${_fx##*[![:space:]]}"}"
    [ -z "$_fx" ] && continue
    case "$_fx" in \#*) continue ;; esac
    eval "export TURI_ICRUN_$(printf '%s' "$_fx" | tr '-' '_' | tr '/' '_')=1"
done <<< "$TURI_INLINEC_RUN"

fixture_inline_c_runs() {
    local key; key="$(printf '%s' "$1" | tr '-' '_' | tr '/' '_')"
    eval "[ \"\${TURI_ICRUN_${key}:-0}\" = \"1\" ]"
}

# aot-compiled-repl-plan C1 (repl-jit-inline-c, beta): inline-C fixtures that
# run under --interpret when the interpreter may JIT-compile the inline-C defns
# it cannot run itself.  On a TUR_JIT build (the default on 64-bit x86-64 and
# arm64) each runs with --enable=repl-jit-inline-c; on a build with no engine
# it stays in the carve-out.  Measured 2026-10-04: every one fails without the
# flag and passes with it, three runs in a row.  An entry belongs here only if
# that holds -- a fixture that passes only by handing interpreter memory to
# compiled code is exactly what C1's value boundary refuses.
TURI_INLINEC_JIT_RUN="
async-sleep
backtrack-depth
backtrack-depth-exceeded
backtrack-interleave
backtrack-memory
backtrack-n-queens
backtrack-once
backtrack-sudoku
closure-slot-ptr-void-param-sinks
condvar-basic
cps-backend-ptr
effect-extern-c-row
effect-row-capability
eqmap-cstr-content
fat-param-direct-call
fat-param-nullary-closure
gc-heap-struct-rc
gde-generic-dict-eq-map
ghe1-bare-method-dispatch
global-def-read-by-lifted-lambda
gmk-map-literal-cstr-key
grid-basic
hamt-iteration
hamt-large
inline-c-multi-include-hoist
inline-c-optional-hoisted-include
inline-c-variadic-definition
io-deprecated-names
jit-inline-c-struct-stmtexpr-slot
letrec-self-in-nested-closure
letrec-self-recursive-carrier-float-return
poly-fat-float-closure-eqmap
poly-fn-typeclass-capturing-closure
rc-free-queue-deep-cascade
region-catch-retires-stranded-generation
rwlock-basic
sealed-opaque-in-module
seq-builders-unfold
seq-core-from-vec
set-bang-releases-old-rc
set-cstr-content
sized-hash-consistency
sized-sz2-buf-basic
stm-tmvar-tchan
typed-state-cell
unsafe-ascribe-captured-var
"
while IFS= read -r _fx; do
    _fx="${_fx#"${_fx%%[![:space:]]*}"}"; _fx="${_fx%"${_fx##*[![:space:]]}"}"
    [ -z "$_fx" ] && continue
    case "$_fx" in \#*) continue ;; esac
    eval "export TURI_ICJIT_$(printf '%s' "$_fx" | tr '-' '_' | tr '/' '_')=1"
done <<< "$TURI_INLINEC_JIT_RUN"

# The same probe tests/turi/repl-jit-inline-c.sh uses.
TURI_HAS_JIT=1
case "$("$TUR" jit /nonexistent-tur-jit-probe.tur 2>&1 || true)" in
    *"carries no JIT"*) TURI_HAS_JIT=0 ;;
esac
export TURI_HAS_JIT

fixture_inline_c_jit_runs() {
    [ "$TURI_HAS_JIT" = 1 ] || return 1
    local key; key="$(printf '%s' "$1" | tr '-' '_' | tr '/' '_')"
    eval "[ \"\${TURI_ICJIT_${key}:-0}\" = \"1\" ]"
}

# ---------------------------------------------------------------------------
# TI8.b/W3 (turi-interpreter-gap-closure-plan): error-fixture coverage under the
# interpreter.  tests/fixtures/errors/* are negative fixtures that must elaborate
# to a specific diagnostic (expected.diag).  run.sh validates them on the
# compiled path; this harness now runs them under `tur --interpret` too and does
# the same substring diag comparison.  All but the few below already emit the
# identical diagnostic (the interpreter shares the elaborator, so move/linearity/
# affine checks etc. match by construction).  The 3 "reporting-stage" divergences
# (unbound-call-head, unknown-helper-load-hint, tce3-map-heterogeneous-val) were
# fixed: an unbound runtime-dispatch call head now reports the compiler's
# "unknown function or operator" diagnostic (+ load-hint), and cmd_eval prints a
# runtime error from main instead of swallowing it.  The serial-shift
# capturability cases (serial-context-{,do-}not-capturable) now emit TUR-E0706
# under --interpret too: ts_capture_and_run rejects an uncapturable context with
# the same diagnostic the compiled path raises.
# This closes the TI0-noted gap that errors/ was skipped wholesale.
#
# Known reporting-stage divergence shape (interpreter emits a strict subset,
# not a wrong diagnostic): the compiled batch elaborator keeps going after a
# hard elaboration error and reports follow-on cascade diagnostics (e.g. an
# "unknown function or operator" at a later reference to a constructor whose
# defdata failed), while the interpreter halts at the first hard error and
# emits only the primary line.  The one fixture that pinned this,
# errors/defdata-malformed-ctor-field-type, was retired when a bare type name
# in a defdata field (`(MkAcc int)`) became legal -- the program it held is no
# longer an error on either path -- so the list is empty today.  An entry that
# names no fixture is a startup error (below), so this cannot go stale again.
# ---------------------------------------------------------------------------
TURI_ERRORS_DENY="
"
while IFS= read -r _ef; do
    _ef="${_ef%%#*}"                                   # strip trailing comment
    _ef="${_ef#"${_ef%%[![:space:]]*}"}"; _ef="${_ef%"${_ef##*[![:space:]]}"}"
    [ -z "$_ef" ] && continue
    # turi-suite-accounting-and-reporting-gaps (4): a denylist entry that names
    # no fixture is dead prose at best and an uncovered divergence at worst --
    # this one matched nothing for weeks after an unrelated commit deleted the
    # fixture.  Refuse to start rather than silently carve out nothing.
    if [ ! -d "tests/fixtures/errors/$_ef" ]; then
        echo "run-turi: TURI_ERRORS_DENY names tests/fixtures/errors/$_ef, which does not exist" >&2
        echo "  (delete the entry, or restore the fixture it documents)" >&2
        exit 2
    fi
    _ek="$(printf '%s' "$_ef" | tr '-' '_')"
    eval "export TURI_ERRDENY_${_ek}=1"
done <<< "$TURI_ERRORS_DENY"

# ---------------------------------------------------------------------------
# turi-suite-accounting-and-reporting-gaps (1, 3): ONE marker-skip rule for
# both passes, and every skip lands in a result file so the tally counts it.
# The positive pass and the error pass used to honour different marker sets
# (the error pass ignored requires.dedicated-runner / requires.tsan and checked
# requires.spices unconditionally), and only the inline-C carve-out wrote a
# result -- 81 marker skips printed SKIP and then fell out of every bucket.
#   requires.compiled         -- needs the compiled path
#   requires.tur-only         -- a feature the interpreter deliberately omits
#   requires.dedicated-runner -- owned by its own ctest target
#   requires.spices           -- needs the sibling ../turmeric-spices checkout
#   requires.tsan             -- TSan-only fixture
#   requires.stress           -- nightly-only full-size twin (TUR_STRESS=1)
# Returns 0 (and has printed + recorded the skip) when the fixture is skipped.
# ---------------------------------------------------------------------------
record_result() {   # record_result <name> <kind>
    echo "$2" > "$RESULTS_DIR/$(printf '%s' "$1" | tr '/ ' '__').result"
}
marker_skip() {     # marker_skip <dir> <name>
    local dir="$1" name="$2" why=""
    if   [ -f "$dir/requires.compiled" ]; then why="requires.compiled"
    elif [ -f "$dir/requires.tur-only" ]; then why="requires.tur-only"
    elif [ -f "$dir/requires.dedicated-runner" ]; then why="requires.dedicated-runner"
    elif [ -f "$dir/requires.spices" ] && [ ! -d "../turmeric-spices" ]; then
        why="requires.spices; sibling checkout absent"
    elif [ -f "$dir/requires.tsan" ] && [ "${TUR_TSAN:-0}" != "1" ]; then why="requires.tsan"
    elif [ -f "$dir/requires.stress" ] && [ "${TUR_STRESS:-0}" != "1" ]; then why="requires.stress"
    fi
    [ -n "$why" ] || return 1
    printf 'SKIP %s (%s)\n' "$name" "$why"
    record_result "$name" "SKIP_MARKER"
    return 0
}

err_in_denyset() {
    local key; key="$(printf '%s' "$1" | tr '-' '_')"
    eval "[ \"\${TURI_ERRDENY_${key}:-0}\" = \"1\" ]"
}

# Run a single tests/fixtures/errors/<name> fixture under --interpret and verify
# every expected.diag line appears (substring) in the interpreter's stderr.
run_turi_error_fixture() {
    local dir="$1"
    local name="${dir#tests/fixtures/}"           # e.g. errors/linear-dropped
    local base="${name#errors/}"
    local rkey; rkey="$(printf '%s' "$name" | tr '/ ' '__')"

    [ -f "$dir/input.tur" ] || return
    marker_skip "$dir" "$name" && return
    if err_in_denyset "$base"; then
        printf 'SKIP %s (errors denylist: interp diag diverges)\n' "$name"
        record_result "$name" "SKIP_MARKER"
        return
    fi
    # The needles: expected.diag, or run.sh's other substring file,
    # expected.stderr (seven fh-*/borrow fixtures use only that one).  An
    # errors/ fixture with neither asserts nothing -- which is the shape of the
    # loose-.tur-files bug -- so it is loud, not silent (accounting gaps item 2).
    local needles=""
    if   [ -s "$dir/expected.diag" ];   then needles="$dir/expected.diag"
    elif [ -s "$dir/expected.stderr" ]; then needles="$dir/expected.stderr"
    else
        echo "FAIL $name -- errors/ fixture has no expected.diag or expected.stderr (asserts nothing)"
        record_result "$name" "FAIL"
        return
    fi

    if stamp_check "$name" "$dir/input.tur"; then
        printf 'PASS %s\n' "$name"
        echo "PASS" > "$RESULTS_DIR/$rkey.result"
        return
    fi

    local flags=""; [ -f "$dir/flags" ] && flags=$(cat "$dir/flags")
    local err="$dir/turi.stderr"
    if command -v timeout >/dev/null 2>&1; then
        timeout 15 "$TUR" $flags --interpret "$dir/input.tur" >/dev/null 2>"$err" || true
    else
        "$TUR" $flags --interpret "$dir/input.tur" >/dev/null 2>"$err" || true
    fi

    local missing=0 needle
    while IFS= read -r needle; do
        [ -z "$needle" ] && continue
        grep -F -q "$needle" "$err" || missing=1
    done < "$needles"

    if [ "$missing" -eq 0 ]; then
        stamp_write "$name" "$dir/input.tur"
        printf 'PASS %s\n' "$name"
        echo "PASS" > "$RESULTS_DIR/$rkey.result"
    else
        echo "FAIL $name -- interpreter diagnostic mismatch"
        echo "FAIL" > "$RESULTS_DIR/$rkey.result"
    fi
}

run_turi_fixture() {
    local dir="$1"
    local name="${dir#tests/fixtures/}"
    local input

    # Locate input first.  A directory with no input.tur / <name>.tur is a
    # container (e.g. tests/fixtures/typed/), not a fixture -- skip it silently.
    if   [ -f "$dir/input.tur" ]; then input="$dir/input.tur"
    elif [ -f "$dir/$(basename "$dir").tur" ]; then input="$dir/$(basename "$dir").tur"
    else return; fi

    # Marker skips (the shared rule above; mirrors run.sh's skip set).
    marker_skip "$dir" "$name" && return

    # W5 flip (turi-interpreter-gap-closure-plan): the only positive-fixture skip
    # left is the *permanent* inline-C carve-out.  User inline-C (a ```c block) is
    # a TI7 carve-out the tree-walking interpreter never runs, auto-detected here
    # so the ~400 inline-C fixtures are skipped without per-fixture markers.  The
    # inline-C fixtures that DO work under turi (via try_exec_simple_inline_c or
    # native overrides, e.g. inline-c-binop / gen-*) are run because their bodies
    # carry the working ops through a native shim rather than a raw ```c block --
    # those few keep a presence in the suite via the dedicated paths.  Note: some
    # inline-C fixtures silently miscompile under the simple inline-C evaluator --
    # a real bug the carve hides; see
    # docs/archive/history/turi-inline-c-silent-miscompiles.md.  Every other fixture is
    # now run for real under --interpret (no allowlist gate).
    local jit_flag=""
    if fixture_has_inline_c "$dir" && ! fixture_inline_c_runs "$name" &&
       fixture_inline_c_jit_runs "$name"; then
        jit_flag="--enable=repl-jit-inline-c"
    elif fixture_has_inline_c "$dir" && ! fixture_inline_c_runs "$name"; then
        printf 'SKIP %s (inline-c carve-out)\n' "$name"
        echo "SKIP_INLINEC" > "$RESULTS_DIR/$(printf '%s' "$name" | tr '/ ' '__').result"
        return
    fi

    # Stamp fast-path.
    if stamp_check "$name" "$input"; then
        printf 'PASS %s\n' "$name"
        echo "PASS" > "$RESULTS_DIR/$(printf '%s' "$name" | tr '/ ' '__').result"
        return
    fi

    local actual_stdout="$dir/turi.stdout"
    local actual_stderr="$dir/turi.stderr"

    # Fixture-specific flags.
    local fixture_flags=""
    [ -f "$dir/flags" ] && fixture_flags=$(cat "$dir/flags")

    # Per-fixture timeout (default 15 s for interpreter -- slightly longer than compiled).
    local fixture_timeout=15
    if [ -f "$dir/expected.timeout" ]; then
        local _t; _t=$(tr -d '[:space:]' < "$dir/expected.timeout")
        case "$_t" in [0-9]*) fixture_timeout=$_t ;; esac
    fi

    # Run via interpreter.
    local rc=0
    if [ -f "$dir/input.stdin" ]; then
        if command -v timeout >/dev/null 2>&1; then
            timeout "$fixture_timeout" "$TUR" $fixture_flags $jit_flag --interpret "$input" \
                < "$dir/input.stdin" > "$actual_stdout" 2> "$actual_stderr" || rc=$?
        else
            "$TUR" $fixture_flags $jit_flag --interpret "$input" \
                < "$dir/input.stdin" > "$actual_stdout" 2> "$actual_stderr" || rc=$?
        fi
    else
        if command -v timeout >/dev/null 2>&1; then
            timeout "$fixture_timeout" "$TUR" $fixture_flags $jit_flag --interpret "$input" \
                > "$actual_stdout" 2> "$actual_stderr" || rc=$?
        else
            "$TUR" $fixture_flags $jit_flag --interpret "$input" \
                > "$actual_stdout" 2> "$actual_stderr" || rc=$?
        fi
    fi

    # Expected exit code.
    local expected_exit="0"
    [ -f "$dir/expected.exit" ] && expected_exit=$(tr -d '[:space:]' < "$dir/expected.exit")

    # Report a timeout AS a timeout.  timeout(1) exits 124 when it kills the
    # child, and the partial stdout that leaves behind would otherwise fall
    # through to the diff below and be reported as "stdout mismatch" -- which
    # sends whoever reads the log looking for a wrong answer that does not
    # exist.  This check must stay ahead of the stdout diff.  See
    # docs/archive/ci-cps-tramp-turi-timeouts-under-load.md, where exactly that
    # misreport cost a triage pass.
    if [ "$rc" -eq 124 ] && [ "$expected_exit" != "124" ]; then
        echo "FAIL $name -- timed out (>${fixture_timeout}s under --interpret)"
        echo "FAIL" > "$RESULTS_DIR/$(printf '%s' "$name" | tr '/ ' '__').result"
        return
    fi

    # Check stdout.  expected.xfail (see tests/run.sh): the mismatch is the
    # expected outcome of a named failing test and passes; a match says the
    # gap closed and the marker must go.
    if [ -f "$dir/expected.stdout" ]; then
        if ! diff -u "$dir/expected.stdout" "$actual_stdout" > /dev/null 2>&1; then
            if [ -f "$dir/expected.xfail" ]; then
                echo "PASS $name (xfail: still fails, as expected.xfail says)"
                echo "PASS" > "$RESULTS_DIR/$(printf '%s' "$name" | tr '/ ' '__').result"
                return
            fi
            echo "FAIL $name -- stdout mismatch"
            diff -u "$dir/expected.stdout" "$actual_stdout" | head -20 | sed 's/^/    /'
            echo "FAIL" > "$RESULTS_DIR/$(printf '%s' "$name" | tr '/ ' '__').result"
            return
        elif [ -f "$dir/expected.xfail" ]; then
            echo "FAIL $name -- expected to fail (expected.xfail) but its stdout now matches; delete the marker"
            echo "FAIL" > "$RESULTS_DIR/$(printf '%s' "$name" | tr '/ ' '__').result"
            return
        fi
    fi

    # Check exit code.
    if [ "$expected_exit" = "nonzero" ]; then
        if [ "$rc" -eq 0 ]; then
            echo "FAIL $name -- expected nonzero exit, got 0"
            echo "FAIL" > "$RESULTS_DIR/$(printf '%s' "$name" | tr '/ ' '__').result"
            return
        fi
    else
        if [ "$rc" -ne "$expected_exit" ]; then
            echo "FAIL $name -- exited $rc (expected $expected_exit)"
            [ -s "$actual_stderr" ] && head -5 "$actual_stderr" | sed 's/^/    stderr: /'
            echo "FAIL" > "$RESULTS_DIR/$(printf '%s' "$name" | tr '/ ' '__').result"
            return
        fi
    fi

    stamp_write "$name" "$input"
    echo "PASS $name"
    echo "PASS" > "$RESULTS_DIR/$(printf '%s' "$name" | tr '/ ' '__').result"
}

export TUR STAMP_CACHE RESULTS_DIR TUR_FORCE TUR_MTIME
export -f run_turi_fixture fixture_inline_c_runs fixture_inline_c_jit_runs fixture_has_inline_c stamp_check stamp_write stamp_key
export -f _tur_hash_file _tur_mtime
export -f run_turi_error_fixture err_in_denyset marker_skip record_result

# Build list of all fixture dirs (top-level and one subdirectory deep).
# tests/fixtures/errors/* are negative fixtures handled by the dedicated
# error-diag pass below, so they are excluded from this positive pass.
shopt -s nullglob
ALL_DIRS=()
for d in tests/fixtures/*/ tests/fixtures/*/*/; do
    d="${d%/}"
    case "$d" in tests/fixtures/errors|tests/fixtures/errors/*) continue ;; esac
    [ -d "$d" ] || continue
    ALL_DIRS+=("$d")
done

# Apply optional additional filter.  Accept TUR_TEST_FILTER as an alias so the
# same filter env var works against both tests/run.sh and tests/run-turi.sh.
# TURI_FILTER wins when both are set.
TURI_FILTER="${TURI_FILTER:-${TUR_TEST_FILTER:-}}"
# The ordinal advances on EVERY discovered fixture, not just admitted ones, so
# shard membership is a property of the corpus rather than of the filter.  A
# filtered shard run is then a subset of the same slice an unfiltered one takes,
# which is what makes TURI_FILTER usable to re-run one shard's failure -- the
# name stays in the slice that ran it.  Same rule as tests/run-jit.sh.
FILTERED_DIRS=()
fixture_ordinal=0
for d in "${ALL_DIRS[@]}"; do
    name="${d#tests/fixtures/}"
    if { [ -z "$TURI_FILTER" ] || printf '%s\n' "$name" | grep -E -q "$TURI_FILTER"; } \
       && matches_shard "$fixture_ordinal"; then
        FILTERED_DIRS+=("$d")
    fi
    fixture_ordinal=$((fixture_ordinal + 1))
done

# TI8.b/W3: error-fixture diag pass (tests/fixtures/errors/*).  Honors the same
# TURI_FILTER so `TURI_FILTER=errors/ ...` narrows to just this pass.
#
# Built here, BEFORE the positive pass runs, rather than between the two passes
# as it used to be: both slices have to be resolved before anything executes for
# TURI_TEST_LIST to answer "what is in shard i/N?" without running a fixture.
ERROR_DIRS=()
error_ordinal=0
for d in tests/fixtures/errors/*/; do
    d="${d%/}"; [ -d "$d" ] || continue
    name="${d#tests/fixtures/}"
    if { [ -z "$TURI_FILTER" ] || printf '%s\n' "$name" | grep -E -q "$TURI_FILTER"; } \
       && matches_shard "$error_ordinal"; then
        ERROR_DIRS+=("$d")
    fi
    error_ordinal=$((error_ordinal + 1))
done

# Names only, one per line, `errors/` kept in the path -- the same shape
# run-jit.sh prints, so tests/run-shard-partition.sh can assert this harness's
# partition through its REAL enumeration rather than a copy that could drift.
if [ "$LIST_ONLY" = "1" ]; then
    for d in "${FILTERED_DIRS[@]+"${FILTERED_DIRS[@]}"}" \
             "${ERROR_DIRS[@]+"${ERROR_DIRS[@]}"}"; do
        echo "${d#tests/fixtures/}"
    done
    exit 0
fi

# The census: every directory the two passes are about to walk that carries an
# input.  The tally below must account for each of these exactly once.  Counted
# from the already-sharded arrays, so a shard's accounting balances against the
# fixtures that shard actually runs rather than against the whole corpus.
DISCOVERED=0
for d in "${FILTERED_DIRS[@]}"; do
    if [ -f "$d/input.tur" ] || [ -f "$d/$(basename "$d").tur" ]; then
        DISCOVERED=$((DISCOVERED + 1))
    fi
done
for d in "${ERROR_DIRS[@]+"${ERROR_DIRS[@]}"}"; do
    [ -f "$d/input.tur" ] && DISCOVERED=$((DISCOVERED + 1))
done

if [ ${#FILTERED_DIRS[@]} -gt 0 ]; then
    printf '%s\n' "${FILTERED_DIRS[@]}" | \
        xargs -P "$JOBS" -I{} bash -c 'run_turi_fixture "$@"' _ {} 2>/dev/null
fi
if [ ${#ERROR_DIRS[@]} -gt 0 ]; then
    printf '%s\n' "${ERROR_DIRS[@]}" | \
        xargs -P "$JOBS" -I{} bash -c 'run_turi_error_fixture "$@"' _ {} 2>/dev/null
fi

# The tests/turi/eval-async-*.sh scripts used to run here too and be folded
# into these counts.  They are their own ctest targets (tur_eval_async_*), so
# that ran each of them twice per CI job and inflated the fixture count with
# seven things that are not fixtures (accounting gaps item 5).

# Tally results.  Every discovered fixture must land in exactly one bucket.
INLINEC_CARVE=0
MARKER_SKIP=0
for rf in "$RESULTS_DIR"/*.result; do
    [ -f "$rf" ] || continue
    kind="$(cat "$rf")"
    name="$(basename "${rf%.result}" | tr '__' '/')"
    if [ "$kind" = "PASS" ]; then
        PASS=$((PASS + 1))
    elif [ "$kind" = "FAIL" ]; then
        FAIL=$((FAIL + 1))
        FAILED+=("$name")
    elif [ "$kind" = "SKIP_INLINEC" ]; then
        SKIP=$((SKIP + 1))
        INLINEC_CARVE=$((INLINEC_CARVE + 1))
    elif [ "$kind" = "SKIP_MARKER" ]; then
        SKIP=$((SKIP + 1))
        MARKER_SKIP=$((MARKER_SKIP + 1))
    fi
done

echo
echo "turi fixture summary: $PASS passed, $FAIL failed, $SKIP skipped of $DISCOVERED discovered"
if [ "$INLINEC_CARVE" -gt 0 ]; then
    echo "  (of which $INLINEC_CARVE inline-c carve-outs -- TI7, never run under turi)"
    # The marker the CI timing ingest (tools/ci/collect-suite-timings.py)
    # understands: this suite permanently skips about a quarter of what it
    # discovers, and "pass with no note" was hiding that (accounting gaps 6).
    echo "TUR_SKIP_PARTIAL: inline-c carve-out ($INLINEC_CARVE fixtures)"
fi
if [ "$MARKER_SKIP" -gt 0 ]; then
    echo "  (and $MARKER_SKIP requires.* marker skips)"
fi
ACCOUNTED=$((PASS + FAIL + SKIP))
if [ "$ACCOUNTED" -ne "$DISCOVERED" ]; then
    # A fixture that printed something (or nothing) and landed in no bucket is
    # exactly the failure mode this census exists to catch -- fail loudly.
    echo "FAIL run-turi accounting: $ACCOUNTED results for $DISCOVERED discovered fixtures"
    exit 1
fi
if [ $FAIL -ne 0 ]; then
    echo "failed:"
    for f in "${FAILED[@]}"; do echo "  - $f"; done
    exit 1
fi
exit 0
