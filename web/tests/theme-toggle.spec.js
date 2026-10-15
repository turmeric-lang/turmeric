import { test, expect } from '@playwright/test';

/**
 * The light/dark switch (public/theme.js, the [data-theme-toggle] button in
 * <site-nav>, and the :root[data-theme="light"] token blocks).
 *
 * What it promises, and what each test pins:
 *   - Until the reader chooses, the page follows the OS -- including a change
 *     to the OS setting while the page is open.
 *   - A click stores the choice, and the stored choice beats the OS on every
 *     page from then on.
 *   - The theme is on <html> before anything paints. That is a property of
 *     WHERE the script sits (a classic blocking <script> ahead of every
 *     stylesheet), so it is checked as markup on each page: a module or
 *     deferred copy would still pass every behavioural test here and flash
 *     dark-then-light on every navigation.
 *   - The Monaco editor, which takes its colors from JS rather than CSS,
 *     follows the switch live.
 */

const KEY = 'tur-theme';
const LIGHT_BASE = 'rgb(251, 248, 242)';   // --bg-base, light
const DARK_BASE = 'rgb(12, 10, 8)';        // --bg-base, dark

const PAGES = ['/', '/tour/', '/trowel/', '/ci/', '/try/'];

const theme = (page) => page.evaluate(() => document.documentElement.getAttribute('data-theme'));
const bodyBg = (page) => page.evaluate(() => getComputedStyle(document.body).backgroundColor);

test.describe('theme switch', () => {
    for (const path of PAGES) {
        test(`${path} sets the theme from a blocking head script, before any stylesheet`, async ({ page }) => {
            await page.goto(path);
            const order = await page.evaluate(() => {
                const head = Array.from(document.head.children);
                const script = head.findIndex(el => el.tagName === 'SCRIPT' && el.getAttribute('src') === '/theme.js');
                const firstSheet = head.findIndex(el => el.tagName === 'LINK' && el.rel === 'stylesheet');
                const el = head[script];
                return {
                    script, firstSheet,
                    blocking: !!el && !el.async && !el.defer && !el.type,
                };
            });
            expect(order.script, 'theme.js is in <head>').toBeGreaterThanOrEqual(0);
            expect(order.blocking, 'theme.js is a classic, parser-blocking script').toBe(true);
            if (order.firstSheet >= 0) expect(order.script).toBeLessThan(order.firstSheet);
        });
    }

    test('follows the OS until the reader chooses', async ({ page }) => {
        await page.emulateMedia({ colorScheme: 'light' });
        await page.goto('/');
        expect(await theme(page)).toBe('light');
        expect(await bodyBg(page)).toBe(LIGHT_BASE);

        // A change to the OS setting reaches an open page.
        await page.emulateMedia({ colorScheme: 'dark' });
        await expect.poll(() => theme(page)).toBe('dark');
        expect(await bodyBg(page)).toBe(DARK_BASE);
    });

    test('a click stores the choice, which then beats the OS on every page', async ({ page }) => {
        await page.emulateMedia({ colorScheme: 'dark' });
        await page.goto('/');
        const toggle = page.locator('site-nav [data-theme-toggle]');
        await expect(toggle).toBeVisible();
        await expect(toggle).toHaveAttribute('aria-label', 'Switch to light theme');

        await toggle.click();
        expect(await theme(page)).toBe('light');
        await expect(toggle).toHaveAttribute('aria-label', 'Switch to dark theme');
        expect(await page.evaluate(k => localStorage.getItem(k), KEY)).toBe('light');

        // Another page, OS still dark: the stored choice wins.
        await page.goto('/tour/');
        expect(await theme(page)).toBe('light');
        expect(await bodyBg(page)).toBe(LIGHT_BASE);

        // And an OS change no longer moves it.
        await page.emulateMedia({ colorScheme: 'light' });
        await page.emulateMedia({ colorScheme: 'dark' });
        expect(await theme(page)).toBe('light');
    });

    test('a stored choice applies on first paint, with no flip after load', async ({ page }) => {
        await page.emulateMedia({ colorScheme: 'light' });
        await page.addInitScript(k => localStorage.setItem(k, 'dark'), KEY);
        // Record every value <html data-theme> takes, from the very first one.
        // Observed on the document, not on documentElement: when an init
        // script runs the parser has not created the real <html> yet.
        await page.addInitScript(() => {
            window.__themes = [];
            new MutationObserver(records => {
                for (const r of records) {
                    if (r.target === document.documentElement) window.__themes.push(r.target.getAttribute('data-theme'));
                }
            }).observe(document, { attributes: true, subtree: true, attributeFilter: ['data-theme'] });
        });
        await page.goto('/');
        expect(await page.evaluate(() => window.__themes)).toEqual(['dark']);
    });

    test('the Try editor follows the switch live', async ({ page }) => {
        await page.emulateMedia({ colorScheme: 'dark' });
        await page.goto('/try/');
        const editor = page.locator('#editor .monaco-editor').first();
        await expect(editor).toBeVisible({ timeout: 30_000 });
        const editorBg = () => editor.evaluate(el => getComputedStyle(el).backgroundColor);
        expect(await editorBg()).toBe(DARK_BASE);

        await page.locator('site-nav [data-theme-toggle]').click();
        await expect.poll(editorBg).toBe(LIGHT_BASE);

        await page.locator('site-nav [data-theme-toggle]').click();
        await expect.poll(editorBg).toBe(DARK_BASE);
    });
});
