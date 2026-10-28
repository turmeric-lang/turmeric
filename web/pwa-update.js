// pwa-update.js -- the service worker from the page's side: register it (or
// tear it down on a dev server), apply an update once one has taken over, the
// overflow menu's build stamp, and Force update.

/* The service worker is a production feature, and on a dev server it is
 * actively harmful.
 *
 * sw.js falls through to cache-first for same-origin static assets, and its
 * precache names `/main.js` and `/styles.css` -- which on a dev server are the
 * files you are editing. So a worker installed by one `npm run dev` session
 * keeps serving that session's JS and CSS to every later one: the page comes up
 * unstyled, or running code you changed an hour ago, and the only way out is a
 * hard reload every single time.
 *
 * On a loopback host we therefore tear the worker down instead of registering
 * it. `?sw=1` opts back in, which is how the PWA and offline-docs specs still
 * exercise the real thing. */
const SW_LOOPBACK_HOSTS = new Set(['localhost', '127.0.0.1', '::1', '[::1]', '']);
const swDevDisabled = SW_LOOPBACK_HOSTS.has(location.hostname) &&
                      !new URLSearchParams(location.search).has('sw');

/* Was this page load already under a worker's control?
 *
 * The first visit is not: `activate` calls clients.claim(), which makes the
 * page controlled and fires `controllerchange` for a worker whose assets the
 * page is ALREADY running. Reloading on that is a pointless reload of a
 * perfectly current page. A later `controllerchange` on a load that started
 * controlled is the real signal -- a NEW worker took over, so what is on
 * screen came from the one it replaced. */
const swHadControllerAtLoad = 'serviceWorker' in navigator &&
                              !!navigator.serviceWorker.controller;
let swReloadArmed = false;

/**
 * Apply an activated update, once, at a moment that cannot lose work.
 *
 * Not immediately: an edit reaches localStorage through two chained 250ms
 * debounces, so re-navigating on the keystroke that happened to coincide with
 * an activation drops up to half a second of typing. Doing it when the app is
 * next backgrounded-then-foregrounded costs the reader nothing -- a resumed PWA
 * is exactly where they expect to find the new version anyway.
 */
function applySwUpdate() {
    if (swReloadArmed) return;
    swReloadArmed = true;
    const go = () => {
        document.removeEventListener('visibilitychange', onVisible);
        /* A navigation, not a reload -- the same reason forceUpdatePWA needs
         * one. The whole defect this exists to fix is that an iOS standalone
         * web app can answer location.reload() out of WebKit's own HTTP/page
         * cache; a routine update applied with reload() would land in exactly
         * that hole and the reader would again see nothing change. A URL with
         * no cache entry of its own cannot be answered that way.
         *
         * No loop: the load this starts is controlled by the worker that just
         * activated, so no further controllerchange fires until a genuinely
         * newer worker takes over. */
        hardNavigate(`${window.location.pathname}?u=${Date.now()}`);
    };
    const onVisible = () => { if (document.visibilityState === 'visible') go(); };
    if (document.visibilityState === 'hidden') { go(); return; }
    document.addEventListener('visibilitychange', onVisible);
}

if ('serviceWorker' in navigator && !swDevDisabled) {
    // Register after the page settles. Scope `/` so the origin-wide
    // kill-switch and runtime caching can target docs paths too.
    window.addEventListener('load', () => {
        navigator.serviceWorker.register('/sw.js', { scope: '/' })
            .then((reg) => {
                /* An installed PWA on iOS is the case this exists for. The
                 * browser checks for a new sw.js on a NAVIGATION in scope, and
                 * `register()` above is one -- but it only runs on `load`, and
                 * a standalone web app relaunched from the app switcher is
                 * RESUMED, not navigated. So the update check never ran, and
                 * closing and reopening the app -- the thing everyone tries
                 * first -- could not possibly help. Ask on every foreground. */
                const check = () => { reg.update().catch(() => {}); };
                document.addEventListener('visibilitychange', () => {
                    if (document.visibilityState === 'visible') check();
                });
                window.addEventListener('pageshow', (e) => { if (e.persisted) check(); });
            })
            .catch((err) => console.warn('SW registration failed:', err));
    });

    /* ... and once a new worker HAS taken over, apply it. sw.js ends `install`
     * with skipWaiting() and `activate` with clients.claim(), so a new worker
     * becomes this page's controller on its own -- but nothing re-rendered the
     * page, so the reader kept looking at the assets the OLD worker served and
     * the update was invisible until some later cold launch. On iOS that launch
     * may never come. */
    navigator.serviceWorker.addEventListener('controllerchange', () => {
        if (swHadControllerAtLoad) applySwUpdate();
    });
} else if ('serviceWorker' in navigator) {
    window.addEventListener('load', async () => {
        try {
            const regs = await navigator.serviceWorker.getRegistrations();
            // Nothing installed is the steady state, and returning here is what
            // makes the reload below safe: after one teardown there are no
            // registrations left, so the next load cannot reload again.
            if (!regs.length) return;

            const wasControlled = !!navigator.serviceWorker.controller;
            await Promise.all(regs.map((r) => r.unregister()));
            if ('caches' in window) {
                const keys = await caches.keys();
                await Promise.all(keys.map((k) => caches.delete(k)));
            }
            console.info('Service worker disabled on localhost (append ?sw=1 to keep it).');

            // This page load was served BY the worker just removed, so what is
            // on screen is the stale copy. Reload once to get the real files --
            // otherwise the fix appears not to have worked until a manual
            // reload, which is the thing being fixed.
            if (wasControlled) window.location.reload();
        } catch (err) {
            console.warn('SW teardown failed:', err);
        }
    });
}

/**
 * Name the build that is actually running, in the overflow menu.
 *
 * Read from the service worker's own cache key, which the build stamps as
 * `tur-try-v1-<version>-<short sha>`: that is the token the precache is keyed
 * on, so it names the build whose assets are on screen -- not the build the
 * page WANTS to be, which is what a compiled-in constant would report even
 * while a stale worker served everything around it.
 *
 * Silent when there is no worker (a plain browser tab with SW disabled, the dev
 * server): "no cached build" is the normal state there and not worth a row.
 */
export async function refreshBuildStamp() {
    const el = document.getElementById('build-stamp');
    if (!el) return;
    el.textContent = '';
    try {
        if (!('caches' in window) || !navigator.serviceWorker?.controller) return;
        const key = (await caches.keys()).find((k) => k.startsWith('tur-try-'));
        if (!key) return;
        // tur-try-v1-0.49.2-d32ec4ff7-precache -> 0.49.2-d32ec4ff7
        const build = key.replace(/^tur-try-v\d+-/, '').replace(/-(precache|runtime)$/, '');
        el.textContent = `Build ${build}`;
    } catch (_) {
        /* Cache Storage can throw outright in a private window or with site
         * data blocked. A missing stamp is not worth an error. */
    }
}

/**
 * Force-update the installed PWA: unregister every service worker, drop all
 * Cache Storage entries, then NAVIGATE (not reload) so the page and its assets
 * are fetched fresh from the network. This is the user-facing escape hatch for
 * a stuck cache -- it works even when the shipped sw.js forgot to bump
 * CACHE_VERSION, because it nukes the SW entirely so the following navigation
 * is uncontrolled and hits the network directly. localStorage (the editor tabs)
 * is intentionally left alone, so no code is lost.
 *
 * It used to end in location.reload(), which made it a placebo in an installed
 * iOS PWA for months: WebKit can answer a reload out of its own HTTP/page
 * cache, which neither the unregister nor the cache wipe touches, so the same
 * HTML came back naming the same hashed assets. It showed "Updating..." and
 * reloaded, so it looked like it had worked. See the navigation at the end.
 */
export async function forceUpdatePWA() {
    try {
        if ('serviceWorker' in navigator) {
            const regs = await navigator.serviceWorker.getRegistrations();
            await Promise.all(regs.map((r) => r.unregister()));
        }
        if ('caches' in window) {
            const keys = await caches.keys();
            await Promise.all(keys.map((k) => caches.delete(k)));
        }
    } catch (err) {
        console.warn('Force update failed:', err);
    }
    /* Navigate to a URL nothing has ever cached, rather than reload().
     *
     * The unregister and the cache wipe above are the easy half. The hard half
     * is getting a fetch to actually happen afterwards: in an iOS standalone
     * web app `location.reload()` can be answered from WebKit's own HTTP/page
     * cache, which the two lines above do not touch. The old HTML comes back,
     * it names the same hashed assets, and Force update appears to do nothing
     * -- which is exactly what it appeared to do.
     *
     * A navigation to a URL with no cache entry of its own cannot be answered
     * that way, and the service worker treats HTML navigations as network-first
     * anyway, so the query parameter costs nothing when a worker is present.
     * `replace`, not `assign`, so the cache-busted URL does not become a
     * history entry the reader can go back to. */
    hardNavigate(`${window.location.pathname}?u=${Date.now()}`);
}

/**
 * Navigate to `url`, replacing the current history entry.
 *
 * The one indirection exists for the specs: current Chromium makes
 * `location.replace` non-configurable, so a test cannot stub it -- it installs
 * `window.__turiNavigate` and observes the intent instead of navigating away.
 */
function hardNavigate(url) {
    if (typeof window.__turiNavigate === 'function') { window.__turiNavigate(url); return; }
    window.location.replace(url);
}
