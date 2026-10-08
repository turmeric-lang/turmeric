#!/usr/bin/env python3
"""
tools/gendocs.py -- Generate HTML API documentation from Turmeric stdlib files.

Usage:
    # Generate all docs
    python3 tools/gendocs.py stdlib/ --out docs/html/api/

    # Generate single module
    python3 tools/gendocs.py stdlib/list.tur --out docs/html/api/

    # Also emit docstrings.tur (for runtime (doc name) lookup)
    python3 tools/gendocs.py stdlib/ --out docs/html/api/ --emit-tur stdlib/docstrings.tur
"""

import argparse
import os
import re
import sys
import html as html_module
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import packlib  # noqa: E402  (sibling module, path fixed up just above)
# The site chrome -- nav list, sidebar groups, tooltips -- lives in genguides
# so guides, API docs and the spices site show the same links in the same order
# and describe the same page the same way.
from genguides import (build_page_header, build_sidebar,  # noqa: E402
                       SIDEBAR_DRAWER_JS_SRC, font_links, script_tag,
                       sidebar_drawer, write_page_scripts)


# ---------------------------------------------------------------------------
# Parser
# ---------------------------------------------------------------------------

def _parse_params(bracket_content):
    """
    Parse a parameter list like '[value :int next :int]' or '[& clauses]'.
    Returns a list of (name, type_ann) tuples.  Type annotations are optional.

    UT3: ^-annotations (^unique, ^linear, ^mut) that precede a parameter
    are captured and prepended to the parameter name so they appear in the
    rendered signature, e.g. '^unique v' instead of 'v'.
    """
    inner = bracket_content.strip()
    if inner.startswith('['):
        inner = inner[1:]
    if inner.endswith(']'):
        inner = inner[:-1]
    tokens = _split_sig_tokens(inner)
    params = []
    i = 0
    pending_anns = []  # UT3: accumulated ^-annotations for the next param
    while i < len(tokens):
        tok = tokens[i]
        if tok == ':':
            # gendocs-misparses-the-spaced-annotation-form: the SPACED form
            # `a : int` puts the colon in its own token.  It is an infix
            # marker joining the previous name to the next token, so attach
            # that token as the type -- spelled fused (':int') so both
            # spellings take the same downstream path.  A trailing bare ':'
            # with nothing after it is dropped.
            if params and i + 1 < len(tokens):
                params[-1] = _with_type(params[-1], ':' + tokens[i + 1])
            i += 2
            continue
        if tok == '&':
            i += 1
            if i < len(tokens):
                name = tokens[i]
                if pending_anns:
                    name = ' '.join(pending_anns) + ' ' + name
                    pending_anns = []
                params.append((name, '...'))
            i += 1
            continue
        if tok.startswith('^'):
            # UT3: ^-annotation precedes the parameter it qualifies
            pending_anns.append(tok)
            i += 1
            continue
        if tok.startswith(':') or tok.startswith('ptr<'):
            # This is a type annotation for the previous param.
            # Note: bare 'ptr' without '<' is a valid field/param NAME in Turmeric,
            # so we only treat 'ptr<...' (with the angle bracket) as a type token.
            if params:
                params[-1] = _with_type(params[-1], tok)
            i += 1
            continue
        # Regular parameter name -- apply any pending annotations
        name = tok
        if pending_anns:
            name = ' '.join(pending_anns) + ' ' + name
            pending_anns = []
        params.append((name, None))
        i += 1
    return params


def _with_type(param, type_tok):
    """
    Attach a type token to a parsed (name, type) pair.  A rest parameter
    (`& rest :cstr`) keeps its '...' marker and carries the element type
    after it ('... :cstr'), so the renderer can show both the `&` and the
    type rather than one or the other.
    """
    name, cur = param
    if cur and cur.startswith('...'):
        return (name, '... ' + type_tok)
    return (name, type_tok)


def _split_sig_tokens(text):
    """
    Split a signature fragment on whitespace, keeping a balanced `(...)`,
    `[...]` or `<...>` group as ONE token, so a compound type such as
    `(Option int)`, `(fn [int] int)` or `ptr<Foo>` is not broken into a
    phantom parameter per word.
    """
    tokens = []
    cur = []
    depth = 0
    for ch in text:
        if ch in '([<':
            depth += 1
        elif ch in ')]>':
            depth = max(depth - 1, 0)
        if ch.isspace() and depth == 0:
            if cur:
                tokens.append(''.join(cur))
                cur = []
            continue
        cur.append(ch)
    if cur:
        tokens.append(''.join(cur))
    return tokens


# An effect row between the parameter vector and the return type:
#   (defn f [x : int] #fx{Unsafe} : int ...)
_FX_ROW_RE = re.compile(r'^\s*#fx\{[^}]*\}')


def _return_type_after_bracket(rest):
    """
    The return type that follows a closing `]`, in either spelling:
    fused `] :int`, spaced `] : int`, or a bare `ptr<...>`.  An `#fx{...}`
    effect row before it is skipped.  None when the body starts right away.
    """
    rest = _FX_ROW_RE.sub('', rest, count=1)
    parts = _split_sig_tokens(rest.strip())
    if not parts:
        return None
    if parts[0] == ':':
        return ':' + parts[1] if len(parts) > 1 else None
    if parts[0].startswith(':') or parts[0].startswith('ptr<'):
        return parts[0]
    return None


def _find_bracket(text):
    """
    Locate the first `[...]` vector in text, honouring nesting so that a
    field type like `(fn [int] int)` inside the vector does not end it
    early.  Returns (content, end_index) or (None, -1).
    """
    start = text.find('[')
    if start == -1:
        return None, -1
    depth = 0
    for i in range(start, len(text)):
        ch = text[i]
        if ch == '[':
            depth += 1
        elif ch == ']':
            depth -= 1
            if depth == 0:
                return text[start + 1:i], i + 1
    return None, -1


def _extract_return_type(line):
    """
    Extract return type from a defn signature line.
    Looks for a ':type' / ': type' / 'ptr<...>' token after the closing bracket.
    """
    # After the closing ] there may be a return type token
    bracket_end = line.rfind(']')
    if bracket_end == -1:
        return None
    return _return_type_after_bracket(line[bracket_end + 1:])


def _parse_docstring(lines):
    """
    Parse a list of ';;; ...' lines into structured sections.
    Returns a dict:
      {
        'summary': str,
        'params': [(name, desc), ...],
        'returns': str,
        'example': str,
        'since': str,
        'raw': str,   # full text for --emit-tur
      }
    """
    if not lines:
        return None

    # Strip ';;; ' prefix
    stripped = []
    for line in lines:
        line = line.strip()
        if line == ';;;':
            stripped.append('')
        elif line.startswith(';;; '):
            stripped.append(line[4:])
        else:
            stripped.append('')

    summary = stripped[0] if stripped else ''
    # Remove leading 'name -- ' from summary for display
    # but keep the full text for raw

    sections = {'summary': summary, 'params': [], 'returns': '',
                 'example': '', 'since': '', 'deprecated': '',
                 'raw': '\n'.join(stripped)}

    current_section = None
    buf = []

    def flush():
        nonlocal buf
        text = '\n'.join(buf).strip()
        buf = []
        return text

    def _flush_section():
        if current_section == 'returns':
            sections['returns'] = flush()
        elif current_section == 'example':
            sections['example'] = flush()
        elif current_section == 'deprecated':
            sections['deprecated'] = flush()

    for line in stripped[1:]:
        if line.startswith('Parameters:'):
            _flush_section()
            current_section = 'params'
            continue
        elif line.startswith('Returns:'):
            _flush_section()
            current_section = 'returns'
            continue
        elif line.startswith('Example:'):
            _flush_section()
            current_section = 'example'
            continue
        elif line.startswith('Since:'):
            _flush_section()
            sections['since'] = line[len('Since:'):].strip()
            current_section = None
            continue
        elif line.startswith('Deprecated:'):
            _flush_section()
            current_section = 'deprecated'
            rest = line[len('Deprecated:'):].strip()
            if rest:
                buf.append(rest)
            continue

        if current_section == 'params':
            stripped_line = line.strip()
            if stripped_line:
                # Format: '  name -- description'
                m = re.match(r'^(\S+)\s+--\s+(.*)', stripped_line)
                if m:
                    sections['params'].append((m.group(1), m.group(2)))
                else:
                    sections['params'].append(('', stripped_line))
        elif current_section == 'returns':
            buf.append(line)
        elif current_section == 'example':
            buf.append(line)
        elif current_section == 'deprecated':
            buf.append(line)

    _flush_section()

    return sections


def strip_line_comment(line):
    """Return `line` with any `;` comment removed, respecting string literals.

    Used when scanning source for forms: a form written inside a comment (a
    docstring example, a commented-out declaration) is prose, not code, and
    must not be read as the real thing.
    """
    out = []
    in_str = False
    i = 0
    while i < len(line):
        c = line[i]
        if in_str:
            if c == '\\':
                # Copy the escape AND what it escapes: a `\"` keeps the string
                # open, so the `;` after it is still string content, and the
                # text this returns has to stay verbatim for any other caller.
                out.append(line[i:i + 2])
                i += 2
                continue
            if c == '"':
                in_str = False
        elif c == '"':
            in_str = True
        elif c == ';':
            break
        out.append(c)
        i += 1
    return ''.join(out)


def apply_path_fallback_name(module, rel_parts):
    """Refine a filename-derived module name with the file's subdirectory.

    Only applies when the file declares no `(defmodule ...)`: a declared name is
    the module's identity and its page URL, and neither is ours to rewrite.

    The bare-stem fallback is keyed on the *basename*, so any two same-named
    files anywhere under the source root resolve to one module name and one
    page -- whichever renders last wins and the other silently does not exist.
    That is not hypothetical: `stdlib/capability.tur` and
    `stdlib/test/capability.tur` both became `tur/capability`, and the shipped
    `tur-capability.html` held the *test* mocks while the real module had no
    page. Everything under a subdirectory is a same-named sibling waiting to
    happen, so the fallback carries the subdirectory:

        stdlib/capability.tur       -> tur/capability
        stdlib/test/capability.tur  -> tur/test/capability
        stdlib/seq/core.tur         -> tur/seq/core
    """
    if module.get('name_declared'):
        return
    if not rel_parts or len(rel_parts) < 2:
        return
    stem = Path(rel_parts[-1]).stem
    module['name'] = 'tur/' + '/'.join([*rel_parts[:-1], stem])


def parse_tur_file(path):
    """
    Parse a .tur file and return a module description dict:
    {
        'name': 'tur/list',   # or None
        'file_stem': 'list',
        'file_path': path,
        'exports': set(),
        'definitions': [
            {
                'kind': 'defn' | 'defmacro' | 'defstruct' | 'definstance',
                'name': str,
                'params': [(name, type), ...],
                'return_type': str | None,
                'exported': bool,
                'docstring': { ... } | None,
                'line': int,
            },
            ...
        ],
    }
    """
    path = Path(path)
    with open(path, encoding='utf-8', errors='replace') as f:
        raw = f.read()

    lines = raw.splitlines()
    file_stem = path.stem  # e.g. 'list' from 'list.tur'

    module = {
        'name': None,
        'name_declared': False,
        'file_stem': file_stem,
        'file_path': str(path),
        'exports': set(),
        'definitions': [],
        'docstring': None,
    }

    # ------------------------------------------------------------------
    # Step 1: Find (defmodule tur/name ...)
    #
    # Scan code only.  A `defmodule` written inside a `;;` comment -- as an
    # example in a module docstring, which is exactly where one gets written --
    # used to win, because it appears before the real form.  That is how
    # stdlib/turi/eval.tur came to publish itself as `myplugin/core`: its
    # docstring shows `(defmodule myplugin/core ...)` at line 5 and its own
    # declaration sits at line 20.  The result was a bogus myplugin-core.html
    # on the site and no page at all for turi/eval.
    # ------------------------------------------------------------------
    module_re = re.compile(r'\(\s*defmodule\s+([\w/\-]+)')
    # r7rs-lang-plan R9: a Scheme library names itself `(define-library (a b))`,
    # which the compiler maps to the module `a/b`.
    library_re = re.compile(r'\(\s*define-library\s+\(([^()]*)\)')
    for line in lines:
        code = strip_line_comment(line)
        m = module_re.search(code)
        if m:
            module['name'] = m.group(1)
            module['name_declared'] = True
            break
        m = library_re.search(code)
        if m and m.group(1).split():
            module['name'] = '/'.join(m.group(1).split())
            module['name_declared'] = True
            break

    if module['name'] is None:
        # Derive a pseudo-name from the filename.  Callers that know the file's
        # position under the source root refine this via apply_path_fallback_name
        # -- the bare stem collides across subdirectories.
        module['name'] = 'tur/' + file_stem

    # ------------------------------------------------------------------
    # Step 2: Find (export ...) possibly spanning multiple lines
    # ------------------------------------------------------------------
    export_re = re.compile(r'\(\s*export\s+')
    in_export = False
    paren_depth = 0
    export_text = ''

    for line in lines:
        if not in_export:
            if export_re.search(line):
                in_export = True
                # Start from the 'export' keyword
                idx = export_re.search(line).start()
                export_text = line[idx:]
                paren_depth = export_text.count('(') - export_text.count(')')
                if paren_depth <= 0:
                    in_export = False
                    # Done
                    _extract_exports(export_text, module)
        else:
            export_text += ' ' + line
            paren_depth += line.count('(') - line.count(')')
            if paren_depth <= 0:
                in_export = False
                _extract_exports(export_text, module)

    # ------------------------------------------------------------------
    # Step 3: Scan definitions with docstrings
    # ------------------------------------------------------------------
    doc_buf = []  # accumulated ';;;' lines
    def_re = re.compile(
        r'^\s*\(\s*(defn|defmacro|defstruct|defdata|defgadt|definstance|defopaque|defeffect)\s+'
    )
    # r7rs-lang-plan R9: the Scheme definition forms, documented as the
    # Turmeric kinds they correspond to.  Parameters are untyped, like a
    # Saffron defn's: ('x', None).
    scheme_def_re = re.compile(
        r'^\s*\(\s*(define-record-type|define-syntax|define)\s+'
    )

    pending_module_doc = None   # most recently flushed ;;; block before first def
    first_def_seen = False

    i = 0
    while i < len(lines):
        line = lines[i]
        stripped = line.strip()

        if stripped.startswith(';;;'):
            doc_buf.append(stripped)
            i += 1
            continue

        sm = scheme_def_re.match(line)
        if sm:
            if not first_def_seen:
                if module['docstring'] is None and pending_module_doc:
                    module['docstring'] = _parse_docstring(pending_module_doc)
                    pending_module_doc = None
                first_def_seen = True
            # Enough following lines to close the header, which may wrap.
            def_text = line
            j = i + 1
            while def_text.count('(') - def_text.count(')') > 1 and j < len(lines) and j < i + 8:
                def_text += ' ' + lines[j].strip()
                j += 1
            parsed = _parse_scheme_def(sm.group(1), def_text)
            if parsed:
                kind, name, params = parsed
                exported = (name in module['exports']) or (not module['exports'])
                if name.startswith('__'):
                    exported = False
                module['definitions'].append({
                    'kind': kind,
                    'name': name,
                    'params': params,
                    'return_type': None,
                    'exported': exported,
                    'docstring': _parse_docstring(doc_buf) if doc_buf else None,
                    'line': i + 1,
                })
            doc_buf = []
            i += 1
            continue

        m = def_re.match(line)
        if m:
            # Promote the pending module docstring on the first real definition.
            if not first_def_seen:
                if module['docstring'] is None and pending_module_doc:
                    module['docstring'] = _parse_docstring(pending_module_doc)
                    pending_module_doc = None
                first_def_seen = True

            kind = m.group(1)
            # Parse the name + rest from this line (may span lines)
            def_text = line
            j = i + 1
            if kind in ('defdata', 'defgadt'):
                # A sum-type declaration's constructors live on following lines;
                # accumulate until the top-level parentheses balance so the
                # constructor arms are captured.
                open_parens = def_text.count('(') - def_text.count(')')
                while open_parens > 0 and j < len(lines):
                    def_text += ' ' + lines[j].strip()
                    open_parens += lines[j].count('(') - lines[j].count(')')
                    j += 1
            else:
                # Accumulate until we find the closing bracket of the params
                open_brackets = def_text.count('[') - def_text.count(']')
                while open_brackets > 0 and j < len(lines):
                    def_text += ' ' + lines[j].strip()
                    open_brackets += lines[j].count('[') - lines[j].count(']')
                    j += 1

            name, params, return_type, extra = _parse_def_line(kind, def_text)
            if name:
                exported = (name in module['exports']) or (not module['exports'])
                # Don't mark internal helpers as exported even if no export list
                if name.startswith('__'):
                    exported = False
                if name.endswith('/autolink-hint'):
                    exported = False

                docstring = _parse_docstring(doc_buf) if doc_buf else None
                defn_dict = {
                    'kind': kind,
                    'name': name,
                    'params': params,
                    'return_type': return_type,
                    'exported': exported,
                    'docstring': docstring,
                    'line': i + 1,
                }
                defn_dict.update(extra)  # LT4: merge kind-specific extras (e.g. struct_ann)
                module['definitions'].append(defn_dict)
            doc_buf = []
            i += 1
            continue

        # Non-;;; lines that reset the docstring buffer:
        # - blank lines with content that isn't a comment
        # - any non-comment, non-blank line
        # But: regular ;; comments between the ;;; block and the defn are
        # allowed (agents may place them there); do NOT reset on ;; lines.
        if stripped and not stripped.startswith(';'):
            # Snapshot as pending module docstring if no real def seen yet
            if not first_def_seen and module['docstring'] is None and pending_module_doc is None and doc_buf:
                pending_module_doc = list(doc_buf)
            doc_buf = []
        elif stripped.startswith(';;') and not stripped.startswith(';;;'):
            # A ;; (non-doc) comment after a ;;; block terminates the block.
            # Snapshot the accumulated block as the candidate module docstring
            # (before the first real definition).
            if not first_def_seen and module['docstring'] is None and pending_module_doc is None and doc_buf:
                pending_module_doc = list(doc_buf)
                doc_buf = []

        i += 1

    return module


def _parse_scheme_def(form, text):
    """r7rs-lang-plan R9: (kind, name, params) for a Scheme definition, or None.

    (define (name a b . rest) ...)            -> ('defn', name, [a, b, . rest])
    (define name value)                       -> None (a variable: no signature)
    (define-syntax name ...)                  -> ('defmacro', name, [])
    (define-record-type name (make-name f ...) ...) -> ('defstruct', name, [f, ...])
    """
    rest = re.sub(r'^\s*\(\s*' + re.escape(form) + r'\s+', '', text)
    if form == 'define':
        m = re.match(r'\(\s*([^\s()]+)([^()]*)\)', rest)
        if not m:
            return None
        toks = m.group(2).split()
        params, k = [], 0
        while k < len(toks):
            if toks[k] == '.' and k + 1 < len(toks):
                params.append(('. ' + toks[k + 1], None))
                k += 2
            else:
                params.append((toks[k], None))
                k += 1
        return ('defn', m.group(1), params)
    m = re.match(r'([^\s()]+)', rest)
    if not m:
        return None
    name = m.group(1)
    if form == 'define-syntax':
        return ('defmacro', name, [])
    ctor = re.match(r'[^\s()]+\s+\(\s*[^\s()]+([^()]*)\)', rest)
    fields = ctor.group(1).split() if ctor else []
    return ('defstruct', name, [(f, None) for f in fields])


def _extract_exports(text, module):
    """Parse '(export name1 name2 ...)' and add to module['exports']."""
    # Remove the (export and closing )
    inner = re.sub(r'^\s*\(\s*export\s+', '', text)
    inner = re.sub(r'\)\s*$', '', inner)
    for tok in inner.split():
        tok = tok.strip('()')
        if tok:
            module['exports'].add(tok)


def _parse_def_line(kind, text):
    """
    Parse a defn/defmacro/defstruct/definstance line (possibly multi-line collapsed).
    Returns (name, params, return_type, extra) where extra is a dict of kind-specific
    data (e.g. {'struct_ann': 'linear'} for defstruct with :linear annotation).
    """
    # Match kind name. defeffect may carry a leading ^private/^public visibility
    # annotation before the effect name: (defeffect ^private Ask [] :int).  A
    # defn may carry its pre-name attributes -- ^construct, ^byval, ^reflect,
    # ^deprecated ["message"] -- in any order: (defn ^construct some [A] ...).
    if kind == 'defeffect':
        caret_prefix = r'(?:\^\w+\s+)?'
    elif kind == 'defn':
        caret_prefix = r'(?:\^[\w-]+\s+(?:"[^"]*"\s+)?)*'
    else:
        caret_prefix = ''
    pattern = r'\(\s*' + kind + r'\s+' + caret_prefix + r'([\w/\-!?<>*+]+)'
    m = re.search(pattern, text)
    if not m:
        return None, [], None, {}

    name = m.group(1)
    rest = text[m.end():]

    if kind == 'definstance':
        # (definstance TypeName [TypeParam] ...)
        type_m = re.search(r'\[\s*([\w/\-!?<>*+]+)', rest)
        type_param = type_m.group(1) if type_m else ''
        return name + '[' + type_param + ']', [], None, {}

    extra = {}

    # Sum types: (defdata Name [:copy] [L R] (Left L) (Right R)) and the
    # GADT form.  Capture the :copy/:move annotation, the optional type-param
    # vector, and the constructor names so the module page can render a
    # sum-type entry.
    if kind in ('defdata', 'defgadt'):
        ann_m = re.search(r':(copy|move)\b', rest)
        if ann_m:
            extra['struct_ann'] = ann_m.group(1)
        # Optional type-param vector [L R ...] -- bare symbols, no ':' inside.
        tp_m = re.search(r'\[([^\]]*)\]', rest)
        if tp_m and ':' not in tp_m.group(1):
            tp_names = tp_m.group(1).split()
            if tp_names:
                extra['type_params'] = tp_names
        # Constructor arms: top-level (CtorName ...) groups.  The leading
        # capitalised/symbol head of each parenthesised arm is the ctor name.
        ctor_names = re.findall(r'\(\s*([\w\-!?<>*+/]+)', rest)
        if ctor_names:
            extra['ctors'] = ctor_names
        return name, [], None, extra

    # LT4: For defstruct, detect :copy / :move / :linear annotation before the bracket.
    if kind == 'defstruct':
        ann_m = re.search(r':(copy|move|linear)\s', rest)
        if ann_m:
            extra['struct_ann'] = ann_m.group(1)

    # stdlib-linearity-affinity-plan: a defopaque resource handle may carry a
    # trailing :linear / :affine discipline keyword, e.g.
    #   (defopaque TmpFile :int :linear)
    if kind == 'defopaque':
        ann_m = re.search(r':(linear|affine)\b', rest)
        if ann_m:
            extra['struct_ann'] = ann_m.group(1)

    # Extract params bracket (nesting-aware: `(fn [int] int)` as a field
    # type must not end the vector early).
    bracket_inner, bracket_end = _find_bracket(rest)
    params = []
    return_type = None
    if bracket_inner is not None:
        # Phase TM0: For defstruct, detect type-param vector [K V ...] (symbols only, no colons).
        # If present, record as type_params and the bracket is not the field list.
        if kind == 'defstruct':
            bracket_content = bracket_inner.strip()
            # A type-params list contains only bare symbol names (no ':' characters)
            if bracket_content and ':' not in bracket_content:
                tp_names = bracket_content.split()
                if tp_names:
                    extra['type_params'] = tp_names
                    # No field params to parse from this bracket
                    params = []
                    return name, params, return_type, extra
        params = _parse_params('[' + bracket_inner + ']')
        after_bracket = rest[bracket_end:]
        # Return type is the first :type / ': type' / ptr<...> token
        return_type = _return_type_after_bracket(after_bracket)

    return name, params, return_type, extra


# ---------------------------------------------------------------------------
# Doctest extraction  (Phase D0)
# ---------------------------------------------------------------------------

TESTABLE_RE = re.compile(
    r'^(-?[0-9]+\.[0-9]+|-?[0-9]+|true|false|"[^"]*"|nil)$'
)

# An explicit opt-out for an example whose output is correct as documentation
# but not reproducible in the doctest harness -- e.g. one that depends on
# stdout being a tty. Spelled as a trailing `; doctest: <reason>` on the
# `; =>` line:
#
#   ;;;   (term/bold "hello")  ; => "\x1b[1mhello\x1b[0m"  ; doctest: requires a tty
#
# Without this the case is still dropped -- TESTABLE_RE is anchored, so any
# trailing text stops it matching -- but silently and by accident. Declaring
# it keeps the reason in the docstring, where the reader of the rendered docs
# also benefits from knowing the output is environment-dependent.
DOCTEST_ANNOT_RE = re.compile(r';\s*doctest:\s*(.+?)\s*$')


class DocTestCase:
    """A single testable docstring example."""
    __slots__ = ('module_name', 'defn_name', 'setup_lines', 'expr', 'expected')

    def __init__(self, module_name, defn_name, setup_lines, expr, expected):
        self.module_name = module_name
        self.defn_name = defn_name
        self.setup_lines = setup_lines  # list[str] -- lines before this case
        self.expr = expr                # str -- the expression to evaluate
        self.expected = expected        # str -- expected println output


def extract_doctest_cases(module_name, defn_name, doc):
    """
    Extract testable examples from a parsed docstring dict.

    A line is testable when its '; =>' value matches TESTABLE_RE (integer,
    float, true/false, quoted string, or nil).  Lines without '; =>' are
    accumulated as setup statements for the following testable line and
    emitted ahead of it in the generated test file.

    Returns a list of DocTestCase objects; non-testable '; =>' lines are
    skipped silently.
    """
    if not doc or not doc.get('example'):
        return []

    lines = [l.strip() for l in doc['example'].splitlines() if l.strip()]
    cases = []
    pending_setup = []
    skipped = extract_doctest_cases.last_skipped = []

    for line in lines:
        if '; =>' not in line:
            pending_setup.append(line)
            continue
        expr_part, expected_raw = line.split('; =>', 1)
        expr_part = expr_part.strip()
        expected_raw = expected_raw.strip()
        annot = DOCTEST_ANNOT_RE.search(expected_raw)
        if annot:
            # Declared un-runnable here; not a failure and not a silent drop.
            skipped.append((defn_name, annot.group(1)))
            pending_setup = []
            continue
        if TESTABLE_RE.match(expected_raw):
            # Quoted string: strip outer quotes since println prints the raw
            # cstr value without surrounding quotes.
            if expected_raw.startswith('"') and expected_raw.endswith('"'):
                expected_out = expected_raw[1:-1]
            else:
                expected_out = expected_raw
            cases.append(DocTestCase(
                module_name=module_name,
                defn_name=defn_name,
                setup_lines=list(pending_setup),
                expr=expr_part,
                expected=expected_out,
            ))
        # Reset pending setup after each '; =>' line (testable or not)
        pending_setup = []

    return cases


# ---------------------------------------------------------------------------
# HTML Emitter
# ---------------------------------------------------------------------------

CSS = """\
/* Turmeric API Docs -- auto-generated by tools/gendocs.py */

:root {
  /* Backgrounds */
  --bg-base:       #0C0A08;
  --bg-surface:    #111009;
  --bg-panel:      #161411;
  --bg-hover:      #1C1915;

  /* Borders */
  --border:        #222018;
  --border-mid:    #2E2B22;
  --border-str:    #3D3A2F;

  /* Gold palette */
  --gold:          #D48B1C;
  --gold-bright:   #EFA030;
  --gold-dim:      #7A5010;
  --gold-subtle:   rgba(212,139,28,0.08);
  --gold-line:     rgba(212,139,28,0.25);

  /* Text */
  --text-primary:  #EAE0D2;
  --text-sec:      #8A7D6E;
  --text-dim:      #4A4438;

  /* Syntax highlight */
  --syn-kw:        #EFA030;
  --syn-str:       #D9735A;
  --syn-type:      #7AC4B8;
  --syn-num:       #A8C98A;

  /* Secondary accent -- gold leads, green answers it. Same pairing the home
     page uses, so emphasis reads the same across the whole site. */
  --green:         #A8C98A;
  --green-subtle:  rgba(168,201,138,0.10);
  --green-line:    rgba(168,201,138,0.25);
}

*, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }

html { font-size: 16px; }

body {
  font-family: 'DM Sans', system-ui, -apple-system, sans-serif;
  background: var(--bg-base);
  color: var(--text-primary);
  line-height: 1.6;
  min-height: 100vh;
}

a { color: var(--gold-bright); text-decoration: none; }
a:hover { text-decoration: underline; }

/* Emphasis is green here for the same reason it is green on the home page:
   gold is the site's primary, and prose emphasis wants the second voice. */
em { color: var(--green); font-style: italic; }

/* Header */
.site-header {
  background: var(--bg-surface);
  border-bottom: 1px solid var(--border);
  padding: 0 2rem;
  height: 60px;
  display: flex;
  align-items: center;
  gap: 1.5rem;
  position: sticky;
  top: 0;
  z-index: 200;
  backdrop-filter: blur(12px);
  -webkit-backdrop-filter: blur(12px);
}
.site-header .nav-logo {
  display: flex;
  align-items: center;
  gap: 10px;
  flex-shrink: 0;
  text-decoration: none;
}
.site-header .nav-logo:hover { text-decoration: none; }
.site-header .nav-links { display: flex; gap: 2px; font-size: 0.875rem; }
.site-header .nav-links a {
  color: var(--text-sec);
  padding: 6px 12px;
  border-radius: 6px;
  transition: color 0.12s, background 0.12s;
  white-space: nowrap;
}
.site-header .nav-links a:hover { color: var(--text-primary); background: var(--bg-hover); text-decoration: none; }
.site-header .nav-links a.active { color: var(--gold-bright); }

/* Right-hand cluster -- same GitHub / Try it pair the hand-written pages show */
.site-header .nav-right {
  margin-left: auto;
  display: flex;
  align-items: center;
  gap: 8px;
  flex-shrink: 0;
}
/* When a page has a filter box, that box takes the free space and the button
   cluster tucks in beside it instead of fighting it for the same auto margin. */
.site-header .search-wrap + .nav-right { margin-left: 0; }

.btn-ghost, .btn-gold {
  font-family: inherit;
  font-size: 0.84rem;
  padding: 7px 14px;
  border-radius: 7px;
  white-space: nowrap;
  transition: color 0.12s, background 0.12s, border-color 0.12s;
}
.btn-ghost {
  color: var(--text-sec);
  border: 1px solid var(--border-str);
  background: transparent;
}
.btn-ghost:hover { color: var(--text-primary); background: var(--bg-hover); text-decoration: none; }
.btn-gold {
  font-weight: 500;
  color: #000;
  padding: 7px 18px;
  border: 1px solid var(--gold);
  background: var(--gold);
}
.btn-gold:hover { background: var(--gold-bright); border-color: var(--gold-bright); text-decoration: none; }

/* Search */
.search-wrap { margin-left: auto; }
.search-input {
  background: var(--bg-base);
  border: 1px solid var(--border-mid);
  border-radius: 6px;
  color: var(--text-primary);
  padding: 0.3rem 0.65rem;
  font-size: 0.875rem;
  width: 220px;
  transition: border-color 0.15s, width 0.2s;
  font-family: inherit;
}
.search-input:focus { outline: none; border-color: var(--gold); width: 280px; }
.search-input::placeholder { color: var(--text-dim); }
.search-no-results {
  display: none;
  padding: 2rem;
  color: var(--text-sec);
  font-style: italic;
}

/* Layout */
.page-layout {
  display: grid;
  grid-template-columns: 240px 1fr;
  gap: 0;
  max-width: 1200px;
  margin: 0 auto;
  padding: 0;
}

/* Sidebar */
.sidebar {
  /* One face for the whole rail, set on the container so every descendant --
     headings, nav links, definition links, uplinks -- resolves to the same
     stack rather than half inheriting the body's. */
  font-family: 'DM Sans', sans-serif;
  padding: 1.5rem 1rem;
  border-right: 1px solid var(--border);
  position: sticky;
  top: 60px;
  height: calc(100vh - 60px);
  overflow-y: auto;
}
.sidebar h3 {
  font-size: 0.7rem;
  text-transform: uppercase;
  letter-spacing: 0.08em;
  color: var(--text-dim);
  margin-bottom: 0.75rem;
  margin-top: 1rem;
  font-weight: 500;
}
.sidebar h3:first-child { margin-top: 0; }
.sidebar ul { list-style: none; }
.sidebar li { margin: 0.15rem 0; }
/* Every sidebar on every turmeric surface is sans-serif -- nav labels and
   definition links alike.  A sidebar is navigation chrome, not code: the one
   face keeps the rail consistent across the main site, the guides, the API
   reference and the spices site. */
.sidebar a { font-size: 0.825rem; color: var(--text-sec); font-family: 'DM Sans', sans-serif; }
.sidebar a.sidebar-def, .sidebar code { font-family: 'DM Sans', sans-serif; }
.sidebar a:hover { color: var(--gold-bright); text-decoration: none; }
/* An ordinary link, not a button: nothing here submits or toggles, it just
   navigates like every other entry in the rail. Selector carries the `a` so it
   outranks the `.sidebar a` colour rule below it. */
.sidebar a.sidebar-back {
  display: block;
  font-size: 0.85rem;
  color: var(--text-primary);
  font-family: inherit;
  margin-bottom: 1rem;
  transition: color 0.12s;
}
.sidebar a.sidebar-back:hover { color: var(--gold-bright); text-decoration: none; }
/* "Up" links -- All Guides / All Modules / All Spices. One row, directly under
   the Home button, so every page offers the same two steps back out of itself. */
.sidebar-uplinks {
  display: flex;
  flex-wrap: wrap;
  gap: 0.25rem 0.9rem;
  margin-bottom: 1.25rem;
}
.sidebar-uplinks a { font-size: 0.8rem; color: var(--green); }
.sidebar-uplinks a:hover { color: var(--gold-bright); text-decoration: none; }
.sidebar-divider {
  border: none;
  border-top: 1px solid var(--border);
  margin: 1.25rem 0 0.5rem;
}

/* Content */
.content {
  padding: 2rem 2.5rem;
  min-width: 0;
}

/* Module heading */
.module-heading {
  margin-bottom: 2rem;
  padding-bottom: 1rem;
  border-bottom: 1px solid var(--border);
}
.module-heading h1 {
  font-size: 1.75rem;
  color: var(--gold-bright);
  font-family: 'Iosevka', 'Fira Code', monospace;
}
.module-path {
  font-size: 0.85rem;
  color: var(--text-sec);
  margin-top: 0.25rem;
}

/* Definition cards */
.def-card {
  background: var(--bg-surface);
  border: 1px solid var(--border);
  border-radius: 8px;
  padding: 1.25rem 1.5rem;
  margin-bottom: 1.25rem;
  transition: background 0.15s, border-color 0.15s;
}
.def-card:hover { background: var(--bg-hover); border-color: var(--border-mid); }
.def-card:target { border-color: var(--gold); }

.def-card-header {
  display: flex;
  align-items: baseline;
  gap: 0.75rem;
  margin-bottom: 0.75rem;
  flex-wrap: wrap;
}
.def-card-header h2 {
  font-size: 1rem;
  font-family: 'Iosevka', 'Fira Code', monospace;
  color: var(--gold-bright);
  font-weight: 600;
}

.kind-badge {
  display: inline-block;
  padding: 0.15rem 0.55rem;
  border-radius: 99px;
  font-size: 0.7rem;
  font-family: 'Iosevka', 'Fira Code', monospace;
  font-weight: 600;
  text-transform: lowercase;
}
.kind-defn      { background: rgba(212,139,28,0.15); color: var(--gold-bright); }
.kind-defmacro  { background: rgba(217,115,90,0.15); color: var(--syn-str); }
.kind-defstruct { background: rgba(168,201,138,0.12); color: var(--syn-num); }
.kind-definstance { background: rgba(122,196,184,0.12); color: var(--syn-type); }

/* LT4: badge for linear types and ^linear / lref<T> annotations */
.linear-badge {
  display: inline-block;
  padding: 0.15rem 0.55rem;
  border-radius: 99px;
  font-size: 0.7rem;
  font-family: 'Iosevka', 'Fira Code', monospace;
  font-weight: 600;
  text-transform: lowercase;
  background: rgba(80,200,232,0.12);
  color: #50C8E8;
}

.def-signature {
  font-family: 'Iosevka', 'Fira Code', monospace;
  font-size: 0.875rem;
  background: var(--bg-panel);
  border: 1px solid var(--border);
  border-radius: 6px;
  padding: 0.5rem 0.75rem;
  margin-bottom: 0.75rem;
  overflow-x: auto;
  white-space: pre;
  color: var(--text-primary);
}

.def-summary {
  color: var(--text-primary);
  margin-bottom: 0.75rem;
  font-size: 0.9375rem;
}

.def-section { margin-top: 0.75rem; }
/* Parameters / Returns / Example -- structure inside a card, so the secondary
   accent rather than the gold that marks the definition itself. */
.def-section-label {
  font-size: 0.7rem;
  text-transform: uppercase;
  letter-spacing: 0.08em;
  color: var(--green);
  margin-bottom: 0.4rem;
  font-weight: 500;
}

.param-table { border-collapse: collapse; width: 100%; font-size: 0.875rem; }
.param-table td { padding: 0.2rem 0.75rem 0.2rem 0; vertical-align: top; }
.param-table td:first-child {
  font-family: 'Iosevka', 'Fira Code', monospace;
  color: var(--gold-bright);
  white-space: nowrap;
  width: 1%;
}
.param-table td:nth-child(2) { color: var(--text-sec); white-space: nowrap; width: 1%; }

.def-returns { font-size: 0.875rem; color: var(--text-primary); }

.def-example {
  background: var(--bg-panel);
  border: 1px solid var(--border);
  border-radius: 6px;
  padding: 0.6rem 0.75rem;
  font-family: 'Iosevka', 'Fira Code', monospace;
  font-size: 0.8rem;
  overflow-x: auto;
  white-space: pre;
  color: var(--text-primary);
}

.def-since { font-size: 0.8rem; color: var(--text-sec); margin-top: 0.5rem; }

.module-doc {
  margin-bottom: 2rem;
  padding: 1rem 1.25rem;
  background: var(--bg-surface);
  border: 1px solid var(--border);
  border-left: 3px solid var(--green);
  border-radius: 6px;
}
.module-doc-summary {
  color: var(--text-primary);
  font-size: 0.95rem;
  line-height: 1.65;
}

.def-deprecated {
  margin: 0.75rem 0;
  padding: 0.5rem 0.75rem;
  border-left: 3px solid #d97706;
  background: rgba(217, 119, 6, 0.08);
  color: var(--text-primary);
  font-size: 0.9rem;
}
.def-deprecated-label {
  font-weight: 700;
  color: #d97706;
  margin-right: 0.4rem;
  text-transform: uppercase;
  font-size: 0.75rem;
  letter-spacing: 0.04em;
}

/* Internal section */
details.internal-section {
  margin-top: 2rem;
  border: 1px solid var(--border);
  border-radius: 8px;
  padding: 1rem 1.25rem;
}
details.internal-section summary {
  cursor: pointer;
  font-size: 0.85rem;
  color: var(--text-sec);
  user-select: none;
}
.internal-list { margin-top: 0.75rem; }
.internal-item {
  font-family: 'Iosevka', 'Fira Code', monospace;
  font-size: 0.8rem;
  color: var(--text-sec);
  padding: 0.2rem 0;
}
.internal-item-summary { color: var(--text-primary); margin-left: 0.5rem; font-family: inherit; }

/* Index page */
.index-grid {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(280px, 1fr));
  gap: 1rem;
  margin-top: 1.5rem;
}
.index-card {
  background: var(--bg-surface);
  border: 1px solid var(--border);
  border-radius: 8px;
  padding: 1rem 1.25rem;
  transition: background 0.15s, border-color 0.15s;
}
.index-card:hover { background: var(--bg-hover); border-color: var(--border-mid); text-decoration: none; }
.index-card h3 {
  font-family: 'Iosevka', 'Fira Code', monospace;
  font-size: 0.95rem;
  color: var(--gold-bright);
  margin-bottom: 0.35rem;
}
.index-card p { font-size: 0.85rem; color: var(--text-sec); }
.index-card .export-count { font-size: 0.75rem; color: var(--text-dim); margin-top: 0.5rem; }

/* Footer */
.site-footer {
  border-top: 1px solid var(--border);
  padding: 1.5rem 2rem;
  text-align: center;
  font-size: 0.8rem;
  color: var(--text-dim);
  margin-top: 4rem;
}
.site-footer code { color: var(--text-sec); font-family: 'Iosevka', 'Fira Code', monospace; }

/* Hamburger button */
.hamburger {
  display: none;
  background: none;
  border: none;
  cursor: pointer;
  padding: 0.25rem;
  color: var(--text-sec);
  flex-shrink: 0;
}
.hamburger span {
  display: block;
  width: 20px;
  height: 2px;
  background: var(--text-sec);
  margin: 4px 0;
  transition: background 0.15s;
}
.hamburger:hover span { background: var(--text-primary); }

/* Sidebar overlay (mobile) */
.sidebar-overlay {
  display: none;
  position: fixed;
  inset: 0;
  background: rgba(0,0,0,0.55);
  z-index: 99;
}
.sidebar-overlay.is-open { display: block; }

/* Responsive */
@media (max-width: 1080px) {
  /* Six nav links plus two buttons stop fitting before the drawer breakpoint;
     drop the buttons first -- both are reachable from the sidebar groups. */
  .site-header .nav-right { display: none; }
}
@media (max-width: 768px) {
  .hamburger { display: block; }
  .site-header { padding: 0 1rem; }
  .site-header .nav-links { display: none; }
  .search-wrap { display: none; }
  .page-layout { grid-template-columns: 1fr; }
  .sidebar {
    display: none;
    position: fixed;
    top: 60px;
    left: 0;
    width: 280px;
    height: calc(100vh - 60px);
    z-index: 100;
    background: var(--bg-surface);
    border-right: 1px solid var(--border);
    border-bottom: none;
    overflow-y: auto;
    padding: 1.5rem 1rem;
  }
  .sidebar.is-open { display: block; }
  .content { padding: 1.25rem 1rem; }
}
"""


# The drawer itself comes from genguides so the hamburger behaves identically
# on an API page, a guide and a spice; only the search filter below is specific
# to these pages. Both ship as files beside the pages (api-filter.js here), never
# inline -- see genguides.write_page_scripts.
_API_FILTER_JS_SRC = """\
  // Search filtering
  document.addEventListener('DOMContentLoaded', function(){
    var input = document.querySelector('.search-input');
    if (!input) return;

    function filter() {
      var q = input.value.trim().toLowerCase();

      // Module pages: filter .def-card elements
      var defCards = document.querySelectorAll('.def-card');
      var visibleDefs = 0;
      defCards.forEach(function(card) {
        var match = !q || card.textContent.toLowerCase().includes(q);
        card.style.display = match ? '' : 'none';
        if (match) visibleDefs++;
      });

      // Sync sidebar links with card visibility
      document.querySelectorAll('.sidebar a[href^="#"]').forEach(function(link) {
        var anchor = link.getAttribute('href').slice(1);
        var target = document.getElementById(anchor);
        link.parentElement.style.display =
          (!target || target.style.display !== 'none') ? '' : 'none';
      });

      // Index pages: filter .index-card elements
      var indexCards = document.querySelectorAll('.index-card');
      var visibleIndex = 0;
      indexCards.forEach(function(card) {
        var match = !q || card.textContent.toLowerCase().includes(q);
        card.style.display = match ? '' : 'none';
        if (match) visibleIndex++;
      });

      // Show/hide "no results" message
      var noResults = document.querySelector('.search-no-results');
      if (noResults) {
        var total = defCards.length + indexCards.length;
        var visible = visibleDefs + visibleIndex;
        noResults.style.display = (q && total > 0 && visible === 0) ? 'block' : 'none';
      }
    }

    input.addEventListener('input', filter);

    // Clear on Escape
    input.addEventListener('keydown', function(e) {
      if (e.key === 'Escape') { input.value = ''; filter(); input.blur(); }
    });

    // Focus search with '/' shortcut (when not typing elsewhere)
    document.addEventListener('keydown', function(e) {
      if (e.key === '/' && document.activeElement !== input &&
          document.activeElement.tagName !== 'INPUT' &&
          document.activeElement.tagName !== 'TEXTAREA') {
        e.preventDefault();
        input.focus();
      }
    });
  });"""

_API_PAGE_SCRIPTS = {
    'site-drawer.js': SIDEBAR_DRAWER_JS_SRC,
    'api-filter.js':  _API_FILTER_JS_SRC,
}


def _sidebar_tip(defn) -> str:
    """Tooltip for a sidebar definition link: its docstring summary if it has
    one, else a plain 'jump to' so every entry carries something useful."""
    doc = defn.get('docstring')
    if doc and doc.get('summary'):
        s = doc['summary']
        dash_idx = s.find(' -- ')
        tip = s[dash_idx + 4:] if dash_idx != -1 else s
        if tip.strip():
            return html_module.escape(tip.strip(), quote=True)
    return html_module.escape(f'Jump to {defn["name"]}', quote=True)


# Host that the chrome's site-relative links resolve against. Set by
# `render_tree`; only the spices subdomain ever makes it non-empty.
SITE_BASE = ''


def _html_header(title, css_path='style.css'):
    return f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>{html_module.escape(title)}</title>
<link rel="icon" type="image/svg+xml" href="/favicon.svg">
{font_links(indent='')}
<link rel="stylesheet" href="{css_path}">
</head>
<body>
{build_page_header(active='API Docs', base=SITE_BASE, search='Filter definitions', indent='')}
<p class="search-no-results">No matching definitions.</p>
{sidebar_drawer()}
{script_tag('api-filter.js', indent='')}
"""


def _html_footer():
    return """\
<footer class="site-footer">
  Auto-generated by <code>tools/gendocs.py</code>.
  Do not edit by hand -- run <code>just docs</code> to regenerate.
</footer>
</body>
</html>
"""


def _render_signature(defn):
    kind = defn['kind']
    name = defn['name']
    params = defn['params']
    ret = defn['return_type']

    if kind == 'defstruct':
        # LT4: include :linear / :copy / :move annotation if present
        struct_ann = defn.get('struct_ann', '')
        ann_str = f' :{struct_ann}' if struct_ann else ''
        # Phase TM0: render type params like Map[K V] when present
        type_params = defn.get('type_params', [])
        if type_params:
            tp_str = ' '.join(type_params)
            return f"(defstruct {name}{ann_str} [{tp_str}])"
        param_str = ' '.join(
            f"{p} {t}" if t else p
            for p, t in params
        )
        return f"(defstruct {name}{ann_str} [{param_str}])"

    if kind in ('defdata', 'defgadt'):
        struct_ann = defn.get('struct_ann', '')
        ann_str = f' :{struct_ann}' if struct_ann else ''
        type_params = defn.get('type_params', [])
        tp_str = f" [{' '.join(type_params)}]" if type_params else ''
        ctors = defn.get('ctors', [])
        ctor_str = ''.join(f" ({c})" for c in ctors)
        return f"({kind} {name}{ann_str}{tp_str}{ctor_str})"

    if kind == 'definstance':
        # name is like 'Functor[list]'
        return f"(definstance {name.replace('[', ' [').replace(']', ']')})"

    param_str = ' '.join(
        (f"& {p}" + t[3:] if t.startswith('...') else f"{p} {t}") if t else p
        for p, t in params
    )
    sig = f"({name}"
    if params:
        sig += f" [{param_str}]"
    if ret:
        sig += f" {ret}"
    sig += ")"
    return sig


def _render_def_card(defn, anchor_prefix=''):
    kind = defn['kind']
    name = defn['name']
    doc = defn['docstring']
    anchor_name = re.sub(r'[^a-zA-Z0-9_\-]', '_', name)
    anchor = (anchor_prefix + anchor_name) if anchor_prefix else anchor_name

    sig = _render_signature(defn)
    kind_cls = f"kind-{kind}"

    # LT4: detect linear types -- :linear struct annotation, ^linear params, lref<T> types.
    params = defn.get('params', [])
    ret = defn.get('return_type', '') or ''
    is_linear = (
        defn.get('struct_ann') == 'linear'
        or any('^linear' in (p or '') for p, _ in params)
        or any('lref<' in (t or '') for _, t in params)
        or 'lref<' in ret
    )
    is_affine = defn.get('struct_ann') == 'affine'

    h = f'<div class="def-card" id="{html_module.escape(anchor)}">\n'
    h += f'  <div class="def-card-header">\n'
    h += f'    <span class="kind-badge {kind_cls}">{kind}</span>\n'
    if is_linear:
        h += f'    <span class="linear-badge">linear</span>\n'
    elif is_affine:
        h += f'    <span class="linear-badge">affine</span>\n'
    h += f'    <h2>{html_module.escape(name)}</h2>\n'
    h += f'  </div>\n'
    h += f'  <pre class="def-signature">{html_module.escape(sig)}</pre>\n'

    if doc:
        # Summary: strip leading 'name -- '
        summary = doc['summary']
        dash_idx = summary.find(' -- ')
        if dash_idx != -1:
            summary = summary[dash_idx + 4:]
        if summary:
            h += f'  <p class="def-summary">{html_module.escape(summary)}</p>\n'

        if doc['deprecated']:
            h += '  <p class="def-deprecated"><span class="def-deprecated-label">Deprecated</span> '
            h += f'{html_module.escape(doc["deprecated"])}</p>\n'

        if doc['params']:
            h += '  <div class="def-section">\n'
            h += '    <div class="def-section-label">Parameters</div>\n'
            h += '    <table class="param-table">\n'
            for pname, pdesc in doc['params']:
                h += f'    <tr><td>{html_module.escape(pname)}</td>'
                h += f'<td></td><td>{html_module.escape(pdesc)}</td></tr>\n'
            h += '    </table>\n'
            h += '  </div>\n'

        if doc['returns']:
            h += '  <div class="def-section">\n'
            h += '    <div class="def-section-label">Returns</div>\n'
            h += f'    <p class="def-returns">{html_module.escape(doc["returns"])}</p>\n'
            h += '  </div>\n'

        if doc['example']:
            h += '  <div class="def-section">\n'
            h += '    <div class="def-section-label">Example</div>\n'
            h += f'    <pre class="def-example">{html_module.escape(doc["example"])}</pre>\n'
            h += '  </div>\n'

        if doc['since']:
            h += f'  <p class="def-since">Since: {html_module.escape(doc["since"])}</p>\n'

    h += '</div>\n'
    return h


def module_page_name(module):
    """The per-module page/fragment filename: tur/list -> tur-list.html."""
    return module['name'].replace('/', '-') + '.html'


def build_module_content(module, brand='stdlib'):
    """Render a module's article body -- the `<div class="content">` and nothing else.

    This is the single rendering pass for a module's API page: the site wraps
    it in chrome (`render_module_page`), and the docs pack ships it verbatim as
    a fragment for Try Turmeric's in-app docs pane (`--emit-pack`). Rendering
    it once is what keeps the two from drifting.
    """
    mod_name = module['name']
    exported = [d for d in module['definitions'] if d['exported']]
    internal = [d for d in module['definitions'] if not d['exported']]

    # Path display: prefer the relative path stored on the module (when generated
    # by render_tree); fall back to "<brand>/<stem>.tur" for legacy callers.
    rel_path = module.get('rel_path') or f'{brand}/{module["file_stem"]}.tur'

    content = '<div class="content">\n'
    content += '  <div class="module-heading">\n'
    content += f'    <h1>{html_module.escape(mod_name)}</h1>\n'
    content += f'    <div class="module-path">{html_module.escape(rel_path)}</div>\n'
    content += '  </div>\n'

    if module.get('docstring'):
        md = module['docstring']
        summary = md['summary']
        dash_idx = summary.find(' -- ')
        if dash_idx != -1:
            summary = summary[dash_idx + 4:]
        content += '  <div class="module-doc">\n'
        if summary:
            content += f'    <p class="module-doc-summary">{html_module.escape(summary)}</p>\n'
        if md.get('example'):
            content += '    <div class="def-section">\n'
            content += '      <div class="def-section-label">Example</div>\n'
            content += f'      <pre class="def-example">{html_module.escape(md["example"])}</pre>\n'
            content += '    </div>\n'
        if md.get('deprecated'):
            content += '    <p class="def-deprecated"><span class="def-deprecated-label">Deprecated</span> '
            content += f'{html_module.escape(md["deprecated"])}</p>\n'
        if md.get('since'):
            content += f'    <p class="def-since">Since: {html_module.escape(md["since"])}</p>\n'
        content += '  </div>\n'

    for defn in exported:
        content += _render_def_card(defn)

    if internal:
        content += '<details class="internal-section">\n'
        content += '  <summary>Internal definitions</summary>\n'
        content += '  <div class="internal-list">\n'
        for defn in internal:
            doc = defn['docstring']
            summary_text = ''
            if doc and doc['summary']:
                s = doc['summary']
                dash_idx = s.find(' -- ')
                summary_text = s[dash_idx + 4:] if dash_idx != -1 else s
            anchor = re.sub(r'[^a-zA-Z0-9_\-]', '_', defn['name'])
            content += f'  <div class="internal-item" id="{html_module.escape(anchor)}">'
            content += f'<code>{html_module.escape(defn["name"])}</code>'
            if summary_text:
                content += f'<span class="internal-item-summary">-- {html_module.escape(summary_text)}</span>'
            content += '</div>\n'
        content += '  </div>\n</details>\n'

    content += '</div>\n'
    return content


def render_module_page(module, out_dir, brand='stdlib'):
    """Render a per-module HTML page and return the filename."""
    mod_name = module['name']
    page_name = module_page_name(module)

    exported = [d for d in module['definitions'] if d['exported']]
    internal = [d for d in module['definitions'] if not d['exported']]

    # Build sidebar TOC (exported only)
    toc = '      <h3>Exported</h3>\n      <ul>\n'
    for defn in exported:
        anchor = re.sub(r'[^a-zA-Z0-9_\-]', '_', defn['name'])
        toc += (f'        <li><a class="sidebar-def" href="#{html_module.escape(anchor)}"'
                f' title="{_sidebar_tip(defn)}">'
                f'{html_module.escape(defn["name"])}</a></li>\n')
    toc += '      </ul>\n'
    if internal:
        toc += '      <h3>Internal</h3>\n      <ul>\n'
        for defn in internal:
            toc += (f'        <li><a class="sidebar-def" href="#{html_module.escape(defn["name"])}"'
                    f' style="color:var(--faint)" title="{_sidebar_tip(defn)}">'
                    f'{html_module.escape(defn["name"])}</a></li>\n')
        toc += '      </ul>\n'

    sidebar = ('<div class="sidebar">\n'
               + build_sidebar(
                   toc=toc,
                   uplinks=[('index.html', 'All Modules')],
                   base=SITE_BASE,
                   extra_titles={'index.html': 'Every module in this API reference'})
               + '\n</div>\n')

    content = build_module_content(module, brand=brand)

    page = _html_header(f'{html_module.escape(mod_name)} | Turmeric API')
    page += '<div class="page-layout">\n'
    page += sidebar
    page += content
    page += '</div>\n'
    page += _html_footer()

    out_path = Path(out_dir) / page_name
    out_path.write_text(page, encoding='utf-8')
    return page_name


def _index_card_html(module):
    """Render a single index card for a module."""
    mod_name = module['name']
    page_name = mod_name.replace('/', '-') + '.html'
    exported = [d for d in module['definitions'] if d['exported']]
    mod_summary = ''
    if module.get('docstring') and module['docstring']['summary']:
        s = module['docstring']['summary']
        dash_idx = s.find(' -- ')
        mod_summary = s[dash_idx + 4:] if dash_idx != -1 else s
    else:
        # legacy fallback: first ';; ' line at top of file
        with open(module['file_path'], encoding='utf-8', errors='replace') as f:
            for line in f:
                line = line.strip()
                if line.startswith(';; ') and not line.startswith(';;; '):
                    mod_summary = line[3:]
                    break
                if line and not line.startswith(';'):
                    break

    export_count = len(exported)
    h = f'  <a href="{html_module.escape(page_name)}" class="index-card" style="display:block">\n'
    h += f'    <h3>{html_module.escape(mod_name)}</h3>\n'
    if mod_summary:
        h += f'    <p>{html_module.escape(mod_summary)}</p>\n'
    h += f'    <div class="export-count">{export_count} exported definition{"s" if export_count != 1 else ""}</div>\n'
    h += '  </a>\n'
    return h


def render_index_page(modules, out_dir, brand='stdlib', brand_label=None,
                      uplinks=None, uplink_titles=None):
    """Render the module index page, grouped by subdirectory."""
    if brand_label is None:
        brand_label = 'Turmeric Standard Library' if brand == 'stdlib' else f'{brand} API'
    content = '<div class="content">\n'
    content += f'  <h1 style="color:var(--gold);font-size:2rem;margin-bottom:0.5rem">{html_module.escape(brand_label)}</h1>\n'
    content += '  <p style="color:var(--faint);margin-bottom:1.5rem">Auto-generated API reference. Run <code>just docs</code> to regenerate.</p>\n'

    # Group by subdir; None -> 'Core'
    groups = {}
    for module in sorted(modules, key=lambda m: m['name'] or ''):
        key = module.get('subdir') or 'core'
        groups.setdefault(key, []).append(module)

    # Render core first, then remaining groups alphabetically
    group_order = ['core'] + sorted(k for k in groups if k != 'core')

    for group_key in group_order:
        if group_key not in groups:
            continue
        group_modules = groups[group_key]
        group_label = 'Core' if group_key == 'core' else group_key
        content += f'  <h2 style="color:var(--gold);font-size:1.25rem;margin:2rem 0 0.75rem;padding-bottom:0.4rem;border-bottom:1px solid var(--border)">'
        content += f'{html_module.escape(group_label)}</h2>\n'
        content += '  <div class="index-grid">\n'
        for module in group_modules:
            content += _index_card_html(module)
        content += '  </div>\n'

    content += '</div>\n'

    sidebar = ('<div class="sidebar">\n'
               + build_sidebar(uplinks=uplinks, base=SITE_BASE,
                               extra_titles=uplink_titles)
               + '\n</div>\n')

    page = _html_header(f'{brand_label} | API Docs', css_path='style.css')
    page += '<div class="page-layout">\n'
    page += sidebar
    page += content
    page += '</div>\n'
    page += _html_footer()

    out_path = Path(out_dir) / 'index.html'
    out_path.write_text(page, encoding='utf-8')


# ---------------------------------------------------------------------------
# .tur Emitter  (--emit-tur)
# ---------------------------------------------------------------------------

def _escape_tur_string(s):
    """Escape a string for embedding in a Turmeric string literal."""
    return (s
            .replace('\\', '\\\\')
            .replace('"', '\\"')
            .replace('\n', '\\n')
            .replace('\t', '\\t'))


def _build_doc_entry(defn):
    """Build the full text for a doc entry."""
    doc = defn['docstring']
    if not doc:
        return defn['name']

    # LT4: prepend [linear] to the summary when the definition involves linear types.
    params = defn.get('params', [])
    ret = defn.get('return_type', '') or ''
    is_linear = (
        defn.get('struct_ann') == 'linear'
        or any('^linear' in (p or '') for p, _ in params)
        or any('lref<' in (t or '') for _, t in params)
        or 'lref<' in ret
    )
    is_affine = defn.get('struct_ann') == 'affine'
    summary = doc['summary']
    if is_linear:
        summary = '[linear] ' + summary
    elif is_affine:
        summary = '[affine] ' + summary

    parts = [summary]

    if doc['deprecated']:
        parts.append('')
        parts.append('Deprecated:')
        for line in doc['deprecated'].splitlines():
            parts.append(f'  {line}')

    if doc['params']:
        parts.append('')
        parts.append('Parameters:')
        for pname, pdesc in doc['params']:
            parts.append(f'  {pname} -- {pdesc}')

    if doc['returns']:
        parts.append('')
        parts.append('Returns:')
        for line in doc['returns'].splitlines():
            parts.append(f'  {line}')

    if doc['example']:
        parts.append('')
        parts.append('Example:')
        for line in doc['example'].splitlines():
            parts.append(f'  {line}')

    if doc['since']:
        parts.append('')
        parts.append(f'Since: {doc["since"]}')

    return '\n'.join(parts)


VERIFIED_MANIFEST = Path('tests/doctest-generated/verified.txt')
VERIFIED_COMPLETE_MARKER = '# complete:'


def read_verified_manifest(path=VERIFIED_MANIFEST):
    """
    Read the doctest manifest.  Returns (names, reason) where `names` is a set
    when the manifest is authoritative and None when it is not.

    None is not the empty set, and the distinction is the whole point of this
    function.  `verified.txt` is a GITIGNORED build artifact
    (.gitignore: tests/doctest-generated/) but its contents are stamped into
    stdlib/docstrings.tur, which is TRACKED.  Treating "I have no doctest
    results" as "no function passes its doctests" is what let a plain
    `just docs` in a fresh clone delete the whole table -- silently, into a
    tracked file, on the release path, for most of the feature's life.
    See docs/reported/docstrings-verified-table-zeroed-by-regen.md.

    A manifest is authoritative only when run-doctests.sh finished and said so
    with its `# complete:` header.  Anything else -- absent, or present without
    the marker (an interrupted run, or one written by the pre-2026-08-20
    append-as-you-go script) -- is no data.
    """
    if not path.exists():
        return None, f'{path} not found'
    text = path.read_text(encoding='utf-8')
    complete = any(l.startswith(VERIFIED_COMPLETE_MARKER) for l in text.splitlines())
    if not complete:
        return None, f'{path} has no "{VERIFIED_COMPLETE_MARKER}" header (partial or stale run)'
    names = set(
        l.strip() for l in text.splitlines()
        if l.strip() and not l.lstrip().startswith('#')
    )
    return names, None


def read_existing_verified(out_path):
    """
    Recover the verified name list already embedded in `out_path`, so a regen
    with no manifest can carry it forward instead of dropping it.  Returns a
    set (possibly empty) or None when the file does not exist or has no block.
    """
    p = Path(out_path)
    if not p.exists():
        return None
    names, in_block = set(), False
    for line in p.read_text(encoding='utf-8').splitlines():
        if 'static const char *verified[] = {' in line:
            in_block = True
            continue
        if in_block:
            if 'NULL' in line:
                break
            m = re.match(r'\s*"((?:[^"\\]|\\.)*)"\s*,\s*$', line)
            if m:
                names.add(m.group(1).replace('\\"', '"').replace('\\\\', '\\'))
    return names if in_block else None


def emit_docstrings_tur(modules, out_path, verified_names=None):
    """
    Emit stdlib/docstrings.tur with C-backed doc-lookup and doc-verified? functions.

    verified_names -- the set of function names with passing doctests, or None
    for "no authoritative data".  None carries the existing file's list forward
    rather than emitting an empty one; see read_verified_manifest.
    """
    entries = []
    for module in modules:
        for defn in module['definitions']:
            name = defn['name']
            # Strip typeclass bracket suffix for lookup key
            key = re.sub(r'\[.*\]$', '', name)
            text = _build_doc_entry(defn)
            entries.append((key, text))
        # MD3: also register the module name itself as a doc-lookup key
        if module.get('docstring') and module.get('name'):
            mod_doc = module['docstring']
            mod_summary = mod_doc['summary']
            # Strip the "module-name -- " prefix for the stored text
            dash_idx = mod_summary.find(' -- ')
            if dash_idx != -1:
                mod_summary = mod_summary[dash_idx + 4:]
            parts = [mod_doc['summary']]
            if mod_doc.get('deprecated'):
                parts.append('')
                parts.append('Deprecated:')
                for line in mod_doc['deprecated'].splitlines():
                    parts.append(f'  {line}')
            if mod_doc.get('since'):
                parts.append('')
                parts.append(f'Since: {mod_doc["since"]}')
            entries.append((module['name'], '\n'.join(parts)))

    # Deduplicate: keep the first entry for each key
    seen = {}
    deduped = []
    for key, text in entries:
        if key not in seen:
            seen[key] = True
            deduped.append((key, text))

    lines = [
        ';; AUTO-GENERATED -- do not edit. Run: just docs',
        '(defmodule tur/docstrings',
        '  (export doc-lookup doc-verified?)',
        '',
        '(defn doc-lookup [name : cstr] : cstr',
        '  ```c',
        '  static const struct { const char *key; const char *val; } entries[] = {',
    ]

    for key, text in deduped:
        escaped_key = _escape_tur_string(key)
        escaped_text = _escape_tur_string(text)
        lines.append(f'    {{"{escaped_key}", "{escaped_text}"}},')

    lines += [
        '    {NULL, NULL}',
        '  };',
        '  const char *n = (const char*)(intptr_t)name;',
        '  if (!n) return 0;',
        '  for (int __i = 0; entries[__i].key; __i++) {',
        '    if (strcmp(entries[__i].key, n) == 0) {',
        '      return (int64_t)(intptr_t)entries[__i].val;',
        '    }',
        '  }',
        '  return 0;',
        '  ```)',
        '',
    ]

    # Phase D5: emit doc-verified? backed by the verified names set.
    # verified_names is None when no authoritative manifest was found; carry the
    # existing file's list forward rather than silently emptying it.
    carried = False
    if verified_names is None:
        prior = read_existing_verified(out_path)
        if prior:
            verified_names, carried = prior, True
    vnames = sorted(verified_names) if verified_names else []
    lines += [
        '(defn doc-verified? [name : cstr] : bool',
        '  ```c',
        '  static const char *verified[] = {',
    ]
    for vname in vnames:
        escaped = _escape_tur_string(vname)
        lines.append(f'    "{escaped}",')
    lines += [
        '    NULL',
        '  };',
        '  const char *n = (const char*)(intptr_t)name;',
        '  if (!n) return 0;',
        '  for (int __i = 0; verified[__i]; __i++) {',
        '    if (strcmp(verified[__i], n) == 0) return 1;',
        '  }',
        '  return 0;',
        '  ```)',
        '',
        ')',
    ]

    Path(out_path).write_text('\n'.join(lines) + '\n', encoding='utf-8')
    how = ' carried forward' if carried else ''
    print(f'  Wrote {out_path} ({len(deduped)} entries, {len(vnames)} verified{how})')


# ---------------------------------------------------------------------------
# JSON name list emitter  (--emit-json)
# ---------------------------------------------------------------------------

def collect_doc_entries(modules, *, spice=None):
    """
    Return a list of {name, summary, kind, [spice]} entries derived from the
    given parsed modules. When `spice` is set, each entry is tagged with that
    short name so the web search bar can show which spice it came from.
    """
    entries = []
    seen = set()
    for module in modules:
        for defn in module['definitions']:
            name = defn['name']
            key = re.sub(r'\[.*\]$', '', name)
            if key in seen:
                continue
            seen.add(key)

            doc = defn['docstring']
            summary = doc['summary'] if (doc and doc.get('summary')) else key

            entry = {'name': key, 'summary': summary, 'kind': defn['kind']}
            if spice:
                entry['spice'] = spice
            entries.append(entry)

        # MD3: also emit a module-level entry for the web search bar
        if module.get('docstring') and module.get('name'):
            mod_name = module['name']
            if mod_name not in seen:
                seen.add(mod_name)
                s = module['docstring']['summary']
                dash_idx = s.find(' -- ')
                summary = s[dash_idx + 4:] if dash_idx != -1 else s
                entry = {'name': mod_name, 'summary': summary, 'kind': 'module'}
                if spice:
                    entry['spice'] = spice
                entries.append(entry)

    return entries


def emit_doc_names_json(modules, out_path, extra_entries=None):
    """
    Emit a JSON array of {name, summary, kind} objects for the web search bar.
    Summary is the first line of the doc entry text. `extra_entries`, if
    provided, is appended after the locally derived entries (typically used
    to merge spice entries into the stdlib payload).
    """
    import json

    entries = collect_doc_entries(modules)
    if extra_entries:
        # Skip duplicate keys so stdlib wins on collisions
        seen = {e['name'] for e in entries}
        for e in extra_entries:
            if e.get('name') not in seen:
                entries.append(e)
                seen.add(e.get('name'))

    entries.sort(key=lambda e: e['name'].lower())

    out = Path(out_path)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(entries, ensure_ascii=True, indent=None, separators=(',', ':')), encoding='utf-8')
    extra_n = len(extra_entries) if extra_entries else 0
    print(f'  Wrote {out_path} ({len(entries)} entries; {extra_n} merged)')


# ---------------------------------------------------------------------------
# Docs pack  (--emit-pack)
# ---------------------------------------------------------------------------

def emit_pack_api(modules, pack_dir, *, section='api', slug_prefix=''):
    """Write chrome-free per-module fragments into the docs pack.

    `section` names both the pack subdirectory and the sidecar that
    tools/genpack.py merges into index.json. `slug_prefix` disambiguates spice
    modules from stdlib ones when a spice tree is folded into the pack.
    """
    pack_dir = Path(pack_dir)

    # When each module's page first appeared and when it last changed, for the
    # pane's Recently Added / Recently Updated sections. One git traversal for
    # the whole set and for both dates, from the checkout the sources actually
    # live in -- which is the spice's own repo when a spice tree is being folded
    # in, not this one.
    sources = [Path(m['file_path']) for m in modules if m.get('file_path')]
    page_dates = packlib.git_page_dates(
        sources, packlib.find_repo_root(sources[0])) if sources else {}

    entries = []
    seen_slugs = {}
    for module in modules:
        page = module_page_name(module)
        slug = f'{slug_prefix}{page[:-len(".html")]}'
        rel = f'{section}/{slug}.html'
        # Two modules with the same name render to the same page. The site has
        # always resolved that by letting the last one win silently; say so
        # here rather than letting the pack quietly carry one fewer page than
        # it parsed.
        if slug in seen_slugs:
            raise SystemExit(
                f'  error: {module.get("rel_path") or module["name"]} and '
                f'{seen_slugs[slug]} both render to {rel}; the last one would '
                f'win and the other would silently have no page. Give one of '
                f'them a distinct (defmodule ...) name.')
        seen_slugs[slug] = module.get('rel_path') or module['name']
        content = build_module_content(module)
        size = packlib.write_fragment(pack_dir, rel, content)

        exported = [d['name'] for d in module['definitions'] if d['exported']]
        summary = ''
        if module.get('docstring') and module['docstring'].get('summary'):
            s = module['docstring']['summary']
            dash_idx = s.find(' -- ')
            summary = s[dash_idx + 4:] if dash_idx != -1 else s

        entry = {
            'slug': slug,
            'path': rel,
            'module': module['name'],
            'title': module['name'],
            'category': module.get('subdir') or 'core',
            'description': summary,
            'symbols': exported,
            'bytes': size,
            'words': packlib.search_string(
                module['name'], summary, ' '.join(exported),
                prose=packlib.strip_tags(content)),
        }
        source = module.get('file_path')
        dates = page_dates.get(Path(source)) if source else {}
        for key in ('added', 'updated'):
            if dates.get(key):
                entry[key] = dates[key]
        entries.append(entry)

    packlib.write_sidecar(pack_dir, section, entries)
    print(f'  pack: {len(entries)} {section} fragments -> {pack_dir}/{section}/')
    return entries


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def collect_tur_files(source):
    """Collect all .tur files from a path (file or directory)."""
    source = Path(source)
    if source.is_file():
        return [source]
    files = []
    for f in sorted(source.rglob('*.tur')):
        # Skip generated files
        if f.name == 'docstrings.tur':
            continue
        files.append(f)
    return files


def render_tree(source, out_dir, *, brand='stdlib', brand_label=None,
                emit_tur=None, emit_json=None, extra_json_entries=None,
                emit_pack=None, pack_section='api', pack_slug_prefix='',
                site_base='', index_uplinks=None, index_uplink_titles=None):
    """
    Generate an HTML doc tree from a .tur source root.

    source       -- a directory of .tur files (or a single .tur file).
    out_dir      -- output directory; created if missing.
    brand        -- short identifier used for path display ('stdlib', 'tur-json').
    brand_label  -- human-readable label for the index heading and page title.
                    Defaults to 'Turmeric Standard Library' when brand == 'stdlib',
                    or '<brand> API' otherwise.
    emit_tur     -- optional Path; when set, also emit a docstrings.tur lookup.
    emit_json    -- optional Path; when set, also emit a JSON name list.
    site_base    -- host to re-root the chrome's site-relative links onto. Empty
                    for turmeric-lang.com itself; the spices subdomain passes the
                    main host, because a bare `/tour` there would resolve against
                    spices.turmeric-lang.com and 404.
    """
    global SITE_BASE
    SITE_BASE = site_base
    source = Path(source)
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    # Write CSS
    css_path = out_dir / 'style.css'
    css_path.write_text(CSS, encoding='utf-8')
    print(f'  Wrote {css_path}')
    write_page_scripts(out_dir, _API_PAGE_SCRIPTS)

    # Collect and parse files
    tur_files = collect_tur_files(source)
    if not tur_files:
        print(f'No .tur files found in {source}', file=sys.stderr)
        sys.exit(1)

    # Resolve the source root for subdir computation (only meaningful for dirs)
    source_root = source.resolve() if source.is_dir() else None
    # Display prefix for module-path lines: use the source dir name (e.g. "stdlib"
    # or "json" for spices/json/src/) so the path label matches the on-disk layout.
    if source_root:
        path_prefix = source_root.name
    else:
        path_prefix = brand

    modules = []
    for f in tur_files:
        try:
            module = parse_tur_file(f)
            if source_root:
                try:
                    rel_parts = Path(f).resolve().relative_to(source_root).parts
                    module['subdir'] = rel_parts[0] if len(rel_parts) > 1 else None
                    module['rel_path'] = f'{path_prefix}/' + '/'.join(rel_parts)
                    apply_path_fallback_name(module, rel_parts)
                except ValueError:
                    module['subdir'] = None
                    module['rel_path'] = None
            else:
                module['subdir'] = None
                module['rel_path'] = f'{path_prefix}/{Path(f).name}'
            modules.append(module)
            exported_count = sum(1 for d in module['definitions'] if d['exported'])
            print(f'  Parsed {f} -> {module["name"]} ({exported_count} exported defs)')
        except Exception as e:
            print(f'  Warning: failed to parse {f}: {e}', file=sys.stderr)

    # Two modules that resolve to the same page name are a silent data loss:
    # whichever renders second overwrites the first, the index still shows both
    # cards, and one of those two links 404s. That shipped for as long as
    # stdlib/capability.tur and stdlib/test/capability.tur both existed, and
    # the only signal was a page count one short of the module count. Fail
    # here, where the message can name both files.
    seen_pages = {}
    for module in modules:
        page = module_page_name(module)
        if page in seen_pages:
            print(f'  error: {module.get("rel_path") or module["file_path"]} and '
                  f'{seen_pages[page]} both render to {page}; give one of them a '
                  f'distinct (defmodule ...) name', file=sys.stderr)
            sys.exit(1)
        seen_pages[page] = module.get('rel_path') or module['file_path']

    # Render per-module pages
    for module in modules:
        page = render_module_page(module, out_dir, brand=brand)
        print(f'  Wrote {out_dir}/{page}')

    # Render index
    render_index_page(modules, out_dir, brand=brand, brand_label=brand_label,
                      uplinks=index_uplinks, uplink_titles=index_uplink_titles)
    print(f'  Wrote {out_dir}/index.html')

    # Optionally emit docstrings.tur
    if emit_tur:
        # Phase D5: read verified function names from the doctest manifest.
        # A missing or partial manifest is "no data", NOT "nothing is verified"
        # -- emit_docstrings_tur carries the existing table forward for it.
        verified_names, why = read_verified_manifest()
        if why:
            print(f'  note: no doctest manifest ({why});', file=sys.stderr)
            print(f'        carrying the existing doc-verified? table forward. '
                  f'Run `just doctest` to refresh it.', file=sys.stderr)
        emit_docstrings_tur(modules, emit_tur, verified_names=verified_names)

    # Optionally emit doc-names.json for the web search bar
    if emit_json:
        emit_doc_names_json(modules, emit_json, extra_entries=extra_json_entries)

    # Optionally emit chrome-free fragments into the docs pack
    if emit_pack:
        emit_pack_api(modules, emit_pack, section=pack_section,
                      slug_prefix=pack_slug_prefix)

    print(f'\nDone. {len(modules)} modules -> {out_dir}/')
    return modules


def main():
    parser = argparse.ArgumentParser(
        description='Generate HTML API docs from Turmeric stdlib files.'
    )
    parser.add_argument(
        'source',
        help='Path to a .tur file or a directory containing .tur files',
    )
    parser.add_argument(
        '--out', '-o',
        default='docs/html/api',
        help='Output directory for HTML files (default: docs/html/api)',
    )
    parser.add_argument(
        '--emit-tur',
        metavar='PATH',
        help='Also emit a stdlib/docstrings.tur lookup table at PATH',
    )
    parser.add_argument(
        '--emit-json',
        metavar='PATH',
        help='Also emit a JSON name list at PATH (for web search bar)',
    )
    parser.add_argument(
        '--brand',
        default='stdlib',
        help='Brand identifier for path display (default: stdlib)',
    )
    parser.add_argument(
        '--brand-label',
        default=None,
        help='Human-readable brand label for the index heading',
    )
    parser.add_argument(
        '--extra-json',
        metavar='PATH',
        help='Read extra doc-name entries (JSON array) from PATH and merge '
             'them into the --emit-json output. Used to fold spice symbols '
             'into the stdlib payload.',
    )
    parser.add_argument(
        '--emit-pack',
        metavar='DIR',
        help='Also write chrome-free per-module fragments into the docs pack '
             'at DIR (see tools/genpack.py)',
    )
    args = parser.parse_args()

    extra_entries = None
    if args.extra_json:
        import json
        extra_path = Path(args.extra_json)
        if extra_path.is_file():
            extra_entries = json.loads(extra_path.read_text(encoding='utf-8'))

    render_tree(
        args.source,
        args.out,
        brand=args.brand,
        brand_label=args.brand_label,
        emit_tur=args.emit_tur,
        emit_json=args.emit_json,
        extra_json_entries=extra_entries,
        emit_pack=args.emit_pack,
    )


if __name__ == '__main__':
    main()
