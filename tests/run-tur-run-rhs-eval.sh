#!/usr/bin/env bash
# run-tur-run-rhs-eval.sh -- regression for tur-run-just-assignment-eval-plan.
#
# `tur run` used to store Justfile `name := VALUE` assignments as raw byte
# fragments (truncated at whitespace) and interpolate them verbatim, which
# made any Justfile whose RHS was more than a bare word / quoted string
# unusable. This test drives the balanced RHS scanner + expression
# evaluator: function calls, string concat via '/' and '+', `if/else`
# conditionals, nested calls, forward-ref errors, and interop with
# environment variables.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TUR="${TUR:-$ROOT/build/tur}"

FAIL=0
pass() { echo "PASS: $1"; }
fail() { echo "FAIL: $1"; FAIL=1; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
cd "$TMP"

case "$(uname -s)" in
  Darwin) HOST_OS=macos ;;
  Linux)  HOST_OS=linux ;;
  *)      HOST_OS=unknown ;;
esac

# ---------------------------------------------------------------------------
# 1. Balanced-parens RHS: multi-arg function call with a comma survives the
#    scanner and evaluates through eval_builtin.
# ---------------------------------------------------------------------------
cat > Justfile <<'EOF'
val := env_var_or_default("TUR_RUN_TEST_UNSET", "fallback-value")

show:
    @echo "val={{val}}"
EOF
unset TUR_RUN_TEST_UNSET
OUT="$("$TUR" run show 2>&1)"
RC=$?
if [ "$RC" -eq 0 ] && grep -q 'val=fallback-value' <<< "$OUT"; then
  pass "balanced-parens: env_var_or_default default is honored"
else
  fail "balanced-parens: got rc=$RC out=$OUT"
fi

# ---------------------------------------------------------------------------
# 2. Slash concat: "build" / "debug" yields "build/debug".
# ---------------------------------------------------------------------------
cat > Justfile <<'EOF'
d := "build" / "debug"

show:
    @echo "d={{d}}"
EOF
OUT="$("$TUR" run show 2>&1)"
RC=$?
if [ "$RC" -eq 0 ] && grep -q 'd=build/debug' <<< "$OUT"; then
  pass "slash-concat: 'build' / 'debug' -> build/debug"
else
  fail "slash-concat: got rc=$RC out=$OUT"
fi

# ---------------------------------------------------------------------------
# 3. Plus concat: "foo" + "bar" yields "foobar".
# ---------------------------------------------------------------------------
cat > Justfile <<'EOF'
s := "foo" + "bar"

show:
    @echo "s={{s}}"
EOF
OUT="$("$TUR" run show 2>&1)"
RC=$?
if [ "$RC" -eq 0 ] && grep -q 's=foobar' <<< "$OUT"; then
  pass "plus-concat: 'foo' + 'bar' -> foobar"
else
  fail "plus-concat: got rc=$RC out=$OUT"
fi

# ---------------------------------------------------------------------------
# 4. Conditional expression: if os() == "<host>" { "yes" } else { "no" }.
# ---------------------------------------------------------------------------
cat > Justfile <<EOF
p := if os() == "${HOST_OS}" { "yes" } else { "no" }

show:
    @echo "p={{p}}"
EOF
OUT="$("$TUR" run show 2>&1)"
RC=$?
if [ "$RC" -eq 0 ] && grep -q 'p=yes' <<< "$OUT"; then
  pass "conditional: matched host OS branch"
else
  fail "conditional: got rc=$RC out=$OUT"
fi

# ---------------------------------------------------------------------------
# 5. Nested case (the scite motivating example distilled): a conditional as
#    the default arg to env_var_or_default, with the outer var interpolated
#    through another assignment that uses '/'.
# ---------------------------------------------------------------------------
cat > Justfile <<EOF
preset := env_var_or_default("TUR_RUN_TEST_PRESET_UNSET", if os() == "${HOST_OS}" { "host-debug" } else { "other-debug" })
build_dir := "build" / preset

show:
    @echo "preset={{preset}} build_dir={{build_dir}}"
EOF
unset TUR_RUN_TEST_PRESET_UNSET
OUT="$("$TUR" run show 2>&1)"
RC=$?
if [ "$RC" -eq 0 ] && grep -q 'preset=host-debug build_dir=build/host-debug' <<< "$OUT"; then
  pass "nested: env_var_or_default + if + / concat compose"
else
  fail "nested: got rc=$RC out=$OUT"
fi

# ---------------------------------------------------------------------------
# 6. Env var override wins over the default.
# ---------------------------------------------------------------------------
cat > Justfile <<'EOF'
p := env_var_or_default("TUR_RUN_TEST_OVERRIDE", "default-p")

show:
    @echo "p={{p}}"
EOF
TUR_RUN_TEST_OVERRIDE="from-env" OUT="$(TUR_RUN_TEST_OVERRIDE=from-env "$TUR" run show 2>&1)"
RC=$?
if [ "$RC" -eq 0 ] && grep -q 'p=from-env' <<< "$OUT"; then
  pass "env override wins over default"
else
  fail "env override: got rc=$RC out=$OUT"
fi

# ---------------------------------------------------------------------------
# 7. Forward-reference to a variable defined LATER in the file resolves.
#
#    This used to be an "unknown variable 'b'" error, because assignments were
#    evaluated during the parse and in strict file order.  WP2 of the security
#    audit deferred that evaluation to first use (so `tur run --list` stops
#    running backticks -- see security-audit-plan.md D-2), and order-independence
#    falls out of it.  `just` itself resolves forward references, so this is the
#    behaviour converging, not drifting.
# ---------------------------------------------------------------------------
cat > Justfile <<'EOF'
a := b
b := "x"

show:
    @echo "{{a}}"
EOF
OUT="$("$TUR" run show 2>&1)"
RC=$?
if [ "$RC" -eq 0 ] && grep -q '^x$' <<< "$OUT"; then
  pass "forward-ref resolves under lazy assignment"
else
  fail "forward-ref: got rc=$RC out=$OUT"
fi

# ---------------------------------------------------------------------------
# 7b. A cycle between two assignments is the error lazy evaluation makes
#     reachable, and it must be reported rather than recursed into.
# ---------------------------------------------------------------------------
cat > Justfile <<'EOF'
a := b
b := a

show:
    @echo "{{a}}"
EOF
OUT="$("$TUR" run show 2>&1)"
RC=$?
if [ "$RC" -ne 0 ] && grep -q "defined in terms of itself" <<< "$OUT"; then
  pass "assignment cycle is reported"
else
  fail "assignment cycle: got rc=$RC out=$OUT"
fi

# ---------------------------------------------------------------------------
# 7c. D-2: `tur run --list` must not evaluate a backtick.  Listing is what an
#     editor or a shell completion runs against a tree you have only opened.
# ---------------------------------------------------------------------------
cat > Justfile <<'EOF'
side := `touch SIDE-EFFECT && echo ran`

show:
    @echo "{{side}}"
EOF
rm -f SIDE-EFFECT
"$TUR" run --list >/dev/null 2>&1
if [ ! -e SIDE-EFFECT ]; then
  pass "--list does not evaluate a backtick assignment"
else
  fail "--list ran the backtick (SIDE-EFFECT exists)"
fi
# ... and a recipe that reads it still gets the value.
OUT="$("$TUR" run show 2>&1)"
RC=$?
if [ "$RC" -eq 0 ] && grep -q '^ran$' <<< "$OUT" && [ -e SIDE-EFFECT ]; then
  pass "a recipe that reads the variable still forces it"
else
  fail "forced backtick: got rc=$RC out=$OUT"
fi
rm -f SIDE-EFFECT

# ---------------------------------------------------------------------------
# 8. Bare identifier RHS: a plain word RHS resolves to the previously-
#    defined variable (regression guard for the balanced scanner not
#    eating identifiers).
# ---------------------------------------------------------------------------
cat > Justfile <<'EOF'
a := "hello"
b := a

show:
    @echo "b={{b}}"
EOF
OUT="$("$TUR" run show 2>&1)"
RC=$?
if [ "$RC" -eq 0 ] && grep -q 'b=hello' <<< "$OUT"; then
  pass "bare-ident RHS resolves to prior var"
else
  fail "bare-ident RHS: got rc=$RC out=$OUT"
fi

# ---------------------------------------------------------------------------
# replace() of an empty string is empty.  The builtin's fill loop never ran
# for "", so the 1-byte result was never NUL-terminated (a heap over-read;
# docs/archive/justrun-replace-empty-input-returns-unterminated-buffer.md).
# ---------------------------------------------------------------------------
cat > Justfile <<'EOF'
x := replace("", "a", "b")
y := replace("banana", "a", "o")

show:
    @echo "x=[{{x}}] y=[{{y}}]"
EOF
OUT="$("$TUR" run show 2>&1)"
RC=$?
if [ "$RC" -eq 0 ] && grep -qF 'x=[] y=[bonono]' <<< "$OUT"; then
  pass "replace of empty string is empty"
else
  fail "replace empty: got rc=$RC out=$OUT"
fi

exit $FAIL
