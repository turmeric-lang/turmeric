#!/usr/bin/env python3
"""check-extern-c-widths -- a stdlib `extern-c` must not declare a 64-bit
return for a runtime function that returns a narrower C type.

`(extern-c tur_hamt_count [m :ptr] :int)` emits
`extern int64_t tur_hamt_count(void *)`; the runtime returned `uint32_t`.
Natively (LP64) the narrow value sits zero-extended in a 64-bit register and
nothing notices.  On wasm32 the caller and callee disagree on the wasm
signature (i64 against i32): through the browser JIT's exact-signature call
stubs that is a `function signature mismatch` trap, and under `tur build
--target wasm` it is whatever the engine makes of it.  Shape 3 of
docs/reported/ilp32-word-slots-truncate-64-bit-carriers.md.

The check reads every `(extern-c NAME [...] :RET)` in stdlib/*.tur, finds
NAME's prototype in src/runtime/*.h and src/async/*.h, and fails when RET is
`:int` or `:ptr` and the C return type is a 32-bit-or-narrower integer
(`int`, `unsigned`, `uint32_t`, `size_t`, ...).  `:bool` against a C `int`
is not flagged: both are i32 on wasm and the bool read is sound.  A
runtime function the headers do not declare (an inline-C-only helper) is
skipped.  Fix the runtime's return type to `int64_t` (or declare the
Turmeric side at the real width); do not allowlist.

Usage: python3 tests/check-extern-c-widths.py   (exit 1 on a finding)
"""
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NARROW = {'int', 'unsigned', 'unsigned int', 'uint32_t', 'int32_t', 'size_t',
          'ssize_t', 'uint16_t', 'int16_t', 'uint8_t', 'int8_t', 'short',
          'unsigned short', 'char', 'unsigned char', 'float', 'long',
          'unsigned long'}
PROTO = re.compile(r'^\s*((?:const\s+)?(?:unsigned\s+|signed\s+)?[A-Za-z_]\w*(?:\s+\w+)?)\s*(\**)\s*\b(tur_[a-z0-9_]+)\s*\(', re.M)
EXTERN = re.compile(r'\(extern-c\s+([a-z0-9_]+)\s*\[[^\]]*\]\s*(?:#fx\{[^}]*\}\s*)?:([a-z]+)')


def main():
    protos = {}
    for h in sorted(glob.glob(os.path.join(ROOT, 'src/runtime/*.h')) +
                    glob.glob(os.path.join(ROOT, 'src/async/*.h'))):
        text = open(h).read()
        for m in PROTO.finditer(text):
            ret = m.group(1).replace('static', '').replace('inline', '').strip()
            if m.group(2):          # a pointer return is 32 bits on wasm32, but so is a :ptr
                ret = ret + ' *'
            protos.setdefault(m.group(3), (ret, os.path.relpath(h, ROOT)))
    findings = []
    checked = 0
    for t in sorted(glob.glob(os.path.join(ROOT, 'stdlib/*.tur'))):
        for m in EXTERN.finditer(open(t).read()):
            name, ret = m.group(1), m.group(2)
            if name not in protos:
                continue
            checked += 1
            cret, h = protos[name]
            if ret in ('int', 'ptr') and cret in NARROW:
                findings.append('%s: (extern-c %s ... :%s) but %s returns %s (%s)'
                                % (os.path.relpath(t, ROOT), name, ret, name, cret, h))
    for f in findings:
        print('FAIL check-extern-c-widths: ' + f)
    if findings:
        return 1
    print('PASS check-extern-c-widths (%d extern-c declarations against %d runtime prototypes)'
          % (checked, len(protos)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
