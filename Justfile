# Default: configure (if needed) then debug build
default: build

# ---------------------------------------------------------------------------
# Build targets
# ---------------------------------------------------------------------------

build: debug

debug:
    @[ -f build/CMakeCache.txt ] || just configure
    cmake --build build -j --config Debug

release:
    @[ -f build/CMakeCache.txt ] || just configure
    cmake --build build -j --config Release

tsan:
    @[ -f build/CMakeCache.txt ] || just configure
    cmake --build build -j --config TSan

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

configure:
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_POLICY_VERSION_MINIMUM=3.5

reconfigure:
    rm -rf build
    just configure

# ---------------------------------------------------------------------------
# Test targets
# ---------------------------------------------------------------------------

test: build doctest
    # -j is required, not decoration: without it the ~106 registered targets
    # run sequentially and end-to-end wall time becomes their SUM rather than
    # the slowest one. It is safe because the fan-out harnesses (tur_tests,
    # turi_fixture_tests, tur_jit_fixture_tests) are marked RUN_SERIAL, so
    # ctest never co-schedules two of them. Dropping it is a soft regression
    # that shows up in no individual test's timing -- see section 5 of
    # docs/guides/test-suite-portability-guide.md.
    #
    # The job count must be EXPLICIT. `ctest -j` with no number is documented
    # as `-j <jobs>` through CMake 3.28 (a bare value-less --parallel only
    # arrived in 3.29), and on 3.28 a bare -j is accepted in silence and does
    # nothing: measured on a 4-core box, five targets took 11.2s serial, 11.5s
    # with bare -j, and 5.8s with an explicit count. A bare -j therefore looks
    # like the fix while changing nothing. `getconf _NPROCESSORS_ONLN` is the
    # portable count (nproc is GNU-only; macOS has neither).
    #
    # 720s, not 300s: tests/run.sh alone is ~265s on a 4-core box and is
    # RUN_SERIAL, so a 5-minute cap killed the suite on any machine slower
    # than the one it was tuned on. 12 minutes is the repo-wide suite timeout
    # (see CLAUDE.md).
    timeout 720 ctest -j "$(getconf _NPROCESSORS_ONLN)" --output-on-failure --progress --test-dir build

# Run stdlib doctests (generate + run).
doctest: build
    python3 tools/doctest.py stdlib/ --out tests/doctest-generated/
    bash tools/run-doctests.sh

# Run only the compiled-fixture test suite (tur tests).
test-tur: build
    bash tests/run.sh

# Regenerate all expected.c codegen snapshots from the current compiler output.
# Pass --check to exit non-zero if any snapshot is out of date (used as a CI guard).
regen-snapshots *ARGS:
    #!/usr/bin/env bash
    set -euo pipefail
    TUR=./build/tur
    [ -x "$TUR" ] || { echo "error: $TUR not built; run 'just build' first" >&2; exit 2; }
    CHECK_MODE=0
    for _arg in {{ARGS}}; do
        [ "$_arg" = "--check" ] && CHECK_MODE=1
    done
    FAILED=0
    COUNT=0
    for dir in tests/fixtures/*/; do
        [ -f "$dir/expected.c" ] || continue
        name=$(basename "$dir")
        input="$dir/input.tur"
        [ -f "$input" ] || input="$dir/${name}.tur"
        [ -f "$input" ] || continue
        COUNT=$((COUNT + 1))
        # Honor a fixture's per-fixture compiler flags, exactly as tests/run.sh
        # does.  A fixture whose snapshot needs an experiment gate (e.g.
        # --enable=forall-constraints) records it in a `flags` file; emitting
        # without those flags errors out and would falsely report drift.
        # Unquoted on use so multiple flags word-split (matches run.sh).
        fixture_flags=""
        [ -f "$dir/flags" ] && fixture_flags=$(cat "$dir/flags")
        if [ "$CHECK_MODE" = "1" ]; then
            # Compare emit-c output to the snapshot by piping straight into
            # diff.  Do NOT round-trip through `$(...)` + `echo`: command
            # substitution strips trailing newlines, which falsely reports
            # drift on every snapshot even when the bytes are identical.
            if ! "$TUR" $fixture_flags emit-c "$input" 2>/dev/null | diff -q - "$dir/expected.c" >/dev/null 2>&1; then
                echo "DRIFT: $name"
                FAILED=$((FAILED + 1))
            fi
        else
            "$TUR" $fixture_flags emit-c "$input" > "$dir/expected.c" 2>/dev/null || true
        fi
    done
    if [ "$CHECK_MODE" = "1" ]; then
        if [ "$FAILED" -gt 0 ]; then
            echo "error: $FAILED/$COUNT snapshots are out of date. Run 'tur run regen-snapshots' to fix." >&2
            exit 1
        else
            echo "$COUNT snapshots are up to date."
        fi
    else
        echo "Regenerated $COUNT snapshots."
    fi

# Run the interpreter fixture test suite (turi tests).
test-turi: build
    bash tests/run-turi.sh

test-tsan: tsan
    TUR_TSAN=1 ctest -j "$(getconf _NPROCESSORS_ONLN)" --output-on-failure --test-dir build

# Run production smoke tests against live turmeric-lang.com + try.turmeric-lang.com.
# Requires: npm install run from the web/ directory (just web-deps).
smoke:
    cd web && npm run test:prod

# ---------------------------------------------------------------------------
# Windows (MSYS2 / UCRT64)
#
# The Windows toolchain lives entirely inside the MSYS2 tree (default
# C:\msys64) -- no Visual Studio, no MSVC, no VC++ redistributables.  Removing
# it is `rm -rf` on that one directory.
#
# Every recipe below re-enters a login shell with MSYSTEM=UCRT64, so they work
# from any shell (Git Bash, PowerShell, cmd) -- you do not have to be sitting
# in the MSYS2 UCRT64 terminal.  Set MSYS2_ROOT if MSYS2 is not at C:/msys64.
#
# See "Windows setup" at the bottom of this file for the from-scratch
# bootstrap (these recipes assume MSYS2 itself is already installed).
# ---------------------------------------------------------------------------

MSYS2_ROOT := env_var_or_default("MSYS2_ROOT", "C:/msys64")

# Install or refresh the Windows toolchain (gcc, clang, cmake, ninja, just). Idempotent.
setup-windows:
    #!/usr/bin/env bash
    set -euo pipefail
    MSYS_BASH="{{MSYS2_ROOT}}/usr/bin/bash"
    [ -x "$MSYS_BASH" ] || {
        echo "error: MSYS2 not found at {{MSYS2_ROOT}}" >&2
        echo "       install it with:  winget install MSYS2.MSYS2" >&2
        echo "       or set MSYS2_ROOT to an existing install." >&2
        exit 2
    }
    # `pacman -Syu` can update pacman itself, after which it wants a fresh
    # shell before it will finish the rest -- hence two passes.
    MSYSTEM=UCRT64 "$MSYS_BASH" -lc 'pacman -Syu --noconfirm'
    MSYSTEM=UCRT64 "$MSYS_BASH" -lc 'pacman -Syu --noconfirm'
    # `just` lives in the plain msys repo, not the ucrt64 one -- pacman pulls
    # from both in a single transaction.
    MSYSTEM=UCRT64 "$MSYS_BASH" -lc 'pacman -S --noconfirm --needed \
        mingw-w64-ucrt-x86_64-gcc \
        mingw-w64-ucrt-x86_64-clang \
        mingw-w64-ucrt-x86_64-cmake \
        mingw-w64-ucrt-x86_64-ninja \
        just'

# Print Windows toolchain versions and confirm it emits native PE binaries.
doctor-windows:
    #!/usr/bin/env bash
    set -euo pipefail
    # Run this first when a Windows build misbehaves: it separates "the
    # toolchain is wrong" from "the port is wrong".
    MSYSTEM=UCRT64 "{{MSYS2_ROOT}}/usr/bin/bash" -lc '
        set -e
        for c in gcc clang cc cmake ninja; do
            printf "%-6s %s\n" "$c" "$(command -v "$c" || echo MISSING)"
        done
        echo
        gcc --version | head -1
        clang --version | head -1
        cmake --version | head -1
        echo "ninja $(ninja --version)"
        echo
        # A UCRT64 gcc must produce a PE32+ binary, not an ELF.  If this line
        # says ELF, MSYSTEM was not honored and you are in the wrong subsystem.
        tmp=$(mktemp -d)
        printf "int main(void){return 0;}\n" > "$tmp/t.c"
        gcc "$tmp/t.c" -o "$tmp/t.exe"
        file "$tmp/t.exe"
        rm -rf "$tmp"
    '

# Configure a native Windows build into build-win/.
configure-windows:
    #!/usr/bin/env bash
    set -euo pipefail
    MSYS_BASH="{{MSYS2_ROOT}}/usr/bin/bash"
    # justfile_directory() hands back a backslashed Windows path; bash would
    # eat the backslashes as escapes, so translate it to a POSIX path first.
    ROOT=$(MSYSTEM=UCRT64 "$MSYS_BASH" -lc "cygpath -u '{{justfile_directory()}}'")
    MSYSTEM=UCRT64 "$MSYS_BASH" -lc "cd '$ROOT' && cmake -S . -B build-win -G Ninja \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5"

# Build tur.exe / libturi for Windows.
build-windows:
    #!/usr/bin/env bash
    set -euo pipefail
    [ -f build-win/CMakeCache.txt ] || just configure-windows
    MSYS_BASH="{{MSYS2_ROOT}}/usr/bin/bash"
    ROOT=$(MSYSTEM=UCRT64 "$MSYS_BASH" -lc "cygpath -u '{{justfile_directory()}}'")
    MSYSTEM=UCRT64 "$MSYS_BASH" -lc "cd '$ROOT' && cmake --build build-win -j"

# Drop the Windows build tree (leaves the toolchain alone).
clean-windows:
    rm -rf build-win

# ---------------------------------------------------------------------------
# Cleanup
# ---------------------------------------------------------------------------

clean:
    rm -rf build build-release build-tsan build-wasm build-win tests/out
    find tests/fixtures -name 'actual.*' -delete
    find tests/cli -name 'actual.*' -delete

clean-test:
    rm -rf tests/out
    find tests/fixtures -name 'actual.*' -delete
    find tests/cli -name 'actual.*' -delete

# ---------------------------------------------------------------------------
# Utility
# ---------------------------------------------------------------------------

repl: build
    ./build/tur repl

run file:
    ./build/tur run {{file}}

build-file file:
    ./build/tur build {{file}}

# Phase C2: build a user program with contracts stripped (release builds).
# Contract predicates are dropped at elaboration; see docs/guides/compiler-flags-guide.md.
release-stripped file:
    ./build/tur --no-contracts build {{file}} -O2

emit-c file:
    ./build/tur emit-c {{file}}

# ---------------------------------------------------------------------------
# Games / Examples (CMake targets — require Raylib)
# ---------------------------------------------------------------------------

# Enable the examples subtree (Raylib) and build the snake target.
# On a cold build, prefer installing raylib via your package manager first
# (e.g. `brew install raylib`) so CPM does not need to compile it from source.
configure-examples:
    cmake -S . -B build -DTUR_EXAMPLES=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5

snake: configure-examples
    cmake --build build --target snake

minikanren: configure-examples
    cmake --build build --target minikanren

roguelike: configure-examples
    cmake --build build --target roguelike

games: snake minikanren roguelike

run-snake: snake
    ./build/examples/snake/snake

run-minikanren: minikanren
    ./build/examples/minikanren/minikanren

run-roguelike: roguelike
    ./build/examples/roguelike-raylib/roguelike

guestbook: configure-examples
    cmake --build build --target guestbook

run-guestbook: guestbook
    ./build/examples/guestbook/guestbook

# ---------------------------------------------------------------------------
# Script examples (interpreted via `tur run`)
# ---------------------------------------------------------------------------

run-cellular-automata: build
    ./build/tur run examples/cellular-automata.tur

# ---------------------------------------------------------------------------
# Full rebuild
# ---------------------------------------------------------------------------

rebuild: clean configure build

# ---------------------------------------------------------------------------
# Documentation
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# Documentation
# ---------------------------------------------------------------------------

# Generate HTML API docs from stdlib ;;; docstrings.
# Also emits stdlib/docstrings.tur for the runtime (doc name) lookup,
# and web/public/doc-names.json for the web REPL search bar.
#
# stdlib/docstrings.tur is TRACKED, and its `doc-verified?` table comes from
# `just doctest`'s manifest (tests/doctest-generated/verified.txt), which is
# gitignored. This recipe deliberately does NOT depend on `doctest`: that would
# put a build plus the full doctest run in front of every docs regen, including
# the one inside `wasm` -> `web` -> `deploy-web`. Instead gendocs carries the
# existing table forward when the manifest is missing, and says so on stderr.
# So: run `just doctest` before this if you want the table REFRESHED; without
# it the table is preserved, never emptied.
# See docs/archive/docstrings-verified-table-zeroed-by-regen.md.
# Spice symbols are folded into doc-names.json via --extra-json so the web
# search bar surfaces stdlib + spices in a single list.
#
# OD1: every generator also emits its pages a second way -- chrome-free
# fragments into web/public/docs-pack/ -- and `genpack` merges them into
# index.json, resolves cross-links, and enforces the size budget. The body of
# each page is rendered exactly once and wrapped twice, so the website and Try
# Turmeric's in-app docs pane cannot drift. See docs/guides/offline-docs-guide.md.
#
# `--strict-links` is armed: a cross-link that resolves to nothing is a build
# failure, not a note scrolled past. genguides rewrites any local `.md` href to
# a sibling `.html` without checking the target exists, so a link to a guide
# that was never written -- or into `docs/upcoming/`, `docs/archive/`, or
# `docs/reported/`, none of which are rendered -- ships as a 404 on
# turmeric-lang.com. Link an unrendered doc by its GitHub blob URL, the way the
# guides already link CLAUDE.md.
docs: guides spices
    python3 tools/gendocs.py stdlib/ --out docs/html/api/ --emit-tur stdlib/docstrings.tur --emit-json web/public/doc-names.json --extra-json docs/html/spices/doc-names-spices.json --emit-pack web/public/docs-pack/
    python3 tools/genpack.py web/public/docs-pack/ --strict-links

# Render markdown guides to HTML pages (served at /docs/html/guides/).
guides:
    python3 tools/genguides.py docs/guides/ --out docs/html/guides/ --emit-pack web/public/docs-pack/

# Generate per-spice doc pages from the sibling ../turmeric-spices/ checkout.
# Also emits docs/html/spices/doc-names-spices.json which gendocs merges into
# web/public/doc-names.json on the next step.
spices:
    python3 tools/genspices.py --out docs/html/spices/ --emit-json docs/html/spices/doc-names-spices.json --emit-pack web/public/docs-pack/

# Bundle the rendered docs (site HTML + docs pack) for offline use off the web.
# `just docs` first, then: turmeric-docs-<version>.tar.gz, which the release cut
# attaches to the GitHub release and the Homebrew formula can install into
# share/doc/turmeric/ for `tur docs --open`. See OD4.
docs-tarball: docs
    #!/usr/bin/env bash
    set -euo pipefail
    version="$(cat VERSION)"
    out="turmeric-docs-${version}.tar.gz"
    rm -f "$out"
    tar -czf "$out" \
        --transform "s,^,turmeric-docs-${version}/," \
        -C . docs/html web/public/docs-pack
    echo "wrote $out ($(du -h "$out" | cut -f1))"


# Check that every turmeric+sweet-exp toggle pair in the guides is valid, and
# that every build.tur snippet in the guides and the README actually parses.
# README.md is in the list on purpose: it is the front door, and its manifest
# snippet is the first thing a new user copies.
check-guides:
    python3 tools/check-guide-pairs.py README.md docs/guides/

# Strict check for spice READMEs: every `turmeric block must have an adjacent
# `sweet-exp sibling (or be marked `turmeric no-check`).
check-spices:
    python3 tools/check-guide-pairs.py --spices

# Run both guide and spice README checks.
check-docs: check-guides check-spices

# ---------------------------------------------------------------------------
# WebAssembly & Web REPL targets
# ---------------------------------------------------------------------------

# Configure with WASM support
configure-wasm:
    cmake -S . -B build-wasm -DTUR_WASM=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5

alias build-wasm := wasm

# Build WASM module (requires Emscripten).
# Runs `just docs` first so stdlib/docstrings.tur is up-to-date.
wasm: docs configure-wasm
    cmake --build build-wasm -j --target tur_wasm

# Set up web dependencies (Monaco Editor, etc.)
web-deps:
    cd web && npm install

# Build web bundle (requires Vite)
web: wasm web-deps
    cd web && npm run build

# Deploy try.turmeric-lang.com to Cloudflare Workers (requires `wrangler` auth).
# Depends on `web`, so the wasm is always rebuilt from the current tree before
# publishing -- the artifacts it writes into web/public/ are gitignored and
# must not be committed.
#
# That is not the whole account of what this touches. The chain reaches `docs`
# (deploy-web -> web -> wasm -> docs), which rewrites the TRACKED
# stdlib/docstrings.tur. It is now idempotent when the doctest manifest is
# absent -- it used to empty the file's `doc-verified?` table, which is how a
# zeroed table repeatedly reached main via release commits. If a deploy leaves
# stdlib/docstrings.tur dirty, that is a real regen (new/changed docstrings),
# not noise: read the diff before discarding it.
#
# The spice check is deploy-only on purpose. `genspices.py` reports a missing
# ../turmeric-spices/ checkout as a warning and produces zero spice pages, which
# is correct for CI and for a contributor without the sibling repo -- but a
# deploy of that pack silently drops every spice from the site, the offline pack
# and the search index, and still exits 0. `docs` stays lenient; only this
# recipe insists.
deploy-web: web
    python3 tools/check-pack-spices.py web/public/docs-pack/
    cd web && npm run deploy

# Run web dev server.
# web/public/turmeric.{js,wasm} and doc-names.json are gitignored build
# outputs, so a fresh clone has none. Vite would happily serve the site with a
# 404'ing wasm and a REPL that silently never boots, or with a doc panel that
# finds nothing, so fail loudly instead of debugging that. We check rather than
# depend on `wasm` so you don't need emscripten on PATH just to iterate on CSS
# once the module has been built.
web-dev: web-deps
    #!/usr/bin/env bash
    set -euo pipefail
    if [ ! -f web/public/turmeric.wasm ] || [ ! -f web/public/turmeric.js ]; then
      echo "error: web/public/turmeric.{js,wasm} missing -- run 'just wasm' first" >&2
      echo "       (they are build outputs and are no longer committed)" >&2
      exit 1
    fi
    if [ ! -f web/public/doc-names.json ]; then
      echo "error: web/public/doc-names.json missing -- run 'just docs' first" >&2
      echo "       (it is a build output and is no longer committed; without it" >&2
      echo "        the REPL's doc panel silently finds nothing)" >&2
      exit 1
    fi
    if [ ! -f web/public/docs-pack/index.json ]; then
      echo "error: web/public/docs-pack/index.json missing -- run 'just docs' first" >&2
      echo "       (it is a build output and is not committed; without it the" >&2
      echo "        REPL's docs pane opens onto an empty nav and the service" >&2
      echo "        worker has nothing to precache for offline reading)" >&2
      exit 1
    fi
    cd web && npm run dev

# Clean WASM build
clean-wasm:
    rm -rf build-wasm

# ---------------------------------------------------------------------------
# Release tagging
# ---------------------------------------------------------------------------

# Bump the patch version (0.2.1 -> 0.2.2) and push a release tag.
bump-patch:
    #!/usr/bin/env bash
    set -euo pipefail
    OLD=$(cat VERSION)
    IFS='.' read -r MAJOR MINOR PATCH <<< "$OLD"
    NEW="$MAJOR.$MINOR.$((PATCH + 1))"
    echo "$NEW" > VERSION
    sed -i.bak "s/TURMERIC_VERSION \"$OLD\"/TURMERIC_VERSION \"$NEW\"/" src/web/wasm_glue.h
    rm -f src/web/wasm_glue.h.bak
    # sw.js carries a dev/no-build fallback copy of the version; vite rewrites
    # it in dist/, but the literal must track VERSION or an un-built serve gets
    # a stale precache. Matched by shape, not by $OLD, so a bump re-syncs it
    # even if it has already drifted -- but only on the CACHE_VERSION line,
    # since the file's comments quote old versions as history.
    sed -i.bak -E "s/(CACHE_VERSION = 'tur-try-v1-)[0-9]+\.[0-9]+\.[0-9]+/\1$NEW/" web/public/sw.js
    rm -f web/public/sw.js.bak
    git add VERSION src/web/wasm_glue.h web/public/sw.js
    git commit -m "chore: bump version to v$NEW"
    git tag -a "v$NEW" -m "Release v$NEW"
    git push origin HEAD "v$NEW"

# Bump the minor version (0.2.1 -> 0.3.0) and push a release tag.
bump-minor:
    #!/usr/bin/env bash
    set -euo pipefail
    OLD=$(cat VERSION)
    IFS='.' read -r MAJOR MINOR PATCH <<< "$OLD"
    NEW="$MAJOR.$((MINOR + 1)).0"
    echo "$NEW" > VERSION
    sed -i.bak "s/TURMERIC_VERSION \"$OLD\"/TURMERIC_VERSION \"$NEW\"/" src/web/wasm_glue.h
    rm -f src/web/wasm_glue.h.bak
    # sw.js carries a dev/no-build fallback copy of the version; vite rewrites
    # it in dist/, but the literal must track VERSION or an un-built serve gets
    # a stale precache. Matched by shape, not by $OLD, so a bump re-syncs it
    # even if it has already drifted -- but only on the CACHE_VERSION line,
    # since the file's comments quote old versions as history.
    sed -i.bak -E "s/(CACHE_VERSION = 'tur-try-v1-)[0-9]+\.[0-9]+\.[0-9]+/\1$NEW/" web/public/sw.js
    rm -f web/public/sw.js.bak
    git add VERSION src/web/wasm_glue.h web/public/sw.js
    git commit -m "chore: bump version to v$NEW"
    git tag -a "v$NEW" -m "Release v$NEW"
    git push origin HEAD "v$NEW"

# Bump the major version (0.2.1 -> 1.0.0) and push a release tag.
bump-major:
    #!/usr/bin/env bash
    set -euo pipefail
    OLD=$(cat VERSION)
    IFS='.' read -r MAJOR MINOR PATCH <<< "$OLD"
    NEW="$((MAJOR + 1)).0.0"
    echo "$NEW" > VERSION
    sed -i.bak "s/TURMERIC_VERSION \"$OLD\"/TURMERIC_VERSION \"$NEW\"/" src/web/wasm_glue.h
    rm -f src/web/wasm_glue.h.bak
    # sw.js carries a dev/no-build fallback copy of the version; vite rewrites
    # it in dist/, but the literal must track VERSION or an un-built serve gets
    # a stale precache. Matched by shape, not by $OLD, so a bump re-syncs it
    # even if it has already drifted -- but only on the CACHE_VERSION line,
    # since the file's comments quote old versions as history.
    sed -i.bak -E "s/(CACHE_VERSION = 'tur-try-v1-)[0-9]+\.[0-9]+\.[0-9]+/\1$NEW/" web/public/sw.js
    rm -f web/public/sw.js.bak
    git add VERSION src/web/wasm_glue.h web/public/sw.js
    git commit -m "chore: bump version to v$NEW"
    git tag -a "v$NEW" -m "Release v$NEW"
    git push origin HEAD "v$NEW"

# ---------------------------------------------------------------------------
# Performance comparison
# ---------------------------------------------------------------------------

PERF := "performance-comparison"

# Build a release binary (needed by perf tasks that invoke Turmeric benchmarks).
perf-build:
    cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5
    cmake --build build-rel -j --config Release

# Validate correctness of all benchmark implementations (small inputs).
perf-validate:
    cd {{PERF}} && python3 scripts/validate_correctness.py

# Regenerate golden reference output files (run once after adding benchmarks).
perf-golden:
    cd {{PERF}} && python3 scripts/validate_correctness.py --golden

# Print language runtime versions and hardware details.
perf-env:
    cd {{PERF}} && bash scripts/check_environment.sh

# Run all benchmark categories at small input size.
perf-run:
    cd {{PERF}} && bash scripts/run_all.sh

# Run all benchmark categories at the given size: small | medium | large.
perf-run-size size:
    cd {{PERF}} && bash scripts/run_all.sh all {{size}}

# Run a single category (e.g. `just perf-run-cat numerical`).
perf-run-cat category:
    cd {{PERF}} && bash scripts/run_all.sh {{category}}

# Run a single category at a given size.
perf-run-cat-size category size:
    cd {{PERF}} && bash scripts/run_all.sh {{category}} {{size}}

# Aggregate and normalize raw results (trimmed mean, C-relative speedups).
perf-aggregate:
    cd {{PERF}} && python3 scripts/aggregate_results.py

# Check reproducibility: compute CV for each benchmark group, flag CV > 10%.
perf-reproducibility:
    cd {{PERF}} && python3 scripts/check_reproducibility.py

# Identify outlier runs and compute speedup rankings.
perf-analyze:
    cd {{PERF}} && python3 scripts/analyze_results.py --print-summary

# Generate analysis markdown documents (comparison, by-category, by-language, conclusions).
perf-docs:
    cd {{PERF}} && python3 scripts/generate_analysis.py

# Generate ASCII charts and analysis/report.html.
perf-viz:
    cd {{PERF}} && python3 scripts/visualize_results.py

# Open the HTML report in the default browser.
perf-open:
    open {{PERF}}/analysis/report.html

# Full pipeline: run benchmarks → aggregate → analyze → generate docs → open report.
perf: perf-run perf-aggregate perf-analyze perf-docs perf-viz perf-open

# ---------------------------------------------------------------------------
# Windows setup -- from a clean machine
# ---------------------------------------------------------------------------
#
# The Windows toolchain is MSYS2 + the UCRT64 mingw-w64 packages.  There is no
# Visual Studio, no MSVC, and no VC++ redistributable anywhere in this setup:
# everything lands under one directory (C:\msys64 by default), and uninstalling
# is deleting that directory (or running C:\msys64\uninstall.exe).
#
#   1. Install MSYS2 itself.  winget ships with Windows 11, so:
#
#          winget install MSYS2.MSYS2
#
#      If MSYS2 ends up somewhere other than C:\msys64, export MSYS2_ROOT to
#      point at it -- every windows recipe below honors that variable.
#
#   2. Install the toolchain (gcc, clang, cmake, ninja, just) into the UCRT64
#      subsystem.  Chicken-and-egg: `just setup-windows` is what *installs*
#      just, so the very first time you have to run the pacman lines by hand.
#      Open C:\msys64\ucrt64.exe and paste:
#
#          pacman -Syu --noconfirm            # twice: the first pass may
#          pacman -Syu --noconfirm            # update pacman itself
#          pacman -S --noconfirm --needed \
#              mingw-w64-ucrt-x86_64-gcc   mingw-w64-ucrt-x86_64-clang \
#              mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja just
#
#      After that, `just setup-windows` does the same thing and is idempotent
#      -- re-run it any time to refresh the toolchain.  (Note `tur run` is NOT
#      an option for these: it needs a working tur, which is the thing you do
#      not have yet on a fresh Windows box.)
#
#   3. Confirm the toolchain is sane before blaming the build:
#
#          just doctor-windows
#
#      Every tool should resolve under /ucrt64/bin, and the final `file` line
#      must say "PE32+ executable ... x86-64".  If it says ELF, MSYSTEM was not
#      honored and you are in the wrong MSYS2 subsystem.
#
#   4. Configure and build:
#
#          just configure-windows      # -> build-win/
#          just build-windows          # -> build-win/tur.exe
#
# Working interactively instead?  Launch C:\msys64\ucrt64.exe (Start menu:
# "MSYS2 UCRT64" -- NOT the plain "MSYS2" entry, which is a different
# subsystem and will not have the toolchain on PATH).  `echo $MSYSTEM` must
# print UCRT64.  Inside that shell the raw cmake/ninja commands work directly,
# with no wrapper.
#
# STATUS: the Windows port is not finished -- see
# docs/archive/windows-support-plan.md.  `configure-windows` currently stops
# at the architecture dispatch in src/CMakeLists.txt (CMAKE_SYSTEM_PROCESSOR is
# empty under MSYS2).  The toolchain above is correct; the build is what still
# needs work.
# ---------------------------------------------------------------------------
