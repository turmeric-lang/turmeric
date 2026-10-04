#!/usr/bin/env python3
"""Does the LSP get files OTHER than the open document right?

Two defects, both about the scratch copy the server compiles in place of the
document (src/lsp/lsp.c run_doc_analysis):

  * lsp-publishes-other-files-diagnostics-under-one-uri: a `load`ed or
    imported file's error was published under the open document's URI at the
    OTHER file's line and column, told apart only by a non-standard "file"
    key.  It is now anchored on the form that names the file, prefixed with
    the real `path:line:col`, and carries the real location as
    relatedInformation.

  * lsp-sibling-import-resolves-against-scratch-dir: resolution relative to
    the entry file's directory -- `(import sibling)`, `#use-reader-macros
    "x.tur"` -- started from the temp directory the scratch copy sits in, so
    the editor reported `module 'greeter' not found` for a file `tur check`
    compiles cleanly.  (`load` is cwd-relative by design and is unaffected.)

THE CONTROL IS LOAD-BEARING (see saffron-diagnostics.py): a session that exits
before analysis publishes nothing, which looks exactly like a clean file, so
an own-file error must report at its own coordinates first.

Usage: cross-file-diagnostics.py [path-to-tur]   ($TUR, else build/tur[.exe])
"""
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from jsonrpc_probe import (drain, frame, iter_frames, settle,  # noqa: E402
                           start_reader)

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
WAIT_SECONDS = float(os.environ.get("TUR_LSP_SETTLE", "30"))


def find_tur():
    if len(sys.argv) > 1:
        return os.path.abspath(sys.argv[1])
    env = os.environ.get("TUR")
    if env:
        return os.path.abspath(env)
    for name in ("tur", "tur.exe"):
        cand = os.path.join(ROOT, "build", name)
        if os.path.exists(cand):
            return cand
    return os.path.join(ROOT, "build", "tur")


TUR = find_tur()


def session(path, text, cwd):
    """Open `text` as `path` with the server started in `cwd`; return the last
    published diagnostic set, or None when nothing was published."""
    env = dict(os.environ, TUR_NO_AUTO_SPICE="1")
    p = subprocess.Popen([TUR, "lsp"], stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                         cwd=cwd, env=env)
    buf, reader = start_reader(p)
    uri = "file://" + path
    for msg in ({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                 "params": {"processId": None, "rootUri": None,
                            "capabilities": {}}},
                {"jsonrpc": "2.0", "method": "initialized", "params": {}},
                {"jsonrpc": "2.0", "method": "textDocument/didOpen",
                 "params": {"textDocument": {"uri": uri,
                                             "languageId": "turmeric",
                                             "version": 1, "text": text}}}):
        p.stdin.write(frame(msg))
        p.stdin.flush()
    settle(p, uri)
    try:
        p.stdin.write(frame({"jsonrpc": "2.0", "id": 2, "method": "shutdown",
                             "params": {}}))
        p.stdin.write(frame({"jsonrpc": "2.0", "method": "exit", "params": {}}))
        p.stdin.flush()
    except Exception:
        pass
    drain(p, reader, WAIT_SECONDS)
    published = [obj["params"].get("diagnostics", [])
                 for obj in iter_frames(bytes(buf))
                 if obj.get("method") == "textDocument/publishDiagnostics"]
    return published[-1] if published else None


ok = True


def report(passed, label, detail):
    global ok
    print("%s %-22s: %s" % ("ok  " if passed else "FAIL", label, detail))
    ok = ok and passed


def write(path, text):
    with open(path, "w") as f:
        f.write(text)


work = os.path.realpath(tempfile.mkdtemp(prefix="tur-lsp-xfile-"))
try:
    # -- control: an error in the document itself, at its own coordinates ----
    own = os.path.join(work, "own.tur")
    text = "(defn main [] : int (undefined-own-fn 1))\n"
    last = session(own, text, work)
    if not last:
        report(False, "control", "an undefined function reported nothing "
               "(%r) -- the probe measures nothing" % (last,))
    else:
        d = last[0]
        want = {"line": 0, "character": text.index("undefined-own-fn")}
        report(d["range"]["start"] == want
               and "relatedInformation" not in d,
               "control", "own error at its own position (%r)"
               % (d["range"]["start"],))

    # -- a loaded file's error lands on the `load` that names it -------------
    lib = os.path.join(work, "lib.tur")
    write(lib, "(defn helper [] : int (undefined-lib-fn 1))\n")
    entry = os.path.join(work, "entry.tur")
    text = ';; header\n(load "%s")\n(defn main [] : int 0)\n' % lib
    last = session(entry, text, work) or []
    foreign = [d for d in last if "undefined-lib-fn" in d.get("message", "")]
    if not foreign:
        report(False, "load anchor", "no diagnostic for lib.tur (%r)" % (last,))
    else:
        d = foreign[0]
        col = text.splitlines()[1].index('"')
        rel = (d.get("relatedInformation") or [{}])[0].get("location", {})
        report(d["range"]["start"] == {"line": 1, "character": col}
               and d["message"].startswith("in lib.tur:1:")
               and rel.get("uri", "").endswith("/lib.tur")
               and rel.get("range", {}).get("start", {}).get("line") == 0,
               "load anchor", "range %r, message %r, related %r"
               % (d["range"]["start"], d["message"][:40], rel.get("uri", "")[-12:]))

    # -- a sibling module resolves, and its error lands on the import --------
    greeter = os.path.join(work, "greeter.tur")
    write(greeter, "(defmodule greeter\n  (export greet)\n"
                   "  (defn greet [] : int (no-such-call 1)))\n")
    main_mod = os.path.join(work, "main_mod.tur")
    text = ("(defmodule main-mod\n  (export)\n"
            "  (import greeter :refer [greet])\n"
            "  (defn main [] : int (greet)))\n")
    # Started from a DIFFERENT directory, so only entry-relative resolution
    # can find the sibling -- as `tur check work/main_mod.tur` from `/` does.
    last = session(main_mod, text, "/") or []
    missing = [d for d in last if "not found" in d.get("message", "")]
    foreign = [d for d in last if "no-such-call" in d.get("message", "")]
    report(not missing, "sibling import",
           "resolved" if not missing else missing[0]["message"].splitlines()[0])
    if foreign:
        want = {"line": 2, "character": text.splitlines()[2].index("greeter")}
        report(foreign[0]["range"]["start"] == want,
               "import anchor", "range %r" % (foreign[0]["range"]["start"],))
    else:
        report(False, "import anchor", "no diagnostic for greeter.tur (%r)"
               % ([d.get("message", "")[:40] for d in last],))

    # -- a file loaded BY a loaded file lands on the document's `load` --------
    # The document never names deep.tur, so the anchor is the `load` of the
    # file that does (the origin chain, diag_set_file_origin).
    deep = os.path.join(work, "deep.tur")
    write(deep, "(defn deep-helper [] : int (undefined-deep-fn 1))\n")
    mid = os.path.join(work, "mid.tur")
    write(mid, ';; mid\n(load "%s")\n(defn mid-helper [] : int 1)\n' % deep)
    top = os.path.join(work, "top.tur")
    text = ';; header\n;; more\n(load "%s")\n(defn main [] : int 0)\n' % mid
    last = session(top, text, work) or []
    foreign = [d for d in last if "undefined-deep-fn" in d.get("message", "")]
    if not foreign:
        report(False, "transitive load", "no diagnostic for deep.tur (%r)"
               % (last,))
    else:
        d = foreign[0]
        col = text.splitlines()[2].index('"')
        rel = (d.get("relatedInformation") or [{}])[0].get("location", {})
        report(d["range"]["start"] == {"line": 2, "character": col}
               and d["message"].startswith("in deep.tur:1:")
               and "(via mid.tur)" in d["message"]
               and rel.get("uri", "").endswith("/deep.tur"),
               "transitive load", "range %r, message %r, related %r"
               % (d["range"]["start"], d["message"][:50],
                  rel.get("uri", "")[-12:]))

    # -- the same through modules: entry imports outer, outer imports inner --
    write(os.path.join(work, "inner-mod.tur"),
          "(defmodule inner-mod\n  (export inner)\n"
          "  (defn inner [] : int (no-such-inner 1)))\n")
    write(os.path.join(work, "outer-mod.tur"),
          "(defmodule outer-mod\n  (export outer)\n"
          "  (import inner-mod :refer [inner])\n"
          "  (defn outer [] : int (inner)))\n")
    text = ("(defmodule chain-main\n  (export)\n"
            "  (import outer-mod :refer [outer])\n"
            "  (defn main [] : int (outer)))\n")
    last = session(os.path.join(work, "chain_main.tur"), text, "/") or []
    foreign = [d for d in last if "no-such-inner" in d.get("message", "")]
    if foreign:
        want = {"line": 2, "character": text.splitlines()[2].index("outer-mod")}
        report(foreign[0]["range"]["start"] == want
               and "(via outer-mod.tur)" in foreign[0]["message"],
               "transitive import", "range %r, message %r"
               % (foreign[0]["range"]["start"], foreign[0]["message"][:60]))
    else:
        report(False, "transitive import", "no diagnostic for inner-mod.tur "
               "(%r)" % ([d.get("message", "")[:40] for d in last],))

    # -- an error inside a macro's expansion lands on the call ---------------
    # The error is located in the DEFMACRO's file -- here the auto-loaded
    # stdlib/map.tur, which the document never names, so there is no `load`
    # to anchor on.  The call the user wrote is the right place anyway.
    text = ("#lang saffron\n"
            "(defn main [] : int\n"
            "  (let [m (:: (map-new) (Map Sym int))]\n"
            "    (println (map-get (map-assoc m \"k\" 1) \"k\")))\n"
            "  0)\n")
    last = session(os.path.join(work, "mx.tur"), text, work) or []
    foreign = [d for d in last if "tur-map-kcheck" in d.get("message", "")]
    if foreign:
        d = foreign[0]
        line = text.splitlines()[3]
        rel = (d.get("relatedInformation") or [{}])[0].get("location", {})
        report(d["range"]["start"]["line"] == 3
               and d["range"]["start"]["character"] == line.index("(map-get")
               and "(expanding map-get)" in d["message"]
               and rel.get("uri", "").endswith("/stdlib/map.tur"),
               "macro call anchor", "range %r, message %r, related %r"
               % (d["range"]["start"], d["message"][:70],
                  rel.get("uri", "")[-16:]))
    else:
        report(False, "macro call anchor", "no diagnostic from map.tur (%r)"
               % ([d.get("message", "")[:40] for d in last],))

    # -- loading a stdlib file explicitly brings none of its own lint --------
    # TUR-W0039 skipped the auto-loaded stdlib only; arrow.tur's deliberate
    # free `arr` / `>>>` fallbacks warned on every `(load "stdlib/arrow.tur")`,
    # which the LSP then drew on the user's `load` line.
    text = '(load "stdlib/arrow.tur")\n(defn main [] : int 0)\n'
    last = session(os.path.join(work, "arrows.tur"), text, ROOT)
    report(last == [], "explicit stdlib load",
           "clean" if last == [] else "%r" % ([d.get("message", "")[:60]
                                                for d in (last or [])],))

    # -- a sibling reader-macro file resolves the same way --------------------
    rm_src = os.path.join(ROOT, "tests", "fixtures", "reader-macros-use")
    shutil.copy(os.path.join(rm_src, "macros.tur"), work)
    with open(os.path.join(rm_src, "input.tur")) as f:
        text = f.read()
    last = session(os.path.join(work, "rm.tur"), text, "/")
    report(last == [], "sibling reader macros",
           "clean" if last == [] else "%r" % ([d.get("message", "")[:60]
                                                for d in (last or [])],))
finally:
    shutil.rmtree(work, ignore_errors=True)

sys.exit(0 if ok else 1)
