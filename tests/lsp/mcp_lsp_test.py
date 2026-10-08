#!/usr/bin/env python3
"""End-to-end JSON-RPC tests for `tur mcp` and `tur lsp`.

Spawns the servers as subprocesses and drives them over stdio.

Transport framing:
  tur mcp -- MCP 2024-11-05 stdio: newline-delimited JSON  (one JSON object
             per line, terminated by '\n', no headers).
  tur lsp -- LSP stdio: Content-Length headers followed by the JSON body.

Exit code 0 = all tests passed. Non-zero = at least one assertion failed.
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import textwrap

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
# $TUR wins, then build/tur, then build/tur.exe.  Both halves matter on
# Windows: the binary has a .exe suffix, and the build directory is rarely
# named "build" there (build-win, build-win-rel, ...).  Without this the
# harness silently reports "not built" on the one platform where `tur lsp`
# and `tur dap` were broken.
def _find_tur():
    env = os.environ.get("TUR")
    if env:
        return os.path.abspath(env)
    for name in ("tur", "tur.exe"):
        cand = os.path.join(ROOT, "build", name)
        if os.path.exists(cand):
            return cand
    return os.path.join(ROOT, "build", "tur")


TUR = _find_tur()


def native_path(path):
    """A path the `tur` BINARY can open, not just this interpreter.

    Under MSYS2 -- which is how Windows CI runs this -- tempfile hands back
    /tmp/mcp_test_x.tur.  That is a real file to the msys runtime and to python,
    and meaningless to tur.exe, which is a native Windows program: a leading
    slash there means the root of the current drive, so it looked for C:\tmp and
    reported "Could not read or analyze".  Every MCP test that writes a scratch
    file failed that way, while the same tests passed locally where TEMP already
    pointed at a Windows path.

    cygpath is the translation both halves agree on.  Absent (real POSIX), the
    path is already native and comes back unchanged.
    """
    if os.name != "nt" and "MSYSTEM" not in os.environ:
        return path
    try:
        out = subprocess.run(["cygpath", "-m", path], capture_output=True,
                             timeout=15)
        if out.returncode == 0:
            got = out.stdout.decode().strip()
            if got:
                return got
    except (OSError, subprocess.SubprocessError):
        pass
    return path


def to_uri(path):
    """Spell a path the way an LSP client does.

    On POSIX that is "file://" + an already-absolute path.  On Windows a bare
    concatenation gives file://C:\\dir\\x.tur, which is not a file URI at all --
    the drive letter lands in the authority and the separators are wrong.  Every
    real client sends file:///C:/dir/x.tur, and that is what the server now
    emits, so the tests have to speak it too or they compare two spellings of
    the same file and call them different.
    """
    p = os.path.abspath(path).replace("\\", "/")
    return "file:///" + p if (len(p) > 1 and p[1] == ":") else "file://" + p

PASS = 0
FAIL = 0


def check(cond: bool, msg: str) -> None:
    global PASS, FAIL
    if cond:
        PASS += 1
    else:
        FAIL += 1
        print(f"FAIL: {msg}", file=sys.stderr)


# ---------------------------------------------------------------------------
# MCP transport: newline-delimited JSON (MCP 2024-11-05 stdio spec)
# ---------------------------------------------------------------------------

def mcp_encode(msg: dict) -> bytes:
    return json.dumps(msg).encode("utf-8") + b"\n"


def mcp_read_one(stream) -> dict | None:
    while True:
        line = stream.readline()
        if not line:
            return None
        line = line.strip()
        if not line:
            continue
        return json.loads(line)


# ---------------------------------------------------------------------------
# LSP transport: Content-Length header framing (Language Server Protocol)
# ---------------------------------------------------------------------------

def lsp_encode(msg: dict) -> bytes:
    body = json.dumps(msg).encode("utf-8")
    return f"Content-Length: {len(body)}\r\n\r\n".encode("ascii") + body


def lsp_read_one(stream) -> dict | None:
    headers = b""
    while b"\r\n\r\n" not in headers:
        ch = stream.read(1)
        if not ch:
            return None
        headers += ch
    header_text = headers.decode("ascii", errors="replace")
    length = 0
    for line in header_text.split("\r\n"):
        if line.lower().startswith("content-length:"):
            length = int(line.split(":", 1)[1].strip())
            break
    body = b""
    while len(body) < length:
        chunk = stream.read(length - len(body))
        if not chunk:
            break
        body += chunk
    return json.loads(body)


class Server:
    def __init__(self, args: list[str], transport: str = "mcp"):
        self.proc = subprocess.Popen(
            args,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            cwd=ROOT,
        )
        self._next_id = 0
        self._encode   = mcp_encode   if transport == "mcp" else lsp_encode
        self._read_one = mcp_read_one if transport == "mcp" else lsp_read_one
        self.diagnostics: list[dict] = []

    def send_batch(self, msgs: list[dict]) -> None:
        """Write several messages in one go.

        Cancellation can only be observed if the cancel is already on the wire
        when the server looks -- a message at a time would always lose the
        race.
        """
        self.proc.stdin.write(b"".join(self._encode(m) for m in msgs))
        self.proc.stdin.flush()

    def next_id(self) -> int:
        self._next_id += 1
        return self._next_id

    def await_id(self, want: int) -> dict | None:
        while True:
            r = self._read_one(self.proc.stdout)
            if r is None:
                return None
            if r.get("method") == "textDocument/publishDiagnostics":
                self.diagnostics.append(r.get("params", {}))
                continue
            if r.get("id") == want:
                return r

    def call(self, method: str, params: dict | None = None,
             notification: bool = False, expect_response: bool = True) -> dict | None:
        msg: dict = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            msg["params"] = params
        if not notification:
            self._next_id += 1
            msg["id"] = self._next_id
        self.proc.stdin.write(self._encode(msg))
        self.proc.stdin.flush()
        if notification or not expect_response:
            return None
        # The LSP server may emit publishDiagnostics notifications before the
        # response -- skip over them and return the first message with our id.
        return self.await_id(msg["id"])

    def close(self) -> None:
        try:
            self.proc.stdin.close()
        except Exception:
            pass
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()


# ---------------------------------------------------------------------------
# Test fixtures
# ---------------------------------------------------------------------------

GOOD_TUR = textwrap.dedent("""\
    ;;; add -- add two ints together
    ;;;
    ;;; Returns:
    ;;;   The sum.
    (defn add [a :int b :int] :int
      (+ a b))

    ;;; mul -- multiply two ints
    (defn mul [a :int b :int] :int
      (* a b))
    """)

BAD_TUR = textwrap.dedent("""\
    (defn broken [x :int] :int
      (+ x undeclared-symbol))
    """)


def make_tempfile(content: str) -> str:
    fd, path = tempfile.mkstemp(suffix=".tur", prefix="mcp_test_")
    path = native_path(path)
    os.write(fd, content.encode("utf-8"))
    os.close(fd)
    return path


# ---------------------------------------------------------------------------
# MCP tests
# ---------------------------------------------------------------------------

def test_mcp() -> None:
    print("--- MCP server tests ---")
    good_path = make_tempfile(GOOD_TUR)
    bad_path  = make_tempfile(BAD_TUR)
    try:
        srv = Server([TUR, "mcp"], transport="mcp")

        # initialize
        r = srv.call("initialize", {
            "protocolVersion": "2024-11-05",
            "capabilities": {},
            "clientInfo": {"name": "py-test", "version": "0"},
        })
        check(r is not None, "mcp initialize: response")
        check(r and r.get("result", {}).get("serverInfo", {}).get("name") == "turmeric",
              "mcp initialize: serverInfo.name == turmeric")

        # tools/list
        r = srv.call("tools/list", {})
        tools = r["result"]["tools"] if r else []
        names = {t["name"] for t in tools}
        expected = {"check_file", "symbols", "hover", "definition",
                    "complete", "doc", "format", "build"}
        check(expected.issubset(names),
              f"mcp tools/list: all 8 tools present (got {names})")

        def call_tool(name: str, args: dict) -> dict:
            return srv.call("tools/call", {"name": name, "arguments": args})

        def tool_text(r, what: str) -> str:
            """The `text` of a tools/call result, or a legible failure.

            `srv.call` returns None only when the reader hit EOF on the
            server's stdout -- i.e. the server EXITED mid-session, which is a
            different and much more serious thing than a tool answering
            unhelpfully.  Subscripting that None raised

                TypeError: 'NoneType' object is not subscriptable

            several frames from anything meaningful, which is how a server
            crash came to read as a null result.  Say what actually happened,
            and include the exit status and whatever the server put on stderr,
            since on a crash that is the only evidence there is.  See
            docs/archive/mcp-server-exits-mid-session-on-windows.md.
            """
            if r is None:
                # WAIT for the status, do not poll for it.  EOF on stdout
                # arrives while the process is still tearing down, so an
                # immediate poll() answers None -- which is exactly what the
                # 2026-09-18 occurrence reported, and it says nothing.  The
                # real code is what separates a crash (0xC0000005 access
                # violation, 0xC00000FD stack overflow on Windows; a negative
                # signal number on POSIX) from a clean early exit.
                try:
                    rc = srv.proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    rc = "still running 10s after closing stdout"
                if isinstance(rc, int) and rc > 0xFFFF:
                    rc = f"{rc} ({rc & 0xFFFFFFFF:#010x})"
                err = ""
                try:
                    if srv.proc.stderr is not None:
                        err = srv.proc.stderr.read() or ""
                        if isinstance(err, bytes):
                            err = err.decode("utf-8", "replace")
                except Exception:
                    pass
                check(False,
                      f"{what}: server closed stdout (exited mid-session); "
                      f"exit status {rc!r}; stderr: {err[:400]!r}")
                return ""
            return r["result"]["content"][0]["text"]

        # check_file on a good file: diagnostics array (possibly empty,
        # possibly populated by stdlib-deps).  Either way, response must
        # be valid and isError == false.
        r = call_tool("check_file", {"path": good_path})
        result = r.get("result", {}) if r else {}
        text = result.get("content", [{}])[0].get("text", "")
        check(result.get("isError") is False, "check_file good: isError == false")
        check(text.startswith("["), f"check_file good: text starts with '[' (got {text[:80]!r})")

        # check_file on a bad file: diagnostics array should be non-empty.
        # `tur_check_only` may also reject standalone files that don't
        # import stdlib — in either case `text` should not be just "[]".
        r = call_tool("check_file", {"path": bad_path})
        bad_text = r["result"]["content"][0]["text"]
        check(bad_text != "[]" or r["result"].get("isError"),
              f"check_file bad: produces diagnostics or error (got {bad_text[:80]!r})")

        # symbols on a good file: must list `add` and `mul`
        r = call_tool("symbols", {"path": good_path})
        sym_text = r["result"]["content"][0]["text"]
        # Parse the JSON array; entries are objects with "name" keys.
        try:
            sym_arr = json.loads(sym_text)
            sym_names = {s.get("name") for s in sym_arr if isinstance(s, dict)}
        except Exception:
            sym_arr = []
            sym_names = set()
        check("add" in sym_names, f"symbols: 'add' present (names={sym_names})")
        check("mul" in sym_names, f"symbols: 'mul' present (names={sym_names})")

        # hover on `add` (line 4, char 6 in 0-based -- the 'd' of 'add' in
        # `(defn add ...)`).  Should at minimum mention the symbol name.
        r = call_tool("hover", {"path": good_path, "line": 4, "col": 8})
        hover_text = tool_text(r, "hover")
        check("add" in hover_text or "No type information" in hover_text or
              "No symbol" in hover_text,
              f"hover: mentions 'add' or reports miss (got {hover_text[:80]!r})")

        # definition on `add` at the same position.
        r = call_tool("definition", {"path": good_path, "line": 4, "col": 8})
        def_text = tool_text(r, "definition")
        check("line" in def_text or "No definition" in def_text or "No symbol" in def_text,
              f"definition: returns location or miss (got {def_text[:80]!r})")

        # complete in an import context: dynamic stdlib discovery should
        # surface multiple `stdlib/*` entries.
        imp_path = make_tempfile("(import stdlib/")
        try:
            r = call_tool("complete", {"path": imp_path, "line": 0, "col": 15})
            comp_text = r["result"]["content"][0]["text"]
            check("stdlib/list" in comp_text and "stdlib/option" in comp_text,
                  f"complete (import ctx): stdlib modules present")
            # Verify it's discovered dynamically -- the count should
            # exceed the 14 modules in the old hardcoded list.
            try:
                comp_arr = json.loads(comp_text)
                check(len(comp_arr) > 20,
                      f"complete: >20 stdlib entries (got {len(comp_arr)}) -- dynamic discovery")
            except Exception:
                check(False, "complete: response is valid JSON array")
        finally:
            os.unlink(imp_path)

        # doc on `add`: should return the ;;; block content.
        r = call_tool("doc", {"path": good_path, "name": "add"})
        doc_text = r["result"]["content"][0]["text"]
        check("add" in doc_text and "two ints" in doc_text,
              f"doc 'add': summary returned (got {doc_text[:120]!r})")

        # doc on a missing symbol: should report not-found.
        r = call_tool("doc", {"path": good_path, "name": "nonexistent"})
        miss_text = r["result"]["content"][0]["text"]
        check("No docstring" in miss_text,
              f"doc miss: 'No docstring' message (got {miss_text[:80]!r})")

        # format on a good file: should return formatted source.  `tur
        # format` may not be installed in all configs; tolerate that.
        r = call_tool("format", {"path": good_path})
        fmt_result = r["result"]
        fmt_text = fmt_result["content"][0]["text"]
        check(fmt_result.get("isError") is False or "exited" in fmt_text,
              f"format: either succeeds or reports clean exit-status error")

        srv.close()

        # disabling via TUR_NO_MCP: spawn a fresh server with the env set
        # and verify it exits without producing any JSON-RPC traffic.
        env = os.environ.copy()
        env["TUR_NO_MCP"] = "1"
        p = subprocess.Popen([TUR, "mcp"], stdin=subprocess.PIPE,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                             cwd=ROOT, env=env)
        out, err = p.communicate(timeout=5)
        check(p.returncode == 0,
              f"TUR_NO_MCP=1: exit 0 (got {p.returncode})")
        check(b"disabled" in err.lower() or b"disabled" in err,
              f"TUR_NO_MCP=1: stderr mentions 'disabled'")
        check(out == b"", f"TUR_NO_MCP=1: no stdout traffic (got {out[:80]!r})")

    finally:
        os.unlink(good_path)
        os.unlink(bad_path)


def test_mcp_repeated_analysis() -> None:
    """One server, many analyses: the server must still be there at the end.

    Every MCP tool call compiles its file again in the SAME process, and the
    CPS emitter cached its classification keyed on the program's and the
    EmitCtx's ADDRESSES.  Once the allocator handed a later compile the same
    addresses, the stale cache was emitted and the server died with an access
    violation -- on Windows after 25-40 calls of exactly this sequence, and on
    CI as early as the fifth (docs/archive/mcp-server-exits-mid-session-on-windows.md).
    """
    print("--- MCP repeated analysis ---")
    good_path = make_tempfile(GOOD_TUR)
    bad_path = make_tempfile(BAD_TUR)
    rounds = 40
    try:
        srv = Server([TUR, "mcp"], transport="mcp")
        srv.call("initialize", {"protocolVersion": "2024-11-05",
                                "capabilities": {},
                                "clientInfo": {"name": "py-test", "version": "0"}})
        seq = [("check_file", {"path": good_path}),
               ("check_file", {"path": bad_path}),
               ("symbols", {"path": good_path}),
               ("hover", {"path": good_path, "line": 4, "col": 8}),
               ("definition", {"path": good_path, "line": 4, "col": 8})]
        answered = 0
        died = None
        for r in range(rounds):
            for name, args in seq:
                resp = srv.call("tools/call", {"name": name, "arguments": args})
                if resp is None:
                    try:
                        rc = srv.proc.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        rc = "still running"
                    err = srv.proc.stderr.read().decode("utf-8", "replace")
                    died = (f"{name} in round {r + 1}, after {answered} answers; "
                            f"exit {rc!r}; stderr: {err.strip()[-300:]!r}")
                    break
                answered += 1
            if died:
                break
        check(died is None,
              f"mcp repeated analysis: {rounds * len(seq)} calls in one server"
              + (f" -- server died at {died}" if died else ""))
        if died is None:
            srv.close()
    finally:
        os.unlink(good_path)
        os.unlink(bad_path)


# ---------------------------------------------------------------------------
# LSP tests
# ---------------------------------------------------------------------------

def test_lsp() -> None:
    print("--- LSP server tests ---")
    good_path = make_tempfile(GOOD_TUR)
    uri = to_uri(good_path)
    try:
        srv = Server([TUR, "lsp"], transport="lsp")

        r = srv.call("initialize", {
            "processId": None, "rootUri": None, "capabilities": {},
        })
        caps = r["result"]["capabilities"] if r else {}
        check(caps.get("hoverProvider") is True, "lsp init: hoverProvider")
        check(caps.get("definitionProvider") is True, "lsp init: definitionProvider")
        check(caps.get("documentSymbolProvider") is True, "lsp init: documentSymbolProvider")

        srv.call("initialized", {}, notification=True)

        srv.call("textDocument/didOpen", {
            "textDocument": {
                "uri": uri,
                "languageId": "tur",
                "version": 1,
                "text": GOOD_TUR,
            },
        }, notification=True)

        # documentSymbol: should list add + mul
        r = srv.call("textDocument/documentSymbol", {
            "textDocument": {"uri": uri},
        })
        symbols = r["result"] if r and r.get("result") else []
        names = {s.get("name") for s in symbols if isinstance(s, dict)}
        check("add" in names, f"lsp documentSymbol: 'add' present (got {names})")
        check("mul" in names, f"lsp documentSymbol: 'mul' present (got {names})")

        # hover on `add` (line 4, char 7-ish in `(defn add ...)`).  Should
        # return a hover with non-empty contents.
        r = srv.call("textDocument/hover", {
            "textDocument": {"uri": uri},
            "position": {"line": 4, "character": 8},
        })
        hover = r["result"] if r else None
        if hover:
            val = ""
            contents = hover.get("contents")
            if isinstance(contents, dict):
                val = contents.get("value", "")
            elif isinstance(contents, str):
                val = contents
            check("add" in val or val == "",
                  f"lsp hover: mentions 'add' or empty (got {val[:80]!r})")
        else:
            check(True, "lsp hover: null result tolerated")

        # definition on `add` at the same position
        r = srv.call("textDocument/definition", {
            "textDocument": {"uri": uri},
            "position": {"line": 4, "character": 8},
        })
        defn = r["result"] if r else None
        if defn:
            check("uri" in defn and "range" in defn,
                  f"lsp definition: location object returned")
        else:
            check(True, "lsp definition: null result tolerated")

        srv.call("shutdown", {})
        srv.call("exit", {}, notification=True)
        srv.close()

    finally:
        os.unlink(good_path)


def test_lsp_encoding_and_deferred_analysis() -> None:
    """positionEncoding, \\uXXXX decoding, and deferred (debounced) analysis."""
    print("--- LSP encoding / deferred analysis ---")

    # json.dumps defaults to ensure_ascii=True, so the accented character below
    # goes out on the wire as a \\u00e9 escape — exactly the path under test.
    src = '(def a "é") (def later 2)\n'
    # `later` sits at byte 18 once the escape is decoded (e-acute is 2 UTF-8
    # bytes). Passing the escape through as the 5 literal chars "u00e9" would
    # report 21, and every position after it would be wrong by 3.
    expected_col = src.encode("utf-8").index(b"later")

    path = make_tempfile("(def a 1)\n")
    uri = to_uri(path)
    try:
        srv = Server([TUR, "lsp"], transport="lsp")
        r = srv.call("initialize", {
            "processId": None, "rootUri": None, "capabilities": {},
        })
        caps = r["result"]["capabilities"] if r else {}
        check(caps.get("positionEncoding") == "utf-8",
              f"lsp init: positionEncoding is utf-8 (got {caps.get('positionEncoding')!r})")

        srv.call("initialized", {}, notification=True)
        srv.call("textDocument/didOpen", {
            "textDocument": {"uri": uri, "languageId": "tur",
                             "version": 1, "text": src},
        }, notification=True)

        r = srv.call("textDocument/documentSymbol", {"textDocument": {"uri": uri}})
        symbols = r["result"] if r and r.get("result") else []
        later = next((s for s in symbols
                      if isinstance(s, dict) and s.get("name") == "later"), None)
        check(later is not None,
              f"lsp uXXXX: 'later' found (got {[s.get('name') for s in symbols]})")
        if later:
            col = later["selectionRange"]["start"]["character"]
            check(col == expected_col,
                  f"lsp uXXXX: escape decoded, 'later' at {expected_col} (got {col})")

        # Analysis is deferred until the client goes quiet, but a request that
        # reads symbols must flush it first — otherwise the answer would
        # describe the buffer as it was before the edit.
        srv.call("textDocument/didChange", {
            "textDocument": {"uri": uri, "version": 2},
            "contentChanges": [{"text": "(def freshly-added 7)\n"}],
        }, notification=True)
        r = srv.call("textDocument/documentSymbol", {"textDocument": {"uri": uri}})
        names = {s.get("name") for s in (r["result"] if r and r.get("result") else [])
                 if isinstance(s, dict)}
        check("freshly-added" in names,
              f"lsp deferred analysis: request flushes pending edit (got {names})")
        check("later" not in names,
              f"lsp deferred analysis: stale symbols dropped (got {names})")

        srv.call("shutdown", {})
        srv.call("exit", {}, notification=True)
        srv.close()
    finally:
        os.unlink(path)


GAPS_TUR = textwrap.dedent("""\
    ;;; twice -- double an int
    (defn twice [a :int] :int
      (* a 2))

    ;;; combine -- add two ints
    (defn combine [a :int b :int] :int
      (+ a b))
    """)


def test_lsp_client_gaps() -> None:
    """The gaps a real client (Trowel) had to work around -- see
    docs/archive/lsp-client-gaps-plan.md."""
    print("--- LSP client-gap coverage ---")
    path = make_tempfile(GAPS_TUR)
    uri = to_uri(path)
    try:
        srv = Server([TUR, "lsp"], transport="lsp")

        # A client that advertises plaintext-only hover must not be sent
        # fenced markdown to strip back out itself.
        r = srv.call("initialize", {
            "processId": None, "rootUri": None,
            "capabilities": {
                "textDocument": {"hover": {"contentFormat": ["plaintext"]}},
            },
        })
        caps = r["result"]["capabilities"] if r else {}
        check(caps.get("documentFormattingProvider") is True,
              "lsp init: documentFormattingProvider advertised")
        check(isinstance(caps.get("signatureHelpProvider"), dict),
              "lsp init: signatureHelpProvider advertised")
        # Not `(`: in a lisp the callee is typed AFTER the paren, so a `(`
        # trigger could only ever be answered with null. The space after the
        # head is the first position with an answer.
        sig_triggers = caps.get("signatureHelpProvider", {}).get("triggerCharacters")
        check(sig_triggers == [" "],
              f"lsp init: signature-help triggers are [' '] (got {sig_triggers!r})")
        # Space fires on nearly every keystroke in a lisp; neither known client
        # was willing to honor it, so it should not be advertised.
        triggers = caps.get("completionProvider", {}).get("triggerCharacters")
        check(triggers == ["("],
              f"lsp init: completion triggers are ['('] only (got {triggers!r})")

        srv.call("initialized", {}, notification=True)
        srv.call("textDocument/didOpen", {
            "textDocument": {"uri": uri, "languageId": "tur",
                             "version": 1, "text": GAPS_TUR},
        }, notification=True)

        # -- 2.2/2.3/2.5: completion at the very start of the buffer ----------
        r = srv.call("textDocument/completion", {
            "textDocument": {"uri": uri},
            "position": {"line": 0, "character": 0},
        })
        res = r["result"] if r else None
        check(isinstance(res, dict) and "items" in res,
              f"lsp completion: CompletionList shape (got {type(res).__name__})")
        items = res.get("items", []) if isinstance(res, dict) else []
        check(len(items) > 0,
              "lsp completion at offset 0: non-empty (was empty -- the word at "
              "the cursor was read as the prefix)")
        labels = [i.get("label") for i in items]
        # The buffer's own definitions must not be truncated behind stdlib.
        check("twice" in labels and "combine" in labels,
              f"lsp completion: document-local symbols present (first={labels[:4]})")
        if "twice" in labels and len(labels) > 2:
            check(labels.index("twice") < 2 and labels.index("combine") < 2,
                  f"lsp completion: document-local symbols first (got {labels[:4]})")

        # -- 2.1: completion survives a buffer that does not parse ------------
        broken = GAPS_TUR + "\n(smoke\n"
        srv.call("textDocument/didChange", {
            "textDocument": {"uri": uri, "version": 2},
            "contentChanges": [{"text": broken}],
        }, notification=True)
        r = srv.call("textDocument/completion", {
            "textDocument": {"uri": uri},
            "position": {"line": 8, "character": 0},
        })
        stale = r["result"].get("items", []) if r and r.get("result") else []
        check(len(stale) > 0,
              "lsp completion: last-good symbols retained through a parse "
              f"failure (got {len(stale)} items)")

        # A zero-width diagnostic range paints nothing; it must be widened.
        widths = [d["range"]["end"]["character"] - d["range"]["start"]["character"]
                  for p in srv.diagnostics for d in p.get("diagnostics", [])
                  if d["range"]["start"]["line"] == d["range"]["end"]["line"]]
        check(all(w > 0 for w in widths),
              f"lsp diagnostics: no zero-width ranges (got widths {widths})")

        srv.call("textDocument/didChange", {
            "textDocument": {"uri": uri, "version": 3},
            "contentChanges": [{"text": GAPS_TUR}],
        }, notification=True)

        # -- 4: hover honors the negotiated contentFormat ---------------------
        r = srv.call("textDocument/hover", {
            "textDocument": {"uri": uri},
            "position": {"line": 1, "character": 7},
        })
        contents = (r["result"] or {}).get("contents", {}) if r and r.get("result") else {}
        if isinstance(contents, dict):
            check(contents.get("kind") == "plaintext",
                  f"lsp hover: plaintext honored (got {contents.get('kind')!r})")
            check("```" not in contents.get("value", ""),
                  f"lsp hover: no markdown fences in plaintext "
                  f"(got {contents.get('value', '')[:60]!r})")

        # -- 3.2: textDocument/formatting -------------------------------------
        srv.call("textDocument/didChange", {
            "textDocument": {"uri": uri, "version": 4},
            "contentChanges": [{"text": "(defn  sloppy   [ ] :int\n        1)\n"}],
        }, notification=True)
        r = srv.call("textDocument/formatting", {
            "textDocument": {"uri": uri},
            "options": {"tabSize": 2, "insertSpaces": True},
        })
        edits = r["result"] if r else None
        check(isinstance(edits, list) and len(edits) == 1,
              f"lsp formatting: one full-document edit (got {edits!r})")
        if isinstance(edits, list) and edits:
            e = edits[0]
            check(e["range"]["start"] == {"line": 0, "character": 0},
                  "lsp formatting: edit starts at the top of the buffer")
            check("sloppy" in e["newText"] and "(defn  sloppy" not in e["newText"],
                  f"lsp formatting: output is reformatted (got {e['newText']!r})")

        # An unformattable buffer returns "no edits", not an error popup.
        srv.call("textDocument/didChange", {
            "textDocument": {"uri": uri, "version": 5},
            "contentChanges": [{"text": "(defn oops [\n"}],
        }, notification=True)
        r = srv.call("textDocument/formatting", {
            "textDocument": {"uri": uri}, "options": {},
        })
        check(r is not None and r.get("result") is None and "error" not in r,
              f"lsp formatting: unparseable buffer yields null, not an error "
              f"(got {r})")

        # -- 3.3: signatureHelp ------------------------------------------------
        call_src = GAPS_TUR + "\n(defn use [] :int\n  (combine 1 2))\n"
        srv.call("textDocument/didChange", {
            "textDocument": {"uri": uri, "version": 6},
            "contentChanges": [{"text": call_src}],
        }, notification=True)
        # Line 9 is "  (combine 1 2))"; character 13 sits on the second argument.
        r = srv.call("textDocument/signatureHelp", {
            "textDocument": {"uri": uri},
            "position": {"line": 9, "character": 13},
        })
        sig = r["result"] if r else None
        check(isinstance(sig, dict) and sig.get("signatures"),
              f"lsp signatureHelp: signature returned (got {sig!r})")
        if isinstance(sig, dict) and sig.get("signatures"):
            s0 = sig["signatures"][0]
            check("combine" in s0.get("label", ""),
                  f"lsp signatureHelp: names the callee (got {s0.get('label')!r})")
            check(len(s0.get("parameters", [])) == 2,
                  f"lsp signatureHelp: two parameters (got {s0.get('parameters')!r})")
            check(sig.get("activeParameter") == 1,
                  f"lsp signatureHelp: second argument is active "
                  f"(got {sig.get('activeParameter')!r})")

        # A callee defined only in an edit nothing has analyzed yet: signature
        # help answers from the index as it stands, so it has to notice the
        # miss and analyze before answering, not report null.
        fresh_src = call_src + "\n(defn fresh-fn [a :int b :int c :int] :int a)\n(fresh-fn 1 2 3)\n"
        srv.call("textDocument/didChange", {
            "textDocument": {"uri": uri, "version": 6},
            "contentChanges": [{"text": fresh_src}],
        }, notification=True)
        fresh_line = fresh_src.count("\n") - 1
        r = srv.call("textDocument/signatureHelp", {
            "textDocument": {"uri": uri},
            "position": {"line": fresh_line, "character": len("(fresh-fn ")},
        })
        sig = r["result"] if r else None
        check(isinstance(sig, dict) and bool(sig.get("signatures"))
              and "fresh-fn" in sig["signatures"][0].get("label", ""),
              f"lsp signatureHelp: callee from an unanalyzed edit (got {sig!r})")

        # Outside any call there is nothing to describe.
        r = srv.call("textDocument/signatureHelp", {
            "textDocument": {"uri": uri},
            "position": {"line": 0, "character": 0},
        })
        check(r is not None and r.get("result") is None,
              f"lsp signatureHelp: null at top level (got {r})")

        # -- 3.1: $/cancelRequest ---------------------------------------------
        # The cancel and a following request go out in the same write, so both
        # are already buffered when the server drains after its analysis flush.
        srv.call("textDocument/didChange", {
            "textDocument": {"uri": uri, "version": 7},
            "contentChanges": [{"text": call_src + "\n(def tail 1)\n"}],
        }, notification=True)
        doomed = srv.next_id()
        follow = srv.next_id()
        srv.send_batch([
            {"jsonrpc": "2.0", "id": doomed, "method": "textDocument/completion",
             "params": {"textDocument": {"uri": uri},
                        "position": {"line": 0, "character": 0}}},
            {"jsonrpc": "2.0", "method": "$/cancelRequest", "params": {"id": doomed}},
            {"jsonrpc": "2.0", "id": follow, "method": "textDocument/documentSymbol",
             "params": {"textDocument": {"uri": uri}}},
        ])
        r = srv.await_id(doomed)
        check(r is not None and r.get("error", {}).get("code") == -32800,
              f"lsp $/cancelRequest: cancelled request answered -32800 (got {r})")
        r = srv.await_id(follow)
        names = {s.get("name") for s in (r["result"] if r and r.get("result") else [])
                 if isinstance(s, dict)}
        check("tail" in names,
              f"lsp $/cancelRequest: queued follow-up still served (got {names})")

        srv.call("shutdown", {})
        srv.call("exit", {}, notification=True)
        srv.close()
    finally:
        os.unlink(path)


def test_lsp_unprimed_completion() -> None:
    """Completion in a document that has NEVER parsed.

    Retention (§2.1) rescues a document that used to parse. It does nothing
    for one that never has, and the pre-existing gap-coverage test cannot see
    that: it issues a request between the didOpen and the breaking edit, which
    forces an analysis and primes the index. Every case here is deliberately
    unprimed -- no request lands before the buffer is broken.

    See docs/archive/history/lsp-symbol-retention-never-primes.md.
    """
    print("--- LSP completion without a primed index ---")

    GOOD = "(defn zorkle [a :int] :int a)\n"
    # `zorkle` rather than a plain name on purpose: a symbol that collides with
    # an auto-loaded stdlib name makes the file fail to compile, which would
    # silently turn a passing assertion into a vacuous one.
    BROKEN_TAIL = "\n(smoke\n"

    def completion_items(srv, uri, line, char):
        r = srv.call("textDocument/completion", {
            "textDocument": {"uri": uri},
            "position": {"line": line, "character": char},
        })
        res = r["result"] if r and r.get("result") else {}
        return res.get("items", []) if isinstance(res, dict) else []

    # -- opened good, edited broken before any analysis ran ------------------
    path = make_tempfile(GOOD)
    uri = to_uri(path)
    try:
        srv = Server([TUR, "lsp"], transport="lsp")
        srv.call("initialize", {"processId": None, "rootUri": None,
                                "capabilities": {}})
        srv.call("initialized", {}, notification=True)
        srv.call("textDocument/didOpen", {
            "textDocument": {"uri": uri, "languageId": "tur",
                             "version": 1, "text": GOOD},
        }, notification=True)
        # NO request here -- that is the whole point.
        srv.call("textDocument/didChange", {
            "textDocument": {"uri": uri, "version": 2},
            "contentChanges": [{"text": GOOD + BROKEN_TAIL}],
        }, notification=True)

        items = completion_items(srv, uri, 2, 0)
        check(len(items) > 0,
              f"lsp unprimed: broken before first analysis still completes "
              f"(got {len(items)} items)")

        # Once it parses, the document's own definitions take over and rank
        # ahead of the stdlib -- the fallback must not displace them.
        srv.call("textDocument/didChange", {
            "textDocument": {"uri": uri, "version": 3},
            "contentChanges": [{"text": GOOD}],
        }, notification=True)
        labels = [i.get("label") for i in completion_items(srv, uri, 1, 0)]
        check(labels and labels[0] == "zorkle",
              f"lsp unprimed: own symbols return and rank first once it parses "
              f"(got {labels[:3]})")

        # And retention still works from there.
        srv.call("textDocument/didChange", {
            "textDocument": {"uri": uri, "version": 4},
            "contentChanges": [{"text": GOOD + BROKEN_TAIL}],
        }, notification=True)
        r = srv.call("textDocument/documentSymbol", {"textDocument": {"uri": uri}})
        names = {s.get("name") for s in (r["result"] if r and r.get("result") else [])
                 if isinstance(s, dict)}
        check("zorkle" in names,
              f"lsp unprimed: retention still applies after a later break "
              f"(got {names})")

        srv.call("shutdown", {})
        srv.call("exit", {}, notification=True)
        srv.close()
    finally:
        os.unlink(path)

    # -- opened already broken: never parsed, nothing of its own to retain ---
    path = make_tempfile(GOOD + BROKEN_TAIL)
    uri = to_uri(path)
    try:
        srv = Server([TUR, "lsp"], transport="lsp")
        srv.call("initialize", {"processId": None, "rootUri": None,
                                "capabilities": {}})
        srv.call("initialized", {}, notification=True)
        srv.call("textDocument/didOpen", {
            "textDocument": {"uri": uri, "languageId": "tur", "version": 1,
                             "text": GOOD + BROKEN_TAIL},
        }, notification=True)

        items = completion_items(srv, uri, 2, 0)
        labels = [i.get("label") for i in items]
        check(len(items) > 0,
              f"lsp never-parsed: completion falls back to the stdlib "
              f"(got {len(items)} items)")
        # The fallback is the stdlib surface only. A document symbol appearing
        # here would mean the cache had been filled from some other document's
        # analysis, which would leak one file's definitions into another's
        # completions.
        check("zorkle" not in labels,
              f"lsp never-parsed: fallback carries no document-local symbols "
              f"(got {labels[:4]})")
        # Real completion, not a raw dump: the prefix still filters.
        srv.call("textDocument/didChange", {
            "textDocument": {"uri": uri, "version": 2},
            "contentChanges": [{"text": GOOD + BROKEN_TAIL + "vec-new\n"}],
        }, notification=True)
        pf = [i.get("label") for i in completion_items(srv, uri, 3, 4)]
        check(pf and all(l.lower().startswith("vec-") for l in pf),
              f"lsp never-parsed: prefix still filters the fallback (got {pf[:4]})")

        # Diagnostics for the real document must be unaffected -- the stdlib
        # harvest runs its own compile and must not leak into them.
        diags = [d for p in srv.diagnostics for d in p.get("diagnostics", [])]
        check(len(diags) > 0,
              "lsp never-parsed: the document's own diagnostics still publish")

        srv.call("shutdown", {})
        srv.call("exit", {}, notification=True)
        srv.close()
    finally:
        os.unlink(path)


def test_lsp_unsaved_buffer() -> None:
    """A document with no filesystem path still gets language support.

    Analysis routes the buffer through a temp file, so an `untitled:` URI is
    not actually a blocker on the server side -- worth pinning, because the
    client that skipped unsaved buffers assumed otherwise.
    """
    print("--- LSP unsaved (untitled) buffer ---")
    uri = "untitled:Untitled-1"
    src = "(defn scratch [a :int] :int\n  (+ a 1))\n"
    srv = Server([TUR, "lsp"], transport="lsp")
    try:
        srv.call("initialize", {"processId": None, "rootUri": None,
                                "capabilities": {}})
        srv.call("initialized", {}, notification=True)
        srv.call("textDocument/didOpen", {
            "textDocument": {"uri": uri, "languageId": "tur",
                             "version": 1, "text": src},
        }, notification=True)

        r = srv.call("textDocument/documentSymbol", {"textDocument": {"uri": uri}})
        names = {s.get("name") for s in (r["result"] if r and r.get("result") else [])
                 if isinstance(s, dict)}
        check("scratch" in names,
              f"lsp untitled: symbols collected (got {names})")

        r = srv.call("textDocument/formatting", {
            "textDocument": {"uri": uri}, "options": {},
        })
        edits = r["result"] if r else None
        check(isinstance(edits, list) and edits,
              f"lsp untitled: formatting works without a real path (got {edits!r})")

        srv.call("shutdown", {})
        srv.call("exit", {}, notification=True)
    finally:
        srv.close()


def test_lsp_inside_a_spice() -> None:
    """Cross-file identity, against the real compiler (A6).

    A module inside a spice imports its siblings, and everything the editor
    knows about those names depends on the analysis resolving the import.  The
    LSP writes the buffer to a scratch file and analyses THAT, so resolution
    has to walk up from where the buffer lives rather than from where it was
    written -- and until it did, a file in a spice came back with no symbols at
    all: no completion, no go-to-definition, and a rename that refused because
    it could not see a definition.  `tur check` on the same file worked the
    whole time, which is exactly what made it easy to miss.

    Also pinned here: rename across two modules of one spice, and the refusal
    on a stdlib name.
    """
    print("--- LSP inside a spice (A6) ---")
    # realpath, not the raw mkdtemp result. On macOS /var is a symlink to
    # /private/var, so mkdtemp hands back the unresolved spelling while the
    # compiler registers the resolved one -- and a definition that correctly
    # crossed to the sibling module then failed a string compare on its uri.
    # Canonicalising here makes every path this test builds match whichever
    # spelling the server reports, on both platforms.
    root = native_path(os.path.realpath(tempfile.mkdtemp(prefix="lsp_spice_")))
    src_dir = os.path.join(root, "src")
    os.makedirs(src_dir, exist_ok=True)
    with open(os.path.join(root, "build.tur"), "w") as f:
        f.write('(defpackage lspspice\n  :name    "lspspice"\n'
                '  :version "0.1.0")\n')

    mathy_src = textwrap.dedent("""\
        (defmodule mathy
          (export double-it)
          (defn double-it [n : int] : int
            (* n 2)))
        """)
    user_src = textwrap.dedent("""\
        (defmodule user
          (import mathy)
          (defn go [] : int
            (let [double-it 7]
              (+ double-it 1)))
          (defn go2 [] : int
            (double-it 3)))
        """)
    mathy_path = os.path.join(src_dir, "mathy.tur")
    user_path = os.path.join(src_dir, "user.tur")
    with open(mathy_path, "w") as f:
        f.write(mathy_src)
    with open(user_path, "w") as f:
        f.write(user_src)

    mathy_uri = to_uri(mathy_path)
    user_uri = to_uri(user_path)

    def caret(text: str, needle: str, offset: int = 0):
        for i, line in enumerate(text.split("\n")):
            j = line.find(needle)
            if j >= 0:
                return {"line": i, "character": j + offset}
        raise AssertionError("no %r in fixture" % needle)

    srv = Server([TUR, "lsp"], transport="lsp")
    try:
        r = srv.call("initialize", {"processId": None, "rootUri": None,
                                    "capabilities": {}})
        caps = (r or {}).get("result", {}).get("capabilities", {})
        check(isinstance(caps.get("renameProvider"), dict)
              and caps["renameProvider"].get("prepareProvider") is True,
              "lsp spice: rename advertised in the prepare form")
        check(caps.get("referencesProvider") is True,
              "lsp spice: references advertised")
        srv.call("initialized", {}, notification=True)

        for uri, text in ((mathy_uri, mathy_src), (user_uri, user_src)):
            srv.call("textDocument/didOpen", {
                "textDocument": {"uri": uri, "languageId": "tur",
                                 "version": 1, "text": text},
            }, notification=True)

        # Go-to-definition on a name defined in a SIBLING module lands in that
        # module's file. This is the whole of A6's "verify" list, and it was
        # answering null.
        pos = caret(user_src, "(double-it 3)", 1)
        r = srv.call("textDocument/definition",
                     {"textDocument": {"uri": user_uri}, "position": pos})
        loc = (r or {}).get("result")
        check(isinstance(loc, dict) and loc.get("uri") == mathy_uri,
              f"lsp spice: definition crosses to the sibling module (got {loc!r})")

        # Completion offers the imported name.
        r = srv.call("textDocument/completion",
                     {"textDocument": {"uri": user_uri},
                      "position": {"line": pos["line"],
                                   "character": pos["character"] + 4}})
        res = (r or {}).get("result") or {}
        items = res if isinstance(res, list) else res.get("items", [])
        labels = {i.get("label") for i in items}
        check("double-it" in labels,
              f"lsp spice: completion offers an imported name (got {labels!r})")

        # Rename at the definition rewrites the sibling's call site -- and NOT
        # the `let` in that sibling that shadows the same name.
        defpos = caret(mathy_src, "defn double-it", 7)
        r = srv.call("textDocument/rename",
                     {"textDocument": {"uri": mathy_uri}, "position": defpos,
                      "newName": "twice"})
        changes = ((r or {}).get("result") or {}).get("changes") or {}
        check(mathy_uri in changes,
              "lsp spice: rename edits the defining file")
        user_edits = changes.get(user_uri) or []
        shadow_line = caret(user_src, "let [double-it")["line"]
        check(len(user_edits) == 1,
              f"lsp spice: rename edits exactly the one real use "
              f"(got {len(user_edits)})")
        check(all(e["range"]["start"]["line"] != shadow_line for e in user_edits),
              "lsp spice: the sibling's own shadowing `let` is left alone")

        # prepareRename from a USE in another file refuses rather than
        # producing an edit anchored on a definition it is not looking at.
        # Refusing with a reason is half the feature -- it is the reason c2mp
        # lists rename as not covered.
        r = srv.call("textDocument/prepareRename",
                     {"textDocument": {"uri": user_uri},
                      "position": caret(user_src, "(double-it 3)", 1)})
        msg = str(((r or {}).get("error") or {}).get("message", ""))
        check("another file" in msg,
              f"lsp spice: renaming from a use in another file refuses "
              f"with a reason (got {r!r})")

        # And the caret sitting on a local resolves to the local, so
        # prepareRename offers exactly its range rather than refusing.
        r = srv.call("textDocument/prepareRename",
                     {"textDocument": {"uri": user_uri},
                      "position": caret(user_src, "let [double-it", 5)})
        res = (r or {}).get("result") or {}
        check(res.get("placeholder") == "double-it",
              f"lsp spice: a local is renameable in place (got {r!r})")

        srv.call("shutdown", {})
        srv.call("exit", {}, notification=True)
    finally:
        srv.close()
        for p in (mathy_path, user_path, os.path.join(root, "build.tur")):
            try:
                os.unlink(p)
            except OSError:
                pass
        try:
            os.rmdir(src_dir)
            os.rmdir(root)
        except OSError:
            pass


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def main() -> int:
    if not os.path.isfile(TUR) or not os.access(TUR, os.X_OK):
        print(f"SKIP: {TUR} not built", file=sys.stderr)
        return 0
    test_mcp()
    test_mcp_repeated_analysis()
    test_lsp()
    test_lsp_encoding_and_deferred_analysis()
    test_lsp_client_gaps()
    test_lsp_unprimed_completion()
    test_lsp_unsaved_buffer()
    test_lsp_inside_a_spice()
    print(f"\nResults: {PASS} passed, {FAIL} failed")
    return 0 if FAIL == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
