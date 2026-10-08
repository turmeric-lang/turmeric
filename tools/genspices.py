#!/usr/bin/env python3
"""
tools/genspices.py -- Generate per-spice documentation pages.

For each spice in ../turmeric-spices/spices/<name>/ produces:
  docs/html/spices/<name>/index.html  -- front page rendered from README.md
  docs/html/spices/<name>/api/        -- auto-generated API reference from src/

Also produces the top-level index at docs/html/spices/index.html with one row
per spice (tier, description, links to the front page and API reference).

Requires the sibling repo ../turmeric-spices/ to be present; the previous
GitHub-fetch fallback was dropped because per-spice generation needs the
full source tree, not just one README.

Usage:
    python3 tools/genspices.py [--out docs/html/spices/]
"""

import argparse
import html as html_module
import re
import sys
from pathlib import Path

try:
    import markdown as md_lib
except ImportError:  # pragma: no cover -- preflight, not a code path
    sys.exit(
        "error: tools/genspices.py needs the 'markdown' package\n"
        "       install it with:  python3 -m pip install -r tools/requirements.txt\n"
        "       (or:  python3 -m pip install markdown)"
    )

sys.path.insert(0, str(Path(__file__).parent))
from genguides import (SIDEBAR_DRAWER_JS_SRC, GUIDE_RUNTIME_JS_SRC,
                       font_links, script_tag, sidebar_drawer,
                       guide_runtime, write_page_scripts, GUIDE_CSS,
                       inject_syntax_toggles, toc_tokens_to_sidebar,
                       build_page_header, build_sidebar, MAIN_SITE)
from gendocs import render_tree, collect_doc_entries
import packlib

# Single source of truth for this generator's repo links -- a transfer
# (personal account -> org) is a one-line change here.
GITHUB_BASE = 'https://github.com/turmeric-lang/turmeric-spices'
SPICES_REPO = Path('../turmeric-spices')

# Spice pages are served from spices.turmeric-lang.com, so every site-relative
# link in the shared chrome has to be re-rooted at the main host.
PAGE_HEADER = build_page_header(active='Spices', base=MAIN_SITE)

# The spices index is the third of the site's three index pages, so it carries
# the same filter box the guides and API indexes do -- one topbar, one shape.
# It filters table rows rather than cards, which is the only difference.
INDEX_PAGE_HEADER = (
    build_page_header(active='Spices', base=MAIN_SITE, search='Filter spices')
    + '\n  <p class="search-no-results">No matching spices.</p>'
)

# The index's filter box, shipped as spices-index.js beside the index page (no
# inline scripts under the site's CSP -- see genguides.write_page_scripts).
INDEX_FILTER_JS_SRC = '''\
  document.addEventListener('DOMContentLoaded', function(){
    var input = document.querySelector('.search-input');
    var table = document.querySelector('.spices-table');
    if (!input || !table) return;
    var rows = Array.prototype.slice.call(table.tBodies[0].rows);
    var noResults = document.querySelector('.search-no-results');

    function filter() {
      var q = input.value.trim().toLowerCase();
      var shown = 0;
      rows.forEach(function(tr){
        var match = !q || tr.textContent.toLowerCase().indexOf(q) !== -1;
        tr.style.display = match ? '' : 'none';
        if (match) shown++;
      });
      if (noResults) noResults.style.display = (q && shown === 0) ? 'block' : 'none';
    }

    input.addEventListener('input', filter);
    input.addEventListener('keydown', function(e){
      if (e.key === 'Escape') { input.value = ''; filter(); input.blur(); }
    });
    document.addEventListener('keydown', function(e){
      if (e.key === '/' && document.activeElement !== input &&
          document.activeElement.tagName !== 'INPUT' &&
          document.activeElement.tagName !== 'TEXTAREA') {
        e.preventDefault();
        input.focus();
      }
    });
  });'''

# Written once into the spices output directory: the index loads them by bare
# name, each spice's front page (one level down) through `../`.
SPICE_PAGE_SCRIPTS = {
    'site-drawer.js':   SIDEBAR_DRAWER_JS_SRC,
    'guide-runtime.js': GUIDE_RUNTIME_JS_SRC,
    'spices-index.js':  INDEX_FILTER_JS_SRC,
}


# ---------------------------------------------------------------------------
# Spice metadata
# ---------------------------------------------------------------------------

SpiceMeta = dict  # {name, description, tier, c_dep}


def discover_spices() -> list[Path]:
    """Return sorted spice directories under ../turmeric-spices/spices/.

    Returns an empty list (with a stderr warning) when the sibling repo is
    absent, so `tur run docs` degrades gracefully in environments without the
    optional ../turmeric-spices/ checkout (e.g. CI) -- matching the codebase's
    "spices absent -> skip, don't fail" convention. Callers that emit JSON
    still write an empty array so the downstream gendocs --extra-json step
    finds its file.
    """
    root = SPICES_REPO / 'spices'
    if not root.is_dir():
        print(
            f'warning: sibling spices repo not found at {SPICES_REPO.resolve()}; '
            f'skipping spice docs. Clone it next to this checkout, or run '
            f'`tur fetch`, to include them.',
            file=sys.stderr,
        )
        return []
    return sorted(p for p in root.iterdir() if p.is_dir())


def parse_readme_table(readme_text: str) -> dict[str, SpiceMeta]:
    """
    Parse the spices table in the top-level README.md and return a dict keyed
    by spice short-name (e.g. 'json' for 'tur-json').
    """
    rows: dict[str, SpiceMeta] = {}
    # Match table rows like: | [`tur-foo`](spices/foo/) | desc | tier | c dep |
    row_re = re.compile(
        r'^\|\s*\[`tur-([\w\-]+)`\]\(spices/[^)]+\)\s*\|'
        r'\s*([^|]+?)\s*\|'
        r'\s*([^|]+?)\s*\|'
        r'\s*([^|]+?)\s*\|',
        re.MULTILINE,
    )
    for m in row_re.finditer(readme_text):
        name, desc, tier, c_dep = m.groups()
        rows[name] = {
            'name': name,
            'description': desc.strip(),
            'tier': tier.strip(),
            'c_dep': c_dep.strip(),
        }
    return rows


def extract_build_description(build_tur: Path) -> str:
    """Read :description from a build.tur file, or return ''."""
    if not build_tur.is_file():
        return ''
    text = build_tur.read_text(encoding='utf-8', errors='replace')
    m = re.search(r':description\s+"([^"]+)"', text)
    return m.group(1) if m else ''


def collect_spice_meta(spice_dirs: list[Path],
                       table: dict[str, SpiceMeta]) -> list[SpiceMeta]:
    """Merge README table info with on-disk discovery, falling back to build.tur."""
    out: list[SpiceMeta] = []
    for d in spice_dirs:
        name = d.name
        meta = dict(table.get(name, {}))
        meta['name'] = name
        meta['path'] = d
        if not meta.get('description'):
            meta['description'] = extract_build_description(d / 'build.tur')
        meta.setdefault('tier', '--')
        meta.setdefault('c_dep', '--')
        out.append(meta)
    return out


# ---------------------------------------------------------------------------
# Per-spice front page
# ---------------------------------------------------------------------------

STUB_FRONT_PAGE = '''\
# tur-{name}

Docs in progress.

## See also

- [API reference](api/)
- Source: <{github}/tree/main/spices/{name}>
'''


def render_front_page(meta: SpiceMeta, out_dir: Path, style_rel: str,
                      pack_dir: Path | None = None) -> dict | None:
    """Render docs/html/spices/<name>/index.html from the spice's README.md.

    When `pack_dir` is given, the same rendered body is also written into the
    docs pack as `spices/<name>.html` -- one rendering pass, two wrappers, so
    the site page and Try's in-app pane cannot drift. Returns the pack index
    entry (or None when not emitting a pack).
    """
    out_dir.mkdir(parents=True, exist_ok=True)
    readme = meta['path'] / 'README.md'

    if readme.is_file():
        text = readme.read_text(encoding='utf-8')
        source_label = f'spices/{meta["name"]}/README.md'
    else:
        text = STUB_FRONT_PAGE.format(name=meta['name'], github=GITHUB_BASE)
        source_label = '(no README -- stub)'

    text = re.sub(r'^(`{3,})(turmeric|sweet-exp)\s+no-check\b[^\n]*', r'\1\2', text,
                  flags=re.MULTILINE)
    conv = md_lib.Markdown(
        extensions=['fenced_code', 'tables', 'toc'],
        extension_configs={'toc': {'permalink': False}},
    )
    body_html = conv.convert(text)
    body_html = inject_syntax_toggles(body_html)
    toc_tokens = getattr(conv, 'toc_tokens', [])

    sidebar_items = toc_tokens_to_sidebar(toc_tokens)
    sidebar_html = build_sidebar(
        toc=f'      <h3>On this page</h3>\n      <ul>{sidebar_items}</ul>',
        uplinks=[('../index.html', 'All Spices'), ('api/', 'API reference')],
        base=MAIN_SITE,
        extra_titles={'../index.html': 'Every first-party spice',
                      'api/': f'API reference for tur-{meta["name"]}'},
    )

    title = f'tur-{meta["name"]} | Turmeric Spices'

    html = f'''<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>{html_module.escape(title)}</title>
  <link rel="icon" type="image/svg+xml" href="/favicon.svg">
{font_links()}
  <link rel="stylesheet" href="{style_rel}">
  <style>
{GUIDE_CSS}
  </style>
</head>
<body>
{PAGE_HEADER}
{sidebar_drawer('../')}
  <div class="page-layout">
    <div class="sidebar">
      {sidebar_html}
    </div>
    <div class="content guide-content">
      {body_html}
    </div>
  </div>
  <footer class="site-footer">
    Auto-generated by <code>tools/genspices.py</code> &mdash; source: <code>{html_module.escape(source_label)}</code>
  </footer>
{guide_runtime('../')}
</body>
</html>
'''
    (out_dir / 'index.html').write_text(html, encoding='utf-8')

    if pack_dir is None:
        return None

    name = meta['name']
    rel = f'spices/{name}.html'
    size = packlib.write_fragment(pack_dir, rel, body_html)
    description = (meta.get('description', '') or '').strip()
    return {
        'slug': name,
        'path': rel,
        'title': f'tur-{name}',
        'category': 'Spices',
        'description': description,
        'bytes': size,
        'words': packlib.search_string(
            f'tur-{name}', name, description,
            *packlib.heading_names(toc_tokens),
            prose=packlib.strip_tags(body_html)),
    }


# ---------------------------------------------------------------------------
# Per-spice API reference (delegates to gendocs.render_tree)
# ---------------------------------------------------------------------------

def render_api_reference(meta: SpiceMeta, out_dir: Path,
                         pack_dir: Path | None = None):
    """
    Generate the per-spice API tree under out_dir/api/.
    Returns the parsed module list (for collect_doc_entries), or None when
    the spice has no src/ directory.

    With `pack_dir`, the spice's module fragments also land in the pack under
    `spices/<spice>/<module>.html` -- namespaced by spice so a module name
    shared with the stdlib cannot collide.
    """
    src_dir = meta['path'] / 'src'
    if not src_dir.is_dir():
        return None
    api_out = out_dir / 'api'
    return render_tree(
        src_dir,
        api_out,
        brand=f'tur-{meta["name"]}',
        brand_label=f'tur-{meta["name"]} API',
        emit_pack=pack_dir,
        pack_section='spices',
        pack_slug_prefix=f'{meta["name"]}/',
        # Served from the spices subdomain, so the shared chrome's `/tour`-style
        # links have to be re-rooted at the main host.
        site_base=MAIN_SITE,
        index_uplinks=[('../', f'tur-{meta["name"]} docs'),
                       ('../../index.html', 'All Spices')],
        index_uplink_titles={'../': f'Front page for tur-{meta["name"]}',
                             '../../index.html': 'Every first-party spice'},
    )


# ---------------------------------------------------------------------------
# Top-level spices index
# ---------------------------------------------------------------------------

def render_top_index(metas: list[SpiceMeta], out_dir: Path) -> None:
    """Render docs/html/spices/index.html with one row per spice."""
    out_dir.mkdir(parents=True, exist_ok=True)

    rows = []
    for meta in metas:
        name = meta['name']
        desc = meta.get('description', '') or ''
        tier = meta.get('tier', '--')
        c_dep = meta.get('c_dep', '--')
        rows.append(
            '      <tr>'
            f'<td><a href="{html_module.escape(name)}/"><code>tur-{html_module.escape(name)}</code></a></td>'
            f'<td>{html_module.escape(desc)}</td>'
            f'<td>{html_module.escape(tier)}</td>'
            f'<td>{html_module.escape(c_dep)}</td>'
            f'<td><a href="{html_module.escape(name)}/api/">API</a></td>'
            '</tr>'
        )
    table_html = (
        '<div class="spices-table-wrap">\n'
        '<table class="spices-table">\n'
        '  <thead><tr>'
        '<th>Spice</th><th>Description</th><th>Tier</th><th>C dep</th><th>Docs</th>'
        '</tr></thead>\n'
        '  <tbody>\n'
        + '\n'.join(rows)
        + '\n  </tbody>\n</table>\n'
        '</div>'
    )

    intro = (
        f'<p>{len(metas)} first-party spices for the Turmeric ecosystem. '
        'Each spice has its own docs -- click through for a front page and '
        'a per-spice API reference.</p>'
    )

    sidebar_html = build_sidebar(
        toc=('      <h3>About</h3>\n'
             '      <ul>\n'
             f'        <li><a href="{GITHUB_BASE}">Spices on GitHub</a></li>\n'
             '      </ul>'),
        base=MAIN_SITE,
        extra_titles={GITHUB_BASE: 'Spice sources on GitHub'},
    )

    html = f'''<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Spices | Turmeric</title>
  <link rel="icon" type="image/svg+xml" href="/favicon.svg">
{font_links()}
  <link rel="stylesheet" href="../api/style.css">
  <style>
{GUIDE_CSS}
    .spices-table-wrap {{ width:100%; margin-top:1rem; overflow-x:auto; -webkit-overflow-scrolling:touch; }}
    .spices-table {{ width:100%; min-width:560px; }}
    .spices-table td code {{ font-size:0.85rem; }}
  </style>
</head>
<body>
{INDEX_PAGE_HEADER}
{sidebar_drawer()}
  <div class="page-layout">
    <div class="sidebar">
      {sidebar_html}
    </div>
    <div class="content guide-content">
      <h1>Spices</h1>
      {intro}
      {table_html}
    </div>
  </div>
  <footer class="site-footer">
    Auto-generated by <code>tools/genspices.py</code>
  </footer>
{guide_runtime()}
{script_tag('spices-index.js')}
</body>
</html>
'''
    (out_dir / 'index.html').write_text(html, encoding='utf-8')


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> None:
    import json

    p = argparse.ArgumentParser(description='Generate per-spice doc pages.')
    p.add_argument('--out', default='docs/html/spices/', help='Output directory')
    p.add_argument(
        '--emit-json',
        metavar='PATH',
        help='Write a JSON array of {name, summary, kind, spice} entries to PATH. '
             'Used by `just docs` to fold spice symbols into web/public/doc-names.json.',
    )
    p.add_argument(
        '--emit-pack',
        metavar='DIR',
        help='Also write chrome-free spice fragments into the docs pack at DIR '
             '(see tools/genpack.py). Spices absent from this checkout simply '
             'contribute nothing, so the pack never advertises a page it does '
             'not carry.',
    )
    args = p.parse_args()

    out_dir = Path(args.out)
    pack_dir = Path(args.emit_pack) if args.emit_pack else None

    spice_dirs = discover_spices()
    readme_path = SPICES_REPO / 'README.md'
    table = parse_readme_table(readme_path.read_text(encoding='utf-8')) \
        if readme_path.is_file() else {}
    metas = collect_spice_meta(spice_dirs, table)

    print(f'Generating docs for {len(metas)} spices into {out_dir}/')
    write_page_scripts(out_dir, SPICE_PAGE_SCRIPTS)
    all_entries: list[dict] = []
    pack_entries: list[dict] = []
    for meta in metas:
        print(f'-> {meta["name"]}')
        spice_out = out_dir / meta['name']
        # Front page links to /docs/html/api/style.css via two-level relative path
        front = render_front_page(meta, spice_out, style_rel='../../api/style.css',
                                  pack_dir=pack_dir)
        if front:
            pack_entries.append(front)
        modules = render_api_reference(meta, spice_out, pack_dir=pack_dir)
        if modules is None:
            print(f'   (no src/ directory; skipping API reference)')
            continue
        all_entries.extend(collect_doc_entries(modules, spice=meta['name']))

    if pack_dir is not None:
        # Front pages go in alongside whatever render_api_reference already
        # merged into the `spices` sidecar.
        packlib.write_sidecar(pack_dir, 'spices', pack_entries)

    render_top_index(metas, out_dir)
    if args.emit_json:
        out_json = Path(args.emit_json)
        out_json.parent.mkdir(parents=True, exist_ok=True)
        out_json.write_text(
            json.dumps(all_entries, ensure_ascii=True, indent=None,
                       separators=(',', ':')),
            encoding='utf-8',
        )
        print(f'Wrote {out_json} ({len(all_entries)} spice doc entries)')
    print(f'Done: {out_dir / "index.html"}')


if __name__ == '__main__':
    main()
