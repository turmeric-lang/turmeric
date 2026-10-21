#!/usr/bin/env bash
# run-timings.sh -- `--timings`, the compiler's zone timer
# (docs/upcoming/profiling-plan.md C0+C1; the compiler-side bullet of its
# section 8).
#
# Asserts SHAPE, never numbers.  Percentages depend on the machine, the load
# and the build type, so nothing here compares a time against a bound.  What
# holds everywhere:
#
#   - the JSON is tur-profile/1, kind "compiler", with every envelope key and
#     every phase name, in pipeline order;
#   - the phases partition the wall clock: sum(self_ms) is within 2% of
#     wall_ms (by construction it is exact; 2% is the plan's tolerance);
#   - each hook a plain build must cross actually fired (self_ms > 0), and the
#     ones a command cannot reach did not (emit-c runs no cc);
#   - the flag changes nothing it compiles: emit-c output is byte-identical
#     with and without it, and without it nothing is printed;
#   - `tur jit --timing-json` keeps its own three keys (the benchmark harness
#     parses them).
set -u
cd "$(dirname "$0")/.."

TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || { echo "tests: $TUR not built; run 'cmake --build build' first" >&2; exit 2; }
if ! command -v python3 >/dev/null 2>&1; then
    echo "SKIP run-timings - python3 not available"
    exit 0
fi

PASS=0
FAIL=0
pass() { PASS=$((PASS+1)); echo "PASS $1"; }
fail() { FAIL=$((FAIL+1)); echo "FAIL $1"; [ $# -gt 1 ] && printf '    %s\n' "$2"; }

TMP="$(mktemp -d "${TMPDIR:-/tmp}/tur-timings.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

# A program that crosses every front-end zone a plain build has: an import
# (module-load), of a sweet-exp module (sweet), over the auto-loaded stdlib
# (macros, typeclasses).
cat > "$TMP/helper.tur" <<'EOF'
#lang turmeric/sweet
(defmodule helper
  (export twice)
  (defn twice [x : int] : int {x * 2}))
EOF
cat > "$TMP/main.tur" <<'EOF'
(defmodule app
  (import helper :refer [twice])
  (defn main [] : int
    (println (twice 21))
    0))
EOF

# check_json <name> <file> <must-be-positive,...> <must-be-zero,...>
check_json() {
    local name="$1" file="$2" pos="$3" zero="$4" out
    if out=$(python3 - "$file" "$pos" "$zero" <<'PY' 2>&1
import json, sys
path, pos, zero = sys.argv[1], sys.argv[2], sys.argv[3]
with open(path) as f:
    d = json.load(f)
names = ["read", "sweet", "module-load", "macro-expand", "elaborate",
         "typeclass", "kind-check", "effect-env", "effect-row", "cps",
         "borrow-check", "refine", "mono", "emit", "cc", "jit", "run", "other"]
problems = []
for k in ("schema", "kind", "tur_version", "command", "started_utc",
          "wall_ms", "notes", "phases"):
    if k not in d:
        problems.append("missing key %r" % k)
if d.get("schema") != "tur-profile/1":
    problems.append("schema is %r" % d.get("schema"))
if d.get("kind") != "compiler":
    problems.append("kind is %r" % d.get("kind"))
if not isinstance(d.get("notes"), list):
    problems.append("notes is not a list")
phases = d.get("phases", [])
got = [p.get("name") for p in phases]
if got != names:
    problems.append("phase names %r, want %r" % (got, names))
for p in phases:
    for k in ("name", "self_ms", "self_pct"):
        if k not in p:
            problems.append("phase %r lacks %r" % (p.get("name"), k))
by = {p.get("name"): p for p in phases}
wall = float(d.get("wall_ms", 0))
total = sum(float(p.get("self_ms", 0)) for p in phases)
if wall <= 0:
    problems.append("wall_ms is %r" % wall)
elif abs(total - wall) > 0.02 * wall:
    problems.append("sum(self_ms)=%.4f is not within 2%% of wall_ms=%.4f" % (total, wall))
pct = sum(float(p.get("self_pct", 0)) for p in phases)
if wall > 0 and abs(pct - 100.0) > 0.5:
    problems.append("sum(self_pct)=%.4f is not 100 +/- 0.5" % pct)
for n in filter(None, pos.split(",")):
    if float(by.get(n, {}).get("self_ms", 0)) <= 0:
        problems.append("phase %r never ran (self_ms 0)" % n)
for n in filter(None, zero.split(",")):
    if float(by.get(n, {}).get("self_ms", 0)) != 0:
        problems.append("phase %r ran (self_ms %r) where it cannot" % (n, by[n]["self_ms"]))
for note in d.get("notes", []):
    if "unbalanced" in note or "overflowed" in note:
        problems.append("note: %s" % note)
if problems:
    print("; ".join(problems))
    sys.exit(1)
PY
    ); then
        pass "$name"
    else
        fail "$name" "$out"
    fi
}

FRONT="read,sweet,module-load,macro-expand,elaborate,typeclass,kind-check,effect-env,effect-row,cps,borrow-check,emit"

# --- tur build --timings <path>.json: the full pipeline, cc included -------
if "$TUR" build --timings "$TMP/build.json" "$TMP/main.tur" -o "$TMP/main" \
        > "$TMP/build.out" 2> "$TMP/build.err"; then
    pass "build/exit-0"
else
    fail "build/exit-0" "$(head -5 "$TMP/build.err")"
fi
check_json "build/json-shape" "$TMP/build.json" "$FRONT,cc" "jit,run"
if grep -q '^tur --timings: ' "$TMP/build.err" && grep -q 'total (wall)' "$TMP/build.err"; then
    pass "build/table-on-stderr"
else
    fail "build/table-on-stderr" "$(head -5 "$TMP/build.err")"
fi
if [ -x "$TMP/main" ] && [ "$("$TMP/main")" = "42" ]; then
    pass "build/program-still-builds"
else
    fail "build/program-still-builds"
fi

# --- emit-c: byte-identical output, no cc ---------------------------------
"$TUR" emit-c "$TMP/main.tur" > "$TMP/plain.c" 2> "$TMP/plain.err"
"$TUR" emit-c --timings="$TMP/emit.json" "$TMP/main.tur" > "$TMP/timed.c" 2> "$TMP/timed.err"
if cmp -s "$TMP/plain.c" "$TMP/timed.c" && [ -s "$TMP/plain.c" ]; then
    pass "emit-c/output-unchanged"
else
    fail "emit-c/output-unchanged" "emit-c stdout differs with --timings"
fi
check_json "emit-c/json-shape" "$TMP/emit.json" "$FRONT" "cc,jit,run"
if [ ! -s "$TMP/plain.err" ]; then
    pass "emit-c/silent-without-flag"
else
    fail "emit-c/silent-without-flag" "$(head -3 "$TMP/plain.err")"
fi

# --- check, JSON on stdout ------------------------------------------------
# `--timings=-` everywhere; `/dev/stdout` off Windows, where a native tur.exe
# under MSYS would see the argument rewritten as a path.
"$TUR" check --timings=- "$TMP/main.tur" > "$TMP/check.json" 2> "$TMP/check.err"
check_json "check/json-on-stdout" "$TMP/check.json" "read,elaborate,kind-check" "cc,jit,run"
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*) echo "SKIP check/dev-stdout - native Windows tur" ;;
    *)
        "$TUR" check --timings /dev/stdout "$TMP/main.tur" > "$TMP/check2.json" 2> "$TMP/check2.err"
        check_json "check/dev-stdout" "$TMP/check2.json" "read,elaborate" "cc,jit,run"
        ;;
esac

# --- no path: the table alone, nothing on stdout ---------------------------
"$TUR" emit-c --timings "$TMP/main.tur" > "$TMP/nopath.c" 2> "$TMP/nopath.err"
if cmp -s "$TMP/plain.c" "$TMP/nopath.c" && grep -q '^tur --timings: ' "$TMP/nopath.err"; then
    pass "no-path/table-only"
else
    fail "no-path/table-only" "$(head -3 "$TMP/nopath.err")"
fi

# --- after `--` the flag is the program's, not tur's -----------------------
"$TUR" run "$TMP/main.tur" -- --timings > "$TMP/pass.out" 2> "$TMP/pass.err"
if [ "$(cat "$TMP/pass.out")" = "42" ] && ! grep -q 'tur --timings' "$TMP/pass.err"; then
    pass "run/passthrough-not-eaten"
else
    fail "run/passthrough-not-eaten" "$(head -3 "$TMP/pass.err")"
fi

# --- tur run: the program's own run is its own phase -----------------------
"$TUR" run --timings="$TMP/run.json" "$TMP/main.tur" > "$TMP/run.out" 2> "$TMP/run.err"
check_json "run/json-shape" "$TMP/run.json" "$FRONT,cc,run" "jit"

# --- tur jit, when this build carries the engine ---------------------------
if "$TUR" jit "$TMP/main.tur" > "$TMP/jitprobe.out" 2> "$TMP/jitprobe.err" \
        && [ "$(cat "$TMP/jitprobe.out")" = "42" ]; then
    "$TUR" jit --timings="$TMP/jit.json" "$TMP/main.tur" > "$TMP/jit.out" 2> "$TMP/jit.err"
    check_json "jit/json-shape" "$TMP/jit.json" "$FRONT,jit,run" "cc"
    "$TUR" jit --timing-json "$TMP/legacy.json" "$TMP/main.tur" > /dev/null 2>&1
    if out=$(python3 - "$TMP/legacy.json" <<'PY' 2>&1
import json, sys
d = json.load(open(sys.argv[1]))
if sorted(d) != ["compile_ms", "engine", "run_ms"] or d["engine"] != "jit":
    print("keys %r engine %r" % (sorted(d), d.get("engine")))
    sys.exit(1)
PY
    ); then
        pass "jit/timing-json-unchanged"
    else
        fail "jit/timing-json-unchanged" "$out"
    fi
else
    echo "SKIP jit - this tur carries no JIT engine (or it declined the probe)"
fi

echo
echo "timings summary: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
