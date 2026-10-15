#!/usr/bin/env bash
# check-justfile.sh -- the Justfile must run under `tur run` alone, with no
# `just` binary installed (docs/upcoming/markdown-library-plan.md, Phase J).
#
#   bash tests/check-justfile.sh [path/to/tur]      (default: ./build/tur)
#
# Two ways the file can quietly start needing `just` again, both caught here:
#
#   1. A Justfile feature `tur run` does not implement -- today that means a
#      recipe attribute it does not know, such as `[parallel]`
#      (docs/guides/tur-run-guide.md).  `tur run --list` parses every recipe
#      and reports each one as a `tur run: note:` line, so any such line fails
#      the check.  Nothing else would: the recipe only breaks when someone
#      runs it.
#
#   2. A recipe body that calls `just` itself, e.g. `just configure`.  Under
#      `tur run` on a machine without `just` that line fails at run time.  A
#      recipe that needs another one done first depends on it instead
#      (`debug: _configured`).  Comments do not count, so the Windows
#      bootstrap notes -- where `just` really is the runner, because there is
#      no tur yet on a fresh Windows box -- are left alone.
#
# Exit 0 when both hold, 1 otherwise.
set -u
cd "$(dirname "$0")/.."

TUR="${1:-./build/tur}"
if [ ! -x "$TUR" ]; then
    echo "check-justfile: $TUR is not built" >&2
    exit 1
fi

fail=0

notes=$("$TUR" run --list 2>&1 >/dev/null | grep -F 'tur run: note:')
if [ -n "$notes" ]; then
    echo "FAIL check-justfile: the Justfile uses a feature tur run does not support:"
    printf '%s\n' "$notes" | sed 's/^/      /'
    fail=1
fi

# Recipe bodies are the indented lines; drop comment lines, then look for
# `just` in a command position: start of line, or after ; & | ( or `$(`.
calls=$(grep -nE '^( {4}|	)' Justfile \
        | grep -vE '^[0-9]+:[[:space:]]*#' \
        | grep -E '^[0-9]+:([[:space:]]*@?-?|.*[;&|(][[:space:]]*)just[[:space:]]+[A-Za-z_]')
if [ -n "$calls" ]; then
    echo "FAIL check-justfile: a recipe calls \`just\`; depend on the recipe instead:"
    printf '%s\n' "$calls" | sed 's/^/      Justfile:/'
    fail=1
fi

if [ "$fail" -eq 0 ]; then
    n=$("$TUR" run --list 2>/dev/null | grep -cE '^  [A-Za-z_]')
    echo "PASS check-justfile ($n recipes, all parsed by tur run; none calls just)"
fi
exit "$fail"
