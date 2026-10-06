#!/usr/bin/env python3
"""
tools/packlib.py -- shared helpers for emitting the Turmeric docs pack.

The docs pack is a chrome-free rendering of the guides, the stdlib API pages,
and (when the sibling checkout is present) the spice pages:

    web/public/docs-pack/
      index.json            version, nav tree, search strings, precache manifest
      guides/<slug>.html    article body only -- no <head>, no nav, no CSS
      api/<module>.html     same, one per module
      spices/<spice>.html   same, one per spice

It is emitted by the same generators that build docs/html/, is rendered in
Try Turmeric's in-app docs pane, and is precached wholesale by the service
worker so the pane works with no network at all. This module carries the parts
all three generators need: fragment writing, search-string extraction, and the
per-generator manifest sidecars that `tools/genpack.py` merges into index.json.
"""

from __future__ import annotations

import html as _html
import json
import re
import subprocess
import sys
from pathlib import Path
from typing import Iterable

# Every generator drops one of these into the pack root; genpack.py merges them
# into index.json and then deletes them. Keeping them out of index.json itself
# means a partial `just docs` (say, guides only) never produces a half-written
# contract file that the pane would happily believe.
SIDECAR_SUFFIX = '.pack-manifest.json'

# How much prose from a page body goes into its search string. Full bodies
# would put ~2.5 MB of text into index.json -- which the pane fetches on every
# open and the service worker precaches -- to serve a search that headings and
# descriptions already answer well. Headings are always kept in full; this cap
# applies only to the running text after them.
SEARCH_PROSE_CHARS = 800

_TAG_RE = re.compile(r'<[^>]+>')
_WS_RE = re.compile(r'\s+')


def strip_tags(html_text: str) -> str:
    """Return the visible text of an HTML fragment, whitespace-collapsed."""
    text = re.sub(r'(?is)<(script|style)\b.*?</\1>', ' ', html_text)
    text = _TAG_RE.sub(' ', text)
    text = _html.unescape(text)
    return _WS_RE.sub(' ', text).strip()


def search_string(*parts: str, prose: str = '') -> str:
    """Build the lowercase `words` blob the pane's client-side search matches on.

    `parts` are the high-signal fields (title, category, description, headings,
    symbol names) and are kept whole; `prose` is body text and is truncated to
    SEARCH_PROSE_CHARS.
    """
    kept = [p.strip() for p in parts if p and p.strip()]
    if prose:
        kept.append(prose[:SEARCH_PROSE_CHARS])
    return _WS_RE.sub(' ', ' '.join(kept)).strip().lower()


def heading_names(toc_tokens: list) -> list[str]:
    """Flatten a python-markdown toc_tokens tree into a list of heading names."""
    out = []
    for tok in toc_tokens:
        name = (tok.get('name') or '').strip()
        if name:
            out.append(name)
        out.extend(heading_names(tok.get('children', [])))
    return out


# ---------------------------------------------------------------------------
# When a page appeared, and when it last changed
#
# The pane's "Recently Added" and "Recently Updated" sections answer "what is
# new in here?" and "what has changed under me?", and the only durable record
# of either is the git history of a doc's source file. Reading it here, at pack
# time, keeps the answer out of the pack's content: a guide does not have to
# remember to carry a date in its front matter, and one that forgets does not
# quietly read as new forever.
#
# One `git log` for the whole set rather than one per file, and one traversal
# for both dates rather than one each: the oldest commit touching a path is the
# commit that added it and the newest is its last edit, so a single newest-first
# pass yields both ends. The guides index asks the same question per guide with
# `--follow`, which can only take a single path; the pack asks it about ~300, so
# this pass trades rename following for a single traversal (see git_page_dates).
# ---------------------------------------------------------------------------

_ISO_DATE_RE = re.compile(r'^\d{4}-\d{2}-\d{2}T')


def find_repo_root(start: Path) -> Path | None:
    """Nearest ancestor of `start` holding a `.git`, or None outside a checkout.

    A `.git` file counts as well as a directory, so this finds the root from
    inside a worktree.
    """
    cur = Path(start).resolve()
    while True:
        if (cur / '.git').exists():
            return cur
        if cur == cur.parent:
            return None
        cur = cur.parent


_warned_shallow: set[Path] = set()


def warn_if_shallow(repo_root: Path | None) -> None:
    """Print a stderr warning, once per repo, when `repo_root` is a shallow clone.

    Every page date this module and the guides index derive comes from git
    history, and a shallow clone has none to give: each page reads as added
    and last updated by the clone's tip commit. Nothing fails -- the output is
    just quietly wrong (an empty Recently Updated, a Recently Added that is the
    alphabetically first pages, all dated today) -- so say so where a build log
    will show it. Fetch full history (`fetch-depth: 0`, or `git fetch
    --unshallow`) to fix it.
    """
    if repo_root is None:
        return
    root = Path(repo_root).resolve()
    if root in _warned_shallow:
        return
    _warned_shallow.add(root)
    try:
        proc = subprocess.run(['git', 'rev-parse', '--is-shallow-repository'],
                              cwd=root, capture_output=True, text=True,
                              check=False)
    except OSError:
        return
    if proc.stdout.strip() == 'true':
        print(f'warning: {root} is a shallow git clone, so page dates '
              '(Recently Added / Recently Updated) will all read as the tip '
              "commit's date; fetch full history (fetch-depth: 0 or "
              '`git fetch --unshallow`) to date pages correctly',
              file=sys.stderr)


def git_page_dates(paths: Iterable[Path],
                   repo_root: Path | None) -> dict[Path, dict[str, str]]:
    """Map each path to `{'added': 'YYYY-MM-DD', 'updated': 'YYYY-MM-DD'}`.

    `added` is the day the file was first committed; `updated` the day it was
    last committed to. A file whose only commit is the one that added it has
    both, equal -- the caller decides what that means (the pane's Recently
    Updated section reads it as "never edited since it landed" and leaves the
    page out).

    A path git has no commit for is absent from the result rather than guessed
    at -- an untracked draft, a file outside `repo_root`, a build from a release
    tarball with no history at all. The pack then carries no dates for that page
    and the pane's dated sections simply never list it, which is the honest
    answer to "when did this appear?" when nothing recorded it.

    Renames are not followed: a doc that moved reads as added where it now
    lives. That is what buys the single traversal -- `--follow` takes exactly
    one path -- and for a *recently* added list it is also the more useful of
    the two answers, since a page that arrived under this name last week did
    arrive last week.

    Dates are author dates, matching the guides index's own cards.
    """
    root = Path(repo_root).resolve() if repo_root else None
    if root is None:
        return {}
    warn_if_shallow(root)

    # Query by repo-relative path, answer by the caller's own Path objects:
    # the caller looks results up with the path it passed in, not with whatever
    # spelling git prints.
    by_rel: dict[str, Path] = {}
    for path in paths:
        try:
            by_rel[Path(path).resolve().relative_to(root).as_posix()] = Path(path)
        except (ValueError, OSError):
            continue
    if not by_rel:
        return {}

    try:
        proc = subprocess.run(
            ['git', 'log', '--name-only', '--no-renames',
             '--format=%aI', '--', *sorted(by_rel)],
            cwd=root, capture_output=True, text=True, check=False)
    except OSError:
        return {}  # no git on this machine
    if proc.returncode != 0:
        return {}

    out: dict[Path, dict[str, str]] = {}
    date: str | None = None
    for line in proc.stdout.splitlines():
        line = line.strip()
        if not line:
            continue
        if _ISO_DATE_RE.match(line):
            date = line[:10]
        elif date and line in by_rel:
            # The log runs newest first, so the first line for a path is its
            # latest edit and every later one is older. `updated` therefore
            # takes the first and keeps it; `added` keeps overwriting, so it
            # ends on the oldest commit -- and a file that was deleted and
            # restored counts from when it first appeared, not from its return.
            rec = out.setdefault(by_rel[line], {'updated': date})
            rec['added'] = date
    return out



# ---------------------------------------------------------------------------
# Link rewriting
#
# Fragments are written with the links their source had -- `other-guide.md`,
# `../api/tur-vec.html`. Resolving them into the pack's own `#doc=` URL space
# needs to know what the whole pack contains, which no single generator does,
# so `genpack.py` runs `rewrite_pack_links` over every fragment once all of
# them have landed. One pass, one place, and the same pass reports the links it
# could not resolve.
# ---------------------------------------------------------------------------

# `[text](other-guide.md)` / `[text](other-guide.md#anchor)` after conversion.
_MD_HREF_RE = re.compile(r'href="([^"#]+)\.md(#[^"]*)?"')

# The site's own API pages, as guides reference them: `../api/tur-vec.html`
# from docs/guides/, or `../html/api/tur-vec.html` from elsewhere under docs/.
_SITE_API_HREF_RE = re.compile(
    r'href="(?:\.\./)+(?:html/)?api/([A-Za-z0-9_\-]+)\.html(#[^"]*)?"')

_EXTERNAL_PREFIXES = ('http://', 'https://', 'mailto:', '#', '/')


def rewrite_pack_links(body_html: str, guide_slugs: set[str],
                       api_slugs: set[str],
                       spice_slugs: set[str] | None = None,
                       ) -> tuple[str, list[str]]:
    """Point a fragment's cross-links at other pack pages.

    In-pack targets become `#doc=guides/<slug>` / `#doc=api/<module>` /
    `#doc=spices/<name>`, which the Try docs pane intercepts and resolves
    without ever navigating away from the REPL. A link whose target is not in
    the pack is left exactly as written -- it still resolves against the
    website for an online reader -- and is returned in the second element so
    the emitter can report it instead of the pane silently swallowing a dead
    click.
    """
    unresolved: list[str] = []
    spice_slugs = spice_slugs or set()

    def rewrite_md(m: re.Match) -> str:
        href, frag = m.group(1), m.group(2) or ''
        if href.startswith(_EXTERNAL_PREFIXES):
            return m.group(0)
        parts = href.split('/')
        slug = parts[-1]
        # A spice's front page IS its README, so one spice links to another as
        # `../json/README.md`: the spice is named by the parent directory and
        # the basename is the same for all of them. Match the parent before the
        # basename-keyed guide lookup, which would otherwise ask after a guide
        # called "README" and never find one.
        if slug.lower() == 'readme' and len(parts) >= 2 and parts[-2] in spice_slugs:
            return f'href="#doc=spices/{parts[-2]}{frag}"'
        if slug not in guide_slugs:
            unresolved.append(f'{href}.md')
            return m.group(0)
        return f'href="#doc=guides/{slug}{frag}"'

    def rewrite_api(m: re.Match) -> str:
        page, frag = m.group(1), m.group(2) or ''
        if page not in api_slugs:
            unresolved.append(f'api/{page}.html')
            return m.group(0)
        return f'href="#doc=api/{page}{frag}"'

    body_html = _MD_HREF_RE.sub(rewrite_md, body_html)
    body_html = _SITE_API_HREF_RE.sub(rewrite_api, body_html)
    return body_html, unresolved


_IMG_SRC_RE = re.compile(r'<img\b[^>]*?\bsrc="([^"]+)"')


def local_image_srcs(body_html: str) -> list[str]:
    """Every non-external `<img src>` in a fragment, in document order.

    Guides carry no images today; the pack copies and budget-counts them anyway
    so the day one arrives it is a build-time number, not a silent PWA payload.
    """
    return [m.group(1) for m in _IMG_SRC_RE.finditer(body_html)
            if not m.group(1).startswith(_EXTERNAL_PREFIXES)
            and not m.group(1).startswith('data:')]


def write_fragment(pack_dir: Path, rel_path: str, body_html: str) -> int:
    """Write one chrome-free fragment into the pack. Returns its byte size."""
    out = Path(pack_dir) / rel_path
    out.parent.mkdir(parents=True, exist_ok=True)
    data = body_html.strip() + '\n'
    out.write_text(data, encoding='utf-8')
    return len(data.encode('utf-8'))


def write_sidecar(pack_dir: Path, section: str, entries: list[dict]) -> Path:
    """Write one generator's slice of the pack manifest for genpack.py to merge.

    Merges into an existing sidecar for the same section, replacing entries
    with the same slug: `genspices.py` contributes to the `spices` section once
    per spice, so the section is built up across calls rather than in one shot.
    """
    pack_dir = Path(pack_dir)
    pack_dir.mkdir(parents=True, exist_ok=True)
    out = pack_dir / f'{section}{SIDECAR_SUFFIX}'

    merged: dict[str, dict] = {}
    if out.is_file():
        try:
            prior = json.loads(out.read_text(encoding='utf-8'))
            for entry in prior.get('entries', []):
                merged[entry.get('slug', '')] = entry
        except (OSError, ValueError):
            pass
    for entry in entries:
        merged[entry.get('slug', '')] = entry

    out.write_text(json.dumps({'section': section,
                               'entries': list(merged.values())},
                              ensure_ascii=True, separators=(',', ':')),
                   encoding='utf-8')
    return out


def read_sidecars(pack_dir: Path) -> dict[str, list[dict]]:
    """Read and remove every sidecar in the pack; returns {section: entries}."""
    pack_dir = Path(pack_dir)
    sections: dict[str, list[dict]] = {}
    for path in sorted(pack_dir.glob(f'*{SIDECAR_SUFFIX}')):
        try:
            payload = json.loads(path.read_text(encoding='utf-8'))
        except (OSError, ValueError):
            continue
        section = payload.get('section') or path.name[:-len(SIDECAR_SUFFIX)]
        sections.setdefault(section, []).extend(payload.get('entries', []))
        path.unlink()
    return sections
