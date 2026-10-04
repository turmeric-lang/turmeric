#!/usr/bin/env python3
"""Does the LSP analyse -- and format -- a `#lang r7rs` buffer correctly?

r7rs-lang-plan R9: "the LSP (which 2.6 suggests works by inheritance --
measure it, do not assume it)".  Measured 2026-09-24:

  * DIAGNOSTICS work by inheritance, as they did for Saffron: the server
    compiles the buffer from a temp file and `#lang` detection is content
    based, so a Scheme buffer is lowered, gets the prelude, and a valid one
    publishes nothing.
  * FORMATTING did not: textDocument/formatting handed the whole buffer,
    directive included, to the reader, got a parse error, and answered "no
    edits" -- for every `#lang` document, Saffron's included.  It now runs the
    same document formatter as `tur fmt` (fmt_format_document), which for
    Scheme re-indents and never reprints a token.

THE CONTROL IS LOAD-BEARING (see saffron-diagnostics.py): a session that
exits before analysis publishes nothing, which looks exactly like a clean
file.  So a BROKEN buffer must report, in both dialects, before a clean one is
believed.

Usage: r7rs-diagnostics.py [path-to-tur]   ($TUR, else build/tur[.exe])
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from jsonrpc_probe import (drain, frame, iter_frames, settle,  # noqa: E402
                           start_reader)

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
# How long to allow the server to finish and be read, not a settle time; see
# jsonrpc_probe. $TUR_LSP_SETTLE still raises it for a loaded box.
WAIT_SECONDS = float(os.environ.get("TUR_LSP_SETTLE", "30"))


def find_tur():
    if len(sys.argv) > 1:
        return sys.argv[1]
    env = os.environ.get("TUR")
    if env:
        return env
    for name in ("tur", "tur.exe"):
        cand = os.path.join(ROOT, "build", name)
        if os.path.exists(cand):
            return cand
    return os.path.join(ROOT, "build", "tur")


TUR = find_tur()


def session(text, path, format_it=False):
    """Open `text` as `path`; return (published diagnostic sets, formatting result)."""
    p = subprocess.Popen([TUR, "lsp"], stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    buf, reader = start_reader(p)
    uri = "file://" + path
    msgs = [{"jsonrpc": "2.0", "id": 1, "method": "initialize",
             "params": {"processId": None, "rootUri": None, "capabilities": {}}},
            {"jsonrpc": "2.0", "method": "initialized", "params": {}},
            {"jsonrpc": "2.0", "method": "textDocument/didOpen",
             "params": {"textDocument": {"uri": uri, "languageId": "turmeric",
                                         "version": 1, "text": text}}}]
    if format_it:
        msgs.append({"jsonrpc": "2.0", "id": 7, "method": "textDocument/formatting",
                     "params": {"textDocument": {"uri": uri},
                                "options": {"tabSize": 2, "insertSpaces": True}}})
    for msg in msgs:
        p.stdin.write(frame(msg))
        p.stdin.flush()

    # Sent after the formatting request above (id 7) and before shutdown/exit.
    # The server handles one message at a time, so this orders the flush -- and
    # the publishDiagnostics it writes -- after formatting and before the exit.
    # on_formatting does NOT flush dirty docs, and neither does on_shutdown; only
    # the symbol-reading handlers do, which is why this request has to be here.
    settle(p, uri)
    try:
        p.stdin.write(frame({"jsonrpc": "2.0", "id": 2, "method": "shutdown", "params": {}}))
        p.stdin.write(frame({"jsonrpc": "2.0", "method": "exit", "params": {}}))
        p.stdin.flush()
    except Exception:
        pass
    drain(p, reader, WAIT_SECONDS)

    published, formatting = [], "no-response"
    for obj in iter_frames(bytes(buf)):
        if obj.get("method") == "textDocument/publishDiagnostics":
            published.append(obj["params"].get("diagnostics", []))
        elif obj.get("id") == 7:
            formatting = obj.get("result")
    return published, formatting


R7RS_OK = (
    "#lang r7rs\n"
    "(import (scheme base) (scheme write))\n"
    "(define (add a b) (+ a b))\n"
    "(define v (vector 1 #\\x \"s\" #t))\n"
    "(display (add 1.5 2.25))\n"
    "(newline)\n"
    "(write (car (quote (a b))))\n"
)
R7RS_BROKEN = (
    "#lang r7rs\n"
    "(import (scheme base) (scheme write))\n"
    "(display (undefined-fn-xyz 1))\n"
)
TURMERIC_BROKEN = (
    "(defn main [] : int\n"
    "  (println (undefined-fn-xyz 1))\n"
    "  0)\n"
)
# Mis-indented, and full of lexemes the form printer would rewrite.
R7RS_UNFORMATTED = (
    "#lang r7rs\n"
    "(define (f x)\n"
    "        (if x #t #f))\n"
    "(write '(#\\a |two words| ,x #u8(1 2)))\n"
)
R7RS_FORMATTED = (
    "#lang r7rs\n"
    "(define (f x)\n"
    "  (if x #t #f))\n"
    "(write '(#\\a |two words| ,x #u8(1 2)))\n"
)

ok = True

for name, text in (("turmeric", TURMERIC_BROKEN), ("r7rs", R7RS_BROKEN)):
    pub, _ = session(text, "/tmp/tur_lsp_r7rs_broken_%s.tur" % name)
    last = pub[-1] if pub else []
    if not last:
        print("FAIL control %-8s: a buffer with an undefined function reported "
              "NO diagnostics (%d publish(es)) -- the probe measures nothing"
              % (name, len(pub)))
        ok = False
    else:
        print("ok   control %-8s: reports %r" % (name, last[0].get("message", "")[:60]))

pub, _ = session(R7RS_OK, "/tmp/tur_lsp_r7rs_ok.tur")
last = pub[-1] if pub else []
if not pub:
    print("FAIL r7rs    : no diagnostics were published at all -- analysis did not run")
    ok = False
elif last:
    print("FAIL r7rs    : a VALID Scheme buffer reported %d diagnostic(s): %s"
          % (len(last), "; ".join(d.get("message", "")[:70] for d in last[:3])))
    ok = False
else:
    print("ok   r7rs    : valid Scheme buffer, no diagnostics")

_, fmt = session(R7RS_UNFORMATTED, "/tmp/tur_lsp_r7rs_fmt.tur", format_it=True)
if not isinstance(fmt, list) or not fmt:
    print("FAIL format  : textDocument/formatting returned %r, not an edit" % (fmt,))
    ok = False
elif fmt[0].get("newText") != R7RS_FORMATTED:
    print("FAIL format  : got %r" % (fmt[0].get("newText"),))
    ok = False
else:
    print("ok   format  : re-indented, every Scheme lexeme kept")

# lsp-ignores-the-file-extension: the extension alone selects the reader (and
# for `.scm` the language), as it does for the compiler.  The server analysed
# a scratch copy named `*.tur`, so a headerless `.scm` / `.tur.sweet` buffer
# was read as plain Turmeric.  The `.tur` row is the control: the same Scheme
# body with no header and no telling extension MUST still report, or the clean
# rows prove nothing.
SCHEME_HEADERLESS = "(define (f x) (* x 2))\n"
SWEET_HEADERLESS = "defn double [x]\n  {x * 2}\n"
for label, text, path, want_clean in (
        ("ext .tur ", SCHEME_HEADERLESS, "/tmp/tur_lsp_ext_ctl.tur", False),
        ("ext .scm ", SCHEME_HEADERLESS, "/tmp/tur_lsp_ext.scm", True),
        ("ext sweet", SWEET_HEADERLESS, "/tmp/tur_lsp_ext.tur.sweet", True)):
    pub, _ = session(text, path)
    last = pub[-1] if pub else None
    if last is None:
        print("FAIL %s: no diagnostics were published at all" % label)
        ok = False
    elif want_clean and last:
        print("FAIL %s: headerless buffer read under the wrong reader: %s"
              % (label, "; ".join(d.get("message", "")[:60] for d in last[:3])))
        ok = False
    elif not want_clean and not last:
        print("FAIL %s: control reported nothing -- the probe measures nothing"
              % label)
        ok = False
    else:
        print("ok   %s: %s" % (label, "clean" if want_clean else
                                "control reports %r" % last[0].get("message", "")[:50]))

sys.exit(0 if ok else 1)
