/* The service worker is disabled on loopback hosts.
 *
 * sw.js is cache-first for same-origin static assets and its precache names
 * /main.js and /styles.css -- the very files a dev server exists to re-serve.
 * A worker left installed by one `npm run dev` session hands the next one
 * yesterday's JS and CSS, which shows up as an unstyled page running code you
 * already changed, curable only by a hard reload every time.
 */

import { test, expect } from '@playwright/test';

async function registrations(page) {
    return page.evaluate(async () => {
        const regs = await navigator.serviceWorker.getRegistrations();
        return regs.length;
    });
}

/**
 * registrations() for expect.poll: -1 while a navigation has the page between
 * documents. The teardown under test ends by reloading the page, so a poll that
 * lands mid-reload is expected and must be asked again, not fail the test --
 * expect.poll does not retry a callback that throws.
 */
async function polledRegistrations(page) {
    try {
        return await registrations(page);
    } catch (err) {
        if (/Execution context was destroyed|navigat/i.test(String(err))) return -1;
        throw err;
    }
}

test.describe('service worker on localhost', () => {

    test('is not registered by a plain visit', async ({ page }) => {
        await page.goto('/try/');
        // Registration is deferred to `load`, so give it the chance it would
        // have had -- asserting immediately would pass for the wrong reason.
        await page.waitForLoadState('load');
        await page.waitForTimeout(1000);
        expect(await registrations(page)).toBe(0);
    });

    test('?sw=1 opts back in, and a plain visit then tears it down', async ({ page }) => {
        await page.goto('/try/?sw=1');
        // expect.poll, not waitForFunction: waitForFunction never awaits its
        // predicate, so an `async` one returns a Promise -- truthy -- and the
        // wait passed on its first poll whatever the registration count was.
        await expect.poll(() => polledRegistrations(page), { timeout: 30_000 }).toBeGreaterThan(0);
        await page.waitForFunction(() => !!navigator.serviceWorker.controller,
                                   null, { timeout: 30_000 });

        // Now the situation the report described: a dev server visit with a
        // worker already installed. It should clean up after itself.
        await page.goto('/try/');
        await expect.poll(() => polledRegistrations(page), { timeout: 30_000 }).toBe(0);

        // And having torn it down, the page reloads once so what is on screen
        // is the real files rather than the copies the removed worker served.
        await expect(page.locator('#wasm-status-text')).toHaveText(/Ready/i,
                                                                  { timeout: 45_000 });
        expect(await page.evaluate(() => !!navigator.serviceWorker.controller)).toBe(false);
    });

    test('the teardown does not loop', async ({ page }) => {
        // The early return on "nothing installed" is what bounds this: a second
        // plain visit has nothing to remove, so it must not reload again.
        await page.goto('/try/');
        await page.waitForLoadState('load');
        const marked = await page.evaluate(() => {
            window.__swLoopProbe = true;
            return true;
        });
        expect(marked).toBe(true);
        await page.waitForTimeout(2000);
        // A reload would have wiped the marker off the window.
        expect(await page.evaluate(() => window.__swLoopProbe === true)).toBe(true);
    });
});
