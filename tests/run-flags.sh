#!/usr/bin/env bash
# tests/run-flags.sh — Tests for global flags that can't be expressed as
# standard fixture tests (--explain, --dump-kinds, etc.)
#
# Each test case is a short bash block that prints "PASS <name>" on success
# or "FAIL <name> — <reason>" on failure, then increments the counter.
#
# Exit status: 0 if all tests pass, 1 if any fail.
#
# Phase HKT-P5: tur-explain-kind-mismatch
# Phase HKT-P6: dump-kinds-basic

set -uo pipefail
cd "$(dirname "$0")/.."

# This harness exercises the tree-walking turi/eval interpreter (the eval-*
# cases below), which intentionally never frees its closures or registered
# natives -- they live for the process lifetime. LeakSanitizer (enabled with
# ASan on Linux) would otherwise flag that design choice as a leak. Default to
# detect_leaks=0 to mirror the ctest policy (tur_flags_tests) so a direct
# `bash tests/run-flags.sh` does not surprise contributors; opt back in with
# ASAN_OPTIONS=detect_leaks=1 bash tests/run-flags.sh. See
# docs/asan-debug-leaks-plan.md.
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"

# Overridable so the jit-ffi cases below can be exercised against a
# -DTUR_JIT=ON build (`TUR=./build-jit/tur bash tests/run-flags.sh`),
# matching run-jit.sh's convention.  Default unchanged.
TUR="${TUR:-./build/tur}"

# macos-jit-hang-loses-both-diagnostics: a per-invocation timeout.
#
# This harness carried NONE, while run.sh and run-jit.sh both carry the probe
# below -- and it is in CI's JIT leg precisely because it owns the `jit-ffi-*`
# cases, which drive dynamic FFI, callbacks and threads: the shapes that hang.
# So a hang here had nothing to stop it before the job's `timeout-minutes`
# wall, and that kill skips the remaining steps, taking the ctest log artifact
# with it -- the run is then as blind as if no instrumentation existed.
#
# Prefer timeout, fall back to gtimeout (stock macOS ships neither name by
# default; CI installs coreutils), and run untimed if neither exists -- a hung
# case then hangs the run, which is the pre-existing tradeoff run.sh makes.
# -k adds a SIGKILL after the grace period, because `tur` spawns a C compiler
# and a SIGTERM to the parent alone can leave that child running.
_tur_timeout_bin=""
if command -v timeout >/dev/null 2>&1; then _tur_timeout_bin="timeout"
elif command -v gtimeout >/dev/null 2>&1; then _tur_timeout_bin="gtimeout"; fi

# Per-invocation budget.  Generous: the slowest case here compiles and links a
# fixture, and this is a hang bound, not a performance assertion.
TUR_CASE_TIMEOUT="${TUR_CASE_TIMEOUT:-180}"

# Every case invokes the compiler as `"$TUR" ...`, so ONE indirection bounds
# all ~100 of them -- and bounds the cases added later too, which is the part a
# hand-edited call site is exactly what a new case forgets.  The wrapper runs
# the real binary as argv[0] (resolve_stdlib_root's last-resort path hint), so
# nothing downstream can tell the difference.
_tur_wrap_dir=""
if [ -n "$_tur_timeout_bin" ] && [ -x "$TUR" ]; then
    TUR_REAL="$(cd "$(dirname "$TUR")" && pwd)/$(basename "$TUR")"
    _tur_wrap_dir="$(mktemp -d -t tur-flags-wrap-XXXXXX)"
    cat > "$_tur_wrap_dir/tur" <<WRAP
#!/bin/sh
exec $_tur_timeout_bin -k 5 $TUR_CASE_TIMEOUT "$TUR_REAL" "\$@"
WRAP
    chmod +x "$_tur_wrap_dir/tur"
    TUR="$_tur_wrap_dir/tur"
fi
# One EXIT trap for the whole script: bash keeps only the LAST `trap ... EXIT`,
# so the jit-ffi block below adds its directory to this list rather than
# installing a second trap that would silently drop this one.
TMP_FFI_DIR=""
trap 'rm -rf "$_tur_wrap_dir" "$TMP_FFI_DIR"' EXIT

PASS=0
FAIL=0

pass() { PASS=$((PASS + 1)); echo "PASS $1"; }
fail() { FAIL=$((FAIL + 1)); echo "FAIL $1 — $2"; }

# ---------------------------------------------------------------------------
# HKT-P5 tests
# ---------------------------------------------------------------------------

# tur-explain-kind-mismatch: --explain TUR-E0012 should produce non-empty
# output containing "Kind" and exit 0.
out=$("$TUR" --explain TUR-E0012 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "tur-explain-kind-mismatch" "non-zero exit ($rc)"
elif [ -z "$out" ]; then
    fail "tur-explain-kind-mismatch" "empty output"
elif ! grep -qi "Kind" <<< "$out"; then
    fail "tur-explain-kind-mismatch" "output did not mention 'Kind'"
else
    pass "tur-explain-kind-mismatch"
fi

# tur-explain-orphan-instance: --explain TUR-E0013 should mention "orphan"
out=$("$TUR" --explain TUR-E0013 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "tur-explain-orphan-instance" "non-zero exit ($rc)"
elif ! grep -qi "orphan\|instance" <<< "$out"; then
    fail "tur-explain-orphan-instance" "output did not mention 'orphan/instance'"
else
    pass "tur-explain-orphan-instance"
fi

# tur-explain-all-codes: every known TUR-E code should have an explanation
for code in TUR-E0001 TUR-E0002 TUR-E0003 TUR-E0004 TUR-E0005 \
            TUR-E0007 TUR-E0009 TUR-E0010 TUR-E0011 TUR-E0012 TUR-E0013 TUR-E0021; do
    out=$("$TUR" --explain "$code" 2>&1); rc=$?
    if [ $rc -ne 0 ] || [ -z "$out" ]; then
        fail "tur-explain-${code}" "no explanation registered (exit=$rc)"
    else
        pass "tur-explain-${code}"
    fi
done

# tur-explain-unknown-code: --explain TUR-E9999 should exit non-zero
out=$("$TUR" --explain TUR-E9999 2>&1); rc=$?
if [ $rc -eq 0 ]; then
    fail "tur-explain-unknown-code" "expected non-zero exit, got 0"
else
    pass "tur-explain-unknown-code"
fi

# ---------------------------------------------------------------------------
# HKT-P6 tests
# ---------------------------------------------------------------------------

# dump-kinds-basic: --dump-kinds emit-c on a file with a KIND_ARROW typeclass
# should print "defclass Functor param[0] : * -> *" to stdout.
FIXTURE="tests/fixtures/dump-kinds-basic/input.tur"
out=$("$TUR" --dump-kinds emit-c "$FIXTURE" 2>/dev/null); rc=$?
if [ $rc -ne 0 ]; then
    fail "dump-kinds-basic" "non-zero exit ($rc)"
elif [[ "$out" != *"defclass Functor param[0] : * -> *"* ]]; then
    fail "dump-kinds-basic" "expected 'defclass Functor param[0] : * -> *' in output"
else
    pass "dump-kinds-basic"
fi

# dump-kinds-no-output: without --dump-kinds, the same file should NOT print
# kind annotations mixed into the C output.
out=$("$TUR" emit-c "$FIXTURE" 2>/dev/null); rc=$?
if grep -q "defclass Functor param" <<< "$out"; then
    fail "dump-kinds-no-output" "kind dump appeared without --dump-kinds flag"
else
    pass "dump-kinds-no-output"
fi

# ---------------------------------------------------------------------------
# Phase M3: separate compilation (tur build <dir>)
# ---------------------------------------------------------------------------

# m3-separate-compilation: 'tur build <dir>' with two modules — one library
# exporting `add`, one entry point that calls it — should produce a working
# binary.  We verify by checking the exit code is the expected value.
_m3_dir="$(mktemp -d -t tur-m3-XXXXXX)"
cat > "$_m3_dir/adder.tur" <<'EOF_ADDER'
(defmodule adder
  (export add)
  (defn add [x :int y :int] :int (+ x y)))
EOF_ADDER
cat > "$_m3_dir/myapp.tur" <<'EOF_MYAPP'
(defmodule myapp
  (import adder :as a)
  (defn main [] :int
    (- (a/add 3 4) 7)))
EOF_MYAPP
_m3_exe="$(mktemp -t tur-m3-XXXXXX)"
"$TUR" build "$_m3_dir" -o "$_m3_exe" 2>/tmp/tur-m3-build.err
_m3_build_rc=$?
if [ "$_m3_build_rc" -ne 0 ]; then
    fail "m3-separate-compilation" "build failed (rc=$_m3_build_rc): $(cat /tmp/tur-m3-build.err)"
else
    "$_m3_exe"; _m3_run_rc=$?
    if [ "$_m3_run_rc" -ne 0 ]; then
        fail "m3-separate-compilation" "binary returned unexpected exit code $_m3_run_rc (expected 0)"
    else
        pass "m3-separate-compilation"
    fi
fi
rm -f "$_m3_exe" /tmp/tur-m3-build.err
rm -rf "$_m3_dir"

# m3-cross-module-symbol-mangling: verify that exported symbols get the
# module-prefix mangling in the generated header (adder__add, not just add).
_m3_dir2="$(mktemp -d -t tur-m3b-XXXXXX)"
cat > "$_m3_dir2/adder.tur" <<'EOF_ADDER2'
(defmodule adder
  (export add)
  (defn add [x :int y :int] :int (+ x y)))
EOF_ADDER2
_m3_hbuf="$(mktemp -t tur-m3h-XXXXXX)"
"$TUR" emit-h "$_m3_dir2/adder.tur" > "$_m3_hbuf" 2>/dev/null
if grep -q "adder__add" "$_m3_hbuf"; then
    pass "m3-cross-module-symbol-mangling"
else
    fail "m3-cross-module-symbol-mangling" "expected 'adder__add' in header; got: $(cat "$_m3_hbuf")"
fi
rm -f "$_m3_hbuf"
rm -rf "$_m3_dir2"

# ---------------------------------------------------------------------------
# Formatter round-trip tests (defpackage / deflockfile / comment preservation)
# ---------------------------------------------------------------------------

# fmt-defpackage-basic: a defpackage wide enough to exceed line width should
# format with each :key val pair on its own line.
_fmt_input='(defpackage my-project-with-longer-name :name "my-project-with-longer-name" :version "0.1.0" :description "A test package")'
_fmt_out=$(echo "$_fmt_input" | "$TUR" format 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "fmt-defpackage-basic" "tur format exited $rc: $_fmt_out"
elif ! grep -q ':name' <<< "$_fmt_out"; then
    fail "fmt-defpackage-basic" ":name not present in output: $_fmt_out"
elif ! grep -q '^  :version' <<< "$_fmt_out"; then
    fail "fmt-defpackage-basic" ":version not on its own indented line: $_fmt_out"
else
    pass "fmt-defpackage-basic"
fi

# fmt-defpackage-round-trip: formatting a defpackage twice should be idempotent.
_fmt1=$(printf '(defpackage my-project-with-longer-name\n  :name    "my-project-with-longer-name"\n  :version "0.1.0")' | "$TUR" format 2>&1)
_fmt2=$(echo "$_fmt1" | "$TUR" format 2>&1)
if [ "$_fmt1" != "$_fmt2" ]; then
    fail "fmt-defpackage-round-trip" "formatter not idempotent; first pass != second pass"
else
    pass "fmt-defpackage-round-trip"
fi

# fmt-defpackage-spices-block: a defpackage with multiple spices should expand
# the :spices map to block layout when it doesn't fit on one line.
_fmt_spices_in='(defpackage my-application :name "my-application" :version "0.1.0" :spices #{"geom" #{ :url "https://github.com/alice/tur-geom" :ref "v0.2.1"} "http" #{ :url "https://github.com/alice/tur-http" :ref "v1.0.0"}})'
_fmt_spices_out=$(echo "$_fmt_spices_in" | "$TUR" format 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "fmt-defpackage-spices-block" "tur format exited $rc"
elif ! grep -q '#{$' <<< "$_fmt_spices_out"; then
    fail "fmt-defpackage-spices-block" ":spices map was not expanded to block: $_fmt_spices_out"
else
    pass "fmt-defpackage-spices-block"
fi

# fmt-defpackage-spices-idempotent: the spices block output is itself idempotent.
_fmt_spices1=$(echo "$_fmt_spices_in" | "$TUR" format 2>&1)
_fmt_spices2=$(echo "$_fmt_spices1" | "$TUR" format 2>&1)
if [ "$_fmt_spices1" != "$_fmt_spices2" ]; then
    fail "fmt-defpackage-spices-idempotent" "formatter not idempotent for spices block"
else
    pass "fmt-defpackage-spices-idempotent"
fi

# fmt-defpackage-comments: a defpackage with a leading comment should preserve
# the comment through a format round-trip.
_fmt_cmt_out=$(printf '; This is a comment\n(defpackage my-app\n  :name    "my-app"\n  :version "0.1.0")' | "$TUR" format 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "fmt-defpackage-comments" "tur format exited $rc: $_fmt_cmt_out"
elif ! grep -q 'This is a comment' <<< "$_fmt_cmt_out"; then
    fail "fmt-defpackage-comments" "comment lost after formatting: $_fmt_cmt_out"
else
    pass "fmt-defpackage-comments"
fi

# fmt-deflockfile-round-trip: deflockfile form should be idempotent.
_fmt_lock1=$(printf '(deflockfile\n  :format-version 1\n  :spices         #{}\n  :cmake-deps     #{})' | "$TUR" format 2>&1)
_fmt_lock2=$(echo "$_fmt_lock1" | "$TUR" format 2>&1)
if [ "$_fmt_lock1" != "$_fmt_lock2" ]; then
    fail "fmt-deflockfile-round-trip" "formatter not idempotent for deflockfile"
else
    pass "fmt-deflockfile-round-trip"
fi

# ---------------------------------------------------------------------------
# ER1 tests: --strict-effects and TUR-W0030/W0031
# ---------------------------------------------------------------------------

# strict-effects-warn: an unannotated function that performs Write should trigger TUR-W0030
STRICT_INPUT=$(mktemp /tmp/tur-strict-XXXXXX.tur)
cat > "$STRICT_INPUT" << 'EOF'
(defeffect Write [msg :cstr] :nil)
(defn effectful [] :nil
  (perform (Write "hello")))
(defn main [] :int 0)
EOF
out=$("$TUR" --strict-effects emit-c "$STRICT_INPUT" 2>&1) || true
rm -f "$STRICT_INPUT"
if echo "$out" | grep -F "TUR-W0030" > /dev/null 2>&1; then
    pass "strict-effects-warn"
else
    fail "strict-effects-warn" "expected TUR-W0030 in stderr output"
fi

# over-annotated-warn: a function that declares #{Write} but never performs it should trigger TUR-W0031
OVER_INPUT=$(mktemp /tmp/tur-over-XXXXXX.tur)
cat > "$OVER_INPUT" << 'EOF'
(defeffect Write [msg :cstr] :nil)
(defn pure-fn [] #{Write} :int
  42)
(defn main [] :int 0)
EOF
out=$("$TUR" emit-c "$OVER_INPUT" 2>&1) || true
rm -f "$OVER_INPUT"
if echo "$out" | grep -F "TUR-W0031" > /dev/null 2>&1; then
    pass "over-annotated-warn"
else
    fail "over-annotated-warn" "expected TUR-W0031 in stderr output"
fi

# strict-effects-clean: a fully annotated function should not warn under --strict-effects
CLEAN_INPUT=$(mktemp /tmp/tur-clean-XXXXXX.tur)
cat > "$CLEAN_INPUT" << 'EOF'
(defeffect Write [msg :cstr] :nil)
(defn effectful [] #fx{Write} :nil
  (perform (Write "hello")))
;; main prints, so "fully annotated" means #fx{IO} (W4: println is #fx{IO}).
(defn main [] #fx{IO} :int
  (handle
    (do (effectful) 0)
    (Write [msg] k) (do (println msg) (resume k 0))))
EOF
out=$("$TUR" --strict-effects emit-c "$CLEAN_INPUT" 2>&1); rc=$?
rm -f "$CLEAN_INPUT"
if [ $rc -ne 0 ]; then
    fail "strict-effects-clean" "should succeed under --strict-effects when fully annotated (exit=$rc)"
elif echo "$out" | grep -F "TUR-W0030" > /dev/null 2>&1; then
    fail "strict-effects-clean" "should not warn TUR-W0030 when function has annotation"
else
    pass "strict-effects-clean"
fi

# ---------------------------------------------------------------------------
# ER6 tests: --dump-effects, --lint-effects (alias), -Werror=strict-effects
# ---------------------------------------------------------------------------

# dump-effects-basic: --dump-effects should print "defn effectful : #{Write}"
# Note: use a temp file to avoid grep -q SIGPIPE issue with large output + pipefail.
DUMP_FIXTURE="tests/fixtures/effect-dump/input.tur"
_dump_tmp=$(mktemp /tmp/tur-dump-XXXXXX)
"$TUR" --dump-effects emit-c "$DUMP_FIXTURE" 2>/dev/null > "$_dump_tmp"; rc=$?
if [ $rc -ne 0 ]; then
    fail "dump-effects-basic" "non-zero exit ($rc)"
elif ! grep -q "defn effectful : #" "$_dump_tmp"; then
    fail "dump-effects-basic" "expected 'defn effectful : #...' in output; got: $(grep '^defn' "$_dump_tmp" | head -3)"
elif ! grep -q "defn pure-fn : #{}" "$_dump_tmp"; then
    fail "dump-effects-basic" "expected 'defn pure-fn : #{}' in output"
else
    pass "dump-effects-basic"
fi
rm -f "$_dump_tmp"

# dump-effects-no-output: without --dump-effects, the same file should not emit "defn" effect lines
_nodump_tmp=$(mktemp /tmp/tur-nodump-XXXXXX)
"$TUR" emit-c "$DUMP_FIXTURE" 2>/dev/null > "$_nodump_tmp"
if grep -q "^defn effectful : #" "$_nodump_tmp"; then
    fail "dump-effects-no-output" "effect dump appeared without --dump-effects flag"
else
    pass "dump-effects-no-output"
fi
rm -f "$_nodump_tmp"

# dump-effects-module-members: a defn inside a (defmodule ...) body is inferred
# and listed like a top-level one (module-members-skip-effect-row-checking):
# `report` is unannotated and prints, `ticks` declares #fx{Tick}.
_mod_tmp=$(mktemp /tmp/tur-dump-mod-XXXXXX)
"$TUR" --dump-effects check "tests/fixtures/effect-row-module-member-ok/input.tur" \
    2>/dev/null > "$_mod_tmp"; rc=$?
if [ $rc -ne 0 ]; then
    fail "dump-effects-module-members" "non-zero exit ($rc)"
elif ! grep -qx "defn report : #{IO}" "$_mod_tmp"; then
    fail "dump-effects-module-members" "expected 'defn report : #{IO}'; got: $(grep '^defn report' "$_mod_tmp")"
elif ! grep -qx "defn ticks : #{Tick}" "$_mod_tmp"; then
    fail "dump-effects-module-members" "expected 'defn ticks : #{Tick}'; got: $(grep '^defn ticks' "$_mod_tmp")"
else
    pass "dump-effects-module-members"
fi
rm -f "$_mod_tmp"

# lint-effects-alias: --lint-effects is a deprecated alias for --strict-effects.
# It used to carry a byte-identical copy of the TUR-W0030 emitter (see
# docs/archive/strict-effects-and-lint-effects-are-indistinguishable.md); it
# now says so with TUR-W0050 and does exactly what --strict-effects does.
LINT_INPUT=$(mktemp /tmp/tur-lint-XXXXXX.tur)
cat > "$LINT_INPUT" << 'EOF'
(defeffect Write [msg :cstr] :nil)
(defn effectful [] :nil
  (perform (Write "hello")))
(defn main [] :int 0)
EOF
out=$("$TUR" --lint-effects emit-c "$LINT_INPUT" 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "lint-effects-alias" "deprecated alias should still compile (exit=$rc)"
elif ! echo "$out" | grep -F "TUR-W0050" > /dev/null 2>&1; then
    fail "lint-effects-alias" "expected the TUR-W0050 deprecation warning"
elif [ "$(echo "$out" | grep -cF "[TUR-W0030]")" -ne 1 ]; then
    fail "lint-effects-alias" "expected exactly one TUR-W0030 (the alias must not double-report)"
else
    pass "lint-effects-alias"
fi

# strict-effects-werror: -Werror=strict-effects makes TUR-W0030 an error (and
# implies --strict-effects); without the -Werror it stays a warning.
out=$("$TUR" -Werror=strict-effects emit-c "$LINT_INPUT" 2>&1); rc=$?
if [ $rc -eq 0 ]; then
    fail "strict-effects-werror" "should fail under -Werror=strict-effects"
elif ! echo "$out" | grep -F "error [TUR-W0030]" > /dev/null 2>&1; then
    fail "strict-effects-werror" "expected 'error [TUR-W0030]'; got: $(echo "$out" | head -3)"
else
    pass "strict-effects-werror"
fi
rm -f "$LINT_INPUT"

# strict-effects-werror-clean: an annotated program compiles under -Werror.
LINT_ANN_INPUT=$(mktemp /tmp/tur-lint-ann-XXXXXX.tur)
cat > "$LINT_ANN_INPUT" << 'EOF'
(defeffect Write [msg :cstr] :nil)
(defn effectful [] #fx{Write} :nil
  (perform (Write "hello")))
(defn main [] :int 0)
EOF
out=$("$TUR" -Werror=strict-effects emit-c "$LINT_ANN_INPUT" 2>&1); rc=$?
rm -f "$LINT_ANN_INPUT"
if [ $rc -ne 0 ]; then
    fail "strict-effects-werror-clean" "annotated program should compile under -Werror=strict-effects (exit=$rc)"
elif echo "$out" | grep -F "TUR-W0030" > /dev/null 2>&1; then
    fail "strict-effects-werror-clean" "should not report TUR-W0030 for an annotated function"
else
    pass "strict-effects-werror-clean"
fi

# strict-effects-lambda-name: TUR-W0030 on a `fn` literal names it by its
# enclosing defn, never by the elaborator's `__fn_N` gensym (see
# docs/archive/strict-effects-w0030-names-synthesized-lambdas.md).
LAMBDA_INPUT=$(mktemp /tmp/tur-lambda-XXXXXX.tur)
cat > "$LAMBDA_INPUT" << 'EOF'
(defeffect Bang [] :nil)
(defn make-thunk [] : (fn [] int)
  (fn [] : int (do (perform (Bang)) 0)))
(defn main [] :int 0)
EOF
out=$("$TUR" --strict-effects emit-c "$LAMBDA_INPUT" 2>&1) || true
rm -f "$LAMBDA_INPUT"
if echo "$out" | grep -F "__fn_" > /dev/null 2>&1; then
    fail "strict-effects-lambda-name" "a diagnostic printed a __fn_ gensym: $(echo "$out" | grep -F "__fn_" | head -1)"
elif ! echo "$out" | grep -F "anonymous function in 'make-thunk' performs effects {Bang}" > /dev/null 2>&1; then
    fail "strict-effects-lambda-name" "expected the lambda to be described by its enclosing defn; got: $(echo "$out" | grep TUR-W0030 | head -2)"
else
    pass "strict-effects-lambda-name"
fi

# strict-effects-rowvar-prints: a row-polymorphic HOF that also prints keeps
# #fx{e}.  `println` is #fx{IO} (effect-row-honesty-plan W4), but a capability
# tag is not what flows through the row variable, so TUR-W0032 ("row variable
# is always concrete") must not fire -- under -Werror=strict-effects it would
# fail the build.
ROWVAR_INPUT=$(mktemp /tmp/tur-rowvar-XXXXXX.tur)
cat > "$ROWVAR_INPUT" << 'EOF2'
(defn each-twice [f : (fn [int] #fx{e} int)] #fx{e} : int
  (do (println "twice")
      (+ (f 10) (f 20))))
(defn main [] #fx{IO} :int (do (println (each-twice (fn [x : int] : int x))) 0))
EOF2
out=$("$TUR" -Werror=strict-effects emit-c "$ROWVAR_INPUT" 2>&1); rc=$?
rm -f "$ROWVAR_INPUT"
if [ $rc -ne 0 ]; then
    fail "strict-effects-rowvar-prints" "should compile under -Werror=strict-effects (exit=$rc): $(echo "$out" | grep TUR- | head -2)"
elif echo "$out" | grep -F "TUR-W0032" > /dev/null 2>&1; then
    fail "strict-effects-rowvar-prints" "a capability tag made the row variable look always-concrete"
else
    pass "strict-effects-rowvar-prints"
fi

# try-with-basic: try-with behaves identically to handle
out=$("$TUR" --interpret tests/fixtures/try-with-basic/input.tur 2>/dev/null); rc=$?
if [ $rc -ne 0 ]; then
    fail "try-with-basic" "non-zero exit ($rc)"
elif [ "$out" != "$(printf 'asking\n42')" ]; then
    fail "try-with-basic" "unexpected output: '$out'"
else
    pass "try-with-basic"
fi

# try-with-nested: nested try-with handlers work correctly
out=$("$TUR" --interpret tests/fixtures/try-with-nested/input.tur 2>/dev/null); rc=$?
if [ $rc -ne 0 ]; then
    fail "try-with-nested" "non-zero exit ($rc)"
elif [ "$out" != "$(printf 'start\noops')" ]; then
    fail "try-with-nested" "unexpected output: '$out'"
else
    pass "try-with-nested"
fi

# ---------------------------------------------------------------------------
# ER6: --check mode for effect-row errors
# ---------------------------------------------------------------------------

# check-mode-effect-error: `tur check` reports TUR-E0009 and exits 1 when a
# function's declared row does not include an effect it performs.
CHECK_INPUT=$(mktemp /tmp/tur-check-effect-XXXXXX.tur)
cat > "$CHECK_INPUT" << 'EOF'
(defeffect Write [s :cstr] :nil)
(defn bad [] #{} :nil
  (perform (Write "oops")))
(defn main [] :int 0)
EOF
out=$("$TUR" check "$CHECK_INPUT" 2>&1); rc=$?
rm -f "$CHECK_INPUT"
if [ $rc -eq 0 ]; then
    fail "check-mode-effect-error" "expected non-zero exit from tur check"
elif ! echo "$out" | grep -F "TUR-E0009" > /dev/null 2>&1; then
    fail "check-mode-effect-error" "expected TUR-E0009 in check output"
else
    pass "check-mode-effect-error"
fi

# check-mode-effect-ok: `tur check` exits 0 for a well-annotated program.
CHECK_OK_INPUT=$(mktemp /tmp/tur-check-ok-XXXXXX.tur)
cat > "$CHECK_OK_INPUT" << 'EOF'
(defeffect Write [s :cstr] :nil)
(defn do-write [] #{Write} :nil
  (perform (Write "hello")))
(defn main [] :int 0)
EOF
out=$("$TUR" check "$CHECK_OK_INPUT" 2>&1); rc=$?
rm -f "$CHECK_OK_INPUT"
if [ $rc -ne 0 ]; then
    fail "check-mode-effect-ok" "expected zero exit from tur check (exit=$rc): $out"
else
    pass "check-mode-effect-ok"
fi

# ---------------------------------------------------------------------------
# PR5-3-B: effect-export-syntax
# ---------------------------------------------------------------------------

# effect-export-syntax: (export (effect Write)) is accepted by the parser/elab.
# stderr is dropped (the interpreter's sanitizer build emits a benign ASan
# makecontext/swapcontext warning there); rc captures elaboration errors and
# stdout carries the program output.
err=$(mktemp)
out=$("$TUR" --interpret tests/fixtures/effect-export-explicit/input.tur 2>"$err"); rc=$?
if [ $rc -ne 0 ]; then
    fail "effect-export-syntax" "should elaborate (exit=$rc): $(cat "$err")"
elif [ "$out" != "hello" ]; then
    fail "effect-export-syntax" "unexpected output: '$out'"
else
    pass "effect-export-syntax"
fi
rm -f "$err"

# ---------------------------------------------------------------------------
# E1: --help / -h (Tier 1)
# ---------------------------------------------------------------------------

# tur --help: should print usage and exit 0
out=$("$TUR" --help 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "help-global" "expected exit 0, got $rc"
elif ! grep -q "usage:" <<< "$out"; then
    fail "help-global" "output did not contain 'usage:'"
else
    pass "help-global"
fi

# tur -h: same as --help
out=$("$TUR" -h 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "help-short" "expected exit 0, got $rc"
elif ! grep -q "usage:" <<< "$out"; then
    fail "help-short" "output did not contain 'usage:'"
else
    pass "help-short"
fi

# tur build --help: should print subcommand help and exit 0
out=$("$TUR" build --help 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "help-build" "expected exit 0, got $rc"
elif ! grep -q "tur build" <<< "$out"; then
    fail "help-build" "output did not mention 'tur build'"
else
    pass "help-build"
fi

# tur run --help: should print subcommand help and exit 0
out=$("$TUR" run --help 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "help-run" "expected exit 0, got $rc"
elif ! grep -q "tur run" <<< "$out"; then
    fail "help-run" "output did not mention 'tur run'"
else
    pass "help-run"
fi

# tur check --help: should print subcommand help and exit 0
out=$("$TUR" check --help 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "help-check" "expected exit 0, got $rc"
elif ! grep -q "tur check" <<< "$out"; then
    fail "help-check" "output did not mention 'tur check'"
else
    pass "help-check"
fi

# tur eval --help: should print subcommand help and exit 0
out=$("$TUR" eval --help 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "help-eval" "expected exit 0, got $rc"
elif ! grep -q "tur eval" <<< "$out"; then
    fail "help-eval" "output did not mention 'tur eval'"
else
    pass "help-eval"
fi

# tur format --help: should print subcommand help and exit 0
out=$("$TUR" format --help 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "help-format" "expected exit 0, got $rc"
elif ! grep -q "tur format" <<< "$out"; then
    fail "help-format" "output did not mention 'tur format'"
else
    pass "help-format"
fi

# tur test --help: should print subcommand help and exit 0
out=$("$TUR" test --help 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "help-test" "expected exit 0, got $rc"
elif ! grep -q "tur test" <<< "$out"; then
    fail "help-test" "output did not mention 'tur test'"
else
    pass "help-test"
fi

# tur repl --help: should print subcommand help and exit 0
out=$("$TUR" repl --help 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "help-repl" "expected exit 0, got $rc"
elif ! grep -q "tur repl" <<< "$out"; then
    fail "help-repl" "output did not mention 'tur repl'"
else
    pass "help-repl"
fi

# cli-usage-error-paths-exit-zero: for every subcommand with a usage helper,
# `--help` is a successful request for help (exit 0) AND an unknown flag is an
# error (exit nonzero).  Both halves, per subcommand: each alone passes for
# the wrong reason -- the bug was that the error path reused the help helper
# and inherited its 0, so `tur build --typo || exit 1` reported success.
for sub in build run check eval doc explain format expand emit-c emit-h link \
           repl test parse-check docs smt; do
    "$TUR" "$sub" --help >/dev/null 2>&1; rc=$?
    if [ $rc -ne 0 ]; then
        fail "usage-status-help-$sub" "expected 'tur $sub --help' to exit 0, got $rc"
    else
        pass "usage-status-help-$sub"
    fi
    out=$("$TUR" "$sub" --bogus-flag 2>&1); rc=$?
    if [ $rc -eq 0 ]; then
        fail "usage-status-error-$sub" "expected 'tur $sub --bogus-flag' to exit nonzero, got 0"
    elif ! grep -qi "usage\|unknown\|bogus" <<< "$out"; then
        fail "usage-status-error-$sub" "expected a usage/unknown-option message"
    else
        pass "usage-status-error-$sub"
    fi
done

# ---------------------------------------------------------------------------
# E2: --version / -V (Tier 1)
# ---------------------------------------------------------------------------

# tur --version: should print "tur: the Turmeric compiler (v<version>)" and exit 0
out=$("$TUR" --version 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "version-long" "expected exit 0, got $rc"
elif ! grep -q "^tur: the Turmeric compiler " <<< "$out"; then
    fail "version-long" "output '$out' did not start with 'tur: the Turmeric compiler '"
else
    pass "version-long"
fi

# tur -V: same as --version
out=$("$TUR" -V 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "version-short" "expected exit 0, got $rc"
elif ! grep -q "^tur: the Turmeric compiler " <<< "$out"; then
    fail "version-short" "output '$out' did not start with 'tur: the Turmeric compiler '"
else
    pass "version-short"
fi

# ---------------------------------------------------------------------------
# E3: tur eval (Tier 1)
# ---------------------------------------------------------------------------

# tur eval: evaluate an arithmetic expression
out=$("$TUR" eval "(+ 1 2)" 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "eval-basic" "expected exit 0, got $rc; output: $out"
elif [ "$out" != "3" ]; then
    fail "eval-basic" "expected '3', got '$out'"
else
    pass "eval-basic"
fi

# tur eval: evaluate a comparison (builtin)
out=$("$TUR" eval "(< 1 2)" 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "eval-bool" "expected exit 0, got $rc; output: $out"
elif [ "$out" != "true" ]; then
    fail "eval-bool" "expected 'true', got '$out'"
else
    pass "eval-bool"
fi

# tur eval: nil result prints nothing
out=$("$TUR" eval "(def x 42)" 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "eval-nil-silent" "expected exit 0, got $rc; output: $out"
else
    pass "eval-nil-silent"
fi

# tur eval: parse error exits nonzero
out=$("$TUR" eval "(+ 1" 2>&1); rc=$?
if [ $rc -eq 0 ]; then
    fail "eval-error" "expected nonzero exit for bad syntax, got 0"
else
    pass "eval-error"
fi

# tur eval --file: run a .tur file through the interpreter
TMP_TUR=$(mktemp /tmp/tur_eval_test_XXXXXX.tur)
echo '(def x 42) (+ x 1)' > "$TMP_TUR"
out=$("$TUR" eval --file "$TMP_TUR" 2>&1); rc=$?
rm -f "$TMP_TUR"
if [ $rc -ne 0 ]; then
    fail "eval-file" "expected exit 0, got $rc; output: $out"
else
    pass "eval-file"
fi

# Phase R2: catch-unwind composes with ok?/err? through the interpreter.
# Regression guard for the fix that (a) types ok?/err?/some?/none? as :bool in
# the --interpret/eval path and (b) returns catch-unwind results in the native
# Result-box layout so the predicates recover ok vs caught-panic.
TMP_CU=$(mktemp /tmp/tur_catch_unwind_XXXXXX.tur)
cat > "$TMP_CU" <<'CUEOF'
(defn boom [] :int (panic "boom"))
(defn main [] :int
  (let [r1 (catch-unwind (fn [] :int 42))]
    (if (ok? r1) (println "ok") (println "not-ok")))
  (let [r2 (catch-unwind (fn [] :int (boom)))]
    (if (err? r2) (println "caught") (println "not-caught")))
  0)
CUEOF
out=$(ASAN_OPTIONS=detect_leaks=0 "$TUR" eval --file "$TMP_CU" 2>&1); rc=$?
rm -f "$TMP_CU"
if [ $rc -eq 0 ] && grep -q "ok" <<< "$out" && grep -q "caught" <<< "$out"; then
    pass "eval-catch-unwind"
else
    fail "eval-catch-unwind" "expected ok+caught, got rc=$rc output: $out"
fi

# Phase R2 (OQ#2): result-must / option-must raise a *catchable* panic instead
# of calling _exit(1), so catch-unwind recovers them and an uncaught one fires
# defers + exits nonzero with the standard panic message.
TMP_MUST=$(mktemp /tmp/tur_must_XXXXXX.tur)
cat > "$TMP_MUST" <<'MUSTEOF'
(defn main [] :int
  (let [r (catch-unwind (fn [] :int (result-must (err 7))))]
    (if (err? r) (println "must-caught") (println "must-not-caught")))
  (let [v (result-must (ok 5))]
    (if (= v 5) (println "must-passthrough") (println "must-wrong")))
  0)
MUSTEOF
out=$(ASAN_OPTIONS=detect_leaks=0 "$TUR" eval --file "$TMP_MUST" 2>&1); rc=$?
rm -f "$TMP_MUST"
if [ $rc -eq 0 ] && grep -q "must-caught" <<< "$out" && grep -q "must-passthrough" <<< "$out"; then
    pass "eval-must-catchable"
else
    fail "eval-must-catchable" "expected must-caught+must-passthrough, got rc=$rc output: $out"
fi

# An uncaught result-must panic exits nonzero (does not silently _exit(0)).
TMP_MUST2=$(mktemp /tmp/tur_must2_XXXXXX.tur)
echo '(defn main [] :int (result-must (err 1)))' > "$TMP_MUST2"
out=$(ASAN_OPTIONS=detect_leaks=0 "$TUR" eval --file "$TMP_MUST2" 2>&1); rc=$?
rm -f "$TMP_MUST2"
if [ $rc -ne 0 ] && grep -q "result-must: called on err" <<< "$out"; then
    pass "eval-must-uncaught"
else
    fail "eval-must-uncaught" "expected nonzero + panic msg, got rc=$rc output: $out"
fi

# ---------------------------------------------------------------------------
# E4: Nonzero exit codes on errors (Tier 1 verification)
# ---------------------------------------------------------------------------

# tur check on a bad file exits nonzero
TMP_BAD=$(mktemp /tmp/tur_bad_XXXXXX.tur)
echo '(+ 1 "not-an-int")' > "$TMP_BAD"
"$TUR" check "$TMP_BAD" >/dev/null 2>&1; rc=$?
rm -f "$TMP_BAD"
if [ $rc -eq 0 ]; then
    fail "exit-code-check-error" "expected nonzero exit for type error, got 0"
else
    pass "exit-code-check-error"
fi

# tur check on a nonexistent file exits nonzero
"$TUR" check /nonexistent/file.tur >/dev/null 2>&1; rc=$?
if [ $rc -eq 0 ]; then
    fail "exit-code-file-not-found" "expected nonzero exit for missing file, got 0"
else
    pass "exit-code-file-not-found"
fi

# ---------------------------------------------------------------------------
# Tier 2: tur doc <sym>  (2a)
# ---------------------------------------------------------------------------

# tur doc +: should print arithmetic doc and exit 0
out=$("$TUR" doc "+" 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "doc-builtin-plus" "expected exit 0, got $rc; output: $out"
elif ! grep -q "+" <<< "$out"; then
    fail "doc-builtin-plus" "output '$out' did not contain '+'"
else
    pass "doc-builtin-plus"
fi

# tur doc let: should print let doc and exit 0
out=$("$TUR" doc "let" 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "doc-builtin-let" "expected exit 0, got $rc; output: $out"
elif ! grep -q "let" <<< "$out"; then
    fail "doc-builtin-let" "output '$out' did not contain 'let'"
else
    pass "doc-builtin-let"
fi

# tur doc defstruct: should print defstruct doc and exit 0
out=$("$TUR" doc "defstruct" 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "doc-builtin-defstruct" "expected exit 0, got $rc; output: $out"
elif ! grep -q "defstruct" <<< "$out"; then
    fail "doc-builtin-defstruct" "output '$out' did not contain 'defstruct'"
else
    pass "doc-builtin-defstruct"
fi

# tur doc unknown: should print error message and exit nonzero
out=$("$TUR" doc "this-symbol-does-not-exist-xyz" 2>&1); rc=$?
if [ $rc -eq 0 ]; then
    fail "doc-unknown-sym" "expected nonzero exit for unknown symbol, got 0"
else
    pass "doc-unknown-sym"
fi

# tur doc --help: should print usage and exit 0
out=$("$TUR" doc --help 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "doc-help" "expected exit 0, got $rc"
elif ! grep -q "tur doc" <<< "$out"; then
    fail "doc-help" "output '$out' did not mention 'tur doc'"
else
    pass "doc-help"
fi

# ---------------------------------------------------------------------------
# Tier 2: tur run -  (2b — stdin mode)
# ---------------------------------------------------------------------------

# tur run -: compile and run source from stdin
out=$(echo '(println (+ 40 2))' | "$TUR" run - 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "run-stdin" "expected exit 0, got $rc; output: $out"
elif ! grep -q "42" <<< "$out"; then
    fail "run-stdin" "expected '42' in output, got '$out'"
else
    pass "run-stdin"
fi

# ---------------------------------------------------------------------------
# Tier 3: tur explain subcommand  (E13)
# ---------------------------------------------------------------------------

# tur explain --help: should print usage and exit 0
out=$("$TUR" explain --help 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "explain-help" "expected exit 0, got $rc"
elif ! grep -q "tur explain" <<< "$out"; then
    fail "explain-help" "output '$out' did not mention 'tur explain'"
else
    pass "explain-help"
fi

# tur explain <snippet>: compile a bad snippet and exit nonzero
out=$("$TUR" explain '(+ 1 "x")' 2>&1); rc=$?
if [ $rc -eq 0 ]; then
    fail "explain-snippet-error" "expected nonzero exit for type error snippet, got 0"
else
    pass "explain-snippet-error"
fi

# tur explain <good snippet>: no errors, exit 0
out=$("$TUR" explain '(+ 1 2)' 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "explain-snippet-ok" "expected exit 0 for valid snippet, got $rc; output: $out"
else
    pass "explain-snippet-ok"
fi

# ---------------------------------------------------------------------------
# Tier 3: tur format --diff  (E12)
# ---------------------------------------------------------------------------

# tur format --diff on already-formatted file: exit 0, no output
TMP_FMT=$(mktemp /tmp/tur_fmt_XXXXXX.tur)
printf '(+ 1 2)\n' > "$TMP_FMT"
out=$("$TUR" format --diff "$TMP_FMT" 2>&1); rc=$?
rm -f "$TMP_FMT"
if [ $rc -ne 0 ]; then
    fail "format-diff-clean" "expected exit 0 for already-formatted file, got $rc; output: $out"
else
    pass "format-diff-clean"
fi

# tur format --diff on unformatted file: exit 1 and print diff
TMP_UGLY=$(mktemp /tmp/tur_ugly_XXXXXX.tur)
printf '(+   1   2)\n' > "$TMP_UGLY"
out=$("$TUR" format --diff "$TMP_UGLY" 2>&1); rc=$?
rm -f "$TMP_UGLY"
if [ $rc -eq 0 ]; then
    fail "format-diff-changed" "expected exit 1 for unformatted file, got 0"
elif ! grep -q "^[-+]" <<< "$out"; then
    fail "format-diff-changed" "expected diff output, got '$out'"
else
    pass "format-diff-changed"
fi

# tur format --help: should mention --diff
out=$("$TUR" format --help 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "format-help-diff" "expected exit 0, got $rc"
elif ! grep -q -- "--diff" <<< "$out"; then
    fail "format-help-diff" "format --help did not mention --diff"
else
    pass "format-help-diff"
fi

# ---------------------------------------------------------------------------
# Tier 3: --json output flag  (E14)
# ---------------------------------------------------------------------------

# tur --json doc +: should print JSON with name and doc fields
out=$("$TUR" --json doc "+" 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "json-doc" "expected exit 0, got $rc; output: $out"
elif ! grep -q '"name"' <<< "$out"; then
    fail "json-doc" "expected JSON with 'name' field, got '$out'"
elif ! grep -q '"doc"' <<< "$out"; then
    fail "json-doc" "expected JSON with 'doc' field, got '$out'"
else
    pass "json-doc"
fi

# tur --json doc unknown: exit 1 even in JSON mode
out=$("$TUR" --json doc "no-such-sym-xyz" 2>&1); rc=$?
if [ $rc -eq 0 ]; then
    fail "json-doc-unknown" "expected nonzero exit for unknown sym in JSON mode, got 0"
else
    pass "json-doc-unknown"
fi

# ---------------------------------------------------------------------------
# CPS1 (cps-transform-plan): --dump-cps-coloring may-capture coloring
# ---------------------------------------------------------------------------

# dump-cps-coloring-partition: the analysis must color uses-shift (seed) and
# calls-shifter (transitive), and leave the pure functions + main uncolored.
# NOTE: match with bash globs, not `echo "$out" | grep -q`. The emit-c output is
# large; under `set -o pipefail` a matching `grep -q` exits early, SIGPIPEs the
# `echo`, and the pipeline is reported as failed even though the match succeeded.
CPS_FIXTURE="tests/fixtures/cps-coloring/input.tur"
out=$("$TUR" --dump-cps-coloring emit-c "$CPS_FIXTURE" 2>/dev/null); rc=$?
if [ $rc -ne 0 ]; then
    fail "dump-cps-coloring-partition" "non-zero exit ($rc)"
elif [[ "$out" != *"cps-coloring: pure-arith uncolored"* ]]; then
    fail "dump-cps-coloring-partition" "expected 'pure-arith uncolored'"
elif [[ "$out" != *"cps-coloring: also-pure uncolored"* ]]; then
    fail "dump-cps-coloring-partition" "expected 'also-pure uncolored'"
elif [[ "$out" != *"cps-coloring: uses-shift COLORED"* ]]; then
    fail "dump-cps-coloring-partition" "expected 'uses-shift COLORED' (seed)"
elif [[ "$out" != *"cps-coloring: calls-shifter COLORED"* ]]; then
    fail "dump-cps-coloring-partition" "expected 'calls-shifter COLORED' (transitive)"
elif [[ "$out" != *"cps-coloring: main uncolored"* ]]; then
    fail "dump-cps-coloring-partition" "expected 'main uncolored'"
else
    pass "dump-cps-coloring-partition"
fi

# dump-cps-coloring-no-output: without the flag, no coloring lines should appear.
out=$("$TUR" emit-c "$CPS_FIXTURE" 2>/dev/null); rc=$?
if [[ "$out" == *"cps-coloring:"* ]]; then
    fail "dump-cps-coloring-no-output" "coloring dump appeared without --dump-cps-coloring flag"
else
    pass "dump-cps-coloring-no-output"
fi

# dump-cps-coloring-effects: F1 (compiled-first-class-continuations-plan) -- the
# algebraic-effect operators must seed the may-capture coloring the same way
# shift/reset do.  does-perform (direct `perform`) and has-handle (direct
# `handle`) are seeds; calls-performer reaches `perform` transitively; the two
# pure functions stay uncolored.  This locks the "mechanical part" of Phase F1.
EFF_FIXTURE="tests/fixtures/cps-effect-coloring/input.tur"
out=$("$TUR" --dump-cps-coloring emit-c "$EFF_FIXTURE" 2>/dev/null); rc=$?
if [ $rc -ne 0 ]; then
    fail "dump-cps-coloring-effects" "non-zero exit ($rc)"
elif [[ "$out" != *"cps-coloring: pure-arith uncolored"* ]]; then
    fail "dump-cps-coloring-effects" "expected 'pure-arith uncolored'"
elif [[ "$out" != *"cps-coloring: also-pure uncolored"* ]]; then
    fail "dump-cps-coloring-effects" "expected 'also-pure uncolored'"
elif [[ "$out" != *"cps-coloring: does-perform COLORED"* ]]; then
    fail "dump-cps-coloring-effects" "expected 'does-perform COLORED' (perform seed)"
elif [[ "$out" != *"cps-coloring: calls-performer COLORED"* ]]; then
    fail "dump-cps-coloring-effects" "expected 'calls-performer COLORED' (transitive)"
elif [[ "$out" != *"cps-coloring: has-handle COLORED"* ]]; then
    fail "dump-cps-coloring-effects" "expected 'has-handle COLORED' (handle seed)"
else
    pass "dump-cps-coloring-effects"
fi

# ---------------------------------------------------------------------------
# CPS2 (cps-transform-plan): --dump-cps ANF/CPS IR
# ---------------------------------------------------------------------------
# (bash globs, not `grep -q` -- see the SIGPIPE/pipefail note above.)

# dump-cps-anf: the CPS IR for the colored functions must show ANF naming, the
# typed continuation parameter, a join continuation, a threaded tail call, and
# the reset/shift control forms.
out=$("$TUR" --dump-cps emit-c "$CPS_FIXTURE" 2>/dev/null); rc=$?
if [ $rc -ne 0 ]; then
    fail "dump-cps-anf" "non-zero exit ($rc)"
elif [[ "$out" != *"cps-fn uses-shift"* ]]; then
    fail "dump-cps-anf" "expected a 'cps-fn uses-shift' block"
elif [[ "$out" != *"k:cont<int>"* ]]; then
    fail "dump-cps-anf" "expected typed continuation 'k:cont<int>'"
elif [[ "$out" != *"tailcall uses-shift("* ]]; then
    fail "dump-cps-anf" "expected a threaded 'tailcall uses-shift(...)'"
elif [[ "$out" != *"letcont j"* ]]; then
    fail "dump-cps-anf" "expected a join continuation 'letcont j...'"
elif [[ "$out" != *"reset __t"* ]]; then
    fail "dump-cps-anf" "expected a 'reset' control form"
elif [[ "$out" != *"shift k'"* ]]; then
    fail "dump-cps-anf" "expected a 'shift' control form"
else
    pass "dump-cps-anf"
fi

# dump-cps-no-output: without the flag, no CPS IR should appear.
out=$("$TUR" emit-c "$CPS_FIXTURE" 2>/dev/null); rc=$?
if [[ "$out" == *"cps-fn "* ]]; then
    fail "dump-cps-no-output" "CPS IR dump appeared without --dump-cps flag"
else
    pass "dump-cps-no-output"
fi

# ---------------------------------------------------------------------------
# CPS3 (cps-transform-plan): selective lowering + direct<->CPS boundary bridging
# ---------------------------------------------------------------------------

# dump-cps-bridge: the mixed fixture must show the boundary classification --
# main is a direct->CPS entry root; shift-then-twice/run are internal; the call
# into the uncolored `twice` is a cps->direct bridge; colored tail calls are
# cps->cps; and the uncolored `twice` is NOT lowered (stays direct style).
MIX_FIXTURE="tests/fixtures/cps-mixed-coloring/input.tur"
out=$("$TUR" --dump-cps emit-c "$MIX_FIXTURE" 2>/dev/null); rc=$?
if [ $rc -ne 0 ]; then
    fail "dump-cps-bridge" "non-zero exit ($rc)"
elif [[ "$out" != *"cps-fn main "*"entry"* ]]; then
    fail "dump-cps-bridge" "expected main classified as a direct->CPS 'entry'"
elif [[ "$out" != *"cps-fn shift-then-twice "*"internal"* ]]; then
    fail "dump-cps-bridge" "expected shift-then-twice classified 'internal'"
elif [[ "$out" != *"call twice("*"; cps->direct"* ]]; then
    fail "dump-cps-bridge" "expected a 'cps->direct' bridge into uncolored twice"
elif [[ "$out" != *"; cps->cps"* ]]; then
    fail "dump-cps-bridge" "expected a 'cps->cps' threaded tail call"
elif [[ "$out" == *"cps-fn twice "* ]]; then
    fail "dump-cps-bridge" "uncolored twice must stay direct (not CPS-lowered)"
else
    pass "dump-cps-bridge"
fi

# ---------------------------------------------------------------------------
# #lang base switching in a REPL session + canonical reader name
# ---------------------------------------------------------------------------

# string-literal-always-on: `#s"..."` needs no directive of any kind, and a
# `#lang` switch cannot turn it off -- the dispatch is installed by the reader
# for every file and every base. This replaces lang-layer-toggle-off, which
# asserted the opposite of the post-decommission contract: that a dropped
# `stringed` layer deactivated the dispatch mid-session.
# The blank line after the sweet-exp expression is required, not incidental:
# the REPL's sweet continuation detection holds an indented block open until a
# dedent or a blank line, so a piped sweet expression followed straight by
# :quit is never flushed and evaluates to nothing at all. Without it this
# assertion fails identically whatever #s"..." does.
out=$(printf '#s"first"\n#lang turmeric/sweet\n#s"second"\n\n:quit\n' \
      | "$TUR" repl 2>&1); rc=$?
if ! grep -q '=> "first"' <<< "$out"; then
    fail "string-literal-always-on" "#s\"...\" did not dispatch with no #lang line at all"
elif grep -q "unknown reader string macro '#s'" <<< "$out"; then
    fail "string-literal-always-on" "#s\"...\" reported as an unknown dispatch"
elif ! grep -q '=> "second"' <<< "$out"; then
    fail "string-literal-always-on" "#s\"...\" stopped dispatching after a base switch"
else
    pass "string-literal-always-on"
fi

# reader-name-canonical: reader_type_name(READER_SWEET) reports the canonical
# slash-namespaced spelling; the legacy `sweet-exp` alias is accepted on
# input but never generated, so the round-trip is stable.
out=$(printf '#lang sweet-exp\n#lang turmeric/sweet\n:quit\n' | "$TUR" repl 2>&1); rc=$?
if ! grep -q "; reader set to turmeric/sweet (session reset)" <<< "$out"; then
    fail "reader-name-canonical" "legacy alias did not report canonical 'turmeric/sweet'"
elif ! grep -q "; reader already set to turmeric/sweet" <<< "$out"; then
    fail "reader-name-canonical" "canonical spelling not recognized as the same reader"
elif grep -q "reader (set to\|already set to) sweet-exp" <<< "$out"; then
    fail "reader-name-canonical" "legacy 'sweet-exp' spelling was generated"
else
    pass "reader-name-canonical"
fi

# lang-same-base-no-reset: repeating the SAME `#lang` line must not reset the
# session (turi_env_apply_lang is a no-op when nothing changes).  Retargeted
# from a base+layer line to a bare base when the layer axis was decommissioned;
# the property being pinned -- identical directive is a no-op, bindings survive
# -- is unchanged.
out=$(printf '#lang turmeric\n(def keep 41)\n#lang turmeric\n(+ keep 1)\n:quit\n' \
      | "$TUR" repl 2>&1); rc=$?
if ! grep -q "; reader already set to turmeric" <<< "$out"; then
    fail "lang-same-base-no-reset" "identical #lang line was not treated as a no-op"
elif ! grep -q "=> 42" <<< "$out"; then
    fail "lang-same-base-no-reset" "binding did not survive an identical #lang line"
else
    pass "lang-same-base-no-reset"
fi

# repl-doc-no-exceptions: the :doc builtin table must not resurrect
# try/catch/throw.  Exceptions were deleted end-to-end in v0.25.0
# (CHANGELOG.md:1974) but their :doc rows survived for eleven releases, so the
# prompt kept describing a form that no longer elaborates.  Assert both halves:
# the dead names are gone, and the Result-based surface that replaced them is
# documented in their place.
out=$(printf ':doc try\n:doc catch\n:doc throw\n:doc panic\n:doc catch-unwind\n:quit\n' \
      | "$TUR" repl 2>&1); rc=$?
if grep -qi "catch runtime errors\|raise a runtime error\|error handler clause" <<< "$out"; then
    fail "repl-doc-no-exceptions" "the removed try/catch/throw docs are still in the :doc table"
elif [ "$(echo "$out" | grep -c "no documentation for")" -lt 3 ]; then
    fail "repl-doc-no-exceptions" "expected try/catch/throw to report no documentation"
elif ! grep -q "abort with an unrecoverable error" <<< "$out"; then
    fail "repl-doc-no-exceptions" ":doc panic did not describe the panic form"
elif ! grep -q "catch-unwind thunk" <<< "$out"; then
    fail "repl-doc-no-exceptions" ":doc catch-unwind did not describe the Result-returning form"
else
    pass "repl-doc-no-exceptions"
fi

# ---------------------------------------------------------------------------
# jit-ffi-c2mir-plan: dynamic FFI (call thunks, extern-c, call-ptr)
# ---------------------------------------------------------------------------

# The interpreter halves of the feature exist only in -DTUR_JIT=ON builds;
# probe the binary once and PASS-skip those cases against a JIT-less tur
# (mirroring how run-jit.sh treats an engine-less binary).  The non-JIT
# diagnostics ARE asserted either way.
# A temp DIRECTORY plus a fixed filename, rather than `mktemp -t
# tur-jit-ffi.XXXXXX.tur`.  `-t` is not portable in the way that spelling
# assumes: GNU coreutils reads the argument as a TEMPLATE (so the name ends
# in `.tur`), while BSD/macOS reads it as a PREFIX and appends its own
# `.XXXXXXXX` -- leaving a file that does NOT end in `.tur`.  `tur run
# <path>` then declines to see it as a source file, falls through to
# project-task resolution, and dies with "recipe not found / available:
# <the whole Justfile>".  That is what made the two compiled-path cases
# below fail on the macOS legs only, with an empty stdout and no clue.
# The other cases never noticed because emit-c / --interpret / check take
# the path as given.
TMP_FFI_DIR=$(mktemp -d -t tur-jit-ffi-XXXXXX)
TMP_FFI="$TMP_FFI_DIR/case.tur"
# Removed by the single EXIT trap at the top of this file -- a `trap ... EXIT`
# here would replace that one and leak the $TUR timeout wrapper.
# NOTE: captured via command substitution, not a pipeline -- this script
# runs `set -o pipefail`, and `tur jit`'s non-zero exit (or the SIGPIPE from
# grep -q's early close) would mask a successful match.
HAS_JIT=1
printf '(defn main [] : int 0)\n' > "$TMP_FFI"
probe_out=$("$TUR" jit "$TMP_FFI" 2>&1 || true)
case "$probe_out" in *"no JIT engine"*) HAS_JIT=0 ;; esac

# jit-ffi-extern-c-real: under --interpret in a JIT build, an extern-c
# declaration beyond the known-override table resolves via dlsym and gets
# CALLED for real -- the old behavior silently returned nil (printed 0).
printf '(extern-c strtol [s :cstr endp :int base :int] :int)\n(defn main [] : int (println (strtol "123abc" 0 10)) 0)\n' > "$TMP_FFI"
if [ "$HAS_JIT" = "1" ]; then
    out=$(ASAN_OPTIONS=detect_leaks=0 "$TUR" --interpret "$TMP_FFI" 2>/dev/null)
    if ! grep -q "^123$" <<< "$out"; then
        fail "jit-ffi-extern-c-real" "expected strtol to return 123 under --interpret, got: $out"
    else
        pass "jit-ffi-extern-c-real"
    fi
else
    echo "SKIP jit-ffi-extern-c-real (no JIT engine in this build)"
fi

# jit-ffi-call-ptr-interp: the call-ptr form routed through the c2mir thunk
# provider under --interpret (dlopen -> dlsym -> call-ptr, the full loop).
# Linux-only: the fixture names a soname; macOS spells libm differently and
# the portable half is covered by tests/fixtures/jit-ffi-call-ptr.
if [ "$HAS_JIT" = "1" ] && [ "$(uname)" = "Linux" ]; then
    printf '(defn main [] : int\n  (unsafe\n    (let [h (dlopen "libm.so.6")\n          p (dlsym h "cbrt")]\n      (println (call-ptr p [:float -> :float] 27.0))))\n  0)\n' > "$TMP_FFI"
    out=$(ASAN_OPTIONS=detect_leaks=0 "$TUR" --interpret "$TMP_FFI" 2>/dev/null)
    if ! grep -q "^3$" <<< "$out"; then
        fail "jit-ffi-call-ptr-interp" "expected cbrt(27) = 3 via call-ptr under --interpret, got: $out"
    else
        pass "jit-ffi-call-ptr-interp"
    fi
else
    echo "SKIP jit-ffi-call-ptr-interp (needs a JIT build on Linux)"
fi

# jit-ffi-interp-parametric-record-field: a record field whose type is a
# concrete application of a parametric record -- `(BoxW int32)`, `(Box float)`
# -- marshals under --interpret and agrees with the compiled path.
#
# It used to be refused: the F4 marshaller read field types from the GENERIC
# def, where a type-variable field reports the int64 carrier, so it could not
# describe the monomorph codegen actually emits. Measured, that monomorph is
# `struct tur_adt_Box__float { double x; }` inlined by value -- so describing
# `x` as an int64 would hand the callee eight bytes of reinterpreted double.
# Refusing was right; rendering it correctly is better.
#
# The float case is the one that matters: an int32 field survives a wrong
# member code by accident, a double does not. `fouter_check` returns 1 only if
# the double arrived intact AND the sibling int32 did too, so a layout slip
# shows up as 0 rather than as a plausible-looking number.
#
# Needs cc to build the callee; skipped where it is unavailable.
if [ "$HAS_JIT" = "1" ] && command -v cc >/dev/null 2>&1; then
    _pf_dir=$(mktemp -d)
    case "$(uname)" in
        Darwin) _pf_lib="$_pf_dir/libprobe.dylib" ;;
        *)      _pf_lib="$_pf_dir/libprobe.so"    ;;
    esac
    cat > "$_pf_dir/probe.c" <<'PROBEC'
#include <stdint.h>
struct BoxWi { int32_t raw; };
struct Outer { struct BoxWi b; int32_t tag; };
int32_t outer_sum(struct Outer o) { return o.b.raw * 10 + o.tag; }
struct Boxf { double x; };
struct FOuter { struct Boxf b; int32_t tag; };
int32_t fouter_check(struct FOuter o) {
    return (o.b.x > 1.49 && o.b.x < 1.51 && o.tag == 7) ? 1 : 0;
}
PROBEC
    if cc -shared -fPIC -o "$_pf_lib" "$_pf_dir/probe.c" 2>/dev/null; then
        cat > "$TMP_FFI" <<TURFFI
(defstruct BoxW [a] (raw :int32))
(defstruct Outer [b : (BoxW int32) tag : int32])
(defstruct Box [a] (x a))
(defstruct FOuter [b : (Box float) tag : int32])
(defn main [] : int
  (unsafe
    (let [h (dlopen "$_pf_lib")
          p (dlsym h "outer_sum")
          q (dlsym h "fouter_check")]
      (println (call-ptr p [Outer -> :int32]
                         (make-struct Outer (make-struct BoxW (:: 4 :int32)) (:: 2 :int32))))
      (println (call-ptr q [FOuter -> :int32]
                         (make-struct FOuter (make-struct Box 1.5) (:: 7 :int32))))))
  0)
TURFFI
        # stdout and stderr are kept apart: the sanitized build prints ASan's
        # makecontext/swapcontext warning on stderr for this program, and a
        # `2>&1` capture folded it into the text compared against '42\n1', so
        # the check failed on a correct answer (it did, on the ubuntu JIT
        # leg, where continue-on-error hid it). The refusal diagnostic is
        # still looked for on both streams; only stdout is compared.
        _pf_err=$(mktemp)
        out=$(ASAN_OPTIONS=detect_leaks=0 "$TUR" --interpret "$TMP_FFI" 2>"$_pf_err")
        want=$(printf '42\n1')
        if grep -q "no by-value C member type\|parametric monomorph" "$_pf_err" ||
           grep -q "no by-value C member type\|parametric monomorph" <<< "$out"; then
            fail "jit-ffi-interp-parametric-record-field" "still refused: $out $(cat "$_pf_err")"
        elif [ "$out" != "$want" ]; then
            fail "jit-ffi-interp-parametric-record-field" "expected '42' then '1' (the compiled path's answer), got: $out (stderr: $(cat "$_pf_err"))"
        else
            pass "jit-ffi-interp-parametric-record-field"
        fi
        rm -f "$_pf_err"
    else
        echo "SKIP jit-ffi-interp-parametric-record-field (cc could not build the probe)"
    fi
    rm -rf "$_pf_dir"
else
    echo "SKIP jit-ffi-interp-parametric-record-field (needs a JIT build and cc)"
fi

# jit-cc-fallback-reentry-monomorph-state: the step-6 cc fallback must not
# inherit the abandoned engine attempt's monomorph registry.
#
# cmd_jit falls back by calling cmd_run IN THE SAME PROCESS, so mono_specs.c's
# file-scope registry survives. cmd_run then elaborates a second time against a
# registry that already believes the van-Laarhoven specs were requested, and
# the emitted C CALLS `over_px__lens_<hash>` while nothing emits its
# definition -- a hard cc error, from a program `tur run` compiles cleanly.
#
# The path is otherwise unreachable: with __auto_type gone from the emitter the
# engine no longer declines these fixtures, which is exactly how a latent bug
# waits to be rediscovered. TUR_JIT_FORCE_FALLBACK=1 declines the engine
# deliberately (before executing, so the program is not run twice).
if [ "$HAS_JIT" = "1" ]; then
    _fb_fx="tests/fixtures/van-laarhoven-lens-wide-mono"
    if [ -f "$_fb_fx/input.tur" ]; then
        _fb_err=$(mktemp)
        out=$(TUR_JIT_FORCE_FALLBACK=1 ASAN_OPTIONS=detect_leaks=0 \
                  "$TUR" jit "$_fb_fx/input.tur" 2>"$_fb_err")
        if grep -q "undeclared function" "$_fb_err"; then
            fail "jit-cc-fallback-reentry-monomorph-state" \
                 "the fallback re-entry emitted a call to a monomorph spec it never defined: $(grep -m1 'undeclared function' "$_fb_err")"
        elif [ "$out" != "$(cat "$_fb_fx/expected.stdout")" ]; then
            fail "jit-cc-fallback-reentry-monomorph-state" \
                 "forced fallback produced: $out"
        else
            pass "jit-cc-fallback-reentry-monomorph-state"
        fi
        rm -f "$_fb_err"
    else
        echo "SKIP jit-cc-fallback-reentry-monomorph-state (fixture missing)"
    fi
else
    echo "SKIP jit-cc-fallback-reentry-monomorph-state (needs a JIT build)"
fi

# jit-ffi-call-ptr-nonjit-diag: a JIT-less interpreter reports a clean
# "requires a JIT-enabled build" diagnostic for call-ptr -- never nil, never
# a crash.  (In a JIT build the call succeeds instead, so only the JIT-less
# side of the fork is asserted here.)
if [ "$HAS_JIT" = "0" ]; then
    printf '(defn main [] : int\n  (unsafe (println (call-ptr 1 [-> :int])))\n  0)\n' > "$TMP_FFI"
    out=$(ASAN_OPTIONS=detect_leaks=0 "$TUR" --interpret "$TMP_FFI" 2>&1)
    if ! grep -q "requires a JIT-enabled build" <<< "$out"; then
        fail "jit-ffi-call-ptr-nonjit-diag" "expected the clean non-JIT diagnostic, got: $out"
    else
        pass "jit-ffi-call-ptr-nonjit-diag"
    fi
    # F5's half of the same contract: callback-ptr also needs the engine to
    # synthesize its callback, and must say so rather than return nil.
    cat > "$TMP_FFI" <<'TURFFI'
(defn cb [x : int] : int x)
(defn main [] : int
  (unsafe (println (call-ptr 1 [:ptr -> :int] (callback-ptr cb [:int -> :int]))))
  0)
TURFFI
    out=$(ASAN_OPTIONS=detect_leaks=0 "$TUR" --interpret "$TMP_FFI" 2>&1)
    if ! grep -q "requires a JIT-enabled build" <<< "$out"; then
        fail "jit-ffi-callback-nonjit-diag" "expected the clean non-JIT diagnostic, got: $out"
    else
        pass "jit-ffi-callback-nonjit-diag"
    fi
else
    echo "SKIP jit-ffi-call-ptr-nonjit-diag (this build has the engine)"
    echo "SKIP jit-ffi-callback-nonjit-diag (this build has the engine)"
fi

# jit-ffi-call-ptr-struct-interp: F4 struct-by-value through the interpreter's
# own marshaller (TuriStruct -> C bytes -> TuriStruct), for the aggregate
# classes every backend agrees on: all-integer, and mixed int/float.  Covers
# both an aggregate ARGUMENT and an aggregate RETURN.  The compiled half --
# including the floating-point aggregate the interpreter refuses below -- is
# tests/fixtures/jit-ffi-call-ptr-struct.
#
# `div` is the portable choice of callee: every libc declares it, and it
# RETURNS div_t {int quot; int rem;} by value -- an all-integer aggregate.
# Only the soname differs per platform (turi's dlopen has no spelling for
# "this process", so the library is named explicitly).
case "$(uname)" in
  Darwin) LIBC_SO="/usr/lib/libSystem.B.dylib" ;;
  Linux)  LIBC_SO="libc.so.6" ;;
  *)      LIBC_SO="" ;;
esac
# Shared-library extension for the helper libraries the jit-ffi cases below
# build with cc.  Load-bearing on macOS, not cosmetic: Apple's linker
# resolves `-lfoo` against libfoo.dylib / libfoo.tbd / libfoo.a and does NOT
# consider libfoo.so, so an autolinked helper named .so fails to LINK there
# and the fixture dies before printing anything.  (dlopen by absolute path
# does not care about the extension, but there is no reason for the two
# cases to disagree.)
case "$(uname)" in
  Darwin) SOEXT="dylib" ;;
  *)      SOEXT="so" ;;
esac
if [ "$HAS_JIT" = "1" ] && [ -n "$LIBC_SO" ]; then
    cat > "$TMP_FFI" <<TURFFI
(defstruct IPair [a : int32 b : int32])
(defn main [] : int
  (unsafe
    (let [h  (dlopen "$LIBC_SO")
          pd (dlsym h "div")]
      (let [r (call-ptr pd [:int :int -> IPair] 47 10)]
        (println (:: (.a r) :int))
        (println (:: (.b r) :int)))))
  0)
TURFFI
    out=$(ASAN_OPTIONS=detect_leaks=0 "$TUR" --interpret "$TMP_FFI" 2>/dev/null)
    if [ "$(echo "$out" | tr '\n' ' ')" != "4 7 " ]; then
        fail "jit-ffi-call-ptr-struct-interp" "expected div(47,10) = {4,7} unpacked from an aggregate return, got: $out"
    else
        pass "jit-ffi-call-ptr-struct-interp"
    fi
else
    echo "SKIP jit-ffi-call-ptr-struct-interp (needs a JIT build on a known libc)"
fi

# jit-ffi-call-ptr-struct-nested-interp: a NESTED by-value record field is
# inlined in the emitted C layout (adt_field_is_inline_byval), so the
# interpreter's sig must render it as a nested `{...}` -- it used to class
# the field as an int64 carrier ('q'), building a thunk whose ABI shape
# disagreed with the natively compiled callee (silent wrong answers, the
# same failure mode as the aarch64 HFA report but on every arch).  The
# callee is compiled with the native cc so the thunk really crosses the
# c2mir <-> native ABI boundary; both directions are asserted.
if [ "$HAS_JIT" = "1" ] && command -v cc >/dev/null 2>&1; then
    _nested_dir="$(mktemp -d -t tur-ffi-nested-XXXXXX)"
    # Mixed int/float leaves, NOT an HFA -- an all-float nested aggregate
    # would (correctly) hit the aarch64 HFA refusal instead of the marshaller.
    cat > "$_nested_dir/helper.c" <<'EOF'
typedef struct { int a; float b; } IW;
typedef struct { IW lo; double c; } NX;
double nx_sum(NX v) { return (double)v.lo.a * 10000.0
                           + (double)v.lo.b * 100.0 + v.c; }
NX mk_nx(int a, double b, double c) {
    NX r; r.lo.a = a; r.lo.b = (float)b; r.c = c; return r;
}
EOF
    if cc -shared -fPIC -o "$_nested_dir/libnested.$SOEXT" "$_nested_dir/helper.c" 2>/dev/null; then
        cat > "$TMP_FFI" <<TURFFI
(defstruct IW [a : int32 b : float32])
(defstruct NX [lo : IW c : float])
(defn main [] : int
  (unsafe
    (let [h (dlopen "$_nested_dir/libnested.$SOEXT")]
      (println (call-ptr (dlsym h "nx_sum") [NX -> :float]
                         (NX (IW (:: 3 :int32) (:: 2.25 :float32)) 1.5)))
      (let [r (call-ptr (dlsym h "mk_nx") [:int :float :float -> NX]
                        3 2.25 1.5)]
        (println (:: (.a (.lo r)) :int))
        (println (:: (.b (.lo r)) :float))
        (println (.c r)))))
  0)
TURFFI
        out=$(ASAN_OPTIONS=detect_leaks=0 "$TUR" --interpret "$TMP_FFI" 2>/dev/null)
        if [ "$(echo "$out" | tr '\n' ' ')" != "30226.5 3 2.25 1.5 " ]; then
            fail "jit-ffi-call-ptr-struct-nested-interp" "expected nested aggregate to marshal as 30226.5 / 3 / 2.25 / 1.5, got: $out"
        else
            pass "jit-ffi-call-ptr-struct-nested-interp"
        fi
    else
        echo "SKIP jit-ffi-call-ptr-struct-nested-interp (cc could not build the helper .so)"
    fi
    rm -rf "$_nested_dir"
else
    echo "SKIP jit-ffi-call-ptr-struct-nested-interp (needs a JIT build and cc)"
fi

# jit-ffi-extern-c-struct-interp: extern-c with by-value AGGREGATE slots
# (the plan's "extern-c with aggregate parameters" open item).  div(3)
# RETURNS div_t {int quot; int rem;}; inet_ntoa(3) TAKES struct in_addr by
# value.  Both live in the already-loaded libc, so dlsym(RTLD_DEFAULT)
# resolves them at registration with no dlopen.  0x0100007F -> "127.0.0.1".
if [ "$HAS_JIT" = "1" ] && [ -n "$LIBC_SO" ]; then
    cat > "$TMP_FFI" <<'TURFFI'
(defstruct DivT [quot : int32 rem : int32])
(defstruct InAddr [addr : uint32])
(extern-c div [a :int b :int] : DivT)
(extern-c inet_ntoa [a : InAddr] : cstr)
(defn main [] : int
  (let [r (div 47 10)]
    (println (:: (.quot r) :int))
    (println (:: (.rem r) :int)))
  (println (inet_ntoa (InAddr (:: 16777343 :uint32))))
  0)
TURFFI
    out=$(ASAN_OPTIONS=detect_leaks=0 "$TUR" --interpret "$TMP_FFI" 2>/dev/null)
    if [ "$(echo "$out" | tr '\n' ' ')" != "4 7 127.0.0.1 " ]; then
        fail "jit-ffi-extern-c-struct-interp" "expected div/inet_ntoa through aggregate extern-c to print 4 / 7 / 127.0.0.1, got: $out"
    else
        pass "jit-ffi-extern-c-struct-interp"
    fi
else
    echo "SKIP jit-ffi-extern-c-struct-interp (needs a JIT build on a known libc)"
fi

# jit-ffi-extern-c-struct-compiled: the same declarations on the COMPILED
# path -- the prototype emitter spells the record's C type
# (`extern tur_adt_IqFd mk_iqfd(...)`), the C linker binds it to the
# layout-identical symbol in the helper .so, and result_full_type is what
# makes `.field` on the aggregate return resolve.  Works in every build
# (pure codegen; no JIT needed).
if command -v cc >/dev/null 2>&1; then
    _ecagg_dir="$(mktemp -d -t tur-ecagg-XXXXXX)"
    cat > "$_ecagg_dir/helper.c" <<'EOF'
typedef struct { long long n; double d; } IqFd;
double iqfd_sum(IqFd v) { return (double)v.n * 100.0 + v.d; }
IqFd mk_iqfd(long long n, double d) { IqFd r = { n, d }; return r; }
EOF
    if cc -shared -fPIC -o "$_ecagg_dir/libecagg.$SOEXT" "$_ecagg_dir/helper.c" 2>/dev/null; then
        cat > "$TMP_FFI" <<TURFFI
(defstruct IqFd [n : int d : float])
(defn __link [] : int
  \`\`\`c
  /* __tur_autolink__: -L$_ecagg_dir -lecagg */
  return 0;
  \`\`\`)
(extern-c iqfd_sum [v : IqFd] : float)
(extern-c mk_iqfd [n :int d :float] : IqFd)
(defn main [] : int
  (println (iqfd_sum (IqFd 42 6.125)))
  (let [r (mk_iqfd 42 6.125)]
    (println (.n r))
    (println (.d r)))
  0)
TURFFI
        # stderr is CAPTURED, not discarded: when this failed on the macOS
        # leg it reported `got: ` with no other information, which says the
        # program never printed -- i.e. it died in cc -- but not why.  A
        # compile/link diagnostic in the failure line is the difference
        # between one push and a blind round trip.
        _ec_err="$_ecagg_dir/stderr.txt"
        out=$(LD_LIBRARY_PATH="$_ecagg_dir" DYLD_LIBRARY_PATH="$_ecagg_dir" "$TUR" run "$TMP_FFI" 2>"$_ec_err")
        if [ "$(echo "$out" | tr '\n' ' ')" != "4206.12 42 6.125 " ]; then
            fail "jit-ffi-extern-c-struct-compiled" "expected compiled aggregate extern-c to print 4206.12 / 42 / 6.125, got: $out; stderr: $(tr '\n' ' ' < "$_ec_err" | tail -c 600)"
        else
            pass "jit-ffi-extern-c-struct-compiled"
        fi
    else
        echo "SKIP jit-ffi-extern-c-struct-compiled (cc could not build the helper .so)"
    fi
    rm -rf "$_ecagg_dir"
else
    echo "SKIP jit-ffi-extern-c-struct-compiled (needs cc)"
fi

# jit-ffi-extern-c-struct-rejects-heap: a :heap record's ABI is a pointer
# to its header, not the aggregate -- naming one in an extern-c slot is a
# hard elaboration error on every build.
cat > "$TMP_FFI" <<'TURFFI'
(defstruct Boxy :heap [n : int])
(extern-c bad_fn [v : Boxy] : int)
(defn main [] : int 0)
TURFFI
out=$("$TUR" emit-c "$TMP_FFI" 2>&1); rc=$?
if [ $rc -eq 0 ]; then
    fail "jit-ffi-extern-c-struct-rejects-heap" "a :heap record was accepted as a by-value extern-c parameter"
elif ! grep -q "cannot cross the C boundary by value" <<< "$out"; then
    fail "jit-ffi-extern-c-struct-rejects-heap" "expected the by-value aggregate diagnostic, got: $out"
else
    pass "jit-ffi-extern-c-struct-rejects-heap"
fi

# jit-ffi-call-ptr-hfa: an AAPCS64 Homogeneous Floating-point Aggregate (all
# members the same FP type, 1-4 of them) travels in v0..v7.  MIR's aarch64
# backend had no HFA class and passed every aggregate in x0..x7, so a c2mir
# thunk calling a natively compiled callee read the wrong registers and
# produced a DATA-DEPENDENT wrong answer -- `tur run` printed 152.25 where
# `tur jit` printed 225.  Fixed in the pinned MIR fork; see
# docs/archive/mir-aarch64-fp-aggregate-abi.md.
#
# These assert the ANSWER, not a diagnostic: the failure mode was never a
# crash.  The callee is compiled with the native cc so the call really crosses
# the c2mir <-> native ABI boundary.
#
# Note the single-member cases are not redundant with the two-member ones. A
# naive one-argument probe of a 1-member HFA PASSES even when broken, because
# the value wrongly read out of v0 is very often the one the caller just
# materialized there on its way to the store.  Two HFA arguments disambiguate:
# at most one can be accidentally right.
case "$(uname -m)" in
  arm64|aarch64) HFA_HOST=1 ;;
  *)             HFA_HOST=0 ;;
esac
if [ "$HAS_JIT" = "1" ] && [ "$HFA_HOST" = "1" ] && command -v cc >/dev/null 2>&1; then
    _hfa_dir="$(mktemp -d -t tur-ffi-hfa-XXXXXX)"
    cat > "$_hfa_dir/helper.c" <<'EOF'
typedef struct { double a, b; } D2;
typedef struct { float a, b; }  F2;
typedef struct { double x; }    D1;
double h_d2 (D2 v)        { return v.a * 100.0 + v.b; }
double h_f2 (F2 v)        { return (double) v.a * 100.0 + v.b; }
double h_d1x2 (D1 a, D1 b) { return a.x * 100.0 + b.x; }
D2     h_ret (void)       { D2 r = {1.5, 2.25}; return r; }
EOF
    if cc -shared -fPIC -Wl,-install_name,"$_hfa_dir/libhfa.$SOEXT" \
          -o "$_hfa_dir/libhfa.$SOEXT" "$_hfa_dir/helper.c" 2>/dev/null; then
        cat > "$TMP_FFI" <<TURFFI
(defstruct D2 [a : float b : float])
(defstruct F2 [a : float32 b : float32])
(defstruct D1 [x : float])
(defn main [] : int
  (unsafe
    (let [h (dlopen "$_hfa_dir/libhfa.$SOEXT")]
      (println (call-ptr (dlsym h "h_d2") [D2 -> :float] (D2 1.5 2.25)))
      (println (call-ptr (dlsym h "h_f2") [F2 -> :float]
                         (F2 (:: 1.5 :float32) (:: 2.25 :float32))))
      (println (call-ptr (dlsym h "h_d1x2") [D1 D1 -> :float] (D1 3.0) (D1 4.0)))
      (let [r (call-ptr (dlsym h "h_ret") [-> D2])]
        (println (.a r))
        (println (.b r)))))
  0)
TURFFI
        out=$(ASAN_OPTIONS=detect_leaks=0 "$TUR" --interpret "$TMP_FFI" 2>/dev/null \
              | tr '\n' ' ')
        if [ "$out" != "152.25 152.25 304 1.5 2.25 " ]; then
            fail "jit-ffi-call-ptr-hfa" \
                 "expected '152.25 152.25 304 1.5 2.25 ' from HFA call-ptr, got: $out"
        else
            pass "jit-ffi-call-ptr-hfa"
        fi
    else
        echo "SKIP jit-ffi-call-ptr-hfa (helper library did not build)"
    fi
    rm -rf "$_hfa_dir"
else
    echo "SKIP jit-ffi-call-ptr-hfa (needs a JIT build on aarch64 with cc)"
fi

# jit-ffi-extern-c-hfa-compiled: the same shape on the COMPILED side.  The test
# above covers the interpreter's thunk engine; this covers `tur jit`, where the
# whole program goes through c2mir.  This is the case that had no diagnostic at
# all before the MIR fix: `tur run` printed 152.25 and `tur jit` printed 226.5
# for the same source, exit 0 both times.
#
# Asserted against `tur run` rather than a literal, so the two backends are
# compared directly -- the property that actually matters is that they agree.
if [ "$HAS_JIT" = "1" ] && [ "$HFA_HOST" = "1" ] && command -v cc >/dev/null 2>&1; then
    _hfa2_dir="$(mktemp -d -t tur-ffi-hfa2-XXXXXX)"
    cat > "$_hfa2_dir/helper.c" <<'EOF'
typedef struct { double a, b; } D2;
double h_ec_d2 (D2 v) { return v.a * 100.0 + v.b; }
EOF
    if cc -shared -fPIC -Wl,-install_name,"$_hfa2_dir/libhfa2.$SOEXT" \
          -o "$_hfa2_dir/libhfa2.$SOEXT" "$_hfa2_dir/helper.c" 2>/dev/null; then
        cat > "$TMP_FFI" <<TURFFI
(defstruct D2 [a : float b : float])
(extern-c h_ec_d2 [v : D2] : float)
(defn linkme [] : int
  \`\`\`c
  /* __tur_autolink__: -L$_hfa2_dir -lhfa2 */
  return 0;
  \`\`\`)
(defn main [] : int
  (let [_ (linkme)]
    (println (h_ec_d2 (D2 1.5 2.25)))
    0))
TURFFI
        jit_out=$("$TUR" jit "$TMP_FFI" 2>/dev/null | tr -d '[:space:]')
        native_out=$("$TUR" run "$TMP_FFI" 2>/dev/null | tr -d '[:space:]')
        if [ "$native_out" != "152.25" ]; then
            fail "jit-ffi-extern-c-hfa-compiled" \
                 "native cc path is itself wrong (got '$native_out'); the test is broken"
        elif [ "$jit_out" != "$native_out" ]; then
            fail "jit-ffi-extern-c-hfa-compiled" \
                 "tur jit disagrees with tur run on an HFA extern-c: '$jit_out' vs '$native_out'"
        else
            pass "jit-ffi-extern-c-hfa-compiled"
        fi
    else
        echo "SKIP jit-ffi-extern-c-hfa-compiled (helper library did not build)"
    fi
    rm -rf "$_hfa2_dir"
else
    echo "SKIP jit-ffi-extern-c-hfa-compiled (needs a JIT build on aarch64 with cc)"
fi

# jit-ffi-callback-hfa: the INBOUND direction, which is the mirror hazard and
# the only one that exercises the callee-side half of the MIR fix (the arg
# gather in target_machinize).  Here a NATIVELY compiled caller writes v0..v7
# and a c2mir-generated callback must read them from the same place; before the
# fix the callback read x0..x7.  Nothing else in this file covers that half.
if [ "$HAS_JIT" = "1" ] && [ "$HFA_HOST" = "1" ] && command -v cc >/dev/null 2>&1; then
    _cb_dir="$(mktemp -d -t tur-ffi-cbhfa-XXXXXX)"
    cat > "$_cb_dir/helper.c" <<'EOF'
typedef struct { double a, b; } D2;
double call_cb_d2 (double (*cb) (D2), double x, double y) { D2 v = {x, y}; return cb (v); }
EOF
    if cc -shared -fPIC -Wl,-install_name,"$_cb_dir/libcbhfa.$SOEXT" \
          -o "$_cb_dir/libcbhfa.$SOEXT" "$_cb_dir/helper.c" 2>/dev/null; then
        cat > "$TMP_FFI" <<TURFFI
(defstruct D2 [a : float b : float])
(defn cb-d2 [v : D2] : float (+ (* (.a v) 100.0) (.b v)))
(defn main [] : int
  (unsafe
    (let [h (dlopen "$_cb_dir/libcbhfa.$SOEXT")]
      (println (call-ptr (dlsym h "call_cb_d2") [:ptr :float :float -> :float]
                         (callback-ptr cb-d2 [D2 -> :float]) 1.5 2.25))))
  0)
TURFFI
        out=$(ASAN_OPTIONS=detect_leaks=0 "$TUR" --interpret "$TMP_FFI" 2>/dev/null \
              | tr -d '[:space:]')
        if [ "$out" != "152.25" ]; then
            fail "jit-ffi-callback-hfa" \
                 "expected 152.25 from an HFA passed INTO a callback, got: $out"
        else
            pass "jit-ffi-callback-hfa"
        fi
    else
        echo "SKIP jit-ffi-callback-hfa (helper library did not build)"
    fi
    rm -rf "$_cb_dir"
else
    echo "SKIP jit-ffi-callback-hfa (needs a JIT build on aarch64 with cc)"
fi

# jit-ffi-extern-c-nonhfa-still-jits: proof the HFA classification is narrow.
# A mixed-member aggregate is INTEGER-class under AAPCS64, not an HFA, so it
# must keep travelling in x0..x7 exactly as a natively compiled callee expects.
# Without this, widening the classifier to "any aggregate containing a float"
# would still look green while silently breaking a large amount of code that
# works today.
if [ "$HAS_JIT" = "1" ] && [ "$HFA_HOST" = "1" ] && command -v cc >/dev/null 2>&1; then
    _mix_dir="$(mktemp -d -t tur-ffi-mix-XXXXXX)"
    cat > "$_mix_dir/helper.c" <<'EOF'
typedef struct { double a; long long b; } MixT;
double __sbv_mix(MixT v) { return v.a * 100.0 + (double)v.b; }
EOF
    if cc -shared -fPIC -Wl,-install_name,"$_mix_dir/libmix.$SOEXT" \
          -o "$_mix_dir/libmix.$SOEXT" "$_mix_dir/helper.c" 2>/dev/null; then
        cat > "$TMP_FFI" <<TURFFI
(defstruct MixT [a : float b : int])
(extern-c __sbv_mix [v : MixT] : float)
(defn linkme [] : int
  \`\`\`c
  /* __tur_autolink__: -L$_mix_dir -lmix */
  return 0;
  \`\`\`)
(defn main [] : int
  (let [_ (linkme)
        v (MixT 1.5 2)]
    (println (__sbv_mix v))
    0))
TURFFI
        out=$("$TUR" jit "$TMP_FFI" 2>/dev/null | tr -d '[:space:]')
        if [ "$out" != "152" ]; then
            fail "jit-ffi-extern-c-nonhfa-still-jits" \
                 "expected 152 from an INTEGER-class aggregate under \`tur jit\`, got: $out"
        else
            pass "jit-ffi-extern-c-nonhfa-still-jits"
        fi
    else
        echo "SKIP jit-ffi-extern-c-nonhfa-still-jits (helper library did not build)"
    fi
    rm -rf "$_mix_dir"
else
    echo "SKIP jit-ffi-extern-c-nonhfa-still-jits (needs a JIT build on aarch64 with cc)"
fi

# jit-ffi-callback-interp: F5 in the direction only the interpreter exercises
# -- a c2mir-generated callback with the context address baked in as a
# literal, calling back through tur_ffi_cb_dispatch into a Turmeric function.
# The plan's acceptance case: qsort(3) with a Turmeric comparator.
if [ "$HAS_JIT" = "1" ] && [ -n "$LIBC_SO" ]; then
    cat > "$TMP_FFI" <<TURFFI
(defn intcmp [a : ptr b : ptr] : int
  (unsafe (- (ptr-deref a) (ptr-deref b))))
(defn main [] : int
  (unsafe
    (let [h   (dlopen "$LIBC_SO")
          pq  (dlsym h "qsort")
          buf (raw-malloc 24)
          cmp (callback-ptr intcmp [:ptr :ptr -> :int])]
      (ptr-write buf 30)
      (ptr-write (ptr-add buf 8) 10)
      (ptr-write (ptr-add buf 16) 20)
      (call-ptr pq [:ptr :int :int :ptr -> :void] buf 3 8 cmp)
      (println (ptr-deref buf))
      (println (ptr-deref (ptr-add buf 8)))
      (println (ptr-deref (ptr-add buf 16)))))
  0)
TURFFI
    out=$(ASAN_OPTIONS=detect_leaks=0 "$TUR" --interpret "$TMP_FFI" 2>/dev/null)
    if [ "$(echo "$out" | tr '\n' ' ')" != "10 20 30 " ]; then
        fail "jit-ffi-callback-interp" "expected qsort driven by a Turmeric comparator to sort 30,10,20, got: $out"
    else
        pass "jit-ffi-callback-interp"
    fi
else
    echo "SKIP jit-ffi-callback-interp (needs a JIT build on a known libc)"
fi

# jit-ffi-narrow-return: scalar-width fidelity.  A C callee returning `int`
# (or `short`) leaves the upper bits of the return register unspecified, so
# a thunk that declared the call as returning `long long` read garbage for
# any negative value -- neg_int(1234) came back as 4294966062, not -1234.
# The sig now carries exact-width scalar codes, so the thunk declares the
# callee's true C type and the cast extends correctly.
if [ "$HAS_JIT" = "1" ] && command -v cc >/dev/null 2>&1; then
    _narrow_dir="$(mktemp -d -t tur-narrow-XXXXXX)"
    cat > "$_narrow_dir/helper.c" <<'EOF'
int neg_int(int x) { return -x; }
short neg_short(short x) { return (short)-x; }
unsigned int big_uint(void) { return 4294967295u; }
EOF
    if cc -shared -fPIC -o "$_narrow_dir/libnarrow.$SOEXT" "$_narrow_dir/helper.c" 2>/dev/null; then
        cat > "$TMP_FFI" <<TURFFI
(defn main [] : int
  (unsafe
    (let [h (dlopen "$_narrow_dir/libnarrow.$SOEXT")]
      (println (:: (call-ptr (dlsym h "neg_int")   [:int32 -> :int32] 1234) :int))
      (println (:: (call-ptr (dlsym h "neg_short") [:int16 -> :int16] 77) :int))
      (println (:: (call-ptr (dlsym h "big_uint")  [-> :uint32]) :int))))
  0)
TURFFI
        out=$(ASAN_OPTIONS=detect_leaks=0 "$TUR" --interpret "$TMP_FFI" 2>/dev/null)
        if [ "$(echo "$out" | tr '\n' ' ')" != "-1234 -77 4294967295 " ]; then
            fail "jit-ffi-narrow-return" "expected exact-width returns -1234 / -77 / 4294967295, got: $out"
        else
            pass "jit-ffi-narrow-return"
        fi
    else
        echo "SKIP jit-ffi-narrow-return (cc could not build the helper .so)"
    fi
    rm -rf "$_narrow_dir"
else
    echo "SKIP jit-ffi-narrow-return (needs a JIT build and cc)"
fi

# jit-ffi-callback-struct: aggregates INBOUND to a callback (the last F4/F5
# open item) -- a native caller passes a record by value into a Turmeric
# comparator-style function, and a callback RETURNS a record by value that
# the native caller hands back.  Mixed int/float members (not an HFA), so it
# runs on aarch64 too.  Asserted on both paths: the interpreter's dispatch
# rebuilds the record from raw bytes; the compiled per-site adapter passes
# the aggregate through uncast.
if command -v cc >/dev/null 2>&1; then
    _cbagg_dir="$(mktemp -d -t tur-cbagg-XXXXXX)"
    cat > "$_cbagg_dir/helper.c" <<'EOF'
typedef struct { long long n; double d; } IqFd;
long long call_n(long long (*cb)(IqFd), long long n, double d) {
    IqFd v = { n, d }; return cb(v);
}
double call_d(double (*cb)(IqFd), long long n, double d) {
    IqFd v = { n, d }; return cb(v);
}
IqFd call_mk(IqFd (*cb)(long long, double), long long n, double d) {
    return cb(n, d);
}
EOF
    if cc -shared -fPIC -o "$_cbagg_dir/libcbagg.$SOEXT" "$_cbagg_dir/helper.c" 2>/dev/null; then
        cat > "$TMP_FFI" <<TURFFI
(defstruct IqFd [n : int d : float])
(defn iqfd-n [v : IqFd] : int   (.n v))
(defn iqfd-d [v : IqFd] : float (.d v))
(defn mk-iqfd [n : int d : float] : IqFd (IqFd n d))
(defn main [] : int
  (unsafe
    (let [h (dlopen "$_cbagg_dir/libcbagg.$SOEXT")]
      (println (call-ptr (dlsym h "call_n") [:ptr :int :float -> :int]
                         (callback-ptr iqfd-n [IqFd -> :int]) 42 6.125))
      (println (call-ptr (dlsym h "call_d") [:ptr :int :float -> :float]
                         (callback-ptr iqfd-d [IqFd -> :float]) 42 6.125))
      (let [r (call-ptr (dlsym h "call_mk") [:ptr :int :float -> IqFd]
                        (callback-ptr mk-iqfd [:int :float -> IqFd]) 42 6.125)]
        (println (.n r))
        (println (.d r)))))
  0)
TURFFI
        if [ "$HAS_JIT" = "1" ]; then
            out=$(ASAN_OPTIONS=detect_leaks=0 "$TUR" --interpret "$TMP_FFI" 2>/dev/null)
            if [ "$(echo "$out" | tr '\n' ' ')" != "42 6.125 42 6.125 " ]; then
                fail "jit-ffi-callback-struct-interp" "expected inbound/outbound callback aggregates to print 42 / 6.125 / 42 / 6.125, got: $out"
            else
                pass "jit-ffi-callback-struct-interp"
            fi
        else
            echo "SKIP jit-ffi-callback-struct-interp (needs a JIT build)"
        fi
        # stderr CAPTURED -- see the note on jit-ffi-extern-c-struct-compiled.
        _cb_err="$_cbagg_dir/stderr.txt"
        out=$("$TUR" run "$TMP_FFI" 2>"$_cb_err")
        if [ "$(echo "$out" | tr '\n' ' ')" != "42 6.125 42 6.125 " ]; then
            fail "jit-ffi-callback-struct-compiled" "expected inbound/outbound callback aggregates to print 42 / 6.125 / 42 / 6.125, got: $out; stderr: $(tr '\n' ' ' < "$_cb_err" | tail -c 600)"
        else
            pass "jit-ffi-callback-struct-compiled"
        fi
    else
        echo "SKIP jit-ffi-callback-struct-interp (cc could not build the helper .so)"
        echo "SKIP jit-ffi-callback-struct-compiled (cc could not build the helper .so)"
    fi
    rm -rf "$_cbagg_dir"
else
    echo "SKIP jit-ffi-callback-struct-interp (needs cc)"
    echo "SKIP jit-ffi-callback-struct-compiled (needs cc)"
fi

# jit-ffi-callback-needs-toplevel: a CAPTURING closure cannot become a C
# callback -- the slot is a bare function pointer with no room for the
# environment.  (A non-capturing lambda IS accepted: it lifts to a plain C
# function, which is the whole criterion.)  Enforced in ELABORATION so both
# paths agree; asserted on the compiled path, which every build has.
cat > "$TMP_FFI" <<'TURFFI'
(defn main [] : int
  (unsafe
    (let [n  3
          cb (callback-ptr (fn [x : int] : int (+ x n)) [:int -> :int])]
      (println (call-ptr 1 [:ptr -> :int] cb))))
  0)
TURFFI
out=$("$TUR" emit-c "$TMP_FFI" 2>&1); rc=$?
if [ $rc -eq 0 ]; then
    fail "jit-ffi-callback-needs-toplevel" "a lambda was accepted as a C callback"
elif ! grep -q "captured environment" <<< "$out"; then
    fail "jit-ffi-callback-needs-toplevel" "expected the top-level-function diagnostic, got: $out"
else
    pass "jit-ffi-callback-needs-toplevel"
fi

# jit-ffi-graduated: the experiment graduated 2026-08-21, so the form needs no
# --enable on any build.  This replaces the old jit-ffi-gate case, which
# asserted the opposite.
printf '(defn main [] : int\n  (unsafe (println (call-ptr 1 [-> :int])))\n  0)\n' > "$TMP_FFI"
out=$("$TUR" emit-c "$TMP_FFI" 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "jit-ffi-graduated" "call-ptr did not compile without a flag: $out"
elif grep -q "enable=jit-ffi" <<< "$out"; then
    fail "jit-ffi-graduated" "still pointing at the retired --enable=jit-ffi: $out"
else
    pass "jit-ffi-graduated"
fi

# jit-ffi-enable-noop: a downstream build.tur / command line that still says
# --enable=jit-ffi keeps compiling, as an accept-and-warn TUR-W0063 no-op
# rather than the hard TUR-E0310 an unknown experiment name gets.
out=$("$TUR" --enable=jit-ffi emit-c "$TMP_FFI" 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    fail "jit-ffi-enable-noop" "a lingering --enable=jit-ffi failed the build: $out"
elif ! grep -q "TUR-W0063" <<< "$out"; then
    fail "jit-ffi-enable-noop" "expected the TUR-W0063 graduated no-op, got: $out"
else
    pass "jit-ffi-enable-noop"
fi

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo
echo "flags summary: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
