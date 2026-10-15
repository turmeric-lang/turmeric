// theme.js -- the site's light/dark switch, applied BEFORE first paint.
//
// Loaded as a classic, parser-blocking <script src> at the top of every page's
// <head> (the hand-written pages in web/ and the ones tools/gendocs.py,
// genguides.py and genspices.py generate). It has to run before the first
// stylesheet paints anything: the light palette is keyed on
// `<html data-theme="light">`, and a page that painted dark and then flipped
// would flash on every navigation. It is external, like pwa-shell.js, because
// the site's Content-Security-Policy (web/csp.js) allows no inline script.
//
// The choice:
//   - Until the reader picks one, the page follows the OS
//     (prefers-color-scheme), live -- flipping the OS setting flips an open
//     page.
//   - A click on any [data-theme-toggle] button stores the opposite of what is
//     showing under localStorage `tur-theme`, and that wins from then on, on
//     every page of the site and in every open tab.
//   - With no script at all the page is dark: `:root` carries the dark
//     tokens, and only [data-theme="light"] overrides them.
//
// What it exposes, for code that paints with something CSS cannot reach (the
// Monaco editor, which takes its colors as a JS theme object):
//   window.turTheme.get()         'light' | 'dark'
//   window.turTheme.set(theme)    store and apply
//   window.turTheme.toggle()
//   window.turTheme.sync()        relabel [data-theme-toggle] buttons rendered
//                                 after this ran (site.js's <site-nav>)
//   'tur-themechange' event on document, detail { theme }, after each change.
(function () {
  var KEY = 'tur-theme';
  // The browser-chrome tint for <meta name="theme-color">: each theme's
  // --bg-base, so the mobile address bar runs into the page.
  var THEME_COLOR = { dark: '#0C0A08', light: '#FBF8F2' };
  var root = document.documentElement;

  var mql = null;
  try { mql = window.matchMedia && window.matchMedia('(prefers-color-scheme: light)'); }
  catch (e) { /* no matchMedia: dark */ }

  function stored() {
    try {
      var v = window.localStorage.getItem(KEY);
      return v === 'light' || v === 'dark' ? v : null;
    } catch (e) {
      return null;   // storage blocked (private mode, sandboxed frame)
    }
  }

  function system() { return mql && mql.matches ? 'light' : 'dark'; }

  function current() { return root.getAttribute('data-theme') === 'light' ? 'light' : 'dark'; }

  function sync() {
    var theme = current();
    var label = 'Switch to ' + (theme === 'light' ? 'dark' : 'light') + ' theme';
    var buttons = document.querySelectorAll('[data-theme-toggle]');
    for (var i = 0; i < buttons.length; i++) {
      buttons[i].setAttribute('aria-label', label);
      buttons[i].setAttribute('title', label);
    }
    var metas = document.querySelectorAll('meta[name="theme-color"]');
    for (var j = 0; j < metas.length; j++) metas[j].setAttribute('content', THEME_COLOR[theme]);
  }

  function apply(theme, notify) {
    if (root.getAttribute('data-theme') === theme) { sync(); return; }
    // Without this every element with a `transition: color/background` fades
    // on its own clock, and the page repaints as a ripple instead of at once.
    if (notify) root.classList.add('theme-switching');
    root.setAttribute('data-theme', theme);
    sync();
    if (!notify) return;
    if (window.requestAnimationFrame) {
      window.requestAnimationFrame(function () {
        window.requestAnimationFrame(function () { root.classList.remove('theme-switching'); });
      });
    } else {
      root.classList.remove('theme-switching');
    }
    var ev;
    try { ev = new CustomEvent('tur-themechange', { detail: { theme: theme } }); }
    catch (e) { ev = document.createEvent('CustomEvent'); ev.initCustomEvent('tur-themechange', false, false, { theme: theme }); }
    document.dispatchEvent(ev);
  }

  function set(theme) {
    if (theme !== 'light' && theme !== 'dark') return;
    try { window.localStorage.setItem(KEY, theme); } catch (e) { /* this page only */ }
    apply(theme, true);
  }

  function toggle() { set(current() === 'light' ? 'dark' : 'light'); }

  try {
    apply(stored() || system(), false);

    if (mql) {
      var onSystem = function () { if (!stored()) apply(system(), true); };
      if (mql.addEventListener) mql.addEventListener('change', onSystem);
      else if (mql.addListener) mql.addListener(onSystem);
    }

    // Another tab of the site toggled: follow it.
    window.addEventListener('storage', function (e) {
      if (e.key === KEY) apply(stored() || system(), true);
    });

    // Delegated, so a button rendered later -- by <site-nav>, or into a
    // generated page's header -- needs no wiring of its own.
    document.addEventListener('click', function (e) {
      var t = e.target;
      var btn = t && t.closest ? t.closest('[data-theme-toggle]') : null;
      if (btn) { e.preventDefault(); toggle(); }
    });

    document.addEventListener('DOMContentLoaded', sync);

    window.turTheme = { get: current, set: set, toggle: toggle, sync: sync };
  } catch (e) { /* the dark page is better than a blank one */ }
})();
