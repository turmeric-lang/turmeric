#!/usr/bin/env python3
"""check-driver-headers.py -- each section of a sectioned header declares only
what the file it names defines.

src/driver/driver.h and src/driver/commands.h are split into sections headed

    /* == driver/paths.c ===================================== */

and every function prototype and `extern` variable in a section must be
defined in that file (the path is relative to src/).  A prototype outside any
section is an error too.  Types and macros are not checked.  The section
grouping in emit_internal.h drifted until most of its prototypes sat under the
wrong file (main-c-split-plan); this keeps the driver's headers from starting
down that road.  Usage: check-driver-headers.py [header ...]
"""
import re, sys
from pathlib import Path

SRC = Path(__file__).resolve().parent.parent / "src"
SECTION = re.compile(r"^/\* == (\S+) =+ \*/$")
IDENT = re.compile(r"[A-Za-z_]\w*")


def strip(text):
    """Blank comments, string/char literals and preprocessor lines; keep lines."""
    text = re.sub(r"/\*.*?\*/|//[^\n]*|\"(?:\\.|[^\"\\\n])*\"|'(?:\\.|[^'\\\n])*'",
                  lambda m: re.sub(r"[^\n]", " ", m.group(0)), text, flags=re.S)
    return re.sub(r"(?m)^[ \t]*#(?:[^\n]*\\\n)*[^\n]*", "", text)


def statements(text):
    """Yield (head, ends_with_brace) for each depth-0 statement, skipping bodies."""
    depth, head = 0, ""
    for ch in text:
        if depth:
            depth += {"{": 1, "}": -1}.get(ch, 0)
            continue
        if ch == "{":
            yield head, True
            depth, head = 1, ""
        elif ch == ";":
            yield head, False
            head = ""
        else:
            head += ch


def declared_name(head):
    """(name, is_function) for a declaration or definition head, else None."""
    head = " ".join(head.split())
    if not head or re.match(r"(typedef|struct|enum|union)\b", head) and "(" not in head:
        return None
    m = re.match(r"[^=(]*?\(\s*\*\s*(\w+)\s*\)", head)  # function pointer variable
    if m:
        return m.group(1), False
    pre = head.split("=")[0]
    if "(" in pre:
        names = [n for n in re.findall(r"(\w+)\s*\(", pre) if not n.startswith("__")]
        return (names[0], True) if names else None
    names = IDENT.findall(pre.split("[")[0])
    return (names[-1], False) if names else None


def definitions(path):
    defs = set()
    for head, brace in statements(strip(path.read_text())):
        flat = " ".join(head.split())
        if flat.startswith(("extern", "typedef")):
            continue
        got = declared_name(head)
        if got and (got[1] == brace or not got[1]):
            defs.add(got[0])
    return defs


def check(header):
    errors, section, defs = [], None, set()
    raw = header.read_text().split("\n")
    stripped = strip(header.read_text()).split("\n")
    pending = ""
    for lineno, (line, code) in enumerate(zip(raw, stripped), 1):
        m = SECTION.match(line.strip())
        if m:
            section = m.group(1)
            target = SRC / section
            defs = definitions(target) if target.exists() else None
            if defs is None:
                errors.append(f"{header}:{lineno}: section names missing file {section}")
            continue
        pending += code + "\n"
        if ";" not in code or pending.count("{") != pending.count("}"):
            continue
        for head, brace in statements(pending):
            flat = " ".join(head.split())
            if brace or not flat or flat.startswith("typedef") or "}" in flat:
                continue
            got = declared_name(head)
            if not got or (not got[1] and not flat.startswith("extern")):
                continue
            if section is None:
                errors.append(f"{header}:{lineno}: {got[0]} is outside any section")
            elif defs is not None and got[0] not in defs:
                errors.append(f"{header}:{lineno}: {got[0]} is not defined in {section}")
        pending = ""
    return errors


def main(argv):
    headers = [Path(a) for a in argv] or [SRC / "driver/driver.h", SRC / "driver/commands.h"]
    errors = [e for h in headers for e in check(h)]
    for e in errors:
        print(f"FAIL {e}")
    if not errors:
        print(f"PASS check-driver-headers ({len(headers)} headers)")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
