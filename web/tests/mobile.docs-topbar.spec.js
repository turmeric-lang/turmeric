// The docs topbar's Contents toggle, on a phone.
//
// The button is `.btn .btn-icon .btn-sm`, whose shared rule is
// `display: inline-flex; align-items: center` -- vertical centering, and
// nothing about the main axis. That is invisible everywhere the button is
// sized by its own content, which is every desktop use of it. The phone rule
// gives it `min-width: 40px` so a thumb can land on it, and 40px is wider than
// a 16px icon inside 8px padding inside a 1px border: the leftover 6px all
// went to the right of the glyph under the default `justify-content:
// flex-start`, sitting the icon 3px left of the box it is drawn in.
//
// Measured rather than asserted against the rule text: a CSS declaration can
// be present and still not apply (the installed-app shell shipped green twice
// on a rule that never matched), so these read getBoundingClientRect and
// compare centers.

import { test, expect } from '@playwright/test';

async function openTry(page) {
    await page.goto('/try/');
    await page.waitForFunction(() => !!window.turmericApp, null, { timeout: 30_000 });
}

/**
 * Open the docs pane and let the shell's entry animation finish.
 *
 * `.docs-shell` animates in with `transform: translateY(12px)`, so a rect read
 * on the frame after openDocsPane() is offset -- and every assertion here is
 * about exact geometry.
 */
async function openDocs(page) {
    await page.evaluate(() => window.turmericApp.openDocsPane());
    await page.evaluate(async () => {
        const shell = document.querySelector('.docs-shell');
        await Promise.all(shell.getAnimations().map(a => a.finished.catch(() => {})));
        await new Promise(requestAnimationFrame);
    });
}

test.describe('docs topbar Contents toggle', () => {
    test('the hamburger icon is centered in its button', async ({ page }) => {
        await openTry(page);
        await openDocs(page);

        const geom = await page.evaluate(() => {
            const btn = document.getElementById('docs-nav-toggle');
            const icon = btn.querySelector('svg');
            const b = btn.getBoundingClientRect();
            const i = icon.getBoundingClientRect();
            return {
                btnCenterX: b.left + b.width / 2,
                btnCenterY: b.top + b.height / 2,
                iconCenterX: i.left + i.width / 2,
                iconCenterY: i.top + i.height / 2,
                btnWidth: b.width,
                btnHeight: b.height,
            };
        });

        // The tap target the phone rule exists to create.
        expect(geom.btnWidth).toBeGreaterThanOrEqual(40);
        expect(geom.btnHeight).toBeGreaterThanOrEqual(40);

        // Sub-pixel tolerance only: 0.5px absorbs a fractional layout, and the
        // bug this pins was a whole 3px.
        expect(Math.abs(geom.iconCenterX - geom.btnCenterX)).toBeLessThanOrEqual(0.5);
        expect(Math.abs(geom.iconCenterY - geom.btnCenterY)).toBeLessThanOrEqual(0.5);
    });
});
