import { test, expect } from '@playwright/test';
import { readFile } from 'node:fs/promises';

async function gotoTry(page) {
    await page.goto('/try/');
    await expect(page.locator('#wasm-status-text')).toHaveText('Ready', { timeout: 30_000 });
    await page.waitForFunction(() => !!window._turiTabs);
}

// Minimal store-only ZIP reader. Mirrors the writer in zip.js -- we only
// support entries with method 0 (store), which is everything our writer
// produces. Returns { [path]: Uint8Array }.
function readStoreZip(bytes) {
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    // Locate the EOCD by scanning back from the end for 0x06054b50.
    let eocd = -1;
    for (let i = bytes.length - 22; i >= 0 && i > bytes.length - 65536; i--) {
        if (view.getUint32(i, true) === 0x06054b50) { eocd = i; break; }
    }
    if (eocd < 0) throw new Error('EOCD not found -- not a ZIP file');
    const total      = view.getUint16(eocd + 10, true);
    const cdSize     = view.getUint32(eocd + 12, true);
    const cdOffset   = view.getUint32(eocd + 16, true);
    const dec = new TextDecoder('utf-8');
    const out = {};
    let p = cdOffset;
    for (let i = 0; i < total; i++) {
        if (view.getUint32(p, true) !== 0x02014b50) {
            throw new Error(`Bad central directory entry at ${p}`);
        }
        const method   = view.getUint16(p + 10, true);
        const compSize = view.getUint32(p + 20, true);
        const nameLen  = view.getUint16(p + 28, true);
        const extraLen = view.getUint16(p + 30, true);
        const cmtLen   = view.getUint16(p + 32, true);
        const localOff = view.getUint32(p + 42, true);
        const name = dec.decode(bytes.subarray(p + 46, p + 46 + nameLen));
        if (method !== 0) throw new Error(`Unsupported method ${method} for ${name}`);
        // Read the local header to find the data offset.
        const lNameLen  = view.getUint16(localOff + 26, true);
        const lExtraLen = view.getUint16(localOff + 28, true);
        const dataOff = localOff + 30 + lNameLen + lExtraLen;
        out[name] = bytes.subarray(dataOff, dataOff + compSize);
        p += 46 + nameLen + extraLen + cmtLen;
    }
    if (p - cdOffset !== cdSize) {
        throw new Error('Central directory size mismatch');
    }
    return out;
}

test.describe('Project download (Phase 2)', () => {
    test('projectEntries() returns build.tur + src + workspace.json for one tab', async ({ page }) => {
        await gotoTry(page);
        await page.evaluate(() => window._turiEditor.setValue('(println "hello")'));
        await page.waitForTimeout(400);
        const entries = await page.evaluate(() => window._turiTabs.projectEntries());
        const paths = entries.map(e => e.path);
        expect(paths).toContain('build.tur');
        expect(paths).toContain('src/main.tur');
        expect(paths).toContain('.turmeric/workspace.json');
        const buildTur = entries.find(e => e.path === 'build.tur').content;
        expect(buildTur).toMatch(/:main\s+"main\.tur"/);
        expect(buildTur).toMatch(/:src\s+\["main\.tur"\]/);
        const mainSrc = entries.find(e => e.path === 'src/main.tur').content;
        expect(mainSrc).toBe('(println "hello")');
        const ws = JSON.parse(entries.find(e => e.path === '.turmeric/workspace.json').content);
        expect(ws.version).toBe(1);
        expect(ws.tabs).toHaveLength(1);
        expect(ws.tabs[0].name).toBe('main.tur');
        expect(ws.tabs[0].file).toBe('src/main.tur');
        expect(ws.activeId).toBe(ws.tabs[0].id);
    });

    test('multi-tab download yields a slugified, collision-free src tree', async ({ page }) => {
        await gotoTry(page);
        // Two tabs with shell-unfriendly characters + collisions.
        await page.evaluate(() => window._turiTabs.create({ name: 'util:bad/name.tur' }));
        await page.evaluate(() => window._turiTabs.rename(
            window._turiTabs.tabs()[1].id, 'util:bad/name'));  // ends up as util_bad_name.tur
        await page.evaluate(() => window._turiTabs.create({ name: 'main.tur' }));  // collides with seed
        const entries = await page.evaluate(() => window._turiTabs.projectEntries());
        const srcPaths = entries.filter(e => e.path.startsWith('src/')).map(e => e.path).sort();
        // Original seed main.tur stays as src/main.tur; the colliding new tab
        // gets disambiguated to main-2.tur (or similar) via slugifyTabName.
        // util:bad/name.tur becomes util_bad_name.tur.
        expect(srcPaths).toContain('src/main.tur');
        expect(srcPaths.some(p => p.startsWith('src/main-'))).toBe(true);
        expect(srcPaths.some(p => /util.*name\.tur$/.test(p))).toBe(true);
        // No path-escape characters survived into a filename.
        for (const p of srcPaths) {
            const base = p.replace(/^src\//, '');
            expect(base).not.toMatch(/[\\\/<>:"|?*]/);
        }
    });

    test('Download button triggers a valid ZIP whose entries match projectEntries()', async ({ page }) => {
        await gotoTry(page);
        await page.evaluate(() => window._turiTabs.create({ name: 'util.tur' }));
        await page.evaluate(() => window._turiEditor.setValue('(defn square [x] (* x x))'));
        await page.waitForTimeout(400);
        const expected = await page.evaluate(() => window._turiTabs.projectEntries());

        // On mobile the #download-btn is hidden behind the ⋯ overflow menu,
        // so open the menu and click the menu item -- this also covers the
        // actual mobile-user path.
        const [download] = await Promise.all([
            page.waitForEvent('download', { timeout: 10_000 }),
            (async () => {
                await page.locator('#more-btn').click();
                await page.locator('.more-item[data-cmd="download-btn"]').click();
            })(),
        ]);
        const path = await download.path();
        expect(path).toBeTruthy();
        const bytes = await readFile(path);
        // ZIP local-file-header signature.
        expect(bytes[0]).toBe(0x50);  // 'P'
        expect(bytes[1]).toBe(0x4b);  // 'K'
        expect(bytes[2]).toBe(0x03);
        expect(bytes[3]).toBe(0x04);

        const parsed = readStoreZip(new Uint8Array(bytes));
        const decoder = new TextDecoder('utf-8');
        for (const entry of expected) {
            expect(parsed[entry.path], `missing ${entry.path}`).toBeTruthy();
            expect(decoder.decode(parsed[entry.path])).toBe(entry.content);
        }
    });

    test('download filename matches try-turmeric-<timestamp>.zip', async ({ page }) => {
        await gotoTry(page);
        // On mobile the #download-btn is hidden behind the ⋯ overflow menu,
        // so open the menu and click the menu item -- this also covers the
        // actual mobile-user path.
        const [download] = await Promise.all([
            page.waitForEvent('download', { timeout: 10_000 }),
            (async () => {
                await page.locator('#more-btn').click();
                await page.locator('.more-item[data-cmd="download-btn"]').click();
            })(),
        ]);
        expect(download.suggestedFilename()).toMatch(/^try-turmeric-.*\.zip$/);
    });
});
