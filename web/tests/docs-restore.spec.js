// The docs pane remembers whether it was open, across LAUNCHES.
//
// `#doc=` already carries the pane's location across a reload, and drives
// back/forward. It does not survive a relaunch: an installed app cold-starts
// at the manifest's start_url, which has no fragment, so a reader who closed
// the app inside a guide came back to the editor with nothing on screen saying
// where they had been.
//
// Every test here therefore LEAVES the origin and comes back, rather than
// reloading: a reload keeps the hash, which would take the deep-link branch
// and test nothing. Leaving and returning is what an app launch looks like.
//
// The docs pack is stubbed. It is a build artefact (`tur run docs`, gitignored),
// so a checkout does not have one -- and these tests are about the pane's
// memory, not about the pack's contents.

import { test, expect } from '@playwright/test';

const INDEX = {
    version: '0.0.0-test',
    generated: '2026-01-01T00:00:00Z',
    guides: [
        { slug: 'alpha', path: 'guides/alpha.html', title: 'Alpha guide',
          category: 'core', description: 'first', words: 'alpha' },
        { slug: 'beta', path: 'guides/beta.html', title: 'Beta guide',
          category: 'core', description: 'second', words: 'beta' },
    ],
    api: [],
    spices: [],
    files: ['guides/alpha.html', 'guides/beta.html'],
};

/** A fragment tall enough to have somewhere to scroll to. */
const fragment = (name) =>
    `<h1>${name}</h1>` + '<p style="height: 300px">body</p>'.repeat(12);

async function stubPack(page) {
    await page.route('**/docs-pack/index.json', (route) =>
        route.fulfill({ contentType: 'application/json', body: JSON.stringify(INDEX) }));
    await page.route('**/docs-pack/guides/*.html', (route) => {
        const name = route.request().url().split('/').pop().replace('.html', '');
        return route.fulfill({ contentType: 'text/html', body: fragment(name) });
    });
}

async function openTry(page) {
    await page.goto('/try/');
    await page.waitForFunction(() => !!window.turmericApp, null, { timeout: 30_000 });
}

/** Leave the origin and come back with no fragment -- an app launch. */
async function relaunch(page) {
    await page.goto('/');
    await openTry(page);
}

const paneState = (page) => page.evaluate(() => {
    const overlay = document.getElementById('docs-overlay');
    const article = document.getElementById('docs-article');
    return {
        open: !!overlay && overlay.style.display !== 'none',
        heading: article?.querySelector('h1')?.textContent || null,
        scrollTop: article ? article.scrollTop : null,
        hash: window.location.hash,
        stored: JSON.parse(localStorage.getItem('tur.try.docs.v1') || 'null'),
    };
});

test.describe('docs pane across launches', () => {
    test.beforeEach(async ({ page }) => { await stubPack(page); });

    test('reopens the guide it was left on, at the place it was left', async ({ page }) => {
        await openTry(page);
        await page.evaluate(() => window.turmericApp.openDocsPane('guides/beta'));
        await page.waitForFunction(
            () => document.querySelector('#docs-article h1')?.textContent === 'beta');

        // `behavior: 'instant'` for the same reason docsSetScroll uses it:
        // `.docs-article` sets `scroll-behavior: smooth`, so a plain
        // `scrollTop =` assignment ANIMATES, and the rAF-coalesced handler
        // banks wherever the animation happens to be one frame in (34px, on
        // the run that caught this).
        await page.evaluate(() => {
            document.getElementById('docs-article')
                .scrollTo({ top: 400, behavior: 'instant' });
        });
        await page.waitForFunction(
            () => document.getElementById('docs-article').scrollTop === 400);
        // The offset is banked from a rAF-coalesced scroll handler.
        await page.evaluate(() => new Promise(requestAnimationFrame));

        await relaunch(page);
        await page.waitForFunction(
            () => document.getElementById('docs-overlay')?.style.display === 'flex',
            null, { timeout: 10_000 });
        await page.waitForFunction(
            () => document.querySelector('#docs-article h1')?.textContent === 'beta');

        const after = await paneState(page);
        expect(after.open).toBe(true);
        expect(after.heading).toBe('beta');
        // Restored, not merely reopened at the top.
        expect(after.scrollTop).toBe(400);
        // And the restore re-establishes the deep link, so sharing the page
        // you came back to works the same as sharing one you navigated to.
        expect(after.hash).toContain('doc=guides/beta');
    });

    test('a pane closed before leaving stays closed', async ({ page }) => {
        await openTry(page);
        await page.evaluate(() => window.turmericApp.openDocsPane('guides/alpha'));
        await page.waitForFunction(
            () => document.querySelector('#docs-article h1')?.textContent === 'alpha');
        await page.evaluate(() => window.turmericApp.closeDocsPane());

        await relaunch(page);
        // Nothing to wait on -- assert the absence after the pane's own boot
        // path has had a turn.
        await page.waitForTimeout(500);

        const after = await paneState(page);
        expect(after.open).toBe(false);
        expect(after.stored).toBeNull();
    });

    test('a saved page the pack no longer has is dropped, not restored as an error',
        async ({ page }) => {
            await page.addInitScript(() => {
                localStorage.setItem('tur.try.docs.v1',
                    JSON.stringify({ ref: 'guides/renamed-away', scrollTop: 0 }));
            });
            await openTry(page);
            await page.waitForTimeout(500);

            const after = await paneState(page);
            expect(after.open).toBe(false);
            // Cleared, so it is not re-checked on every launch from here on.
            expect(after.stored).toBeNull();
        });

    test('a tutorial is not buried under a restored pane', async ({ page }) => {
        await openTry(page);
        await page.evaluate(() => window.turmericApp.openDocsPane('guides/beta'));
        await page.waitForFunction(
            () => document.querySelector('#docs-article h1')?.textContent === 'beta');

        await page.goto('/');
        await page.goto('/try/?tutorial');
        await page.waitForFunction(() => !!window.turmericApp, null, { timeout: 30_000 });
        await page.waitForTimeout(500);

        const after = await paneState(page);
        expect(after.open).toBe(false);
        expect(after.stored?.ref).toBe('guides/beta');
    });

    test('a #code= share link is not buried under a restored pane', async ({ page }) => {
        await openTry(page);
        await page.evaluate(() => window.turmericApp.openDocsPane('guides/beta'));
        await page.waitForFunction(
            () => document.querySelector('#docs-article h1')?.textContent === 'beta');

        await page.goto('/');
        await page.goto('/try/#code=not-a-real-payload');
        await page.waitForFunction(() => !!window.turmericApp, null, { timeout: 30_000 });
        await page.waitForTimeout(500);

        const after = await paneState(page);
        expect(after.open).toBe(false);
        // The memory survives: the next ordinary launch still restores it.
        expect(after.stored?.ref).toBe('guides/beta');
    });
});
