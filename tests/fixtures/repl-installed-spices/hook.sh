#!/usr/bin/env bash
# installed-spice-libraries IL4/IL5: the REPL outside a project searches the
# installed spices (after its -I dirs), attaches the compiled image of a spice
# a turn imports from -- so an inline-C function whose loop the interpreter
# cannot run is called compiled -- and `:spice` loads a spice by name or by
# path, inside a project too.  The install area is a scratch TUR_HOME.
set -u
TMP="$1"
TUR="$(cd "$(dirname "$TUR")" && pwd)/$(basename "$TUR")"
export TUR_HOME="$TMP/home"
export TUR_BIN="$TUR"
export ASAN_OPTIONS=detect_leaks=0
# The harness turns the fallback off for every other fixture; this one is
# about the fallback.
unset TUR_NO_GLOBAL_SPICES TUR_NO_AUTO_SPICE

mkdir -p "$TMP/ilc/src/ilc" "$TMP/incd/incm" "$TMP/scratch" "$TMP/proj/src"
cat > "$TMP/ilc/build.tur" <<'TUR_EOF'
(defpackage ilc :name "ilc" :version "0.4.0")
TUR_EOF
cat > "$TMP/ilc/src/ilc/core.tur" <<'TUR_EOF'
(defmodule ilc/core
  (export twice)
  (defn twice [x : int] : int
    ```c
    long acc = 0;
    for (long i = 0; i < x; i++) acc += 2;
    return acc;
    ```))
TUR_EOF
cat > "$TMP/incd/incm/m.tur" <<'TUR_EOF'
(defmodule incm/m
  (export inc1)
  (defn inc1 [x : int] : int (+ x 1)))
TUR_EOF
echo '(defpackage proj :version "0.1.0")' > "$TMP/proj/build.tur"
"$TUR" install "$TMP/ilc" --path >/dev/null 2>&1 || echo "install failed"

# Keep the lines a reader checks, and spell the spice's directory <tmp>/ilc.
# tur prints it realpath'd, which is not $TMP's spelling everywhere (Windows
# prints a drive-lettered, possibly backslashed path for an MSYS /tmp/...), so
# the match is on the path's last component rather than on $TMP itself.
filter() {
    grep -E '^=>|Loaded spice|already loaded|not installed|^;;   ' |
        tr -d '\r' | tr '\\' '/' |
        sed -E -e 's#[^ ]*/ilc( |$)#<tmp>/ilc\1#g' -e 's/  */ /g'
}

echo "-- outside a project: installed spice, -I, :spice by name and path"
cd "$TMP/scratch"
printf '%s\n' \
    '(import ilc/core :refer [twice])' \
    '(twice 21)' \
    '(import incm/m :refer [inc1])' \
    '(inc1 41)' \
    ':spice ilc' \
    ':spice ilc' \
    ':spice ../ilc' \
    ':spice nosuch' \
    ':reset' \
    '(ilc/core/twice 3)' \
    ':spice' \
  | "$TUR" repl -I ../incd 2>&1 | filter

echo "-- inside a project: :spice is an explicit request"
cd "$TMP/proj"
printf '%s\n' \
    '(import ilc/core :refer [twice])' \
    ':spice ilc' \
    '(ilc/core/twice 5)' \
  | "$TUR" repl 2>&1 | filter
exit 0
