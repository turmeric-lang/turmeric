#!/usr/bin/env bash
# tests/run-script-deps.sh -- a single-file script's `(defscript ...)` header,
# end to end (docs/upcoming/script-spice-deps-plan.md): the header's grammar
# and errors (SD0), the script-dep cache (SD1), the hermetic search path and
# link inputs (SD2), `tur <script> args` (SD3), `tur add --script` and the
# <script>.lock sidecar (SD4), and `tur cache` (SD5).
#
# Isolated under a temporary TUR_HOME; every fetched dep is a local bare repo
# reached through a `file://` URL, so nothing touches the network.

set -u
cd "$(dirname "$0")/.."

TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || { echo "tests: $TUR not built; run 'tur run build' first" >&2; exit 2; }
if ! command -v git >/dev/null 2>&1; then
    if [ "${TUR_REQUIRE_GIT:-0}" = 1 ]; then
        echo "FAIL run-script-deps -- git not found, and TUR_REQUIRE_GIT=1" >&2
        exit 1
    fi
    echo "tests: git not found; skipping (set TUR_REQUIRE_GIT=1 to make this fatal)" >&2
    exit 0
fi

case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) IS_WINDOWS=1; native() { cygpath -m "$1"; } ;;
  *)                    IS_WINDOWS=0; native() { echo "$1"; } ;;
esac

PASS=0
FAIL=0
FAILED=()
note() { printf "  %s\n" "$*"; }
ok()   { PASS=$((PASS+1)); echo "PASS $1"; }
bad()  { FAIL=$((FAIL+1)); FAILED+=("$1"); echo "FAIL $1"; [ -n "${2:-}" ] && note "$2"; }
# expect NAME WANT GOT -- exact match
expect() { if [ "$3" = "$2" ]; then ok "$1"; else bad "$1" "want [$2] got [$3]"; fi; }
# expect_grep NAME PATTERN TEXT
expect_grep() { if printf '%s' "$3" | grep -q -- "$2"; then ok "$1"; else bad "$1" "no /$2/ in: $3"; fi; }

WORK="$(mktemp -d -t tur-script-deps-XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

TUR_ABS="$(cd "$(dirname "$TUR")" && pwd)/$(basename "$TUR")"
export TUR_HOME="$WORK/home"
# A developer's own experiments file must not decide what these runs see.
export XDG_CONFIG_HOME="$WORK/config"
X=("$TUR_ABS" --enable=script-deps)
# The lifecycle warning is not what these tests are about.
quiet() { grep -v 'TUR-W0060'; }

GIT=(git -c user.email=t@example.com -c user.name=t -c init.defaultBranch=main)

# mkspice DIR NAME MODULE BODY [MANIFEST-EXTRA] -- a git repo holding one spice
mkspice() {
    local dir="$1" name="$2" mod="$3" body="$4" extra="${5:-}"
    mkdir -p "$dir/src/$(dirname "$mod")"
    printf '(defpackage %s\n  :version "0.1.0"%s)\n' "$name" "$extra" > "$dir/build.tur"
    printf '%s\n' "$body" > "$dir/src/$mod.tur"
    (cd "$dir" && "${GIT[@]}" init -q && "${GIT[@]}" add -A && "${GIT[@]}" commit -qm init)
}
# publish DIR -- (re)create DIR.git as a bare mirror of DIR's refs
publish() {
    rm -rf "$1.git"
    git clone -q --bare "$1" "$1.git"
}
url() { echo "file://$(native "$1").git"; }

# ------------------------------------------------------------------ #
# Fixtures                                                            #
# ------------------------------------------------------------------ #
G="$WORK/greet"
mkspice "$G" greet greet/hello '(defmodule greet/hello
  (export answer)
  (defn answer [] : int 42))'
(cd "$G" && "${GIT[@]}" tag v1)
publish "$G"
GURL="$(url "$G")"

S="$WORK/scripts"
mkdir -p "$S"
cat > "$S/report.tur" <<EOF
;; reads the answer
(defscript
  :spices #map{"greet" #map{:url "$GURL" :ref "v1"}})

(defmodule report
  (import greet/hello)
  (defn main [] : int
    (println (greet/hello/answer))
    0))
EOF

# ================================================================== #
# SD0 -- the header: grammar, reading, gating                         #
# ================================================================== #
L="$S/localdep"
mkdir -p "$L/src/ld"
printf '(defpackage ld)\n' > "$L/build.tur"
printf '(defmodule ld/m\n  (export seven)\n  (defn seven [] : int 7))\n' > "$L/src/ld/m.tur"
cat > "$S/pathdep.tur" <<'EOF'
#!/usr/bin/env tur
#| a block comment before the header |#
(defscript :spices #map{"ld" #map{:path "localdep"}})
(defmodule pathdep
  (import ld/m)
  (defn main [] : int
    (println (ld/m/seven))
    0))
EOF
out="$(cd "$WORK" && "${X[@]}" run scripts/pathdep.tur 2>&1 | quiet)"
expect "SD0: a header is consumed, and a :path dep resolves against the script" "7" "$out"

out="$(cd "$S" && "$TUR_ABS" check pathdep.tur 2>&1)"
expect_grep "SD0: without --enable=script-deps a header is TUR-E0633" "TUR-E0633" "$out"

printf '(defn f [] : int 1)\n(defscript :spices #map{})\n(defn main [] : int 0)\n' > "$S/late.tur"
out="$("${X[@]}" check "$S/late.tur" 2>&1)"
expect_grep "SD0: a header that is not the first form is TUR-E0630" "TUR-E0630" "$out"

mkdir -p "$S/mods"
printf '(defscript :spices #map{})\n(defmodule mods/inner (export one) (defn one [] : int 1))\n' > "$S/mods/inner.tur"
printf '(defmodule outer\n  (import mods/inner)\n  (defn main [] : int (mods/inner/one)))\n' > "$S/outer.tur"
out="$("${X[@]}" check "$S/outer.tur" 2>&1)"
expect_grep "SD0: a header in an imported module is TUR-E0630" "TUR-E0630" "$out"

printf '(defscript :spices #map{} :cmake-deps #map{})\n(defn main [] : int 0)\n' > "$S/badkey.tur"
out="$("${X[@]}" check "$S/badkey.tur" 2>&1)"
expect_grep "SD0: an unknown header key is TUR-E0631" "TUR-E0631.*cmake-deps" "$out"

printf '(defscript :spices #map{"a" #map{:url "%s"}})\n(defn main [] : int 0)\n' "$GURL" > "$S/noref.tur"
out="$("${X[@]}" check "$S/noref.tur" 2>&1)"
expect_grep "SD0: a :url dep without :ref is TUR-E0631" "TUR-E0631.*no :ref" "$out"

P="$WORK/proj"
mkdir -p "$P/src"
printf '(defpackage proj :version "0.1.0")\n' > "$P/build.tur"
printf '(defscript :spices #map{})\n(defn main [] : int 0)\n' > "$P/inproj.tur"
out="$("${X[@]}" check "$P/inproj.tur" 2>&1)"; rc=$?
expect_grep "SD0: a header inside a project is TUR-E0632" "TUR-E0632" "$out"
[ "$rc" -ne 0 ] && ok "SD0: ... and fails the command" || bad "SD0: ... and fails the command" "rc=$rc"

cat > "$S/sweet.tur.sweet" <<'EOF'
;; a sweet-exp script
defscript
  :spices #map{"ld" #map{:path "localdep"}}

defmodule sweet
  import ld/m
  defn main [] : int
    println(ld/m/seven())
    0
EOF
out="$("${X[@]}" run "$S/sweet.tur.sweet" 2>&1 | quiet)"
expect "SD0: a sweet-exp header" "7" "$out"

cp "$S/pathdep.tur" "$S/fmt.tur"
"$TUR_ABS" fmt "$S/fmt.tur" >/dev/null 2>&1
out="$("$TUR_ABS" fmt --check "$S/fmt.tur" 2>&1)"; rc=$?
if [ "$rc" -eq 0 ] && grep -q '^(defscript' "$S/fmt.tur"; then ok "SD0: tur fmt round-trips a header"
else bad "SD0: tur fmt round-trips a header" "rc=$rc $out $(head -4 "$S/fmt.tur")"; fi
out="$("${X[@]}" run "$S/fmt.tur" 2>&1 | quiet)"
expect "SD0: ... and the formatted header still works" "7" "$out"

# ================================================================== #
# SD1 -- the dep cache                                                #
# ================================================================== #
out="$("${X[@]}" run "$S/report.tur" 2>"$WORK/err")"
expect "SD1: cold fetch, then run" "42" "$out"
expect_grep "SD1: a fetch says so on stderr" "fetching greet (v1) for report.tur" "$(cat "$WORK/err")"
if ! grep -qi 'cloning\|switching' <<<"$out"; then ok "SD1: the fetch writes nothing to stdout"
else bad "SD1: the fetch writes nothing to stdout" "$out"; fi
if grep -q "$GURL" "$TUR_HOME/cache/script-spices/index.tur" 2>/dev/null; then ok "SD1: index.tur records the resolved ref"
else bad "SD1: index.tur records the resolved ref"; fi
if ! grep -q greet "$TUR_HOME/state.tur" 2>/dev/null; then ok "SD1: a script dep is not an installed spice"
else bad "SD1: a script dep is not an installed spice"; fi

out="$("${X[@]}" run --offline "$S/report.tur" 2>"$WORK/err")"
expect "SD1: a warm run works --offline" "42" "$out"
if ! grep -q fetching "$WORK/err"; then ok "SD1: ... and fetches nothing"; else bad "SD1: ... and fetches nothing" "$(cat "$WORK/err")"; fi

# The tag moves: the pinned run keeps the old commit, --update takes the new.
(cd "$G" && sed -i.bak 's/42/43/' src/greet/hello.tur && rm -f src/greet/hello.tur.bak \
   && "${GIT[@]}" commit -qam v1b && "${GIT[@]}" tag -f v1 >/dev/null)
V1B="$(cd "$G" && git rev-parse HEAD)"
publish "$G"
out="$("${X[@]}" run "$S/report.tur" 2>/dev/null)"
expect "SD1: a moved tag stays pinned until --update" "42" "$out"
out="$("${X[@]}" run --update "$S/report.tur" 2>/dev/null)"
expect "SD1: --update re-resolves the tag" "43" "$out"
out="$("${X[@]}" run "$S/report.tur" 2>/dev/null)"
expect "SD1: ... and later runs use the new pin" "43" "$out"

out="$(TUR_HOME="$WORK/cold" "${X[@]}" run --offline "$S/report.tur" 2>&1)"; rc=$?
if [ "$rc" -ne 0 ] && grep -q 'TUR-E0634.*not in the.*--offline' <<<"$out"; then ok "SD1: --offline with a cold cache fails, naming the dep"
else bad "SD1: --offline with a cold cache fails, naming the dep" "rc=$rc $out"; fi

# Two scripts pin two commits of one spice, side by side.
V1A="$(cd "$G" && git rev-parse HEAD~1)"
sed "s/:ref \"v1\"/:ref \"$V1A\"/" "$S/report.tur" > "$S/old.tur"
out_old="$("${X[@]}" run "$S/old.tur" 2>/dev/null)"
out_new="$("${X[@]}" run "$S/report.tur" 2>/dev/null)"
expect "SD1: two scripts pin two commits of one spice" "42 43" "$out_old $out_new"

# ================================================================== #
# SD2 -- the header decides the search path                          #
# ================================================================== #
I="$WORK/instd"
mkdir -p "$I/src/instd"
printf '(defpackage instd :version "0.1.0")\n' > "$I/build.tur"
printf '(defmodule instd/x\n  (export five)\n  (defn five [] : int 5))\n' > "$I/src/instd/x.tur"
"$TUR_ABS" install "$I" --path >/dev/null 2>&1
printf '(defmodule plain\n  (import instd/x)\n  (defn main [] : int (println (instd/x/five)) 0))\n' > "$S/plain.tur"
out="$("$TUR_ABS" run "$S/plain.tur" 2>&1)"
expect "SD2: (control) without a header the installed spice resolves" "5" "$out"
{ printf '(defscript :spices #map{"greet" #map{:url "%s" :ref "v1"}})\n' "$GURL"; cat "$S/plain.tur"; } > "$S/hermetic.tur"
out="$("${X[@]}" run "$S/hermetic.tur" 2>&1)"
expect_grep "SD2: with a header, an undeclared installed spice is not importable" "module 'instd/x' not found" "$out"
{ printf '(defscript :spices #map{"instd" #map{:global true}})\n' ; cat "$S/plain.tur"; } > "$S/global.tur"
out="$("${X[@]}" run "$S/global.tur" 2>&1 | quiet)"
expect "SD2: a :global true dep resolves from the install registry" "5" "$out"

C="$WORK/cfx"
mkdir -p "$C/c"
mkspice "$C" cfx cfx/core '(defmodule cfx/core
  (export seven)
  (defn seven [] : int
    ```c
    extern long cfx_seven(void);
    return cfx_seven();
    ```))' '
  :build-opts #map{:c-sources ["c/cfx.c"]}'
echo 'long cfx_seven(void) { return 7; }' > "$C/c/cfx.c"
(cd "$C" && "${GIT[@]}" add -A && "${GIT[@]}" commit -qm c && "${GIT[@]}" tag v1)
publish "$C"
cat > "$S/usec.tur" <<EOF
(defscript :spices #map{"cfx" #map{:url "$(url "$C")" :ref "v1"}})
(defmodule usec
  (import cfx/core)
  (defn main [] : int
    (println (* (cfx/core/seven) 6))
    0))
EOF
out="$("${X[@]}" run "$S/usec.tur" 2>/dev/null)"
expect "SD2: a header dep's :c-sources link" "42" "$out"

# A header dep with :spices of its own: set up as a fetched project.
O="$WORK/outer"
mkspice "$O" outer outer/core "(defmodule outer/core
  (import greet/hello)
  (export doubled)
  (defn doubled [] : int (* 2 (greet/hello/answer))))" "
  :spices #map{\"greet\" #map{:url \"$GURL\" :ref \"v1\"}}"
(cd "$O" && "${GIT[@]}" tag v1)
publish "$O"
cat > "$S/trans.tur" <<EOF
(defscript :spices #map{"outer" #map{:url "$(url "$O")" :ref "v1"}})
(defmodule trans
  (import outer/core)
  (defn main [] : int
    (println (outer/core/doubled))
    0))
EOF
out="$("${X[@]}" run "$S/trans.tur" 2>/dev/null)"
expect "SD2: a header dep's own :spices are fetched into its entry" "86" "$out"

# ================================================================== #
# SD3 -- `tur <script> args...`                                       #
# ================================================================== #
BIN="$WORK/bin"
mkdir -p "$BIN"
ln -sf "$TUR_ABS" "$BIN/tur" 2>/dev/null || cp "$TUR_ABS" "$BIN/tur"
cat > "$S/args.tur" <<'EOF'
#!/usr/bin/env -S tur --enable=script-deps
(defn main [] : int
  (println (length *args*))
  3)
EOF
chmod +x "$S/args.tur"
if [ "$IS_WINDOWS" = 0 ]; then
    out="$(cd "$S" && PATH="$BIN:$PATH" ./args.tur a --no-color c 2>/dev/null)"; rc=$?
    expect "SD3: a #! script receives its arguments" "3" "$out"
    expect "SD3: ... and its exit status passes through" "3" "$rc"
fi
out="$(cd "$S" && "$TUR_ABS" args.tur x y 2>/dev/null)"; rc=$?
expect "SD3: tur <file> args runs the file" "2/3" "$out/$rc"
cp "$S/args.tur" "$S/check.tur"
out="$(cd "$S" && "$TUR_ABS" check.tur 2>/dev/null)"; rc=$?
expect "SD3: a script named like a subcommand runs" "0/3" "$out/$rc"
out="$(cd "$S" && "$TUR_ABS" nope.tur 2>&1)"; rc=$?
if [ "$rc" = 2 ] && grep -q "no such file 'nope.tur'" <<<"$out"; then ok "SD3: a missing file is a clean usage error"
else bad "SD3: a missing file is a clean usage error" "rc=$rc $out"; fi
out="$("${X[@]}" "$S/report.tur" 2>/dev/null)"
expect "SD3: tur <script with a header>" "43" "$out"

# ================================================================== #
# SD4 -- tur add --script, and the lock                               #
# ================================================================== #
cat > "$S/added.tur" <<'EOF'
#!/usr/bin/env -S tur --enable=script-deps
;; no header yet
(defmodule added
  (import greet/hello)
  (defn main [] : int
    (println (greet/hello/answer))
    0))
EOF
"${X[@]}" add --script "$S/added.tur" "$GURL" --ref v1 --name greet >/dev/null 2>&1
if head -4 "$S/added.tur" | grep -q '^(defscript' && head -1 "$S/added.tur" | grep -q '^#!'; then ok "SD4: add --script writes a header after the preamble"
else bad "SD4: add --script writes a header after the preamble" "$(head -6 "$S/added.tur")"; fi
out="$("${X[@]}" run "$S/added.tur" 2>/dev/null)"
expect "SD4: ... and the script runs" "43" "$out"
"${X[@]}" add --script "$S/added.tur" ./localdep --path --name ld >/dev/null 2>&1
if grep -q '"ld" #map{:path "./localdep"}' "$S/added.tur" && [ "$(grep -c defscript "$S/added.tur")" = 1 ]; then ok "SD4: add --script adds to an existing header"
else bad "SD4: add --script adds to an existing header" "$(head -8 "$S/added.tur")"; fi

"${X[@]}" fetch --script "$S/report.tur" --lock >/dev/null 2>&1
if [ -f "$S/report.tur.lock" ] && grep -q "\"greet\".*:resolved \"$V1B\".*:sha256 \"tree1:" "$S/report.tur.lock"; then ok "SD4: fetch --script --lock writes <file>.lock"
else bad "SD4: fetch --script --lock writes <file>.lock" "$(cat "$S/report.tur.lock" 2>&1)"; fi
(cd "$G" && sed -i.bak 's/43/44/' src/greet/hello.tur && rm -f src/greet/hello.tur.bak \
   && "${GIT[@]}" commit -qam v1c && "${GIT[@]}" tag -f v1 >/dev/null)
publish "$G"
out="$("${X[@]}" run --update "$S/report.tur" 2>/dev/null)"
expect "SD4: a lock wins over --update" "43" "$out"
out="$("${X[@]}" fetch --script "$S/report.tur" --lock --update 2>/dev/null; "${X[@]}" run "$S/report.tur" 2>/dev/null)"
expect "SD4: fetch --script --lock --update moves the lock" "44" "$out"

ENTRY="$(ls -d "$TUR_HOME"/cache/script-spices/greet-"${V1B:0:12}" 2>/dev/null)"
"${X[@]}" fetch --script "$S/report.tur" --lock >/dev/null 2>&1   # lock the 44 commit
NEW="$(cd "$G" && git rev-parse HEAD)"
ENTRY="$TUR_HOME/cache/script-spices/greet-${NEW:0:12}"
echo ';; tampered' >> "$ENTRY/src/greet/hello.tur"
out="$("${X[@]}" run "$S/report.tur" 2>&1)"; rc=$?
if [ "$rc" -ne 0 ] && grep -q 'TUR-E0634.*does not match' <<<"$out"; then ok "SD4: a tampered cache entry fails the lock's hash check"
else bad "SD4: a tampered cache entry fails the lock's hash check" "rc=$rc $out"; fi
sed -i.bak '/;; tampered/d' "$ENTRY/src/greet/hello.tur"; rm -f "$ENTRY/src/greet/hello.tur.bak"

sed "s|^  :spices #map{\"greet\"|  :spices #map{\"greet2\" #map{:url \"$GURL\" :ref \"v1\"} \"greet\"|" "$S/report.tur" > "$S/stale.tur"
cp "$S/report.tur.lock" "$S/stale.tur.lock"
out="$("${X[@]}" run "$S/stale.tur" 2>&1)"; rc=$?
if [ "$rc" -ne 0 ] && grep -q 'lock is stale' <<<"$out"; then ok "SD4: a dep added after locking reports a stale lock"
else bad "SD4: a dep added after locking reports a stale lock" "rc=$rc $out"; fi

# ================================================================== #
# SD5 -- tur cache                                                    #
# ================================================================== #
out="$("$TUR_ABS" cache list 2>&1)"
expect_grep "SD5: tur cache list shows the entries" "greet-${NEW:0:12}.*last used.*@ v1" "$out"
out="$("$TUR_ABS" cache clean --older-than 1 2>&1)"
expect_grep "SD5: clean --older-than keeps recently used entries" "removed 0 entries" "$out"
out="$("$TUR_ABS" cache clean 2>&1)"
n_left="$(find "$TUR_HOME/cache/script-spices" -mindepth 1 -maxdepth 1 | wc -l | tr -d ' ')"
expect "SD5: tur cache clean empties the cache" "0" "$n_left"

echo
echo "summary: $PASS passed, $FAIL failed"
if [ "$FAIL" -gt 0 ]; then
    printf '  failed: %s\n' "${FAILED[@]}"
    exit 1
fi
exit 0
