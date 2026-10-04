#!/usr/bin/env bash
# run-security-driver.sh -- WP2 of docs/upcoming/security-audit-plan.md.
#
# The compiler driver takes text out of files the project it is compiling
# supplies -- a build.tur manifest's :link-flags, a transitive spice's
# :c-sources, the __tur_autolink__ marker a module's inline C emits, a
# Justfile's backtick assignment -- and used to splice it into a shell command
# string handed to system(), or into a directory path it then wrote to.
#
# Each fixture here is one of those channels carrying text that would run a
# command or escape a directory.  The assertion is always the same shape:
#
#   * a DIAGNOSTIC is printed, and
#   * the file the payload tries to create does not exist.
#
# The second half is the one that matters.  A fixture that merely fails to
# build proves nothing -- a manifest naming a spice that is not there fails to
# build too.  `PWNED` is what separates "rejected" from "ran and then failed".
#
# `good-framework-flag` is the counterweight: a grammar that rejected
# `-framework Cocoa` would break every macOS spice, so the vocabulary has to be
# admitted as well as the injection refused.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TUR="${TUR:-$ROOT/build/tur}"
FIX="$ROOT/tests/fixtures/security-driver"

PASS=0
FAIL=0
pass() { PASS=$((PASS + 1)); echo "PASS $1"; }
fail() { FAIL=$((FAIL + 1)); echo "FAIL $1 -- $2"; }

if [ ! -x "$TUR" ]; then
    echo "tests: $TUR not built; run 'just build' first" >&2
    exit 2
fi

WORK="$(mktemp -d -t tur-secdrv.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

# Leaks in the Debug (ASan) build of tur are out of scope here; this exercises
# the driver's command construction, not compiler-internal allocation.
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"

# --------------------------------------------------------------------------
# reject <fixture> <needle> -- build it, expect a diagnostic matching <needle>
#                             and no PWNED anywhere under the scratch copy.
# --------------------------------------------------------------------------
reject() {
    local name="$1" needle="$2"
    local dir="$WORK/$name"
    rm -rf "$dir"
    cp -R "$FIX/$name" "$dir"

    local out rc
    out="$(cd "$dir" && "$TUR" build . -o "$dir/out" 2>&1)"
    rc=$?

    if [ "$rc" -eq 0 ]; then
        fail "$name-rejected" "tur build exited 0; expected a diagnostic"
    elif ! grep -qF "$needle" <<< "$out"; then
        fail "$name-rejected" "no diagnostic matching '$needle'; got: $out"
    else
        pass "$name-rejected"
    fi

    # The payload's side effect, wherever it would have landed.  Captured into
    # a variable rather than piped into `grep -q`: under `pipefail` a writer
    # that is still producing when grep exits takes SIGPIPE, and the pipeline
    # then reports failure precisely BECAUSE the pattern matched
    # (tests/check-pipefail-grep-q.sh lints for exactly this).
    local hits
    hits="$(find "$dir" "$WORK" -maxdepth 3 -name PWNED 2>/dev/null)"
    if [ -n "$hits" ]; then
        fail "$name-inert" "the payload ran (PWNED exists)"
        find "$WORK" -name PWNED -delete 2>/dev/null
    else
        pass "$name-inert"
    fi
}

# --------------------------------------------------------------------------
# 1-8. Manifest channels: a name, a ref, a subdir, two link vectors, a
#      vendored source path, the build dir, a workspace member.
# --------------------------------------------------------------------------
reject bad-spice-name    "is not a usable directory name"
reject bad-spice-ref     "unusable :ref"
reject bad-spice-subdir  "unusable :subdir"
reject bad-link-flags    ":link-flags entry"
reject bad-link-libs     ":link-libs entry"
reject bad-c-sources     "must stay inside the spice directory"
reject bad-build-dir     ":build-dir"
reject bad-members       ":members entry"
reject bad-cmake-ref     "cmake-dep 'evil'"

# --------------------------------------------------------------------------
# 9. D-1: the __tur_autolink__ marker.  Not a manifest -- a comment the
#    compiler itself emits from an inline-C block, scraped back out of the
#    generated C with strstr.  Any module in the dependency closure can write
#    one, which is why it is checked rather than trusted.
# --------------------------------------------------------------------------
{
    dir="$WORK/autolink-injection"
    rm -rf "$dir"; cp -R "$FIX/autolink-injection" "$dir"
    out="$(cd "$dir" && "$TUR" build evil.tur -o "$dir/out" 2>&1)"
    rc=$?
    if [ "$rc" -eq 0 ]; then
        fail "autolink-rejected" "tur build exited 0; expected a diagnostic"
    elif ! grep -qF "refusing link flag" <<< "$out"; then
        fail "autolink-rejected" "no diagnostic; got: $out"
    else
        pass "autolink-rejected"
    fi
    if [ -e "$dir/PWNED" ]; then
        fail "autolink-inert" "the marker's command ran (PWNED exists)"
    else
        pass "autolink-inert"
    fi
}

# --------------------------------------------------------------------------
# 10. D-2: `tur run --list` must not evaluate a Justfile backtick.  Listing is
#     what shell completion and an editor integration run against a tree you
#     have only opened, so it has to be an inventory command and nothing more.
# --------------------------------------------------------------------------
{
    dir="$WORK/justfile-list"
    rm -rf "$dir"; cp -R "$FIX/justfile-list" "$dir"

    out="$(cd "$dir" && "$TUR" run --list 2>&1)"
    rc=$?
    if [ "$rc" -ne 0 ]; then
        fail "justfile-list-works" "tur run --list exit=$rc: $out"
    elif ! grep -q "quiet" <<< "$out"; then
        fail "justfile-list-works" "listing did not name the recipes: $out"
    else
        pass "justfile-list-works"
    fi
    if [ -e "$dir/PWNED" ]; then
        fail "justfile-list-inert" "--list ran the backtick (PWNED exists)"
    else
        pass "justfile-list-inert"
    fi

    # A recipe that does not mention the variable must not force it either.
    (cd "$dir" && "$TUR" run quiet >/dev/null 2>&1)
    if [ -e "$dir/PWNED" ]; then
        fail "justfile-unused-inert" "an unrelated recipe forced the backtick"
    else
        pass "justfile-unused-inert"
    fi

    # ... and the recipe that DOES read it still gets the command's output.
    out="$(cd "$dir" && "$TUR" run loud 2>&1)"
    rc=$?
    if [ "$rc" -eq 0 ] && grep -q "ran" <<< "$out" && [ -e "$dir/PWNED" ]; then
        pass "justfile-used-forces"
    else
        fail "justfile-used-forces" "exit=$rc out=$out"
    fi
}

# --------------------------------------------------------------------------
# 11. The grammar must admit the vocabulary the tree actually uses.
# --------------------------------------------------------------------------
{
    dir="$WORK/good-framework-flag"
    rm -rf "$dir"; cp -R "$FIX/good-framework-flag" "$dir"
    out="$(cd "$dir" && "$TUR" build . -o "$dir/out" 2>&1)"
    rc=$?
    # The -L names a directory that does not exist, which cc ignores; what must
    # NOT happen is a rejection from the grammar.
    if grep -qF "is not a link token" <<< "$out"; then
        fail "good-flags-admitted" "the grammar rejected a documented flag: $out"
    else
        pass "good-flags-admitted"
    fi
    if [ "$rc" -ne 0 ]; then
        fail "good-flags-builds" "tur build exit=$rc: $out"
    else
        pass "good-flags-builds"
    fi
}

# --------------------------------------------------------------------------
# 11b. ... including the spellings the HOST linker will not take.
#
#     A manifest written for macOS is still parsed on Linux, so the grammar's
#     vocabulary has to be cross-platform even though no single linker accepts
#     all of it.  This asserts the grammar alone -- whether the link succeeds
#     is the linker's business, and on the wrong platform it will not.
# --------------------------------------------------------------------------
{
    dir="$WORK/good-platform-flags"
    rm -rf "$dir"; cp -R "$FIX/good-platform-flags" "$dir"
    out="$(cd "$dir" && "$TUR" build . -o "$dir/out" 2>&1)"
    if grep -qF "is not a link token" <<< "$out"; then
        fail "platform-flags-admitted" "the grammar rejected a documented flag: $out"
    else
        pass "platform-flags-admitted"
    fi
}

# --------------------------------------------------------------------------
# 12. D-1b: a path with a space in it.  This is the correctness half of the
#     same fix -- the driver's own paths were spliced unquoted, so a checkout
#     under a directory with a space could not link at all.
# --------------------------------------------------------------------------
{
    dir="$WORK/My Build Dir"
    mkdir -p "$dir"
    cat > "$dir/ok.tur" <<'EOF'
(defn main [] : int
  (println "spaced")
  0)
EOF
    out="$("$TUR" build "$dir/ok.tur" -o "$dir/my out" 2>&1)"
    rc=$?
    if [ "$rc" -ne 0 ]; then
        fail "spaced-path-builds" "tur build exit=$rc: $out"
    elif [ ! -x "$dir/my out" ]; then
        fail "spaced-path-builds" "no binary at '$dir/my out'"
    else
        ran="$("$dir/my out" 2>&1)"
        if [ "$ran" = "spaced" ]; then
            pass "spaced-path-builds"
        else
            fail "spaced-path-builds" "binary printed '$ran'"
        fi
    fi
}

# --------------------------------------------------------------------------
# 13. A shebang recipe under a TMPDIR with a space or shell metacharacters.
#     The script path comes from $TMPDIR and used to reach system() as shell
#     text: a space gave exit 127, and `;` ran the rest as a command.
# --------------------------------------------------------------------------
{
    dir="$WORK/shebang-tmpdir"
    mkdir -p "$dir"
    tdir="$WORK/my tmp;touch PWNED"  # PWNED lands in cwd ($dir) if it runs
    mkdir -p "$tdir"
    printf 'bang:\n    #!/bin/sh\n    echo shebang-ran\n' > "$dir/Justfile"
    out="$(cd "$dir" && TMPDIR="$tdir" "$TUR" run bang 2>&1)"
    rc=$?
    if [ "$rc" -eq 0 ] && grep -q "shebang-ran" <<< "$out"; then
        pass "shebang-spaced-tmpdir-runs"
    else
        fail "shebang-spaced-tmpdir-runs" "exit=$rc out=$out"
    fi
    # Only this case's dirs: justfile-used-forces leaves a PWNED on purpose.
    hits="$(find "$dir" "$tdir" -name PWNED 2>/dev/null)"
    if [ -n "$hits" ]; then
        fail "shebang-tmpdir-inert" "the TMPDIR text ran as shell (PWNED exists)"
    else
        pass "shebang-tmpdir-inert"
    fi
}

echo
echo "run-security-driver: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
