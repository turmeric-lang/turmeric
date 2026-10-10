#!/usr/bin/env bash
# tests/spice-resolver-tests.sh -- per-file subcommand include-path tests.
#
# Exercises the SC0/SC1 surface of the spice-aware-check plan:
#
#   SC0: `tur check <file>` on a missing intra-spice import emits the
#        multi-line "searched: ... hint: ..." diagnostic.
#   SC1: `tur check -I <src> <file>` resolves intra-spice imports and exits 0.
#
# Later SC phases extend this script (SC2 wires emit-c/emit-h/format/run;
# SC4 adds auto-discovery and the --no-auto-spice escape hatch). New cases
# should follow the assert_* helpers below.

set -u
cd "$(dirname "$0")/.."

TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || { echo "tests: $TUR not built; run 'just build' first" >&2; exit 2; }

FIXTURE="tests/fixtures/spice-resolver-ok"
SRC_ROOT="$FIXTURE/src"
ENTRY="$SRC_ROOT/foo/b.tur"

# SC5 fixtures: a consumer spice that depends on a sibling producer spice
# via :spices :path.  See tests/fixtures/spice-resolver-deps/build.tur.
DEPS_FIXTURE="tests/fixtures/spice-resolver-deps"
DEPS_ENTRY="$DEPS_FIXTURE/src/app/main.tur"

PASS=0
FAIL=0
FAILED=()

# assert_exit <expected-exit> <case-label> <cmd...>
# Runs the command; if exit code matches, PASS; else FAIL and dump stderr.
assert_exit() {
    local want="$1" label="$2"; shift 2
    local out err rc
    out=$(mktemp); err=$(mktemp)
    "$@" >"$out" 2>"$err"
    rc=$?
    if [ "$rc" -eq "$want" ]; then
        echo "PASS $label"
        PASS=$((PASS + 1))
    else
        echo "FAIL $label -- expected exit $want, got $rc"
        echo "  cmd: $*"
        echo "  stdout:" ; sed 's/^/    /' "$out"
        echo "  stderr:" ; sed 's/^/    /' "$err"
        FAIL=$((FAIL + 1))
        FAILED+=("$label")
    fi
    rm -f "$out" "$err"
}

# assert_stderr_contains <needle> <case-label> <cmd...>
# Runs the command (ignoring exit code) and asserts stderr contains the
# literal substring `needle`.
assert_stderr_contains() {
    local needle="$1" label="$2"; shift 2
    local err
    err=$(mktemp)
    "$@" >/dev/null 2>"$err"
    if grep -F -q "$needle" "$err"; then
        echo "PASS $label"
        PASS=$((PASS + 1))
    else
        echo "FAIL $label -- stderr missing substring:"
        echo "    $needle"
        echo "  cmd: $*"
        echo "  stderr:"; sed 's/^/    /' "$err"
        FAIL=$((FAIL + 1))
        FAILED+=("$label")
    fi
    rm -f "$err"
}

# SC1: `tur check -I src <file>` resolves intra-spice imports and exits 0.
assert_exit 0 "SC1: tur check -I <src> resolves intra-spice import" \
    "$TUR" check -I "$SRC_ROOT" "$ENTRY"

# SC1: short -I form (`-I<path>`) also works.
assert_exit 0 "SC1: tur check -I<src> (no space) also works" \
    "$TUR" check -I"$SRC_ROOT" "$ENTRY"

# SC0: `tur check <file>` without -I fails with the new searched/hint diag.
# Pass --no-auto-spice so SC4 auto-discovery doesn't quietly satisfy the
# import; the test exists to prove the diagnostic still triggers for users
# who explicitly opt out.
assert_exit 1 "SC0: tur check (--no-auto-spice, no -I) fails on intra-spice import" \
    "$TUR" --no-auto-spice check "$ENTRY"

assert_stderr_contains "searched:" \
    "SC0: diagnostic lists searched paths" \
    "$TUR" --no-auto-spice check "$ENTRY"

assert_stderr_contains "intra-spice import" \
    "SC0: diagnostic mentions intra-spice import" \
    "$TUR" --no-auto-spice check "$ENTRY"

# SC2: `tur emit-c -I <src> <file>` succeeds and emits valid C to stdout.
assert_exit 0 "SC2: tur emit-c -I <src> resolves intra-spice import" \
    "$TUR" emit-c -I "$SRC_ROOT" "$ENTRY"

# SC2: `tur emit-c` without -I (and without auto-discovery) fails.
assert_exit 1 "SC2: tur emit-c (--no-auto-spice, no -I) fails on intra-spice import" \
    "$TUR" --no-auto-spice emit-c "$ENTRY"

# SC2: `tur emit-h -I <src> <file>` succeeds.
assert_exit 0 "SC2: tur emit-h -I <src> resolves intra-spice import" \
    "$TUR" emit-h -I "$SRC_ROOT" "$ENTRY"

# SC2: `tur emit-h` without -I (and without auto-discovery) fails.
assert_exit 1 "SC2: tur emit-h (--no-auto-spice, no -I) fails on intra-spice import" \
    "$TUR" --no-auto-spice emit-h "$ENTRY"

# SC2: `tur emit-c --output-dir <dir> -I <src> <file>` succeeds and writes
# files to the output directory.
EMIT_OUT=$(mktemp -d)
assert_exit 0 "SC2: tur emit-c --output-dir -I <src> succeeds" \
    "$TUR" emit-c -I "$SRC_ROOT" --output-dir "$EMIT_OUT" "$ENTRY"
if [ -f "$EMIT_OUT/b.h" ] && [ -f "$EMIT_OUT/b.c" ]; then
    echo "PASS SC2: emit-c --output-dir produced b.h and b.c"
    PASS=$((PASS + 1))
else
    echo "FAIL SC2: emit-c --output-dir missing b.h or b.c in $EMIT_OUT"
    ls -la "$EMIT_OUT" || true
    FAIL=$((FAIL + 1))
    FAILED+=("SC2: emit-c --output-dir output present")
fi
rm -rf "$EMIT_OUT"

# SC2: `tur run -I <src> <file>` succeeds and the produced binary exits with
# the value `main` returns (42 in our fixture).
assert_exit 42 "SC2: tur run -I <src> compiles and executes intra-spice import" \
    "$TUR" run -I "$SRC_ROOT" "$ENTRY"

# SC4: with auto-discovery, `tur check <file>` (no -I) inside a spice now
# succeeds because find_spice_root walks up from b.tur to the build.tur
# at the fixture root and adds <root>/src to the include path.
assert_exit 0 "SC4: tur check auto-discovers enclosing spice src/" \
    "$TUR" check "$ENTRY"

assert_exit 0 "SC4: tur emit-c auto-discovers enclosing spice src/" \
    "$TUR" emit-c "$ENTRY"

assert_exit 0 "SC4: tur emit-h auto-discovers enclosing spice src/" \
    "$TUR" emit-h "$ENTRY"

assert_exit 42 "SC4: tur run auto-discovers enclosing spice src/" \
    "$TUR" run "$ENTRY"

# SC4 escape hatch: --no-auto-spice restores the pre-SC4 behavior, so
# the check should fail again with the SC0 diagnostic.
assert_exit 1 "SC4: --no-auto-spice opts out of auto-discovery (check fails again)" \
    "$TUR" --no-auto-spice check "$ENTRY"

assert_stderr_contains "intra-spice import" \
    "SC4: --no-auto-spice falls through to SC0 hint" \
    "$TUR" --no-auto-spice check "$ENTRY"

# SC4-build (tur-build-single-file-no-spice-autodiscover): `tur build <file>`
# is the last per-file subcommand to gain auto-discovery. Like check/emit-c/
# emit-h/run, it now walks up from the input to the enclosing build.tur and
# adds <root>/src to the include path, so the intra-spice `import foo/a`
# resolves with no explicit -I. The produced binary runs and returns 42.
SC4B_BIN=$(mktemp)
"$TUR" build "$ENTRY" -o "$SC4B_BIN" >/dev/null 2>&1
sc4b_rc=$?
if [ "$sc4b_rc" -eq 0 ] && [ -x "$SC4B_BIN" ]; then
    "$SC4B_BIN" >/dev/null 2>&1
    sc4b_run=$?
    if [ "$sc4b_run" -eq 42 ]; then
        echo "PASS SC4-build: tur build auto-discovers spice src/ (binary returns 42)"
        PASS=$((PASS + 1))
    else
        echo "FAIL SC4-build: tur build binary returned $sc4b_run (want 42)"
        FAIL=$((FAIL + 1))
        FAILED+=("SC4-build: tur build binary value")
    fi
else
    echo "FAIL SC4-build: tur build did not auto-discover spice src/ (exit $sc4b_rc)"
    FAIL=$((FAIL + 1))
    FAILED+=("SC4-build: tur build auto-discovery")
fi
rm -f "$SC4B_BIN"

# SC4-build regression: --no-auto-spice opts the single-file build out too,
# so the intra-spice import is unresolved and the build fails with the SC0
# diagnostic (confirms the opt-out still composes with build).
assert_exit 1 "SC4-build: --no-auto-spice disables build auto-discovery" \
    "$TUR" --no-auto-spice build "$ENTRY" -o /dev/null

assert_stderr_contains "intra-spice import" \
    "SC4-build: --no-auto-spice build falls through to SC0 hint" \
    "$TUR" --no-auto-spice build "$ENTRY" -o /dev/null

# SC6: --json output exercises the same auto-discovery code path the LSP
# server uses via tur_check_only().  No -I, no --no-auto-spice; expect a
# successful JSON envelope and exit 0.
assert_exit 0 "SC6: tur --json check inherits auto-discovery (LSP-equivalent path)" \
    "$TUR" --json check "$ENTRY"

# SC5: with cross-spice deps wired via auto-discovery, `tur check` on a
# consumer spice resolves `import helper/util` (from the sibling
# spice-resolver-dep) without any explicit -I.
assert_exit 0 "SC5: tur check resolves :spices :path dep via auto-discovery" \
    "$TUR" check "$DEPS_ENTRY"

# SC5: same for emit-c.
assert_exit 0 "SC5: tur emit-c resolves :spices :path dep via auto-discovery" \
    "$TUR" emit-c "$DEPS_ENTRY"

# SC5: tur run also resolves the dep and produces a working binary.
# The dep's `bonus` returns 100, so main's exit code is 100.
assert_exit 100 "SC5: tur run with :spices :path dep returns dep's value (100)" \
    "$TUR" run "$DEPS_ENTRY"

# SC5-build: cross-spice deps resolve under `tur build <file>` too -- the
# manifest's `:spices :path` producer src/ joins the include path, so the
# consumer's `import helper/util` resolves and the binary returns the dep's
# value (100).
SC5B_BIN=$(mktemp)
"$TUR" build "$DEPS_ENTRY" -o "$SC5B_BIN" >/dev/null 2>&1
sc5b_rc=$?
if [ "$sc5b_rc" -eq 0 ] && [ -x "$SC5B_BIN" ]; then
    "$SC5B_BIN" >/dev/null 2>&1
    sc5b_run=$?
    if [ "$sc5b_run" -eq 100 ]; then
        echo "PASS SC5-build: tur build resolves :spices :path dep (binary returns 100)"
        PASS=$((PASS + 1))
    else
        echo "FAIL SC5-build: tur build dep binary returned $sc5b_run (want 100)"
        FAIL=$((FAIL + 1))
        FAILED+=("SC5-build: tur build dep binary value")
    fi
else
    echo "FAIL SC5-build: tur build did not resolve :spices :path dep (exit $sc5b_rc)"
    FAIL=$((FAIL + 1))
    FAILED+=("SC5-build: tur build dep auto-discovery")
fi
rm -f "$SC5B_BIN"

# SC5: --no-auto-spice disables the manifest read too, so the same check
# falls back to the SC0 diagnostic for the missing helper/util.
assert_exit 1 "SC5: --no-auto-spice disables :spices auto-resolution as well" \
    "$TUR" --no-auto-spice check "$DEPS_ENTRY"

# LS2 (local-spice-dev-workflow): workspace member auto-resolution.
# beta imports alpha/util by name with no :spices entry; the resolver
# walks up to the workspace build.tur, sees beta is a listed member, and
# adds sibling members' src/ dirs to the search path.
WS_FIXTURE="tests/fixtures/workspace-ls2"
WS_ENTRY="$WS_FIXTURE/spices/beta/src/beta/main.tur"

assert_exit 0 "LS2: tur check resolves sibling member via :members" \
    "$TUR" check "$WS_ENTRY"

assert_exit 0 "LS2: tur emit-c resolves sibling member via :members" \
    "$TUR" emit-c "$WS_ENTRY"

# Sibling's `bonus` returns 42; main returns it, so binary exits 42.
assert_exit 42 "LS2: tur run with sibling-member import returns 42" \
    "$TUR" run "$WS_ENTRY"

# --no-auto-spice disables manifest discovery, so the sibling import fails.
assert_exit 1 "LS2: --no-auto-spice disables workspace-sibling resolution" \
    "$TUR" --no-auto-spice check "$WS_ENTRY"

# LS2 warning: undeclared sibling import emits a one-time advisory naming
# the sibling's workspace-relative dir and pointing at TUR_DEBUG_RESOLVER.
assert_stderr_contains "resolved via workspace sibling 'spices/alpha'" \
    "LS2: warning fires when sibling import is undeclared" \
    "$TUR" check "$WS_ENTRY"

assert_stderr_contains "TUR_DEBUG_RESOLVER" \
    "LS2: warning mentions TUR_DEBUG_RESOLVER opt-in" \
    "$TUR" check "$WS_ENTRY"

# LS2 breadcrumb: TUR_DEBUG_RESOLVER=1 logs every search-path addition
# attributed to workspace siblings.
assert_stderr_contains "added workspace sibling 'spices/alpha'" \
    "LS2: TUR_DEBUG_RESOLVER=1 prints search-path breadcrumb" \
    env TUR_DEBUG_RESOLVER=1 "$TUR" check "$WS_ENTRY"

# LS3 (local-spice-dev-workflow §3): `tur fetch` skips local-source deps
# (entries with `:path` or names matching a workspace member) and does
# not record them in tur.lock.  Direct `:path` deps that point at a
# nonexistent or build-less directory are a hard error.
#
# The LS3 cases below run inside per-case scratch dirs because they
# write tur.lock; the fixture trees themselves are read-only.
LS3_ABS_TUR="$PWD/$TUR"

# LS3 case A: `:path` dep fetches no remote and writes no lock row.
LS3_A=$(mktemp -d)
mkdir -p "$LS3_A/sib/src/sib" "$LS3_A/consumer/src/consumer"
cat >"$LS3_A/sib/build.tur" <<EOF
(defpackage sib :name "sib")
EOF
cat >"$LS3_A/consumer/build.tur" <<EOF
(defpackage consumer
  :name "consumer"
  :spices #{
    "sib" #{:path "../sib"}
  })
EOF
LS3_A_OUT=$(mktemp); LS3_A_ERR=$(mktemp)
( cd "$LS3_A/consumer" && "$LS3_ABS_TUR" fetch ) >"$LS3_A_OUT" 2>"$LS3_A_ERR"
rc=$?
if [ "$rc" -eq 0 ] && ! grep -qF '"sib"' "$LS3_A/consumer/tur.lock"; then
    echo "PASS LS3: tur fetch with :path dep exits 0 and writes no lock row"
    PASS=$((PASS + 1))
else
    echo "FAIL LS3: tur fetch with :path dep -- exit $rc"
    echo "  tur.lock:"; sed 's/^/    /' "$LS3_A/consumer/tur.lock" 2>/dev/null
    echo "  stderr:"; sed 's/^/    /' "$LS3_A_ERR"
    FAIL=$((FAIL + 1))
    FAILED+=("LS3: :path dep produces no lock row")
fi
rm -f "$LS3_A_OUT" "$LS3_A_ERR"
rm -rf "$LS3_A"

# LS3 case B: a stale URL row left over from a previous lock is removed
# once the dep is switched to `:path`.
LS3_B=$(mktemp -d)
mkdir -p "$LS3_B/sib" "$LS3_B/consumer"
cat >"$LS3_B/sib/build.tur" <<EOF
(defpackage sib :name "sib")
EOF
cat >"$LS3_B/consumer/build.tur" <<EOF
(defpackage consumer
  :name "consumer"
  :spices #{
    "sib" #{:path "../sib"}
  })
EOF
cat >"$LS3_B/consumer/tur.lock" <<'EOF'
(deflockfile
  :format-version 1
  :spices #{
    "sib" #{:url "https://example.com/sib" :ref "v0.1.0" :resolved "abc" :sha256 "def" :fetched-at "2024-01-01T00:00:00Z"}
  }
  :cmake-deps #{
  })
EOF
( cd "$LS3_B/consumer" && "$LS3_ABS_TUR" fetch ) >/dev/null 2>&1
if ! grep -qF '"sib"' "$LS3_B/consumer/tur.lock"; then
    echo "PASS LS3: tur fetch prunes stale URL row when dep is now :path"
    PASS=$((PASS + 1))
else
    echo "FAIL LS3: stale URL row not pruned"
    echo "  tur.lock:"; sed 's/^/    /' "$LS3_B/consumer/tur.lock"
    FAIL=$((FAIL + 1))
    FAILED+=("LS3: stale URL row pruned for :path dep")
fi
rm -rf "$LS3_B"

# LS3 case C: missing `:path` directory is a hard error.
LS3_C=$(mktemp -d)
mkdir -p "$LS3_C/consumer"
cat >"$LS3_C/consumer/build.tur" <<EOF
(defpackage consumer
  :name "consumer"
  :spices #{
    "missing" #{:path "../nope-not-here"}
  })
EOF
LS3_C_ERR=$(mktemp)
( cd "$LS3_C/consumer" && "$LS3_ABS_TUR" fetch ) >/dev/null 2>"$LS3_C_ERR"
rc=$?
if [ "$rc" -ne 0 ] && grep -qF 'does not exist or contains no build.tur' "$LS3_C_ERR"; then
    echo "PASS LS3: missing :path directory is a hard error"
    PASS=$((PASS + 1))
else
    echo "FAIL LS3: missing :path should error but didn't"
    echo "  exit: $rc (want non-zero)"
    echo "  stderr:"; sed 's/^/    /' "$LS3_C_ERR"
    FAIL=$((FAIL + 1))
    FAILED+=("LS3: missing :path hard error")
fi
rm -f "$LS3_C_ERR"
rm -rf "$LS3_C"

# LS3 case D: a `:spices` entry whose name matches a workspace member is
# skipped by `tur fetch` -- no remote fetch attempt is made even though a
# bogus :url is declared.
LS3_D=$(mktemp -d)
mkdir -p "$LS3_D/ws/alpha" "$LS3_D/ws/beta"
cat >"$LS3_D/ws/build.tur" <<'EOF'
(defpackage ws :name "ws" :members ["alpha" "beta"])
EOF
cat >"$LS3_D/ws/alpha/build.tur" <<'EOF'
(defpackage alpha :name "alpha")
EOF
cat >"$LS3_D/ws/beta/build.tur" <<'EOF'
(defpackage beta
  :name "beta"
  :spices #{
    "alpha" #{:url "https://example.invalid/should-never-fetch" :ref "v9.9.9"}
  })
EOF
LS3_D_ERR=$(mktemp)
( cd "$LS3_D/ws/beta" && "$LS3_ABS_TUR" fetch ) >/dev/null 2>"$LS3_D_ERR"
rc=$?
if [ "$rc" -eq 0 ] \
   && ! grep -qF '"alpha"' "$LS3_D/ws/beta/tur.lock" \
   && ! grep -qF 'fetching' "$LS3_D_ERR"; then
    echo "PASS LS3: workspace-member dep skipped by tur fetch"
    PASS=$((PASS + 1))
else
    echo "FAIL LS3: workspace-member dep not skipped -- exit $rc"
    echo "  tur.lock:"; sed 's/^/    /' "$LS3_D/ws/beta/tur.lock" 2>/dev/null
    echo "  stderr:"; sed 's/^/    /' "$LS3_D_ERR"
    FAIL=$((FAIL + 1))
    FAILED+=("LS3: workspace-member dep skipped by tur fetch")
fi
rm -f "$LS3_D_ERR"
rm -rf "$LS3_D"

# LS4 (local-spice-dev-workflow §4): resolution is decoupled from the
# lockfile for local-source deps.  `tur check` on a :path dep, a workspace
# sibling, or a workspace member declared as a URL :spices entry must
# succeed with no tur.lock file present at all.
LS4_ABS_TUR="$PWD/$TUR"

# LS4 case A: :path dep resolves with no tur.lock.
LS4_A=$(mktemp -d)
mkdir -p "$LS4_A/sib/src/sib" "$LS4_A/consumer/src/consumer"
cat >"$LS4_A/sib/build.tur" <<'EOF'
(defpackage sib :name "sib")
EOF
cat >"$LS4_A/sib/src/sib/api.tur" <<'EOF'
(defmodule sib/api
  (export answer)
  (defn answer [] :int 42))
EOF
cat >"$LS4_A/consumer/build.tur" <<'EOF'
(defpackage consumer
  :name "consumer"
  :spices #{
    "sib" #{:path "../sib"}
  })
EOF
cat >"$LS4_A/consumer/src/consumer/main.tur" <<'EOF'
(defmodule consumer/main
  (import sib/api :refer [answer])
  (defn main [] :int (answer)))
EOF
LS4_A_ERR=$(mktemp)
( cd "$LS4_A/consumer" && "$LS4_ABS_TUR" check src/consumer/main.tur ) >/dev/null 2>"$LS4_A_ERR"
rc=$?
if [ "$rc" -eq 0 ] && [ ! -f "$LS4_A/consumer/tur.lock" ]; then
    echo "PASS LS4: :path dep resolves with no tur.lock"
    PASS=$((PASS + 1))
else
    echo "FAIL LS4: :path dep resolution requires no lockfile"
    echo "  exit: $rc"
    echo "  lock present: $([ -f "$LS4_A/consumer/tur.lock" ] && echo yes || echo no)"
    echo "  stderr:"; sed 's/^/    /' "$LS4_A_ERR"
    FAIL=$((FAIL + 1))
    FAILED+=("LS4: :path dep no lockfile")
fi
rm -f "$LS4_A_ERR"
rm -rf "$LS4_A"

# LS4 case B: :path dep resolves even when tur.lock exists but has no
# row for the dep (the historical "lockfile gates resolution" bug).
LS4_B=$(mktemp -d)
mkdir -p "$LS4_B/sib/src/sib" "$LS4_B/consumer/src/consumer"
cat >"$LS4_B/sib/build.tur" <<'EOF'
(defpackage sib :name "sib")
EOF
cat >"$LS4_B/sib/src/sib/api.tur" <<'EOF'
(defmodule sib/api
  (export answer)
  (defn answer [] :int 42))
EOF
cat >"$LS4_B/consumer/build.tur" <<'EOF'
(defpackage consumer
  :name "consumer"
  :spices #{
    "sib" #{:path "../sib"}
  })
EOF
cat >"$LS4_B/consumer/src/consumer/main.tur" <<'EOF'
(defmodule consumer/main
  (import sib/api :refer [answer])
  (defn main [] :int (answer)))
EOF
cat >"$LS4_B/consumer/tur.lock" <<'EOF'
(deflockfile
  :format-version 1
  :spices #{
  }
  :cmake-deps #{
  })
EOF
LS4_B_ERR=$(mktemp)
( cd "$LS4_B/consumer" && "$LS4_ABS_TUR" check src/consumer/main.tur ) >/dev/null 2>"$LS4_B_ERR"
rc=$?
if [ "$rc" -eq 0 ]; then
    echo "PASS LS4: :path dep resolves with empty tur.lock (no row required)"
    PASS=$((PASS + 1))
else
    echo "FAIL LS4: :path dep resolution requires a matching lock row"
    echo "  exit: $rc"
    echo "  stderr:"; sed 's/^/    /' "$LS4_B_ERR"
    FAIL=$((FAIL + 1))
    FAILED+=("LS4: :path dep no lock row")
fi
rm -f "$LS4_B_ERR"
rm -rf "$LS4_B"

# LS4 case C: workspace sibling import resolves with no tur.lock in
# either the workspace root or the member directory.
LS4_C=$(mktemp -d)
mkdir -p "$LS4_C/ws/alpha/src/alpha" "$LS4_C/ws/beta/src/beta"
cat >"$LS4_C/ws/build.tur" <<'EOF'
(defpackage ws :name "ws" :members ["alpha" "beta"])
EOF
cat >"$LS4_C/ws/alpha/build.tur" <<'EOF'
(defpackage alpha :name "alpha")
EOF
cat >"$LS4_C/ws/alpha/src/alpha/util.tur" <<'EOF'
(defmodule alpha/util
  (export bonus)
  (defn bonus [] :int 42))
EOF
cat >"$LS4_C/ws/beta/build.tur" <<'EOF'
(defpackage beta :name "beta")
EOF
cat >"$LS4_C/ws/beta/src/beta/main.tur" <<'EOF'
(defmodule beta/main
  (import alpha/util :refer [bonus])
  (defn main [] :int (bonus)))
EOF
LS4_C_ERR=$(mktemp)
( cd "$LS4_C/ws/beta" && "$LS4_ABS_TUR" check src/beta/main.tur ) >/dev/null 2>"$LS4_C_ERR"
rc=$?
if [ "$rc" -eq 0 ] \
   && [ ! -f "$LS4_C/ws/tur.lock" ] \
   && [ ! -f "$LS4_C/ws/beta/tur.lock" ]; then
    echo "PASS LS4: workspace sibling resolves with no tur.lock anywhere"
    PASS=$((PASS + 1))
else
    echo "FAIL LS4: workspace sibling needs a lockfile to resolve"
    echo "  exit: $rc"
    echo "  ws lock:   $([ -f "$LS4_C/ws/tur.lock" ] && echo yes || echo no)"
    echo "  beta lock: $([ -f "$LS4_C/ws/beta/tur.lock" ] && echo yes || echo no)"
    echo "  stderr:"; sed 's/^/    /' "$LS4_C_ERR"
    FAIL=$((FAIL + 1))
    FAILED+=("LS4: workspace sibling no lockfile")
fi
rm -f "$LS4_C_ERR"
rm -rf "$LS4_C"

# LS4 case D: a `:spices` entry that names a workspace sibling but ALSO
# declares a (deliberately bogus) :url/:ref does not trigger a remote
# fetch and does not need a lockfile row.  `tur run` must resolve the
# sibling via the workspace, not via the URL.  The bogus URL would error
# out if the run path tried to contact it.
LS4_D=$(mktemp -d)
mkdir -p "$LS4_D/ws/alpha/src/alpha" "$LS4_D/ws/beta/src"
cat >"$LS4_D/ws/build.tur" <<'EOF'
(defpackage ws :name "ws" :members ["alpha" "beta"])
EOF
cat >"$LS4_D/ws/alpha/build.tur" <<'EOF'
(defpackage alpha :name "alpha")
EOF
cat >"$LS4_D/ws/alpha/src/alpha/util.tur" <<'EOF'
(defmodule alpha/util
  (export bonus)
  (defn bonus [] :int 42))
EOF
cat >"$LS4_D/ws/beta/build.tur" <<'EOF'
(defpackage beta
  :name "beta"
  :spices #{
    "alpha" #{:url "https://example.invalid/should-never-fetch" :ref "v9.9.9"}
  })
EOF
cat >"$LS4_D/ws/beta/src/main.tur" <<'EOF'
(defmodule main
  (import alpha/util :refer [bonus])
  (defn main [] :int (bonus)))
EOF
LS4_D_ERR=$(mktemp)
( cd "$LS4_D/ws/beta" && "$LS4_ABS_TUR" check src/main.tur ) >/dev/null 2>"$LS4_D_ERR"
rc=$?
if [ "$rc" -eq 0 ] && ! grep -qF 'fetching' "$LS4_D_ERR"; then
    echo "PASS LS4: workspace-member URL :spices entry resolves locally, no fetch"
    PASS=$((PASS + 1))
else
    echo "FAIL LS4: workspace-member URL entry tried to fetch (or failed to resolve)"
    echo "  exit: $rc"
    echo "  stderr:"; sed 's/^/    /' "$LS4_D_ERR"
    FAIL=$((FAIL + 1))
    FAILED+=("LS4: workspace-member URL entry local resolution")
fi
rm -f "$LS4_D_ERR"
rm -rf "$LS4_D"

# LS5 (local-spice-dev-workflow §5): partial-fetch failure is isolated.
# A single broken URL dep must not strip healthy deps from the search
# path.  `tur check` and `tur run` on files that only reference healthy
# deps keep working; files that actually import from the broken dep
# still fail with a normal "module not found" diagnostic.
LS5_ABS_TUR="$PWD/$TUR"

# LS5 case A: `tur check` on a file that only imports from a healthy
# :path dep succeeds even when a sibling :url dep is bogus.
LS5_A=$(mktemp -d)
mkdir -p "$LS5_A/sib/src/sib" "$LS5_A/consumer/src/consumer"
cat >"$LS5_A/sib/build.tur" <<'EOF'
(defpackage sib :name "sib")
EOF
cat >"$LS5_A/sib/src/sib/api.tur" <<'EOF'
(defmodule sib/api
  (export answer)
  (defn answer [] :int 42))
EOF
cat >"$LS5_A/consumer/build.tur" <<EOF
(defpackage consumer
  :name "consumer"
  :spices #{
    "sib"    #{:path "../sib"}
    "broken" #{:url "file:///nonexistent-ls5a-$$.git" :ref "v0.0.0"}
  })
EOF
cat >"$LS5_A/consumer/src/consumer/main.tur" <<'EOF'
(defmodule consumer/main
  (import sib/api :refer [answer])
  (defn main [] :int (answer)))
EOF
LS5_A_ERR=$(mktemp)
( cd "$LS5_A/consumer" && "$LS5_ABS_TUR" check src/consumer/main.tur ) \
    >/dev/null 2>"$LS5_A_ERR"
rc=$?
if [ "$rc" -eq 0 ]; then
    echo "PASS LS5: tur check on healthy-only imports ignores broken URL dep"
    PASS=$((PASS + 1))
else
    echo "FAIL LS5: tur check failed despite using only healthy deps"
    echo "  exit: $rc (want 0)"
    echo "  stderr:"; sed 's/^/    /' "$LS5_A_ERR"
    FAIL=$((FAIL + 1))
    FAILED+=("LS5: tur check isolates broken URL dep")
fi
rm -f "$LS5_A_ERR"
rm -rf "$LS5_A"

# LS5 case B: `tur run` in project mode used to abort on any
# pkg_fetch_all failure ("dependency fetch failed").  After LS5 it
# warns and continues; the binary produced from the healthy-only entry
# file runs and returns its value.
LS5_B=$(mktemp -d)
mkdir -p "$LS5_B/sib/src/sib" "$LS5_B/consumer/src"
cat >"$LS5_B/sib/build.tur" <<'EOF'
(defpackage sib :name "sib")
EOF
cat >"$LS5_B/sib/src/sib/api.tur" <<'EOF'
(defmodule sib/api
  (export answer)
  (defn answer [] :int 42))
EOF
cat >"$LS5_B/consumer/build.tur" <<EOF
(defpackage consumer
  :name "consumer"
  :spices #{
    "sib"    #{:path "../sib"}
    "broken" #{:url "file:///nonexistent-ls5b-$$.git" :ref "v0.0.0"}
  })
EOF
cat >"$LS5_B/consumer/src/main.tur" <<'EOF'
(defmodule main
  (import sib/api :refer [answer])
  (defn main [] :int (answer)))
EOF
LS5_B_ERR=$(mktemp)
( cd "$LS5_B/consumer" && "$LS5_ABS_TUR" run --release ) \
    >/dev/null 2>"$LS5_B_ERR"
rc=$?
if [ "$rc" -eq 42 ]; then
    echo "PASS LS5: tur run continues past broken URL; healthy dep value (42) propagates"
    PASS=$((PASS + 1))
else
    echo "FAIL LS5: tur run aborted on partial fetch failure"
    echo "  exit: $rc (want 42)"
    echo "  stderr:"; sed 's/^/    /' "$LS5_B_ERR"
    FAIL=$((FAIL + 1))
    FAILED+=("LS5: tur run isolates broken URL dep")
fi
rm -f "$LS5_B_ERR"
rm -rf "$LS5_B"

# LS5 case C: a file that DOES import from the broken dep still fails.
# Isolation does not mean silent ignore -- only that healthy deps stay
# usable.  The diagnostic is the standard "module not found", scoped to
# the import that actually needs the missing source.
LS5_C=$(mktemp -d)
mkdir -p "$LS5_C/consumer/src/consumer"
cat >"$LS5_C/consumer/build.tur" <<EOF
(defpackage consumer
  :name "consumer"
  :spices #{
    "broken" #{:url "file:///nonexistent-ls5c-$$.git" :ref "v0.0.0"}
  })
EOF
cat >"$LS5_C/consumer/src/consumer/main.tur" <<'EOF'
(defmodule consumer/main
  (import broken/missing :refer [whatever])
  (defn main [] :int (whatever)))
EOF
LS5_C_ERR=$(mktemp)
( cd "$LS5_C/consumer" && "$LS5_ABS_TUR" check src/consumer/main.tur ) \
    >/dev/null 2>"$LS5_C_ERR"
rc=$?
if [ "$rc" -ne 0 ]; then
    echo "PASS LS5: file importing the broken dep still fails (isolation, not silence)"
    PASS=$((PASS + 1))
else
    echo "FAIL LS5: file importing the broken dep should have failed"
    echo "  exit: $rc (want non-zero)"
    echo "  stderr:"; sed 's/^/    /' "$LS5_C_ERR"
    FAIL=$((FAIL + 1))
    FAILED+=("LS5: file importing broken dep still fails")
fi
rm -f "$LS5_C_ERR"
rm -rf "$LS5_C"

# LS6 (local-spice-dev-workflow §6): CLI ergonomics.
#   - `tur add --workspace <name>` asserts <name> is a workspace member
#     of the enclosing workspace and exits without modifying build.tur.
#   - `tur fetch --dry-run` classifies each :spices entry as
#     fetch/skip(:path)/skip(workspace member) without performing any
#     fetches or writing tur.lock.
LS6_ABS_TUR="$PWD/$TUR"

# LS6 case A: `tur add --workspace alpha` from beta succeeds, prints
# the "no manifest entry needed" hint, and leaves build.tur untouched.
LS6_A=$(mktemp -d)
mkdir -p "$LS6_A/ws/alpha" "$LS6_A/ws/beta"
cat >"$LS6_A/ws/build.tur" <<'EOF'
(defpackage ws :name "ws" :members ["alpha" "beta"])
EOF
cat >"$LS6_A/ws/alpha/build.tur" <<'EOF'
(defpackage alpha :name "alpha")
EOF
cat >"$LS6_A/ws/beta/build.tur" <<'EOF'
(defpackage beta :name "beta")
EOF
LS6_A_BEFORE=$(cat "$LS6_A/ws/beta/build.tur")
LS6_A_OUT=$(mktemp); LS6_A_ERR=$(mktemp)
( cd "$LS6_A/ws/beta" && "$LS6_ABS_TUR" add --workspace alpha ) \
    >"$LS6_A_OUT" 2>"$LS6_A_ERR"
rc=$?
LS6_A_AFTER=$(cat "$LS6_A/ws/beta/build.tur")
if [ "$rc" -eq 0 ] \
   && grep -qF 'workspace sibling' "$LS6_A_OUT" \
   && [ "$LS6_A_BEFORE" = "$LS6_A_AFTER" ]; then
    echo "PASS LS6-A: tur add --workspace <member> exits 0 without writing build.tur"
    PASS=$((PASS + 1))
else
    echo "FAIL LS6-A: tur add --workspace happy path"
    echo "  exit: $rc"
    echo "  stdout:"; sed 's/^/    /' "$LS6_A_OUT"
    echo "  stderr:"; sed 's/^/    /' "$LS6_A_ERR"
    [ "$LS6_A_BEFORE" = "$LS6_A_AFTER" ] || echo "  build.tur was MODIFIED"
    FAIL=$((FAIL + 1))
    FAILED+=("LS6-A: tur add --workspace happy path")
fi
rm -f "$LS6_A_OUT" "$LS6_A_ERR"
rm -rf "$LS6_A"

# LS6 case B: `tur add --workspace <unknown>` errors with a clear
# diagnostic naming the unknown member.
LS6_B=$(mktemp -d)
mkdir -p "$LS6_B/ws/alpha" "$LS6_B/ws/beta"
cat >"$LS6_B/ws/build.tur" <<'EOF'
(defpackage ws :name "ws" :members ["alpha" "beta"])
EOF
cat >"$LS6_B/ws/alpha/build.tur" <<'EOF'
(defpackage alpha :name "alpha")
EOF
cat >"$LS6_B/ws/beta/build.tur" <<'EOF'
(defpackage beta :name "beta")
EOF
LS6_B_ERR=$(mktemp)
( cd "$LS6_B/ws/beta" && "$LS6_ABS_TUR" add --workspace nope ) \
    >/dev/null 2>"$LS6_B_ERR"
rc=$?
if [ "$rc" -ne 0 ] && grep -qF "'nope' is not a member" "$LS6_B_ERR"; then
    echo "PASS LS6-B: tur add --workspace errors on non-member name"
    PASS=$((PASS + 1))
else
    echo "FAIL LS6-B: tur add --workspace should reject unknown member"
    echo "  exit: $rc (want non-zero)"
    echo "  stderr:"; sed 's/^/    /' "$LS6_B_ERR"
    FAIL=$((FAIL + 1))
    FAILED+=("LS6-B: tur add --workspace unknown member error")
fi
rm -f "$LS6_B_ERR"
rm -rf "$LS6_B"

# AUDIT1/2: `tur audit` lists every origin the build fetches code from.
# The security section of consuming-spices-guide.md promised this command; it
# did not exist (only `audit-spans`, an unrelated debugger mode).
AUD=$(mktemp -d)
mkdir -p "$AUD/proj" "$AUD/utils"
cat >"$AUD/utils/build.tur" <<'EOF'
(defpackage utils :name "utils")
EOF
cat >"$AUD/proj/build.tur" <<'EOF'
(defpackage demo
  :name "demo"
  :spices #{
    "geom"  #{:url "https://example.invalid/tur-geom" :ref "v0.2.1"}
    "utils" #{:path "../utils"}
  }
  :cmake-deps #{
    "raylib" #{:url "https://example.invalid/raylib" :ref "5.0"}
  })
EOF

# AUDIT1: with no tur.lock, every :url origin is reported UNPINNED. A :path dep
# is not -- it resolves from local source and has nothing to pin, so flagging it
# would train the reader to ignore the warning.
#
# The honesty line is asserted too, because it is the part most likely to drift
# away from what the command actually does. It used to read "it verifies
# nothing"; since C-3 (security-audit-plan) `tur audit` re-hashes every present,
# pinned tree, so with no lockfile it now says it has nothing to verify AGAINST
# -- and still says the hashes are not a signature, which remains true and is
# the claim that must never quietly disappear.
AUD_OUT=$(mktemp)
( cd "$AUD/proj" && "$LS6_ABS_TUR" audit ) >"$AUD_OUT" 2>&1
rc=$?
if [ "$rc" -eq 0 ] \
   && grep -qF 'example.invalid/tur-geom' "$AUD_OUT" \
   && grep -qF 'example.invalid/raylib' "$AUD_OUT" \
   && grep -qF 'local path' "$AUD_OUT" \
   && [ "$(grep -c 'NOT IN tur.lock' "$AUD_OUT")" -eq 2 ] \
   && grep -qF 'nothing to verify against' "$AUD_OUT" \
   && grep -qF 'not a signature' "$AUD_OUT"; then
    echo "PASS AUDIT1: audit lists origins and flags the unpinned ones"
    PASS=$((PASS + 1))
else
    echo "FAIL AUDIT1: audit output"
    echo "  exit: $rc"; sed 's/^/    /' "$AUD_OUT"
    FAIL=$((FAIL + 1))
    FAILED+=("AUDIT1: audit lists origins")
fi
rm -f "$AUD_OUT"

# AUDIT2: with a tur.lock present, each pinned origin reports its commit and
# hash and is no longer flagged. Written in pkg_lock_write's own format.
cat >"$AUD/proj/tur.lock" <<'EOF'
(deflockfile
  :format-version 1
  :spices #{
    "geom" #{:url "https://example.invalid/tur-geom" :ref "v0.2.1" :resolved "a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4" :sha256 "deadbeef00112233445566778899aabbccddeeff00112233445566778899aabb"}
  }
  :cmake-deps #{
    "raylib" #{:url "https://example.invalid/raylib" :ref "5.0" :resolved "0123456789abcdef0123456789abcdef01234567" :sha256 "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"}
  }
)
EOF
AUD_OUT=$(mktemp)
( cd "$AUD/proj" && "$LS6_ABS_TUR" audit ) >"$AUD_OUT" 2>&1
rc=$?
if [ "$rc" -eq 0 ] \
   && grep -qF 'commit a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4' "$AUD_OUT" \
   && grep -qF 'commit 0123456789abcdef0123456789abcdef01234567' "$AUD_OUT" \
   && grep -qF 'sha256 deadbeef' "$AUD_OUT" \
   && ! grep -qF 'NOT IN tur.lock' "$AUD_OUT"; then
    echo "PASS AUDIT2: audit reports the pin from tur.lock"
    PASS=$((PASS + 1))
else
    echo "FAIL AUDIT2: audit with a lock file"
    echo "  exit: $rc"; sed 's/^/    /' "$AUD_OUT"
    FAIL=$((FAIL + 1))
    FAILED+=("AUDIT2: audit reports the pin")
fi
rm -f "$AUD_OUT"
rm -rf "$AUD"

# MSG1: `tur add` on an already-declared dep must not point at a `tur update`
# subcommand -- there is no "update" row in CANONICAL_COMMANDS (`tur upgrade`
# is the installed-tool pipeline, a different thing), so the old text sent
# users to a command that does not exist.  Assert both that the dead name is
# gone and that the replacement names the flow that works.
MSG1=$(mktemp -d)
mkdir -p "$MSG1/proj" "$MSG1/geom"
cat >"$MSG1/geom/build.tur" <<'EOF'
(defpackage geom :name "geom")
EOF
cat >"$MSG1/proj/build.tur" <<'EOF'
(defpackage proj
  :name "proj"
  :spices #{
    "geom" #{:path "../geom"}
  })
EOF
MSG1_ERR=$(mktemp)
( cd "$MSG1/proj" && "$LS6_ABS_TUR" add ../geom --path ) >/dev/null 2>"$MSG1_ERR"
rc=$?
if [ "$rc" -ne 0 ] \
   && ! grep -qF 'tur update' "$MSG1_ERR" \
   && grep -qF 'already a dependency' "$MSG1_ERR" \
   && grep -qF 'tur fetch --update' "$MSG1_ERR"; then
    echo "PASS MSG1: duplicate-dep message points at a real command"
    PASS=$((PASS + 1))
else
    echo "FAIL MSG1: duplicate-dep message"
    echo "  exit: $rc (want non-zero)"
    echo "  stderr:"; sed 's/^/    /' "$MSG1_ERR"
    grep -qF 'tur update' "$MSG1_ERR" && echo "  still names the nonexistent \`tur update\`"
    FAIL=$((FAIL + 1))
    FAILED+=("MSG1: duplicate-dep message names a real command")
fi
rm -f "$MSG1_ERR"
rm -rf "$MSG1"

# MSG2: `tur add-cmake` must name the file `tur fetch` actually writes.
# It writes cmake/CMakeLists.txt; SpiceDeps is the name of the cmake PROJECT
# declared inside that file, which is what the old message reported as the
# filename.
MSG2=$(mktemp -d)
mkdir -p "$MSG2/proj"
cat >"$MSG2/proj/build.tur" <<'EOF'
(defpackage proj :name "proj")
EOF
MSG2_OUT=$(mktemp)
( cd "$MSG2/proj" && "$LS6_ABS_TUR" add-cmake https://example.invalid/cjson --ref v1.7.16 ) \
    >"$MSG2_OUT" 2>/dev/null
rc=$?
if [ "$rc" -eq 0 ] \
   && grep -qF 'cmake/CMakeLists.txt' "$MSG2_OUT" \
   && ! grep -qF 'SpiceDeps.cmake' "$MSG2_OUT"; then
    echo "PASS MSG2: add-cmake names the file fetch actually generates"
    PASS=$((PASS + 1))
else
    echo "FAIL MSG2: add-cmake generated-file message"
    echo "  exit: $rc"
    echo "  stdout:"; sed 's/^/    /' "$MSG2_OUT"
    FAIL=$((FAIL + 1))
    FAILED+=("MSG2: add-cmake names the generated file correctly")
fi
rm -f "$MSG2_OUT"
rm -rf "$MSG2"

# LS6 case C: `tur fetch --dry-run` classifies each dep and writes no
# lock file.  The bogus URL would error out on a real fetch; --dry-run
# proves it was never contacted.
LS6_C=$(mktemp -d)
mkdir -p "$LS6_C/ws/alpha" "$LS6_C/ws/beta" "$LS6_C/sib"
cat >"$LS6_C/ws/build.tur" <<'EOF'
(defpackage ws :name "ws" :members ["alpha" "beta"])
EOF
cat >"$LS6_C/ws/alpha/build.tur" <<'EOF'
(defpackage alpha :name "alpha")
EOF
cat >"$LS6_C/sib/build.tur" <<'EOF'
(defpackage sib :name "sib")
EOF
cat >"$LS6_C/ws/beta/build.tur" <<EOF
(defpackage beta
  :name "beta"
  :spices #{
    "alpha"  #{:url "https://example.invalid/alpha" :ref "v9.9.9"}
    "remote" #{:url "https://example.invalid/ls6c-$$" :ref "v0.0.0"}
    "sib"    #{:path "../../sib"}
  })
EOF
LS6_C_OUT=$(mktemp); LS6_C_ERR=$(mktemp)
( cd "$LS6_C/ws/beta" && "$LS6_ABS_TUR" fetch --dry-run ) \
    >"$LS6_C_OUT" 2>"$LS6_C_ERR"
rc=$?
LS6_C_OK=1
[ "$rc" -eq 0 ] || LS6_C_OK=0
[ -f "$LS6_C/ws/beta/tur.lock" ] && LS6_C_OK=0
grep -qF "workspace member" "$LS6_C_OUT" || LS6_C_OK=0
grep -qF ":path" "$LS6_C_OUT" || LS6_C_OK=0
grep -qF "fetch  URL" "$LS6_C_OUT" || LS6_C_OK=0
grep -qF "summary:" "$LS6_C_OUT" || LS6_C_OK=0
# critically: no actual fetch attempt (`spice: fetching ...`) and the
# bogus URL never errors out.
grep -qF "fetching '" "$LS6_C_ERR" && LS6_C_OK=0
if [ "$LS6_C_OK" -eq 1 ]; then
    echo "PASS LS6-C: tur fetch --dry-run classifies deps without fetching or writing tur.lock"
    PASS=$((PASS + 1))
else
    echo "FAIL LS6-C: tur fetch --dry-run behavior"
    echo "  exit: $rc"
    echo "  lock present: $([ -f "$LS6_C/ws/beta/tur.lock" ] && echo yes || echo no)"
    echo "  stdout:"; sed 's/^/    /' "$LS6_C_OUT"
    echo "  stderr:"; sed 's/^/    /' "$LS6_C_ERR"
    FAIL=$((FAIL + 1))
    FAILED+=("LS6-C: tur fetch --dry-run")
fi
rm -f "$LS6_C_OUT" "$LS6_C_ERR"
rm -rf "$LS6_C"

# SC4: works the same way when invoked from a deeply-nested unrelated
# cwd, since find_spice_root canonicalizes via realpath() and walks
# ancestors of the *file*, not the cwd.
ABS_TUR="$PWD/$TUR"
ABS_ENTRY="$PWD/$ENTRY"
NESTED=$(mktemp -d)
mkdir -p "$NESTED/a/b/c/d"
( cd "$NESTED/a/b/c/d" && "$ABS_TUR" check "$ABS_ENTRY" ) >/dev/null 2>&1
rc=$?
if [ "$rc" -eq 0 ]; then
    echo "PASS SC4: auto-discovery works from a deeply-nested unrelated cwd"
    PASS=$((PASS + 1))
else
    echo "FAIL SC4: auto-discovery from deeply-nested cwd -- expected exit 0, got $rc"
    FAIL=$((FAIL + 1))
    FAILED+=("SC4: auto-discovery from nested cwd")
fi
rm -rf "$NESTED"

# SN3: regression guard against the launcher's cwd-relative stdlib path.
# Before SN1, running `tur check` from any cwd other than the repo root
# produced ~18 lines of `tur: cannot open 'stdlib/X.tur'` stderr noise
# on every invocation, even on success. After SN1+SN2 those should be
# silent. Running from `/tmp` (a directory with no stdlib/ subtree)
# exercises the exe-relative resolver and reaffirms the fix.
SN3_TMP=$(mktemp -d)
SN3_TUR_ABS="$PWD/$TUR"
SN3_ENTRY_ABS="$PWD/$ENTRY"
SN3_STDERR=$(mktemp)
( cd "$SN3_TMP" && "$SN3_TUR_ABS" check "$SN3_ENTRY_ABS" ) >/dev/null 2>"$SN3_STDERR"
rc=$?
SN3_LINES=$(wc -l <"$SN3_STDERR" | tr -d ' ')
if [ "$rc" -eq 0 ] && [ "$SN3_LINES" -eq 0 ]; then
    echo "PASS SN3: tur check from non-repo cwd has empty stderr on success"
    PASS=$((PASS + 1))
else
    echo "FAIL SN3: tur check from non-repo cwd"
    echo "  cwd:    $SN3_TMP"
    echo "  exit:   $rc (want 0)"
    echo "  stderr lines: $SN3_LINES (want 0)"
    echo "  stderr content:"
    sed 's/^/    /' "$SN3_STDERR"
    FAIL=$((FAIL + 1))
    FAILED+=("SN3: empty stderr on success from non-repo cwd")
fi
rm -rf "$SN3_TMP" "$SN3_STDERR"

# RM4: `tur run` on a file inside a spice picks up the manifest's
# `:reader-macros [...]` entry and preloads the named files into the
# reader-macro registry, so the source can use `#sum[...]` / `#pi`
# without a per-file `#use-reader-macros` directive.
RM_FIXTURE="tests/fixtures/reader-macros-manifest"
RM_ENTRY="$RM_FIXTURE/src/main.tur"
RM_OUT=$(mktemp)
"$TUR" run "$RM_ENTRY" >"$RM_OUT" 2>/dev/null
rm_rc=$?
RM_EXPECTED="15
3.14159
6.28318"
if [ "$rm_rc" -eq 0 ] && [ "$(cat "$RM_OUT")" = "$RM_EXPECTED" ]; then
    echo "PASS RM4: tur run preloads :reader-macros from build.tur"
    PASS=$((PASS + 1))
else
    echo "FAIL RM4: tur run :reader-macros -- exit $rm_rc"
    echo "  expected:"; printf '    %s\n' "$RM_EXPECTED"
    echo "  got:"; sed 's/^/    /' "$RM_OUT"
    FAIL=$((FAIL + 1))
    FAILED+=("RM4: tur run preloads :reader-macros")
fi
rm -f "$RM_OUT"

# RM4: same fixture via `tur build` should also pick up the manifest's
# reader-macros (top-level `tur build` dispatch walks up to find build.tur).
RM_BIN=$(mktemp)
"$TUR" build "$RM_ENTRY" -o "$RM_BIN" 2>/dev/null
rm_build_rc=$?
if [ "$rm_build_rc" -eq 0 ] && [ -x "$RM_BIN" ]; then
    RM_BIN_OUT=$("$RM_BIN")
    if [ "$RM_BIN_OUT" = "$RM_EXPECTED" ]; then
        echo "PASS RM4: tur build preloads :reader-macros from build.tur"
        PASS=$((PASS + 1))
    else
        echo "FAIL RM4: tur build :reader-macros -- output mismatch"
        echo "  expected:"; printf '    %s\n' "$RM_EXPECTED"
        echo "  got:"; printf '    %s\n' "$RM_BIN_OUT"
        FAIL=$((FAIL + 1))
        FAILED+=("RM4: tur build preloads :reader-macros")
    fi
else
    echo "FAIL RM4: tur build :reader-macros -- exit $rm_build_rc"
    FAIL=$((FAIL + 1))
    FAILED+=("RM4: tur build preloads :reader-macros")
fi
rm -f "$RM_BIN"

# RM4 follow-up: every other compile entry point should also pick up the
# manifest's :reader-macros via discover_manifest_reader_macros. Each
# command below would fail to parse src/main.tur (which uses #sum / #pi)
# without the preload.
assert_exit 0 "RM4: tur check picks up :reader-macros from build.tur" \
    "$TUR" check "$RM_ENTRY"

assert_exit 0 "RM4: tur emit-c picks up :reader-macros from build.tur" \
    "$TUR" emit-c "$RM_ENTRY"

assert_exit 0 "RM4: tur emit-h picks up :reader-macros from build.tur" \
    "$TUR" emit-h "$RM_ENTRY"

# RM4: tur format pretty-prints a file that uses manifest-declared macros.
# Without manifest preload the reader would fail with "unexpected character '#'".
assert_exit 0 "RM4: tur format picks up :reader-macros from build.tur" \
    "$TUR" format "$RM_ENTRY"

# RM4: tur emit-c --output-dir <dir> exercises the multi-file emit path
# (cmd_emit_c_to_dir + compile_to_h + compile_to_implementation).
RM_EMIT_DIR=$(mktemp -d)
assert_exit 0 "RM4: tur emit-c --output-dir picks up :reader-macros" \
    "$TUR" emit-c --output-dir "$RM_EMIT_DIR" "$RM_ENTRY"
rm -rf "$RM_EMIT_DIR"

# Transitive-RM T1: a spice with `:reader-macros` in build.tur AND an
# imported module that *uses* those macros. Without the T1 wiring
# (elab_module.c reads the imported file via read_all_with_registry
# instead of bare read_all), the import would fail with the generic
# "unexpected character '#'" error.
RMT_ENTRY="tests/fixtures/reader-macros-transitive/src/main.tur"
RMT_EXPECTED="15
6.28318
60"
RMT_OUT=$(mktemp)
"$TUR" run "$RMT_ENTRY" >"$RMT_OUT" 2>/dev/null
rmt_rc=$?
if [ "$rmt_rc" -eq 0 ] && [ "$(cat "$RMT_OUT")" = "$RMT_EXPECTED" ]; then
    echo "PASS T1: imported module sees spice :reader-macros"
    PASS=$((PASS + 1))
else
    echo "FAIL T1: imported module sees spice :reader-macros -- exit $rmt_rc"
    echo "  expected:"; printf '    %s\n' "$RMT_EXPECTED"
    echo "  got:"; sed 's/^/    /' "$RMT_OUT"
    FAIL=$((FAIL + 1))
    FAILED+=("T1: imported module sees spice :reader-macros")
fi
rm -f "$RMT_OUT"

# Transitive-RM T3: cross-module define visibility. A module that only
# registers reader macros (lib/syntax) can be imported by a sibling
# (lib/user) that uses those macros, as long as the entry file lists
# the syntax import first so it's loaded before the user import is read.
RMX_ENTRY="tests/fixtures/reader-macros-cross-module/src/main.tur"
RMX_OUT=$(mktemp)
"$TUR" run "$RMX_ENTRY" >"$RMX_OUT" 2>/dev/null
rmx_rc=$?
if [ "$rmx_rc" -eq 0 ] && [ "$(cat "$RMX_OUT")" = "15" ]; then
    echo "PASS T3: cross-module define visibility (sibling imports in order)"
    PASS=$((PASS + 1))
else
    echo "FAIL T3: cross-module define visibility -- exit $rmx_rc"
    echo "  expected: 15"
    echo "  got:"; sed 's/^/    /' "$RMX_OUT"
    FAIL=$((FAIL + 1))
    FAILED+=("T3: cross-module define visibility")
fi
rm -f "$RMX_OUT"

# Transitive-RM T1 (negative): a parse error inside an imported module
# should surface BOTH the original error at the import target AND a
# `while loading module 'X'` note at the import site so the user can
# trace it back.
RMC_ENTRY="tests/fixtures/errors/reader-macros-import-chain/src/main.tur"
RMC_STDERR=$(mktemp)
"$TUR" run "$RMC_ENTRY" >/dev/null 2>"$RMC_STDERR"
rmc_rc=$?
if [ "$rmc_rc" -ne 0 ] && \
   grep -qF "unknown data-literal dispatch tag '#bogus{...}'" "$RMC_STDERR" && \
   grep -qF "while loading module 'lib/syntax'" "$RMC_STDERR"; then
    echo "PASS T1: import-chain note attached to reader error in imported module"
    PASS=$((PASS + 1))
else
    echo "FAIL T1: import-chain note missing or unexpected exit"
    echo "  exit: $rmc_rc (want non-zero)"
    echo "  stderr:"; sed 's/^/    /' "$RMC_STDERR"
    FAIL=$((FAIL + 1))
    FAILED+=("T1: import-chain note attached to reader error in imported module")
fi
rm -f "$RMC_STDERR"

# ------------------------------------------------------------------
# SF (tur-fetch-system-first-plan): :prefer-system cmake-deps
# ------------------------------------------------------------------
TUR_ABS=$(cd "$(dirname "$TUR")" && pwd)/$(basename "$TUR")

# SF0: :prefer-system true without :cmake-name is a hard manifest error.
SF0=$(mktemp -d)
cat >"$SF0/build.tur" <<'EOF'
(defpackage sf0
  :name "sf0"
  :cmake-deps #{
    "bad" #{:prefer-system true :url "https://example.com/x" :ref "v1"}
  })
EOF
SF0_ERR=$(mktemp)
( cd "$SF0" && "$TUR_ABS" fetch ) >/dev/null 2>"$SF0_ERR"
sf0_rc=$?
if [ "$sf0_rc" -ne 0 ] \
   && grep -qF ':prefer-system true requires :cmake-name' "$SF0_ERR" \
   && [ ! -d "$SF0/cmake" ]; then
    echo "PASS SF0: :prefer-system without :cmake-name errors before codegen"
    PASS=$((PASS + 1))
else
    echo "FAIL SF0: missing :cmake-name not rejected (exit $sf0_rc)"
    echo "  stderr:"; sed 's/^/    /' "$SF0_ERR"
    FAIL=$((FAIL + 1))
    FAILED+=("SF0: :prefer-system without :cmake-name")
fi
rm -f "$SF0_ERR"; rm -rf "$SF0"

# SF1/SF2: with a system copy available (fake find_package config), the dep
# resolves via the system path -- no fetch, generated CMakeLists carries the
# find_package fallback, and resolved_via lands in both the JSON manifest and
# tur.lock.  Fully hermetic: no network because find_package short-circuits.
SF1=$(mktemp -d)
mkdir -p "$SF1/fk/lib/cmake/FakeDep" "$SF1/fk/include"
: >"$SF1/fk/lib/libfakedep.a"
cat >"$SF1/fk/lib/cmake/FakeDep/FakeDepConfig.cmake" <<'EOF'
add_library(FakeDep::fakedep STATIC IMPORTED)
set_target_properties(FakeDep::fakedep PROPERTIES
  IMPORTED_LOCATION "${CMAKE_CURRENT_LIST_DIR}/../../libfakedep.a"
  INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/../../../include")
set(FakeDep_FOUND TRUE)
EOF
cat >"$SF1/build.tur" <<'EOF'
(defpackage sf1
  :name "sf1"
  :cmake-deps #{
    "fakedep" #{
      :prefer-system true
      :cmake-name    "FakeDep"
      :targets       ["FakeDep::fakedep"]
      :url           "https://example.invalid/never-fetched"
      :ref           "v1.0"
    }
  })
EOF
SF1_ERR=$(mktemp)
( cd "$SF1" && CMAKE_PREFIX_PATH="$SF1/fk" "$TUR_ABS" fetch ) \
    >/dev/null 2>"$SF1_ERR"
sf1_rc=$?
if [ "$sf1_rc" -eq 0 ] \
   && grep -qF 'find_package(FakeDep QUIET)' "$SF1/cmake/CMakeLists.txt" \
   && grep -qF 'if (NOT FakeDep_FOUND)' "$SF1/cmake/CMakeLists.txt" \
   && grep -qF '"resolved_via": "system"' "$SF1/cmake/spice-deps-manifest.json" \
   && grep -qF ':resolved-via "system"' "$SF1/tur.lock"; then
    echo "PASS SF1/SF2: :prefer-system resolves via system find_package"
    PASS=$((PASS + 1))
else
    echo "FAIL SF1/SF2: system-first resolution did not take the system path"
    echo "  exit: $sf1_rc"
    echo "  stderr:"; sed 's/^/    /' "$SF1_ERR"
    [ -f "$SF1/cmake/spice-deps-manifest.json" ] && \
      { echo "  manifest:"; sed 's/^/    /' "$SF1/cmake/spice-deps-manifest.json"; }
    [ -f "$SF1/tur.lock" ] && \
      { echo "  tur.lock:"; sed 's/^/    /' "$SF1/tur.lock"; }
    FAIL=$((FAIL + 1))
    FAILED+=("SF1/SF2: :prefer-system system path")
fi

# SF3: --refetch forces the source build even when the system copy exists.
SF3_ERR=$(mktemp)
rm -rf "$SF1/cmake" "$SF1/tur.lock"
( cd "$SF1" && CMAKE_PREFIX_PATH="$SF1/fk" "$TUR_ABS" fetch --refetch ) \
    >/dev/null 2>"$SF3_ERR"
# A forced fetch hits the bogus :url, so the build is expected to fail; what
# matters is that the system short-circuit was bypassed (find_package guarded).
if grep -qF 'if (NOT TUR_FETCH_FORCE_FETCH)' "$SF1/cmake/CMakeLists.txt"; then
    echo "PASS SF3: --refetch guards find_package behind TUR_FETCH_FORCE_FETCH"
    PASS=$((PASS + 1))
else
    echo "FAIL SF3: --refetch did not bypass the system path"
    echo "  stderr:"; sed 's/^/    /' "$SF3_ERR"
    FAIL=$((FAIL + 1))
    FAILED+=("SF3: --refetch bypass")
fi
rm -f "$SF1_ERR" "$SF3_ERR"; rm -rf "$SF1"

# SF2b: a system package whose INTERFACE_INCLUDE_DIRECTORIES holds multiple
# dirs (a CMake ;-list) must expand into separate JSON array elements, not a
# single "d1;d2" string (which would produce a bogus -Id1;d2 flag).
SF2B=$(mktemp -d)
mkdir -p "$SF2B/fk/lib/cmake/MultiDep" "$SF2B/fk/include/a" "$SF2B/fk/include/b"
: >"$SF2B/fk/lib/libmultidep.a"
cat >"$SF2B/fk/lib/cmake/MultiDep/MultiDepConfig.cmake" <<'EOF'
add_library(MultiDep::multidep STATIC IMPORTED)
set_target_properties(MultiDep::multidep PROPERTIES
  IMPORTED_LOCATION "${CMAKE_CURRENT_LIST_DIR}/../../libmultidep.a"
  INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/../../../include/a;${CMAKE_CURRENT_LIST_DIR}/../../../include/b")
set(MultiDep_FOUND TRUE)
EOF
cat >"$SF2B/build.tur" <<'EOF'
(defpackage sf2b
  :name "sf2b"
  :cmake-deps #{
    "multidep" #{:prefer-system true :cmake-name "MultiDep"
                 :targets ["MultiDep::multidep"]
                 :url "https://example.invalid/x" :ref "v1"}
  })
EOF
SF2B_ERR=$(mktemp)
( cd "$SF2B" && CMAKE_PREFIX_PATH="$SF2B/fk" "$TUR_ABS" fetch ) \
    >/dev/null 2>"$SF2B_ERR"
sf2b_rc=$?
# Expect two distinct quoted include_dirs entries, ending in /include/a and /b.
if [ "$sf2b_rc" -eq 0 ] \
   && grep -qF 'include/a", "' "$SF2B/cmake/spice-deps-manifest.json" \
   && grep -qF 'include/b"]' "$SF2B/cmake/spice-deps-manifest.json"; then
    echo "PASS SF2b: multi-dir system include path expands to JSON array"
    PASS=$((PASS + 1))
else
    echo "FAIL SF2b: multi-dir include path not expanded"
    echo "  exit: $sf2b_rc"
    [ -f "$SF2B/cmake/spice-deps-manifest.json" ] && \
      { echo "  manifest:"; sed 's/^/    /' "$SF2B/cmake/spice-deps-manifest.json"; }
    echo "  stderr:"; sed 's/^/    /' "$SF2B_ERR"
    FAIL=$((FAIL + 1))
    FAILED+=("SF2b: multi-dir include expansion")
fi
rm -f "$SF2B_ERR"; rm -rf "$SF2B"

# SF4: :options reach find_package.  A find module reads its hints
# (PostgreSQL_ROOT, OPENSSL_ROOT_DIR) from the cache, and the generator used to
# write a :prefer-system dep's options only inside the fetch fallback -- after
# the search they were meant to steer.  Here the system copy sits at a prefix
# nothing else searches, so only the :options hint can find it; when it does
# not, the fallback hits the bogus :url and the fetch fails.
SF4=$(mktemp -d)
mkdir -p "$SF4/fk/lib/cmake/HintDep" "$SF4/fk/include"
: >"$SF4/fk/lib/libhintdep.a"
cat >"$SF4/fk/lib/cmake/HintDep/HintDepConfig.cmake" <<'EOF'
add_library(HintDep::hintdep STATIC IMPORTED)
set_target_properties(HintDep::hintdep PROPERTIES
  IMPORTED_LOCATION "${CMAKE_CURRENT_LIST_DIR}/../../libhintdep.a"
  INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/../../../include")
set(HintDep_FOUND TRUE)
EOF
cat >"$SF4/build.tur" <<EOF
(defpackage sf4
  :name "sf4"
  :cmake-deps #map{
    "hintdep" #map{:prefer-system true :cmake-name "HintDep"
                   :targets ["HintDep::hintdep"]
                   :url "https://example.invalid/never-fetched" :ref "v1"
                   :options #map{:HintDep_ROOT "$SF4/fk"}}
  })
EOF
SF4_ERR=$(mktemp)
( cd "$SF4" && "$TUR_ABS" fetch ) >/dev/null 2>"$SF4_ERR"
sf4_rc=$?
if [ "$sf4_rc" -eq 0 ] \
   && grep -qF '"resolved_via": "system"' "$SF4/cmake/spice-deps-manifest.json"; then
    echo "PASS SF4: :prefer-system :options reach find_package as hints"
    PASS=$((PASS + 1))
else
    echo "FAIL SF4: :options hint did not reach find_package"
    echo "  exit: $sf4_rc"
    echo "  stderr:"; sed 's/^/    /' "$SF4_ERR"
    FAIL=$((FAIL + 1))
    FAILED+=("SF4: :prefer-system :options as find hints")
fi
rm -f "$SF4_ERR"; rm -rf "$SF4"

# SF5: a :prefer-system dep with no :url is system-only (libpq: there is no
# repo to build it from).  Not found, the configure must stop on a message
# naming the dep and the package -- an empty FetchContent_Declare used to stand
# there, failing on "No download info given for 'x-populate'".  Found, it
# resolves via the system even under --refetch, which has nothing to fetch.
SF5=$(mktemp -d)
mkdir -p "$SF5/fk/lib/cmake/SysOnly" "$SF5/fk/include"
: >"$SF5/fk/lib/libsysonly.a"
cat >"$SF5/fk/lib/cmake/SysOnly/SysOnlyConfig.cmake" <<'EOF'
add_library(SysOnly::sysonly STATIC IMPORTED)
set_target_properties(SysOnly::sysonly PROPERTIES
  IMPORTED_LOCATION "${CMAKE_CURRENT_LIST_DIR}/../../libsysonly.a"
  INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/../../../include")
set(SysOnly_FOUND TRUE)
EOF
cat >"$SF5/build.tur" <<'EOF'
(defpackage sf5
  :name "sf5"
  :cmake-deps #map{
    "sysonly" #map{:prefer-system true :cmake-name "SysOnly"
                   :targets ["SysOnly::sysonly"]}
  })
EOF
SF5_ERR=$(mktemp)
( cd "$SF5" && "$TUR_ABS" fetch ) >"$SF5_ERR" 2>&1
sf5_missing_rc=$?
rm -rf "$SF5/cmake" "$SF5/tur.lock"
( cd "$SF5" && CMAKE_PREFIX_PATH="$SF5/fk" "$TUR_ABS" fetch --refetch ) \
    >>"$SF5_ERR" 2>&1
sf5_found_rc=$?
# Nothing is downloaded, so there is nothing to pin: `tur audit` must not
# report it as unpinned forever.
SF5_AUD=$( cd "$SF5" && "$TUR_ABS" audit 2>&1 )
if [ "$sf5_missing_rc" -ne 0 ] \
   && grep -qF "cmake-dep 'sysonly': find_package(SysOnly) found no system copy" "$SF5_ERR" \
   && ! grep -qF 'No download info' "$SF5_ERR" \
   && [ "$sf5_found_rc" -eq 0 ] \
   && grep -qF '"resolved_via": "system"' "$SF5/cmake/spice-deps-manifest.json" \
   && printf '%s\n' "$SF5_AUD" | grep -qF 'system only  find_package(SysOnly)' \
   && ! printf '%s\n' "$SF5_AUD" | grep -qF 'NOT IN tur.lock'; then
    echo "PASS SF5: a :prefer-system dep with no :url is system-only"
    PASS=$((PASS + 1))
else
    echo "FAIL SF5: system-only :prefer-system dep mishandled"
    echo "  exit (not installed): $sf5_missing_rc  exit (--refetch, installed): $sf5_found_rc"
    echo "  output:"; sed 's/^/    /' "$SF5_ERR"
    echo "  audit:"; printf '%s\n' "$SF5_AUD" | sed 's/^/    /'
    FAIL=$((FAIL + 1))
    FAILED+=("SF5: system-only :prefer-system dep")
fi
rm -f "$SF5_ERR"; rm -rf "$SF5"

# docs/archive/spice-guides-bare-brace-manifest-syntax.md follow-up: `tur add`
# spliced the new entry into the existing :spices map by reading
# `old_map->tag == F_MAP` only.  `#map{...}` reads as F_MAP_LITERAL, so the
# splice saw an empty map and wrote back a :spices holding ONLY the new dep --
# every previously declared dependency silently deleted.  `#map{...}` is the
# canonical spelling the guides use, so this was the common path, not an
# exotic one.  Both spellings must survive an add, and the manifest's own
# spelling must be preserved rather than respelled underneath the user.
for add_case in 'maplit|#map' 'legacy|#'; do
    add_name=${add_case%%|*}
    add_pre=${add_case#*|}
    ADDDIR=$(mktemp -d)
    mkdir -p "$ADDDIR/src"
    cat >"$ADDDIR/build.tur" <<EOF
(defpackage addcase
  :name    "addcase"
  :version "0.1.0"
  :spices ${add_pre}{"geom" ${add_pre}{:url "https://example.invalid/geom" :ref "v1"}})
EOF
    echo '(defn main [] : int 0)' >"$ADDDIR/src/main.tur"
    ADD_ERR=$(mktemp)
    ( cd "$ADDDIR" && "$LS6_ABS_TUR" add https://example.invalid/math --ref v2 ) \
        >/dev/null 2>"$ADD_ERR"
    add_rc=$?
    if [ "$add_rc" -eq 0 ] \
       && grep -qF '"geom"' "$ADDDIR/build.tur" \
       && grep -qF '"math"' "$ADDDIR/build.tur" \
       && grep -qF ":spices ${add_pre}{" "$ADDDIR/build.tur"; then
        echo "PASS tur-add-preserves-existing-spices-$add_name"
        PASS=$((PASS + 1))
    else
        echo "FAIL tur-add-preserves-existing-spices-$add_name"
        echo "  exit: $add_rc"
        echo "  build.tur:"; sed 's/^/    /' "$ADDDIR/build.tur"
        echo "  stderr:"; sed 's/^/    /' "$ADD_ERR"
        FAIL=$((FAIL + 1))
        FAILED+=("tur-add-preserves-existing-spices-$add_name")
    fi
    rm -f "$ADD_ERR"; rm -rf "$ADDDIR"
done

# ---------------------------------------------------------------------------
# A BROKEN build.tur must fail the command, and must not degrade into a
# cascade of `module not found`.
# docs/archive/manifest-read-failure-degrades-to-module-not-found.md
#
# pkg_manifest_read returned a bare `false` for two unrelated situations --
# "no manifest here" (normal) and "there IS a manifest and it is broken"
# (fatal) -- so every caller took the benign branch.  The spice root's src/
# then never joined the module search path and every intra-spice import failed
# with `module not found`, naming the import rather than the manifest.  In the
# field that hid 41 unreadable manifests for four weeks: one manifest typo
# presented as 66 unrelated import failures, and `tur check` printed the
# manifest error at `error:` severity while exiting 0.
MF_DIR=$(mktemp -d)
mkdir -p "$MF_DIR/src/demo" "$MF_DIR/tests"
# `:spices #fx{...}` -- an effect-row literal where the reader wants a map.
# This is the exact spelling that caused the field incident (TUR-E0620).
cat >"$MF_DIR/build.tur" <<'EOF'
(defpackage demo
  :name    "demo"
  :version "0.1.0"
  :spices #fx{ "test" #map{:url "https://example.invalid/test"} }
  :exports #map{ "demo/lib" ["answer"] })
EOF
cat >"$MF_DIR/src/demo/lib.tur" <<'EOF'
(defmodule demo/lib
  (export answer)
  (defn answer [] : int 42))
EOF
cat >"$MF_DIR/tests/use_test.tur" <<'EOF'
(defmodule use_it
  (export)
  (import demo/lib :refer [answer])
  (defn main [] : int (println (answer)) 0))
EOF

# An `error:` diagnostic must never coexist with exit 0.  This exited 0 before,
# so any CI step shelling out to `tur check` and trusting $? saw a clean
# manifest.
MF_ERR=$(mktemp)
( cd "$MF_DIR" && "$LS6_ABS_TUR" check tests/use_test.tur ) >/dev/null 2>"$MF_ERR"
mf_rc=$?
if [ "$mf_rc" -ne 0 ]; then
    echo "PASS malformed-manifest-fails-tur-check"
    PASS=$((PASS + 1))
else
    echo "FAIL malformed-manifest-fails-tur-check -- printed error: and exited 0"
    echo "  stderr:"; sed 's/^/    /' "$MF_ERR"
    FAIL=$((FAIL + 1))
    FAILED+=("malformed-manifest-fails-tur-check")
fi

# The failure must name the MANIFEST, and the offending slot.
for mf_case in 'TUR-E0624|malformed-manifest-names-the-manifest' \
               ':spices is malformed|malformed-manifest-names-the-slot'; do
    mf_needle=${mf_case%%|*}
    mf_label=${mf_case#*|}
    if grep -F -q "$mf_needle" "$MF_ERR"; then
        echo "PASS $mf_label"
        PASS=$((PASS + 1))
    else
        echo "FAIL $mf_label -- stderr missing: $mf_needle"
        echo "  stderr:"; sed 's/^/    /' "$MF_ERR"
        FAIL=$((FAIL + 1))
        FAILED+=("$mf_label")
    fi
done

# And it must NOT degrade into the misleading cascade.  The `-I src` hint is
# the specific trap: it does make the import resolve, so a reader who follows
# it concludes the IMPORT was mis-specified and never revisits the manifest.
for mf_anti in 'not found|malformed-manifest-no-module-not-found-cascade' \
               'try `tur check -I src|malformed-manifest-suppresses-I-src-hint'; do
    mf_needle=${mf_anti%%|*}
    mf_label=${mf_anti#*|}
    if grep -F -q "$mf_needle" "$MF_ERR"; then
        echo "FAIL $mf_label -- stderr still contains: $mf_needle"
        echo "  stderr:"; sed 's/^/    /' "$MF_ERR"
        FAIL=$((FAIL + 1))
        FAILED+=("$mf_label")
    else
        echo "PASS $mf_label"
        PASS=$((PASS + 1))
    fi
done

# Control: the SAME tree with `#fx{` corrected to `#map{` must pass clean.
# Nothing else changes, which is what makes the manifest the sole variable.
sed 's/#fx{ "test"/#map{ "test"/' "$MF_DIR/build.tur" >"$MF_DIR/build.tur.new"
mv "$MF_DIR/build.tur.new" "$MF_DIR/build.tur"
MF_OK_ERR=$(mktemp)
( cd "$MF_DIR" && "$LS6_ABS_TUR" check tests/use_test.tur ) >/dev/null 2>"$MF_OK_ERR"
mf_ok_rc=$?
if [ "$mf_ok_rc" -eq 0 ]; then
    echo "PASS well-formed-manifest-still-checks-clean"
    PASS=$((PASS + 1))
else
    echo "FAIL well-formed-manifest-still-checks-clean -- exit $mf_ok_rc"
    echo "  stderr:"; sed 's/^/    /' "$MF_OK_ERR"
    FAIL=$((FAIL + 1))
    FAILED+=("well-formed-manifest-still-checks-clean")
fi

# An ABSENT manifest is NOT malformed: the walk-up probe misses on most
# directories, and that must stay quiet.  This is the distinction the old
# single `false` collapsed, so it is the half most at risk from the fix.
MF_NONE=$(mktemp -d)
cat >"$MF_NONE/solo.tur" <<'EOF'
(defn main [] : int 0)
EOF
MF_NONE_ERR=$(mktemp)
( cd "$MF_NONE" && "$LS6_ABS_TUR" check solo.tur ) >/dev/null 2>"$MF_NONE_ERR"
mf_none_rc=$?
if [ "$mf_none_rc" -eq 0 ] && ! grep -F -q "TUR-E0624" "$MF_NONE_ERR"; then
    echo "PASS absent-manifest-is-not-malformed"
    PASS=$((PASS + 1))
else
    echo "FAIL absent-manifest-is-not-malformed -- exit $mf_none_rc"
    echo "  stderr:"; sed 's/^/    /' "$MF_NONE_ERR"
    FAIL=$((FAIL + 1))
    FAILED+=("absent-manifest-is-not-malformed")
fi

rm -f "$MF_ERR" "$MF_OK_ERR" "$MF_NONE_ERR"
rm -rf "$MF_DIR" "$MF_NONE"

# ---------------------------------------------------------------------------
# `tur build src/` and `tur check src/` must handle a NESTED module layout.
# docs/archive/tur-build-nested-src-dir-finds-no-files.md
#
# Both used a FLAT readdir, so a spice whose modules live one level down --
# `src/demo/lib.tur`, the layout `:exports "demo/lib"` implies -- reported
# `tur: no .tur files found in 'src/'`.  That is the exact invocation the
# `module not found` diagnostic recommends, so the advertised recovery from one
# confusing error produced a second one.  Project mode (`tur build .`) already
# recursed, so the two spellings of "build this spice" disagreed with nothing in
# the output to say which you got.
#
# Finding the files is only half of it: once the walk is recursive the sibling
# that does `(import demo/lib)` has to resolve it, so the build dir goes on the
# include path too.  The `main.tur` below imports across the nesting for that
# reason -- a case with no cross-file import would pass on a half fix.
NEST=$(mktemp -d)
mkdir -p "$NEST/src/demo" "$NEST/src/app"
cat >"$NEST/build.tur" <<'EOF'
(defpackage nested
  :name    "nested"
  :version "0.1.0"
  :exports #map{ "demo/lib" ["answer"] })
EOF
cat >"$NEST/src/demo/lib.tur" <<'EOF'
(defmodule demo/lib
  (export answer)
  (defn answer [] : int 42))
EOF
cat >"$NEST/src/app/main.tur" <<'EOF'
(defmodule app/main
  (export)
  (import demo/lib :refer [answer])
  (defn main [] : int (println (answer)) 0))
EOF

NEST_ERR=$(mktemp)
( cd "$NEST" && "$LS6_ABS_TUR" check src/ ) >/dev/null 2>"$NEST_ERR"
nest_check_rc=$?
if [ "$nest_check_rc" -eq 0 ]; then
    echo "PASS nested-src-tur-check-finds-files"
    PASS=$((PASS + 1))
else
    echo "FAIL nested-src-tur-check-finds-files -- exit $nest_check_rc"
    echo "  stderr:"; sed 's/^/    /' "$NEST_ERR"
    FAIL=$((FAIL + 1))
    FAILED+=("nested-src-tur-check-finds-files")
fi

# The message that used to come out; assert it is gone rather than only
# assert on the exit code, so a future regression names itself.
if grep -qF "no .tur files found" "$NEST_ERR"; then
    echo "FAIL nested-src-no-empty-dir-message"
    echo "  stderr:"; sed 's/^/    /' "$NEST_ERR"
    FAIL=$((FAIL + 1))
    FAILED+=("nested-src-no-empty-dir-message")
else
    echo "PASS nested-src-no-empty-dir-message"
    PASS=$((PASS + 1))
fi

NEST_BIN="$NEST/out"
NEST_BERR=$(mktemp)
( cd "$NEST" && "$LS6_ABS_TUR" build src/ -o "$NEST_BIN" ) >/dev/null 2>"$NEST_BERR"
nest_build_rc=$?
nest_out=""
[ -x "$NEST_BIN" ] && nest_out="$("$NEST_BIN" 2>/dev/null)"
if [ "$nest_build_rc" -eq 0 ] && [ "$nest_out" = "42" ]; then
    echo "PASS nested-src-tur-build-resolves-intra-spice-import"
    PASS=$((PASS + 1))
else
    echo "FAIL nested-src-tur-build-resolves-intra-spice-import"
    echo "  exit: $nest_build_rc  output: '$nest_out' (want 42)"
    echo "  stderr:"; sed 's/^/    /' "$NEST_BERR"
    FAIL=$((FAIL + 1))
    FAILED+=("nested-src-tur-build-resolves-intra-spice-import")
fi

rm -f "$NEST_ERR" "$NEST_BERR"; rm -rf "$NEST"

echo
echo "summary: $PASS passed, $FAIL failed"
if [ "$FAIL" -ne 0 ]; then
    echo "failed cases:"
    for f in "${FAILED[@]}"; do echo "  - $f"; done
    exit 1
fi
exit 0
