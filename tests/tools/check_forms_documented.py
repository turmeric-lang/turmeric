#!/usr/bin/env python3
"""
tests/tools/check_forms_documented.py -- every special form and compiler
builtin is documented, or is on the list of ones not documented yet.

The compiler implements ~160 names itself: the reserved special forms
(`reserved_special_forms_`, src/compiler/elab_call.c) and the builtin
functions (`table_`, src/compiler/builtins.c).  None has a definition for a
`;;;` docstring to sit on, so for a long time none had an API page or a
`(doc ...)` entry (docs/archive/api-docs-miss-language-primitives.md).  They
are documented in stdlib/forms/ with doc-only declarations
(`;;@form` / `;;@builtin`, read by tools/gendocs.py).

This check keeps that from drifting:

  - a reserved form or builtin with no documentation must be listed in
    tests/forms-undocumented.txt -- so a NEW form cannot arrive undocumented
    without someone writing its name down;
  - a listed name that is documented, or that the compiler no longer has,
    fails too -- so the list only shrinks, and stays true;
  - a `;;@form` / `;;@builtin` must name something the compiler knows, so a
    typo cannot document a form that does not exist.

Also pins how gendocs reads the declarations.  Needs no `tur`.
"""
import os
import re
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, 'tools'))

from gendocs import (collect_tur_files, parse_tur_file, _render_signature,  # noqa: E402
                     _build_doc_entry)

ALLOWLIST = os.path.join(ROOT, 'tests', 'forms-undocumented.txt')
fails = []


def read(rel):
    with open(os.path.join(ROOT, rel), encoding='utf-8') as f:
        return f.read()


def c_string_list(src, start_marker):
    """The string literals of a C array initializer, comments stripped."""
    body = src[src.index(start_marker):]
    body = re.sub(r'/\*.*?\*/', '', body[:body.index('};')], flags=re.S)
    return re.findall(r'"((?:[^"\\]|\\.)*)"', body)


# --- what the compiler implements ------------------------------------------

# Non-ASCII spellings (the Greek-letter lambda alias of `fn`) are documented
# under their ASCII form.
reserved = [n for n in c_string_list(read('src/compiler/elab_call.c'),
                                     'reserved_special_forms_[] = {')
            if '\\x' not in n]
builtins = sorted({n for n in re.findall(r'^\s*\{ *"([^"]+)", *NULL',
                                         read('src/compiler/builtins.c'), re.M)
                   if not n.startswith(('__', 'tur_'))})
required = set(reserved) | set(builtins)
# Every name the elaborator interns is one it can dispatch on, so a doc-only
# declaration may document any of them (e.g. callback-ptr, which is not
# reserved), not only the required set.
known = required | set(re.findall(r'intern_cstr\(st, "([^"]+)"\)',
                                  read('src/compiler/elab_core.c')))

# --- what is documented -----------------------------------------------------

documented, declared = set(), []
for path in collect_tur_files(os.path.join(ROOT, 'stdlib')):
    if path.name == 'docstrings.tur':
        continue
    for d in parse_tur_file(path)['definitions']:
        if d['docstring']:
            documented.add(d['name'])
        if d['kind'] in ('form', 'builtin'):
            declared.append((os.path.relpath(path, ROOT), d['name']))

with open(ALLOWLIST, encoding='utf-8') as f:
    allowed = [ln.strip() for ln in f
               if ln.strip() and not ln.lstrip().startswith('#')]

# --- the checks -------------------------------------------------------------

for name in sorted(required - documented - set(allowed)):
    fails.append(f"`{name}` is a special form or builtin with no documentation: "
                 f"document it in stdlib/forms/ (a `;;;` block over a `;;@form` or "
                 f"`;;@builtin` line), or add it to tests/forms-undocumented.txt")
for name in allowed:
    if name in documented:
        fails.append(f"`{name}` is documented now -- delete it from "
                     f"tests/forms-undocumented.txt")
    elif name not in required:
        fails.append(f"`{name}` in tests/forms-undocumented.txt is not a reserved "
                     f"form or builtin -- delete it")
dupes = {n for n in allowed if allowed.count(n) > 1}
for name in sorted(dupes):
    fails.append(f"`{name}` is listed twice in tests/forms-undocumented.txt")
for path, name in declared:
    if name not in known:
        fails.append(f"{path}: `;;@` declaration of `{name}`, which the compiler "
                     f"does not know (a typo?)")

# --- the declaration parser -------------------------------------------------

SRC = """;;; tur/forms/demo -- a demo page.
;;
;;; ptr-add -- advance a pointer.
;;;
;;; The offset is in bytes.
;;;
;;; Example:
;;;   (ptr-add p 8)
;;@builtin (ptr-add [p : ptr<void> n : int] : ptr<void>)

;;; not= -- inequality.
;;@builtin (not= [a : int b : int] : bool)

;;; unsafe -- run unsafe code.
;;@form (unsafe body ...)
;;@form (unsafe [Eff ...] body ...)
;;@form (handle expr clause body ...)
"""
with tempfile.TemporaryDirectory() as td:
    p = os.path.join(td, 'demo.tur')
    with open(p, 'w') as f:
        f.write(SRC)
    mod = parse_tur_file(p)
defs = {d['name']: d for d in mod['definitions']}
for ok, what in [
    (mod['docstring'] is not None, "the module docstring is promoted"),
    (set(defs) == {'ptr-add', 'not=', 'unsafe', 'handle'},
     f"declared names: {sorted(defs)}"),
    (_render_signature(defs['ptr-add']) == '(ptr-add [p :ptr<void> n :int] :ptr<void>)',
     f"builtin signature: {_render_signature(defs['ptr-add'])!r}"),
    (_render_signature(defs['not=']) == '(not= [a :int b :int] :bool)',
     "a name outside the defn pattern is kept whole"),
    (_render_signature(defs['unsafe']) == '(unsafe body ...)\n(unsafe [Eff ...] body ...)',
     "consecutive shapes of one form join"),
    (defs['handle']['docstring'] is None,
     "a shape line for another name starts a new, undocumented definition"),
    (_build_doc_entry(defs['unsafe']).split('\n')[2:4] ==
     ['(unsafe body ...)', '(unsafe [Eff ...] body ...)'],
     "the doc-lookup entry carries the syntax"),
    (_build_doc_entry(defs['ptr-add']).split('\n')[2:5] ==
     ['(ptr-add [p :ptr<void> n :int] :ptr<void>)', '', 'The offset is in bytes.'],
     "the doc-lookup entry carries the prose under the signature"),
]:
    if not ok:
        fails.append(f"gendocs declaration parser: {what}")

for f in fails:
    print(f"FAIL check-forms-documented -- {f}")
if fails:
    sys.exit(1)
print(f"PASS check-forms-documented ({len(required)} forms and builtins: "
      f"{len(required & documented)} documented, {len(allowed)} listed as not yet)")
