// Safe-area insets in the installed PWA.
//
// Two reports, one root cause. On a notched phone in standalone the status bar
// is drawn over the page (apple-mobile-web-app-status-bar-style is
// black-translucent, and the viewport is viewport-fit=cover), so the top
// ~59 CSS px of the viewport belongs to the clock and the battery indicator.
// #app reserved that strip. Every `position: fixed` overlay did not -- fixed
// positioning resolves against the viewport, so it escapes the padding the
// shell reserved -- and the docs pane's 44px topbar holds every control that
// leaves the pane: close, Contents, search. The whole row sat inside the inset
// and a reader who opened a doc had no way back out.
//
// These specs drive the inset through the --safe-* tokens rather than env(),
// which is the reason the tokens exist: env() cannot be set from script, and
// Chromium will not report (display-mode: standalone) under automation, so a
// token is the only way to put a notched phone's geometry on the page. The
// numbers below are an iPhone 15/16 in portrait.

import { test, expect } from '@playwright/test';

const SAFE_TOP = 59;
const SAFE_BOTTOM = 34;

async function openTry(page) {
    await page.goto('/try/');
    await page.waitForFunction(() => !!window.turmericApp, null, { timeout: 30_000 });
}

/** Stand in for a notched phone in standalone. */
async function applyInsets(page, { top = SAFE_TOP, bottom = SAFE_BOTTOM } = {}) {
    await page.evaluate(({ t, b }) => {
        document.documentElement.style.setProperty('--safe-top', `${t}px`);
        document.documentElement.style.setProperty('--safe-bottom', `${b}px`);
    }, { t: top, b: bottom });
    // One frame for the new box sizes to land.
    await page.evaluate(() => new Promise(requestAnimationFrame));
}

/**
 * Open the docs pane and wait for it to settle.
 *
 * .docs-shell animates in with `transform: translateY(12px)`, so a rect read
 * on the frame after openDocsPane() is 12px off and every assertion here is
 * about exact geometry. Waiting on the animation rather than on a sleep keeps
 * that from turning into a flake.
 */
async function openDocs(page) {
    await page.evaluate(() => window.turmericApp.openDocsPane());
    await page.evaluate(async () => {
        const shell = document.querySelector('.docs-shell');
        await Promise.all(shell.getAnimations().map(a => a.finished.catch(() => {})));
        await new Promise(requestAnimationFrame);
    });
}

const boxOf = (page, sel) => page.evaluate((s) => {
    const el = document.querySelector(s);
    if (!el) return null;
    const b = el.getBoundingClientRect();
    return { top: b.top, bottom: b.bottom, height: b.height, width: b.width };
}, sel);

test.describe('PWA safe-area insets', () => {
    test('the docs pane exit control clears the status bar inset', async ({ page }) => {
        await openTry(page);
        await applyInsets(page);
        await openDocs(page);

        const close = await boxOf(page, '#docs-close');
        expect(close).not.toBeNull();
        // The bug: the whole 44px topbar lived at y 0-44, inside a 59px inset.
        expect(close.top).toBeGreaterThanOrEqual(SAFE_TOP);
        // And it has to be hittable once it is on screen.
        expect(close.height).toBeGreaterThanOrEqual(44);

        // The other control in the bar went under the clock with it.
        const toggle = await boxOf(page, '#docs-nav-toggle');
        expect(toggle.top).toBeGreaterThanOrEqual(SAFE_TOP);

        // The bar's own background still paints up through the inset, so the
        // status bar sits on chrome rather than on the article text.
        const bar = await boxOf(page, '.docs-topbar');
        expect(bar.top).toBe(0);
        expect(bar.bottom).toBeGreaterThan(SAFE_TOP);
    });

    test('the docs footer reserves the home-indicator inset', async ({ page }) => {
        await openTry(page);
        await applyInsets(page);
        await openDocs(page);

        const footer = await boxOf(page, '.docs-footer');
        const viewportHeight = await page.evaluate(() => window.innerHeight);
        // Painted to the very edge -- no black band ...
        expect(Math.round(footer.bottom)).toBe(viewportHeight);
        // ... but tall enough that its content clears the home indicator.
        expect(footer.height).toBeGreaterThanOrEqual(30 + SAFE_BOTTOM);
    });

    test('a phone with no insets is laid out exactly as before', async ({ page }) => {
        await openTry(page);
        await applyInsets(page, { top: 0, bottom: 0 });
        await openDocs(page);

        // The padding is inset-driven, not unconditional: a device that
        // reports no insets (and every desktop browser) must not grow a gap.
        const bar = await boxOf(page, '.docs-topbar');
        expect(bar.top).toBe(0);
        expect(Math.round(bar.height)).toBe(48);

        const footer = await boxOf(page, '.docs-footer');
        expect(Math.round(footer.height)).toBe(30);
    });

    test('the phone sheet offers a labeled Back control that closes the pane',
         async ({ page }) => {
        await openTry(page);
        await applyInsets(page);
        await openDocs(page);
        await expect(page.locator('#docs-overlay')).toBeVisible();

        // "x" in a trailing corner does not read as the way out of a
        // full-screen sheet; on a phone the control is labeled and leading.
        const label = page.locator('.docs-close-label');
        await expect(label).toBeVisible();
        await expect(label).toHaveText('Back');

        const closeBox = await boxOf(page, '#docs-close');
        const toggleBox = await boxOf(page, '#docs-nav-toggle');
        expect(closeBox.top).toBeLessThan(toggleBox.top + toggleBox.height);
        // Leading edge: before the Contents toggle, not after the search box.
        const closeLeft = await page.evaluate(
            () => document.querySelector('#docs-close').getBoundingClientRect().left);
        const toggleLeft = await page.evaluate(
            () => document.querySelector('#docs-nav-toggle').getBoundingClientRect().left);
        expect(closeLeft).toBeLessThan(toggleLeft);

        await page.click('#docs-close');
        await expect(page.locator('#docs-overlay')).toBeHidden();
    });

    test('a centered overlay keeps its content clear of the insets', async ({ page }) => {
        await openTry(page);
        await applyInsets(page);

        // The docs pane is a full-bleed sheet and handles the insets on its
        // bars; the tutorial and loading overlays are centered dialogs and are
        // covered by .overlay's own padding. Force one open with content tall
        // enough to want the whole screen -- a flex item centered in a box it
        // overflows loses its top edge off-screen, which is the docs-topbar
        // failure one level in.
        const top = await page.evaluate(async (t) => {
            const overlay = document.getElementById('tutorial-overlay');
            overlay.style.display = 'flex';
            const content = overlay.querySelector('.overlay-content');
            const filler = document.createElement('div');
            filler.style.height = '2000px';
            content.appendChild(filler);
            await Promise.all(content.getAnimations().map(a => a.finished.catch(() => {})));
            await new Promise(requestAnimationFrame);
            const box = content.getBoundingClientRect();
            filler.remove();
            overlay.style.display = 'none';
            return { top: box.top, bottom: box.bottom, viewport: window.innerHeight, t };
        }, SAFE_TOP);

        expect(top.top).toBeGreaterThanOrEqual(SAFE_TOP);
        expect(top.bottom).toBeLessThanOrEqual(top.viewport - SAFE_BOTTOM);
    });

    test('the installed shell is pinned to the viewport, not sized from 100dvh',
         async ({ page }) => {
        await openTry(page);

        // Put the page in the installed-app shell the way the device does and
        // MEASURE it. The previous version of this test read the rule text out
        // of the CSSOM, which is why the band shipped twice: the assertion was
        // that a rule with `position: fixed` EXISTS, never that it applies. It
        // stayed green while the rule was dead on iOS, where the app reports
        // (display-mode: fullscreen) and the standalone-only query it was
        // written under never matched. The shell is gated on html.pwa now, so
        // there is a real switch to flip here.
        await page.evaluate(() => document.documentElement.classList.add('pwa'));
        await applyInsets(page);

        const shell = await page.evaluate(() => {
            const app = document.getElementById('app');
            const rect = app.getBoundingClientRect();
            const style = getComputedStyle(app);
            return {
                position: style.position,
                paddingTop: parseFloat(style.paddingTop),
                top: rect.top,
                bottom: rect.bottom,
                viewport: window.innerHeight,
            };
        });

        expect(shell.position).toBe('fixed');
        // The whole point: no unpainted strip between the shell and the screen
        // bottom. `height: 100dvh` on iOS lands short by the two insets, which
        // is the black band the reports were about.
        expect(shell.top).toBe(0);
        expect(shell.bottom).toBeCloseTo(shell.viewport, 0);
        // And the status bar's strip is still reserved at the top.
        expect(shell.paddingTop).toBeCloseTo(SAFE_TOP, 0);
    });

    test('the shell is NOT pinned in an ordinary browser tab', async ({ page }) => {
        // The gate has to stay a gate. Pinning #app in a normal tab would take
        // the site nav off the page for every mobile visitor, not just the
        // installed ones.
        await openTry(page);
        await applyInsets(page);

        const position = await page.evaluate(() =>
            getComputedStyle(document.getElementById('app')).position);
        expect(position).not.toBe('fixed');
    });
});
