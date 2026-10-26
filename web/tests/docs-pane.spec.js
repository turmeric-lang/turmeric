// In-app docs browser (OD2).
//
// The pane renders the docs pack -- chrome-free fragments emitted by the same
// generators that build turmeric-lang.com/docs/html/ -- over the REPL. The
// contract these tests pin is that reading docs is not a navigation: the
// editor buffer and console survive browsing, cross-links resolve inside the
// pane, and a #doc= deep link restores a location.
//
// Offline behaviour is a separate concern and lives in docs-offline.spec.js.

import { test, expect } from '@playwright/test';
import { existsSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const WEB_DIR = path.dirname(path.dirname(fileURLToPath(import.meta.url)));

// The pack is a `tur run docs` output and is gitignored, so a fresh clone has
// none. Without this the whole file fails on an empty nav, which reads as a
// pane bug rather than a missing build step.
test.beforeAll(() => {
    test.skip(!existsSync(path.join(WEB_DIR, 'public', 'docs-pack', 'index.json')),
              'no docs pack -- run `tur run docs` first');
});

/**
 * Open /try/ and wait for the editor.
 *
 * Deliberately does NOT wait for `#wasm-status-text` to read Ready: the docs
 * pane must not depend on the WASM boot, and pinning that here is what keeps
 * it that way.
 */
async function openTry(page, hash = '') {
    await page.goto(`/try/${hash}`);
    await page.waitForFunction(() => !!window.turmericApp && !!window._turiEditor,
                               null, { timeout: 30_000 });
}

async function openDocs(page) {
    await page.click('#docs-btn');
    await page.waitForSelector('#docs-nav .docs-nav-section', { timeout: 15_000 });
}

/** Navigate the pane by ref, bypassing the collapsed-by-default nav groups. */
async function gotoDoc(page, ref) {
    await page.evaluate((r) => window.turmericApp.showDocsPage(r), ref);
    await page.waitForFunction(
        (r) => window.turmericApp.getState().docsRef === r, ref, { timeout: 15_000 });
}

/** Current scroll offset of the article column. */
async function articleScrollTop(page) {
    return page.evaluate(() => document.getElementById('docs-article').scrollTop);
}

/**
 * Scroll the article column and return where it actually landed.
 *
 * Returns the real offset rather than the requested one: a short fragment
 * clamps, and a test that assumed its request took effect would pass by
 * comparing 0 to 0.
 */
async function scrollArticle(page, top) {
    await page.evaluate((t) => {
        document.getElementById('docs-article').scrollTop = t;
    }, top);
    await settleArticle(page);
    return articleScrollTop(page);
}

/**
 * Wait until the article column has finished rendering and positioning itself.
 *
 * Reading scrollTop any earlier is what makes a scroll-restore test pass
 * against code that does not restore anything: showDocsPage re-fetches, and
 * until that resolves the column is still showing -- and still scrolled to --
 * the previous render. A poll that accepts the first matching sample latches
 * onto that stale value and reports success. So: wait for the fetch to clear
 * aria-busy, then let the frame in which showDocsPage assigns scrollTop go by,
 * and only then take a single reading.
 */
async function settleArticle(page) {
    await page.waitForFunction(() => {
        const a = document.getElementById('docs-article');
        return a && !a.hasAttribute('aria-busy');
    }, null, { timeout: 15_000 });
    await page.waitForTimeout(400);
}

/** Assert the article settled within a few pixels of `want`. */
async function expectArticleAt(page, want) {
    await settleArticle(page);
    expect(Math.abs(await articleScrollTop(page) - want)).toBeLessThan(40);
}

test.describe('docs pane', () => {
    test('opens from the toolbar and lists guides and API modules', async ({ page }) => {
        await openTry(page);
        await openDocs(page);

        const sections = await page.locator('#docs-nav .docs-nav-section h4').allTextContents();
        expect(sections).toContain('Guides');
        expect(sections).toContain('API');

        // The version stamp answers "are my offline docs stale?" at a glance.
        await expect(page.locator('#docs-version')).toContainText(/^Docs v\d+\.\d+\.\d+$/);

        // A page is showing, rendered from a fragment (no site chrome inside).
        await expect(page.locator('#docs-article h1')).toHaveCount(1);
        await expect(page.locator('#docs-article .site-header')).toHaveCount(0);
    });

    test('opens on the quickstart, and pins it at the top of the nav', async ({ page }) => {
        await openTry(page);
        await openDocs(page);

        // Nothing in the URL and nothing visited this session: the pane picks
        // the beginning, not whatever guide sorts first in the pack.
        await page.waitForFunction(
            () => window.turmericApp.getState().docsRef === 'guides/quickstart',
            null, { timeout: 15_000 });

        // And the same page is one click away from anywhere in the nav.
        const pinned = page.locator('#docs-nav .docs-nav-pinned a[data-doc-ref]');
        await expect(pinned).toHaveCount(1);
        await expect(pinned).toHaveAttribute('data-doc-ref', 'guides/quickstart');
        await expect(pinned).toBeVisible();
    });

    test('recently added leads the nav, newest first', async ({ page }) => {
        await openTry(page);
        await openDocs(page);

        // The pack stamps `added` from git; a pack built outside a checkout
        // carries none, and the section is then absent by design rather than
        // empty. Skip instead of failing -- that is a build input, not a bug.
        const dated = await page.evaluate(async () => {
            const idx = await (await fetch('/docs-pack/index.json')).json();
            return (idx.guides || []).filter(e => e.added).length;
        });
        test.skip(dated === 0, 'pack carries no add dates (built outside a git checkout)');

        const sections = await page.locator('#docs-nav .docs-nav-section h4').allTextContents();
        expect(sections[0]).toBe('Recently Added');

        const dates = await page.locator('#docs-nav .docs-nav-recent a[data-added]')
            .evaluateAll(els => els.map(el => el.dataset.added));
        expect(dates.length).toBeGreaterThan(0);
        expect(dates.length).toBeLessThanOrEqual(10);
        expect([...dates].sort().reverse()).toEqual(dates);

        // Guides only, like the website's cards: an API module or spice page
        // is found in its own tree, not in "what is new to read".
        const addedRefs = await page.locator('#docs-nav .docs-nav-recent a[data-doc-ref]')
            .evaluateAll(els => els.map(el => el.dataset.docRef));
        for (const ref of addedRefs) expect(ref).toMatch(/^guides\//);

        // Closed on load -- it is a "what changed?" section, not a permanent
        // ten-row tax on the top of the nav -- and its summary opens it.
        const recent = page.locator('#docs-nav .docs-nav-recent');
        await expect(recent).not.toHaveAttribute('open', /.*/);
        await expect(recent.locator('a[data-doc-ref]').first()).toBeHidden();
        await recent.locator('summary').click();
        await expect(recent).toHaveAttribute('open', /.*/);

        // It is a nav, not a display: the entries navigate like any other.
        const ref = await page.locator('#docs-nav .docs-nav-recent a[data-doc-ref]')
            .first().getAttribute('data-doc-ref');
        await page.locator('#docs-nav .docs-nav-recent a[data-doc-ref]').first().click();
        await page.waitForFunction(
            (r) => window.turmericApp.getState().docsRef === r, ref, { timeout: 15_000 });
    });

    test('recently updated sits under recently added, newest edit first', async ({ page }) => {
        await openTry(page);
        await openDocs(page);

        // Same build-input caveat as the section above: no git history in the
        // build, no dates, and the section is absent by design. A page whose
        // only commit is its add is deliberately not listed -- it is new, not
        // updated -- so the guard counts the pages that actually qualify.
        const edited = await page.evaluate(async () => {
            const idx = await (await fetch('/docs-pack/index.json')).json();
            return (idx.guides || [])
                .filter(e => e.updated && e.updated !== e.added).length;
        });
        test.skip(edited === 0, 'pack carries no edit dates (built outside a git checkout)');

        const sections = await page.locator('#docs-nav .docs-nav-section h4').allTextContents();
        expect(sections[0]).toBe('Recently Added');
        expect(sections[1]).toBe('Recently Updated');

        const dates = await page.locator('#docs-nav .docs-nav-updated a[data-updated]')
            .evaluateAll(els => els.map(el => el.dataset.updated));
        expect(dates.length).toBeGreaterThan(0);
        expect(dates.length).toBeLessThanOrEqual(10);
        expect([...dates].sort().reverse()).toEqual(dates);

        // Collapsed on load, like its neighbour, and opened by its summary.
        const updated = page.locator('#docs-nav .docs-nav-updated');
        await expect(updated).not.toHaveAttribute('open', /.*/);
        await expect(updated.locator('a[data-doc-ref]').first()).toBeHidden();
        await updated.locator('summary').click();
        await expect(updated).toHaveAttribute('open', /.*/);

        // A page that only just landed belongs in Recently Added and nowhere
        // else: the second list never repeats an unedited arrival.
        const refs = await page.locator('#docs-nav .docs-nav-updated a[data-doc-ref]')
            .evaluateAll(els => els.map(el => el.dataset.docRef));
        const idx = await page.evaluate(async () => {
            const j = await (await fetch('/docs-pack/index.json')).json();
            const out = {};
            for (const e of j.guides || []) out[`guides/${e.slug}`] = e;
            for (const e of j.api || []) out[`api/${e.slug}`] = e;
            for (const e of j.spices || []) out[`spices/${e.slug}`] = e;
            return out;
        });
        for (const ref of refs) expect(idx[ref].updated).not.toBe(idx[ref].added);
        // Guides only, as in Recently Added.
        for (const ref of refs) expect(ref).toMatch(/^guides\//);

        // And it navigates like any other nav entry.
        const first = page.locator('#docs-nav .docs-nav-updated a[data-doc-ref]').first();
        const ref = await first.getAttribute('data-doc-ref');
        await first.click();
        await page.waitForFunction(
            (r) => window.turmericApp.getState().docsRef === r, ref, { timeout: 15_000 });
    });

    test('renders a guide with highlighting, toggles, and load-into-editor', async ({ page }) => {
        await openTry(page);
        await openDocs(page);
        await gotoDoc(page, 'guides/hkt-guide');

        await expect(page.locator('#docs-article h1')).toContainText('Higher-Kinded Types');
        // The shared guide runtime from /docs-pack/guide.js ran against the
        // freshly rendered fragment.
        expect(await page.locator('#docs-article .hl-keyword').count()).toBeGreaterThan(10);
        // Rainbow brackets, as the editor draws them: an outermost and a
        // nested depth both appear, and the guide's code is balanced.
        expect(await page.locator('#docs-article .hl-br1').count()).toBeGreaterThan(10);
        expect(await page.locator('#docs-article .hl-br2').count()).toBeGreaterThan(10);
        expect(await page.locator('#docs-article .hl-br-err').count()).toBe(0);
        expect(await page.locator('#docs-article .code-syntax-toggle').count()).toBeGreaterThan(0);
        expect(await page.locator('#docs-article .docs-load-btn').count()).toBeGreaterThan(0);
    });

    test('a guide cross-link stays inside the pane', async ({ page }) => {
        await openTry(page);
        await openDocs(page);
        await gotoDoc(page, 'guides/hkt-guide');

        const link = page.locator('#docs-article a[href^="#doc="]').first();
        const href = await link.getAttribute('href');
        const target = href.slice('#doc='.length);
        await link.click();

        await page.waitForFunction(
            (t) => window.turmericApp.getState().docsRef === t, target, { timeout: 15_000 });
        // Still on /try/ -- the whole point of embedding rather than linking out.
        expect(new URL(page.url()).pathname).toBe('/try/');
        expect(await page.evaluate(() => window.turmericApp.getState().docsOpen)).toBe(true);
    });

    test('search finds pages and symbols', async ({ page }) => {
        await openTry(page);
        await openDocs(page);

        await page.fill('#docs-search', 'vec');
        await expect(page.locator('#docs-nav .docs-results li').first()).toBeVisible();
        const headers = await page.locator('#docs-nav .docs-nav-section h4').allTextContents();
        // One box, two result kinds: pack pages and doc-names.json symbols.
        expect(headers).toContain('Pages');
        expect(headers).toContain('Symbols');

        // Clearing restores the tree.
        await page.fill('#docs-search', '');
        await expect(page.locator('#docs-nav .docs-nav-group').first()).toBeVisible();
    });

    test('#doc= deep-links restore a location and compose with #code=', async ({ page }) => {
        await openTry(page, '#doc=guides/effects-system-guide');
        await page.waitForFunction(
            () => window.turmericApp.getState().docsRef === 'guides/effects-system-guide',
            null, { timeout: 20_000 });
        await expect(page.locator('#docs-article h1')).toContainText('Effects');

        // Sharing writes #code=; it must merge with the docs key rather than
        // replacing the hash and closing the pane mid-read.
        await page.evaluate(() => {
            window._turiEditor.setValue('(+ 1 2)');
            window.turmericApp.getState();
        });
        await page.waitForTimeout(1200);
        const hash = await page.evaluate(() => location.hash);
        expect(hash).toContain('doc=guides/effects-system-guide');
        expect(await page.evaluate(() => window.turmericApp.getState().docsOpen)).toBe(true);
    });

    test('browsing docs leaves the REPL session untouched', async ({ page }) => {
        await openTry(page);
        await page.evaluate(() => window._turiEditor.setValue('(println "keep me")'));

        await openDocs(page);
        await gotoDoc(page, 'guides/hkt-guide');
        await gotoDoc(page, 'api/tur-list');
        await page.click('#docs-close');

        await expect(page.locator('#docs-overlay')).toBeHidden();
        expect(await page.evaluate(() => window._turiEditor.getValue()))
            .toBe('(println "keep me")');
    });

    test('load into editor drops a snippet in the buffer and closes the pane', async ({ page }) => {
        await openTry(page);
        await openDocs(page);
        await gotoDoc(page, 'guides/hkt-guide');

        const btn = page.locator('#docs-article .docs-load-btn').first();
        await btn.click();

        await expect(page.locator('#docs-overlay')).toBeHidden();
        const buffer = await page.evaluate(() => window._turiEditor.getValue());
        expect(buffer.trim().length).toBeGreaterThan(0);
    });

    test('the doc panel opens the pane instead of navigating away', async ({ page }) => {
        await openTry(page);
        // Surface the doc panel the way the REPL does, then follow its link.
        await page.evaluate(() => window.turmericApp.showDocPanel('cons', null, null));
        await expect(page.locator('#doc-pane')).toBeVisible();

        await page.click('#doc-full-link');
        await expect(page.locator('#docs-overlay')).toBeVisible();
        expect(new URL(page.url()).pathname).toBe('/try/');
    });
    // Reading a guide, closing the pane to try something in the editor, then
    // reopening to continue is the reason to have docs inside the REPL at all.
    // The pane already remembered WHICH page across a close (closeDocsPane
    // deliberately leaves docsCurrentRef set); it dropped WHERE IN IT, so a
    // long guide reopened at the top and you had to re-find your place.
    test('reopening the pane restores where you were in the page', async ({ page }) => {
        await openTry(page);
        await openDocs(page);
        await gotoDoc(page, 'guides/effects-system-guide');

        const scrolled = await scrollArticle(page, 900);
        expect(scrolled).toBeGreaterThan(0);   // the guide must be long enough to scroll

        await page.click('#docs-close');
        await expect(page.locator('#docs-overlay')).toBeHidden();

        await openDocs(page);
        await expectArticleAt(page, scrolled);
    });

    // Per REF, not one global offset: the pane is a browser over many pages,
    // so coming back to guide A must not restore guide B's position.
    test('each page remembers its own offset', async ({ page }) => {
        await openTry(page);
        await openDocs(page);

        await gotoDoc(page, 'guides/effects-system-guide');
        const first = await scrollArticle(page, 700);
        expect(first).toBeGreaterThan(0);

        // A page opened for the FIRST time still starts at the top, whatever
        // the page you came from was scrolled to.
        await gotoDoc(page, 'guides/hkt-guide');
        await expectArticleAt(page, 0);
        const second = await scrollArticle(page, 300);
        expect(second).toBeGreaterThan(0);

        await gotoDoc(page, 'guides/effects-system-guide');
        await expectArticleAt(page, first);

        // ... and the other page still has its own, not the one just restored.
        await gotoDoc(page, 'guides/hkt-guide');
        await expectArticleAt(page, second);
    });

    // An explicit anchor is a request for a specific heading; it outranks
    // wherever you happened to be last time.
    //
    // Unlike the two above, this one passes against the pre-restore code too --
    // it is a guard, not a repro. Restoring an offset is exactly the kind of
    // change that quietly outranks an anchor, and the failure would look like
    // "deep links stopped working" rather than anything to do with scrolling.
    test('an explicit anchor wins over a remembered offset', async ({ page }) => {
        await openTry(page);
        await openDocs(page);
        await gotoDoc(page, 'guides/effects-system-guide');
        const remembered = await scrollArticle(page, 900);
        expect(remembered).toBeGreaterThan(0);

        const anchor = await page.evaluate(() => {
            const h = document.querySelector('#docs-article h2[id], #docs-article h3[id]');
            return h ? h.id : null;
        });
        test.skip(!anchor, 'guide fragment has no anchored heading to aim at');

        await gotoDoc(page, 'guides/hkt-guide');
        await page.evaluate(
            (ref) => window.turmericApp.showDocsPage(ref),
            `guides/effects-system-guide#${anchor}`);
        await settleArticle(page);
        const delta = await page.evaluate((id) => {
            const a = document.getElementById('docs-article');
            const h = document.getElementById(id);
            if (!a || !h) return null;
            return h.getBoundingClientRect().top - a.getBoundingClientRect().top;
        }, anchor);
        expect(delta).not.toBeNull();
        expect(Math.abs(delta)).toBeLessThan(60);
    });

    test('the Guides section switches between Categories and Tags', async ({ page }) => {
        await openTry(page);

        // Tags come from the guides' frontmatter. A pack built before they
        // existed carries none, and the nav then draws no switch by design.
        const tagged = await page.evaluate(async () => {
            const idx = await (await fetch('/docs-pack/index.json')).json();
            return (idx.guides || []).filter(e => e.tags && e.tags.length).length;
        });
        test.skip(!tagged, 'pack carries no guide tags -- rebuild it with `tur run docs`');

        await openDocs(page);
        const guides = page.locator('#docs-nav .docs-nav-section', { has: page.locator('h4', { hasText: /^Guides$/ }) });
        const tabs = guides.locator('.docs-view-btn');
        await expect(tabs).toHaveText(['Categories', 'Tags']);
        await expect(guides.locator('.docs-view-btn.active')).toHaveText('Categories');
        const groups = guides.locator('details.docs-nav-group > summary');
        const categoryGroups = await groups.allTextContents();
        expect(categoryGroups).toContain('Getting Started');

        // Tags: one group per tag with a guide count, alphabetical.
        await guides.locator('.docs-view-btn', { hasText: 'Tags' }).click();
        await expect(guides.locator('.docs-view-btn.active')).toHaveText('Tags');
        await expect(guides.locator('.docs-view-btn.active')).toHaveAttribute('aria-selected', 'true');
        const names = await groups.evaluateAll(
            els => els.map(e => e.firstChild.textContent.trim()));
        expect(names.length).toBeGreaterThan(categoryGroups.length);
        // The pseudo-tag `other` holds untagged guides and sorts last.
        expect(names[names.length - 1]).toBe('other');
        const named = names.filter(n => n !== 'other');
        expect(named).toEqual([...named].sort((a, b) => a.localeCompare(b)));
        await expect(groups.first().locator('.docs-nav-count')).toHaveText(/^\d+$/);

        // A guide with several tags is listed under each of them.
        const multi = await page.evaluate(async () => {
            const idx = await (await fetch('/docs-pack/index.json')).json();
            const g = (idx.guides || []).find(e => (e.tags || []).length > 1);
            return g ? { ref: `guides/${g.slug}`, n: g.tags.length } : null;
        });
        test.skip(!multi, 'no guide carries more than one tag');
        expect(await guides.locator(`a[data-doc-ref="${multi.ref}"]`).count()).toBe(multi.n);

        // The choice survives a reload, and the state says so.
        await openTry(page, '#doc=guides/quickstart');
        await page.waitForSelector('#docs-nav .docs-nav-section', { timeout: 15_000 });
        expect(await page.evaluate(() => window.turmericApp.getState().docsGuideView)).toBe('tag');
        await expect(guides.locator('.docs-view-btn.active')).toHaveText('Tags');

        // Arrow keys move between the two tabs, and focus stays on the active one.
        await guides.locator('.docs-view-btn.active').focus();
        await page.keyboard.press('ArrowLeft');
        await expect(guides.locator('.docs-view-btn.active')).toHaveText('Categories');
        await expect(guides.locator('.docs-view-btn.active')).toBeFocused();
        expect(await groups.allTextContents()).toEqual(categoryGroups);
    });

    test('a guide draws one rule under its headline, not two', async ({ page }) => {
        await openTry(page);
        await openDocs(page);
        await gotoDoc(page, 'guides/hamt-guide');

        // The Contents card has a border of its own, so the headline's rule
        // above it made every guide read as two lines in a row.
        const rules = await page.evaluate(() => {
            const h1 = document.querySelector('#docs-article h1');
            const toc = document.querySelector('#docs-article .guide-toc');
            if (!h1 || !toc) return null;
            return {
                h1: parseFloat(getComputedStyle(h1).borderBottomWidth),
                toc: parseFloat(getComputedStyle(toc).borderTopWidth),
            };
        });
        test.skip(!rules, 'guide has no Contents card to sit under its headline');
        expect(rules.h1).toBe(0);
        expect(rules.toc).toBeGreaterThan(0);

        // An API page has no Contents card, so its headline keeps its rule.
        await gotoDoc(page, 'api/tur-list');
        const api = await page.evaluate(() =>
            parseFloat(getComputedStyle(document.querySelector('#docs-article h1')).borderBottomWidth));
        expect(api).toBeGreaterThan(0);
    });

    test('API signatures and fenced prose code are syntax-highlighted', async ({ page }) => {
        await openTry(page);
        await openDocs(page);
        await gotoDoc(page, 'api/httpd-compress');

        // Both come from the generator marking the block language-turmeric: a
        // signature always, and a prose block only when its docstring fences it.
        // The pane's highlighter then stamps each with data-hl-done.
        const sig = page.locator('#docs-article pre.def-signature code.language-turmeric');
        await expect(sig.first()).toHaveAttribute('data-hl-done', '1');
        expect(await sig.count()).toBeGreaterThan(0);
        // A bracket inside a signature is wrapped in a coloured span.
        await expect(sig.first().locator('[class^="hl-br"]').first()).toBeVisible();

        const prose = page.locator('#docs-article pre.def-prose-code code.language-turmeric');
        await expect(prose.first()).toHaveAttribute('data-hl-done', '1');
        expect(await prose.count()).toBeGreaterThan(1);
    });
});
