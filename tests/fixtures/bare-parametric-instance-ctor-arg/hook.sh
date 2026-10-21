#!/usr/bin/env bash
# bare-parametric-instance-ctor-arg-unbridged: the defect is in the base
# `__inst_Pl_pr_Rose`, which a default `tur build` prunes (nothing calls it --
# the call goes to its spec), so build unpruned, as `emit-c` output and a
# `TUR_CC_NO_PRUNE=1` build compile it, and fail on cc's int/pointer warning
# (a hard error under clang 15+ and gcc 14).
set -e
FIXTURE_DIR="$(cd "$(dirname "$0")" && pwd)"
TUR_CC_NO_PRUNE=1 "$TUR" build "$FIXTURE_DIR/input.tur" -o "$1/prog" 2> "$1/cc.log" ||
    { cat "$1/cc.log" >&2; exit 1; }
if grep -E '\[-W(int-conversion|incompatible-pointer-types)\]' "$1/cc.log" >&2; then
    exit 1
fi
"$1/prog"
