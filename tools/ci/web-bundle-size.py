#!/usr/bin/env python3
"""
tools/ci/web-bundle-size.py -- what the Try Turmeric bundle weighs
(performance-audit-plan WP5, W-1).

Reports, raw and compressed (gzip -9 always; brotli -q 11 when the `brotli`
module or CLI is available):

  - turmeric.wasm and turmeric.js, the module both page workers load;
  - turmeric-jit.wasm and turmeric-jit.js, the browser JIT module the page
    loads only for the Compile engine (wasm-jit-plan P4);
  - stdlib.data, the stdlib package both modules fetch (wasm-jit-plan 5.11:
    it used to be embedded in each module's data section);
  - turmeric.wasm by section: code, data and the rest;
  - the stdlib tree the package is made from, and stdlib/docstrings.tur on its
    own (W-1: it is read only on a doc lookup);
  - web/dist/client, the whole production build, and its largest files.

Usage:
    python3 tools/ci/web-bundle-size.py [--web web] [--stdlib stdlib] [--json out.json]

Writes a markdown summary to stdout, and also to $GITHUB_STEP_SUMMARY when set.
Missing inputs are reported as missing, not as an error: the step is
informational.
"""

import argparse
import gzip
import json
import os
import shutil
import subprocess
import sys

WASM_SECTIONS = {0: "custom", 1: "type", 2: "import", 3: "function", 4: "table",
                 5: "memory", 6: "global", 7: "export", 8: "start", 9: "element",
                 10: "code", 11: "data", 12: "datacount", 13: "tag"}


def brotli_size(data):
    try:
        import brotli  # type: ignore
        return len(brotli.compress(data, quality=11))
    except ImportError:
        pass
    exe = shutil.which("brotli")
    if not exe:
        return None
    r = subprocess.run([exe, "-c", "-q", "11", "-"], input=data, capture_output=True)
    return len(r.stdout) if r.returncode == 0 else None


def sizes(data):
    return {"raw": len(data), "gzip": len(gzip.compress(data, 9)), "brotli": brotli_size(data)}


def leb128(data, i):
    n = shift = 0
    while True:
        b = data[i]
        i += 1
        n |= (b & 0x7F) << shift
        shift += 7
        if not b & 0x80:
            return n, i


def wasm_sections(data):
    """Bytes per section kind; custom sections keyed by their name."""
    if data[:4] != b"\0asm":
        return {}
    out = {}
    i = 8
    while i < len(data):
        sid = data[i]
        size, i = leb128(data, i + 1)
        key = WASM_SECTIONS.get(sid, f"id{sid}")
        if sid == 0:
            nlen, j = leb128(data, i)
            key = "custom:" + data[j:j + nlen].decode("utf-8", "replace")
        out[key] = out.get(key, 0) + size
        i += size
    return out


def tree_bytes(root):
    """(total bytes, file count, concatenated bytes) of every file under root."""
    total = count = 0
    chunks = []
    for d, _, files in os.walk(root):
        for f in sorted(files):
            p = os.path.join(d, f)
            b = open(p, "rb").read()
            total += len(b)
            count += 1
            chunks.append(b)
    return total, count, b"".join(chunks)


def fmt(n):
    if n is None:
        return "-"
    return f"{n / 1e6:.2f} MB" if n >= 1e5 else f"{n / 1e3:.1f} KB"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--web", default="web")
    ap.add_argument("--stdlib", default="stdlib")
    ap.add_argument("--json", default="")
    args = ap.parse_args()

    report = {"files": {}, "wasm_sections": {}, "dist": {}}
    for name in ("turmeric.wasm", "turmeric.js", "turmeric-jit.wasm", "turmeric-jit.js",
                 "stdlib.data"):
        p = os.path.join(args.web, "public", name)
        if os.path.isfile(p):
            data = open(p, "rb").read()
            report["files"][name] = sizes(data)
            if name == "turmeric.wasm":
                report["wasm_sections"] = wasm_sections(data)
        else:
            report["files"][name] = None

    if os.path.isdir(args.stdlib):
        total, count, blob = tree_bytes(args.stdlib)
        report["files"]["stdlib/ (packaged as stdlib.data)"] = dict(sizes(blob), files=count)
        doc = os.path.join(args.stdlib, "docstrings.tur")
        if os.path.isfile(doc):
            report["files"]["stdlib/docstrings.tur"] = sizes(open(doc, "rb").read())

    dist = os.path.join(args.web, "dist", "client")
    if os.path.isdir(dist):
        files = []
        for d, _, fs in os.walk(dist):
            for f in fs:
                p = os.path.join(d, f)
                data = open(p, "rb").read()
                files.append((os.path.relpath(p, dist).replace(os.sep, "/"), len(data),
                              len(gzip.compress(data, 9))))
        report["dist"] = {
            "files": len(files),
            "raw": sum(f[1] for f in files),
            "gzip": sum(f[2] for f in files),
            "largest": [{"path": p, "raw": r, "gzip": g}
                        for p, r, g in sorted(files, key=lambda f: -f[1])[:12]],
        }

    lines = ["### Try Turmeric bundle (performance-audit WP5)", "",
             "| file | raw | gzip -9 | brotli -q 11 |", "|---|--:|--:|--:|"]
    for name, s in report["files"].items():
        if s is None:
            lines.append(f"| `{name}` | missing | | |")
        else:
            extra = f" ({s['files']} files)" if "files" in s else ""
            lines.append(f"| `{name}`{extra} | {fmt(s['raw'])} | {fmt(s['gzip'])} | {fmt(s['brotli'])} |")
    if report["wasm_sections"]:
        secs = report["wasm_sections"]
        lines += ["", "| turmeric.wasm section | bytes |", "|---|--:|"]
        for k, v in sorted(secs.items(), key=lambda kv: -kv[1]):
            lines.append(f"| {k} | {fmt(v)} |")
    if report["dist"]:
        d = report["dist"]
        lines += ["", f"`web/dist/client`: {d['files']} files, {fmt(d['raw'])} raw, {fmt(d['gzip'])} gzip.",
                  "", "| largest in dist/client | raw | gzip -9 |", "|---|--:|--:|"]
        for f in d["largest"]:
            lines.append(f"| `{f['path']}` | {fmt(f['raw'])} | {fmt(f['gzip'])} |")
    text = "\n".join(lines) + "\n"
    sys.stdout.write(text)
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a") as fh:
            fh.write(text + "\n")
    if args.json:
        with open(args.json, "w") as fh:
            json.dump(report, fh, indent=2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
