#!/usr/bin/env python3
"""check_turi_native_parity.py -- ratchet for the turi interpreter preload gap.

Prereq 3a of docs/archive/history/turi-open-reports-prereqs.md.  Companion to
check_turi_parity.py: where that script ratchets the *expression-kind* parity
(every EX_* the compiler emits has a case arm in eval.c), this one ratchets the
*module preload* parity that drives the allowlist->denylist harness flip
(docs/archive/history/turi-harness-flip-reconciliation.md).

The compiled path auto-loads a fixed set of stdlib modules so every compiled
program sees the typed-collection / typeclass / macro definitions for free
(`autoload_files_[]`, src/compiler/stdlib_autoload.c).  The `--interpret`/
`tur eval` path preloads its own prelude (`prelude[]` in
turi_env_preload_collections, src/turi/preload.c).  Any module in the compiled
set but NOT in the interpreter prelude is a *preload gap*: every public name it
defines (`map-count`, `mutmap-set!`, `json/decode`, ...) is an "unknown
function / unbound variable" under --interpret while resolving fine when
compiled.  That gap is precisely the harness-flip "missing native" bucket.

This script:
  1. Parses both module lists out of those two files.
  2. Computes GAP = compiled-auto-load - interpreter-prelude.
  3. Requires every GAP module to carry a one-line rationale in
     docs/artifacts/turi-preload-carve-out.txt.  An undocumented gap fails the build
     (someone widened the compiled auto-load without teaching the interpreter,
     or without consciously carving it out).
  4. A carve-out entry for a module that is NO LONGER a gap (now preloaded), or
     that names a module the compiled path does not auto-load, is stale and
     also fails -- keeping the list honest, same discipline as
     docs/artifacts/turi-carve-out.txt.
  5. With --worklist, prints the public names each gap module would contribute
     to the interpreter once preloaded (the concrete preload work list).

Exit status: 0 on parity, 1 on any violation.

Usage:
  python3 tools/check_turi_native_parity.py
  python3 tools/check_turi_native_parity.py --quiet      # print only on failure
  python3 tools/check_turi_native_parity.py --worklist   # also list the gap names
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# The compiled-side autoload list moved out of main.c so the WASM
# playground's analyzer can share it (try-turmeric-lsp-plan, L0).
AUTOLOAD_C = os.path.join(ROOT, "src", "compiler", "stdlib_autoload.c")
# The interpreter prelude array was relocated from cmd_eval (main.c) into the
# shared preload helper so the native --interpret path and the WASM REPL load
# one list (web-repl-missing-stdlib-preload).
PRELOAD_C = os.path.join(ROOT, "src", "turi", "preload.c")
STDLIB = os.path.join(ROOT, "stdlib")
CARVE_OUT = os.path.join(ROOT, "docs", "artifacts", "turi-preload-carve-out.txt")


def read(path):
    with open(path, "r", encoding="utf-8") as f:
        return f.read()


def _strip_c_comments(text):
    return re.sub(r"/\*.*?\*/", "", text, flags=re.S)


def _module_array(src, header_regex):
    """Return the list of "<name>.tur" basenames inside the first C array whose
    declaration matches header_regex.  Block comments are stripped first so a
    commented-out entry (e.g. the disabled "gen.tur") never counts."""
    m = re.search(header_regex + r"\s*=\s*\{(.*?)\};", src, re.S)
    if not m:
        sys.exit("check_turi_native_parity: could not find array via %r" % header_regex)
    body = _strip_c_comments(m.group(1))
    return re.findall(r'"([a-z0-9_-]+\.tur)"', body)


def compiled_autoload():
    src = read(AUTOLOAD_C)
    return _module_array(src, r"static const char\s*\*const autoload_files_\[\]")


def interpreter_prelude():
    """The interpreter preload set.  The shared helper (src/turi/preload.c,
    called by both cmd_eval and the WASM REPL) loads macros.tur first, then the
    `prelude[]` array (which itself ends with sym.tur), plus contract.tur in the
    macros pass.  macros.tur and contract.tur both export macros that must
    register before the user file is read, so they are loaded outside the array
    (turi_env_preload_macros); they are counted explicitly here."""
    src = read(PRELOAD_C)
    base = _module_array(src, r"static const char\s*\*prelude\[\]")
    return ["macros.tur"] + base + ["contract.tur"]


# Top-level definer forms whose name becomes a public stdlib binding.
_DEFINER = re.compile(
    r"^\(\s*(?:defn|defmacro|defstruct|definstance|defopaque|def)\s+(?:\^[\w-]+\s+)*([^\s\[\(\]^][^\s\[\(\]]*)"
)


def module_public_names(basename):
    """Public top-level names a module contributes (best-effort, for --worklist).
    Matches column-0 definer forms; sweet-exp / indented bodies are skipped --
    this is an informational work list, not the ratchet itself."""
    path = os.path.join(STDLIB, basename)
    if not os.path.isfile(path):
        return []
    names = []
    for line in read(path).splitlines():
        m = _DEFINER.match(line)
        if m and m.group(1) not in ("Eq",):  # typeclass re-decls are not new names
            names.append(m.group(1))
    return names


def carve_outs():
    """Map of carved-out module -> rationale from docs/artifacts/turi-preload-carve-out.txt.
    Format: one `module.tur -- rationale` per line; blank / `#` lines ignored."""
    if not os.path.isfile(CARVE_OUT):
        return {}
    out = {}
    for raw in read(CARVE_OUT).splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        m = re.match(r"(\S+\.tur)\s*--\s*(.+)", line)
        if not m:
            sys.exit("check_turi_native_parity: malformed carve-out line: %r" % raw)
        out[m.group(1)] = m.group(2).strip()
    return out


def main(argv):
    quiet = "--quiet" in argv
    worklist = "--worklist" in argv

    compiled = compiled_autoload()
    prelude = set(interpreter_prelude())
    gap = [m for m in compiled if m not in prelude]
    carved = carve_outs()

    errors = []

    # (3) every gap module must be documented.
    undocumented = [m for m in gap if m not in carved]
    for m in undocumented:
        errors.append(
            "preload gap %r is not in the interpreter prelude and has no "
            "carve-out rationale in docs/artifacts/turi-preload-carve-out.txt" % m
        )

    # (4) stale carve-outs: entry for a module that is no longer a gap.
    gapset = set(gap)
    compiledset = set(compiled)
    for m in carved:
        if m not in compiledset:
            errors.append(
                "stale carve-out %r: the compiled path no longer auto-loads it" % m
            )
        elif m not in gapset:
            errors.append(
                "stale carve-out %r: it is now in the interpreter prelude "
                "(remove it from docs/artifacts/turi-preload-carve-out.txt)" % m
            )

    if errors:
        print("turi native-parity check FAILED:", file=sys.stderr)
        for e in errors:
            print("  - " + e, file=sys.stderr)
        return 1

    if not quiet:
        print(
            "turi native-parity OK: %d auto-loaded modules, %d preload gaps "
            "(all carved): %s"
            % (len(compiled), len(gap), ", ".join(gap) if gap else "(none)")
        )
        if worklist:
            for m in gap:
                names = module_public_names(m)
                print("  %s -- %s" % (m, carved.get(m, "")))
                if names:
                    print("      preload would add: " + " ".join(names))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
