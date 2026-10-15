import { test, expect } from '@playwright/test';

/**
 * The light/dark switch on a phone. Below 1080px the nav's GitHub/Try-it
 * cluster drops out and below 768px the links do; the toggle has to survive
 * both and stay pinned to the right edge, opposite the hamburger -- it is the
 * only way to change theme on a phone. See theme-toggle.spec.js for the
 * behaviour itself.
 */

for (const path of ['/', '/try/']) {
    test(`${path} keeps the theme toggle on screen at the right edge`, async ({ page }) => {
        await page.goto(path);
        const toggle = page.locator('site-nav [data-theme-toggle]');
        await expect(toggle).toBeVisible();

        const box = await toggle.boundingBox();
        const width = page.viewportSize().width;
        expect(box.x + box.width).toBeLessThanOrEqual(width);
        // Pinned right: nothing but the nav's own gutter beyond it.
        expect(width - (box.x + box.width)).toBeLessThanOrEqual(24);
        // A comfortable tap target.
        expect(box.width).toBeGreaterThanOrEqual(32);
        expect(box.height).toBeGreaterThanOrEqual(32);

        const before = await page.evaluate(() => document.documentElement.getAttribute('data-theme'));
        await toggle.tap();
        const after = await page.evaluate(() => document.documentElement.getAttribute('data-theme'));
        expect(after).not.toBe(before);
    });
}
