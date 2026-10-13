#!/usr/bin/env bash
# stage-release.sh -- lay out a release archive's contents from a build tree.
#
# Usage:
#   scripts/stage-release.sh <build-dir> <dist-dir>
#   scripts/stage-release.sh --headers <include-dir>
#
# The first form writes the PREFIX layout every release archive ships:
#
#   <dist>/bin/tur[.exe]
#   <dist>/lib/libturi.a libturt_runtime.a libturt_preamble.a libtur_mir.a
#   <dist>/include/{turi,compiler,runtime}/*.h
#   <dist>/share/turmeric/stdlib/
#
# The second writes only the include/ part (tvm's build-from-source path, which
# lays out the rest itself).
#
# This is the one place that shape is written down.  release.yml packages
# every leg through it, and tests/check-release-sdk.sh stages the same tree
# from a local build and compiles an embedder against it, so the archive the
# release publishes is the archive the test suite exercises.  Before this, the
# layout lived only in release.yml and nothing but a release run ever saw it:
# include/turi/ shipped four headers that #include seven more, and no
# libturi embedder could build against a release from v0.47.0 on
# (docs/archive/release-tarball-turi-headers-are-unusable.md).
#
# `tur` resolves each part natively: resolve_stdlib_root step 3 is
# <exe_dir>/../share/turmeric/stdlib, locate_runtime_lib probes
# <exe_dir>/../lib, and resolve_autolink_flags adds -I<exe_dir>/../include
# when a program links -lturi.  See docs/archive/unify-release-archive-layout.md.
#
# CC (default: cc) computes and checks the header closure.
set -euo pipefail

usage() {
    echo "usage: $0 <build-dir> <dist-dir>" >&2
    echo "       $0 --headers <include-dir>" >&2
    exit 2
}
[ "$#" -eq 2 ] || usage

# Anchor the arguments at the caller's directory before moving to the root.
abs() { case $1 in /*) printf '%s' "$1" ;; *) printf '%s/%s' "$PWD" "$1" ;; esac; }
if [ "$1" = "--headers" ]; then
    mode=headers
    include=$(abs "$2")
else
    mode=archive
    build=$(abs "$1")
    dist=$(abs "$2")
    include="$dist/include"
fi
cd "$(dirname "$0")/.."
cc=${CC:-cc}

# The embedding headers.  ROOTS are the ones a program names: stdlib/turi/eval.tur
# and the notebook spice include eval.h / env.h / value.h, and
# stdlib/r7rs/eval.tur -- `(import (scheme eval))` -- includes r7rs_embed.h.
# They #include the compiler's diag/forms/symbols headers and the runtime's
# arena/buf/globals/platform headers by their src/-relative spelling
# ("compiler/diag.h"), so the closure ships under include/ with the same
# layout and -I<prefix>/include resolves every one of them.
#
# The closure is COMPUTED, not listed: a header that gains an #include ships
# the new dependency with no edit here.  (A hand-kept list is how the archive
# came to ship four headers out of eleven.)
stage_headers() {
    local dest=$1 tmp deps h rel
    local roots="turi/eval.h turi/env.h turi/value.h turi/fiber.h turi/r7rs_embed.h"
    tmp=$(mktemp -d)
    for h in $roots; do printf '#include "%s"\n' "$h"; done > "$tmp/roots.c"

    # `|| true`: an empty result is reported just below, by name, rather than
    # by set -e exiting silently on grep's status.
    deps=$("$cc" -MM -MT roots -I src "$tmp/roots.c" | tr ' \\' '\n\n' | grep '^src/' | sort -u) || true
    if [ -z "$deps" ]; then
        echo "stage-release: $cc -MM found no headers under src/ -- cannot stage include/" >&2
        rm -rf "$tmp"
        return 1
    fi
    for h in $deps; do
        rel=${h#src/}
        mkdir -p "$dest/$(dirname "$rel")"
        cp "$h" "$dest/$rel"
    done

    # The staged set must compile on its own: only -I<dest>, none of src/.
    if ! "$cc" -fsyntax-only -I "$dest" "$tmp/roots.c" -o /dev/null; then
        echo "stage-release: the staged include/ does not compile on its own" >&2
        rm -rf "$tmp"
        return 1
    fi
    rm -rf "$tmp"
    echo "stage-release: staged $(echo "$deps" | wc -l | tr -d ' ') headers into $dest"
}

if [ "$mode" = headers ]; then
    stage_headers "$include"
    exit 0
fi

# The executable: `tur` on POSIX, `tur.exe` under MSYS2.
if [ -f "$build/tur.exe" ]; then
    exe="$build/tur.exe"
elif [ -f "$build/tur" ]; then
    exe="$build/tur"
else
    echo "stage-release: no tur or tur.exe in $build" >&2
    exit 1
fi

mkdir -p "$dist/bin" "$dist/lib" "$dist/include" "$dist/share/turmeric"
cp "$exe" "$dist/bin/"

# Every copy below is a hard cp, so a build that lost an archive fails here
# rather than shipping without it.
#
# libturi.a -- the interpreter, for programs that embed it (-lturi).
cp "$build/src/libturi.a" "$dist/lib/"
# libturt_runtime.a is the one TUR_RT_AUTO will actually link.  Without it in
# the archive, `tur run` from an extracted tarball fell back to recompiling
# src/runtime/*.c -- which the archive does not ship -- and failed at the C
# compile step.  docs/archive/release-archive-cannot-compile.md.
cp "$build/src/libturt_runtime.a" "$dist/lib/"
# libturt_preamble.a is what lets a default build skip recompiling the
# runtime preamble in every program (cc-path-preamble-split-plan).  Without it
# the build still works -- it keeps the whole preamble -- so a missing copy
# would only ever show up as slowness.
cp "$build/src/libturt_preamble.a" "$dist/lib/"
# TUR_JIT defaults ON (vendored MIR), so libturi.a carries the JIT engine's
# image API and links PUBLIC against MIR: a C host that embeds libturi and
# calls that API needs libtur_mir.a beside it.  A plain `-lturi` link never
# pulls the engine's object in.
cp "$build/libtur_mir.a" "$dist/lib/"

stage_headers "$include"

cp -R stdlib "$dist/share/turmeric/"
