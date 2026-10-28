#!/usr/bin/env bash
# tools/wasm-jit/check-examples.sh -- wasm-jit-plan P3's gate: every Try
# Turmeric example (web/main.js EXAMPLES) runs on the browser JIT module and
# either compiles or falls back loudly, and none gives a wrong answer.
#
# "Right" is what `tur interpret` prints for the same text run as a program:
# the page's Interpret engine evaluates a REPL session instead, which also
# shows each top-level value, but a compiled program shows only its output,
# so program-mode output is the fair comparison.
#
#   bash tools/wasm-jit/check-examples.sh
#
# Environment: TUR (a native tur, default build/tur), TUR_WASM_JIT_MODULE
# (default build-wasm/wasm-jit).  Exits 1 when an example's output differs.
set -u
cd "$(dirname "$0")/../.."
TUR="${TUR:-build/tur}"
MODULE_DIR="${TUR_WASM_JIT_MODULE:-build-wasm/wasm-jit}"
[ -x "$TUR" ] || { echo "check-examples: no tur at $TUR" >&2; exit 2; }
[ -f "$MODULE_DIR/turmeric-jit.wasm" ] || { echo "check-examples: no $MODULE_DIR/turmeric-jit.wasm" >&2; exit 2; }
export TUR_NO_GLOBAL_SPICES=1
# The interpreter never frees its closures (process-lifetime, by design), so
# on a Debug tur LeakSanitizer reports them and exits before stdout is
# flushed: the reference came back empty with exit 1, and a broken run on the
# module "matched" it.  The interpreter harnesses opt out the same way
# (CLAUDE.md, leak detection policy).
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tur-examples.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
node -e '
const fs = require("fs");
const s = fs.readFileSync("web/main.js", "utf8");
const a = s.indexOf("const EXAMPLES = {");
const b = s.indexOf("\n};", a);
const EX = eval("(" + s.slice(a + "const EXAMPLES = ".length, b + 2) + ")");
for (const [k, v] of Object.entries(EX)) fs.writeFileSync(process.argv[1] + "/" + k + ".tur", v + "\n");
' "$WORK"

wrong=0
for f in "$WORK"/*.tur; do
    name=$(basename "$f" .tur)
    "$TUR" interpret "$f" > "$WORK/$name.want" 2> /dev/null
    want_rc=$?
    node tools/wasm-jit/run.js --module "$MODULE_DIR" --result "$WORK/$name.json" "$f" \
        > "$WORK/$name.got" 2> "$WORK/$name.err"
    got_rc=$?
    engine=$(grep -o '"engine":"[a-z]*"' "$WORK/$name.json" | cut -d'"' -f4)
    reason=$(grep -o '"reason":"[^"]*"' "$WORK/$name.json" | cut -d'"' -f4)
    if cmp -s "$WORK/$name.want" "$WORK/$name.got" && [ "$want_rc" = "$got_rc" ]; then
        printf '%-15s %-12s %s\n' "$name" "$engine" "${reason:+($reason)}"
    else
        printf '%-15s %-12s WRONG (exit %s, interpreter %s)%s\n' "$name" "$engine" "$got_rc" "$want_rc" "${reason:+ ($reason)}"
        diff "$WORK/$name.want" "$WORK/$name.got" | head -6 | sed 's/^/    /'
        wrong=$((wrong + 1))
    fi
done
[ "$wrong" -eq 0 ] && echo "check-examples: all right" || echo "check-examples: $wrong wrong"
[ "$wrong" -eq 0 ]
