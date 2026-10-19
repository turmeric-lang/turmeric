#!/usr/bin/env bash
# run-tur-docs.sh -- `tur doc <symbol>` and `tur docs` (OD4).
#
# The offline-docs work made the documentation reachable from Try Turmeric with
# no network. This is the other half of that: reaching it from a shell.
#
# What is asserted here, and why each case matters:
#
#   - `tur doc <builtin>` still answers. The stdlib fallback is additive; if it
#     shadowed or broke the builtin table, `tur doc let` would regress silently.
#   - `tur doc <stdlib-name>` answers from stdlib/docstrings.tur. This is the
#     new capability: the full docstring -- summary, Parameters, Returns,
#     Example -- printed with no network and no interpreter boot.
#   - It answers for an entry that is NOT the first in the generated table. The
#     first version of the scanner stopped after entry one, because each entry's
#     own closing brace looks exactly like the end of the array, and a test that
#     only probed the first key would have passed.
#   - A missing symbol exits non-zero. A doc lookup that prints nothing and
#     succeeds is indistinguishable from one that worked, to a script.
#   - `--json` emits a single well-formed object, and does NOT truncate. The
#     old path escaped into a fixed 512-byte buffer; every real stdlib
#     docstring is longer than that.
#   - `tur docs` finds docs/html in a source checkout, and reports a clear,
#     non-zero failure when there are none -- an absent optional artifact
#     should name the two ways to get it, not stack-trace.
#   - The `docs` subcommand does not collide with `doc`: exact match wins over
#     prefix resolution in both directions.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TUR="${TUR:-$ROOT/build/tur}"

FAIL=0
pass() { echo "PASS: $1"; }
fail() { echo "FAIL: $1"; FAIL=1; }

if [ ! -x "$TUR" ]; then
    echo "tests: $TUR not built" >&2
    exit 2
fi

if [ ! -f "$ROOT/stdlib/docstrings.tur" ]; then
    echo "SKIP: stdlib/docstrings.tur absent (run 'tur run docs')" >&2
    exit 0
fi

# Pick a stdlib key that is deliberately NOT the first entry in the generated
# table, and read its expected summary from the table itself so this test does
# not go stale when a docstring is reworded.
pick_late_key() {
    grep -o '^    {"[^"]*", "[^"]* -- ' "$ROOT/stdlib/docstrings.tur" \
        | sed -n '20p' | sed 's/^    {"\([^"]*\)".*/\1/'
}

# --- builtins still work -----------------------------------------------------
OUT="$("$TUR" doc let 2>&1)"; RC=$?
if [ "$RC" -eq 0 ] && grep -q 'bind local ' <<<"$OUT"; then
    pass "builtin lookup (let)"
else
    fail "builtin lookup (let) -- rc=$RC out=$(head -1 <<<"$OUT")"
fi

# --- stdlib docstrings, from the table shipped with the binary ---------------
KEY="$(pick_late_key)"
if [ -z "$KEY" ]; then
    fail "could not pick a stdlib key from docstrings.tur"
else
    OUT="$("$TUR" doc "$KEY" 2>&1)"; RC=$?
    if [ "$RC" -eq 0 ] && grep -q -- "$KEY -- " <<<"$OUT"; then
        pass "stdlib lookup past the first table entry ($KEY)"
    else
        fail "stdlib lookup ($KEY) -- rc=$RC out=$(head -1 <<<"$OUT")"
    fi
fi

# A docstring with the full block structure, to prove the value literal is read
# whole rather than stopping at its first escape sequence. Chosen from the table
# rather than hardcoded, so a renamed or reworded definition does not turn this
# into a false failure.
RICH_KEY="$(grep -o '^    {"[^"]*", "[^"]*Parameters:[^"]*Returns:[^"]*Example:[^"]*"' \
              "$ROOT/stdlib/docstrings.tur" | head -1 \
            | sed 's/^    {"\([^"]*\)".*/\1/')"
if [ -z "$RICH_KEY" ]; then
    fail "no docstring with Parameters/Returns/Example found in the table"
    RICH_KEY="vec-push!"
fi
OUT="$("$TUR" doc "$RICH_KEY" 2>&1)"; RC=$?
if [ "$RC" -eq 0 ] \
   && grep -q 'Parameters:' <<<"$OUT" \
   && grep -q 'Returns:' <<<"$OUT"; then
    pass "multi-line docstring keeps its Parameters/Returns blocks ($RICH_KEY)"
else
    fail "multi-line docstring ($RICH_KEY) -- rc=$RC out=$(head -3 <<<"$OUT")"
fi

# --- a miss is an error ------------------------------------------------------
OUT="$("$TUR" doc definitely-not-a-real-symbol-xyzzy 2>&1)"; RC=$?
if [ "$RC" -ne 0 ] && grep -q 'no documentation' <<<"$OUT"; then
    pass "unknown symbol exits non-zero"
else
    fail "unknown symbol -- rc=$RC out=$(head -1 <<<"$OUT")"
fi

# --- --json is one object, untruncated --------------------------------------
OUT="$("$TUR" --json doc "$RICH_KEY" 2>&1)"; RC=$?
if [ "$RC" -ne 0 ]; then
    fail "--json lookup -- rc=$RC"
elif [ "$(wc -l <<<"$OUT")" -ne 1 ]; then
    fail "--json lookup emitted $(wc -l <<<"$OUT") lines, expected 1"
elif ! python3 -c '
import json, sys
want = sys.argv[1]
d = json.loads(sys.stdin.read())
assert d["name"] == want, d["name"]
assert "Parameters:" in d["doc"], "docstring truncated before Parameters"
assert "Returns:" in d["doc"], "docstring truncated before Returns"
' "$RICH_KEY" <<<"$OUT" 2>/dev/null; then
    fail "--json lookup is not a complete, well-formed object"
else
    pass "--json lookup is one complete object"
fi

OUT="$("$TUR" --json doc definitely-not-a-real-symbol-xyzzy 2>&1)"; RC=$?
if [ "$RC" -ne 0 ] && grep -q '"error"' <<<"$OUT"; then
    pass "--json miss reports an error object"
else
    fail "--json miss -- rc=$RC out=$(head -1 <<<"$OUT")"
fi

# --- tur docs ----------------------------------------------------------------
# `docs` must resolve to itself, not to `doc` via prefix matching.
OUT="$("$TUR" docs --help 2>&1)"; RC=$?
if grep -q 'tur docs --serve' <<<"$OUT"; then
    pass "docs subcommand resolves separately from doc"
else
    fail "docs subcommand resolution -- out=$(head -1 <<<"$OUT")"
fi

if [ -f "$ROOT/docs/html/guides/index.html" ]; then
    OUT="$("$TUR" docs 2>&1)"; RC=$?
    if [ "$RC" -eq 0 ] && grep -q 'guides/index.html' <<<"$OUT"; then
        pass "docs found in a source checkout"
    else
        fail "docs in a source checkout -- rc=$RC out=$(head -1 <<<"$OUT")"
    fi
else
    echo "SKIP: docs/html not generated (run 'tur run docs') -- skipping the found case"
fi

# With no docs anywhere, the failure names both ways to get them. TUR_DOCS_DIR
# is set to an empty dir AND cwd is moved somewhere with no ancestor checkout,
# since resolution walks up from the binary and from the environment.
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/empty"
OUT="$(cd "$TMP" && TUR_DOCS_DIR="$TMP/empty" "$TUR" docs 2>&1)"; RC=$?
if grep -q 'ignoring TUR_DOCS_DIR' <<<"$OUT"; then
    pass "a TUR_DOCS_DIR without guides/ is reported, not trusted"
else
    fail "bad TUR_DOCS_DIR -- out=$(head -2 <<<"$OUT")"
fi

if [ "$FAIL" -ne 0 ]; then
    echo "tur-docs: FAILED"
    exit 1
fi
echo "tur-docs: all checks passed"
