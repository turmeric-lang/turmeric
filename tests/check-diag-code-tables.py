#!/usr/bin/env python3
"""tests/check-diag-code-tables.py

Guard for docs/archive/diag-code-tables-drift-without-a-check.md.

`src/compiler/diag.c` keeps the DiagCode enum's members in three hand-kept
tables: code -> string (`diag_code_to_string`), string -> code
(`diag_code_from_string`) and the long-form text `tur explain` prints
(`diag_explanations_`).  Nothing tied them to the enum or to each other, so a
code added to the enum and to one table was a code `tur explain` could not
find, or a diagnostic that printed a bare `error []`.

Every property is checked from the source text, so this needs no build:

  1. Every enum member except DIAG_CODE_NONE has a `case` in
     `diag_code_to_string`, and the string is the one its name spells
     (TUR_E0020_... -> "TUR-E0020").  No code is spelled twice.
  2. `diag_code_to_string` has no `default:` arm, so -Wswitch makes a member
     added without a case a build failure on every compiler that warns.
  3. Every enum member has a line in `diag_code_from_string` that parses that
     same string back to it, and no line parses a string to a different member.
  4. Every enum member has an entry in `diag_explanations_`, or is listed in
     tests/diag-explain-missing.txt.  That list may only shrink: a listed code
     that has gained text, or that is no longer in the enum, fails here, so
     the file cannot rot into a second table nobody reads.

Usage: python3 tests/check-diag-code-tables.py
"""
import os
import re
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), '..'))
DIAG_H = os.path.join(ROOT, 'src', 'compiler', 'diag.h')
DIAG_C = os.path.join(ROOT, 'src', 'compiler', 'diag.c')
MISSING = os.path.join(ROOT, 'tests', 'diag-explain-missing.txt')

errors = []


def fail(msg):
    errors.append(msg)


def strip_comments(text):
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    return re.sub(r'//[^\n]*', '', text)


def read(path):
    with open(path, encoding='utf-8') as f:
        return f.read()


def body_after(text, header, what):
    """The text from `header` to the line that closes it (a `}` in column 0)."""
    i = text.find(header)
    if i < 0:
        fail(f'{what}: could not find `{header}` -- update this check')
        return ''
    m = re.search(r'^\}', text[i:], flags=re.M)
    return text[i:i + m.end()] if m else text[i:]


def enum_members():
    h = read(DIAG_H)
    m = re.search(r'typedef enum DiagCode \{(.*?)\} DiagCode;', h, flags=re.S)
    if not m:
        fail('diag.h: could not find `typedef enum DiagCode` -- update this check')
        return []
    names = []
    for item in strip_comments(m.group(1)).split(','):
        item = item.strip()
        if not item:
            continue
        name = item.split('=')[0].strip()
        if not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', name):
            fail(f'diag.h: cannot read enum item `{item}`')
            continue
        names.append(name)
    return names


def spelled(name):
    """The string a member's name spells, or None for a name that spells none."""
    m = re.match(r'TUR_([EWDI][0-9]{4})_', name)
    return f'TUR-{m.group(1)}' if m else None


def main():
    members = [n for n in enum_members() if n != 'DIAG_CODE_NONE']
    if len(members) < 100:
        fail(f'diag.h: only {len(members)} enum members read -- the parse is broken')
    seen = set()
    for n in members:
        if n in seen:
            fail(f'diag.h: enum member {n} appears twice')
        seen.add(n)

    c = strip_comments(read(DIAG_C))

    # 1 + 2: code -> string
    to_body = body_after(c, 'const char *diag_code_to_string(DiagCode code) {',
                         'diag_code_to_string')
    if re.search(r'\bdefault\s*:', to_body):
        fail('diag_code_to_string has a `default:` arm: -Wswitch cannot flag a '
             'member added without a case.  List DIAG_CODE_NONE instead.')
    to_str = {}
    for name, s in re.findall(r'case\s+(\w+)\s*:\s*return\s+"([^"]*)"\s*;', to_body):
        if name in to_str:
            fail(f'diag_code_to_string: {name} has two cases')
        to_str[name] = s
    spelling_owner = {}
    for name in members:
        want = spelled(name)
        if want is None:
            fail(f'{name}: the name does not spell a code (TUR_<E|W|D|I>NNNN_...)')
            continue
        got = to_str.get(name)
        if got is None:
            fail(f'{name}: no case in diag_code_to_string (it prints a bare `error []`)')
        elif got != want:
            fail(f'{name}: diag_code_to_string returns "{got}", its name spells "{want}"')
        if want in spelling_owner and spelling_owner[want] != name:
            fail(f'{want} is spelled by both {spelling_owner[want]} and {name}')
        spelling_owner[want] = name
    for name in to_str:
        if name not in seen and name != 'DIAG_CODE_NONE':
            fail(f'diag_code_to_string: case for {name}, which is not in the enum')

    # 3: string -> code
    from_body = body_after(c, 'DiagCode diag_code_from_string(const char *s) {',
                           'diag_code_from_string')
    from_str = {}
    for s, name in re.findall(r'strcmp\(s,\s*"([^"]+)"\)\s*==\s*0\)\s*return\s+(\w+)\s*;',
                              from_body):
        if s in from_str:
            fail(f'diag_code_from_string: "{s}" is parsed twice')
        from_str[s] = name
    for name in members:
        want = spelled(name)
        if want is None:
            continue
        got = from_str.get(want)
        if got is None:
            fail(f'{name}: diag_code_from_string does not parse "{want}" '
                 f'(`tur explain {want}` cannot find it)')
        elif got != name:
            fail(f'diag_code_from_string: "{want}" parses to {got}, not {name}')
    for s, name in from_str.items():
        if name not in seen:
            fail(f'diag_code_from_string: "{s}" parses to {name}, which is not in the enum')

    # 4: explain text
    ex_m = re.search(r'diag_explanations_\[\]\s*=\s*\{(.*?)^\};', c, flags=re.S | re.M)
    if not ex_m:
        fail('diag.c: could not find the diag_explanations_ table -- update this check')
        explained = set()
    else:
        explained = set(re.findall(r'^\s*\{\s*(TUR_\w+)\s*,', ex_m.group(1), flags=re.M))
    for name in explained:
        if name not in seen:
            fail(f'diag_explanations_: entry for {name}, which is not in the enum')

    allow = []
    if os.path.exists(MISSING):
        for line in read(MISSING).splitlines():
            line = line.split('#')[0].strip()
            if line:
                allow.append(line)
    allow_set = set(allow)
    if len(allow) != len(allow_set):
        fail(f'{os.path.relpath(MISSING, ROOT)}: a code is listed twice')
    for name in members:
        if name not in explained and name not in allow_set:
            fail(f'{name}: no `tur explain` text.  Add an entry to diag_explanations_ '
                 f'(diag.c); only a code that cannot be explained yet may go in '
                 f'tests/diag-explain-missing.txt')
    for name in allow_set:
        if name not in seen:
            fail(f'tests/diag-explain-missing.txt: {name} is not in the enum any more '
                 f'-- delete the line')
        elif name in explained:
            fail(f'tests/diag-explain-missing.txt: {name} now has explain text '
                 f'-- delete the line')

    if errors:
        for e in errors:
            print(f'FAIL check-diag-code-tables: {e}')
        print(f'check-diag-code-tables: {len(errors)} problem(s)')
        return 1
    print(f'PASS check-diag-code-tables ({len(members)} codes: all round-trip; '
          f'{len(members) - len(allow_set)} explained, {len(allow_set)} listed as not yet)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
