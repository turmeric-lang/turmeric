#!/usr/bin/env bash
# tools/update-mir.sh -- refresh the vendored MIR sources under external/mir/.
#
# The JIT engine (`tur jit`, -DTUR_JIT=ON) compiles three MIR translation
# units -- mir.c, mir-gen.c and c2mir/c2mir.c -- plus the per-architecture
# files those #include.  That is ~2.7 MB of a ~55 MB checkout (most of the
# rest is c-benchmarks/), so the tree carries exactly those files and nothing
# else.  A configure therefore never reaches the network, which is what lets
# TUR_JIT default ON (cmake/mir.cmake).
#
# Fixes to MIR itself are made in the fork (external/mir/VENDORED.md lists
# them), merged there, and then copied in with this script.  Do not edit the
# files under external/mir/ by hand: the next sync silently reverts the edit.
#
#   bash tools/update-mir.sh                  # re-sync the recorded commit
#   bash tools/update-mir.sh <commit>         # move to another fork commit
#   MIR_REPOSITORY=<url> bash tools/update-mir.sh <commit>
#   bash tools/update-mir.sh --from <dir>     # copy from a local checkout, as is
#
# --from is for trying a MIR change before it is pushed anywhere; it records
# the checkout's HEAD, and warns when the checkout has uncommitted changes,
# because that commit then does not describe the files copied.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="$ROOT/external/mir"
UPSTREAM="$DEST/UPSTREAM"

# Defaults come from the recorded pin, so a bare run is a reproducible re-sync.
if [ -f "$UPSTREAM" ]; then
    # shellcheck disable=SC1090
    . "$UPSTREAM"
fi
REPO="${MIR_REPOSITORY:-https://github.com/turmeric-lang/mir.git}"
COMMIT="${MIR_COMMIT:-}"

FROM=""
if [ "${1:-}" = "--from" ]; then
    FROM="${2:?--from needs a directory}"
    shift 2
fi
if [ -n "${1:-}" ]; then COMMIT="$1"; fi

WORK=""
cleanup() { if [ -n "$WORK" ]; then rm -rf "$WORK"; fi; }
trap cleanup EXIT

if [ -n "$FROM" ]; then
    SRC="$(cd "$FROM" && pwd)"
    COMMIT="$(git -C "$SRC" rev-parse HEAD)"
    if [ -n "$(git -C "$SRC" status --porcelain)" ]; then
        echo "warning: $SRC has uncommitted changes; UPSTREAM will record" \
             "$COMMIT, which does not describe the copied files" >&2
    fi
    REPO="$(git -C "$SRC" config --get remote.origin.url || echo "$SRC")"
else
    if [ -z "$COMMIT" ]; then
        echo "error: no commit given and none recorded in $UPSTREAM" >&2
        exit 2
    fi
    WORK="$(mktemp -d)"
    SRC="$WORK/mir"
    git init -q "$SRC"
    git -C "$SRC" remote add origin "$REPO"
    # GitHub serves a reachable commit by SHA, so a depth-1 fetch is enough.
    # Fall back to a full fetch for a server that refuses that, or a ref name.
    if ! git -C "$SRC" fetch -q --depth 1 origin "$COMMIT" 2>/dev/null; then
        git -C "$SRC" fetch -q origin
    fi
    git -C "$SRC" checkout -q --detach "$COMMIT"
    COMMIT="$(git -C "$SRC" rev-parse HEAD)"
fi

# What tur_mir compiles, and everything those three TUs #include on any target
# MIR supports.  Every architecture is kept, not just the hosts CI runs on, so
# that a build on ppc64/s390x/riscv64 still configures.
ARCHES="x86_64 aarch64 ppc64 s390x riscv64"
FILES="LICENSE
mir.c mir.h mir-gen.c mir-gen.h mir-interp.c
mir-alloc.h mir-alloc-default.c
mir-code-alloc.h mir-code-alloc-default.c mir-code-alloc-wasm.c
mir-bitmap.h mir-dlist.h mir-hash.h mir-htab.h mir-reduce.h mir-varr.h
real-time.h mir-wasm.c
c2mir/c2mir.c c2mir/c2mir.h"
for a in $ARCHES; do
    FILES="$FILES mir-$a.c mir-$a.h mir-gen-$a.c"
done
for f in "$SRC"/c2mir/mirc*.h; do
    FILES="$FILES c2mir/$(basename "$f")"
done
DIRS="c2mir/x86_64 c2mir/aarch64 c2mir/ppc64 c2mir/s390x c2mir/riscv64 c2mir/wasm32"

for f in $FILES; do
    [ -f "$SRC/$f" ] || { echo "error: $f is missing from $REPO@$COMMIT" >&2; exit 1; }
done
for d in $DIRS; do
    [ -d "$SRC/$d" ] || { echo "error: $d/ is missing from $REPO@$COMMIT" >&2; exit 1; }
done

# Start from empty so a file MIR deleted upstream does not linger here.
# VENDORED.md is ours, not MIR's, and survives.
mkdir -p "$DEST"
find "$DEST" -mindepth 1 -maxdepth 1 ! -name VENDORED.md -exec rm -rf {} +

for f in $FILES; do
    mkdir -p "$DEST/$(dirname "$f")"
    cp "$SRC/$f" "$DEST/$f"
done
for d in $DIRS; do
    mkdir -p "$DEST/$d"
    cp "$SRC/$d"/* "$DEST/$d/"
done

cat > "$UPSTREAM" <<EOF
# external/mir/UPSTREAM -- where the files in this directory came from.
# Written by tools/update-mir.sh; do not edit by hand.  Sourced by that script
# and read by cmake/mir.cmake.
MIR_REPOSITORY=$REPO
MIR_COMMIT=$COMMIT
EOF

# A header the list above missed shows up as a failed #include here rather
# than as a configure that works on one architecture and not another.
CC="${CC:-cc}"
for tu in mir.c mir-gen.c c2mir/c2mir.c; do
    if ! "$CC" -std=gnu11 -fsyntax-only -w -Wno-psabi -I"$DEST" -I"$DEST/c2mir" \
             "$DEST/$tu"; then
        echo "error: $tu does not compile from the vendored files" >&2
        exit 1
    fi
done

# The repo's .gitignore ignores *.c / *.h outside an allowlist; a copied file
# it still ignores would build here and be missing from every clone.
if git -C "$ROOT" rev-parse --git-dir > /dev/null 2>&1; then
    # check-ignore exits 1 when it ignores nothing -- the good case.
    ignored=$(cd "$ROOT" && find external/mir -type f -print0 \
              | { git check-ignore --no-index --stdin -z || true; } | tr '\0' '\n')
    if [ -n "$ignored" ]; then
        echo "error: .gitignore ignores vendored MIR files; extend the" \
             "!external/mir/** rules:" >&2
        printf '  %s\n' $ignored >&2
        exit 1
    fi
fi

echo "external/mir: $REPO @ $COMMIT ($(du -sh "$DEST" | cut -f1))"
git -C "$ROOT" status --short -- external/mir | head -20
