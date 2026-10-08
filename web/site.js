// site.js — Shared JS for turmeric-lang.com marketing pages (/, /tour)
// Load this with <script type="module" src="/site.js"> in <head>; the module
// is deferred by default, so it runs after the DOM is parsed.

import Prism from 'prismjs';
import './icons.js'; // registers the <t-icon> custom element (Lucide set)
import { GITHUB_URL } from './repo.js';

// ── TURMERIC SYNTAX GRAMMAR ─────────────────────────────────────────────────

Prism.languages.turmeric = {
  comment:     /;.*/,
  string:      /"(?:[^"\\]|\\.)*"/,
  keyword: {
    pattern: /\b(?:defn|defmacro|defstruct|defclass|definstance|defdata|defgadt|defeffect|defpackage|let|let\*|letrec|if|cond|when|match|fn|do|begin|and|or|not|handle|perform|resume)\b/,
    greedy: false,
  },
  type:        /:[a-zA-Z][a-zA-Z0-9_\-?!]*/,
  number:      /\b\d[\d._]*\b/,
  punctuation: /[()[\]{}]/,
};

// ── SYNTAX HIGHLIGHTING ─────────────────────────────────────────────────────

// Highlight all .code-version and .step-code elements that contain plain code.
// site.js runs after DOM parsing (module = implicit defer), so elements are
// available immediately without waiting for DOMContentLoaded.
document.querySelectorAll('.code-version, .step-code').forEach(el => {
  const raw = el.textContent;
  el.innerHTML = Prism.highlight(raw, Prism.languages.turmeric, 'turmeric');
});

// ── COPY BUTTONS ────────────────────────────────────────────────────────────

const SVG_COPY  = '<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><rect x="9" y="9" width="13" height="13" rx="2"/><path d="M5 15H4a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v1"/></svg>';
const SVG_CHECK = '<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><polyline points="20 6 9 17 4 12"/></svg>';

function makeCopyBtn(text) {
  const btn = document.createElement('button');
  btn.className = 'copy-btn';
  btn.setAttribute('aria-label', 'Copy to clipboard');
  btn.title = 'Copy';
  btn.innerHTML = SVG_COPY;
  btn.addEventListener('click', () => {
    navigator.clipboard.writeText(text.trim()).then(() => {
      btn.innerHTML = SVG_CHECK;
      btn.classList.add('copied');
      btn.setAttribute('aria-label', 'Copied!');
      setTimeout(() => {
        btn.innerHTML = SVG_COPY;
        btn.classList.remove('copied');
        btn.setAttribute('aria-label', 'Copy to clipboard');
      }, 2000);
    });
  });
  return btn;
}

// Inline copy button after .install-cmd (fits into the flex row naturally)
document.querySelectorAll('.install-cmd').forEach(el => {
  el.insertAdjacentElement('afterend', makeCopyBtn(el.textContent));
});

// Absolutely-positioned copy button overlaid top-right on each .step-code block
document.querySelectorAll('.step-code').forEach(el => {
  el.appendChild(makeCopyBtn(el.textContent));
});

// ── WEB COMPONENTS ─────────────────────────────────────────────────────────

// Native `title` tooltips for the site chrome (nav, sidebar, footer). Keyed by
// href so the three lists cannot drift apart in what they claim a page is; a
// link whose href is absent simply gets no tooltip.
const LINK_TITLES = {
  '/':                                    'Turmeric home',
  '/tour':                                'A guided tour of the language in fourteen stops',
  '/try':                                 'Run Turmeric in your browser -- nothing to install',
  '/trowel':                              'Trowel -- the native Turmeric editor for macOS and Linux',
  '/docs/html/guides/':                   'Guides and tutorials, from quickstart to compiler internals',
  '/docs/html/api/':                      'Generated API reference for the standard library',
  '/docs/html/spices/':                   'Browse Spice packages -- the Turmeric package registry',
  '/roadmap':                             'Planned features, work in progress, and recent milestones',
  '/ci':                                  'Build and test metrics from continuous integration',
  'https://spices.turmeric-lang.com':     'Browse Spice packages -- the Turmeric package registry',
  'https://c.turmeric-lang.com':          'A C interpreter running in your browser',
  [GITHUB_URL]:                           'Turmeric source code on GitHub',
  'https://phasor.space':                 "Roger Jungemann's site",
};

// The canonical topbar and sidebar. `tools/genguides.py` carries the same two
// lists as NAV_LINKS / SIDEBAR_GROUPS and every generated page (guides, API
// docs, spices) is built from them, so a link added here has to be added there
// too -- otherwise half the site disagrees with the other half about what is on
// it. The mobile drawer renders SIDEBAR_GROUPS as well, so a phone sees the
// same site map a desktop sidebar shows.
const NAV_LINKS = [
  ['/tour',                            'Tour'],
  ['/try',                             'Try It'],
  ['/docs/html/guides/',               'Guides'],
  ['/docs/html/api/',                  'API Docs'],
  ['https://spices.turmeric-lang.com', 'Spices'],
  ['/trowel',                          'Trowel'],
];

const SIDEBAR_GROUPS = [
  ['Language', [
    ['/tour',   'Tour'],
    ['/trowel', 'Trowel'],
    ['/try',    'Try It'],
  ]],
  ['Ecosystem', [
    ['/docs/html/guides/',               'Guides'],
    ['/docs/html/api/',                  'API Docs'],
    ['https://spices.turmeric-lang.com', 'Spices'],
    ['https://c.turmeric-lang.com',      'C Interpreter'],
  ]],
  ['Community', [
    [GITHUB_URL,                               'GitHub'],
    ['/ci',                                    'CI Metrics'],
  ]],
];

function sidebarGroupsHTML() {
  return SIDEBAR_GROUPS.map(([heading, links]) =>
    `<h3>${heading}</h3><ul>` +
    links.map(([href, label]) => `<li><a href="${href}">${label}</a></li>`).join('') +
    '</ul>'
  ).join('');
}

function applyLinkTitles(root) {
  root.querySelectorAll('a[href]').forEach(a => {
    if (a.title) return;
    const href = a.getAttribute('href');
    if (LINK_TITLES[href]) a.title = LINK_TITLES[href];
    else if (href.startsWith('#')) a.title = `Jump to ${a.textContent.trim()}`;
  });
}

class SiteNav extends HTMLElement {
  connectedCallback() {
    const active = this.getAttribute('active') ?? '';

    // On /try itself, every route to /try is a link to the page you are already
    // on -- both the nav entry and the gold CTA. Drop them rather than render a
    // self-link (the other `active` pages keep their entry as a you-are-here
    // highlight; only /try has a duplicate CTA that would be left dangling).
    const onTry = active === 'Try It';

    const linkHTML = NAV_LINKS
      .filter(([href]) => !(onTry && href === '/try'))
      .map(([href, label]) =>
        `<a href="${href}"${active === label ? ' class="active"' : ''}>${label}</a>`
      ).join('');

    const ctaHTML = onTry ? '' : '<a href="/try" class="btn-gold">Try it</a>';

    // The mobile panel is the fallback drawer for the pages that have no
    // <site-sidebar> (/try, /ci). It lists the same groups the sidebar does, so
    // the drawer shows the same site map either way; on pages that do have a
    // sidebar the hamburger opens that instead and this panel stays hidden.
    this.innerHTML = `
      <nav>
        <button class="nav-hamburger" aria-label="Toggle navigation" aria-expanded="false">
          <span></span><span></span><span></span>
        </button>
        <a class="nav-logo" href="/">
          <img src="/logo-icon.svg" class="nav-logo-icon" width="28" height="28" alt="">
          <img src="/logo.svg" class="nav-logo-wordmark" width="101" height="28" alt="Turmeric">
        </a>
        <div class="nav-links">${linkHTML}</div>
        <div class="nav-right">
          <a href="${GITHUB_URL}" class="btn-ghost">GitHub</a>
          ${ctaHTML}
        </div>
        <div class="nav-mobile-panel">
          <a class="nav-mobile-back" href="/">Home</a>
          ${sidebarGroupsHTML()}
        </div>
      </nav>`;

    applyLinkTitles(this);

    const nav   = this.querySelector('nav');
    const btn   = this.querySelector('.nav-hamburger');
    const panel = this.querySelector('.nav-mobile-panel');
    const sidebar = document.querySelector('site-sidebar');

    let overlay = document.querySelector('.sidebar-overlay');
    if (sidebar && !overlay) {
      overlay = document.createElement('div');
      overlay.className = 'sidebar-overlay';
      document.body.appendChild(overlay);
    }

    // A <site-sidebar> already carries the page's own contents plus the same
    // global groups, so it is the better drawer where one exists -- but only
    // where it is actually rendered. The home page hides its rail on desktop
    // and shows it as the drawer on mobile, so this is asked on every open
    // rather than once at startup: a check made at load time would answer for
    // the wrong viewport as soon as the window was resized.
    const drawerIsSidebar = () =>
      !!sidebar && getComputedStyle(sidebar).display !== 'none';

    const setOpen = (open) => {
      const useDrawer = open && drawerIsSidebar();
      nav.classList.toggle('nav-open', open);
      // Suppresses the fallback panel while the sidebar is the drawer, so the
      // two never stack on top of each other.
      nav.classList.toggle('nav-drawer', useDrawer);
      sidebar?.classList.toggle('is-open', useDrawer);
      overlay?.classList.toggle('is-open', useDrawer);
      btn.setAttribute('aria-expanded', String(open));
    };

    btn.addEventListener('click', () => setOpen(!nav.classList.contains('nav-open')));
    overlay?.addEventListener('click', () => setOpen(false));
    document.addEventListener('keydown', e => {
      if (e.key === 'Escape') setOpen(false);
    });
    document.addEventListener('click', e => {
      if (nav.classList.contains('nav-open') &&
          !this.contains(e.target) && !sidebar?.contains(e.target)) {
        setOpen(false);
      }
    });
    panel.addEventListener('click', e => { if (e.target.closest('a')) setOpen(false); });
    sidebar?.addEventListener('click', e => { if (e.target.closest('a')) setOpen(false); });
  }
}

class SiteFooter extends HTMLElement {
  connectedCallback() {
    this.innerHTML = `
      <footer>
        <div class="footer-inner">
          <div class="footer-brand footer-col">
            <a href="/" class="nav-logo" style="display:inline-flex;gap:10px;align-items:center;">
              <img src="/logo-icon.svg" class="nav-logo-icon" width="28" height="28" alt="">
              <img src="/logo.svg" class="nav-logo-wordmark" width="101" height="28" alt="Turmeric">
            </a>
            <p>A lightning-fast functional language with typeclasses, effects, and a
               Lisp-flavored syntax. Open source under the MIT license.</p>
          </div>
          <!-- Built from SIDEBAR_GROUPS, not hand-listed: the footer and the
               sidebar name the same three groups, and two lists that name the
               same thing must name the same members or the site disagrees with
               itself about what the ecosystem contains. Labels are bare noun
               phrases ("C Interpreter", not "Try our C Interpreter") -- the
               verb reads as noise in a link column. -->
          ${SIDEBAR_GROUPS.map(([heading, links]) => `
          <div class="footer-col">
            <div class="footer-col-title">${heading}</div>
            ${links.map(([href, label]) => `<a href="${href}">${label}</a>`).join('')}
          </div>`).join('')}
        </div>
        <div class="footer-bottom">
          <span class="footer-copy">© 2025 The Turmeric Project and
            <a href="https://phasor.space">Roger Jungemann</a>. MIT License.</span>
          <div class="footer-links"></div>
        </div>
      </footer>`;

    applyLinkTitles(this);
  }
}

class SiteSidebar extends HTMLElement {
  connectedCallback() {
    const back = this.getAttribute('back') ?? '/';
    const backLabel = this.getAttribute('back-label') ?? 'Home';
    const tocHtml = this.innerHTML.trim();
    // Same shape as the generated pages build in `genguides.build_sidebar`:
    // Home link, then this page's own contents, then one divider, then the
    // global groups. The divider is the only rule in the rail, and it always
    // sits immediately above those groups.
    this.innerHTML = `
      <a class="sidebar-back" href="${back}">${backLabel}</a>
      ${tocHtml ? `<div class="sidebar-toc">${tocHtml}</div>` : ''}
      <hr class="sidebar-divider">
      ${sidebarGroupsHTML()}`;

    applyLinkTitles(this);
  }
}

customElements.define('site-nav', SiteNav);
customElements.define('site-footer', SiteFooter);
customElements.define('site-sidebar', SiteSidebar);

// ── REVEAL ON SCROLL ────────────────────────────────────────────────────────

const revealTargets = Array.from(document.querySelectorAll('.reveal'));
let revealPending = revealTargets.length;

const revealObs = new IntersectionObserver(entries => {
  entries.forEach(e => {
    if (e.isIntersecting) {
      e.target.classList.add('visible');
      revealObs.unobserve(e.target);
      revealPending--;
      if (revealPending <= 0) revealObs.disconnect();
    }
  });
}, { threshold: 0.08, rootMargin: '0px 0px -40px 0px' });

revealTargets.forEach((el, i) => {
  el.style.transitionDelay = (i === 0 ? 0.1 : 0) + 's';
  revealObs.observe(el);
});

// ── INSTALL METHOD TABS ────────────────────────────────────────────────────

// Homepage "01 -- Install" step: one tablist switching between install methods.
document.querySelectorAll('.install-tabs').forEach(tabs => {
  const card    = tabs.closest('.step-card');
  const buttons = Array.from(tabs.querySelectorAll('.install-tab'));

  const select = (method) => {
    buttons.forEach(btn => {
      const active = btn.dataset.method === method;
      btn.classList.toggle('active', active);
      btn.setAttribute('aria-selected', active ? 'true' : 'false');
    });
    card?.querySelectorAll('.install-panel').forEach(panel => {
      panel.hidden = panel.dataset.method !== method;
    });
  };

  tabs.addEventListener('click', (e) => {
    const btn = e.target.closest('.install-tab');
    if (btn) select(btn.dataset.method);
  });

  tabs.addEventListener('keydown', (e) => {
    const idx = buttons.indexOf(document.activeElement);
    if (idx === -1) return;
    if (e.key === 'ArrowRight') { buttons[(idx + 1) % buttons.length].focus(); e.preventDefault(); }
    if (e.key === 'ArrowLeft')  { buttons[(idx - 1 + buttons.length) % buttons.length].focus(); e.preventDefault(); }
  });
});

// ── SYNTAX TOGGLE ──────────────────────────────────────────────────────────

const SYNTAX_PREF_KEY = 'tur-syntax-pref';
const DEFAULT_SYNTAX  = 'sweet-exp';

function applySyntaxToToggle(toggle, syntax) {
  const card = toggle.closest('.code-card');
  toggle.querySelectorAll('.seg-btn').forEach(btn => {
    const active = btn.dataset.syntax === syntax;
    btn.classList.toggle('active', active);
    btn.setAttribute('aria-selected', active ? 'true' : 'false');
  });
  const filenameEl = card?.querySelector('.code-card-filename');
  if (filenameEl) {
    const key = syntax.replace(/-([a-z])/g, (_, c) => c.toUpperCase());
    if (filenameEl.dataset[key]) filenameEl.textContent = filenameEl.dataset[key];
  }
  card?.querySelectorAll('.code-version').forEach(v => {
    v.style.display = v.classList.contains(syntax + '-version') ? '' : 'none';
  });
}

// Apply stored preference (or default) to every toggle on the page
const storedSyntax = localStorage.getItem(SYNTAX_PREF_KEY) ?? DEFAULT_SYNTAX;
document.querySelectorAll('.code-syntax-toggle').forEach(toggle => {
  // Only apply if the toggle actually has a button for this syntax
  if (toggle.querySelector(`[data-syntax="${storedSyntax}"]`)) {
    applySyntaxToToggle(toggle, storedSyntax);
  }
});

// Supports multiple independent toggles per page
document.querySelectorAll('.code-syntax-toggle').forEach(toggle => {
  toggle.addEventListener('click', (e) => {
    if (!e.target.classList.contains('seg-btn')) return;
    const syntax = e.target.dataset.syntax;
    applySyntaxToToggle(toggle, syntax);
    localStorage.setItem(SYNTAX_PREF_KEY, syntax);
  });

  // Arrow-key navigation within the tablist
  toggle.addEventListener('keydown', (e) => {
    const btns = Array.from(toggle.querySelectorAll('.seg-btn'));
    const idx = btns.indexOf(document.activeElement);
    if (idx === -1) return;
    if (e.key === 'ArrowRight') { btns[(idx + 1) % btns.length].focus(); e.preventDefault(); }
    if (e.key === 'ArrowLeft')  { btns[(idx - 1 + btns.length) % btns.length].focus(); e.preventDefault(); }
  });
});
