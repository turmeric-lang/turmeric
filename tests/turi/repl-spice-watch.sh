#!/usr/bin/env bash
# tests/turi/repl-spice-watch.sh -- RP6 integration test.
#
# Scenarios:
#   1. `tur repl --watch` without source changes is functionally
#      identical to plain `tur repl` (no extra output, no reload).
#   2. With --watch, mutating a source file mid-session triggers
#      an auto-reload right before the next eval, so the next call
#      sees the new code.
#   3. Without --watch (default), the same mutation does NOT auto-
#      reload: the next call still returns the old value.
#   4. `--watch` survives `--help` parsing (no false positive).
#
# Scenarios 2/3 drive the REPL through a FIFO so input is fed
# line-by-line; the source mutation lands between the user's two
# inputs while the REPL is blocked in readline.

set -uo pipefail
cd "$(dirname "$0")/../.."

TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || { echo "tests: $TUR not built; run 'tur run build' first" >&2; exit 2; }
export TUR_BIN
case "$TUR" in
    /*) TUR_BIN="$TUR" ;;
    *)  TUR_BIN="$(cd "$(dirname "$TUR")" && pwd)/$(basename "$TUR")" ;;
esac

WORK="$(mktemp -d -t tur-rp6.XXXXXX)"
trap 'rm -rf "$WORK"; rm -f "$WORK.fifo"' EXIT

PASS=0
FAIL=0
pass() { PASS=$((PASS + 1)); echo "PASS $1"; }
fail() { FAIL=$((FAIL + 1)); echo "FAIL $1 -- $2"; }

make_project() {
    local root="$1"
    mkdir -p "$root/src"
    cat > "$root/build.tur" <<'EOF'
(defpackage rp6-fixture)
EOF
}

write_lib() {
    local root="$1"
    local body="$2"
    cat > "$root/src/lib.tur" <<EOF
(defmodule sh
  (export answer)
  (defn answer [] :int ${body}))
EOF
}

# Portable file mtime in whole seconds.  GNU coreutils uses `stat -c %Y`;
# BSD/macOS uses `stat -f %m`.  These flags are not interchangeable (`-f` on
# GNU stat means "filesystem status" and would print garbage), so try GNU
# first and only fall back to BSD when the result is not a pure integer.
# Returns the empty string if neither form yields an integer.
mtime_of() {
    local m
    m="$(stat -c %Y "$1" 2>/dev/null)"
    case "$m" in ''|*[!0-9]*) m="$(stat -f %m "$1" 2>/dev/null)" ;; esac
    case "$m" in ''|*[!0-9]*) m="" ;; esac
    printf '%s' "$m"
}

# Block until $file contains a line matching the grep -E $pat, or $timeout
# seconds elapse.  Returns 0 on match, 1 on timeout.  Replaces fixed sleeps so
# the session advances on observed REPL output rather than wall-clock guesses
# (de-flake plan, Option A -- see docs/archive/history/repl-spice-watch-flake-plan.md).
wait_for() {
    local file="$1" pat="$2" timeout="${3:-30}"
    local ticks=0 max=$((timeout * 10))
    while ! grep -Eq "$pat" "$file" 2>/dev/null; do
        sleep 0.1
        ticks=$((ticks + 1))
        [ "$ticks" -ge "$max" ] && return 1
    done
    return 0
}

# Rewrite src/lib.tur and guarantee its mtime is strictly newer than the
# watcher's last-seen baseline, even on 1s-granularity filesystems (macOS
# APFS).  Without this, an edit landing in the same mtime tick as the original
# is invisible to an mtime-based watcher (de-flake plan, Option C).
write_lib_newer() {
    local root="$1" body="$2"
    local f="$root/src/lib.tur"
    local base; base="$(mtime_of "$f")"
    write_lib "$root" "$body"
    local now; now="$(mtime_of "$f")"
    while [ -n "$base" ] && [ -n "$now" ] && [ "$now" -le "$base" ]; do
        sleep 0.3
        touch "$f"
        now="$(mtime_of "$f")"
    done
}

# Drive a REPL session through a FIFO; mutate the source between the
# two `(answer)` inputs. The function dumps the REPL output to stdout.
run_edit_session() {
    local root="$1"; shift
    local fifo="$WORK.fifo"
    local out="$root/out.log"
    rm -f "$fifo"
    mkfifo "$fifo"
    : > "$out"
    (cd "$root" && "$TUR_BIN" "$@" < "$fifo") >"$out" 2>&1 &
    local pid=$!
    # Opening the FIFO for write blocks until the REPL opens it for read, so
    # input 1 is never written before the REPL is listening.
    exec 3>"$fifo"
    printf '(answer)\n' >&3
    # Wait until the first eval has actually printed a result -- this also
    # guarantees the REPL has started and recorded its watch baseline before
    # we mutate the source.
    wait_for "$out" '=> [0-9]' 60 || true
    write_lib_newer "$root" 99
    printf '(answer)\n' >&3
    printf ':quit\n' >&3
    exec 3>&-
    # The REPL flushes all output (including any rebuild + second result)
    # before it exits on :quit, so the captured log is complete here.
    wait "$pid"
    rm -f "$fifo"
    cat "$out"
}

# -------- scenario 1: --watch with no source changes ---------------------
PROJ1="$WORK/p1"
make_project "$PROJ1"
write_lib "$PROJ1" 42
out=$(cd "$PROJ1" && printf '(answer)\n:quit\n' \
      | "$TUR_BIN" repl --watch 2>&1)
if grep -qx '=> 42' <<< "$out" \
    && ! grep -q 'rebuilt' <<< "$out"; then
    pass "rp6-watch-no-changes"
else
    fail "rp6-watch-no-changes" \
         "expected '=> 42' and no 'rebuilt' line; got: $out"
fi

# -------- scenario 2: --watch auto-rebuilds on source edit ---------------
PROJ2="$WORK/p2"
make_project "$PROJ2"
write_lib "$PROJ2" 42
out=$(run_edit_session "$PROJ2" repl --watch)
if   grep -qx '=> 42' <<< "$out" \
  && grep -q '(reload) rebuilt 1 export' <<< "$out" \
  && grep -qx '=> 99' <<< "$out"; then
    pass "rp6-watch-edit-auto-reload"
else
    fail "rp6-watch-edit-auto-reload" \
         "expected => 42, rebuild line, => 99; got: $out"
fi

# -------- scenario 3: without --watch, same edit is NOT auto-reloaded ----
PROJ3="$WORK/p3"
make_project "$PROJ3"
write_lib "$PROJ3" 42
out=$(run_edit_session "$PROJ3" repl)
# Both (answer) calls should return 42 -- no auto-rebuild fired.
got_42=$(echo "$out" | grep -cx '=> 42')
if [ "$got_42" = "2" ] && ! grep -q 'rebuilt' <<< "$out"; then
    pass "rp6-no-watch-skips-auto-reload"
else
    fail "rp6-no-watch-skips-auto-reload" \
         "expected '=> 42' twice and no rebuild; got: $out"
fi

# -------- scenario 4: --watch doesn't confuse the help dispatch ----------
#
# NOTE: capture the output first, then grep the captured string.  Do NOT
# write these as `"$TUR" ... | grep -q ...`.  `grep -q` exits the instant it
# matches and closes the read end of the pipe; glibc emits an unbuffered
# stderr stream in several write() calls, so `tur` is still writing the rest
# of the usage text and takes SIGPIPE (exit 141).  With `set -o pipefail`
# (line 19) the pipeline then reports 141 even though grep matched, and the
# assertion fails with a message that is a flat lie -- the help output did
# mention 'tur repl'.  Measured at roughly 1 invocation in 300 locally; it is
# the cause of the intermittent CI red on this target, and it flakes the two
# assertions below and no others (they were the only piped ones).
out=$("$TUR" repl --watch --help 2>&1)
if grep -q 'tur repl' <<< "$out"; then
    pass "rp6-watch-with-help"
else
    fail "rp6-watch-with-help" \
         "expected help output to mention 'tur repl'; got: $out"
fi

# Unknown flag still errors.
out=$("$TUR" repl --nope 2>&1)
if ! grep -q "unknown option" <<< "$out"; then
    fail "rp6-watch-unknown-flag" "no 'unknown option' diagnostic; got: $out"
else
    pass "rp6-watch-unknown-flag"
fi

echo
echo "repl-spice-watch: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
