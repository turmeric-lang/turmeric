#!/usr/bin/env python3
"""hoist-includes.py -- apply the driver's __tur_include__ hoist to emitted C.

`tur build` scans the C it generated for `/* __tur_include__: LINE */` markers
and prepends each LINE at file scope (hoist_tur_include_directives in
src/driver/cc.c): preprocessor directives first, then code payloads such as
httpd's `HttpdConn` typedef, each bucket in source order, behind a
`#define _DEFAULT_SOURCE 1`.  `tur emit-c` prints the C before that step, so a
fuzz harness that #includes emitted C for a module with markers
(stdlib/httpd.tur) needs the same step applied.  This is that step, and
nothing else.

usage: hoist-includes.py <in.c> <out.c>
"""
import sys

MARK = "/* __tur_include__: "
DIRECTIVES = ("#include", "#define", "#undef", "#pragma")


def main(argv):
    src = open(argv[1], encoding="utf-8", errors="surrogateescape").read()
    directives, code = [], []
    pos = 0
    while True:
        i = src.find(MARK, pos)
        if i < 0:
            break
        i += len(MARK)
        end = src.find(" */", i)
        if end < 0:
            break
        payload = src[i:end]
        (directives if payload.lstrip(" \t").startswith(DIRECTIVES) else code).append(payload)
        pos = end + 3
    with open(argv[2], "w", encoding="utf-8", errors="surrogateescape") as out:
        if directives or code:
            out.write("#define _DEFAULT_SOURCE 1\n")
            for line in directives + code:
                out.write(line + "\n")
        out.write(src)


if __name__ == "__main__":
    main(sys.argv)
