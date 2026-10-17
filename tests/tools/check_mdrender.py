#!/usr/bin/env python3
"""
tests/tools/check_mdrender.py -- tools/mdrender.py and tools/md/render.tur, the
docs generators' Markdown renderer (markdown-library-plan M4).

genguides.py and genspices.py read three things back from the renderer for
every page: the HTML fragment, the headings (nested toc tokens with
python-markdown's ids and names, which the contents boxes, the sidebars and
the docs pack's search are built from) and the raw HTML it passed through
(with the line each starts on, since the guides' build fails on it). This
renders a few pages through one render_batch call and checks each.

Needs a tur (mdrender.find_tur: $TUR, ./build/tur, PATH); the first run builds
the renderer, which takes a few seconds.
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, 'tools'))

import mdrender  # noqa: E402
from genguides import mark_task_items  # noqa: E402

PAGES = {
    # Headings: nesting, python-markdown's ids (a repeat gets _1, NFKD makes
    # the e of Cafe ASCII), and the toc name -- the HTML with its tags out and
    # a plain quote. Three blank lines first, as genguides leaves in place of
    # front matter.
    'headings': '\n\n\n# Title\n\nIntro.\n\n## Setup -- "quick"\n\n'
                '### Use `make`\n\n## Setup -- "quick"\n\n#### Deep\n\n'
                '# Café\n',
    # Raw HTML: an inline tag (reported at its paragraph's first line) and an
    # HTML block; a tag in a code span is not raw HTML.
    'raw': 'line one\n\nText with\nptr<void> here.\n\n<div>\nx\n</div>\n\n'
           '`<code-span>` is fine.\n',
    # GFM: a task list, a table, the tag filter (the block is still reported).
    'gfm': '- [ ] open\n- [x] done\n\n| a | b |\n|---|--:|\n| 1 | 2 |\n\n'
           '<script>alert(1)</script>\n',
    # The JSON the tool writes: a backslash, a quote and a control byte in a
    # heading survive the trip.
    'escape': '# a\\b "c" d\x01e\n',
    'empty': '',
}

failures = []


def check(what, got, want):
    if got != want:
        failures.append(f'{what}:\n    got  {got!r}\n    want {want!r}')


def flat(tokens):
    for t in tokens:
        yield t
        yield from flat(t['children'])


pages = mdrender.render_batch(PAGES)
check('page keys', sorted(pages), sorted(PAGES))

# -- headings ----------------------------------------------------------------
toc = pages['headings']['toc_tokens']
shape = [(t['level'], t['id'], [(c['level'], c['id'], [g['id'] for g in c['children']])
                                for c in t['children']])
         for t in toc]
check('heading nesting and ids', shape, [
    (1, 'title', [(2, 'setup-quick', ['use-make']),
                  (2, 'setup-quick_1', ['deep'])]),
    (1, 'cafe', []),
])
names = [t['name'] for t in flat(toc)]
check('heading names', names,
      ['Title', 'Setup -- "quick"', 'Use make', 'Setup -- "quick"', 'Deep', 'Café'])
check('heading html', [t['html'] for t in flat(toc)][1:3],
      ['Setup -- &quot;quick&quot;', 'Use <code>make</code>'])
html = pages['headings']['html']
for frag in ('<h1 id="title">Title</h1>', '<h2 id="setup-quick_1">',
             '<h4 id="deep">Deep</h4>', '<h1 id="cafe">Café</h1>'):
    if frag not in html:
        failures.append(f'headings html lacks {frag!r}')
check('no raw HTML in headings page', pages['headings']['raw_html'], [])

# -- raw HTML ----------------------------------------------------------------
raw = pages['raw']['raw_html']
check('raw HTML lines', [r['line'] for r in raw], [3, 6])
check('raw HTML inline', raw[0]['html'] if raw else None, '<void>')
check('raw HTML block', raw[1]['html'].strip() if len(raw) > 1 else None,
      '<div>\nx\n</div>')

# -- GFM ---------------------------------------------------------------------
gfm = pages['gfm']['html']
for frag in ('<li><input type="checkbox" disabled="" /> open</li>',
             '<li><input type="checkbox" checked="" disabled="" /> done</li>',
             '<th align="right">b</th>', '&lt;script>alert(1)&lt;/script>'):
    if frag not in gfm:
        failures.append(f'gfm html lacks {frag!r}')
check('tag-filtered block still reported',
      [r['line'] for r in pages['gfm']['raw_html']], [8])
check('task items get the CSS class',
      mark_task_items(gfm).count('<li class="task-item"><input type="checkbox"'), 2)

# -- escaping and the empty page ---------------------------------------------
esc = pages['escape']['toc_tokens']
check('escaped heading name', esc[0]['name'] if esc else None, 'a\\b "c" d\x01e')
check('empty page', pages['empty'], {'html': '', 'toc_tokens': [], 'raw_html': []})

# -- the build is cached -----------------------------------------------------
check('second build reuses the first', mdrender.build_tool(), mdrender.build_tool())

if failures:
    for f in failures:
        print(f'FAIL {f}')
    sys.exit(1)
print(f'PASS check-mdrender ({len(PAGES)} pages through one renderer run)')
