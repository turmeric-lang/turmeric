#!/usr/bin/env bash
# tests/check-cloudflare-guide-service.sh -- the Cloudflare guide's service
# builds, and serves concurrent requests.
#
# docs/guides/cloudflare-deployment-guide.md ("Approach A", Step 1) carries a
# whole program, service.tur, in an s-expression and a sweet-exp copy.  No
# test compiled it, so it rotted unseen: its fibers started from capturing
# lambdas (`fiber-new` runs a bare C function pointer, so the closure record
# was jumped to as code -- TUR-E0295 since `fiber-new` takes `(c-fn [] nil)`),
# it called an `int->str` it never loaded and socket operations outside
# `unsafe`, and handed `scheduler-spawn` a FiberHandle where it took
# `ptr<void>` (docs/archive/cloudflare-guide-service-captures-into-fiber-new.md).
#
# This extracts both copies from the guide, builds each with `tur build`
# (failing on TUR-W0071, the pruned unit's full-unit retry, which this program
# also hit), runs the s-expression build on a free loopback port and checks
# that three concurrent connections each get the guide's response.  The sweet
# copy is only built: the guide-pairs check already proves it reads to the same
# program.  Skips on a Windows host, where the program's fd model is not POSIX.
set -uo pipefail
cd "$(dirname "$0")/.."
TUR_REL="${TUR:-${1:-./build/tur}}"
TUR="$(cd "$(dirname "$TUR_REL")" && pwd)/$(basename "$TUR_REL")"
[ -x "$TUR" ] || { echo "check-cloudflare-guide-service: $TUR not built" >&2; exit 2; }
if [ "${TUR_HOST_WINDOWS:-0}" = "1" ]; then
    echo "SKIP check-cloudflare-guide-service: Windows host"
    exit 0
fi
GUIDE=docs/guides/cloudflare-deployment-guide.md
WORK=$(mktemp -d)
SVC_PID=""
cleanup() {
    [ -n "$SVC_PID" ] && kill "$SVC_PID" 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

fail=0
pass() { echo "PASS: $1"; }
bad()  { echo "FAIL: $1"; fail=1; }

# The first ```turmeric block and the first ````sweet-exp block after the
# Step 1 heading.  The sweet block is fenced with four backticks because it
# holds an inline-C fence of three.
python3 - "$GUIDE" "$WORK/service.tur" "$WORK/service.tur.sweet" <<'PY'
import sys
src = open(sys.argv[1]).read()
at = src.index("### Step 1 -- write the Turmeric service")
def block(opener, closer, start):
    b = src.index(opener, start) + len(opener)
    return src[b:src.index(closer, b)], b
sexp, b = block("```turmeric\n", "\n```\n", at)
sweet, _ = block("````sweet-exp\n", "\n````\n", b)
open(sys.argv[2], "w").write(sexp + "\n")
open(sys.argv[3], "w").write(sweet + "\n")
PY
[ -s "$WORK/service.tur" ] && [ -s "$WORK/service.tur.sweet" ] ||
    { echo "FAIL: could not extract service.tur from $GUIDE"; exit 1; }

build_one() {   # $1 = source, $2 = output binary, $3 = label
    if ! (cd "$WORK" && "$TUR" build "$1" -o "$2") > "$WORK/$3.log" 2>&1; then
        bad "$3: tur build failed"; sed 's/^/    /' "$WORK/$3.log" | head -40; return 1
    fi
    if grep -q 'TUR-W0071' "$WORK/$3.log"; then
        bad "$3: built only on the full-unit retry (TUR-W0071)"
        sed 's/^/    /' "$WORK/$3.log" | head -40; return 1
    fi
    pass "$3 builds"
}
build_one service.tur svc "s-expression copy"
build_one service.tur.sweet svc-sweet "sweet-exp copy"

if [ -x "$WORK/svc" ]; then
    PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()')
    PORT="$PORT" TUR_BIND_LOOPBACK=1 "$WORK/svc" > "$WORK/svc.out" 2>&1 &
    SVC_PID=$!
    # The client is Python: one sendall per request, all three connections
    # opened before any is read, and a reset after the response arrived
    # tolerated (the service closes after one recv).  It also waits for the
    # port, with a connect that sends nothing -- the service answers that one
    # and closes.
    python3 - "$PORT" "$WORK" <<'PY' || fail=1
import socket, sys, time
port, work = int(sys.argv[1]), sys.argv[2]
for _ in range(100):
    try:
        socket.create_connection(("127.0.0.1", port), timeout=1).close()
        break
    except OSError:
        time.sleep(0.1)
else:
    print("FAIL: service never listened on :%d" % port)
    print(open(work + "/svc.out").read())
    sys.exit(1)
conns = [socket.create_connection(("127.0.0.1", port), timeout=5) for _ in range(3)]
for c in conns:
    c.sendall(b"GET / HTTP/1.0\r\nHost: localhost\r\n\r\n")
ok = True
for i, c in enumerate(conns, 1):
    data = b""
    try:
        while True:
            chunk = c.recv(4096)
            if not chunk:
                break
            data += chunk
    except (ConnectionResetError, socket.timeout):
        pass
    c.close()
    text = data.decode("latin-1")
    if text.startswith("HTTP/1.1 200 OK\r\n") and text.endswith("\r\n\r\nHello from Turmeric!\n"):
        print("PASS: connection %d answered" % i)
    else:
        print("FAIL: connection %d: unexpected response %r" % (i, text))
        ok = False
sys.exit(0 if ok else 1)
PY
fi

exit "$fail"
