"""
tools/mdrender.py -- render Markdown for the docs generators with tur/markdown.

genguides.py and genspices.py turn guide and README Markdown into HTML through
this module, which runs tools/md/render.tur (markdown-library-plan M4): the
stdlib's CommonMark + GFM renderer, the same rules GitHub renders the sources
with. It replaced python-markdown, the docs path's one third-party package.

turmeric-spices carries a verbatim copy of this file and of md/render.tur, for
its own genguides.py and genspices.py, so nothing here names either
repository's layout beyond where each keeps a `tur`. Change both copies
together.

`render_batch` takes every page of a run at once, so the renderer starts once
per generator. It writes the sources into a scratch directory, runs the tool
over them, and reads back each page's HTML fragment, its headings (as the
nested toc tokens the page chrome is built from) and the raw HTML it passed
through.

The tool is compiled with `tur build` on first use and cached under the
system temp directory, keyed on the `tur` binary, the tool and the stdlib it
compiles against, so a second generator in the same run reuses it; a cached
build a week old is deleted when the next one is made. `tur` is found as
$TUR, then in the checkout (turmeric's build/, or the release turmeric-spices's
scripts/install-tur.sh unpacks into vendor/tur/), then on PATH. It needs a
stdlib with markdown/heading-html: turmeric main, or a release made after M4.
"""

import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
TOOL_SRC = HERE / 'md' / 'render.tur'

_EXE = 'tur.exe' if os.name == 'nt' else 'tur'
# Where a checkout keeps a tur of its own: turmeric's build tree, and the two
# layouts of the release turmeric-spices's scripts/install-tur.sh unpacks.
_CHECKOUT_TURS = (Path('build') / _EXE,
                  Path('vendor') / 'tur' / 'bin' / _EXE,
                  Path('vendor') / 'tur' / _EXE)


def find_tur() -> Path:
    """The `tur` that builds the renderer: $TUR, the checkout's, or PATH's."""
    env = os.environ.get('TUR')
    if env:
        return Path(env)
    for rel in _CHECKOUT_TURS:
        if (REPO / rel).is_file():
            return REPO / rel
    found = shutil.which('tur')
    if found:
        return Path(found)
    sys.exit(
        'error: rendering Markdown needs a tur binary (tools/md/render.tur)\n'
        '       in turmeric, build one:  cmake -S . -B build && cmake --build build -j\n'
        '       in turmeric-spices, fetch one:  ./scripts/install-tur.sh\n'
        '       or point TUR at one:  TUR=/path/to/tur'
    )


def _stdlib_dir(tur: Path) -> Path | None:
    """The stdlib `tur` compiles against, as near as can be told from outside.

    $TUR_STDLIB_DIR, else beside the binary: a release's share/turmeric/stdlib
    (or the flat stdlib/ of older ones), or a build tree's ../stdlib. None when
    none of those holds markdown.tur.
    """
    env = os.environ.get('TUR_STDLIB_DIR')
    bin_dir = tur.resolve().parent
    for cand in ([Path(env)] if env else []) + [
            bin_dir.parent / 'share' / 'turmeric' / 'stdlib',
            bin_dir / 'stdlib',
            bin_dir.parent / 'stdlib']:
        if (cand / 'markdown.tur').is_file():
            return cand
    return None


def _cache_key(tur: Path) -> str:
    """A digest of what the built renderer depends on.

    The `tur` binary (by path, size and mtime: rebuilding it changes the
    compiler), the tool's bytes, and every source of the stdlib it compiles
    against, since the tool's `load`s reach well past stdlib/markdown/.
    """
    h = hashlib.sha256()
    st = tur.stat()
    h.update(f'{tur.resolve()}\0{st.st_size}\0{st.st_mtime_ns}\0'.encode())
    h.update(TOOL_SRC.read_bytes())
    stdlib = _stdlib_dir(tur)
    if stdlib is not None:
        for p in sorted(stdlib.rglob('*.tur')):
            h.update(str(p.relative_to(stdlib)).encode() + b'\0')
            h.update(p.read_bytes())
    return h.hexdigest()[:20]


# A cached build nobody has rebuilt for this long is deleted the next time
# a build is made, so the cache does not grow by one binary per stdlib edit.
_STALE_SECONDS = 7 * 24 * 3600


def build_tool() -> Path:
    """Compile tools/md/render.tur once, or reuse the cached build."""
    tur = find_tur()
    cache = Path(tempfile.gettempdir()) / 'tur-md-render'
    exe = cache / f'render-{_cache_key(tur)}'
    if exe.is_file():
        return exe
    cache.mkdir(parents=True, exist_ok=True)
    # Built under a temporary name and renamed into place, so a generator
    # running alongside never sees a half-written binary.
    tmp = cache / f'.render-{os.getpid()}'
    r = subprocess.run([str(tur), 'build', str(TOOL_SRC), '-o', str(tmp)],
                       cwd=REPO, capture_output=True, text=True)
    if r.returncode != 0 or not tmp.is_file():
        tmp.unlink(missing_ok=True)
        sys.stderr.write(r.stdout + r.stderr)
        sys.exit(f'error: {tur} build {TOOL_SRC.relative_to(REPO)} failed\n'
                 '       the renderer needs tur/markdown with markdown/heading-html:\n'
                 '       a tur from turmeric main, or a release made after it')
    os.replace(tmp, exe)
    now = time.time()
    for old in cache.glob('render-*'):
        try:
            if old != exe and now - old.stat().st_mtime > _STALE_SECONDS:
                old.unlink()
        except OSError:
            pass                    # another run's build, gone or in use
    return exe


def strip_tags(html: str) -> str:
    """A heading's HTML as the text a table of contents shows.

    Comments, then tags, taken out and whitespace runs collapsed; character
    references are kept, since the result is written back into HTML, except
    that a quote is a plain `"` (text needs no `&quot;`). This is the `name`
    python-markdown's toc extension gave each heading, so the contents boxes,
    the sidebars and the docs pack's search words read as they did.
    """
    while (a := html.find('<!--')) != -1 and (z := html.find('-->', a)) != -1:
        html = html[:a] + html[z + 3:]
    while (a := html.find('<')) != -1 and (z := html.find('>', a)) != -1:
        html = html[:a] + html[z + 1:]
    return ' '.join(html.replace('&quot;', '"').split())


def nest_headings(flat: list) -> list:
    """Nest a page's headings into toc tokens: each one under the nearest
    heading before it with a lower level.

    A token is {'level', 'id', 'name', 'html', 'children'}. A heading with no
    such heading before it is a root, so a page that opens at ## or skips a
    level still lists every heading.
    """
    roots: list = []
    stack: list = []
    for h in flat:
        tok = {'level': h['level'], 'id': h['id'], 'name': strip_tags(h['html']),
               'html': h['html'], 'children': []}
        while stack and stack[-1]['level'] >= tok['level']:
            stack.pop()
        (stack[-1]['children'] if stack else roots).append(tok)
        stack.append(tok)
    return roots


def render_batch(sources: dict) -> dict:
    """Render {key: markdown text} to {key: page}.

    A page is {'html': fragment, 'toc_tokens': nested headings,
    'raw_html': [{'line', 'html'}, ...]}. Line numbers count from the first
    line of the text as given, so a caller that blanks out front matter
    rather than cutting it keeps them true to the file.
    """
    if not sources:
        return {}
    exe = build_tool()
    with tempfile.TemporaryDirectory(prefix='tur-md-') as tmp:
        work = Path(tmp)
        names = {}
        for i, (key, text) in enumerate(sources.items()):
            path = work / f'p{i}.md'
            path.write_text(text, encoding='utf-8')
            names[key] = path
        lst = work / 'list.txt'
        lst.write_text(''.join(f'{p}\n' for p in names.values()), encoding='utf-8')
        r = subprocess.run([str(exe), f'--list={lst}'], capture_output=True, text=True)
        if r.returncode != 0:
            sys.stderr.write(r.stdout + r.stderr)
            sys.exit(f'error: the Markdown renderer failed (exit {r.returncode})')
        out = {}
        for key, path in names.items():
            meta = json.loads(path.with_suffix('.json').read_text(encoding='utf-8'))
            out[key] = {
                'html': path.with_suffix('.html').read_text(encoding='utf-8'),
                'toc_tokens': nest_headings(meta['headings']),
                'raw_html': meta['raw_html'],
            }
        return out
