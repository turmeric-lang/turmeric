#!/usr/bin/env bash
# tests/check-examples.sh -- `tur check` sweep over examples/, ratcheted.
#
# Nothing under tests/ or CMakeLists.txt referenced examples/ before this, so
# nothing compiled them and they rotted: at the time this was added, 9 of 17
# failed `tur check`, including four of the five datalog files the
# datalog-01..05 tutorial series walks through line by line.
#
# This is a RATCHET, not a gate. examples/examples-check-baseline.txt lists the
# files known to fail today, each with its reason. The sweep fails when:
#
#   * a file NOT in the baseline fails       -- a regression, fix it; or
#   * a file IN the baseline now passes      -- good news, drop it from the
#                                               baseline so it is protected.
#
# The second direction is what keeps the list honest: without it a baseline
# silently becomes a list of things nobody ever fixed.
set -uo pipefail
cd "$(dirname "$0")/.."

TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || { echo "check-examples: $TUR not built" >&2; exit 2; }
BASELINE="examples/examples-check-baseline.txt"

known=""
if [ -f "$BASELINE" ]; then
    while IFS= read -r line || [ -n "$line" ]; do
        case "$line" in ''|\#*) continue ;; esac
        known="$known ${line%% *}"
    done < "$BASELINE"
fi
is_known() { case " $known " in *" $1 "*) return 0 ;; *) return 1 ;; esac; }

PASS=0; FAIL=0
regressions=""; fixed=""; sanitized=""; checked_ok=""

# A Debug `tur` carries -fsanitize=address,undefined, and UBSan does not halt on
# error -- it prints one line and the compiler carries on, so a real defect in
# the compiler can pass right through a sweep that only looks at the exit code.
# One did: `(perform ...)` in statement position shares its emit_stmt case with
# EX_HANDLE and read `is_unsafe_marker` out of the wrong union member, which
# surfaced only as "runtime error: load of value 190" on stderr while checking
# examples/snake. Whether that byte happens to be non-zero is uninitialized
# memory, so it cannot be pinned by a fixture -- but it can be caught here, on
# every example, for free. A Release `tur` prints nothing and this is a no-op.
sanitizer_line() {
    "$TUR" check "$1" 2>&1 >/dev/null \
        | grep -m1 -E 'runtime error:|AddressSanitizer|LeakSanitizer' || true
}

while IFS= read -r f; do
    san=$(sanitizer_line "$f")
    if [ -n "$san" ]; then
        sanitized="$sanitized $f"; FAIL=$((FAIL + 1))
        echo "FAIL $f -- tur emitted a sanitizer error while checking it:"
        echo "    $san"
        continue
    fi
    if "$TUR" check "$f" >/dev/null 2>&1; then
        if is_known "$f"; then
            fixed="$fixed $f"; FAIL=$((FAIL + 1))
            echo "FAIL $f -- now passes, but is still listed in $BASELINE (remove it)"
        else
            PASS=$((PASS + 1)); echo "PASS $f"
            checked_ok="$checked_ok $f"
        fi
    else
        if is_known "$f"; then
            PASS=$((PASS + 1)); echo "SKIP $f -- known-failing (see $BASELINE)"
        else
            regressions="$regressions $f"; FAIL=$((FAIL + 1))
            echo "FAIL $f -- tur check failed and it is not in $BASELINE"
            "$TUR" check "$f" 2>&1 | head -6 | sed 's/^/    /'
        fi
    fi
done <<EOF
$(find examples -name '*.tur' ! -name 'build.tur' | sort)
EOF

# ---------------------------------------------------------------------------
# Run phase: every example that checks clean must also RUN to exit 0.
#
# `tur check` passing is not the bar and was twice mistaken for one. The most
# expensive instance: all five examples/datalog files type-checked and every
# one of them segfaulted, because `rvec-get`'s inline C did
# `return (int)vec->data[i];` -- C's `int` is 32 bits, so a 64-bit datum
# pointer came back truncated. Nothing a checker looks at could have caught
# that; running it takes under five seconds for the whole tree.
#
# examples/examples-run-baseline.txt lists examples that cannot run in CI (a
# display, a listener, an interactive input) with a reason each. It is empty
# today, and this ratchet also fails if a listed example starts running, so it
# cannot decay into a list nobody revisits.
# ---------------------------------------------------------------------------
RUN_BASELINE=examples/examples-run-baseline.txt
run_known=""
if [ -f "$RUN_BASELINE" ]; then
    while IFS= read -r line; do
        case "$line" in ''|'#'*) continue ;; esac
        run_known="$run_known ${line%% *}"
    done < "$RUN_BASELINE"
fi
is_run_known() { case " $run_known " in *" $1 "*) return 0 ;; *) return 1 ;; esac; }

for f in $checked_ok; do
    if timeout 60 "$TUR" run "$f" >/dev/null 2>&1; then
        if is_run_known "$f"; then
            FAIL=$((FAIL + 1))
            echo "FAIL $f -- now runs, but is still listed in $RUN_BASELINE (remove it)"
        else
            PASS=$((PASS + 1)); echo "RUN  $f"
        fi
    else
        rc=$?
        if is_run_known "$f"; then
            PASS=$((PASS + 1)); echo "NORUN $f -- known-unrunnable (see $RUN_BASELINE)"
        else
            FAIL=$((FAIL + 1))
            echo "FAIL $f -- checks clean but exited $rc when run"
            timeout 60 "$TUR" run "$f" 2>&1 | tail -6 | sed 's/^/    /'
        fi
    fi
done

# ---------------------------------------------------------------------------
# Interpret phase: examples/examples-interpret.txt lists examples that must
# also run under `tur --interpret` and print exactly what the compiled run
# prints.  The datalog tutorial's examples are there: they ran compiled for
# months while every one of them stopped under the interpreter, which no
# sweep looked at.
# ---------------------------------------------------------------------------
INTERP_LIST=examples/examples-interpret.txt
if [ -f "$INTERP_LIST" ]; then
    while IFS= read -r line || [ -n "$line" ]; do
        case "$line" in ''|'#'*) continue ;; esac
        f="${line%% *}"
        if [ ! -f "$f" ]; then
            FAIL=$((FAIL + 1)); echo "FAIL $f -- listed in $INTERP_LIST but missing"
            continue
        fi
        compiled=$(timeout 60 "$TUR" run "$f" 2>/dev/null)
        interpreted=$(timeout 60 "$TUR" --interpret "$f" 2>&1)
        if [ "$compiled" = "$interpreted" ]; then
            PASS=$((PASS + 1)); echo "INTERP $f"
        else
            FAIL=$((FAIL + 1))
            echo "FAIL $f -- interpreted output differs from the compiled run"
            diff <(printf '%s\n' "$compiled") <(printf '%s\n' "$interpreted") \
                | head -8 | sed 's/^/    /'
        fi
    done < "$INTERP_LIST"
fi

echo ""
echo "examples check: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] && exit 0
[ -n "$regressions" ] && echo "  regressions:$regressions"
[ -n "$fixed"       ] && echo "  now-passing, drop from baseline:$fixed"
exit 1
