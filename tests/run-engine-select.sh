#!/usr/bin/env bash
# tests/run-engine-select.sh -- engine-selection-plan E1-E3 behavior tests.
#
# Exercises `tur run`'s engine resolution ladder against scratch projects:
#   --engine > TUR_ENGINE env > build.tur :engine > "cc"
# plus the unsatisfiable-engine hard errors and the bare-run fallback.
#
# The probe program prints "compiled" under the cc/jit engines (the JIT is a
# compiled target: it takes the `:tur` reader-conditional branch) and
# "interpreted" under the tree-walker, so each case asserts WHICH engine
# actually ran, not merely that something ran.
#
# Works on every build: the jit rows adapt to whether $TUR carries the
# engine (both outcomes -- run-or-fallback vs hard error -- are asserted).
set -u
cd "$(dirname "$0")/.."

TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || { echo "engine-select: $TUR not built" >&2; exit 2; }
TUR_ABS=$(cd "$(dirname "$TUR")" && pwd)/$(basename "$TUR")

# The compiled arms shell out to cc against the sanitized libturi; anchor -L
# exactly as run.sh does, and silence the interpreter's intentional leaks.
_tur_build_dir=$(dirname "$TUR_ABS")
export TUR_CC_FLAGS="${TUR_CC_FLAGS:--O2 -std=c99 -Wall -fno-strict-aliasing -L${_tur_build_dir}/src}"
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"
unset TUR_ENGINE

# Does this binary carry the JIT engine?  (Probe with a nonexistent input:
# a bare `tur jit` prints usage on every build since P0 moved the input scan
# ahead of the gates; only the "carries no JIT" answer discriminates.)
probe=$("$TUR_ABS" jit /nonexistent-tur-jit-probe.tur 2>&1 || true)
have_jit=1
case "$probe" in *"carries no JIT"*) have_jit=0 ;; esac

tmp=$(mktemp -d "${TMPDIR:-/tmp}/tur-engine-select.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

mk_project() { # $1=dir $2=engine-line (may be empty) $3=extra-line (may be empty)
    mkdir -p "$1/src"
    {
        echo "(defpackage p"
        echo "  :name    \"p\""
        echo "  :version \"0.1.0\""
        [ -n "$2" ] && echo "  $2"
        [ -n "$3" ] && echo "  $3"
        echo ")"
    } > "$1/build.tur"
    cat > "$1/src/main.tur" <<'TUR_EOF'
(defn main [] : int
  #?(:tur  (println "compiled")
     :turi (println "interpreted"))
  0)
TUR_EOF
}

PASS=0; FAIL=0
chk() { # $1=name $2=expected-substring $3=actual
    case "$3" in
        *"$2"*) PASS=$((PASS+1)); echo "PASS $1" ;;
        *) FAIL=$((FAIL+1)); echo "FAIL $1 -- wanted '$2' in:"; printf '%s\n' "$3" | sed 's/^/    /' ;;
    esac
}

# 1. Default: no key anywhere -> cc.
mk_project "$tmp/def" "" ""
out=$(cd "$tmp/def" && "$TUR_ABS" run src/main.tur 2>&1)
chk "default-is-cc" "compiled" "$out"

# 2. Manifest :engine interp, bare run (Justfile-less project fallback).
mk_project "$tmp/mi" ':engine  "interp"' ""
out=$(cd "$tmp/mi" && "$TUR_ABS" run 2>&1)
chk "manifest-interp-bare-run" "interpreted" "$out"

# 3. Manifest :engine interp, explicit-file mode.
out=$(cd "$tmp/mi" && "$TUR_ABS" run src/main.tur 2>&1)
chk "manifest-interp-explicit-file" "interpreted" "$out"

# 4. CLI --engine cc beats the manifest.
out=$(cd "$tmp/mi" && "$TUR_ABS" run --engine cc 2>&1)
chk "cli-beats-manifest" "compiled" "$out"

# 5. TUR_ENGINE env beats the manifest.
out=$(cd "$tmp/mi" && TUR_ENGINE=cc "$TUR_ABS" run 2>&1)
chk "env-beats-manifest" "compiled" "$out"

# 6. CLI beats env.
out=$(cd "$tmp/mi" && TUR_ENGINE=cc "$TUR_ABS" run --engine interp 2>&1)
chk "cli-beats-env" "interpreted" "$out"

# 7. Unknown CLI value: hard error, its own code, nothing runs.
out=$(cd "$tmp/mi" && "$TUR_ABS" run --engine jitt 2>&1; echo "rc=$?")
chk "unknown-cli-value-errors" "TUR-E0311" "$out"
chk "unknown-cli-value-nonzero" "rc=2" "$out"

# 8. Unknown manifest value: hard error at manifest read.
mk_project "$tmp/bad" ':engine  "turbo"' ""
out=$(cd "$tmp/bad" && "$TUR_ABS" run 2>&1; echo "rc=$?")
chk "unknown-manifest-value-errors" "TUR-E0311" "$out"

# 9. :engine jit -- outcome depends on the build, both are asserted.
#    The manifest carries ONLY `:engine "jit"`.  It used to also carry
#    `:experiments [jit]`, to pin that a project which opted in before the
#    2026-08-17 graduation kept working; that shim was retired at 0.38.0, one
#    minor line later, so the key is now a hard error and case 10 below pins
#    that instead.  Engine selection never depended on it.
mk_project "$tmp/mj" ':engine  "jit"' ""
mk_project "$tmp/mx" ':engine  "jit"' ':experiments [jit]'
if [ "$have_jit" = "1" ]; then
    out=$(cd "$tmp/mj" && "$TUR_ABS" run 2>&1)
    chk "manifest-jit-runs" "compiled" "$out"
else
    out=$(cd "$tmp/mj" && "$TUR_ABS" run 2>&1; echo "rc=$?")
    chk "manifest-jit-no-engine-hard-error" "-DTUR_JIT=ON" "$out"
    chk "manifest-jit-no-engine-nonzero" "rc=2" "$out"
fi

# 10. `jit` GRADUATED 2026-08-17 and its compatibility shim was RETIRED at
#     0.38.0.  Three states, and this case has now pinned each of them in turn:
#     before graduation the gate stayed closed without the key; after it, the
#     key was a TUR-W0063 no-op so a project that opted in early kept building;
#     after retirement the key is the hard TUR-E0310 an unknown experiment name
#     gets, because the migration window is a window and not an alias.
#     What still gates `tur jit` is the BUILD, which is why the no-engine arm
#     asserts the -DTUR_JIT=ON message instead.
mk_project "$tmp/ng" "" ""
out=$(cd "$tmp/mx" && "$TUR_ABS" jit src/main.tur 2>&1; echo "rc=$?")
chk "retired-manifest-experiments-key-errors" "TUR-E0310" "$out"
if [ "$have_jit" = "1" ]; then
    out=$(cd "$tmp/ng" && "$TUR_ABS" jit src/main.tur 2>&1)
    chk "jit-runs-without-experiments-key" "compiled" "$out"

    out=$(cd "$tmp/mj" && "$TUR_ABS" jit src/main.tur 2>&1)
    chk "jit-runs-with-engine-key-only" "compiled" "$out"
else
    out=$(cd "$tmp/ng" && "$TUR_ABS" jit src/main.tur 2>&1; echo "rc=$?")
    chk "jit-no-engine-hard-error" "-DTUR_JIT=ON" "$out"
    chk "jit-no-engine-nonzero" "rc=2" "$out"
fi

# 11. Program args pass through to the interp arm as *args*.
cat > "$tmp/mi/src/args.tur" <<'TUR_EOF'
(defn main [] : int
  #?(:tur  (println "ENGINE-BUG-compiled")
     :turi (println (list-length *args*)))
  0)
TUR_EOF
out=$(cd "$tmp/mi" && "$TUR_ABS" run src/args.tur -- a b c 2>&1)
chk "interp-arg-passthrough" "3" "$out"

# 12. tur-jit-skips-spice-include-discovery: `tur jit <file>` walks up to the
#     enclosing build.tur and puts the spice's src/ on the search path, as
#     `tur run --engine jit` and every other per-file command do.  A file
#     outside src/ importing a module under it is the shape that broke.
if [ "$have_jit" = "1" ]; then
    mk_project "$tmp/sj" "" ""
    mkdir -p "$tmp/sj/src/demo" "$tmp/sj/other"
    cat > "$tmp/sj/src/demo/util.tur" <<'TUR_EOF'
(defmodule demo/util
  (export answer)
  (defn answer [] : int 42))
TUR_EOF
    cat > "$tmp/sj/other/main.tur" <<'TUR_EOF'
(defmodule main
  (import demo/util :refer [answer])
  (defn main [] : int (println (answer)) 0))
TUR_EOF
    out=$(cd "$tmp/sj" && "$TUR_ABS" jit other/main.tur 2>&1)
    chk "jit-discovers-spice-includes" "42" "$out"
fi

echo "engine-select summary: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
