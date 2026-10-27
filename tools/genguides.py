#!/usr/bin/env python3
"""
tools/genguides.py -- Render Turmeric guide markdown files to HTML.

Usage:
    python3 tools/genguides.py docs/guides/ [--out docs/html/guides/]
                                            [--emit-pack web/public/docs-pack/]

Two consumers, one rendering pass. `build_guide_bodies` converts each guide's
markdown into its article body exactly once; `render_guide` wraps that body in
site chrome for docs/html/guides/, and `emit_pack_fragment` writes the same
body -- chrome-free, links rewritten into the pack's `#doc=` URL space -- into
the docs pack that Try Turmeric renders in-app and precaches for offline use.
The two outputs cannot drift because there is only one renderer.
"""

import argparse
import html as _html
import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import mdrender  # noqa: E402  (sibling module, path fixed up just above)
import packlib  # noqa: E402


# The Turmeric repo on GitHub. Single source of truth for this generator --
# every rendered page's nav link, link title and "source:" footer derives from
# it, so a repo transfer (personal account -> org) is a one-line change.
# web/site.js carries the same constant for the hand-written pages; the comment
# on SIDEBAR_GROUPS below explains why these two lists are deliberately
# duplicated rather than shared.
GITHUB_URL = 'https://github.com/turmeric-lang/turmeric'


def get_creation_date(path: Path, repo_root: Path) -> str | None:
    """Return the YYYY-MM-DD date the file was first added to git, or None."""
    try:
        rel = path.resolve().relative_to(repo_root.resolve())
    except ValueError:
        rel = path
    try:
        result = subprocess.run(
            ['git', 'log', '--diff-filter=A', '--follow',
             '--format=%ai', '--', str(rel)],
            cwd=repo_root, capture_output=True, text=True, check=False,
        )
    except FileNotFoundError:
        return None
    if result.returncode != 0:
        return None
    lines = [ln.strip() for ln in result.stdout.splitlines() if ln.strip()]
    if not lines:
        return None
    return lines[-1].split(' ', 1)[0]


def parse_front_matter(text: str) -> tuple[dict, str]:
    """Return (meta_dict, body) after stripping YAML front matter (--- blocks)."""
    if not text.startswith('---\n'):
        return {}, text
    end = text.find('\n---', 4)
    if end == -1:
        return {}, text
    fm_text = text[4:end]
    rest_start = text.find('\n', end + 1)
    body = text[rest_start + 1:] if rest_start != -1 else ''
    meta: dict = {}

    def _unquote(v: str) -> str:
        # A YAML value quoted because it contains a colon -- which is why
        # several guide titles are quoted -- is still just its text. Left
        # in, the quotes render as part of the title everywhere the title
        # goes: the page's <title>, the index card, and the docs pane.
        if len(v) >= 2 and v[0] == v[-1] and v[0] in '"\'':
            v = v[1:-1]
        return v

    # Scalar keys are `key: value`. A key with an empty value may introduce a
    # block YAML list (`tags:` followed by `  - item` lines); collect those
    # items into a Python list. Anything else stays a flat string, so existing
    # scalar frontmatter parses exactly as before.
    list_key: str | None = None
    for line in fm_text.splitlines():
        item = re.match(r'^\s+-\s+(.*)$', line)
        if item and list_key is not None:
            meta[list_key].append(_unquote(item.group(1).strip()))
            continue
        if list_key is not None and meta[list_key] == []:
            meta[list_key] = ''
        list_key = None
        if ':' in line:
            k, _, v = line.partition(':')
            k, v = k.strip(), _unquote(v.strip())
            if k:
                if v == '':
                    meta[k] = []
                    list_key = k
                else:
                    meta[k] = v
    if list_key is not None and meta[list_key] == []:
        meta[list_key] = ''
    return meta, body


def build_categories_from_meta(meta_by_stem: dict, all_stems: set) -> list:
    """Build ordered category list from guide front matter."""
    buckets: dict[str, list] = {}
    for stem in sorted(all_stems):
        meta = meta_by_stem.get(stem, {})
        cat = meta.get('category', '').strip() or 'Other'
        title = meta.get('title', stem.replace('-', ' ').title()).strip()
        desc = meta.get('description', '').strip()
        # `tags` is a list when the frontmatter has a block list, '' otherwise.
        tags = meta.get('tags') or []
        buckets.setdefault(cat, []).append(
            {'stem': stem, 'label': title, 'desc': desc, 'tags': list(tags)})
    cat_names = sorted(k for k in buckets if k != 'Other')
    if 'Other' in buckets:
        cat_names.append('Other')
    return [{'name': name, 'guides': buckets[name]} for name in cat_names]


STYLE_REL = '../api/style.css'

# ---------------------------------------------------------------------------
# Page scripts and fonts under the site's Content-Security-Policy
#
# Every generated page is served from turmeric-lang.com under the policy in
# web/csp.js, whose script-src carries no 'unsafe-inline'. So nothing a page
# runs may be written into the page: behaviour ships as .js files written
# beside the pages that load them (write_page_scripts), named with a plain
# <script src> (script_tag), and the fonts are ordinary stylesheet links rather
# than the `<link rel=preload onload="this.rel='stylesheet'">` swap -- under the
# policy that handler never runs and the fonts never apply.
#
# The files are relative to the page, so a docs tarball read from disk keeps
# working exactly as it did.
# ---------------------------------------------------------------------------

def font_links(indent: str = '  ') -> str:
    """The web-font preconnects and stylesheets every generated page loads."""
    return '\n'.join(indent + line for line in (
        '<link rel="preconnect" href="https://fonts.googleapis.com">',
        '<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>',
        '<link rel="preconnect" href="https://cdn.jsdelivr.net">',
        '<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=DM+Sans:wght@300;400;500&display=swap">',
        '<link rel="stylesheet" href="https://cdn.jsdelivr.net/npm/@fontsource/iosevka@5/400.css">',
        '<link rel="stylesheet" href="https://cdn.jsdelivr.net/npm/@fontsource/iosevka@5/500.css">',
    ))


def theme_head(indent: str = '  ') -> str:
    """The light/dark switch's <head> lines: the theme-color tint and the
    blocking /theme.js, which sets <html data-theme> before first paint.

    Root-relative, like /logo.svg: every deploy (the main site and the spices
    subdomain alike) serves web/public/ at its root. A page read from disk
    without it is simply dark -- `:root` carries the dark tokens.
    """
    return '\n'.join(indent + line for line in (
        '<meta name="theme-color" content="#0C0A08">',
        '<script src="/theme.js"></script>',
    ))


def script_tag(src: str, indent: str = '  ') -> str:
    """A classic external script. Placed where the inline block used to be, so
    it runs at the same point in the parse."""
    return f'{indent}<script src="{src}"></script>'


def write_page_scripts(out_dir: Path, scripts: dict[str, str]) -> None:
    """Write each `name -> source` pair to out_dir/name."""
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    for name, src in scripts.items():
        (out_dir / name).write_text(src.rstrip('\n') + '\n', encoding='utf-8')

# Native `title` tooltips for the site chrome (topbar, sidebar, footer). Keyed
# by site-relative href -- absolute turmeric-lang.com URLs, which the spices
# site uses for cross-site links, are normalized to the same key so all three
# generators describe a page identically. Mirrors LINK_TITLES in web/site.js.
LINK_TITLES = {
    '/':                                      'Turmeric home',
    '/tour':                                  'A guided tour of the language in fourteen stops',
    '/try':                                   'Run Turmeric in your browser -- nothing to install',
    '/trowel':                                'Trowel -- the native Turmeric editor for macOS and Linux',
    '/docs/html/guides/':                     'Guides and tutorials, from quickstart to compiler internals',
    '/docs/html/api/':                        'Generated API reference for the standard library',
    '/docs/html/spices/':                     'Browse Spice packages -- the Turmeric package registry',
    '/roadmap':                               'Planned features, work in progress, and recent milestones',
    '/ci':                                    'Build and test metrics from continuous integration',
    'https://spices.turmeric-lang.com':       'Browse Spice packages -- the Turmeric package registry',
    'https://c.turmeric-lang.com':            'A C interpreter running in your browser',
    GITHUB_URL:                               'Turmeric source code on GitHub',
    'https://phasor.space':                   "Roger Jungemann's site",
}

# ---------------------------------------------------------------------------
# Canonical site chrome -- one list per region, four consumers
#
# The topbar and the sidebar name the same set of places on every page of the
# site, whether that page is hand-written (web/*.html, driven by web/site.js)
# or generated (guides here, API docs in gendocs.py, spices in genspices.py).
# Keeping the lists as data in one module is what stops the four surfaces from
# drifting into four different answers to "what is on this site". `web/site.js`
# mirrors NAV_LINKS/SIDEBAR_GROUPS verbatim -- change one, change the other.
# ---------------------------------------------------------------------------

# Topbar links, in order. `active` is matched by label.
NAV_LINKS = [
    ('/tour',                            'Tour'),
    ('/try',                             'Try It'),
    ('/docs/html/guides/',               'Guides'),
    ('/docs/html/api/',                  'API Docs'),
    ('https://spices.turmeric-lang.com', 'Spices'),
    ('/trowel',                          'Trowel'),
]

# Sidebar link groups, in order. The mobile drawer renders the same groups, so
# a phone sees the same site map a desktop sidebar shows.
SIDEBAR_GROUPS = [
    ('Language', [
        ('/tour',   'Tour'),
        ('/trowel', 'Trowel'),
        ('/try',    'Try It'),
    ]),
    ('Ecosystem', [
        ('/docs/html/guides/',               'Guides'),
        ('/docs/html/api/',                  'API Docs'),
        ('https://spices.turmeric-lang.com', 'Spices'),
        ('https://c.turmeric-lang.com',      'C Interpreter'),
    ]),
    ('Community', [
        (GITHUB_URL,                               'GitHub'),
        ('/ci',                                    'CI Metrics'),
    ]),
]

# The spices site lives on its own subdomain, so its root-relative hrefs have
# to be re-rooted at the main host. Everything already absolute is left alone.
MAIN_SITE = 'https://turmeric-lang.com'


def _href(href: str, base: str = '') -> str:
    """Re-root a site-relative href onto `base` (used by the spices subdomain)."""
    if not base or not href.startswith('/'):
        return href
    return base.rstrip('/') + href


def build_sidebar_globals(base: str = '', indent: str = '      ') -> str:
    """Render SIDEBAR_GROUPS -- the block every sidebar ends with.

    Emitted after the page's own back link and table of contents, separated
    from them by the one `sidebar-divider` on the page.
    """
    out = [f'{indent}<hr class="sidebar-divider">']
    for heading, links in SIDEBAR_GROUPS:
        out.append(f'{indent}<h3>{heading}</h3>')
        out.append(f'{indent}<ul>')
        for href, label in links:
            out.append(f'{indent}  <li><a href="{_href(href, base)}">{label}</a></li>')
        out.append(f'{indent}</ul>')
    return apply_link_titles('\n'.join(out))


# The light/dark switch. /theme.js handles every [data-theme-toggle] click by
# delegation and keeps the label current; this is only the markup, and the CSS
# shows the icon of the theme a click switches TO. Mirrors THEME_TOGGLE_HTML in
# web/site.js -- change one, change the other.
_ICON_ATTRS = ('viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" '
               'stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"')
THEME_TOGGLE = (
    '<button type="button" class="theme-toggle" data-theme-toggle '
    'aria-label="Toggle light and dark theme">'
    f'<svg class="icon-sun" {_ICON_ATTRS}><circle cx="12" cy="12" r="4"/><path d="M12 2v2"/>'
    '<path d="M12 20v2"/><path d="m4.93 4.93 1.41 1.41"/><path d="m17.66 17.66 1.41 1.41"/>'
    '<path d="M2 12h2"/><path d="M20 12h2"/><path d="m6.34 17.66-1.41 1.41"/>'
    '<path d="m19.07 4.93-1.41 1.41"/></svg>'
    f'<svg class="icon-moon" {_ICON_ATTRS}><path d="M20.985 12.486a9 9 0 1 1-9.473-9.472'
    'c.405-.022.617.46.402.803a6 6 0 0 0 8.268 8.268c.344-.215.825-.004.803.401"/></svg>'
    '</button>'
)


def build_page_header(active: str = '', base: str = '', search: str = '',
                      indent: str = '  ') -> str:
    """Render the topbar -- identical on every page but for the `active` mark.

    `search` is the aria-label for the filter box on the pages that have one
    (the guides and API indexes); pages without a filter simply omit it. The
    Try It entry and the gold CTA both drop out on Try Turmeric itself, where
    every route to /try is a link to the page you are already looking at.
    """
    on_try = active == 'Try It'
    parts = []
    for href, label in NAV_LINKS:
        if on_try and href == '/try':
            continue
        cls = ' class="active"' if label == active else ''
        parts.append(f'<a href="{_href(href, base)}"{cls}>{label}</a>')
    links = ''.join(parts)
    search_html = (
        f'\n{indent}  <div class="search-wrap">'
        f'<input class="search-input" type="search" placeholder="Filter... (/)" '
        f'aria-label="{search}"></div>'
        if search else ''
    )
    cta = ('' if on_try else
           f'\n{indent}    <a href="{_href("/try", base)}" class="btn-gold">Try it</a>')
    github = GITHUB_URL
    return apply_link_titles(f'''\
{indent}<header class="site-header">
{indent}  <button class="hamburger" aria-label="Toggle navigation" aria-expanded="false">
{indent}    <span></span><span></span><span></span>
{indent}  </button>
{indent}  <a class="nav-logo" href="{_href('/', base)}">
{indent}    <img src="/logo-icon.svg" width="28" height="28" alt="">
{indent}    <img src="/logo.svg" width="101" height="28" alt="Turmeric">
{indent}  </a>
{indent}  <nav class="nav-links">{links}</nav>{search_html}
{indent}  {THEME_TOGGLE}
{indent}  <div class="nav-right">
{indent}    <a href="{github}" class="btn-ghost">GitHub</a>{cta}
{indent}  </div>
{indent}</header>''')

_A_TAG_RE = re.compile(r'<a\s+([^>]*)href="([^"]+)"([^>]*)>')


def apply_link_titles(html: str, extra: dict | None = None) -> str:
    """Add a native `title` tooltip to every chrome link with a known href.

    Only used on the header/sidebar chrome, never on article bodies: a tooltip
    belongs on a navigation target, not on every prose link that happens to
    point at the same page. Links already carrying a title, and hrefs absent
    from the table, are left alone.
    """
    table = {**LINK_TITLES, **(extra or {})}

    def repl(m):
        before, href, after = m.group(1), m.group(2), m.group(3)
        if 'title=' in before or 'title=' in after:
            return m.group(0)
        key = href.replace('https://turmeric-lang.com', '') or '/'
        title = table.get(href) or table.get(key)
        if not title:
            return m.group(0)
        return f'<a {before}href="{href}"{after} title="{_html.escape(title, quote=True)}">'

    return _A_TAG_RE.sub(repl, html)


SIDEBAR_GLOBALS = build_sidebar_globals()


def build_sidebar(toc: str = '', uplinks: list | None = None, base: str = '',
                  extra_titles: dict | None = None) -> str:
    """Assemble a sidebar in the site's one shape.

    Home link, then the page's own "up" links (All Guides, All Spices, ...),
    then its table of contents, then the divider and the global groups. Every
    page in the site reads top-to-bottom in that order, so the global links sit
    in the same place whether you arrived at a guide, an API module, or a spice.
    """
    out = [f'      <a class="sidebar-back" href="{_href("/", base)}">Home</a>']
    if uplinks:
        items = ''.join(f'<a href="{href}">{label}</a>' for href, label in uplinks)
        out.append(f'      <div class="sidebar-uplinks">{items}</div>')
    if toc:
        out.append(toc.rstrip())
    out.append(build_sidebar_globals(base))
    return apply_link_titles('\n'.join(out), extra=extra_titles)

PAGE_HEADER = build_page_header(active='Guides')

INDEX_PAGE_HEADER = (build_page_header(active='Guides', search='Filter guides')
                     + '\n  <p class="search-no-results">No matching guides.</p>')

# The guides index's filter box. Loaded from guide-index.js (see script_tag).
INDEX_FILTER_JS_SRC = '''\
document.addEventListener('DOMContentLoaded', function(){
    // ---- Recently Added / Recently Updated tabs ----------------------------
    // The choice is remembered in localStorage, the same way the guide pages'
    // turmeric/sweet-exp toggle remembers 'guide-syntax'. A #recently-added or
    // #recently-updated hash (the sidebar links, an old bookmark) wins over
    // the stored choice for that visit but is not saved as one.
    var RECENT_KEY = 'guide-recent-tab';
    var recentBtns = Array.prototype.slice.call(
      document.querySelectorAll('.recent-tablist .seg-btn'));

    function selectRecent(slug, focus) {
      var found = recentBtns.some(function(b){ return b.dataset.recentTab === slug; });
      if (!found) return false;
      recentBtns.forEach(function(b){
        var on = b.dataset.recentTab === slug;
        b.classList.toggle('active', on);
        b.setAttribute('aria-selected', on ? 'true' : 'false');
        b.tabIndex = on ? 0 : -1;
        var panel = document.getElementById(b.dataset.recentTab);
        if (panel) panel.hidden = !on;
        if (on && focus) b.focus();
      });
      return true;
    }

    function rememberRecent(slug) {
      try { localStorage.setItem(RECENT_KEY, slug); } catch (e) {}
    }

    function selectFromHash() {
      return selectRecent(location.hash.slice(1), false);
    }

    if (recentBtns.length) {
      if (!selectFromHash()) {
        var stored = null;
        try { stored = localStorage.getItem(RECENT_KEY); } catch (e) {}
        if (stored) selectRecent(stored, false);
      }
      window.addEventListener('hashchange', selectFromHash);
      recentBtns.forEach(function(btn, i){
        btn.addEventListener('click', function(){
          selectRecent(btn.dataset.recentTab, false);
          rememberRecent(btn.dataset.recentTab);
        });
        // Arrow / Home / End move between tabs (WAI-ARIA tabs pattern).
        btn.addEventListener('keydown', function(e){
          var n = recentBtns.length, j = -1;
          if (e.key === 'ArrowRight') j = (i + 1) % n;
          else if (e.key === 'ArrowLeft') j = (i - 1 + n) % n;
          else if (e.key === 'Home') j = 0;
          else if (e.key === 'End') j = n - 1;
          if (j < 0) return;
          e.preventDefault();
          var slug = recentBtns[j].dataset.recentTab;
          selectRecent(slug, true);
          rememberRecent(slug);
        });
      });
    }

    // ---- Recently Added / Recently Updated pages ---------------------------
    // Each panel ships every row, all but the first page `hidden`, and its
    // first | previous | page m of n | next | last pager hidden too, so the
    // page reads as page 1 with no JS. A dead end is aria-disabled rather than
    // disabled, so the button you just pressed keeps focus on reaching the
    // first or last page.
    document.querySelectorAll('.recent-panel').forEach(function(panel){
      var pager = panel.querySelector('.recent-pager');
      if (!pager) return;
      var rows = Array.prototype.slice.call(panel.querySelectorAll('tbody tr'));
      var size = Number(pager.dataset.pageSize) || 10;
      var last = Math.max(0, Math.ceil(rows.length / size) - 1);
      var status = pager.querySelector('.recent-page-status');
      var page = 0;

      function show(p) {
        page = Math.max(0, Math.min(last, p));
        rows.forEach(function(tr, i){ tr.hidden = Math.floor(i / size) !== page; });
        pager.querySelectorAll('button').forEach(function(b){
          var back = b.dataset.page === 'first' || b.dataset.page === 'previous';
          var dead = back ? page === 0 : page === last;
          b.setAttribute('aria-disabled', dead ? 'true' : 'false');
        });
        if (status) status.textContent = 'page ' + (page + 1) + ' of ' + (last + 1);
      }

      pager.addEventListener('click', function(e){
        var b = e.target.closest('button');
        if (!b || b.getAttribute('aria-disabled') === 'true') return;
        show({ first: 0, previous: page - 1, next: page + 1, last: last }[b.dataset.page]);
      });
      pager.hidden = false;
      show(0);
    });

    // ---- By Category / By Tag -----------------------------------------------
    // Two grids of cards hold the same guides. The control picks which one is
    // on screen, and the sidebar swaps its Categories / Tags list to match. The
    // choice is remembered like the Recent tab's; a hash that points into the
    // other grid (a sidebar link, a shared URL such as #tag-effects) switches
    // to it for that visit without saving it.
    var VIEW_KEY = 'guide-index-view';
    var viewBtns = Array.prototype.slice.call(
      document.querySelectorAll('.view-tablist .seg-btn'));
    var refilter = function(){};   // the filter box below takes this over

    // `focusBtn`, when given, is the button to leave focused: the control the
    // keyboard is in, since the sidebar and the page each have one.
    function selectView(key, focusBtn) {
      var found = viewBtns.some(function(b){ return b.dataset.viewTab === key; });
      if (!found) return false;
      viewBtns.forEach(function(b){
        var on = b.dataset.viewTab === key;
        b.classList.toggle('active', on);
        b.setAttribute('aria-selected', on ? 'true' : 'false');
        b.tabIndex = on ? 0 : -1;
      });
      if (focusBtn) focusBtn.focus();
      document.querySelectorAll('.index-grid[data-view], .sidebar [data-view]')
        .forEach(function(el){ el.hidden = el.dataset.view !== key; });
      refilter();
      return true;
    }

    function rememberView(key) {
      try { localStorage.setItem(VIEW_KEY, key); } catch (e) {}
    }

    function viewFromHash() {
      var id = location.hash.slice(1);
      var el = id ? document.getElementById(id) : null;
      var grid = el && el.closest('.index-grid[data-view]');
      if (!grid) return false;
      var changed = grid.hidden;
      selectView(grid.dataset.view, null);
      // The browser could not scroll to a card that was hidden when it tried.
      if (changed) el.scrollIntoView();
      return true;
    }

    if (viewBtns.length) {
      document.querySelectorAll('.view-switch').forEach(function(el){ el.hidden = false; });
      document.querySelectorAll('.sidebar-view-heading').forEach(function(el){ el.hidden = true; });
      if (!viewFromHash()) {
        var storedView = null;
        try { storedView = localStorage.getItem(VIEW_KEY); } catch (e) {}
        if (storedView) selectView(storedView, null);
      }
      window.addEventListener('hashchange', viewFromHash);
      viewBtns.forEach(function(btn){
        btn.addEventListener('click', function(){
          selectView(btn.dataset.viewTab, null);
          rememberView(btn.dataset.viewTab);
        });
        // Arrow / Home / End move between the two buttons of the control that
        // has focus, as for the Recent tabs.
        btn.addEventListener('keydown', function(e){
          var group = Array.prototype.slice.call(
            btn.parentNode.querySelectorAll('.seg-btn'));
          var i = group.indexOf(btn), n = group.length, j = -1;
          if (e.key === 'ArrowRight') j = (i + 1) % n;
          else if (e.key === 'ArrowLeft') j = (i - 1 + n) % n;
          else if (e.key === 'Home') j = 0;
          else if (e.key === 'End') j = n - 1;
          if (j < 0) return;
          e.preventDefault();
          selectView(group[j].dataset.viewTab, group[j]);
          rememberView(group[j].dataset.viewTab);
        });
      });
    }

    // ---- Filter box ---------------------------------------------------------
    var input = document.querySelector('.search-input');
    if (!input) return;

    function filter() {
      var q = input.value.trim().toLowerCase();
      var visibleItems = 0;

      // The Recently Added / Updated tabs repeat guides the category cards
      // already list, and one of them is always a hidden tab, so a search hides
      // the whole box instead of filtering inside it.
      var recentBox = document.querySelector('.recent-tabs');
      if (recentBox) recentBox.style.display = q ? 'none' : 'block';

      // Only the grid on screen is searched; switching view filters the other.
      document.querySelectorAll('.index-grid:not([hidden]) .index-card').forEach(function(card) {
        var items = card.querySelectorAll('ul li');
        var shown = 0;
        items.forEach(function(li) {
          var match = !q || li.textContent.toLowerCase().includes(q);
          li.style.display = match ? '' : 'none';
          if (match) shown++;
        });
        // A card with no matching guides drops out entirely.
        card.style.display = (!q || shown > 0) ? 'block' : 'none';
        visibleItems += shown;
      });

      // Sync sidebar category links with card visibility.
      document.querySelectorAll('.sidebar a[href^="#"]').forEach(function(link) {
        var target = document.getElementById(link.getAttribute('href').slice(1));
        // A recent-tab panel answers for its whole box: an unselected tab is
        // hidden, but its sidebar link must stay.
        var box = target && (target.closest('.recent-tabs') || target);
        link.parentElement.style.display =
          (!box || box.style.display !== 'none') ? '' : 'none';
      });

      var noResults = document.querySelector('.search-no-results');
      if (noResults) {
        noResults.style.display = (q && visibleItems === 0) ? 'block' : 'none';
      }
    }

    input.addEventListener('input', filter);
    refilter = filter;

    // Clear on Escape
    input.addEventListener('keydown', function(e) {
      if (e.key === 'Escape') { input.value = ''; filter(); input.blur(); }
    });

    // Focus the filter with '/' (when not already typing somewhere)
    document.addEventListener('keydown', function(e) {
      if (e.key === '/' && document.activeElement !== input &&
          document.activeElement.tagName !== 'INPUT' &&
          document.activeElement.tagName !== 'TEXTAREA') {
        e.preventDefault();
        input.focus();
      }
    });
});'''

# The mobile drawer. One implementation, shared by every generated page and
# mirrored by `web/site.js` for the hand-written ones, so the hamburger does
# the same four things everywhere: toggle, close on overlay, close on Escape,
# close after following a link. The overlay element is page markup; the
# behaviour is site-drawer.js, emitted by every generator that uses it.
SIDEBAR_DRAWER_JS_SRC = '''\
    document.addEventListener('DOMContentLoaded', function(){
      var btn = document.querySelector('.hamburger');
      var sidebar = document.querySelector('.sidebar');
      var overlay = document.querySelector('.sidebar-overlay');
      if (!btn || !sidebar) return;
      // Pages with a header filter box hide it on mobile; offer a copy at the
      // top of the drawer and forward it to the real box, so each page's own
      // filter script keeps working unchanged.
      var search = document.querySelector('.search-wrap .search-input');
      if (search) {
        var wrap = document.createElement('div');
        wrap.className = 'sidebar-search';
        var copy = document.createElement('input');
        copy.className = 'sidebar-search-input';
        copy.type = 'search';
        copy.placeholder = 'Filter...';
        copy.setAttribute('aria-label', search.getAttribute('aria-label') || 'Filter');
        wrap.appendChild(copy);
        sidebar.insertBefore(wrap, sidebar.firstChild);
        copy.addEventListener('input', function(){
          search.value = copy.value;
          search.dispatchEvent(new Event('input'));
        });
        search.addEventListener('input', function(){ copy.value = search.value; });
        copy.addEventListener('keydown', function(e){
          if (e.key === 'Escape' && copy.value) {
            copy.value = '';
            search.value = '';
            search.dispatchEvent(new Event('input'));
            e.stopPropagation();
          }
        });
      }
      function setOpen(open) {
        sidebar.classList.toggle('is-open', open);
        overlay && overlay.classList.toggle('is-open', open);
        btn.setAttribute('aria-expanded', String(open));
      }
      btn.addEventListener('click', function(){
        setOpen(!sidebar.classList.contains('is-open'));
      });
      overlay && overlay.addEventListener('click', function(){ setOpen(false); });
      document.addEventListener('keydown', function(e){
        if (e.key === 'Escape') setOpen(false);
      });
      sidebar.addEventListener('click', function(e){
        if (e.target.closest('a')) setOpen(false);
      });
    });'''


def sidebar_drawer(base: str = '') -> str:
    """The drawer's overlay plus its script, `base` being the relative path
    from the page to the directory its site-drawer.js was written into."""
    return ('  <div class="sidebar-overlay"></div>\n'
            + script_tag(base + 'site-drawer.js'))

# ---------------------------------------------------------------------------
# Guide runtime -- one source, three consumers
#
# A rendered guide body needs three behaviours to look right: Turmeric syntax
# highlighting on its code blocks, the turmeric/sweet-exp segmented toggle on
# paired blocks, and mermaid rendering on its diagram blocks. Those behaviours
# are needed by the site pages under docs/html/guides/, by the spice pages
# genspices.py renders, and by Try Turmeric's in-app docs pane, which renders
# the very same bodies out of the docs pack.
#
# So GUIDE_JS_CORE below is the only copy. The site pages load it and call
# into it immediately (guide-runtime.js); the docs pack ships it as guide.js
# and the pane calls the same two entry points against its own subtree after
# each render. Both entry points take a root element and are idempotent, which
# is what makes re-running them on a freshly rendered fragment safe.
# ---------------------------------------------------------------------------

GUIDE_JS_CORE = '''\
(function(){
  var KW = new Set([
    'defn','defmacro','defstruct','definstance','defdata','defgadt','defclass','def','let','let*','letrec',
    'if','cond','when','unless','do','begin','and','or','not','else',
    'fn','lambda','async','await','match','case',
    'quote','quasiquote','unquote','for','while','loop','do-m',
    'set!','finally','with','use',
    'import','export','module','require','provide',
    'cons','car','cdr','nil-value','some','none','ok','err',
    'return','yield',
    'coerce','cast','type-of','any',
  ]);

  // Builtins: the editor's keyword.operator set (web/main.js), set apart from
  // the keywords so `println` and `map` do not share a colour with types and
  // kinds. Turmeric only; the Racket and Scheme specs have no such set.
  var BUILTIN = new Set([
    'println','print','read','eval','apply','map','filter','reduce','fold',
    'rc/of','rc/clone','rc/drop','rc/strong-count',
    'defeffect','perform','handle','resume','cont',
    'try','catch','throw','raise',
  ]);

  // What a `^name` is, as the elaborator reads it (Try Turmeric's editor does
  // the same): a modifier from a closed list, an extern-c C type, or else a
  // kind (`^f`, `^^f`, `^&r`) or a typeclass constraint (`^Eq`). CARET_MODIFIERS
  // is the sym_caret_* table in src/compiler/elab_core.c plus `^tailcall`; a
  // name missing from it reads as a kind, which is also what the compiler does
  // with an unrecognised `^name`.
  var CARET_MODIFIERS = new Set([
    'mut','private','persistent','linear','unique','affine','relevant',
    'atomic','thread-local','deferred-init','borrow','fat','extends',
    'capability','multishot','deprecated','reflect','construct','byval',
    'non-exhaustive','tailcall',
    // The flags of a defopaque / defstruct / defdata / defgadt, written before
    // the name (caret-type-attributes-plan); `linear` and `affine` are above.
    'copy','move','heap','sealed','non-null','no-auto-ctor',
  ]);
  var CARET_C_TYPES = new Set([
    'int','float','bool','char','cstr','void','nil','size','usize','isize',
    'ssize','float32','float64','int8','int16','int32','int64','uint8',
    'uint16','uint32','uint64','i8','i16','i32','i64','u8','u16',
  ]);

  // ---- Racket ------------------------------------------------------------
  // The guides quote Racket where it is the reference implementation of an
  // idea Turmeric borrows -- `send/suspend` in the web-continuations pair,
  // `shift`/`reset` in the delimited-control guides. Those blocks sit inches
  // from a highlighted Turmeric block making the same point, and an
  // unhighlighted one reads as a rendering failure rather than as "this is
  // the other language". Racket is close enough to Turmeric to share the
  // tokenizer; what differs is the keyword set and three bits of syntax, so
  // it is a spec passed to hl() rather than a second scanner.
  var RKT_KW = new Set([
    'define','define-values','define-syntax','define-syntax-rule','define-struct',
    'define-signature','define-values-for-syntax','lambda','case-lambda',
    'let','let*','letrec','let-values','let*-values','letrec-values','let/cc','let/ec',
    'if','cond','case','when','unless','else','and','or','not','begin','begin0',
    'set!','quote','quasiquote','unquote','unquote-splicing','syntax','syntax-rules',
    'syntax-case','with-syntax','match','match-lambda','match-define',
    'for','for*','for/list','for*/list','for/fold','for/vector','for/hash','do',
    'require','provide','module','module+','module*','struct','lang','#lang',
    'parameterize','with-handlers','dynamic-wind','call/cc','call-with-current-continuation',
    'call-with-composable-continuation','shift','reset','prompt','control',
    'delay','force','thunk','error','raise','λ',
  ]);
  var RKT_LIT = new Set(['#t','#f','#true','#false','null','empty','eof','void']);

  // ---- R7RS Scheme -------------------------------------------------------
  // `#lang r7rs` is a first-class dialect, so r7rs-guide.md is written in
  // Scheme throughout and quotes it in ```scheme fences. Scheme is close
  // enough to Turmeric to share the tokenizer, as Racket is; what it needs on
  // top is the number syntax (ratios, complex, `.5`, `#i3/2`), `#\\c`
  // characters, `#(...)` vectors and `#;` -- each a spec flag below rather
  // than a scanner of its own.
  var SCM_KW = new Set([
    'define','define-values','define-syntax','define-record-type',
    'define-library','import','export','include','include-ci','cond-expand',
    'lambda','case-lambda','let','let*','letrec','letrec*','let-values',
    'let*-values','let-syntax','letrec-syntax','parameterize','guard',
    'if','cond','case','when','unless','else','and','or','not','begin','do',
    'set!','quote','quasiquote','unquote','unquote-splicing',
    'syntax-rules','syntax-error','#lang','#!fold-case','#!no-fold-case',
    'delay','delay-force','force','make-promise','make-parameter',
    'call/cc','call-with-current-continuation','call-with-values','values',
    'dynamic-wind','with-exception-handler','raise','raise-continuable',
    'error','apply','eval','environment','interaction-environment','load',
    'car','cdr','cons','list','append','reverse','length','map','for-each',
    'display','write','newline','vector','string','features',
  ]);
  var SCM_LIT = new Set(['#t','#f','#true','#false']);

  // One anchored pattern for the whole R7RS number grammar, tried ahead of
  // the symbol rule and accepted only when a delimiter follows -- which is
  // what keeps the `+` in `(+ i 1)` and the `-` in `(- n 1)` the procedures
  // they are, while `1+2i`, `-4/3`, `.5`, `#i3/2` and `+inf.0` read as one
  // number each.
  var SCM_UREAL = '(?:[0-9]+/[0-9]+|(?:[0-9]+\\\\.?[0-9]*|\\\\.[0-9]+)(?:e[+-]?[0-9]+)?)';
  // A radix prefix changes the digit class, so `#x11/2` is its own branch
  // rather than a flag threaded through the decimal one.
  var SCM_HEX = '#(?:[ei]#)?x[+-]?[0-9a-f]+(?:/[0-9a-f]+)?';
  var SCM_NUM = new RegExp(
    '^(?:' + SCM_HEX + '|(?:#[eibodx])*' +
    '(?:(?:[+-]?' + SCM_UREAL + '|[+-](?:inf|nan)\\\\.0)' +
    // `|i` is what makes the pure imaginary `+2i` one number: without it the
    // real branch matches `+2` and the trailing `i` fails the delimiter test.
    '(?:[+-](?:' + SCM_UREAL + '|(?:inf|nan)\\\\.0)?i|i)?' +
    '|[+-](?:' + SCM_UREAL + ')?i))', 'i');
  var SCM_DELIM = /[\\s()\\[\\]{}";'`,|]/;

  var TUR_LIT = new Set(['true','false','nil']);

  function esc(s){return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');}
  // ---- Brace-family highlighter: C, C++, GDScript ------------------------
  // The guides interleave Turmeric with the C it emits and embeds
  // (sandboxing-guide, c-integration-guide, ffi-guide) and with the C++ and
  // GDScript it binds against (godot-resource-loader-guide). An unhighlighted
  // block sitting next to a highlighted one reads as a rendering failure, so
  // these run through one tokenizer and share the same five hl-* classes
  // rather than getting a palette of their own.
  var C_KW = new Set([
    'alignas','alignof','auto','break','case','catch','class','const',
    'const_cast','constexpr','continue','decltype','default','delete','do',
    'dynamic_cast','else','enum','explicit','extern','final','for','friend',
    'goto','if','inline','mutable','namespace','new','noexcept','operator',
    'override','private','protected','public','register','reinterpret_cast',
    'restrict','return','self','sizeof','static','static_assert','static_cast',
    'struct','switch','template','this','throw','try','typedef','typename',
    'union','using','virtual','volatile','while','_Atomic','_Static_assert',
  ]);
  var C_TYPE = new Set([
    'bool','char','double','float','int','long','short','signed','unsigned',
    'void','_Bool','FILE','va_list','size_t','ssize_t','ptrdiff_t','intptr_t',
    'uintptr_t','int8_t','int16_t','int32_t','int64_t','uint8_t','uint16_t',
    'uint32_t','uint64_t','wchar_t',
  ]);
  var C_LIT = new Set(['true','false','NULL','nullptr']);

  var GD_KW = new Set([
    'and','as','assert','await','break','breakpoint','class','class_name',
    'const','continue','elif','else','enum','export','extends','for','func',
    'if','in','is','match','not','onready','or','pass','preload','return',
    'self','setget','signal','static','super','tool','var','while','yield',
  ]);
  var GD_TYPE = new Set([
    'bool','float','int','void','Variant','String','StringName','NodePath',
    'Array','Dictionary','Callable','Signal','Color','Rect2','Vector2',
    'Vector2i','Vector3','Vector3i','Transform2D','Transform3D','Object',
    'Node','Resource','RefCounted','Ref','PackedByteArray','PackedStringArray',
  ]);
  var GD_LIT = new Set(['true','false','null','PI','TAU','INF','NAN']);

  // A CamelCase word -- upper initial with a lower-case letter somewhere after
  // -- is a type in all three languages (TuriEnv, PackedStringArray, Ref).
  // SCREAMING_CASE (TURI_INT, GDCLASS, OK) deliberately fails the test: those
  // are macros and enum constants, not types.
  function looksLikeType(w){ return /^[A-Z]/.test(w) && /[a-z]/.test(w); }

  function hlBrace(code, spec){
    var out = '', i = 0, n = code.length, lineStart = true;
    function span(cls, s){ return '<span class="hl-' + cls + '">' + esc(s) + '</span>'; }
    while(i<n){
      var c = code[i];
      if(c==='\\n'){ out+='\\n'; i++; lineStart=true; continue; }
      if(c===' '||c==='\\t'){ out+=c; i++; continue; }
      // Preprocessor directive: `#include <turi/eval.h>`, `#define X 1`. The
      // angle-bracket header is a string, so it is not mistaken for a `<`
      // comparison and left bare.
      if(spec.preproc && c==='#' && lineStart){
        var pe=code.indexOf('\\n',i); if(pe===-1)pe=n;
        var line=code.slice(i,pe);
        var dir=/^#[ \\t]*[a-z_]+/.exec(line);
        if(dir){
          out+=span('keyword', dir[0]);
          var restp=line.slice(dir[0].length);
          var inc=/^([ \\t]*)(<[^>\\n]*>)/.exec(restp);
          if(inc){ out+=esc(inc[1])+span('string', inc[2]); restp=restp.slice(inc[0].length); }
          out+=hlBrace(restp, spec);
          i=pe; lineStart=false; continue;
        }
      }
      // Line comment
      if(spec.line && code.substr(i, spec.line.length)===spec.line){
        var le=code.indexOf('\\n',i); if(le===-1)le=n;
        out+=span('comment', code.slice(i,le)); i=le; lineStart=false; continue;
      }
      // Block comment
      if(spec.block && code.substr(i,2)==='/*'){
        var be=code.indexOf('*/', i+2); be=(be===-1)?n:be+2;
        out+=span('comment', code.slice(i,be)); i=be; lineStart=false; continue;
      }
      // String or character literal. Unterminated at end of line, it stops
      // there -- a stray apostrophe in prose cannot swallow the rest of the
      // block.
      if(c==='"'||c==="'"){
        var q=c, sj=i+1;
        while(sj<n){
          if(code[sj]==='\\\\'){ sj+=2; continue; }
          if(code[sj]===q){ sj++; break; }
          if(code[sj]==='\\n') break;
          sj++;
        }
        out+=span('string', code.slice(i,sj)); i=sj; lineStart=false; continue;
      }
      // GDScript annotation (@export) and node path ($Player/Sprite)
      if(spec.at && (c==='@'||c==='$')){
        var aj=i+1;
        while(aj<n&&/[A-Za-z0-9_\\/]/.test(code[aj]))aj++;
        if(aj>i+1){ out+=span('type', code.slice(i,aj)); i=aj; lineStart=false; continue; }
      }
      // Number, with a C integer/float suffix (42u, 1.5f, 0xFFULL)
      if(/[0-9]/.test(c)){
        var nj=i;
        while(nj<n&&/[0-9a-fA-FxXbBoO_\\.]/.test(code[nj]))nj++;
        while(nj<n&&/[uUlLfF]/.test(code[nj]))nj++;
        out+=span('number', code.slice(i,nj)); i=nj; lineStart=false; continue;
      }
      // Identifier
      if(/[A-Za-z_]/.test(c)){
        var ij=i;
        while(ij<n&&/[A-Za-z0-9_]/.test(code[ij]))ij++;
        var w=code.slice(i,ij);
        if(spec.lit.has(w))                                      out+=span('number', w);
        else if(spec.kw.has(w))                                  out+=span('keyword', w);
        else if(spec.type.has(w)||/_t$/.test(w)||looksLikeType(w)) out+=span('type', w);
        else                                                     out+=esc(w);
        i=ij; lineStart=false; continue;
      }
      out+=esc(c); i++; lineStart=false;
    }
    return out;
  }

  var C_SPEC  = { kw:C_KW,  type:C_TYPE,  lit:C_LIT,  line:'//', block:true,  preproc:true,  at:false };
  var GD_SPEC = { kw:GD_KW, type:GD_TYPE, lit:GD_LIT, line:'#',  block:false, preproc:false, at:true  };
  var BRACE_LANGS = {
    'language-c':        C_SPEC,
    'language-cpp':      C_SPEC,
    'language-gdscript': GD_SPEC,
  };

  // ---- Rainbow brackets ---------------------------------------------------
  // Depth-colored (), [] and {}, as Try Turmeric's editor and trowel draw
  // them: one color per nesting level, cycling after six, whatever the kind
  // of bracket. A closer that does not match the innermost open bracket is
  // marked as an error and leaves the nesting alone, as Monaco does. Strings
  // and comments are scanned before this is reached, so a paren inside
  // either is not counted. The colors are the --syn-br* tokens (HL_CSS).
  var BR_OPEN = { '(':')', '[':']', '{':'}' };
  var BR_DEPTHS = 6;
  function bracket(c, stack){
    var cls;
    if(BR_OPEN[c]){
      cls = 'hl-br' + (stack.length % BR_DEPTHS + 1);
      stack.push(BR_OPEN[c]);
    } else if(stack.length && stack[stack.length-1] === c){
      stack.pop();
      cls = 'hl-br' + (stack.length % BR_DEPTHS + 1);
    } else {
      cls = 'hl-br-err';
    }
    return '<span class="' + cls + '">' + c + '</span>';
  }

  // End of a generic's argument list starting at code[j] -- `<void>` in
  // `ptr<void>`, `<ptr<void>>` in `ptr<ptr<void>>` -- or j itself when there is
  // none. Only a balanced, space-free `<...>` counts, so a lone `<` (the
  // less-than operator) is never taken for one.
  function typeArgsEnd(code, j){
    if(code[j]!=='<') return j;
    var depth=0, k=j;
    while(k<code.length){
      var ch=code[k];
      if(ch==='<') depth++;
      else if(ch==='>'){ depth--; if(depth===0) return k+1; }
      else if(!/[a-zA-Z0-9_\\-?!,:.\\/]/.test(ch)) return j;
      k++;
    }
    return j;
  }

  // A whole symbol that is a generic type name: `ptr<void>`, `option<int>`,
  // `rc<T>`. A name, then a balanced <...> that runs to the end of the symbol.
  // `char<?`, `string->cstr` and `<=` are not (no closing `>` after a name).
  function isGenericType(sym){
    var m=/^[A-Za-z_][A-Za-z0-9_\\-?!\\/]*/.exec(sym);
    if(!m||sym[m[0].length]!=='<') return false;
    return typeArgsEnd(sym, m[0].length)===sym.length;
  }

  function hl(code, spec){
    spec = spec || TUR_SPEC;
    var out='', i=0, n=code.length, stack=[];
    while(i<n){
      var c=code[i];
      // A `#lang` line names the file's dialect. It is a header rather than
      // code to read, so it is set back in the comment colour instead of lit
      // as a keyword. Only at the start of a line, and only with an argument.
      if(c==='#' && code.substr(i,5)==='#lang' && (i===0||code[i-1]==='\\n')
         && /[ \\t]/.test(code[i+5]||'')){
        var le=code.indexOf('\\n',i); if(le===-1)le=n;
        out+='<span class="hl-lang">'+esc(code.slice(i,le))+'</span>'; i=le; continue;
      }
      // Line comment
      if(c===';'){
        var e=code.indexOf('\\n',i); if(e===-1)e=n;
        out+='<span class="hl-comment">'+esc(code.slice(i,e))+'</span>'; i=e; continue;
      }
      // Racket block comment: #| ... |#, nestable in the language and here.
      if(spec.blockComment && code.substr(i,2)==='#|'){
        var depth=1, bj=i+2;
        while(bj<n&&depth>0){
          if(code.substr(bj,2)==='#|'){ depth++; bj+=2; continue; }
          if(code.substr(bj,2)==='|#'){ depth--; bj+=2; continue; }
          bj++;
        }
        out+='<span class="hl-comment">'+esc(code.slice(i,bj))+'</span>'; i=bj; continue;
      }
      // Datum comment: `#;` removes the datum that follows it, not the rest
      // of the line. Without this the `#` falls out of the symbol rule as
      // bare punctuation and the `;` behind it greys everything to the end of
      // the line -- a comment the program does not have.
      if(spec.datumComment && code.substr(i,2)==='#;'){
        var dj=i+2;
        while(dj<n&&/[ \\t\\n]/.test(code[dj]))dj++;
        if(code[dj]==='('||code[dj]==='['){
          var open=code[dj], close=(open==='(')?')':']', depth2=0;
          while(dj<n){
            var d2=code[dj];
            if(d2==='"'){ dj++; while(dj<n&&code[dj]!=='"'){ if(code[dj]==='\\\\')dj++; dj++; } }
            else if(d2===open){ depth2++; }
            else if(d2===close){ depth2--; if(depth2===0){ dj++; break; } }
            dj++;
          }
        } else {
          while(dj<n&&!SCM_DELIM.test(code[dj]))dj++;
        }
        out+='<span class="hl-comment">'+esc(code.slice(i,dj))+'</span>'; i=dj; continue;
      }
      // Character literal: #\\a, #\\space, #\\x41. The backslash is outside the
      // symbol character class, so without this `#\\c` scans as three separate
      // pieces and the character reads as a stray identifier.
      if(spec.charLit && code.substr(i,2)==='#\\\\'){
        var cj=i+2;
        if(cj<n){
          cj++;
          while(cj<n&&/[a-zA-Z0-9]/.test(code[cj])&&/[a-zA-Z]/.test(code[i+2]))cj++;
        }
        out+='<span class="hl-string">'+esc(code.slice(i,cj))+'</span>'; i=cj; continue;
      }
      // Vector and bytevector prefixes: #(1 2), #u8(0 255). Marked so the
      // literal is visibly one, rather than a lone `#` beside a list.
      if(spec.hashVector){
        var hv=/^#(?:u8)?\\(/.exec(code.slice(i,i+5));
        if(hv){
          out+='<span class="hl-type">'+esc(hv[0].slice(0,-1))+'</span>'+bracket('(',stack);
          i+=hv[0].length; continue;
        }
      }
      // Racket keyword argument: #:mode, #:when. Scanned before the symbol
      // rule, whose character class stops at the colon and would leave the
      // `#` stranded as bare punctuation.
      if(spec.hashKeyword && code.substr(i,2)==='#:'){
        var kj=i+2;
        while(kj<n&&/[a-zA-Z0-9_\\-!?*+<>=\\/]/.test(code[kj]))kj++;
        out+='<span class="hl-type">'+esc(code.slice(i,kj))+'</span>'; i=kj; continue;
      }
      // String
      if(c==='"'){
        var j=i+1;
        while(j<n){if(code[j]==='\\\\'){j+=2;continue;}if(code[j]==='"'){j++;break;}j++;}
        out+='<span class="hl-string">'+esc(code.slice(i,j))+'</span>'; i=j; continue;
      }
      // An inline-C fence inside a Turmeric body is C, not Turmeric. Scanned
      // after the comment and string cases so a ```c inside either stays
      // inside it. Without this every `;` ending a C statement reads as a Lisp
      // line comment and greys out the rest of the line -- which is how the
      // C bodies in c-integration-guide and ffi-guide used to render. The
      // block closes at the next ``` (CLAUDE.md spells it ```) , so the paren
      // is left for the Turmeric scan that resumes after it).
      if(spec.inlineC&&code.substr(i,4)==='```c'&&!/[A-Za-z0-9_]/.test(code[i+4]||'')){
        var fe=code.indexOf('```', i+4);
        var inner=(fe===-1)?code.slice(i+4):code.slice(i+4,fe);
        out+=esc('```c')+hlBrace(inner, C_SPEC);
        if(fe===-1){ i=n; } else { out+=esc('```'); i=fe+3; }
        continue;
      }
      // Type annotation :keyword. A generic's arguments belong to the name --
      // `:ptr<void>` is one type, not `:ptr` followed by `<void>` -- so the
      // whole of it takes one colour.
      if(spec.colonType&&c===':'&&i+1<n&&/[a-zA-Z_]/.test(code[i+1])){
        var j=i+1;
        while(j<n&&/[a-zA-Z0-9_\\-?!]/.test(code[j]))j++;
        j=typeArgsEnd(code,j);
        out+='<span class="hl-type">'+esc(code.slice(i,j))+'</span>'; i=j; continue;
      }
      // The same annotation spelled with a space after the colon, as the
      // stdlib writes it: `[name : cstr] : void`. The colon and the name are
      // both type, the gap between them is not.
      if(spec.colonType&&c===':'&&(code[i+1]===' '||code[i+1]==='\\t')){
        var sj=i+1;
        while(sj<n&&(code[sj]===' '||code[sj]==='\\t'))sj++;
        if(sj<n&&/[a-zA-Z_]/.test(code[sj])){
          var tj=sj;
          while(tj<n&&/[a-zA-Z0-9_\\-?!\\/]/.test(code[tj]))tj++;
          tj=typeArgsEnd(code,tj);
          out+='<span class="hl-type">:</span>'+esc(code.slice(i+1,sj))
              +'<span class="hl-type">'+esc(code.slice(sj,tj))+'</span>'; i=tj; continue;
        }
        // A colon that stands alone is the annotation separator whatever
        // follows it -- `x : (w a)`, `g : (fn [int] int)` -- so it is a type
        // token too, and every separator in a signature matches.
        out+='<span class="hl-type">:</span>'; i++; continue;
      }
      // R7RS number: one match for the whole grammar (SCM_NUM), gated on a
      // plausible first character and on a delimiter after the match, so the
      // bare `+` and `-` of `(+ i 1)` stay procedures.
      if(spec.schemeNum && /[0-9+\\-.#]/.test(c)){
        var sm=SCM_NUM.exec(code.slice(i));
        if(sm){
          var after=code[i+sm[0].length];
          if(after===undefined||SCM_DELIM.test(after)){
            out+='<span class="hl-number">'+esc(sm[0])+'</span>'; i+=sm[0].length; continue;
          }
        }
      }
      // Number (integer or float, possibly negative)
      if(/[0-9]/.test(c)||(c==='-'&&i+1<n&&/[0-9]/.test(code[i+1]))){
        var j=i; if(code[j]==='-')j++;
        while(j<n&&/[0-9a-fA-FxX_\\.]/.test(code[j]))j++;
        out+='<span class="hl-number">'+esc(code.slice(i,j))+'</span>'; i=j; continue;
      }
      // Symbol / identifier
      if(/[a-zA-Z_\\-!?*+<>=\\/&%^~#@]/.test(c)){
        var j=i;
        while(j<n&&/[a-zA-Z0-9_\\-!?*+<>=\\/&%^~#@\\.]/.test(code[j]))j++;
        var sym=code.slice(i,j);
        if(spec.caretAnn&&sym.length>1&&sym[0]==='^'){
          // `^mut`, `^fat`, `^state/GameState`, `^&cols`: the reader takes a
          // `^` as the start of a metadata symbol (reader.c), so the caret is
          // part of the name and the whole symbol is one token. The name says
          // which colour: a modifier is hl-ann, an extern-c C type (`^int`,
          // `^ptr<void>`) is hl-type, and anything else is a kind or a
          // typeclass constraint, hl-kind.
          var cname=sym.slice(1), ccls='hl-kind';
          if(CARET_MODIFIERS.has(cname)) ccls='hl-ann';
          else if(CARET_C_TYPES.has(cname)||(/^ptr</.test(cname)&&isGenericType(cname))) ccls='hl-type';
          out+='<span class="'+ccls+'">'+esc(sym)+'</span>';
        } else if(spec.colonType&&isGenericType(sym)){
          out+='<span class="hl-type">'+esc(sym)+'</span>';
        } else if(spec.lit.has(sym)){
          out+='<span class="hl-number">'+esc(sym)+'</span>';
        } else if(spec.builtin&&spec.builtin.has(sym)){
          out+='<span class="hl-builtin">'+esc(sym)+'</span>';
        } else if(spec.kw.has(sym)){
          out+='<span class="hl-keyword">'+esc(sym)+'</span>';
        } else {
          out+=esc(sym);
        }
        i=j; continue;
      }
      if(c==='('||c===')'||c==='['||c===']'||c==='{'||c==='}'){
        out+=bracket(c, stack); i++; continue;
      }
      out+=esc(c); i++;
    }
    return out;
  }

  // The Scheme-family flags are off for Turmeric, which has none of that
  // syntax, and on for both Racket and Scheme, which share all of it -- bar
  // charLit: Turmeric reads `#\\A` too (reader.c), and the rainbow needs the
  // `(` of `(pchar #\\()` to be a character, not an opener.
  var TUR_SPEC = { kw:KW,     lit:TUR_LIT, inlineC:true,  colonType:true,  caretAnn:true,
                   builtin:BUILTIN,
                   blockComment:false, hashKeyword:false,
                   datumComment:false, charLit:true,  hashVector:false,
                   schemeNum:false };
  var RKT_SPEC = { kw:RKT_KW, lit:RKT_LIT, inlineC:false, colonType:false,
                   blockComment:true,  hashKeyword:true,
                   datumComment:true,  charLit:true,  hashVector:true,
                   schemeNum:true  };
  var SCM_SPEC = { kw:SCM_KW, lit:SCM_LIT, inlineC:false, colonType:false,
                   blockComment:true,  hashKeyword:false,
                   datumComment:true,  charLit:true,  hashVector:true,
                   schemeNum:true  };
  var LISP_LANGS = {
    'language-turmeric':  TUR_SPEC,
    'language-sweet-exp': TUR_SPEC,
    'language-racket':    RKT_SPEC,
    'language-scheme':    SCM_SPEC,
    'language-r7rs':      SCM_SPEC,
  };


  // Idempotent: the data-hl stamp means a second pass over the same DOM (the
  // docs pane re-renders on every navigation) cannot double-escape the markup.
  function highlightGuideCode(root){
    var scope = root || document;
    Object.keys(LISP_LANGS).forEach(function(cls){
      scope.querySelectorAll('pre code.' + cls).forEach(function(el){
        if (el.dataset.hlDone) return;
        el.dataset.hlDone = '1';
        el.innerHTML = hl(el.textContent, LISP_LANGS[cls]);
      });
    });
    Object.keys(BRACE_LANGS).forEach(function(cls){
      scope.querySelectorAll('pre code.' + cls).forEach(function(el){
        if (el.dataset.hlDone) return;
        el.dataset.hlDone = '1';
        el.innerHTML = hlBrace(el.textContent, BRACE_LANGS[cls]);
      });
    });
  }

  function applyToggle(toggle, syntax) {
    var card = toggle.closest('.code-card');
    if (!card) return;
    toggle.querySelectorAll('.seg-btn').forEach(function(btn){
      var active = btn.dataset.syntax === syntax;
      btn.classList.toggle('active', active);
      btn.setAttribute('aria-selected', active ? 'true' : 'false');
    });
    card.querySelectorAll('.code-version').forEach(function(v){
      v.style.display = v.classList.contains(syntax + '-version') ? '' : 'none';
    });
  }

  function storedSyntax(){
    try { return localStorage.getItem('guide-syntax'); } catch (e) { return null; }
  }

  function initSyntaxToggles(root){
    var scope = root || document;
    var toggles = scope.querySelectorAll('.code-syntax-toggle');

    // ST1.5: restore the stored preference across every card on load
    var stored = storedSyntax();
    if (stored) toggles.forEach(function(t){ applyToggle(t, stored); });

    toggles.forEach(function(toggle){
      if (toggle.dataset.toggleWired) return;
      toggle.dataset.toggleWired = '1';
      // Click handler
      toggle.addEventListener('click', function(e){
        if (!e.target.classList.contains('seg-btn')) return;
        var syntax = e.target.dataset.syntax;
        document.querySelectorAll('.code-syntax-toggle').forEach(function(t){ applyToggle(t, syntax); });
        try { localStorage.setItem('guide-syntax', syntax); } catch (err) {}
      });
      // ST5.2: arrow-key navigation within the tablist
      toggle.addEventListener('keydown', function(e){
        var btns = Array.from(toggle.querySelectorAll('.seg-btn'));
        var idx = btns.indexOf(document.activeElement);
        if (idx === -1) return;
        if (e.key === 'ArrowRight'){ btns[(idx+1)%btns.length].focus(); e.preventDefault(); }
        if (e.key === 'ArrowLeft') { btns[(idx-1+btns.length)%btns.length].focus(); e.preventDefault(); }
      });
    });
  }

  // ---- Mermaid diagrams ---------------------------------------------------
  //
  // `finish_guide_body` has already turned every ```mermaid fence into a
  // `<pre class="mermaid">`. Mermaid itself is NOT bundled: it is ~3 MB, and
  // the docs pack is precached wholesale by Try Turmeric's service worker, so
  // vendoring it would grow every offline install by half again for a feature
  // a minority of pages use. It is imported on demand instead -- the first
  // page that actually contains a diagram pays for it, and nothing else does.
  //
  // The consequence is deliberate and is the reason the escaped source is left
  // in the <pre>: with no network (the offline docs pane, a docs tarball read
  // from disk) the import fails and the diagram degrades to its own source
  // text, which is readable. It does not degrade to an empty box.
  var MERMAID_SRC = 'https://cdn.jsdelivr.net/npm/mermaid@11.17.2/dist/mermaid.esm.min.mjs';
  var mermaidPromise = null;

  // Mermaid gets the guide palette by hand: it cannot read our CSS custom
  // properties, and its stock dark theme is a blue that clashes with the gold.
  // One set per site theme (/theme.js puts it on <html data-theme>), with the
  // same values as the --bg-panel / --text-* / --gold tokens of each.
  var MERMAID_VARS = {
    dark: {
        darkMode: true,
        background: '#161411',
        primaryColor: '#161411',
        primaryTextColor: '#EAE0D2',
        primaryBorderColor: '#EFA030',
        secondaryColor: '#1C1A15',
        tertiaryColor: '#12100D',
        lineColor: '#8A7D6E',
        textColor: '#EAE0D2',
        mainBkg: '#161411',
        nodeBorder: '#EFA030',
        clusterBkg: '#12100D',
        clusterBorder: '#222018',
        titleColor: '#EFA030',
        edgeLabelBackground: '#161411',
        fontSize: '14px'
    },
    light: {
        darkMode: false,
        background: '#EFE8DC',
        primaryColor: '#EFE8DC',
        primaryTextColor: '#2A2016',
        primaryBorderColor: '#945602',
        secondaryColor: '#E7DFD0',
        tertiaryColor: '#F5F0E7',
        lineColor: '#6A5B4B',
        textColor: '#2A2016',
        mainBkg: '#EFE8DC',
        nodeBorder: '#945602',
        clusterBkg: '#F5F0E7',
        clusterBorder: '#D5C8B3',
        titleColor: '#804900',
        edgeLabelBackground: '#EFE8DC',
        fontSize: '14px'
    }
  };

  function siteTheme(){
    return document.documentElement.getAttribute('data-theme') === 'light' ? 'light' : 'dark';
  }

  function mermaidConfig(){
    return {
      startOnLoad: false,
      securityLevel: 'strict',
      // A diagram that fails to parse keeps its own source text, the same way an
      // unreachable CDN leaves it. Mermaid's default is to swap in a 'Syntax
      // error' bomb graphic, which destroys the content it failed to render.
      suppressErrorRendering: true,
      theme: 'base',
      fontFamily: '"DM Sans", system-ui, sans-serif',
      themeVariables: MERMAID_VARS[siteTheme()]
    };
  }

  // A rendered diagram's <pre> holds the SVG, not its source, so the source is
  // kept here to draw it again in the other theme.
  var mermaidSources = typeof WeakMap !== 'undefined' ? new WeakMap() : null;

  function loadMermaid(){
    if (mermaidPromise) return mermaidPromise;
    mermaidPromise = import(MERMAID_SRC).then(function(mod){
      var m = mod.default || mod;
      m.initialize(mermaidConfig());
      return m;
    });
    return mermaidPromise;
  }

  // Idempotent the same way highlightGuideCode is: the stamp goes on BEFORE
  // the async render, so a second pass over a subtree whose first render is
  // still in flight cannot queue the same node twice.
  function renderMermaid(root){
    var scope = root || document;
    var nodes = [];
    scope.querySelectorAll('pre.mermaid').forEach(function(el){
      if (el.dataset.mermaidDone) return;
      el.dataset.mermaidDone = '1';
      if (mermaidSources && !mermaidSources.has(el)) mermaidSources.set(el, el.textContent);
      nodes.push(el);
    });
    if (!nodes.length) return Promise.resolve();

    // Leave the source visible and say why, rather than failing silently.
    function unrendered(els, err){
      els.forEach(function(el){ el.classList.add('mermaid-unrendered'); });
      if (typeof console !== 'undefined' && console.warn) {
        console.warn('mermaid: diagram left as source text', err);
      }
    }

    return loadMermaid().then(function(m){
      // One node at a time. `mermaid.run()` rejects on the FIRST diagram that
      // fails to parse and abandons the rest of the batch, so a single typo in
      // one block would otherwise mark every other diagram on the page as
      // unrendered -- including the ones that drew correctly.
      return nodes.reduce(function(chain, el){
        return chain.then(function(){
          return m.run({ nodes: [el] }).catch(function(err){ unrendered([el], err); });
        });
      }, Promise.resolve());
    }).catch(function(err){
      unrendered(nodes, err);   // mermaid itself never loaded
    });
  }

  // The reader switched theme: redraw every diagram already drawn. One that
  // never loaded mermaid has nothing to redraw -- its next render reads the
  // theme then.
  function redrawMermaid(){
    if (!mermaidPromise || !mermaidSources) return;
    mermaidPromise.then(function(m){
      m.initialize(mermaidConfig());
      document.querySelectorAll('pre.mermaid[data-processed]').forEach(function(el){
        if (!mermaidSources.has(el)) return;
        el.removeAttribute('data-processed');
        el.textContent = mermaidSources.get(el);
        delete el.dataset.mermaidDone;
      });
      renderMermaid(document);
    }).catch(function(){ /* never loaded: the source text is still showing */ });
  }
  if (typeof document !== 'undefined') {
    document.addEventListener('tur-themechange', redrawMermaid);
  }

  var api = { highlightGuideCode: highlightGuideCode, initSyntaxToggles: initSyntaxToggles,
              renderMermaid: renderMermaid, hl: hl };
  if (typeof window !== 'undefined') window.turmericGuide = api;
})();'''

# What a rendered site page runs, as guide-runtime.js: the shared core, then
# the calls that apply it to the whole document.
GUIDE_RUNTIME_JS_SRC = GUIDE_JS_CORE + '''
window.turmericGuide.highlightGuideCode(document);
window.turmericGuide.initSyntaxToggles(document);
window.turmericGuide.renderMermaid(document);'''


def guide_runtime(base: str = '') -> str:
    """The script tag for guide-runtime.js, relative to the page as for
    sidebar_drawer."""
    return script_tag(base + 'guide-runtime.js')


# The scripts a guides directory needs beside its pages.
GUIDE_PAGE_SCRIPTS = {
    'site-drawer.js':   SIDEBAR_DRAWER_JS_SRC,
    'guide-runtime.js': GUIDE_RUNTIME_JS_SRC,
    'guide-index.js':   INDEX_FILTER_JS_SRC,
}

# The token colors highlightGuideCode paints with. Split out of GUIDE_CSS so the
# API pages (gendocs.py), which highlight their Example blocks with the same
# runtime but do not take the guide prose styles, carry the same rules.
#
# The fallbacks are the dark theme's values; the --syn-* tokens themselves live
# in web/vars.css (Try Turmeric's docs pane) and gendocs.CSS (every generated
# page), both themes. The --syn-br* rainbow is the one web/main.js gives
# Monaco (RAINBOW_BRACKETS), itself taken from trowel's turmeric-dark theme --
# keep the three in step.
HL_CSS = '''\
    .hl-comment { color:var(--syn-cmt, #48433D); font-style:italic; }
    .hl-lang    { color:var(--syn-cmt, #48433D); }
    .hl-ann     { color:var(--syn-ann, #7AC4B8); }
    .hl-kind    { color:var(--syn-kind, #C4A0E8); }
    .hl-builtin { color:var(--syn-builtin, #8AB0E8); }
    .hl-string  { color:var(--syn-str, #D9735A); }
    .hl-number  { color:var(--syn-num, #A8C98A); }
    .hl-keyword { color:var(--syn-kw, #EFA030); font-weight:bold; }
    .hl-type    { color:var(--syn-kind, #C4A0E8); }
    .hl-br1     { color:var(--syn-br1, #EFA030); }
    .hl-br2     { color:var(--syn-br2, #D7C94A); }
    .hl-br3     { color:var(--syn-br3, #A8C98A); }
    .hl-br4     { color:var(--syn-br4, #7AC4B8); }
    .hl-br5     { color:var(--syn-br5, #8AB0E8); }
    .hl-br6     { color:var(--syn-br6, #C4A0E8); }
    .hl-br-err  { color:var(--syn-br-err, #FF5C57); }'''

# Gold leads, green answers -- the same two-colour split the home page uses for
# headline and emphasis, carried into long-form prose so a guide, an API page
# and a spice page all rank their headings by the same pair.
GUIDE_CSS = '''\
    .guide-content h1 { font-size:1.75rem; color:var(--gold-bright); margin-bottom:1.5rem; padding-bottom:0.75rem; border-bottom:1px solid var(--border); }
    .guide-content h2 { font-size:1.2rem; color:var(--gold); margin:2rem 0 0.75rem; }
    .guide-content h3 { font-size:1rem; color:var(--green); margin:1.5rem 0 0.5rem; }
    .guide-content h4 { font-size:0.925rem; color:var(--text-primary); margin:1.25rem 0 0.5rem; }
    .guide-content em { color:var(--green); font-style:italic; }
    .guide-content p  { margin-bottom:1rem; }
    .guide-content ul, .guide-content ol { margin:0 0 1rem 1.5rem; }
    .guide-content li { margin:0.25rem 0; }
    /* GFM task-list items. The checkbox is DISABLED on purpose: a guide page is
       prerendered HTML with no persistence on either consumer (the site or the
       offline docs pack), so an interactive box would silently drop every tick
       on reload. It marks the list as a checklist; the reader tracks state
       wherever the work actually happens. */
    .guide-content li.task-item { list-style:none; margin-left:-1.4rem; }
    /* Drawn, not native. A DISABLED checkbox in its UA skin is a solid grey
       square whose checked and unchecked states are all but identical against
       this theme -- an unchecked item reads as done. `appearance:none` drops
       that skin so an empty box is an empty box. */
    .guide-content li.task-item input[type="checkbox"] { -webkit-appearance:none; appearance:none; position:relative; width:0.95em; height:0.95em; margin:0 0.55rem 0 0; padding:0; border:1px solid var(--border-mid); border-radius:3px; background:var(--bg-panel); cursor:default; vertical-align:-0.12em; }
    .guide-content li.task-item input[type="checkbox"]:checked::after { content:""; position:absolute; left:0.3em; top:0.06em; width:0.2em; height:0.48em; border:solid var(--green); border-width:0 2px 2px 0; transform:rotate(43deg); }
    .guide-content code { font-family:"Iosevka","Fira Code",monospace; font-size:0.85em; background:var(--bg-panel); border:1px solid var(--border); border-radius:3px; padding:0.1em 0.35em; }
    .guide-content pre { background:var(--bg-panel); border:1px solid var(--border); border-radius:4px; padding:1rem; overflow-x:auto; margin-bottom:1rem; }
    /* ```ascii -- preformatted text whose VERTICAL alignment carries meaning: a
       directory tree's `|` gutter, a diagnostic's caret column, a grammar's
       aligned alternatives. Those glyphs have to touch across lines to read as
       a continuous stroke, and the body's inherited line-height:1.6 -- which is
       right for reading code, and stays -- opens a gap that breaks them into a
       dotted stutter. Only this fence tightens; every other block keeps 1.6.
       Anything that is a GRAPH should be a ```mermaid block instead. */
    .guide-content pre code.language-ascii { line-height:1.15; }
    /* A mermaid block before (or instead of) its render: still a code block, so
       an un-rendered diagram reads as its own source rather than as a blank. */
    .guide-content pre.mermaid { font-family:"Iosevka","Fira Code",monospace; font-size:0.85em; line-height:1.5; color:var(--text-sec); }
    /* Post-render mermaid injects an <svg>; drop the code-block chrome then. */
    .guide-content pre.mermaid[data-processed] { background:none; border:none; padding:0.5rem 0; text-align:center; line-height:normal; }
    .guide-content pre.mermaid[data-processed] svg { max-width:100%; height:auto; }
    .guide-content pre.mermaid.mermaid-unrendered { border-style:dashed; }
    .guide-content pre code { background:none; border:none; padding:0; font-size:0.85rem; }
    .guide-content blockquote { border-left:3px solid var(--green); padding-left:1rem; color:var(--text-sec); margin:1rem 0; }
    /* A `---` separator. The page reset zeroes hr's UA margins and leaves its
       UA `1px inset gray` border, so an unstyled rule renders as a 2px grey bar
       whose spacing comes entirely from its NEIGHBOURS: 16px above (the
       preceding p's margin-bottom), 32px below a heading, 0px below a
       paragraph -- the separator ends up glued to the entry under it. That is
       what made docs/guides/bibliography.md, which separates every entry with
       `---`, look unevenly spaced. Give the rule its own symmetric margin so
       margin-collapsing settles every neighbour pair at the same 2rem. */
    .guide-content hr { height:0; border:0; border-top:1px solid var(--border-mid); margin:2rem 0; }
    .guide-content hr:first-child { margin-top:0; }
    .guide-content hr:last-child { margin-bottom:0; }
    .guide-content .table-scroll { max-width:100%; overflow-x:auto; -webkit-overflow-scrolling:touch; margin-bottom:1rem; }
    .guide-content .table-scroll table { margin-bottom:0; }
    .guide-content table { border-collapse:collapse; width:100%; margin-bottom:1rem; font-size:0.9rem; }
    .guide-content th { background:var(--bg-surface); border:1px solid var(--border); padding:0.5rem 0.75rem; text-align:left; color:var(--gold-bright); }
    .guide-content td { border:1px solid var(--border); padding:0.5rem 0.75rem; }
    .guide-content a { color:var(--gold-bright); }
    .guide-content strong { color:var(--text-primary); }
    .guide-toc { border:1px solid var(--border); border-radius:6px; background:var(--bg-panel); padding:0.85rem 1.15rem 0.95rem; margin:0 0 2rem; }
    .guide-toc { border-left:3px solid var(--green-line); }
    .guide-toc-title { font-family:system-ui; font-size:0.7rem; text-transform:uppercase; letter-spacing:0.09em; color:var(--green); margin-bottom:0.5rem; }
    .guide-toc ul { margin:0 0 0 1.15rem; padding:0; }
    .guide-toc ul ul { margin-top:0.15rem; margin-bottom:0.15rem; }
    .guide-toc li { margin:0.2rem 0; font-size:0.875rem; }
    .guide-toc a { color:var(--gold-bright); }
    .guide-toc a:hover { color:var(--gold); }
''' + HL_CSS + '''
    .code-toggle { border:1px solid var(--border); border-radius:4px; margin-bottom:1rem; overflow:hidden; }
    .code-card-bar { background:var(--bg-surface); border-bottom:1px solid var(--border); padding:0.35rem 0.75rem; display:flex; align-items:center; }
    .code-syntax-toggle { margin-left:auto; display:flex; border:1px solid var(--border); border-radius:4px; overflow:hidden; font-family:"Iosevka","Fira Code",monospace; font-size:11px; }
    .seg-btn { padding:3px 10px; background:transparent; color:var(--text-sec); border:none; cursor:pointer; }
    .seg-btn:hover { color:var(--text-primary); }
    .seg-btn.active { color:var(--gold-bright); background:var(--bg-hover); }
    .code-card-body { }
    .code-version { }
    .guide-content .code-toggle pre { border:none; border-radius:0; margin-bottom:0; }'''


# A GFM task-list item as tur/markdown writes it: `<li>` then the disabled
# checkbox, with a `<p>` between them in a loose list. The renderer writes the
# box (as GitHub does); the class is what the guide CSS draws it by.
_TASK_ITEM_RE = re.compile(r'<li>(\s*(?:<p>)?<input type="checkbox")')


def mark_task_items(body_html: str) -> str:
    """Give each task-list item the `task-item` class the guide CSS styles."""
    return _TASK_ITEM_RE.sub(r'<li class="task-item">\1', body_html)


# A ```mermaid fence, as the renderer writes it. Mermaid renders from
# `<pre class="mermaid">`, not from the `<pre><code>` pair every other fence
# becomes, so the block is unwrapped here rather than in the browser -- one
# rewrite at build time instead of a DOM fixup on every page.
#
# The escaping stays: mermaid reads `textContent`, which the browser has
# already decoded, so `A --&gt; B` arrives at the parser as `A --> B`. Leaving
# the entities in place is what keeps the block valid HTML in the meantime --
# and what makes the un-rendered fallback (no JS, no network, the offline docs
# pane) show the diagram source as ordinary preformatted text instead of
# swallowing everything after the first `<`.
_MERMAID_BLOCK_RE = re.compile(
    r'<pre><code class="language-mermaid">(.*?)</code></pre>', re.DOTALL)


def wrap_tables(body_html: str) -> str:
    """Wrap each <table> in a scroll container.

    A table wider than the column scrolls inside its own box instead of
    stretching the page (and, in the offline docs pane, the whole panel).
    """
    return re.sub(r'<table\b.*?</table>',
                  lambda m: f'<div class="table-scroll">{m.group(0)}</div>',
                  body_html, flags=re.S)


def render_mermaid_blocks(body_html: str) -> str:
    """Unwrap ```mermaid fences into the `<pre class="mermaid">` mermaid wants."""
    return _MERMAID_BLOCK_RE.sub(
        lambda m: f'<pre class="mermaid">{m.group(1)}</pre>', body_html)


def inject_syntax_toggles(body_html: str) -> str:
    """Wrap adjacent turmeric+sweet-exp block pairs in a syntax-toggle widget."""
    # The `(?:(?!</code></pre>).)*` is a tempered dot, not decoration: a plain
    # `.*?` here backtracks PAST its own block's close when the next sibling is
    # not a sweet-exp block, swallowing the prose and code blocks in between
    # until it reaches a turmeric block that IS followed by one. The widget then
    # hid that prose inside the code card, and toggling to sweet-exp made it
    # vanish. Tempering keeps each group inside a single <pre>.
    pattern = re.compile(
        r'(<pre><code class="language-turmeric">(?:(?!</code></pre>).)*</code></pre>)'
        r'(\s*)'
        r'(<pre><code class="language-sweet-exp">(?:(?!</code></pre>).)*</code></pre>)',
        re.DOTALL,
    )

    def wrap_pair(m: re.Match) -> str:
        tur_block = m.group(1)
        sweet_block = m.group(3)
        return (
            '<div class="code-card code-toggle">'
            '<div class="code-card-bar">'
            '<div class="code-syntax-toggle" role="tablist">'
            '<button class="seg-btn active" data-syntax="turmeric"'
            ' role="tab" aria-selected="true">turmeric</button>'
            '<button class="seg-btn" data-syntax="sweet-exp"'
            ' role="tab" aria-selected="false">sweet-exp</button>'
            '</div>'
            '</div>'
            '<div class="code-card-body">'
            f'<div class="code-version turmeric-version" role="tabpanel">{tur_block}</div>'
            f'<div class="code-version sweet-exp-version" role="tabpanel"'
            f' style="display:none">{sweet_block}</div>'
            '</div>'
            '</div>'
        )

    return pattern.sub(wrap_pair, body_html)


# A hand-written "## Table of Contents" (or "## Contents") heading plus the
# list that follows it, up to the next heading (or end of file). We strip these
# from the source before rendering so the auto-generated in-body Contents box is
# the single source of truth -- no stale, hand-maintained duplicate.
_MANUAL_TOC_RE = re.compile(
    r'^#{1,6}[ \t]+(?:table of contents|contents)[ \t]*\n'  # the TOC heading
    r'(?:(?!^#{1,6}[ \t]).*\n?)*',                          # non-heading lines
    re.IGNORECASE | re.MULTILINE,
)


def strip_manual_toc(text: str) -> str:
    """Blank out a hand-written Table of Contents section from guide markdown.

    Blank lines rather than nothing, so the lines after it keep their numbers
    in the renderer's raw-HTML report.
    """
    return _MANUAL_TOC_RE.sub(lambda m: '\n' * m.group(0).count('\n'), text,
                              count=1)


def _count_toc_entries(tokens: list) -> int:
    return sum(1 + _count_toc_entries(t.get('children', [])) for t in tokens)


def toc_tokens_to_inline(tokens: list) -> str:
    """Render toc_tokens into a nested <ul> for the in-body Contents box."""
    items = []
    for tok in tokens:
        anchor = tok.get('id', '')
        name = tok.get('name', '')
        children = tok.get('children', [])
        sub = toc_tokens_to_inline(children) if children else ''
        items.append(f'<li><a href="#{anchor}">{name}</a>{sub}</li>')
    return '<ul>' + ''.join(items) + '</ul>'


def build_inline_toc(toc_tokens: list) -> str:
    """Build the in-body "Contents" navigation box from a doc's toc_tokens.

    The single top-level H1 (the page title) is elided -- its subsections
    become the roots. Returns '' when there are fewer than 3 entries, so short
    guides don't get a redundant one- or two-line box.
    """
    roots = toc_tokens
    if len(roots) == 1 and roots[0].get('level') == 1:
        roots = roots[0].get('children', [])
    if _count_toc_entries(roots) < 3:
        return ''
    return (
        '<nav class="guide-toc" aria-label="Table of contents">'
        '<div class="guide-toc-title">Contents</div>'
        f'{toc_tokens_to_inline(roots)}'
        '</nav>'
    )


def toc_tokens_to_sidebar(tokens: list) -> str:
    """Recursively render toc_tokens into sidebar <li> elements."""
    items = []
    for tok in tokens:
        anchor = tok.get('id', '')
        name = tok.get('name', '')
        level = tok.get('level', 2)
        indent = 'padding-left:0.75rem;' if level > 2 else ''
        color = 'color:var(--text-sec);' if level > 2 else ''
        # `name` is already the heading text, so the tooltip says what the link
        # does (jump within this page) rather than repeating the label. Round
        # -trip through unescape so an entity in the heading is not re-escaped.
        plain = _html.unescape(re.sub(r'<[^>]+>', '', name)).strip()
        tip = _html.escape(f'Jump to {plain}', quote=True)
        items.append(
            f'<li style="{indent}"><a href="#{anchor}" style="{color}" '
            f'title="{tip}">{name}</a></li>'
        )
        children = tok.get('children', [])
        if children:
            items.append(toc_tokens_to_sidebar(children))
    return '\n'.join(items)


def guide_source(src: Path, meta: dict | None = None) -> tuple[str, str, dict]:
    """A guide's Markdown as the renderer sees it.

    Returns (text, body, meta). `text` is what is rendered: the source with
    its front matter and any hand-written contents section blanked out, so
    the renderer's line numbers are the file's. `body` is the source after
    its front matter, and `meta` the front matter unless the caller gave it.
    """
    raw = src.read_text(encoding='utf-8')
    fm_meta, body = parse_front_matter(raw)
    if meta is None:
        meta = fm_meta
    text = '\n' * raw[:len(raw) - len(body)].count('\n') + body
    return strip_manual_toc(text), body, meta


def finish_guide_body(stem: str, page: dict, body: str, meta: dict) -> dict:
    """Turn one rendered guide into its article body and page facts."""
    body_html = inject_syntax_toggles(page['html'])
    body_html = mark_task_items(body_html)
    body_html = wrap_tables(body_html)
    body_html = render_mermaid_blocks(body_html)
    toc_tokens = page['toc_tokens']

    # In-body "Contents" box, inserted right after the page title (first <h1>),
    # or at the top of the content when the guide has no <h1>.
    inline_toc = build_inline_toc(toc_tokens)
    if inline_toc:
        h1_end = re.search(r'</h1>', body_html)
        if h1_end:
            i = h1_end.end()
            body_html = body_html[:i] + '\n' + inline_toc + body_html[i:]
        else:
            body_html = inline_toc + body_html

    fm_title = meta.get('title', '').strip() if meta else ''
    if fm_title:
        title = fm_title
    else:
        title_match = re.match(r'^#\s+(.+)', body, re.MULTILINE)
        title = title_match.group(1) if title_match else stem.replace('-', ' ').title()

    return {
        'stem': stem,
        'title': title,
        'body': body_html,
        'toc_tokens': toc_tokens,
        'meta': meta,
        'raw_html': page['raw_html'],
    }


def build_guide_bodies(jobs: list) -> list[dict]:
    """Render guides' markdown to their article bodies -- the single rendering pass.

    `jobs` holds one (stem, src, meta) per guide, and every guide goes through
    tur/markdown in one run of its renderer (tools/mdrender.py): CommonMark
    plus the GFM extensions, as GitHub renders the same source, with
    python-markdown's heading ids so existing anchors keep working.

    Each returned ``body`` still carries the source's raw ``href="other.md"``
    cross-links: each consumer rewrites them into its own URL space
    (``rewrite_links_site`` for docs/html/, ``rewrite_links_pack`` for the docs
    pack). Everything else about the body -- the syntax toggles, the in-body
    Contents box, the heading anchors -- is identical for both, which is what
    keeps the site and the pack from drifting.

    Returns one dict per job, in order, with: stem, title, body, toc_tokens,
    meta, raw_html.
    """
    prepared = [(stem, *guide_source(src, meta)) for stem, src, meta in jobs]
    pages = mdrender.render_batch({stem: text for stem, text, _, _ in prepared})
    return [finish_guide_body(stem, pages[stem], body, meta)
            for stem, _, body, meta in prepared]


def build_guide_body(stem: str, src: Path, meta: dict | None = None) -> dict:
    """build_guide_bodies for one guide."""
    return build_guide_bodies([(stem, src, meta)])[0]


# `[text](other-guide.md)` and `[text](other-guide.md#anchor)`, as they appear
# in the converted HTML. The optional fragment group is what lets a
# deep-linking cross-reference survive the rewrite instead of being left as a
# dead `.md` href.
_MD_HREF_RE = re.compile(r'href="([^"#]+)\.md(#[^"]*)?"')


def rewrite_links_site(body_html: str) -> str:
    """Rewrite `.md` cross-links to the sibling `.html` pages of docs/html/guides/.

    Must run AFTER markdown conversion -- the source uses `[text](file.md)`
    syntax, which only becomes `href="file.md"` once the converter has run.
    """
    def rewrite(m: re.Match) -> str:
        href, frag = m.group(1), m.group(2) or ''
        if href.startswith(('http://', 'https://', '/', '..')):
            return m.group(0)
        return f'href="{Path(href).name}.html{frag}"'

    return _MD_HREF_RE.sub(rewrite, body_html)


def render_guide(stem: str, src: Path, out: Path, all_stems: set,
                 meta: dict | None = None, doc: dict | None = None) -> None:
    if doc is None:
        doc = build_guide_body(stem, src, meta)
    title = doc['title']
    toc_tokens = doc['toc_tokens']
    body_html = rewrite_links_site(doc['body'])

    sidebar_items = toc_tokens_to_sidebar(toc_tokens)
    sidebar_html = build_sidebar(
        toc=f'      <h3>On this page</h3>\n      <ul>{sidebar_items}</ul>',
        uplinks=[('index.html', 'All Guides')],
        extra_titles={'index.html': 'Every guide, grouped by category'},
    )

    html = f'''<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>{title} | Turmeric Guides</title>
  <link rel="icon" type="image/svg+xml" href="/favicon.svg">
{theme_head()}
{font_links()}
  <link rel="stylesheet" href="{STYLE_REL}">
  <style>
{GUIDE_CSS}
  </style>
</head>
<body>
{PAGE_HEADER}
{sidebar_drawer()}
  <div class="page-layout">
    <div class="sidebar">
      {sidebar_html}
    </div>
    <div class="content guide-content">
      {body_html}
    </div>
  </div>
  <footer class="site-footer">
    Auto-generated by <code>tools/genguides.py</code> &mdash; source: <a href="{GITHUB_URL}/blob/main/docs/guides/{stem}.md"><code>docs/guides/{stem}.md</code></a>
  </footer>
{guide_runtime()}
</body>
</html>
'''
    out.write_text(html, encoding='utf-8')
    print(f'  {stem}.html')


def _fmt_inline(text: str) -> str:
    """HTML-escape text and convert backtick spans to <code> elements."""
    text = _html.escape(text)
    text = re.sub(r'`([^`]+)`', lambda m: f'<code>{m.group(1)}</code>', text)
    return text


def _fmt_desc(text: str) -> str:
    """Normalize and render a guide description for the index page.

    - Replaces em dashes with '--'
    - HTML-escapes special characters
    - Converts backtick spans to <code> elements
    """
    text = text.replace('—', '--')
    return _fmt_inline(text)


# Rows per page of the Recently Added / Recently Updated tabs. Ten, the length
# each list had before it had pages, and the length of the matching sections
# in Try Turmeric's docs pane (DOCS_RECENT_COUNT in web/main.js).
RECENT_PAGE_SIZE = 10


def render_index(categories: list[dict], all_stems: set[str], out_dir: Path,
                 recent: list[dict] | None = None,
                 recent_updated: list[dict] | None = None) -> None:
    categorized_stems = {g['stem'] for c in categories for g in c['guides']}
    recent = recent or []
    recent_updated = recent_updated or []

    def guide_item(g: dict) -> str:
        if g['stem'] not in all_stems:
            return ''
        return (f'<li><a href="{g["stem"]}.html">{_fmt_inline(g["label"])}</a>'
                f'<span style="color:var(--text-sec)"> -- {_fmt_desc(g["desc"])}</span></li>')

    cards = []
    for cat in categories:
        items = [s for g in cat['guides'] if (s := guide_item(g))]
        if not items:
            continue
        slug = re.sub(r'\s+', '-', cat['name'].lower())
        cards.append(f'''\
    <div class="index-card" style="display:block" id="{slug}">
      <h3 style="font-family:system-ui;font-size:0.9rem;margin-bottom:0.5rem">{cat['name']}</h3>
      <ul style="list-style:none;margin:0">
        {"".join(items)}
      </ul>
    </div>''')

    uncategorized = sorted(all_stems - categorized_stems)
    if uncategorized:
        items = [f'<li><a href="{s}.html">{s}</a></li>'
                 for s in uncategorized]
        cards.append(f'''\
    <div class="index-card" style="display:block">
      <h3 style="font-family:system-ui;font-size:0.9rem;margin-bottom:0.5rem">Other</h3>
      <ul style="list-style:none;margin:0">{"".join(items)}</ul>
    </div>''')

    # The same guides regrouped by tag: one card per tag, alphabetical, each
    # listing its guides by title. A guide with several tags appears under each
    # of them; one with none goes under the pseudo-tag `other`, which sorts
    # last. Card ids carry a `tag-` prefix because a tag can share its name
    # with a category slug (`performance`).
    OTHER_TAG = 'other'
    by_tag: dict[str, list[dict]] = {}
    any_tagged = False
    for cat in categories:
        for g in cat['guides']:
            if g['stem'] not in all_stems:
                continue
            any_tagged = any_tagged or bool(g.get('tags'))
            for t in g.get('tags') or [OTHER_TAG]:
                by_tag.setdefault(t, []).append(g)
    tag_order = sorted(t for t in by_tag if t != OTHER_TAG)
    if OTHER_TAG in by_tag:
        tag_order.append(OTHER_TAG)

    def tag_card(card_id: str, name: str, guides: list[dict]) -> str:
        items = ''.join(guide_item(g) for g in
                        sorted(guides, key=lambda g: g['label'].lower()))
        count = f' <span class="tag-count">{len(guides)}</span>'
        return f'''\
    <div class="index-card" style="display:block" id="{_html.escape(card_id, quote=True)}">
      <h3 style="font-family:system-ui;font-size:0.9rem;margin-bottom:0.5rem">{_html.escape(name)}{count}</h3>
      <ul style="list-style:none;margin:0">
        {items}
      </ul>
    </div>'''

    tag_cards = [tag_card(f'tag-{t}', t, by_tag[t]) for t in tag_order]

    # One sidebar list serves both views: the Recently links are always there,
    # and the category / tag links carry data-view so guide-index.js shows
    # only the ones for the grid on screen. The tag links ship `hidden`.
    sidebar_cats_list = [
        f'<li data-view="category"><a href="#{re.sub(r" +", "-", c["name"].lower())}" '
        f'title="Jump to {_html.escape(c["name"], quote=True)}">{c["name"]}</a></li>'
        for c in categories if any(g['stem'] in all_stems for g in c['guides'])
    ]
    sidebar_cats_list += [
        f'<li data-view="tag" hidden><a href="#tag-{_html.escape(t, quote=True)}" '
        f'title="Jump to {_html.escape(t, quote=True)}">{_html.escape(t)}</a></li>'
        for t in tag_order
    ]
    if recent_updated:
        sidebar_cats_list.insert(
            0, '<li><a href="#recently-updated" title="Jump to Recently '
               'Updated">Recently Updated</a></li>')
    if recent:
        sidebar_cats_list.insert(
            0, '<li><a href="#recently-added" title="Jump to Recently Added">'
               'Recently Added</a></li>')
    sidebar_cats = '\n'.join(sidebar_cats_list)
    # The list's heading is the same Categories | Tags switch as the one under
    # the Recent card, and the two stay in step. Without JS the switch stays
    # `hidden` and the plain "Categories" heading stands in for it.
    def view_tablist(labels: dict[str, str], aria_label: str, ids: bool) -> str:
        btns = []
        for i, (key, text) in enumerate(labels.items()):
            attrs = [f'class="seg-btn{" active" if i == 0 else ""}"', 'role="tab"']
            if ids:
                attrs.append(f'id="view-{key}-tab"')
            attrs += [f'data-view-tab="{key}"', f'aria-controls="grid-{key}"',
                      f'aria-selected="{"true" if i == 0 else "false"}"',
                      f'tabindex="{0 if i == 0 else -1}"']
            btns.append(f'<button {" ".join(attrs)}>{text}</button>')
        return (f'<div class="view-tablist" role="tablist"'
                f' aria-label="{aria_label}">{"".join(btns)}</div>')

    sidebar_switch = (
        '      <div class="view-switch sidebar-view-switch" hidden>'
        + view_tablist({'category': 'Categories', 'tag': 'Tags'},
                       'Group guides by', ids=False)
        + '</div>\n') if any_tagged else ''
    sidebar_html = build_sidebar(
        toc=f'      <h3 class="sidebar-view-heading">Categories</h3>\n'
            f'{sidebar_switch}'
            f'      <ul>{sidebar_cats}</ul>')

    # Title | date | description, one table row each, with no headings and no
    # rules. The title and date columns size to their content; the
    # description takes the width they leave and ellipsizes there, so a long
    # one costs the row nothing but its tail. Its cell's title attribute
    # carries the whole description for hover. Rows past the first page ship
    # `hidden`, so without JS the panel reads as its first page -- the same
    # ten rows it listed before it had pages.
    def dated_rows(entries: list[dict]) -> str:
        out = []
        for i, r in enumerate(entries):
            desc = r.get('desc', '').replace('—', '--')
            out.append(
                f'<tr{" hidden" if i >= RECENT_PAGE_SIZE else ""}>'
                f'<td class="recent-title"><a href="{r["stem"]}.html">'
                f'{_fmt_inline(r["label"])}</a></td>'
                f'<td class="recent-date">{r["date"]}</td>'
                f'<td class="recent-desc" title="{_html.escape(desc, quote=True)}">'
                f'{_fmt_desc(desc)}</td></tr>')
        return ''.join(out)

    # first | previous | page m of n | next | last, centered under a panel with
    # more than one page. Shipped `hidden`: guide-index.js reveals it once it
    # can turn the pages, so a page read without JS never shows controls that
    # do nothing.
    def pager(heading: str, entries: list[dict]) -> str:
        pages = -(-len(entries) // RECENT_PAGE_SIZE)
        if pages <= 1:
            return ''
        button = lambda name: (f'<button type="button" data-page="{name}">'
                               f'{name}</button>')
        status = (f'<span class="recent-page-status" aria-live="polite">'
                  f'page 1 of {pages}</span>')
        sep = '<span class="recent-pager-sep" aria-hidden="true">|</span>'
        items = sep.join([button('first'), button('previous'), status,
                          button('next'), button('last')])
        return (f'<nav class="recent-pager" aria-label="{heading} pages"'
                f' data-page-size="{RECENT_PAGE_SIZE}" hidden>{items}</nav>')

    # What arrived, then what changed, as two tabs of one card. The dates are
    # the two ends of each guide's git history -- first commit and last -- so a
    # guide can honestly appear in both only when it landed and was then edited
    # on a later day. The panel ids keep the old card ids, so the sidebar's
    # #recently-added / #recently-updated links (and any bookmark of them) still
    # land here; guide-index.js selects the matching tab. The first tab is
    # selected server-side, so the page reads correctly with no JS at all.
    tabs = [(slug, heading, entries) for slug, heading, entries in (
        ('recently-added', 'Recently Added', recent),
        ('recently-updated', 'Recently Updated', recent_updated),
    ) if entries]
    buttons = ''.join(
        f'<button class="seg-btn{" active" if i == 0 else ""}" role="tab"'
        f' id="{slug}-tab" data-recent-tab="{slug}" aria-controls="{slug}"'
        f' aria-selected="{"true" if i == 0 else "false"}"'
        f' tabindex="{0 if i == 0 else -1}">{heading}</button>'
        for i, (slug, heading, _) in enumerate(tabs)
    )
    panels = ''.join(
        f'<div class="recent-panel" role="tabpanel" id="{slug}"'
        f' aria-labelledby="{slug}-tab"{"" if i == 0 else " hidden"}>'
        f'<table class="recent-table"><tbody>{dated_rows(entries)}</tbody></table>'
        f'{pager(heading, entries)}</div>'
        for i, (slug, heading, entries) in enumerate(tabs)
    )
    recent_html = f'''\
      <div class="index-card recent-tabs" style="display:block;margin-bottom:1.5rem">
        <div class="recent-tablist" role="tablist" aria-label="Recent guide changes">{buttons}</div>
        {panels}
      </div>''' if tabs else ''

    # By Category | By Tag, under the Recent card: which grid of cards the page
    # shows. By Category is selected server-side and the control ships
    # `hidden`, so a page read without JS is the category grid with no dead
    # buttons; guide-index.js reveals it and remembers the choice.
    view_switch = (f'      <div class="view-switch" hidden>\n'
                   f'        '
                   + view_tablist({'category': 'By Category', 'tag': 'By Tag'},
                                  'Group guides by', ids=True)
                   + '\n      </div>') if any_tagged else ''

    html = f'''<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Guides | Turmeric</title>
  <link rel="icon" type="image/svg+xml" href="/favicon.svg">
{theme_head()}
{font_links()}
  <link rel="stylesheet" href="{STYLE_REL}">
  <style>
    .index-card ul li {{ margin:0.3rem 0; font-size:0.875rem; }}
    .index-card ul li a {{ color:var(--text-primary); }}
    .index-card ul li a:hover {{ color:var(--gold); }}
    .recent-tablist, .view-tablist {{ display:inline-flex; border:1px solid var(--border); border-radius:4px; overflow:hidden; margin-bottom:0.6rem; font-family:system-ui; font-size:0.8rem; }}
    .recent-tablist .seg-btn, .view-tablist .seg-btn {{ padding:4px 12px; background:transparent; color:var(--text-sec); border:none; cursor:pointer; font:inherit; }}
    .recent-tablist .seg-btn + .seg-btn, .view-tablist .seg-btn + .seg-btn {{ border-left:1px solid var(--border); }}
    .recent-tablist .seg-btn:hover, .view-tablist .seg-btn:hover {{ color:var(--text-primary); }}
    .recent-tablist .seg-btn.active, .view-tablist .seg-btn.active {{ color:var(--gold-bright); background:var(--bg-hover); }}
    .recent-tablist .seg-btn:focus-visible, .view-tablist .seg-btn:focus-visible {{ outline:2px solid var(--gold); outline-offset:-2px; }}
    .view-switch[hidden], .index-grid[hidden], .sidebar [data-view][hidden], .sidebar-view-heading[hidden] {{ display:none; }}
    .view-switch {{ margin-bottom:0; }}
    .view-tablist {{ margin-bottom:0; }}
    /* The shared .module-heading draws a rule under its text; the index runs the intro straight into the cards. */
    .module-heading.index-heading {{ border-bottom:none; padding-bottom:0; margin-bottom:1.25rem; }}
    .index-grid[data-view] {{ margin-top:0.75rem; }}
    .sidebar .sidebar-view-switch {{ margin:0.25rem 0 0.75rem; }}
    .sidebar .view-tablist {{ display:flex; font-size:0.75rem; }}
    .sidebar .view-tablist .seg-btn {{ flex:1; padding:4px 8px; }}
    .tag-count {{ margin-left:0.35rem; font-weight:normal; color:var(--text-dim); font-variant-numeric:tabular-nums; }}
    .recent-table {{ width:100%; border-collapse:collapse; font-size:0.875rem; }}
    .recent-table td {{ padding:0.2rem 1.25rem 0.2rem 0; border:none; white-space:nowrap; vertical-align:baseline; }}
    .recent-table td:last-child {{ padding-right:0; }}
    .recent-table .recent-title, .recent-table .recent-date {{ width:1%; }}
    .recent-table .recent-title a {{ display:block; max-width:min(24rem, 45vw); overflow:hidden; text-overflow:ellipsis; color:var(--text-primary); }}
    .recent-table .recent-title a:hover {{ color:var(--gold); }}
    .recent-table .recent-date {{ color:var(--text-sec); font-variant-numeric:tabular-nums; }}
    .recent-table .recent-desc {{ max-width:0; overflow:hidden; text-overflow:ellipsis; color:var(--text-sec); }}
    .recent-pager {{ display:flex; justify-content:center; flex-wrap:wrap; align-items:baseline; gap:0.5rem; margin-top:0.75rem; font-family:system-ui; font-size:0.8rem; color:var(--text-dim); }}
    .recent-pager[hidden] {{ display:none; }}
    .recent-pager button {{ padding:0; background:none; border:none; font:inherit; color:var(--gold); cursor:pointer; }}
    .recent-pager button:hover {{ color:var(--gold-bright); text-decoration:underline; }}
    .recent-pager button[aria-disabled="true"] {{ color:var(--text-dim); cursor:default; text-decoration:none; }}
    .recent-pager button:focus-visible {{ outline:2px solid var(--gold); outline-offset:2px; }}
    .recent-page-status {{ color:var(--text-sec); }}
    /* A phone leaves the description a letter or two -- a column of "H..." --
       so below 600px it goes, and the title takes the width it held. */
    @media (max-width: 600px) {{
      .recent-table .recent-desc {{ display:none; }}
      .recent-table .recent-title {{ width:100%; max-width:0; }}
      .recent-table .recent-title a {{ max-width:none; }}
      .recent-table .recent-date {{ padding-right:0; }}
    }}
  </style>
</head>
<body>
{INDEX_PAGE_HEADER}
{sidebar_drawer()}
  <div class="page-layout">
    <div class="sidebar">
{sidebar_html}
    </div>
    <div class="content">
      <div class="module-heading index-heading">
        <h1 style="font-family:system-ui;color:var(--gold)">Guides</h1>
        <div class="module-path guide-count">There are currently {len(all_stems)} tutorials, how-tos, and in-depth feature guides for Turmeric.</div>
        <p class="module-path">Visit the <a href="https://spices.turmeric-lang.com/">Spices</a> page for spice-specific guides.</p>
        <p class="module-path">For docs for the Trowel editor, <a href="https://github.com/turmeric-lang/trowel#using-trowel">go here</a>.</p>
      </div>
{recent_html}
{view_switch}
      <div class="index-grid" id="grid-category" data-view="category" role="tabpanel" aria-labelledby="view-category-tab">
        {"".join(cards)}
      </div>
      <div class="index-grid" id="grid-tag" data-view="tag" role="tabpanel" aria-labelledby="view-tag-tab" hidden>
        {"".join(tag_cards)}
      </div>
    </div>
  </div>
  <footer class="site-footer">
    Auto-generated by <code>tools/genguides.py</code>
  </footer>
{guide_runtime()}
{script_tag('guide-index.js')}
</body>
</html>
'''
    (out_dir / 'index.html').write_text(html, encoding='utf-8')
    print('  index.html')


def emit_pack_guides(docs: list[dict], guides_dir: Path, pack_dir: Path,
                     added_by_stem: dict[str, str] | None = None,
                     updated_by_stem: dict[str, str] | None = None) -> None:
    """Write the chrome-free guide fragments and the guides slice of index.json.

    Fragments keep their source links; `tools/genpack.py` rewrites them into the
    pack's `#doc=` URL space once every generator has contributed, because only
    it knows what the finished pack contains.

    `added_by_stem` and `updated_by_stem` are the same date maps the index
    page's Recently Added and Recently Updated cards are built from -- passed in
    rather than recomputed so the pane's sections and the website's cards cannot
    disagree about when a guide arrived or when it last changed.
    """
    pack_dir = Path(pack_dir)

    # The pane owns typography, so the pack ships the same guide stylesheet the
    # site pages inline and the same runtime that highlights their code blocks
    # and drives the turmeric/sweet-exp toggles -- emitted from the very
    # constants those pages are built from, so a guide looks the same in Try as
    # it does on turmeric-lang.com without the rules being written twice.
    packlib.write_fragment(pack_dir, 'guide.css', GUIDE_CSS)
    packlib.write_fragment(pack_dir, 'guide.js', GUIDE_JS_CORE)

    entries = []
    for doc in docs:
        stem = doc['stem']
        rel = f'guides/{stem}.html'
        size = packlib.write_fragment(pack_dir, rel, doc['body'])
        meta = doc['meta'] or {}
        headings = packlib.heading_names(doc['toc_tokens'])
        description = (meta.get('description', '') or '').replace('—', '--').strip()
        entry = {
            'slug': stem,
            'path': rel,
            'title': doc['title'],
            'category': (meta.get('category', '') or '').strip() or 'Other',
            'description': description,
            'bytes': size,
            'words': packlib.search_string(
                stem.replace('-', ' '), doc['title'], description,
                *headings, prose=packlib.strip_tags(doc['body'])),
        }
        # The guide's frontmatter tags, for the pane's By Tag view. Left off an
        # untagged guide rather than written as [], so the pane files it under
        # the pseudo-tag `other` and a pack with no tags at all shows no tag
        # switch.
        if meta.get('tags'):
            entry['tags'] = list(meta['tags'])
        # Only when git knows: a guide written but not yet committed has no
        # date to claim, and the pane leaves an undated page out of Recently
        # Added / Recently Updated rather than dating it from the build.
        for key, by_stem in (('added', added_by_stem), ('updated', updated_by_stem)):
            date = (by_stem or {}).get(stem)
            if date:
                entry[key] = date
        entries.append(entry)

        # Copy any local images the guide references, so the pack is
        # self-contained and the budget check counts what it ships.
        for src_rel in packlib.local_image_srcs(doc['body']):
            src_path = (guides_dir / src_rel).resolve()
            if not src_path.is_file():
                print(f'  warning: {stem}.md references missing image {src_rel}',
                      file=sys.stderr)
                continue
            dst = pack_dir / 'guides' / src_rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_bytes(src_path.read_bytes())

    packlib.write_sidecar(pack_dir, 'guides', entries)
    print(f'  pack: {len(entries)} guide fragments -> {pack_dir}/guides/')


def main() -> None:
    p = argparse.ArgumentParser(description='Render Turmeric guide markdown to HTML.')
    p.add_argument('guides_dir', help='Path to docs/guides/ directory')
    p.add_argument('--out', default=None, help='Output directory (default: same as guides_dir)')
    p.add_argument('--emit-pack', metavar='DIR', default=None,
                   help='Also write chrome-free guide fragments into the docs '
                        'pack at DIR (see tools/genpack.py)')
    args = p.parse_args()

    guides_dir = Path(args.guides_dir)
    out_dir = Path(args.out) if args.out else guides_dir
    out_dir.mkdir(parents=True, exist_ok=True)
    write_page_scripts(out_dir, GUIDE_PAGE_SCRIPTS)

    md_files = sorted(f for f in guides_dir.glob('*.md') if f.stem != 'README')
    all_stems = {f.stem for f in md_files}

    meta_by_stem: dict = {}
    for src in md_files:
        fm, _ = parse_front_matter(src.read_text(encoding='utf-8'))
        meta_by_stem[src.stem] = fm

    categories = build_categories_from_meta(meta_by_stem, all_stems)

    # Find the git repo root by walking up from the guides dir.
    repo_root = guides_dir.resolve()
    while repo_root != repo_root.parent and not (repo_root / '.git').exists():
        repo_root = repo_root.parent

    # `get_creation_date` shells out to `git log --follow` once per guide, and
    # --follow forces full-history rename detection, so each call costs ~0.12s
    # and the serial loop was ~25s of the ~34s total -- almost all of it spent
    # blocked on the child process, not computing. The calls are independent
    # and I/O-bound, so a thread pool collapses the wait; `ex.map` preserves
    # input order, so the result is identical to the serial loop, not merely
    # equivalent. Threads (not processes) because the work is a subprocess
    # wait, which releases the GIL.
    packlib.warn_if_shallow(repo_root)
    n_git_workers = min(32, (os.cpu_count() or 4) * 4, max(1, len(md_files)))
    with ThreadPoolExecutor(max_workers=n_git_workers) as ex:
        creation_dates = list(
            ex.map(lambda s: get_creation_date(s, repo_root), md_files)
        )
    # The other end of each guide's history, for the Recently Updated card.
    # One `git log` for the whole set rather than `--follow` per file: for a
    # *last* edit, following renames cannot change the answer -- the newest
    # commit touching the current path is the newest edit either way -- so the
    # single traversal is both cheaper and exactly as correct here.
    modified_dates = {
        src.stem: d['updated']
        for src, d in packlib.git_page_dates(md_files, repo_root).items()
        if d.get('updated')
    }

    def label_for(src: Path) -> str:
        m = meta_by_stem.get(src.stem, {})
        return m.get('title', src.stem.replace('-', ' ').title()).strip()

    def dated_entry(src: Path, date: str) -> dict:
        return {'stem': src.stem, 'label': label_for(src), 'date': date,
                'desc': meta_by_stem.get(src.stem, {}).get('description', '').strip()}

    # Every dated guide, not just the newest ten: the index pages through
    # them RECENT_PAGE_SIZE at a time, so the last page is the oldest guides
    # rather than nothing past the tenth.
    dated: list[tuple[str, Path]] = [
        (d, src) for src, d in zip(md_files, creation_dates) if d
    ]
    dated.sort(key=lambda t: t[0], reverse=True)
    recent = [dated_entry(src, date) for date, src in dated]

    # A guide whose newest commit is the one that added it has not been updated
    # -- it is new, which the card above already says. Listing it in both would
    # make Recently Updated a second copy of Recently Added for every guide
    # written this month, so an untouched-since-arrival guide is left out.
    edited: list[tuple[str, Path]] = [
        (mod, src) for src, add in zip(md_files, creation_dates)
        if (mod := modified_dates.get(src.stem)) and mod != add
    ]
    edited.sort(key=lambda t: t[0], reverse=True)
    recent_updated = [dated_entry(src, date) for date, src in edited]

    print('Generating guides:')
    # One run of the renderer for every guide (tools/mdrender.py), rather
    # than a process per guide: it takes well under a second for the lot.
    # render_guide stays in this loop, so stdout and the write order follow
    # md_files.
    built = build_guide_bodies(
        [(src.stem, src, meta_by_stem.get(src.stem, {})) for src in md_files])

    docs = []
    for src, doc in zip(md_files, built):
        render_guide(src.stem, src, out_dir / f'{src.stem}.html', all_stems,
                     doc=doc)
        docs.append(doc)
    render_index(categories, all_stems, out_dir, recent=recent,
                 recent_updated=recent_updated)
    print(f'Done: {len(md_files)} guides + index → {out_dir}')

    # Raw HTML in a guide is an accident until a page says otherwise: no
    # guide uses it on purpose, and Turmeric type syntax such as `ptr<void>`
    # or `<name>` written outside a code span is a tag to the browser, which
    # hides it. So any the renderer passed through fails the build, unless the
    # guide's front matter says `raw-html: allow` (markdown-library-plan 6.2).
    bad = [(doc['stem'], r) for doc in docs
           if doc['meta'].get('raw-html') != 'allow' for r in doc['raw_html']]
    for stem, r in bad:
        snippet = ' '.join(r['html'].split())[:60]
        print(f'error: {stem}.md:{r["line"]}: raw HTML: {snippet}', file=sys.stderr)
    if bad:
        sys.exit(f'error: {len(bad)} raw HTML node(s) in the guides; see above.\n'
                 '       Put Turmeric syntax in a code span, or write \\< for a '
                 'literal <.\n'
                 '       A page that means it says `raw-html: allow` in its '
                 'front matter.')

    if args.emit_pack:
        emit_pack_guides(docs, guides_dir, Path(args.emit_pack),
                         added_by_stem={src.stem: d
                                        for src, d in zip(md_files, creation_dates)
                                        if d},
                         updated_by_stem=modified_dates)


if __name__ == '__main__':
    main()
