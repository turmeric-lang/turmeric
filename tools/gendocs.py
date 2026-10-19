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
                       SIDEBAR_DRAWER_JS_SRC, font_links, theme_head, script_tag,
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


def _dedent_block(lines):
    """Strip blank edges and the common leading indent from a block of lines."""
    while lines and not lines[0].strip():
        lines = lines[1:]
    while lines and not lines[-1].strip():
        lines = lines[:-1]
    indents = [len(l) - len(l.lstrip()) for l in lines if l.strip()]
    cut = min(indents) if indents else 0
    return '\n'.join(l[cut:] for l in lines)


def _parse_docstring(lines):
    """
    Parse a list of ';;; ...' lines into structured sections.
    Returns a dict:
      {
        'summary': str,            # the first line, as lookup tables show it
        'summary_full': str,       # the first paragraph, joined (a wrapped summary)
        'description': str,        # prose between the summary and the first section
        'params': [(name, desc), ...],
        'methods': [(name, desc), ...],   # a defclass's `Methods:` section
        'laws': str,               # a defclass's `Laws:` section, verbatim
        'returns': str,
        'example': str,
        'notes': str,              # prose after the first section
        'since': str,
        'raw': str,   # full text for --emit-tur
      }

    A section runs until the next header, or until a blank line followed by an
    UNINDENTED line: section bodies are indented, so an unindented paragraph
    after a gap is prose again (`Superclass: Eq.`, `Note: ...`), not one more
    parameter or example line.  Prose is never dropped: before the first
    section it is the description, after it the notes.
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

    # A summary that wraps continues on the following unindented lines, up to
    # the first blank line or section header.
    headers = ('Parameters:', 'Methods:', 'Laws:', 'Returns:', 'Example:',
               'Since:', 'Deprecated:')
    summary_lines = [summary]
    k = 1
    while (k < len(stripped) and stripped[k].strip()
           and not stripped[k][0].isspace()
           and not stripped[k].startswith(headers)):
        summary_lines.append(stripped[k])
        k += 1

    sections = {'summary': summary, 'summary_full': ' '.join(summary_lines),
                'description': '', 'params': [], 'methods': [], 'laws': '',
                'returns': '', 'example': '', 'notes': '', 'since': '',
                'deprecated': '', 'raw': '\n'.join(stripped)}

    current_section = None
    seen_section = False
    prev_blank = False
    buf = []
    prose = {'description': [], 'notes': []}
    method_indent = None

    def flush():
        nonlocal buf
        text = '\n'.join(buf).strip()
        buf = []
        return text

    def _flush_section():
        nonlocal buf
        if current_section in ('returns', 'deprecated'):
            sections[current_section] = flush()
        elif current_section in ('example', 'laws'):
            # Code: drop the common indent, not just the first line's, or a
            # multi-line example renders with its first line out of step.
            sections[current_section] = _dedent_block(buf)
            buf = []

    for line in stripped[k:]:
        header = next((h for h in headers if line.startswith(h)), None)
        if header:
            _flush_section()
            seen_section = True
            prev_blank = False
            rest = line[len(header):].strip()
            current_section = {'Parameters:': 'params', 'Methods:': 'methods',
                               'Laws:': 'laws', 'Returns:': 'returns',
                               'Example:': 'example', 'Since:': None,
                               'Deprecated:': 'deprecated'}[header]
            if header == 'Since:':
                sections['since'] = rest
            elif header == 'Methods:':
                method_indent = None
            elif rest and current_section in ('deprecated', 'returns', 'example', 'laws'):
                # `Returns: void` -- the body on the header line itself.
                buf.append(rest)
            continue

        if current_section and line.strip() and prev_blank and not line[0].isspace():
            # An unindented paragraph after a gap leaves the section.
            _flush_section()
            current_section = None

        prev_blank = not line.strip()

        if current_section is None:
            prose['notes' if seen_section else 'description'].append(line)
        elif current_section == 'params':
            stripped_line = line.strip()
            if stripped_line:
                # Format: '  name -- description'
                m = re.match(r'^(\S+)\s+--\s+(.*)', stripped_line)
                if m:
                    sections['params'].append((m.group(1), m.group(2)))
                else:
                    sections['params'].append(('', stripped_line))
        elif current_section == 'methods':
            stripped_line = line.strip()
            if stripped_line:
                # '  name -- description', several names allowed
                # ('  lt? lte? gt? gte? -- ...').  A line indented deeper than
                # the entries continues the one above it.
                indent = len(line) - len(line.lstrip())
                m = re.match(r'^(.+?)\s+--\s+(.*)', stripped_line)
                if m and (method_indent is None or indent <= method_indent):
                    method_indent = indent
                    sections['methods'].append((m.group(1), m.group(2)))
                elif sections['methods']:
                    name, desc = sections['methods'][-1]
                    sections['methods'][-1] = (name, f'{desc} {stripped_line}'.strip())
                else:
                    sections['methods'].append(('', stripped_line))
        else:
            buf.append(line)

    _flush_section()
    sections['description'] = _dedent_prose(prose['description'])
    sections['notes'] = _dedent_prose(prose['notes'])

    return sections


def _dedent_prose(lines):
    """Prose lines with blank edges trimmed; indentation is kept, because an
    indented run inside prose is a code block (see _prose_html)."""
    while lines and not lines[0].strip():
        lines = lines[1:]
    while lines and not lines[-1].strip():
        lines = lines[:-1]
    return '\n'.join(lines)


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
        r'^\s*\(\s*(defn|defmacro|defstruct|defdata|defgadt|defclass|definstance|defopaque|defeffect)\s+'
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

            if kind == 'defclass':
                # The methods are the class's API, so read the whole form --
                # through its last method, `;;;` method docs included -- and
                # resume scanning after it.
                j = i + 1
                depth = strip_line_comment(line).count('(') - strip_line_comment(line).count(')')
                while depth > 0 and j < len(lines):
                    code = strip_line_comment(lines[j])
                    depth += code.count('(') - code.count(')')
                    j += 1
                name, extra = _parse_defclass(lines[i:j])
                if name:
                    module['definitions'].append({
                        'kind': kind,
                        'name': name,
                        'params': [],
                        'return_type': None,
                        # A class and its methods are global -- no export list
                        # names one -- so the class is always public API.
                        'exported': not name.startswith('__'),
                        'docstring': _parse_docstring(doc_buf) if doc_buf else None,
                        'line': i + 1,
                        **extra,
                    })
                doc_buf = []
                i = j
                continue

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

        # A blank line also ends a module docstring -- one whose summary names
        # the module -- so the first definition's own `;;;` block after the
        # gap is not merged into it (typeclass-ord.tur's Ord, which used to
        # vanish into the module text).
        if (not stripped and doc_buf and not first_def_seen
                and module['docstring'] is None and pending_module_doc is None
                and _names_module(doc_buf[0], module)):
            pending_module_doc = list(doc_buf)
            doc_buf = []

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


def _defclass_members(lines):
    """
    Split a defclass form, given as its raw source lines, into its top-level
    members: [(opener, text, doc_lines), ...] where opener is '[' (the type
    parameter vector, the superclass vector) or '(' (a method, or an
    associated `(type Name : Kind)`).

    doc_lines are the `;;;` lines directly above a member -- a method may be
    documented in place, inside the class body, as well as in the class's
    `Methods:` section.  `;` comments are code-stripped before counting
    parentheses, so a `(` in a comment cannot unbalance the form.
    """
    members = []
    pending_doc = []
    depth = 0
    cur = None  # [opener, chars, doc]
    for line in lines:
        s = line.strip()
        if s.startswith(';;;'):
            if depth == 1 and cur is None:
                pending_doc.append(s)
            continue
        code = strip_line_comment(line)
        in_str = False
        for ch in code:
            if ch == '"':
                in_str = not in_str
            if not in_str and ch in '([':
                depth += 1
                if depth == 2 and cur is None:
                    cur = [ch, [], pending_doc]
                    pending_doc = []
            if cur is not None:
                cur[1].append(ch)
            if not in_str and ch in ')]':
                depth -= 1
                if depth == 1 and cur is not None:
                    members.append((cur[0], ' '.join(''.join(cur[1]).split()), cur[2]))
                    cur = None
                if depth <= 0:
                    return members
        if cur is not None:
            cur[1].append(' ')
        elif s and not s.startswith(';'):
            pending_doc = []
    return members


def _parse_defclass(lines):
    """
    Parse a `(defclass Name [params] [(Super a) ...] (method [args] : T) ...)`
    form from its raw source lines.  Returns (name, extra) where extra holds
    'type_params', 'superclasses', 'assoc_types' and 'methods'; each method is
    {'name', 'params', 'return_type', 'doc'} with 'doc' the parsed `;;;` block
    written above it inside the class body (or None).
    """
    m = re.search(r'\(\s*defclass\s+([^\s\[\]()]+)', lines[0])
    if not m:
        return None, {}
    name = m.group(1)
    extra = {'type_params': [], 'superclasses': [], 'assoc_types': [], 'methods': []}
    vectors = 0
    for opener, text, doc in _defclass_members(lines):
        if opener == '[':
            inner = text[1:-1].strip()
            if vectors == 0 and '(' not in inner:
                extra['type_params'] = inner.split()
            else:
                extra['superclasses'] = re.findall(r'\([^()]*\)', inner)
            vectors += 1
            continue
        mm = re.match(r'\(\s*([^\s\[\]()]+)', text)
        if not mm:
            continue
        mname = mm.group(1)
        body = text[mm.end():-1]   # drop the method form's closing paren
        if mname == 'type':
            # An associated type: `(type Storage : Type)`.
            extra['assoc_types'].append(f'(type {body.strip()})')
            continue
        inner, end = _find_bracket(body)
        params = []
        ret = None
        if inner is not None:
            # A grouped parameter `[ma [handler :fn]]` reads as its members;
            # a vector inside a type, `(fn [a] b)`, is one token and stays.
            flat = ' '.join(t[1:-1] if t.startswith('[') and t.endswith(']') else t
                            for t in _split_sig_tokens(inner))
            params = _parse_params('[' + flat + ']')
            ret = _return_type_after_bracket(body[end:])
        extra['methods'].append({
            'name': mname,
            'params': params,
            'return_type': ret,
            'doc': _parse_docstring(doc) if doc else None,
        })
    return name, extra


def _names_module(first_doc_line, module):
    """True when a `;;; name -- ...` line's name is this module: its full
    name, last segment, or file stem (`tur/typeclass-ord`, `app`)."""
    m = re.match(r';;;\s+(\S+)\s+--\s', first_doc_line)
    if not m:
        return False
    name = m.group(1)
    full = module.get('name') or ''
    return name in (full, full.rsplit('/', 1)[-1], module.get('file_stem'))


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
        # (definstance ClassName [Type] ...).  The head may be compound --
        # `[(->)]`, `[(Result _ B)]`, `[int cstr]` -- so take the whole
        # balanced vector, not its first word: the first-word regex this
        # replaced read `[(->)]` as empty and then grabbed a METHOD's
        # parameter vector (`ArrowLoop[f]`).
        head, _ = _find_bracket(rest)
        type_param = ' '.join(head.split()) if head is not None else ''
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

/* Two themes, keyed exactly as on the hand-written pages (web/vars.css):
   `:root` is dark, `:root[data-theme="light"]` overrides it, and
   /theme.js sets the attribute before first paint. The values are the
   site's; keep them in step with web/vars.css. */
:root {
  color-scheme: dark;

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
  --gold-fill:       #D48B1C;
  --gold-fill-hover: #EFA030;
  --on-gold:         #0A0800;

  /* Text */
  --text-primary:  #EAE0D2;
  --text-sec:      #8A7D6E;
  --text-dim:      #4A4438;

  --scrim:         rgba(0,0,0,0.55);
  --logo-filter:   none;

  /* Syntax highlight */
  --syn-kw:        #EFA030;
  --syn-str:       #D9735A;
  --syn-cmt:       #48433D;
  --syn-type:      #7AC4B8;
  --syn-num:       #A8C98A;

  /* Linear-type badge and deprecation callout */
  --linear:        #50C8E8;
  --deprecated:    #d97706;

  /* Secondary accent -- gold leads, green answers it. Same pairing the home
     page uses, so emphasis reads the same across the whole site. */
  --green:         #A8C98A;
  --green-subtle:  rgba(168,201,138,0.10);
  --green-line:    rgba(168,201,138,0.25);
}

/* Light theme -- the same block as web/vars.css :root[data-theme="light"]. */
:root[data-theme="light"] {
  color-scheme: light;

  --bg-base:       #FBF8F2;
  --bg-surface:    #F5F0E7;
  --bg-panel:      #EFE8DC;
  --bg-hover:      #E7DFD0;

  --border:        #E3D9C8;
  --border-mid:    #D5C8B3;
  --border-str:    #C0B099;

  --gold:          #945602;
  --gold-bright:   #804900;
  --gold-dim:      #B88A45;
  --gold-subtle:   rgba(148,86,2,0.08);
  --gold-line:     rgba(148,86,2,0.25);
  --gold-fill:       #D9921F;
  --gold-fill-hover: #C98318;
  --on-gold:         #1C1408;

  --text-primary:  #2A2016;
  --text-sec:      #6A5B4B;
  --text-dim:      #978874;

  --scrim:         rgba(42,32,22,0.35);
  --logo-filter:   brightness(0.80) saturate(1.2);

  --syn-kw:        #965500;
  --syn-str:       #AE3C26;
  --syn-cmt:       #A39682;
  --syn-type:      #127065;
  --syn-num:       #456F20;

  --linear:        #0B6F8A;
  --deprecated:    #9A4F00;

  --green:         #456F20;
  --green-subtle:  rgba(69,111,32,0.10);
  --green-line:    rgba(69,111,32,0.25);
}

*, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }

/* The sticky .site-header is 60px tall and the page scrolls on the window, so
   without this a #section link lands its heading underneath the header. The
   extra 1rem leaves the heading some air below the bar. */
html { font-size: 16px; scroll-padding-top: calc(60px + 1rem); }

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
.site-header .nav-logo img { filter: var(--logo-filter); }

/* Light/dark switch -- same button as web/site.css .theme-toggle; the click
   is handled by /theme.js through [data-theme-toggle]. It sits just before
   .nav-right and inherits that cluster's auto margin, so it stays pinned
   right when the cluster drops out. */
.theme-toggle {
  margin-left: auto;
  flex-shrink: 0;
  display: inline-flex;
  align-items: center;
  justify-content: center;
  width: 34px;
  height: 34px;
  padding: 0;
  border-radius: 7px;
  border: 1px solid var(--border-str);
  background: transparent;
  color: var(--text-sec);
  cursor: pointer;
}
.theme-toggle:hover { color: var(--text-primary); background: var(--bg-hover); }
.theme-toggle:focus-visible { outline: 2px solid var(--gold); outline-offset: 2px; }
.theme-toggle svg { display: block; width: 16px; height: 16px; }
.theme-toggle .icon-moon { display: none; }
:root[data-theme="light"] .theme-toggle .icon-sun  { display: none; }
:root[data-theme="light"] .theme-toggle .icon-moon { display: block; }
.site-header .search-wrap + .theme-toggle { margin-left: 0; }
/* The header's 1.5rem gap is for separating groups; the toggle belongs to the
   button cluster, which spaces its own members 8px apart. */
.site-header .theme-toggle + .nav-right { margin-left: calc(8px - 1.5rem); }

.btn-ghost, .btn-gold {
  font-family: inherit;
  font-size: 0.84rem;
  padding: 7px 14px;
  border-radius: 7px;
  white-space: nowrap;
}
.btn-ghost {
  color: var(--text-sec);
  border: 1px solid var(--border-str);
  background: transparent;
}
.btn-ghost:hover { color: var(--text-primary); background: var(--bg-hover); text-decoration: none; }
.btn-gold {
  font-weight: 500;
  color: var(--on-gold);
  padding: 7px 18px;
  border: 1px solid var(--gold-fill);
  background: var(--gold-fill);
}
.btn-gold:hover { background: var(--gold-fill-hover); border-color: var(--gold-fill-hover); text-decoration: none; }

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
  font-family: inherit;
}
.search-input:focus { outline: none; border-color: var(--gold); width: 280px; }
.search-input::placeholder { color: var(--text-dim); }
/* The header filter box is hidden on mobile; site-drawer.js then adds a copy at
   the top of the sidebar drawer.  It is a separate class so the filter scripts,
   which bind the first .search-input, never see it. */
.sidebar-search { display: none; }
.sidebar-search-input {
  background: var(--bg-base);
  border: 1px solid var(--border-mid);
  border-radius: 6px;
  color: var(--text-primary);
  padding: 0.4rem 0.65rem;
  font-size: 0.875rem;
  width: 100%;
  font-family: inherit;
}
.sidebar-search-input:focus { outline: none; border-color: var(--gold); }
.sidebar-search-input::placeholder { color: var(--text-dim); }
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
}
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
.kind-defn      { background: color-mix(in srgb, var(--gold) 15%, transparent); color: var(--gold-bright); }
.kind-defmacro  { background: color-mix(in srgb, var(--syn-str) 15%, transparent); color: var(--syn-str); }
.kind-defstruct { background: color-mix(in srgb, var(--syn-num) 12%, transparent); color: var(--syn-num); }
.kind-definstance { background: color-mix(in srgb, var(--syn-type) 12%, transparent); color: var(--syn-type); }
.kind-defclass  { background: color-mix(in srgb, var(--green) 14%, transparent); color: var(--green); }

/* LT4: badge for linear types and ^linear / lref<T> annotations */
.linear-badge {
  display: inline-block;
  padding: 0.15rem 0.55rem;
  border-radius: 99px;
  font-size: 0.7rem;
  font-family: 'Iosevka', 'Fira Code', monospace;
  font-weight: 600;
  text-transform: lowercase;
  background: color-mix(in srgb, var(--linear) 12%, transparent);
  color: var(--linear);
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

/* Docstring prose: the description under a summary and the notes after the
   sections (_prose_html).  An indented run inside it is a code block. */
.def-prose { font-size: 0.9rem; color: var(--text-primary); line-height: 1.6; margin: 0.5rem 0; }
.def-prose-list { font-size: 0.9rem; line-height: 1.6; margin: 0.5rem 0 0.5rem 1.25rem; }
.def-prose-code {
  background: var(--bg-panel);
  border: 1px solid var(--border);
  border-radius: 6px;
  padding: 0.45rem 0.75rem;
  margin: 0.4rem 0 0.6rem;
  font-family: 'Iosevka', 'Fira Code', monospace;
  font-size: 0.8rem;
  overflow-x: auto;
  white-space: pre;
  color: var(--text-primary);
}

/* A defclass card's methods: each signature, then what it does. */
.method-list { margin: 0; }
.method-list dt {
  font-family: 'Iosevka', 'Fira Code', monospace;
  font-size: 0.85rem;
  color: var(--gold-bright);
  margin-top: 0.5rem;
  overflow-x: auto;
}
.method-list dt:first-child { margin-top: 0; }
.method-list dd { margin: 0.15rem 0 0 1.25rem; }
.method-list dd .def-prose { margin: 0.15rem 0; }
.def-instances { font-size: 0.875rem; line-height: 1.9; }
.def-instances a { text-decoration: none; }

/* A docstring's `code` spans (_inline_html).  The same chip the guides draw
   (genguides GUIDE_CSS), so a name reads the same in prose on either page. */
.def-card p code, .param-table td code, .module-doc p code,
.def-card li code, .def-instances code,
.internal-item-summary code, .index-card p code {
  font-family: 'Iosevka', 'Fira Code', monospace;
  font-size: 0.85em;
  background: var(--bg-panel);
  border: 1px solid var(--border);
  border-radius: 3px;
  padding: 0.1em 0.35em;
}

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
  border-left: 3px solid var(--deprecated);
  background: color-mix(in srgb, var(--deprecated) 8%, transparent);
  color: var(--text-primary);
  font-size: 0.9rem;
}
.def-deprecated-label {
  font-weight: 700;
  color: var(--deprecated);
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
}
a.index-card:hover { background: var(--bg-hover); border-color: var(--border-mid); text-decoration: none; }
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
}
.hamburger:hover span { background: var(--text-primary); }

/* Sidebar overlay (mobile) */
.sidebar-overlay {
  display: none;
  position: fixed;
  inset: 0;
  background: var(--scrim);
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
  .sidebar-search { display: block; margin-bottom: 1rem; }
  /* The filter box is hidden but still a sibling, so its "+" rule above
     would leave the toggle beside the logo. */
  .site-header .search-wrap + .theme-toggle { margin-left: auto; }
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


# Docstring prose -- summaries, parameter descriptions, Returns, Deprecated --
# writes code the way the guides do, as Markdown `code` spans; well over a
# thousand `;;;` lines in stdlib/ carry one.  Escaped as plain text, they
# reached the page with their backticks showing.
#
# Only the span syntax is honoured.  A docstring is not a Markdown document:
# its `*`, `_` and `[...]` are ordinary Turmeric (`*args*`, `tur_box_*`,
# `[x : int]`), not emphasis or links.  A run of N backticks opens a span that
# the next run of exactly N closes, so ``a `b` c`` can show a backtick, and a
# span may wrap onto the next `;;;` line.  An unmatched backtick stays literal.
# Example blocks are <pre> and untouched.
_CODE_SPAN_RE = re.compile(r'(?<!`)(`+)(?!`)(.+?)(?<!`)\1(?!`)', re.DOTALL)


def _inline_html(text):
    """Escape docstring prose for HTML, rendering `code` spans as <code>."""
    out = []
    pos = 0
    for m in _CODE_SPAN_RE.finditer(text):
        # A span that wrapped onto the next line reads as one space there, as
        # in CommonMark.
        body = m.group(2).replace('\n', ' ')
        # CommonMark: one space either side is padding, there so the span can
        # start or end with a backtick (`` `x` ``), not part of the code.
        if len(body) > 2 and body[0] == ' ' and body[-1] == ' ' and body.strip():
            body = body[1:-1]
        out.append(html_module.escape(text[pos:m.start()]))
        out.append(f'<code>{html_module.escape(body)}</code>')
        pos = m.end()
    out.append(html_module.escape(text[pos:]))
    return ''.join(out)


def _prose_html(text, indent='  '):
    """
    Render docstring prose (a description or notes block) as HTML.

    Paragraphs are separated by blank lines.  Within one, unindented lines
    are running text and join into a <p>; an indented run is a code block and
    keeps its lines (a law, a signature, a snippet under `Two caveats:`); a
    paragraph whose lines open with `- ` is a bullet list, its indented
    lines continuing the item above.
    """
    if not text.strip():
        return ''
    out = []
    paras, cur = [], []
    for line in text.split('\n'):
        if line.strip():
            cur.append(line)
        elif cur:
            paras.append(cur)
            cur = []
    if cur:
        paras.append(cur)

    def bullet(l):
        return l.startswith('- ') or l.startswith('* ')

    for para in paras:
        if bullet(para[0]):
            items = []
            for l in para:
                if bullet(l) or not items:
                    items.append(l[2:].strip() if bullet(l) else l.strip())
                else:
                    items[-1] += ' ' + l.strip()
            out.append(f'{indent}<ul class="def-prose-list">'
                       + ''.join(f'<li>{_inline_html(it)}</li>' for it in items)
                       + '</ul>\n')
            continue
        run, run_code = [], None
        for l in para + [None]:
            is_code = l is not None and l[0].isspace()
            if run and (l is None or is_code != run_code):
                if run_code:
                    out.append(f'{indent}<pre class="def-prose-code">'
                               f'{html_module.escape(_dedent_block(run))}</pre>\n')
                else:
                    out.append(f'{indent}<p class="def-prose">'
                               f'{_inline_html(" ".join(s.strip() for s in run))}</p>\n')
                run = []
            if l is not None:
                run.append(l)
                run_code = is_code
    return ''.join(out)


def _method_signature(method):
    """`(extract [wa] :int)` -- a class method, spelled like a defn signature."""
    param_str = ' '.join(
        (f"& {p}" + t[3:] if t.startswith('...') else f"{p} {t}") if t else p
        for p, t in method['params']
    )
    sig = f"({method['name']} [{param_str}]"
    if method.get('return_type'):
        sig += f" {method['return_type']}"
    return sig + ")"


def _class_method_rows(defn):
    """
    A defclass's methods as [(signature, description)], in source order.

    A method's description is the `;;;` block written above it in the class
    body, else its entry in the class docstring's `Methods:` section (an entry
    may name several methods: `lt? lte? gt? gte? -- ...`).  `Methods:` entries
    that name no source method -- prose about a derived operation, a typo --
    are kept at the end with no signature rather than silently dropped.
    """
    doc = defn.get('docstring') or {}
    by_name = {}
    for names, desc in doc.get('methods', []):
        for n in names.split():
            by_name.setdefault(n, desc)
    rows, used = [], set()
    for meth in defn.get('methods', []):
        mdoc = meth.get('doc')
        if mdoc:
            desc = mdoc.get('summary_full') or mdoc.get('summary') or ''
            dash = desc.find(' -- ')
            if dash != -1 and desc[:dash].strip() == meth['name']:
                desc = desc[dash + 4:]
            rest = mdoc.get('description') or ''
            desc = (desc + ('\n\n' + rest if rest else '')).strip()
        else:
            desc = by_name.get(meth['name'], '')
        if meth['name'] in by_name:
            used.add(meth['name'])
        rows.append((_method_signature(meth), desc))
    for names, desc in doc.get('methods', []):
        if not any(n in used for n in names.split()):
            rows.append((names, desc))
    return rows


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
{theme_head(indent='')}
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
  Do not edit by hand -- run <code>tur run docs</code> to regenerate.
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
        return f"(definstance {name.replace('[', ' [', 1)})"

    if kind == 'defclass':
        # The class header; its methods follow in the card's Methods section.
        sig = f"(defclass {name} [{' '.join(defn.get('type_params', []))}]"
        if defn.get('superclasses'):
            sig += f" [{' '.join(defn['superclasses'])}]"
        return sig + ")"

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
        summary = doc.get('summary_full') or doc['summary']
        dash_idx = summary.find(' -- ')
        if dash_idx != -1:
            summary = summary[dash_idx + 4:]
        if summary:
            h += f'  <p class="def-summary">{_inline_html(summary)}</p>\n'

        if doc['deprecated']:
            h += '  <p class="def-deprecated"><span class="def-deprecated-label">Deprecated</span> '
            h += f'{_inline_html(doc["deprecated"])}</p>\n'

        h += _prose_html(doc.get('description', ''))

        if doc['params']:
            # A class's parameters are its type parameters (`^f -- ...`).
            label = 'Type parameters' if kind == 'defclass' else 'Parameters'
            h += '  <div class="def-section">\n'
            h += f'    <div class="def-section-label">{label}</div>\n'
            h += '    <table class="param-table">\n'
            for pname, pdesc in doc['params']:
                h += f'    <tr><td>{html_module.escape(pname)}</td>'
                h += f'<td></td><td>{_inline_html(pdesc)}</td></tr>\n'
            h += '    </table>\n'
            h += '  </div>\n'

    if kind == 'defclass':
        h += _render_class_sections(defn)

    if doc:
        if doc['returns']:
            h += '  <div class="def-section">\n'
            h += '    <div class="def-section-label">Returns</div>\n'
            h += f'    <p class="def-returns">{_inline_html(doc["returns"])}</p>\n'
            h += '  </div>\n'

        if doc.get('laws'):
            h += '  <div class="def-section">\n'
            h += '    <div class="def-section-label">Laws</div>\n'
            h += f'    <pre class="def-example">{html_module.escape(doc["laws"])}</pre>\n'
            h += '  </div>\n'

        if doc['example']:
            h += '  <div class="def-section">\n'
            h += '    <div class="def-section-label">Example</div>\n'
            h += f'    <pre class="def-example">{html_module.escape(doc["example"])}</pre>\n'
            h += '  </div>\n'

        if doc.get('notes'):
            h += '  <div class="def-section def-notes">\n'
            h += _prose_html(doc['notes'], indent='    ')
            h += '  </div>\n'

    if kind == 'defclass':
        h += _render_class_instances(defn)

    if doc:
        if doc['since']:
            h += f'  <p class="def-since">Since: {_inline_html(doc["since"])}</p>\n'

    h += '</div>\n'
    return h


def _render_class_sections(defn):
    """A defclass card's Associated types and Methods sections."""
    h = ''
    if defn.get('assoc_types'):
        h += '  <div class="def-section">\n'
        h += '    <div class="def-section-label">Associated types</div>\n'
        h += ('    <pre class="def-example">'
              + html_module.escape('\n'.join(defn['assoc_types'])) + '</pre>\n')
        h += '  </div>\n'

    rows = _class_method_rows(defn)
    if rows:
        h += '  <div class="def-section">\n'
        h += '    <div class="def-section-label">Methods</div>\n'
        h += '    <dl class="method-list">\n'
        for sig, desc in rows:
            if sig:
                h += f'      <dt><code>{html_module.escape(sig)}</code></dt>\n'
            h += '      <dd>'
            if desc:
                h += '\n' + _prose_html(desc, indent='        ') + '      '
            h += '</dd>\n'
        h += '    </dl>\n'
        h += '  </div>\n'

    return h


def _render_class_instances(defn):
    """A defclass card's Instances line: every type the rendered tree gives an
    instance, linked to the definstance it names."""
    h = ''
    instances = defn.get('instances') or []
    if instances:
        h += '  <div class="def-section">\n'
        h += '    <div class="def-section-label">Instances</div>\n'
        links = []
        for type_name, href, module_name in instances:
            label = f'<code>{html_module.escape(type_name)}</code>'
            title = html_module.escape(f'{defn["name"]} [{type_name}] in {module_name}', quote=True)
            if href:
                links.append(f'<a href="{html_module.escape(href, quote=True)}" '
                             f'title="{title}">{label}</a>')
            else:
                links.append(f'<span title="{title}">{label}</span>')
        h += f'    <p class="def-instances">{", ".join(links)}</p>\n'
        h += '  </div>\n'
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
        summary = md.get('summary_full') or md['summary']
        dash_idx = summary.find(' -- ')
        if dash_idx != -1:
            summary = summary[dash_idx + 4:]
        content += '  <div class="module-doc">\n'
        if summary:
            content += f'    <p class="module-doc-summary">{_inline_html(summary)}</p>\n'
        content += _prose_html(md.get('description', ''), indent='    ')
        if md.get('example'):
            content += '    <div class="def-section">\n'
            content += '      <div class="def-section-label">Example</div>\n'
            content += f'      <pre class="def-example">{html_module.escape(md["example"])}</pre>\n'
            content += '    </div>\n'
        content += _prose_html(md.get('notes', ''), indent='    ')
        if md.get('deprecated'):
            content += '    <p class="def-deprecated"><span class="def-deprecated-label">Deprecated</span> '
            content += f'{_inline_html(md["deprecated"])}</p>\n'
        if md.get('since'):
            content += f'    <p class="def-since">Since: {_inline_html(md["since"])}</p>\n'
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
                content += f'<span class="internal-item-summary">-- {_inline_html(summary_text)}</span>'
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
        h += f'    <p>{_inline_html(mod_summary)}</p>\n'
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
    content += '  <p style="color:var(--faint);margin-bottom:1.5rem">Auto-generated API reference. Run <code>tur run docs</code> to regenerate.</p>\n'

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


def _class_keys(modules):
    """Names every parsed defclass declares.  A definstance's lookup key is its
    class name (`Functor[Option]` -> `Functor`), so without this the first
    instance the scan met -- often in another module -- answered `(doc
    'Functor)` instead of the class."""
    return {d['name'] for m in modules for d in m['definitions'] if d['kind'] == 'defclass'}


def _method_doc_entries(modules):
    """
    [(key, summary, text)] for each class method: `(doc 'extract)` should say
    what `extract` is even though no defn of that name exists.  Callers add an
    entry only for a key nothing else claims -- a method name such as `drop` or
    `first` is also an ordinary function somewhere, and that one keeps it.
    """
    out = []
    for module in modules:
        for d in module['definitions']:
            if d['kind'] != 'defclass':
                continue
            rows = dict(_class_method_rows(d))
            for meth in d.get('methods', []):
                sig = _method_signature(meth)
                desc = ' '.join((rows.get(sig) or '').split())
                summary = f"{meth['name']} -- {d['name']} method" + (f': {desc}' if desc else '.')
                text = '\n'.join([summary, '', 'Signature:', f'  {sig}', '',
                                  f"Class: {_render_signature(d)} in {module['name']}"])
                out.append((meth['name'], summary, text))
    return out


def _build_doc_entry(defn):
    """Build the full text for a doc entry."""
    doc = defn['docstring']
    if defn.get('kind') == 'defclass':
        return _build_class_doc_entry(defn)
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


def _build_class_doc_entry(defn):
    """The doc-lookup text for a defclass: its header, then every method's
    signature with what it does, then the laws and example."""
    doc = defn['docstring'] or {}
    parts = [doc.get('summary') or defn['name'], '', _render_signature(defn)]
    if doc.get('deprecated'):
        parts += ['', 'Deprecated:'] + [f'  {l}' for l in doc['deprecated'].splitlines()]
    rows = _class_method_rows(defn)
    if rows:
        parts += ['', 'Methods:']
        for sig, desc in rows:
            desc = ' '.join(desc.split())
            parts.append(f'  {sig} -- {desc}' if sig and desc else f'  {sig or desc}')
    if doc.get('laws'):
        parts += ['', 'Laws:'] + [f'  {l}' for l in doc['laws'].splitlines()]
    if doc.get('example'):
        parts += ['', 'Example:'] + [f'  {l}' for l in doc['example'].splitlines()]
    if doc.get('since'):
        parts += ['', f'Since: {doc["since"]}']
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
    `tur run docs` in a fresh clone delete the whole table -- silently, into a
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
    class_keys = _class_keys(modules)
    for module in modules:
        for defn in module['definitions']:
            name = defn['name']
            # Strip typeclass bracket suffix for lookup key
            key = re.sub(r'\[.*\]$', '', name)
            if defn['kind'] == 'definstance' and key in class_keys:
                continue   # the class itself answers for its name
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

    # Class methods last, so any definition of the same name keeps its key.
    entries += [(key, text) for key, _summary, text in _method_doc_entries(modules)]

    # Deduplicate: keep the first entry for each key
    seen = {}
    deduped = []
    for key, text in entries:
        if key not in seen:
            seen[key] = True
            deduped.append((key, text))

    lines = [
        ';; AUTO-GENERATED -- do not edit. Run: tur run docs',
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
    class_keys = _class_keys(modules)
    for module in modules:
        for defn in module['definitions']:
            name = defn['name']
            key = re.sub(r'\[.*\]$', '', name)
            if key in seen:
                continue
            if defn['kind'] == 'definstance' and key in class_keys:
                continue   # the class itself answers for its name
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

    # Class methods last, so any definition of the same name keeps its key.
    for key, summary, _text in _method_doc_entries(modules):
        if key not in seen:
            seen.add(key)
            entry = {'name': key, 'summary': summary, 'kind': 'method'}
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
        # A module with no git history yet (new, uncommitted) has no dates.
        dates = (page_dates.get(Path(source)) if source else None) or {}
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


def attach_class_instances(modules, *, link_other_modules):
    """
    Give every defclass the instances the parsed tree declares for it, as
    defn['instances'] = [(type, href, module name), ...]: "which types are
    Functors?" is the first thing a reader of a class asks, and the instances
    are spread over the modules that own the types.

    An instance in the class's own module links to its card (`#anchor`).  One
    in another module links to that page only when link_other_modules: the
    stdlib pages are siblings and spell the link `../api/<page>`, which both
    resolves on the site and is the form genpack rewrites into the docs pack.
    A spice's pages live elsewhere, so there the type is listed unlinked.
    """
    by_class = {}
    for module in modules:
        for d in module['definitions']:
            if d['kind'] != 'definstance':
                continue
            cls, _, type_name = d['name'].partition('[')
            type_name = type_name[:-1] if type_name.endswith(']') else type_name
            if type_name:
                anchor = re.sub(r'[^a-zA-Z0-9_\-]', '_', d['name'])
                by_class.setdefault(cls, []).append((type_name, module, anchor))
    for module in modules:
        for d in module['definitions']:
            if d['kind'] != 'defclass':
                continue
            seen, out = set(), []
            for type_name, owner, anchor in by_class.get(d['name'], []):
                if type_name in seen:
                    continue
                seen.add(type_name)
                if owner is module:
                    href = '#' + anchor
                elif link_other_modules:
                    href = f'../api/{module_page_name(owner)}#{anchor}'
                else:
                    href = None
                out.append((type_name, href, owner['name']))
            d['instances'] = out


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

    attach_class_instances(modules, link_other_modules=(brand == 'stdlib'))

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
                  f'Run `tur run doctest` to refresh it.', file=sys.stderr)
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
