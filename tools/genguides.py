#!/usr/bin/env python3
"""
tools/genguides.py -- Render Turmeric guide markdown files to HTML.

Usage:
    python3 tools/genguides.py docs/guides/ [--out docs/html/guides/]
                                            [--emit-pack web/public/docs-pack/]

Two consumers, one rendering pass. `build_guide_body` converts a guide's
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
import packlib  # noqa: E402  (sibling module, path fixed up just above)

try:
    import markdown as md_lib
except ImportError:  # pragma: no cover -- preflight, not a code path
    sys.exit(
        "error: tools/genguides.py needs the 'markdown' package\n"
        "       install it with:  python3 -m pip install -r tools/requirements.txt\n"
        "       (or:  python3 -m pip install markdown)"
    )


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
    for line in fm_text.splitlines():
        if ':' in line:
            k, _, v = line.partition(':')
            k, v = k.strip(), v.strip()
            # A YAML value quoted because it contains a colon -- which is why
            # several guide titles are quoted -- is still just its text. Left
            # in, the quotes render as part of the title everywhere the title
            # goes: the page's <title>, the index card, and the docs pane.
            if len(v) >= 2 and v[0] == v[-1] and v[0] in '"\'':
                v = v[1:-1]
            if k:
                meta[k] = v
    return meta, body


def build_categories_from_meta(meta_by_stem: dict, all_stems: set) -> list:
    """Build ordered category list from guide front matter."""
    buckets: dict[str, list] = {}
    for stem in sorted(all_stems):
        meta = meta_by_stem.get(stem, {})
        cat = meta.get('category', '').strip() or 'Other'
        title = meta.get('title', stem.replace('-', ' ').title()).strip()
        desc = meta.get('description', '').strip()
        buckets.setdefault(cat, []).append({'stem': stem, 'label': title, 'desc': desc})
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

      document.querySelectorAll('.index-card:not(.recent-tabs)').forEach(function(card) {
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
    'set!','try','catch','finally','with','use',
    'import','export','module','require','provide',
    'cons','car','cdr','nil-value','some','none','ok','err',
    'map','filter','reduce','apply','return','yield','raise','throw',
    'coerce','cast','type-of','any',
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

  function hl(code, spec){
    spec = spec || TUR_SPEC;
    var out='', i=0, n=code.length;
    while(i<n){
      var c=code[i];
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
          out+='<span class="hl-type">'+esc(hv[0].slice(0,-1))+'</span>'+esc('(');
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
      // Type annotation :keyword
      if(spec.colonType&&c===':'&&i+1<n&&/[a-zA-Z_]/.test(code[i+1])){
        var j=i+1;
        while(j<n&&/[a-zA-Z0-9_\\-?!]/.test(code[j]))j++;
        out+='<span class="hl-type">'+esc(code.slice(i,j))+'</span>'; i=j; continue;
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
        if(spec.lit.has(sym)){
          out+='<span class="hl-number">'+esc(sym)+'</span>';
        } else if(spec.kw.has(sym)){
          out+='<span class="hl-keyword">'+esc(sym)+'</span>';
        } else {
          out+=esc(sym);
        }
        i=j; continue;
      }
      out+=esc(c); i++;
    }
    return out;
  }

  // The five Scheme-family flags are off for Turmeric, which has none of that
  // syntax, and on for both Racket and Scheme, which share all of it.
  var TUR_SPEC = { kw:KW,     lit:TUR_LIT, inlineC:true,  colonType:true,
                   blockComment:false, hashKeyword:false,
                   datumComment:false, charLit:false, hashVector:false,
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
  // `build_guide_body` has already turned every ```mermaid fence into a
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
      themeVariables: {
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
      }
    };
  }

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

  var api = { highlightGuideCode: highlightGuideCode, initSyntaxToggles: initSyntaxToggles,
              renderMermaid: renderMermaid };
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
    .hl-comment { color:#48433D; font-style:italic; }
    .hl-string  { color:#D9735A; }
    .hl-number  { color:#A8C98A; }
    .hl-keyword { color:#EFA030; font-weight:bold; }
    .hl-type    { color:#7AC4B8; }
    .code-toggle { border:1px solid var(--border); border-radius:4px; margin-bottom:1rem; overflow:hidden; }
    .code-card-bar { background:var(--bg-surface); border-bottom:1px solid var(--border); padding:0.35rem 0.75rem; display:flex; align-items:center; }
    .code-syntax-toggle { margin-left:auto; display:flex; border:1px solid var(--border); border-radius:4px; overflow:hidden; font-family:"Iosevka","Fira Code",monospace; font-size:11px; }
    .seg-btn { padding:3px 10px; background:transparent; color:var(--text-sec); border:none; cursor:pointer; transition:all 0.14s; }
    .seg-btn:hover { color:var(--text-primary); }
    .seg-btn.active { color:var(--gold-bright); background:var(--bg-hover); }
    .code-card-body { }
    .code-version { }
    .guide-content .code-toggle pre { border:none; border-radius:0; margin-bottom:0; }'''


# A GFM task-list item as it appears AFTER markdown conversion. python-markdown
# ships no task-list extension, so `- [ ] text` reaches the HTML as a literal
# `<li>[ ] text`. Matching post-conversion is what keeps the source a plain
# bullet list -- which GitHub already renders as a checklist on its own -- and
# costs no new package in tools/requirements.txt.
_TASK_ITEM_RE = re.compile(r'<li>\[([ xX])\]\s+')


def render_task_lists(body_html: str) -> str:
    """Render `- [ ]` / `- [x]` list items as real, disabled checkboxes."""
    def sub(m: re.Match) -> str:
        checked = ' checked' if m.group(1) in 'xX' else ''
        return f'<li class="task-item"><input type="checkbox" disabled{checked}> '

    return _TASK_ITEM_RE.sub(sub, body_html)


# A ```mermaid fence, as python-markdown's fenced_code leaves it. Mermaid
# renders from `<pre class="mermaid">`, not from the `<pre><code>` pair every
# other fence becomes, so the block is unwrapped here rather than in the
# browser -- one rewrite at build time instead of a DOM fixup on every page.
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
    """Remove a hand-written Table of Contents section from guide markdown."""
    return _MANUAL_TOC_RE.sub('', text, count=1)


_LANG_FENCE_OPEN_RE = re.compile(r'(?m)^(`{3})(turmeric|sweet-exp)([^\n]*)\n')


def _read_fenced_block(text: str, pos: int) -> tuple[str, int]:
    """Read a markdown fenced block whose opening fence's newline is at `pos`.

    Returns (content, end) where `end` is just past the closing fence line.

    A markdown block closes at a column-0 bare ``` line. Turmeric inline-C
    blocks use ``` to toggle a C span (```c opens; ``` or ```) closes) and may
    be indented or written inline, so track the C span and only treat a bare
    ``` as the block close when not inside one. This is the same scan
    tools/check-guide-pairs.py uses to delimit blocks.
    """
    n = len(text)
    i = pos
    line_start = pos
    in_c = False
    while i < n:
        if text.startswith('```', i):
            at_col0 = (i == line_start)
            after = text[i + 3] if i + 3 < n else '\n'
            if in_c:
                in_c = False
                i += 3
                continue
            if after.isalnum():           # info string -> opens a (C) span
                in_c = True
                i += 3
                continue
            if at_col0:                   # bare ``` at column 0 -> block close
                j = text.find('\n', i)
                end = (j + 1) if j != -1 else n
                return text[pos:i], end
            i += 3
            continue
        if text[i] == '\n':
            line_start = i + 1
        i += 1
    return text[pos:], n


def widen_nested_fences(text: str) -> str:
    """Re-fence turmeric/sweet-exp blocks that contain inline-C fences.

    Turmeric's inline-C syntax puts ``` runs *inside* a code block. The project
    style closes an inline-C body with ```) on the same line, which markdown
    ignores -- but a module-level inline-C block legitimately closes with a bare
    ``` at column 0, and python-markdown's fenced_code reads that as the end of
    the *enclosing* turmeric block. Everything after it then renders as prose
    instead of code (docs/guides/thread-pool-guide.md is the case in the tree).

    Widening the enclosing fence to five backticks makes the inner
    three-backtick runs ordinary content, so the block survives intact. Only
    blocks that actually contain a nested run are touched.
    """
    out = []
    pos = 0
    for m in _LANG_FENCE_OPEN_RE.finditer(text):
        if m.start() < pos:
            continue  # inside a block already consumed
        content, end = _read_fenced_block(text, m.end())
        if '```' not in content:
            continue
        out.append(text[pos:m.start()])
        out.append(f'`````{m.group(2)}{m.group(3)}\n{content}`````\n')
        pos = end
    if not out:
        return text
    out.append(text[pos:])
    return ''.join(out)


_FENCE_LINE_RE = re.compile(r'^( {0,3})(`{3,}|~{3,})([^\n]*)$')


def dedent_indented_fences(text: str) -> str:
    """Move a fence indented by 1-3 spaces to column 0.

    CommonMark lets a fence sit at a list item's content column -- two spaces
    under `- `, three under `2. ` -- and GitHub renders it inside the item.
    python-markdown's fenced_code only knows a column-0 fence, and its lists
    want four-space continuation besides, so such a block rendered as running
    text -- in sixteen guides when this was written, most often as one
    inline <code> span flattening the snippet into a sentence.  Dedented, it
    renders as a code block with its language class (so the turmeric/sweet-exp
    toggles apply), directly after the list item.

    Only fences OUTSIDE a column-0 fenced block are touched: a turmeric block's
    inline C legitimately holds indented ``` runs.
    """
    lines = text.split('\n')
    out = []
    i = 0
    outer = None                           # (char, len) of an open column-0 fence
    while i < len(lines):
        ln = lines[i]
        m = _FENCE_LINE_RE.match(ln)
        if outer:
            if m and not m.group(1) and m.group(2)[0] == outer[0] and \
               len(m.group(2)) >= outer[1] and not m.group(3).strip():
                outer = None
            out.append(ln)
            i += 1
            continue
        if not m:
            out.append(ln)
            i += 1
            continue
        indent, fence = m.group(1), m.group(2)
        if not indent:
            outer = (fence[0], len(fence))
            out.append(ln)
            i += 1
            continue
        # An indented opening fence: find its closer at the same indent.
        j = i + 1
        while j < len(lines):
            c = _FENCE_LINE_RE.match(lines[j])
            if c and c.group(1) == indent and c.group(2)[0] == fence[0] and \
               len(c.group(2)) >= len(fence) and not c.group(3).strip():
                break
            j += 1
        if j >= len(lines):                # unclosed: leave it as it is
            out.append(ln)
            i += 1
            continue
        n = len(indent)
        for k in range(i, j + 1):
            row = lines[k]
            lead = len(row) - len(row.lstrip(' '))
            out.append(row[min(n, lead):])
        i = j + 1
    return '\n'.join(out)


_QUOTED_FENCE_OPEN_RE = re.compile(r'^( {0,3}(?:>[ \t]?)+)(`{3,}|~{3,})[^\n`]*$')


def unquote_blockquote_fences(text: str) -> str:
    """Render a fenced block inside a blockquote as code.

    python-markdown's fenced_code is a preprocessor over the whole text, and it
    only knows a fence that starts at column 0. Inside a blockquote every line
    starts with `>`, so the fence is never seen: the opening ```sh renders as a
    literal paragraph, a `# comment` in the snippet becomes an <h1> -- and a
    TOC entry -- and `<tag>` reaches the HTML as an element.  The source is
    ordinary CommonMark (GitHub renders it), so the guide is not what is wrong.

    The blockquote parser DOES recurse into block processing, and an indented
    code block is a block processor, so the fence is rewritten into that form:
    same quote prefix, four more spaces per line, a bare quoted line in place of
    each fence (an indented block cannot interrupt a paragraph).  The info
    string's language is dropped; nothing in a guide highlights by it outside
    the turmeric/sweet-exp toggles, which never appear quoted.  An unclosed
    fence is left alone.
    """
    lines = text.split('\n')
    out = []
    i = 0
    while i < len(lines):
        m = _QUOTED_FENCE_OPEN_RE.match(lines[i])
        if not m:
            out.append(lines[i])
            i += 1
            continue
        prefix, fence = m.group(1), m.group(2)
        quote = prefix.rstrip()
        close_re = re.compile(r'^%s{%d,}[ \t]*$' % (re.escape(fence[0]), len(fence)))
        body, j, closed = [], i + 1, False
        while j < len(lines):
            ln = lines[j]
            if ln.rstrip() == quote:
                inner = ''
            elif ln.startswith(prefix):
                inner = ln[len(prefix):]
            else:
                break                      # the blockquote ended first
            if close_re.match(inner):
                closed = True
                break
            body.append(inner)
            j += 1
        if not closed:
            out.append(lines[i])
            i += 1
            continue
        out.append(quote)
        out.extend(quote + '     ' + b if b.strip() else quote for b in body)
        out.append(quote)
        i = j + 1
    return '\n'.join(out)


def unrendered_fences(body_html: str) -> list:
    """Fences the renderer never saw, left in the output as text.

    The signature is a ``` run that STARTS a line of prose -- right after a
    <p> or <li>, or at the head of a line inside one -- with code spans and
    <pre> blocks removed first, since backticks legitimately survive there.  A
    run in the middle of a sentence or a table cell is prose that mentions
    backticks, not a fence, and is left alone.

    The second signature is the same failure one step later: when the opening
    fence line is not a fence (an info string fenced_code rejects), the three
    backticks pair up as an inline code span, so a paragraph OPENS with a
    <code> that runs across a line break -- the info string and the code,
    flattened into one sentence.

    Returns a short excerpt per occurrence."""
    def excerpt(text, m):
        return text[max(0, m.start() - 40):m.end() + 60].replace('\n', ' ')
    found = [excerpt(body_html, m)
             for m in re.finditer(r'<p><code>[^<]*\n', body_html)]
    prose = re.sub(r'<pre\b.*?</pre>|<code\b.*?</code>', '', body_html, flags=re.S)
    found += [excerpt(prose, m)
              for m in re.finditer(r'(?:<p>|<li>|\n)[ \t]*(```)', prose)]
    return found


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


def build_guide_body(stem: str, src: Path, meta: dict | None = None) -> dict:
    """Render one guide's markdown to its article body -- the single rendering pass.

    The returned ``body`` still carries the source's raw ``href="other.md"``
    cross-links: each consumer rewrites them into its own URL space
    (``rewrite_links_site`` for docs/html/, ``rewrite_links_pack`` for the docs
    pack). Everything else about the body -- the syntax toggles, the in-body
    Contents box, the heading anchors -- is identical for both, which is what
    keeps the site and the pack from drifting.

    Returns a dict with: stem, title, body, toc_tokens, meta.
    """
    raw = src.read_text(encoding='utf-8')
    fm_meta, text = parse_front_matter(raw)
    if meta is None:
        meta = fm_meta

    # First, so the marker stripping below sees a list item's fence too.
    text = dedent_indented_fences(text)

    # Drop every checker marker from an opening fence's info string.
    #
    # `no-check`, `no-manifest-check` and anything check-guide-pairs.py grows
    # next are instructions to *that* tool; python-markdown's fenced_code only
    # accepts a single bare word after the fence, so a marker left in place
    # stops the line being a fence at all. The block then renders as a literal
    # paragraph and its closing ``` opens a new one, swallowing the prose and
    # headings that follow until the next fence -- silently, because nothing
    # errors. Matching the markers generically rather than by name is what
    # keeps the next one from repeating it.
    text = re.sub(r'^(`{3,})(turmeric|sweet-exp)[ \t]+(?!\{)[^\n]*', r'\1\2', text,
                  flags=re.MULTILINE)
    text = strip_manual_toc(text)
    text = widen_nested_fences(text)
    text = unquote_blockquote_fences(text)

    conv = md_lib.Markdown(extensions=['fenced_code', 'tables', 'toc'],
                            extension_configs={'toc': {'permalink': False}})
    # Keep an ordered list's own first number. A fence inside a list item
    # (dedented above) ends the list for python-markdown, so the items after
    # it open a new <ol>, which would otherwise restart at 1. The `lazy_ol`
    # keyword is ignored by Markdown 3.x, and the sane_lists extension that
    # sets this also stops `-` and `1.` lists merging, so set it directly.
    conv.parser.blockprocessors['olist'].LAZY_OL = False
    body_html = conv.convert(text)
    fence_errors = unrendered_fences(body_html)
    body_html = inject_syntax_toggles(body_html)
    body_html = render_task_lists(body_html)
    body_html = wrap_tables(body_html)
    body_html = render_mermaid_blocks(body_html)
    toc_tokens = getattr(conv, 'toc_tokens', [])

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
        title_match = re.match(r'^#\s+(.+)', text, re.MULTILINE)
        title = title_match.group(1) if title_match else stem.replace('-', ' ').title()

    return {
        'stem': stem,
        'title': title,
        'body': body_html,
        'toc_tokens': toc_tokens,
        'meta': meta,
        'fence_errors': fence_errors,
    }


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


def _build_guide_body_job(job: tuple) -> dict:
    """Process-pool entry point for build_guide_body -- must be top-level to pickle."""
    stem, src, meta = job
    return build_guide_body(stem, src, meta)


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

    sidebar_cats_list = [
        f'<li><a href="#{re.sub(r" +", "-", c["name"].lower())}" '
        f'title="Jump to {_html.escape(c["name"], quote=True)}">{c["name"]}</a></li>'
        for c in categories if any(g['stem'] in all_stems for g in c['guides'])
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
    sidebar_html = build_sidebar(
        toc=f'      <h3>Categories</h3>\n      <ul>{sidebar_cats}</ul>')

    def dated_items(entries: list[dict]) -> str:
        return ''.join(
            f'<li><a href="{r["stem"]}.html">{_fmt_inline(r["label"])}</a>'
            f'<span style="color:var(--text-sec)"> -- {r["date"]}</span></li>'
            for r in entries
        )

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
        f'<ul class="recent-panel" role="tabpanel" id="{slug}"'
        f' aria-labelledby="{slug}-tab" style="list-style:none;margin:0"'
        f'{"" if i == 0 else " hidden"}>{dated_items(entries)}</ul>'
        for i, (slug, _, entries) in enumerate(tabs)
    )
    recent_html = f'''\
      <div class="index-card recent-tabs" style="display:block;margin-bottom:1.5rem">
        <div class="recent-tablist" role="tablist" aria-label="Recent guide changes">{buttons}</div>
        {panels}
      </div>''' if tabs else ''

    html = f'''<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Guides | Turmeric</title>
  <link rel="icon" type="image/svg+xml" href="/favicon.svg">
{font_links()}
  <link rel="stylesheet" href="{STYLE_REL}">
  <style>
    .index-card ul li {{ margin:0.3rem 0; font-size:0.875rem; }}
    .index-card ul li a {{ color:var(--text-primary); }}
    .index-card ul li a:hover {{ color:var(--gold); }}
    .recent-tablist {{ display:inline-flex; border:1px solid var(--border); border-radius:4px; overflow:hidden; margin-bottom:0.6rem; font-family:system-ui; font-size:0.8rem; }}
    .recent-tablist .seg-btn {{ padding:4px 12px; background:transparent; color:var(--text-sec); border:none; cursor:pointer; font:inherit; transition:all 0.14s; }}
    .recent-tablist .seg-btn + .seg-btn {{ border-left:1px solid var(--border); }}
    .recent-tablist .seg-btn:hover {{ color:var(--text-primary); }}
    .recent-tablist .seg-btn.active {{ color:var(--gold-bright); background:var(--bg-hover); }}
    .recent-tablist .seg-btn:focus-visible {{ outline:2px solid var(--gold); outline-offset:-2px; }}
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
      <div class="module-heading">
        <h1 style="font-family:system-ui;color:var(--gold)">Guides</h1>
        <div class="module-path guide-count">There are currently {len(all_stems)} tutorials, how-tos, and in-depth feature guides for Turmeric.</div>
        <p class="module-path">Visit the <a href="https://spices.turmeric-lang.com/">Spices</a> page for spice-specific guides.</p>
        <p class="module-path">For docs for the Trowel editor, <a href="https://github.com/turmeric-lang/trowel#using-trowel">go here</a>.</p>
      </div>
{recent_html}
      <div class="index-grid">
        {"".join(cards)}
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

    dated: list[tuple[str, Path]] = [
        (d, src) for src, d in zip(md_files, creation_dates) if d
    ]
    dated.sort(key=lambda t: t[0], reverse=True)
    recent = [{'stem': src.stem, 'label': label_for(src), 'date': date}
              for date, src in dated[:10]]

    # A guide whose newest commit is the one that added it has not been updated
    # -- it is new, which the card above already says. Listing it in both would
    # make Recently Updated a second copy of Recently Added for every guide
    # written this month, so an untouched-since-arrival guide is left out.
    edited: list[tuple[str, Path]] = [
        (mod, src) for src, add in zip(md_files, creation_dates)
        if (mod := modified_dates.get(src.stem)) and mod != add
    ]
    edited.sort(key=lambda t: t[0], reverse=True)
    recent_updated = [{'stem': src.stem, 'label': label_for(src), 'date': date}
                      for date, src in edited[:10]]

    print('Generating guides:')
    # Markdown conversion is the other half of the runtime (~17s of the
    # original ~34s, essentially all of it inside markdown's inline
    # treeprocessor) and it is pure CPU, so it needs processes rather than
    # threads. Only build_guide_body is farmed out: render_guide is cheap
    # f-string assembly plus the file write, and keeping it -- and its
    # per-guide print -- in the parent is what keeps stdout and the write
    # order byte-identical to the serial version. `ex.map` preserves order,
    # so `docs` is built in md_files order either way.
    n_md_workers = min(os.cpu_count() or 1, len(md_files)) or 1
    jobs = [(src.stem, src, meta_by_stem.get(src.stem, {})) for src in md_files]
    if n_md_workers > 1 and len(jobs) > 1:
        from concurrent.futures import ProcessPoolExecutor
        with ProcessPoolExecutor(max_workers=n_md_workers) as ex:
            built = list(ex.map(_build_guide_body_job, jobs))
    else:
        built = [_build_guide_body_job(j) for j in jobs]

    docs = []
    for src, doc in zip(md_files, built):
        render_guide(src.stem, src, out_dir / f'{src.stem}.html', all_stems,
                     doc=doc)
        docs.append(doc)
    render_index(categories, all_stems, out_dir, recent=recent,
                 recent_updated=recent_updated)
    print(f'Done: {len(md_files)} guides + index → {out_dir}')

    # A fence the renderer never saw turns the rest of its block into prose --
    # headings out of `#` comments, elements out of `<placeholders>` -- with
    # no error.  genpack's fragment check only notices when the stray text
    # happens to look like an unclosed tag, so fail on the cause here.
    bad = [(doc['stem'], e) for doc in docs for e in doc.get('fence_errors', [])]
    for stem, excerpt in bad:
        print(f'error: {stem}.md: a code fence rendered as text: ...{excerpt}...',
              file=sys.stderr)
    if bad:
        sys.exit(f'error: {len(bad)} unrendered code fence(s); see above')

    if args.emit_pack:
        emit_pack_guides(docs, guides_dir, Path(args.emit_pack),
                         added_by_stem={src.stem: d
                                        for src, d in zip(md_files, creation_dates)
                                        if d},
                         updated_by_stem=modified_dates)


if __name__ == '__main__':
    main()
