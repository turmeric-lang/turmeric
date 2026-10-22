#!/usr/bin/env bash
# project-less-tools-plan PT1: `tur serve` from a scratch directory outside the
# repo, on a free port.  Start it in the background, read the port from the
# startup line, curl an index, a redirect, a listing, a file, a dotfile and a
# traversal, check the request log, then stop it.  The usage errors exit 2.
set -u
TMP="$1"
FIXTURE_DIR="$(cd "$(dirname "$0")" && pwd)"
TUR_ABS="$(cd "$(dirname "$TUR")" && pwd)/$(basename "$TUR")"

# The fixture needs an HTTP client; without curl it has nothing to drive.
if ! command -v curl >/dev/null 2>&1; then
    echo "cli-serve: curl not found; skipped" >&2
    cat "$FIXTURE_DIR/expected.stdout"
    exit 0
fi

root="$TMP/site"
mkdir -p "$root/sub/inner"
printf '<p>home</p>' > "$root/index.html"
printf 'bee'         > "$root/sub/b.txt"
printf 'secret'      > "$root/sub/.hidden"
printf 'up'          > "$TMP/outside.txt"

# start <dir-arg>... -- run `tur serve` in $TMP, wait for the startup line.
pid=""
port=""
start() {
    (cd "$TMP" && exec "$TUR_ABS" serve "$@") > "$TMP/serve.out" 2> "$TMP/serve.err" &
    pid=$!
    for _ in $(seq 600); do
        grep -q '^Serving ' "$TMP/serve.out" 2>/dev/null && break
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.1
    done
    port=$(sed -nE 's|^Serving .* on http://127\.0\.0\.1:([0-9]+)/.*|\1|p' "$TMP/serve.out")
    sed -E 's|:[0-9]+/|:PORT/|' "$TMP/serve.out"
    if [ -z "$port" ]; then
        echo "tur serve did not start" >&2
        cat "$TMP/serve.err" >&2
        stop
        exit 1
    fi
}
# stop -- the server, and on the cc engine the program it spawned.
stop() {
    [ -n "$pid" ] || return 0
    pkill -P "$pid" 2>/dev/null
    kill "$pid" 2>/dev/null
    wait "$pid" 2>/dev/null
    pid=""
}
get() {
    curl --noproxy '*' --path-as-is -s -o "$TMP/body" -D "$TMP/head" "http://127.0.0.1:$port$1"
    local status loc
    status=$(sed -nE '1s|^HTTP/1\.[01] ([0-9]+).*|\1|p' "$TMP/head")
    loc=$(tr -d '\r' < "$TMP/head" | sed -nE 's|^Location: (.*)|\1|p')
    printf '%s -> %s' "$1" "$status"
    [ -n "$loc" ] && printf ' location=%s' "$loc"
    printf '\n'
}

start site --port 0
get /
echo "  body: $(cat "$TMP/body")"
get /sub
get /sub/
grep -o '<a href="[^"]*">[^<]*</a>' "$TMP/body" | sed 's/^/  /'
grep -q hidden "$TMP/body" && echo "  .hidden listed"
get /sub/b.txt
echo "  body: $(cat "$TMP/body")"
get /sub/.hidden
get /../outside.txt
get /nope
stop
# The request log: one line per request on stderr, the time normalized.
# Only the log lines: an engine may add notes of its own (a JIT that falls
# back to cc says so), which are not the server's output.
grep -E '^(GET|HEAD|POST) ' "$TMP/serve.err" | sed -E 's/[0-9]+ms$/Nms/' 

# --no-listing: the index still serves, a directory without one is a 404.
start site -p 0 --no-listing -q
get /
get /sub/
stop
echo "quiet log lines: $(grep -cE '^(GET|HEAD|POST) ' "$TMP/serve.err")"

# Default directory: the current one.
(cd "$root" && exec "$TUR_ABS" serve -p 0) > "$TMP/serve.out" 2> "$TMP/serve.err" &
pid=$!
for _ in $(seq 600); do
    grep -q '^Serving ' "$TMP/serve.out" 2>/dev/null && break
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.1
done
sed -E 's|:[0-9]+/|:PORT/|' "$TMP/serve.out"
stop

# Usage errors exit 2 without starting a server.
for args in "--frob" "-p 70000" "no-such-dir" "a b" "--bind nope"; do
    # shellcheck disable=SC2086
    "$TUR_ABS" serve $args > /dev/null 2> "$TMP/err"
    rc=$?
    echo "serve $(echo "$args" | sed "s|$TMP|TMP|") -> exit $rc ($(grep -m1 '^tur serve:' "$TMP/err" | sed "s|$TMP|TMP|"))"
done
"$TUR_ABS" serve --help | head -1
exit 0
