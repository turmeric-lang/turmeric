#!/usr/bin/env bash
# tests/run-init-saffron.sh -- saffron-lang-plan S8: `tur init --saffron`
# scaffolds a project that BUILDS AND RUNS.
#
# The assertion is deliberately the build, not the file contents. Checking that
# `#lang saffron` appears in src/main.tur would have passed while the LIBRARY
# scaffold could not compile at all: `load_project_prelude` (the driver) hardcoded
# READER_TURMERIC, so the autoloaded Saffron prelude died on its own first line
# with `unexpected character '#'`. That is the FOURTH entry point found with
# this defect -- after `tur fmt`, the module import path (S7), and the
# single-file autoload (S6) -- and only the project-library path reached it,
# which is why `--bin` worked and `--lib` did not.
#
# So both shapes are built here, and the binary is RUN.
#
# Skips cleanly (exit 0) when the binary is missing, like its siblings.

set -uo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"

TUR="${TUR:-$ROOT/build/tur}"
[ -x "$TUR" ] || [ ! -x "${TUR}.exe" ] || TUR="${TUR}.exe"
if [ ! -x "$TUR" ]; then
    echo "SKIP: $TUR not built"
    echo "TUR_SKIP: $TUR not built"
    exit 0
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

fails=0

# --- 1. --saffron --bin: scaffolds, builds, runs ---------------------------
mkdir -p "$WORK/bin" && cd "$WORK/bin"
"$TUR" init --no-git --saffron demo >/dev/null 2>&1
if [ "$(head -1 src/main.tur)" != '#lang saffron' ]; then
    echo "FAIL --saffron --bin: src/main.tur has no '#lang saffron' first line"
    head -3 src/main.tur | sed 's/^/       /'
    fails=$((fails + 1))
fi
# ...and NO annotations: a scaffold that annotated would teach the opposite of
# what the dialect is for.
if grep -qE '\[\] *:int|: *int\b' src/main.tur; then
    echo "FAIL --saffron --bin: the scaffold carries type annotations"
    sed 's/^/       /' src/main.tur
    fails=$((fails + 1))
fi
out="$("$TUR" build . 2>&1)"
if [ ! -x build/bin/demo ]; then
    echo "FAIL --saffron --bin: build produced no binary"
    sed 's/^/       /' <<<"$out" | head -6
    fails=$((fails + 1))
elif ! grep -q 'Hello from demo!' < <(./build/bin/demo 2>/dev/null); then
    echo "FAIL --saffron --bin: the built binary did not greet"
    ./build/bin/demo 2>&1 | sed 's/^/       /' | head -4
    fails=$((fails + 1))
else
    echo "ok   --saffron --bin: scaffolds, builds, runs"
fi

# --- 2. --saffron --lib: builds (this is the one that was broken) ----------
mkdir -p "$WORK/lib" && cd "$WORK/lib"
"$TUR" init --no-git --lib --saffron mylib >/dev/null 2>&1
out="$("$TUR" build . 2>&1)"
if grep -q "unexpected character '#'" <<<"$out"; then
    echo "FAIL --saffron --lib: the autoloaded Saffron prelude was read as"
    echo "     plain Turmeric -- load_project_prelude lost its #lang detection"
    sed 's/^/       /' <<<"$out" | head -5
    fails=$((fails + 1))
elif grep -qE '^[^ ]*: *error' <<<"$out"; then
    echo "FAIL --saffron --lib: build reported an error"
    sed 's/^/       /' <<<"$out" | head -6
    fails=$((fails + 1))
else
    echo "ok   --saffron --lib: builds"
fi

# --- 3. the DEFAULT scaffold is unchanged ----------------------------------
# Without this, a change that made every scaffold Saffron would pass 1 and 2.
mkdir -p "$WORK/plain" && cd "$WORK/plain"
"$TUR" init --no-git plain >/dev/null 2>&1
if [[ "$(head -1 src/main.tur)" == '#lang'* ]]; then
    echo "FAIL default: a plain scaffold must not carry a #lang line"
    head -2 src/main.tur | sed 's/^/       /'
    fails=$((fails + 1))
elif ! grep -q ':int' src/main.tur; then
    echo "FAIL default: a plain scaffold should still be annotated Turmeric"
    sed 's/^/       /' src/main.tur
    fails=$((fails + 1))
else
    echo "ok   default: still annotated Turmeric, no #lang line"
fi

cd "$ROOT"
if [ "$fails" -ne 0 ]; then
    echo "run-init-saffron: $fails check(s) failed"
    exit 1
fi
echo "run-init-saffron: all checks passed"
