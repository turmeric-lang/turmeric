// docs-pane.js -- the in-app docs browser (OD2).
//
//
// Reads the docs pack -- chrome-free fragments plus index.json, emitted by the
// same generators that build turmeric-lang.com/docs/html/ -- and renders it
// over the REPL. Nothing here navigates: the editor buffer, console history,
// and WASM session all survive a docs session untouched, which is the whole
// point of embedding rather than linking out.
//
// The pack is precached by the service worker (OD3), so every fetch below is
// expected to hit the cache when offline.

import { STORAGE_KEYS, safeRead, safeWrite, debounce, hasUrlHashCode, isTutorialMode } from './storage.js';
import { escapeHtml, escapeAttr } from './html.js';
import { setHashParam, getHashParam } from './hash-params.js';
import { getEditor } from './editor-ref.js';
import { showStatus } from './status.js';
import { docNames, fetchDocNames, wasmDocLookup, showDocPanel } from './doc-panel.js';

/** Where the pane is, for window.turmericApp.getState(). */
export function docsPaneState() {
    return { ref: docsCurrentRef, guideView: docsGuideView };
}

const DOCS_PACK_BASE = '/docs-pack';

/**
 * How many pages each of the nav's dated sections lists.
 *
 * Ten apiece, matching one page of the Recently Added and Recently Updated tabs
 * at the top of the website's guides index (RECENT_PAGE_SIZE in
 * tools/genguides.py) -- and, like those tabs, guides only. The pack
 * stamps API modules and spice pages with dates too, but these lists answer
 * "what is new to read?", and a module page is reference that its API tree
 * already files; mixed in, it takes a slot from a guide. The dates behind the
 * pane and the website are the same numbers -- the generator stamps each page
 * with the day its source was first committed and the day it was last committed
 * to, and passes the guides' dates straight through -- so the two cannot
 * disagree about when a guide arrived or when it last changed.
 */
const DOCS_RECENT_COUNT = 10;

/**
 * The page the pane opens on when nothing else says where to go.
 *
 * The quickstart, not whatever guide happens to sort first in the pack. A
 * reader who opened the docs without asking for a page has not told us what
 * they want, and "the beginning" is the only answer that is right for a first
 * visit and harmless for every other -- an alphabetical accident is neither.
 * A previously visited page still wins; this is the floor, not a redirect.
 */
const DOCS_DEFAULT_REF = 'guides/quickstart';

let docsIndex = null;          // parsed index.json, once
let docsIndexPromise = null;   // in-flight load, so concurrent opens share one
let docsCurrentRef = null;     // e.g. 'guides/hkt-guide'
let docsNavCollapsed = false;
/**
 * How the nav's Guides section is grouped: 'category' (the default) or 'tag'.
 * The same choice, and the same two names, as the By Category / By Tag switch
 * on the website's guides index (tools/genguides.py). Remembered across
 * launches like the website remembers it; anything but 'tag' reads as
 * 'category', so a stale or hand-edited value cannot strand the nav.
 */
let docsGuideView = safeRead(STORAGE_KEYS.docsView) === 'tag' ? 'tag' : 'category';
/**
 * Where you were in each page you have read this session, keyed by ref.
 *
 * Per-ref rather than one global offset on purpose: the pane is a browser over
 * many pages, so coming back to guide A must not restore guide B's offset.
 *
 * The map itself is session-scoped: every page you read this session is worth
 * an entry in memory, and none of them is worth a localStorage write. The one
 * exception is the page the pane is actually showing when the app goes away,
 * which persistDocsState() banks alongside the ref -- restoring the open guide
 * but not the place in it just moves the complaint one step along.
 */
const docsScrollByRef = new Map();

/**
 * Record where the article column is scrolled to for the page it is showing.
 *
 * Refuses to record while the pane is closed. A hidden element reports
 * scrollTop 0, so a `scroll` callback still queued when the overlay went
 * display:none would overwrite the offset closeDocsPane had just banked --
 * with exactly the value the restore is supposed to avoid.
 */
function rememberDocsScroll() {
    const article = document.getElementById('docs-article');
    if (!article || !docsCurrentRef || !docsPaneIsOpen()) return;
    docsScrollByRef.set(docsCurrentRef, article.scrollTop);
}

/**
 * Fetch and cache the pack manifest. Resolves to null when the pack is absent
 * (a dev server that never ran `tur run docs`), which the pane reports rather
 * than failing silently.
 */
export function loadDocsIndex() {
    if (docsIndex) return Promise.resolve(docsIndex);
    if (docsIndexPromise) return docsIndexPromise;
    docsIndexPromise = (async () => {
        try {
            const res = await fetch(`${DOCS_PACK_BASE}/index.json`);
            if (!res.ok) throw new Error(`HTTP ${res.status}`);
            docsIndex = await res.json();
            return docsIndex;
        } catch (err) {
            console.warn('docs pack unavailable:', err);
            docsIndexPromise = null;
            return null;
        }
    })();
    return docsIndexPromise;
}

/** All pack pages as one flat list of {ref, title, description, words, kind}. */
function docsAllPages() {
    if (!docsIndex) return [];
    const out = [];
    for (const g of docsIndex.guides || []) {
        out.push({ ref: `guides/${g.slug}`, title: g.title, kind: 'guide',
                   category: g.category, description: g.description, words: g.words,
                   added: g.added, updated: g.updated,
                   tags: Array.isArray(g.tags) ? g.tags : [] });
    }
    for (const m of docsIndex.api || []) {
        out.push({ ref: `api/${m.slug}`, title: m.title, kind: 'module',
                   category: m.category, description: m.description, words: m.words,
                   added: m.added, updated: m.updated });
    }
    for (const s of docsIndex.spices || []) {
        out.push({ ref: `spices/${s.slug}`, title: s.title, kind: 'spice',
                   category: s.category, description: s.description, words: s.words,
                   added: s.added, updated: s.updated });
    }
    return out;
}

/** Look up the manifest entry for a pack ref (no #fragment). */
export function docsEntryFor(ref) {
    return docsAllPages().find(p => p.ref === ref) || null;
}

/**
 * Split a `#doc=` value into its page ref and in-page anchor.
 * 'guides/hkt-guide#kinds' -> { ref: 'guides/hkt-guide', anchor: 'kinds' }
 */
export function parseDocsRef(value) {
    if (!value) return { ref: null, anchor: null };
    const hashAt = value.indexOf('#');
    if (hashAt === -1) return { ref: value, anchor: null };
    return { ref: value.slice(0, hashAt), anchor: value.slice(hashAt + 1) };
}

/** Group pack pages by category, preserving 'Other'/'core' last. */
function docsGroupByCategory(pages) {
    const groups = new Map();
    for (const p of pages) {
        const key = p.category || 'Other';
        if (!groups.has(key)) groups.set(key, []);
        groups.get(key).push(p);
    }
    const names = [...groups.keys()].sort((a, b) => {
        const rank = (n) => (n === 'core' ? -1 : n === 'Other' ? 1 : 0);
        return rank(a) - rank(b) || a.localeCompare(b);
    });
    return names.map(name => ({ name, pages: groups.get(name) }));
}

/**
 * Group pack pages by tag: one group per tag, alphabetical, with the
 * pseudo-tag 'other' last, holding every page that carries none. A page with
 * several tags is listed under each of them -- that is the point of a tag, and
 * what the website's By Tag cards do.
 */
const DOCS_OTHER_TAG = 'other';
function docsGroupByTag(pages) {
    const groups = new Map();
    for (const p of pages) {
        for (const t of (p.tags && p.tags.length ? p.tags : [DOCS_OTHER_TAG])) {
            if (!groups.has(t)) groups.set(t, []);
            groups.get(t).push(p);
        }
    }
    // 'other' last, whether it is the pseudo-tag or (unlikely) a real one.
    const names = [...groups.keys()].sort((a, b) =>
        (a === DOCS_OTHER_TAG) - (b === DOCS_OTHER_TAG) || a.localeCompare(b));
    return names.map(name => ({ name, pages: groups.get(name) }));
}

/**
 * The newest pages in the pack, newest first.
 *
 * Reads the `added` date the pack stamps on each page from git. A page git had
 * no add commit for carries no date and is skipped rather than sorted to one
 * end: "we do not know when this arrived" is not the same claim as "it is old",
 * and only one of the two belongs in a list of what is new.
 *
 * Deliberately not windowed to the last N days. A quiet month would empty the
 * section, and a nav that comes and goes is harder to learn than one whose top
 * entry is simply sometimes older -- the dates are on screen, so a reader can
 * see for themselves how recent "recent" currently is.
 */
function docsRecentPages(pages) {
    return pages
        .filter(p => p.added)
        .sort((a, b) => b.added.localeCompare(a.added)
                     || a.title.localeCompare(b.title))
        .slice(0, DOCS_RECENT_COUNT);
}

/**
 * The most recently edited pages in the pack, newest edit first.
 *
 * Reads `updated`, the day the page's source was last committed to -- the other
 * end of the same history `added` reads the near end of.
 *
 * A page whose last commit is the one that added it is left out: `added ===
 * updated` means nothing has happened to it since it arrived, which the
 * Recently Added section above already says. Without that filter every page
 * written this month would fill both lists, and the second one would stop
 * answering the question it is for -- "what changed under me?" is only
 * interesting about pages I might already have read.
 */
function docsUpdatedPages(pages) {
    return pages
        .filter(p => p.updated && p.updated !== p.added)
        .sort((a, b) => b.updated.localeCompare(a.updated)
                     || a.title.localeCompare(b.title))
        .slice(0, DOCS_RECENT_COUNT);
}

const DOCS_MONTHS = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun',
                     'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'];

/**
 * '2026-09-14' -> 'Sep 14'.
 *
 * Split rather than parsed through Date: `new Date('2026-09-14')` is UTC
 * midnight, which renders as the 13th for every reader west of Greenwich. The
 * date is a calendar day from a commit, not an instant, so it has no business
 * being shifted into anyone's timezone.
 */
function docsFormatDay(day) {
    const m = /^(\d{4})-(\d{2})-(\d{2})$/.exec(day || '');
    if (!m) return '';
    return `${DOCS_MONTHS[Number(m[2]) - 1] || ''} ${Number(m[3])}`.trim();
}

/**
 * A pinned link to the quickstart, above everything else in the nav.
 *
 * The one page a first-time reader wants, and the hardest to find by browsing:
 * it is filed under "Getting Started" like a dozen siblings, several rows down
 * a collapsed tree. Pinning it costs one line and spares that search. Rendered
 * only when the pack actually carries the page, so the nav never offers a link
 * it cannot open.
 */
function docsQuickstartSection() {
    const entry = docsEntryFor(DOCS_DEFAULT_REF);
    if (!entry) return '';
    return '<div class="docs-nav-section docs-nav-pinned"><ul>'
         + `<li><a href="#doc=${escapeAttr(DOCS_DEFAULT_REF)}"`
         + ` data-doc-ref="${escapeAttr(DOCS_DEFAULT_REF)}"`
         + ` title="${escapeAttr(entry.description || '')}">`
         + `${escapeHtml(entry.title)}</a></li>`
         + '</ul></div>';
}

/**
 * One dated, flat section -- "Recently Added" or "Recently Updated" -- of
 * guides.
 *
 * A `<details>`, closed on load. Both sections answer a "what changed since I
 * last looked?" question, which you ask occasionally and never on the way to a
 * page you already know the name of -- so ten rows of it sitting permanently
 * between the nav's top and the tree taxes every other visit to pay for that
 * one. Closed each is a single line, and markDocsNavActive() still springs the
 * right one open when the page you are reading is one of its entries.
 *
 * `field` names the date the section is about ('added' / 'updated'), and is
 * also the `data-` attribute each row carries, so a row says which claim it is
 * making rather than leaving the reader to infer it from its section.
 */
function docsDatedSection(cls, heading, field, pages) {
    let html = `<details class="docs-nav-section docs-nav-datelist ${cls}">`
             + `<summary><h4>${escapeHtml(heading)}</h4></summary><ul>`;
    for (const p of pages) {
        const day = p[field];
        const title = `${p.kind} -- ${field} ${day}`
                    + (p.description ? `\n${p.description}` : '');
        html += `<li><a href="#doc=${escapeAttr(p.ref)}" data-doc-ref="${escapeAttr(p.ref)}"`
             +  ` data-${field}="${escapeAttr(day)}" title="${escapeAttr(title)}">`
             +  `<span class="docs-recent-title">${escapeHtml(p.title)}</span>`
             +  `<span class="docs-recent-date">${escapeHtml(docsFormatDay(day))}</span>`
             +  '</a></li>';
    }
    return html + '</ul></details>';
}

/**
 * The Categories | Tags switch under the Guides heading.
 *
 * A tablist of two buttons, the in-pane twin of the website's sidebar switch.
 * Shown only when the pack carries tags (see docsNavSection's `switchHtml`), so
 * a pack built before guides were tagged keeps its plain category tree.
 */
function docsViewSwitch() {
    const btn = (view, text) =>
        `<button type="button" class="docs-view-btn${docsGuideView === view ? ' active' : ''}"`
      + ` role="tab" data-docs-view="${view}"`
      + ` aria-selected="${docsGuideView === view ? 'true' : 'false'}"`
      + ` tabindex="${docsGuideView === view ? 0 : -1}">${text}</button>`;
    return '<div class="docs-view-tablist" role="tablist" aria-label="Group guides by">'
         + btn('category', 'Categories') + btn('tag', 'Tags') + '</div>';
}

function docsNavSection(label, groups, switchHtml = '', countGroups = false) {
    let html = `<div class="docs-nav-section"><h4>${escapeHtml(label)}</h4>${switchHtml}`;
    for (const g of groups) {
        const count = countGroups
            ? ` <span class="docs-nav-count">${g.pages.length}</span>` : '';
        html += `<details class="docs-nav-group"><summary>${escapeHtml(g.name)}${count}</summary><ul>`;
        for (const p of g.pages) {
            html += `<li><a href="#doc=${escapeAttr(p.ref)}" data-doc-ref="${escapeAttr(p.ref)}"`
                 +  ` title="${escapeAttr(p.description || '')}">${escapeHtml(p.title)}</a></li>`;
        }
        html += '</ul></details>';
    }
    return html + '</div>';
}

/** Render the full nav tree (guides by category, API by group, spices). */
function renderDocsNav() {
    const nav = document.getElementById('docs-nav');
    if (!nav) return;
    if (!docsIndex) {
        nav.innerHTML = '<p class="docs-empty">Documentation pack not found. '
                      + 'Run <code>tur run docs</code> to build it.</p>';
        return;
    }
    const pages = docsAllPages();
    const guides = pages.filter(p => p.kind === 'guide');
    // The quickstart first, then what is new, then the tree.
    let html = docsQuickstartSection();
    // Above the tree, and flat: what changed cuts across guide categories, so
    // grouping it by category would bury the one thing it is for. Guides only
    // (see DOCS_RECENT_COUNT): the API tree below is where a module is found.
    // Added first, then updated -- arrival is the bigger news, and a page that
    // has only just landed is filtered out of the second list rather than
    // repeated in it. Either section is absent entirely when the pack carries no
    // dates for it, rather than shown empty.
    const recent = docsRecentPages(guides);
    if (recent.length) {
        html += docsDatedSection('docs-nav-recent', 'Recently Added', 'added', recent);
    }
    const updated = docsUpdatedPages(guides);
    if (updated.length) {
        html += docsDatedSection('docs-nav-updated', 'Recently Updated', 'updated', updated);
    }
    const modules = pages.filter(p => p.kind === 'module');
    const spices = pages.filter(p => p.kind === 'spice');
    if (guides.length) {
        // By category, or by tag when the reader has chosen that and the pack
        // has tags to group by. Without tags there is nothing to switch to, so
        // no switch is drawn and the category tree stands as it always has.
        const tagged = guides.some(g => g.tags && g.tags.length);
        const byTag = tagged && docsGuideView === 'tag';
        html += docsNavSection(
            'Guides',
            byTag ? docsGroupByTag(guides) : docsGroupByCategory(guides),
            tagged ? docsViewSwitch() : '', byTag);
    }
    if (modules.length) html += docsNavSection('API', docsGroupByCategory(modules));
    // Spices only appear when this build actually carries their pages, so the
    // nav never offers a link the pack cannot serve offline.
    if (spices.length) html += docsNavSection('Spices', docsGroupByCategory(spices));
    nav.innerHTML = html;
    markDocsNavActive();
}

/**
 * Choose how the Guides section is grouped, remember it, and redraw the nav.
 *
 * `refocus` is for the keyboard path: the redraw replaces the buttons, so the
 * one that was just activated is focused again on the new nav.
 */
function setDocsGuideView(view, refocus = false) {
    if (view !== 'category' && view !== 'tag') return;
    if (view === docsGuideView) return;
    docsGuideView = view;
    safeWrite(STORAGE_KEYS.docsView, view);
    renderDocsNav();
    if (refocus) {
        document.querySelector('#docs-nav .docs-view-btn.active')?.focus();
    }
}

/** Click and arrow-key handling for the Categories | Tags switch. */
function initDocsViewSwitch() {
    const nav = document.getElementById('docs-nav');
    if (!nav) return;
    nav.addEventListener('click', (e) => {
        const btn = e.target.closest('.docs-view-btn');
        if (btn) setDocsGuideView(btn.dataset.docsView);
    });
    // Arrow / Home / End move between the two tabs (WAI-ARIA tabs pattern).
    nav.addEventListener('keydown', (e) => {
        const btn = e.target.closest('.docs-view-btn');
        if (!btn) return;
        const views = ['category', 'tag'];
        const i = views.indexOf(btn.dataset.docsView);
        let j = -1;
        if (e.key === 'ArrowRight') j = (i + 1) % views.length;
        else if (e.key === 'ArrowLeft') j = (i - 1 + views.length) % views.length;
        else if (e.key === 'Home') j = 0;
        else if (e.key === 'End') j = views.length - 1;
        if (j < 0) return;
        e.preventDefault();
        setDocsGuideView(views[j], true);
    });
}

function markDocsNavActive() {
    const nav = document.getElementById('docs-nav');
    if (!nav) return;
    nav.querySelectorAll('a[data-doc-ref]').forEach(a => {
        const active = a.dataset.docRef === docsCurrentRef;
        a.classList.toggle('active', active);
        if (active) {
            a.closest('details')?.setAttribute('open', '');
            a.scrollIntoView({ block: 'nearest' });
        }
    });
}

/**
 * Render search results into the nav column.
 *
 * One box, two result kinds. Pages come from index.json's `words` blob; symbols
 * come from the doc-names.json the panel already uses, and selecting one routes
 * to the existing docstring panel rather than duplicating it here.
 */
function renderDocsSearch(query) {
    const nav = document.getElementById('docs-nav');
    if (!nav) return;
    const q = query.trim().toLowerCase();
    if (!q) { renderDocsNav(); return; }

    const pages = docsAllPages()
        .map(p => ({ p, score: docsScore(p, q) }))
        .filter(x => x.score > 0)
        .sort((a, b) => b.score - a.score)
        .slice(0, 40);

    const symbols = (docNames || [])
        .filter(d => d.name.toLowerCase().includes(q))
        .slice(0, 25);

    let html = '';
    if (pages.length) {
        html += '<div class="docs-nav-section"><h4>Pages</h4><ul class="docs-results">';
        for (const { p } of pages) {
            html += `<li><a href="#doc=${escapeAttr(p.ref)}" data-doc-ref="${escapeAttr(p.ref)}">`
                 +  `<span class="docs-result-title">${escapeHtml(p.title)}</span>`
                 +  `<span class="docs-result-kind">${escapeHtml(p.kind)}</span>`
                 +  `<span class="docs-result-desc">${escapeHtml(p.description || '')}</span>`
                 +  '</a></li>';
        }
        html += '</ul></div>';
    }
    if (symbols.length) {
        html += '<div class="docs-nav-section"><h4>Symbols</h4><ul class="docs-results">';
        for (const s of symbols) {
            const short = s.summary.replace(/^[\w/\-!?*+<>]+\s+--\s+/, '');
            html += `<li><a href="#" data-doc-symbol="${escapeAttr(s.name)}">`
                 +  `<span class="docs-result-title">${escapeHtml(s.name)}</span>`
                 +  `<span class="docs-result-kind">${escapeHtml(s.kind)}</span>`
                 +  `<span class="docs-result-desc">${escapeHtml(short)}</span>`
                 +  '</a></li>';
        }
        html += '</ul></div>';
    }
    nav.innerHTML = html || '<p class="docs-empty">No matches.</p>';
}

/** Rank a page against a query: title hits beat description hits beat body. */
function docsScore(page, q) {
    const title = (page.title || '').toLowerCase();
    if (title === q) return 100;
    if (title.includes(q)) return 50;
    if ((page.description || '').toLowerCase().includes(q)) return 20;
    if ((page.words || '').includes(q)) return 5;
    return 0;
}

/**
 * Fetch a pack fragment and render it into the article column.
 *
 * `ref` is a pack-relative page id, optionally with an in-page anchor:
 * 'guides/hkt-guide', 'api/tur-list#map', 'spices/json'.
 */
export async function showDocsPage(refWithAnchor, { updateHash = true } = {}) {
    const article = document.getElementById('docs-article');
    if (!article) return;
    const { ref, anchor } = parseDocsRef(refWithAnchor);
    if (!ref) return;

    // Leaving the page you are on: bank its offset before the fragment is
    // replaced, so navigating away and back returns you to your place.
    if (docsCurrentRef && docsCurrentRef !== ref) rememberDocsScroll();

    const entry = docsEntryFor(ref);
    if (!entry) {
        article.innerHTML = `<p class="docs-empty">No page <code>${escapeHtml(ref)}</code> `
                          + 'in this documentation pack.</p>';
        return;
    }

    article.setAttribute('aria-busy', 'true');
    try {
        const res = await fetch(`${DOCS_PACK_BASE}/${ref}.html`);
        if (!res.ok) throw new Error(`HTTP ${res.status}`);
        article.innerHTML = await res.text();
    } catch (err) {
        article.innerHTML = '<p class="docs-empty">This page is not available offline yet. '
                          + 'Reconnect once and it will be cached for good.</p>';
        console.warn('docs fragment fetch failed:', ref, err);
        article.removeAttribute('aria-busy');
        return;
    }
    article.removeAttribute('aria-busy');

    docsCurrentRef = ref;
    decorateDocsArticle(article);
    markDocsNavActive();
    if (updateHash) setHashParam('doc', refWithAnchor);
    persistDocsState();

    const siteLink = document.getElementById('docs-site-link');
    if (siteLink) siteLink.href = docsSiteUrl(ref);

    // Anchor scrolling has to wait for the fragment to be in the document --
    // and so does restoring an offset, since the element is not tall enough to
    // accept one until its content is there.
    //
    // Precedence: an explicit anchor always wins (you asked for that heading),
    // then a remembered offset for this page, then the top for a page opened
    // for the first time. A remembered offset can outlive the content it
    // pointed into if the pack updated between visits; the browser clamps it
    // to the scrollable range, which is the right behaviour, so it just does
    // not round-trip exactly.
    if (anchor) {
        const target = article.querySelector(`#${CSS.escape(anchor)}`);
        if (target) { target.scrollIntoView({ block: 'start' }); return; }
    }
    article.scrollTop = docsScrollByRef.get(ref) || 0;
}

/** The turmeric-lang.com URL for a pack ref, for the "open on the site" link. */
function docsSiteUrl(ref) {
    const [section, ...rest] = ref.split('/');
    const slug = rest.join('/');
    if (section === 'guides') return `/docs/html/guides/${slug}.html`;
    if (section === 'api') return `/docs/html/api/${slug}.html`;
    if (section === 'spices') return `/docs/html/spices/${slug}/`;
    return '/docs/html/guides/';
}

/**
 * Post-process a freshly rendered fragment: syntax highlighting, the
 * turmeric/sweet-exp toggles, and a "load into editor" affordance on every
 * runnable code block.
 *
 * The first three come from window.turmericGuide, which /docs-pack/guide.js
 * defines -- the same code the site's guide pages run, so highlighting,
 * toggles and diagrams behave identically in both places.
 */
function decorateDocsArticle(article) {
    if (window.turmericGuide) {
        window.turmericGuide.highlightGuideCode(article);
        window.turmericGuide.initSyntaxToggles(article);
        // Diagrams are lazy and network-backed; with the pane offline they
        // stay as their own source text, which renderMermaid handles itself.
        if (window.turmericGuide.renderMermaid) {
            window.turmericGuide.renderMermaid(article);
        }
    }
    article.querySelectorAll('pre').forEach(pre => {
        const code = pre.querySelector('code.language-turmeric, code.language-sweet-exp');
        if (!code || pre.querySelector('.docs-load-btn')) return;
        const btn = document.createElement('button');
        btn.type = 'button';
        btn.className = 'docs-load-btn';
        btn.textContent = 'Load into editor';
        btn.title = 'Replace the editor buffer with this snippet';
        btn.addEventListener('click', () => {
            loadSnippetIntoEditor(code.textContent);
            btn.textContent = 'Loaded';
            setTimeout(() => { btn.textContent = 'Load into editor'; }, 1400);
        });
        pre.appendChild(btn);
    });
}

/** Put a guide snippet in the editor and close the docs pane so it is visible. */
function loadSnippetIntoEditor(source) {
    const editor = getEditor();
    if (!editor) return;
    editor.setValue(source.replace(/\s+$/, '') + '\n');
    closeDocsPane();
    editor.focus();
    showStatus('Snippet loaded into the editor', 'info');
}

/**
 * Report offline readiness from the pack-status the service worker writes
 * during install (OD3). Says nothing when the pack is complete -- a working
 * guarantee does not need announcing; a partial one does.
 */
async function refreshDocsStatus() {
    const el = document.getElementById('docs-status');
    if (!el) return;
    el.textContent = '';
    el.className = 'docs-status';
    try {
        const res = await fetch('/docs-pack/pack-status');
        if (!res.ok) return;
        const status = await res.json();
        if (!status || typeof status.cached !== 'number') return;
        if (status.cached < status.expected) {
            el.textContent = `${status.cached} of ${status.expected} pages cached -- reconnect to finish`;
            el.classList.add('partial');
            // Ask the worker to top up now rather than waiting for a release.
            // An install is per-URL failure-tolerant, so a flaky first load can
            // leave the pack short; a guarantee that silently waits a version
            // bump to come true is not a guarantee.
            if (navigator.onLine) {
                navigator.serviceWorker?.controller?.postMessage('REPAIR_DOCS_PACK');
            }
        }
    } catch {
        // No service worker (dev server, or a browser with SW disabled). The
        // pane still works online; there is nothing useful to report.
    }
}

export function openDocsPane(refWithAnchor) {
    const overlay = document.getElementById('docs-overlay');
    if (!overlay) return;
    overlay.style.display = 'flex';
    document.body.classList.add('docs-open');

    // On a phone the nav column would cover the page it navigates to, so open
    // onto the article and leave the tree behind the toolbar toggle.
    if (window.matchMedia('(max-width: 768px)').matches && !docsCurrentRef) {
        docsNavCollapsed = true;
    }
    overlay.classList.toggle('nav-collapsed', docsNavCollapsed);

    // The symbol half of the pane's search comes from doc-names.json; make
    // sure it is on its way even if the WASM boot has not got there yet.
    fetchDocNames();

    loadDocsIndex().then(index => {
        const version = document.getElementById('docs-version');
        if (version) {
            version.textContent = index ? `Docs v${index.version}` : '';
        }
        renderDocsNav();
        // An explicit request wins, then wherever you were last, then the
        // quickstart. The pack's first guide is only a last resort now: it is
        // whatever sorted first, which is a fine fallback and a poor greeting.
        const target = refWithAnchor
            || docsCurrentRef
            || (docsEntryFor(DOCS_DEFAULT_REF) ? DOCS_DEFAULT_REF : null)
            || (index && index.guides && index.guides.length
                ? `guides/${index.guides[0].slug}`
                : null);
        if (target) showDocsPage(target);
        refreshDocsStatus();
        document.getElementById('docs-search')?.focus();
    });
}

export function closeDocsPane() {
    // Bank the offset while the overlay is still laid out. Reading scrollTop
    // after display:none would give 0, which is exactly the position the pane
    // is not supposed to come back to.
    rememberDocsScroll();
    const overlay = document.getElementById('docs-overlay');
    if (overlay) overlay.style.display = 'none';
    document.body.classList.remove('docs-open');
    setHashParam('doc', null);
    persistDocsState();
}

export function docsPaneIsOpen() {
    const overlay = document.getElementById('docs-overlay');
    return !!overlay && overlay.style.display !== 'none';
}

/**
 * Bank the pane's location so a later LAUNCH can come back to it.
 *
 * `#doc=` already carries this across a reload and drives back/forward, but a
 * hash does not survive a relaunch: an installed app cold-starts at the
 * manifest's start_url, which has no fragment. So a reader who closed the app
 * inside a guide reopened it on the editor, with nothing on screen saying
 * where they had been -- the pane remembers within a session and forgot across
 * the only boundary an installed app actually has.
 *
 * Null when the pane is closed, which is what makes this a memory of state
 * rather than a bookmark: closing the docs is how you say you are done with
 * them, and the next launch has to honour that.
 */
function persistDocsState() {
    if (!docsPaneIsOpen() || !docsCurrentRef) {
        safeWrite(STORAGE_KEYS.docs, null);
        return;
    }
    safeWrite(STORAGE_KEYS.docs, {
        ref: docsCurrentRef,
        scrollTop: docsScrollByRef.get(docsCurrentRef) || 0,
    });
}

/**
 * Reopen the pane where it was left, on a launch that arrived without a
 * `#doc=` of its own.
 *
 * The ref is checked against the pack before the pane opens. A guide that was
 * renamed or dropped between releases would otherwise restore as
 * "No page ... in this documentation pack", which is a worse greeting than the
 * editor; a stale entry clears itself instead. Nothing is restored when the
 * pack is missing entirely (a dev server that never ran `tur run docs`), since
 * there is no page to restore into.
 */
function restoreDocsPane() {
    const saved = safeRead(STORAGE_KEYS.docs);
    if (!saved || typeof saved.ref !== 'string') return;
    loadDocsIndex().then((index) => {
        if (!index) return;
        if (!docsEntryFor(saved.ref)) {
            safeWrite(STORAGE_KEYS.docs, null);
            return;
        }
        if (saved.scrollTop) docsScrollByRef.set(saved.ref, saved.scrollTop);
        openDocsPane(saved.ref);
    });
}

/**
 * Intercept clicks inside the pane.
 *
 * Three cases: a `#doc=` link is a pack navigation, a bare `#anchor` scrolls
 * within the current page, and anything else is an outbound link that opens in
 * a new tab so the REPL session is never replaced.
 */
function initDocsLinkInterception() {
    const overlay = document.getElementById('docs-overlay');
    if (!overlay) return;
    overlay.addEventListener('click', (e) => {
        const link = e.target.closest('a');
        if (!link) return;

        if (link.dataset.docSymbol) {
            e.preventDefault();
            const name = link.dataset.docSymbol;
            const entry = (docNames || []).find(d => d.name === name) || null;
            closeDocsPane();
            wasmDocLookup(name).then(text => showDocPanel(name, text, entry));
            return;
        }

        const href = link.getAttribute('href') || '';
        if (href.startsWith('#doc=')) {
            e.preventDefault();
            const raw = href.slice('#doc='.length);
            let ref = raw;
            try { ref = decodeURIComponent(raw); } catch { /* literal already */ }
            // On mobile the nav floats over the article, so picking a page
            // should get out of the way and show it.
            if (link.dataset.docRef
                && window.matchMedia('(max-width: 768px)').matches) {
                docsNavCollapsed = true;
                overlay.classList.add('nav-collapsed');
            }
            showDocsPage(ref);
            return;
        }
        if (href.startsWith('#')) {
            e.preventDefault();
            const id = href.slice(1);
            const article = document.getElementById('docs-article');
            const target = id && article ? article.querySelector(`#${CSS.escape(id)}`) : null;
            if (target) target.scrollIntoView({ block: 'start' });
            return;
        }
        if (href && !href.startsWith('javascript:')) {
            // Absolute or site-relative: leave /try/ intact.
            link.target = '_blank';
            link.rel = 'noopener';
        }
    });
}

export function initDocsPane() {
    initDocsViewSwitch();
    document.getElementById('docs-btn')?.addEventListener('click', () => openDocsPane());
    document.getElementById('docs-close')?.addEventListener('click', closeDocsPane);

    // showDocsPage and closeDocsPane already bank the offset on the two ways of
    // leaving a page, so this listener is a backstop rather than the mechanism:
    // it keeps the map current for any exit path that forgets to, at the cost
    // of one rAF-coalesced store per scroll burst.
    const article = document.getElementById('docs-article');
    if (article) {
        let pending = false;
        article.addEventListener('scroll', () => {
            if (pending) return;
            pending = true;
            requestAnimationFrame(() => { pending = false; rememberDocsScroll(); });
        }, { passive: true });
    }

    // The doc panel's "Open full docs" used to navigate to /docs/html/api/,
    // dropping the REPL session. It opens the pane now.
    document.getElementById('doc-full-link')?.addEventListener('click', (e) => {
        e.preventDefault();
        const href = e.currentTarget.getAttribute('href') || '';
        const ref = href.startsWith('#doc=') ? href.slice('#doc='.length) : null;
        openDocsPane(ref && docsEntryFor(ref) ? ref : undefined);
    });

    document.getElementById('docs-nav-toggle')?.addEventListener('click', () => {
        docsNavCollapsed = !docsNavCollapsed;
        document.getElementById('docs-overlay')
            ?.classList.toggle('nav-collapsed', docsNavCollapsed);
    });

    const search = document.getElementById('docs-search');
    if (search) {
        search.addEventListener('input', debounce(() => {
            renderDocsSearch(search.value);
        }, 120));
        search.addEventListener('keydown', (e) => {
            if (e.key === 'Escape') {
                if (search.value) { search.value = ''; renderDocsNav(); }
                else closeDocsPane();
            }
        });
    }

    // Click the backdrop (not the shell) to dismiss.
    document.getElementById('docs-overlay')?.addEventListener('mousedown', (e) => {
        if (e.target.id === 'docs-overlay') closeDocsPane();
    });

    document.addEventListener('keydown', (e) => {
        if (e.key === 'Escape' && docsPaneIsOpen()) closeDocsPane();
    });

    initDocsLinkInterception();

    // Going away is the last moment the offset is readable, and for an
    // installed app it is the ONLY one: it is backgrounded rather than
    // unloaded, and iOS may kill it later without running anything else.
    // `pagehide` covers the ordinary tab being closed or navigated away.
    const bankDocsState = () => { rememberDocsScroll(); persistDocsState(); };
    window.addEventListener('pagehide', bankDocsState);
    document.addEventListener('visibilitychange', () => {
        if (document.visibilityState === 'hidden') bankDocsState();
    });

    // Deep link: /try/#doc=guides/hkt-guide restores a docs location, and
    // composes with the existing #code= share hash rather than replacing it.
    //
    // The hash wins outright where it has a `doc` of its own -- following a
    // link is a request for that page, not for wherever you were last.
    //
    // The other two launches are suppressed for one reason between them: they
    // arrive pointed at something, and the pane would land on top of it. A
    // `#code=` share link is a request to look at a program; tutorial mode is
    // a lesson that owns the screen (and already opts out of persistence
    // wholesale, for the same reason). Neither clears the memory -- the next
    // ordinary launch still restores.
    const initial = getHashParam('doc');
    if (initial) openDocsPane(initial);
    else if (!hasUrlHashCode() && !isTutorialMode()) restoreDocsPane();
}

/** hashchange hook: a #doc= change opens or moves the pane. */
export function syncDocsPaneFromHash() {
    const ref = getHashParam('doc');
    if (!ref) {
        if (docsPaneIsOpen()) closeDocsPane();
        return;
    }
    if (docsPaneIsOpen()) showDocsPage(ref, { updateHash: false });
    else openDocsPane(ref);
}
