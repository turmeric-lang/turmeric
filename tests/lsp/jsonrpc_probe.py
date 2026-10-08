"""tests/lsp/jsonrpc_probe.py -- framing and an exact sync point for LSP probes.

Shared by tests/lsp/saffron-diagnostics.py and tests/lsp/r7rs-diagnostics.py,
which drive a LIVE `tur lsp` session (unlike stdio-smoke.py, whose answer comes
from a process that exits on its own).

Why this exists
---------------
Both probes used to `time.sleep(15)` after didOpen and then read whatever had
accumulated.  Four sessions per probe, two probes: about two minutes of pure
sleeping in every CI job on both legs.  Measured at 60.1-60.8s per suite with
essentially no variance (Linux min 60.1, max 60.2) -- the signature of a fixed
wait rather than of work, and confirmed by `user` time of 0.24s against 60.10s
of `real`.

The sleep was there for a real reason.  Analysis is DEFERRED: run_doc_analysis
writes the buffer to a temp file and runs a full compile, so doing it inline from
didOpen made a fast typist queue one compile per keystroke (see the comment above
flush_one_dirty in src/lsp/lsp.c).  Diagnostics therefore arrive some time after
didOpen, and a session that tears down first sees none -- which looks exactly
like a clean file, the one failure these probes exist to rule out.

But waiting on the clock was never the right instrument, because the protocol
already orders the events:

  * `textDocument/documentSymbol` -- like hover, definition, references,
    completion and rename -- calls lsp_flush_dirty BEFORE it answers, since
    those handlers read doc->symbols and must not describe a stale buffer.  The
    flush runs the analysis, which sends publishDiagnostics on the same stdout
    stream.  So ASKING for document symbols is what makes the diagnostics
    happen; it is not a probe for whether they have.
  * The server dispatches one message at a time off a single queue, so sending
    documentSymbol before `shutdown`/`exit` means the flush is processed, and its
    notification written, before the process is allowed to leave.

So the ordering does all the work and there is nothing to tune: send
documentSymbol, then exit, then read the stream to EOF.  `settle()` is the one
line that forces the flush; `drain()` is what guarantees we have all of the
output.

Note what is NOT load-bearing here: on_shutdown does NOT flush dirty documents,
so the documentSymbol request is doing the work, not the shutdown.  Removing it
and keeping only the exit would go back to racing the 200ms debounce.
"""

import json
import threading


def frame(obj):
    """Encode one JSON-RPC message with its Content-Length header."""
    body = json.dumps(obj).encode("utf-8")
    return b"Content-Length: %d\r\n\r\n%s" % (len(body), body)


def start_reader(proc):
    """Read proc's stdout to EOF on a thread; return (buffer, thread).

    The thread is returned, not hidden, because a caller MUST join it before
    trusting the buffer -- see drain().
    """
    buf = bytearray()

    def reader():
        while True:
            c = proc.stdout.read(1)
            if not c:
                break
            buf.extend(c)

    t = threading.Thread(target=reader, daemon=True)
    t.start()
    return buf, t


def settle(proc, uri, want_id=9):
    """Force the deferred analysis to run for `uri`.

    Sends documentSymbol, whose handler flushes dirty documents before
    answering, so publishDiagnostics is written ahead of its response.  Send
    this BEFORE shutdown/exit and the ordering guarantees the notification is in
    the stream; there is nothing to wait for afterwards.
    """
    try:
        proc.stdin.write(frame({"jsonrpc": "2.0", "id": want_id,
                                "method": "textDocument/documentSymbol",
                                "params": {"textDocument": {"uri": uri}}}))
        proc.stdin.flush()
    except Exception:
        pass


def drain(proc, thread, timeout=30):
    """Wait for the server to exit AND for its output to be fully read.

    Joining the reader is the part that is easy to leave out and was previously
    masked by the 15s sleep: proc.wait() returns as soon as the process is gone,
    which says nothing about whether the reader thread has consumed the last
    bytes sitting in the pipe.  Without the join, the final frames are a race
    that would show up as a rare, unreproducible "no diagnostics published".
    """
    try:
        proc.wait(timeout=timeout)
    except Exception:
        proc.kill()
    thread.join(timeout=timeout)


def iter_frames(out):
    """Yield each complete JSON-RPC object in `out`, ignoring a partial tail."""
    while out:
        head, sep, rest = out.partition(b"\r\n\r\n")
        if not sep:
            return
        length = None
        for line in head.split(b"\r\n"):
            if line.lower().startswith(b"content-length:"):
                try:
                    length = int(line.split(b":", 1)[1].strip())
                except ValueError:
                    return
        if length is None:
            return
        if len(rest) < length:
            return                      # frame still arriving -- stop here
        body, out = rest[:length], rest[length:]
        try:
            yield json.loads(body.decode("utf-8"))
        except Exception:
            continue
