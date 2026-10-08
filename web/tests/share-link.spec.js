// Share links round-trip.
//
// The Share button and `#code=` links went through `pako.gzip` / `pako.ungzip`
// for as long as they existed, and nothing ever loaded pako: every share said
// "Failed to encode code", the hash never updated, and a `#code=` link left the
// editor as it was (docs/archive/try-share-links-never-encode.md). Nothing
// failed loudly enough to notice, because both functions swallowed the
// ReferenceError and every caller reads '' as "nothing to share".
//
// So these tests assert the round trip end to end -- Share, open what it
// copied in a fresh page, compare the buffer -- rather than that some code
// path runs.
//
// A working link puts someone else's text into the editor. The program shared
// here opens with a `#lang` line that tries to break out of the language
// picker's attributes (WP6, W-2); loading it must not run anything.

import { test, expect } from '@playwright/test';

const HOSTILE_FIRST_LINE = '#lang x"onfocus="window.__pwned=1"autofocus="';
const PROGRAM = `${HOSTILE_FIRST_LINE}\n(println "shared é中")\n(+ 1 2)`;

async function openTry(page, hash = '') {
    await page.goto(`/try/${hash}`);
    await page.waitForFunction(() => !!window.turmericApp && !!window._turiEditor,
        null, { timeout: 30_000 });
}

/** Click Share and return the URL it produced (clipboard, or the prompt fallback). */
async function share(page) {
    let prompted = null;
    page.once('dialog', async (dialog) => {
        prompted = dialog.defaultValue();
        await dialog.dismiss();
    });
    await page.evaluate(() => navigator.clipboard.writeText(''));
    await page.click('#share-btn');
    // Encoding is async: wait for one of the two places the link can land.
    await expect.poll(async () => prompted
        || await page.evaluate(() => navigator.clipboard.readText()), { timeout: 10_000 })
        .toContain('#code=');
    await expect(page.locator('#wasm-status-text')).not.toHaveText('Failed to encode code');
    return prompted || page.evaluate(() => navigator.clipboard.readText());
}

test.describe('share links', () => {
    test.beforeEach(async ({ context }) => {
        await context.grantPermissions(['clipboard-read', 'clipboard-write']);
    });

    test('Share copies a link that reopens the same buffer', async ({ page, browser }) => {
        await openTry(page);
        await page.evaluate((c) => window._turiEditor.setValue(c), PROGRAM);

        const url = await share(page);
        expect(url).toMatch(/#code=[A-Za-z0-9_-]+$/);

        // A NEW context: the persisted tabs live in this one's localStorage and
        // would restore the buffer whether or not the link carried it.
        const fresh = await browser.newContext();
        const other = await fresh.newPage();
        await openTry(other, url.slice(url.indexOf('#')));
        await other.waitForFunction(
            (want) => window._turiEditor.getValue() === want, PROGRAM, { timeout: 30_000 });
        expect(await other.evaluate(() => window.__pwned)).toBeUndefined();
        await fresh.close();
    });

    test('editing writes the buffer into the hash', async ({ page, browser }) => {
        await openTry(page);
        await page.evaluate(() => window._turiEditor.setValue('(println "from the hash")'));
        // updateUrlHash is debounced by a second after the last edit.
        await page.waitForFunction(() => /(^|[#&])code=[A-Za-z0-9_-]+/.test(location.hash),
            null, { timeout: 10_000 });
        const hash = await page.evaluate(() => location.hash);

        const fresh = await browser.newContext();
        const other = await fresh.newPage();
        await openTry(other, hash);
        await other.waitForFunction(
            () => window._turiEditor.getValue() === '(println "from the hash")',
            null, { timeout: 30_000 });
        await fresh.close();
    });

    test('a mangled #code= link leaves the editor alone', async ({ page }) => {
        await openTry(page);
        const before = await page.evaluate(() => window._turiEditor.getValue());
        await page.evaluate(() => { location.hash = 'code=not-a-real-payload'; });
        await page.waitForTimeout(500);
        expect(await page.evaluate(() => window._turiEditor.getValue())).toBe(before);
    });
});
