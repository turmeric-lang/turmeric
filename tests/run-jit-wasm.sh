#!/usr/bin/env bash
# tests/run-jit-wasm.sh -- the fixture corpus through the browser JIT module
# (wasm-jit-plan P3): tests/run-jit.sh's twin, with node and
# turmeric-jit.wasm in place of `tur jit`.
#
# Each happy-path fixture runs through tools/wasm-jit/run.js -- the module's
# turi_wasm_jit_run, the call the page makes -- in a fresh node process, and
# lands in one of three outcomes:
#
#   compiled     the right output, from the MIR engine
#   interpreted  the right output, from the interpreter: the engine refused
#                the program (threads, an import it lacks, a c2mir rejection)
#                and the page fell back, as plan 5.7 says it must
#   fail         wrong output, wrong exit status, or a timeout, on whichever
#                engine ran (a threaded inline-C program, for instance, which
#                neither engine on the page can run)
#
# tests/jit-wasm-baseline.txt records every fixture whose outcome is not
# `compiled`, with the reason.  An outcome worse than its baseline line (a
# fixture that compiled now falls back, or one that fell back now fails)
# FAILs the run; one that got better is reported so its line can go.  Rewrite
# the file from an unsharded, unfiltered run with
#   TUR_JIT_WASM_BASELINE_UPDATE=1 bash tests/run-jit-wasm.sh
#
# The `errors/` fixtures are not run: their diagnostics come from the same
# front half as `tur jit`'s, which tests/run-jit.sh already checks.
#
# Environment:
#   TUR_WASM_JIT_MODULE  directory holding turmeric-jit.{js,wasm} (default:
#                  build-wasm/wasm-jit, the tur_wasm_jit target's output).
#                  Without it the whole run SKIPs (exit 0).
#   TUR_TEST_FILTER  regex on the fixture name
#   TUR_TEST_SHARD   "i/N": the i-th of N round-robin slices, as run-jit.sh
#   TUR_TEST_JOBS    parallelism (default: cpu count, capped at 8)
#   TUR_TEST_LIST    1: print the fixtures this invocation would run, exit 0
#   TUR_JIT_WASM_RESULTS  keep per-fixture results in this directory and
#                  reuse them: a fixture with a result there is not run again.
#                  Lets shards that each fit a time budget build one baseline:
#                    for i in 1 2 3; do TUR_JIT_WASM_RESULTS=/tmp/r TUR_TEST_SHARD=$i/3 bash tests/run-jit-wasm.sh; done
#                    TUR_JIT_WASM_RESULTS=/tmp/r TUR_JIT_WASM_BASELINE_UPDATE=1 bash tests/run-jit-wasm.sh
#
# Markers: requires.cc, hook.sh, requires.interp, requires.interp-only,
# requires.dedicated-runner, requires.spices, requires.tsan and
# requires.stress skip a fixture, as in run-jit.sh.  So does a `flags` file,
# and any other source file in the fixture's directory: the page compiles one
# buffer with no command line.

set -u
cd "$(dirname "$0")/.."
export TUR_NO_GLOBAL_SPICES=1

MODULE_DIR="${TUR_WASM_JIT_MODULE:-build-wasm/wasm-jit}"
BASELINE="tests/jit-wasm-baseline.txt"
RUNNER="tools/wasm-jit/run.js"

if [ ! -f "$MODULE_DIR/turmeric-jit.wasm" ]; then
    echo "SKIP run-jit-wasm -- no $MODULE_DIR/turmeric-jit.wasm (build the tur_wasm_jit target, or set TUR_WASM_JIT_MODULE)"
    exit 0
fi
if ! command -v node >/dev/null 2>&1; then
    echo "SKIP run-jit-wasm -- node is not on PATH"
    exit 0
fi

JOBS="${TUR_TEST_JOBS:-$(nproc 2>/dev/null || echo 4)}"
[ "$JOBS" -gt 8 ] && JOBS=8
FILTER="${TUR_TEST_FILTER:-}"
SHARD_INDEX=0
SHARD_TOTAL=1
case "${TUR_TEST_SHARD:-}" in
    */*)
        SHARD_INDEX=$(( ${TUR_TEST_SHARD%/*} - 1 ))
        SHARD_TOTAL=${TUR_TEST_SHARD#*/}
        ;;
esac
UPDATE="${TUR_JIT_WASM_BASELINE_UPDATE:-0}"
if [ "$UPDATE" = "1" ] && { [ "$SHARD_TOTAL" -gt 1 ] || [ -n "$FILTER" ]; }; then
    echo "FAIL run-jit-wasm -- TUR_JIT_WASM_BASELINE_UPDATE=1 needs an unsharded, unfiltered run" >&2
    echo "  (the baseline is rewritten whole, so a partial run would drop every name it skipped)" >&2
    exit 2
fi

if [ -n "${TUR_JIT_WASM_RESULTS:-}" ]; then
    RESULTS_DIR="$TUR_JIT_WASM_RESULTS"
    mkdir -p "$RESULTS_DIR"
    REUSE=1
else
    RESULTS_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tur-jit-wasm.XXXXXX")"
    trap 'rm -rf "$RESULTS_DIR"' EXIT
    REUSE=0
fi
export MODULE_DIR RUNNER RESULTS_DIR REUSE

# One fixture.  Writes "<outcome>\t<reason>" to $RESULTS_DIR/<key>.result and
# prints a PASS/FAIL/SKIP line.
run_wasm_fixture() {
    local dir="$1"
    local name="${dir#tests/fixtures/}"
    local key; key="$(printf '%s' "$name" | tr '/' '%')"
    local out="$RESULTS_DIR/$key"
    local input
    if   [ -f "$dir/input.tur" ]; then input="$dir/input.tur"
    elif [ -f "$dir/$(basename "$dir").tur" ]; then input="$dir/$(basename "$dir").tur"
    else return; fi
    [ -f "$dir/expected.stdout" ] || return
    if [ "$REUSE" = "1" ] && [ -f "$out.result" ]; then return; fi

    local skip=""
    [ -f "$dir/requires.cc" ] && skip="requires.cc"
    [ -f "$dir/hook.sh" ] && skip="hook.sh; owned by run.sh"
    { [ -f "$dir/requires.interp" ] || [ -f "$dir/requires.interp-only" ]; } && skip="interpreter-owned"
    [ -f "$dir/requires.dedicated-runner" ] && skip="requires.dedicated-runner"
    [ -f "$dir/requires.spices" ] && skip="requires.spices"
    [ -f "$dir/requires.tsan" ] && [ "${TUR_TSAN:-0}" != "1" ] && skip="requires.tsan"
    [ -f "$dir/requires.stress" ] && [ "${TUR_STRESS:-0}" != "1" ] && skip="requires.stress"
    # Two kinds the page cannot express: it compiles one buffer with no
    # command line, so a fixture that imports or loads files beside it, or
    # needs tur flags (--enable=, --strict-refine, ...), says nothing here.
    [ -f "$dir/flags" ] && skip="takes tur flags; the page passes none"
    if [ -n "$(find "$dir" -mindepth 1 \( -name '*.tur' -o -name '*.scm' -o -name '*.sld' -o -name '*.tur.sweet' \) ! -path "$input" -print -quit)" ]; then
        skip="imports or loads files beside it; the page compiles one buffer"
    fi
    if [ -n "$skip" ]; then
        printf 'SKIP %s (%s)\n' "$name" "$skip"
        printf 'skip\t%s\n' "$skip" > "$out.result"
        return
    fi

    # The page engine pays c2mir and an interpreter on top of the program:
    # twice run-jit.sh's default, and twice a fixture's own budget.
    local budget=30
    if [ -f "$dir/expected.timeout" ]; then
        local t; t=$(tr -d '[:space:]' < "$dir/expected.timeout")
        case "$t" in [0-9]*) budget=$((t * 2)) ;; esac
    fi
    local args=()
    if [ -f "$dir/run.args" ]; then
        while IFS= read -r a; do [ -n "$a" ] && args+=("$a"); done < "$dir/run.args"
    fi
    local stdin_file=/dev/null
    [ -f "$dir/input.stdin" ] && stdin_file="$dir/input.stdin"

    local rc=0
    timeout "$budget" node "$RUNNER" --module "$MODULE_DIR" --result "$out.json" \
        "$input" ${args[@]+"${args[@]}"} < "$stdin_file" > "$out.stdout" 2> "$out.stderr" || rc=$?

    local engine reason
    engine=$(tail -1 "$out.json" 2>/dev/null | grep -o '"engine":"[a-z]*"' | cut -d'"' -f4)
    reason=$(tail -1 "$out.json" 2>/dev/null | grep -o '"reason":"[^"]*"' | cut -d'"' -f4)
    [ -n "$engine" ] || engine="none"

    local expected_exit=0
    [ -f "$dir/expected.exit" ] && expected_exit=$(tr -d '[:space:]' < "$dir/expected.exit")
    local why=""
    if [ "$rc" -eq 124 ] && [ "$expected_exit" != "124" ]; then
        why="timed out (>${budget}s)"
    elif ! diff -q "$dir/expected.stdout" "$out.stdout" > /dev/null 2>&1; then
        why="stdout mismatch"
    elif [ "$expected_exit" = "nonzero" ]; then
        [ "$rc" -ne 0 ] || why="expected a nonzero exit, got 0"
    elif [ "$rc" -ne "$expected_exit" ]; then
        why="exited $rc (expected $expected_exit)"
    fi

    if [ -n "$why" ]; then
        local detail="$why on the $engine path${reason:+: $reason}"
        # One write per fixture, so parallel jobs do not interleave reports.
        {
            printf 'FAIL %s -- %s\n' "$name" "$detail"
            if [ "$why" = "stdout mismatch" ]; then
                diff -u "$dir/expected.stdout" "$out.stdout" | sed -n '3,12p' | sed 's/^/    /'
            fi
            grep -v '^tur: the page' "$out.stderr" | tail -3 | sed 's/^/    stderr: /'
        } > "$out.report"
        cat "$out.report"
        printf 'fail\t%s\n' "$detail" > "$out.result"
    elif [ "$engine" = "interpreted" ]; then
        printf 'PASS %s (interpreted: %s)\n' "$name" "$reason"
        printf 'interpreted\t%s\n' "$reason" > "$out.result"
    else
        printf 'PASS %s\n' "$name"
        printf 'compiled\t\n' > "$out.result"
    fi
}
export -f run_wasm_fixture

shopt -s nullglob
DIRS=()
ordinal=0
for d in tests/fixtures/*/ tests/fixtures/*/*/; do
    d="${d%/}"
    case "$d" in tests/fixtures/errors|tests/fixtures/errors/*) continue ;; esac
    [ -f "$d/expected.stdout" ] || continue
    name="${d#tests/fixtures/}"
    if { [ -z "$FILTER" ] || grep -E -q "$FILTER" <<< "$name"; } \
       && { [ "$SHARD_TOTAL" -le 1 ] || [ $((ordinal % SHARD_TOTAL)) -eq "$SHARD_INDEX" ]; }; then
        DIRS+=("$d")
    fi
    ordinal=$((ordinal + 1))
done

if [ "${TUR_TEST_LIST:-0}" = "1" ]; then
    for d in "${DIRS[@]}"; do echo "${d#tests/fixtures/}"; done
    exit 0
fi

echo "run-jit-wasm: ${#DIRS[@]} fixture(s) through $MODULE_DIR/turmeric-jit.wasm, $JOBS at a time"
if [ ${#DIRS[@]} -gt 0 ]; then
    printf '%s\n' "${DIRS[@]}" | xargs -P "$JOBS" -I{} bash -c 'run_wasm_fixture "$@"' _ {}
fi

# --- tally and the baseline ratchet -------------------------------------------
rank() { case "$1" in compiled) echo 0 ;; interpreted) echo 1 ;; fail) echo 2 ;; *) echo -1 ;; esac; }
declare -A BASE
if [ -f "$BASELINE" ]; then
    while read -r n o _; do
        case "$n" in ''|'#'*) continue ;; esac
        BASE[$n]="$o"
    done < "$BASELINE"
fi

COMPILED=0 INTERPRETED=0 FAILED=0 SKIPPED=0
NEW=()
BETTER=()
UPDATE_LINES=()
for rf in "$RESULTS_DIR"/*.result; do
    key="$(basename "${rf%.result}")"
    name="$(printf '%s' "$key" | tr '%' '/')"
    IFS=$'\t' read -r outcome reason < "$rf"
    case "$outcome" in
        compiled) COMPILED=$((COMPILED + 1)) ;;
        interpreted) INTERPRETED=$((INTERPRETED + 1)) ;;
        fail) FAILED=$((FAILED + 1)) ;;
        skip) SKIPPED=$((SKIPPED + 1)); continue ;;
    esac
    [ "$outcome" = "compiled" ] || UPDATE_LINES+=("$name $outcome  # $reason")
    allowed="${BASE[$name]:-compiled}"
    if [ "$(rank "$outcome")" -gt "$(rank "$allowed")" ]; then
        NEW+=("$name: $outcome (baseline: $allowed) -- $reason")
    elif [ "$(rank "$outcome")" -lt "$(rank "$allowed")" ]; then
        BETTER+=("$name: $outcome (baseline: $allowed)")
    fi
done

echo
echo "run-jit-wasm: $COMPILED compiled, $INTERPRETED interpreted (fell back), $FAILED failed, $SKIPPED skipped"

if [ "$UPDATE" = "1" ]; then
    {
        echo "# tests/jit-wasm-baseline.txt -- fixtures whose outcome through the browser"
        echo "# JIT module (tests/run-jit-wasm.sh) is not \`compiled\`, and the worst outcome"
        echo "# each is allowed: \`interpreted\` (the engine refused and the interpreter"
        echo "# gave the right output) or \`fail\` (neither engine on the page gives it)."
        echo "# The comment is the reason the last run gave.  A worse outcome than the"
        echo "# line FAILs the run; a better one is reported so the line can go."
        echo "# Regenerate from an unsharded, unfiltered run:"
        echo "#   TUR_JIT_WASM_BASELINE_UPDATE=1 bash tests/run-jit-wasm.sh"
        printf '%s\n' "${UPDATE_LINES[@]+"${UPDATE_LINES[@]}"}" | LC_ALL=C sort
    } > "$BASELINE"
    echo "run-jit-wasm: wrote $BASELINE (${#UPDATE_LINES[@]} line(s))"
    exit 0
fi

if [ ${#BETTER[@]} -gt 0 ]; then
    echo "run-jit-wasm: better than the baseline (remove or relax these lines in $BASELINE):"
    printf '  %s\n' "${BETTER[@]}"
fi
if [ ${#NEW[@]} -gt 0 ]; then
    echo "run-jit-wasm: worse than the baseline:"
    printf '  %s\n' "${NEW[@]}"
    exit 1
fi
exit 0
