// Try Turmeric editor: syntax highlighting of annotations and types.
//
// The docs pages colour `^annotations` purple and a type -- `:ptr<void>`, with
// its generic arguments -- as one teal token, and the editor does the same.
// The expected colours are read from web/editor-palette.json, the one place the
// token colours are written down, so the test holds in either theme and fails
// if the editor and the palette drift apart. (tests/check-editor-palette.py
// pins the theme rules; this pins what the tokenizer feeds them.)

import { test, expect } from '@playwright/test';
import { readFileSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const WEB_DIR = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const PALETTE = JSON.parse(readFileSync(path.join(WEB_DIR, 'editor-palette.json'), 'utf8'));

const rgb = (hex) => {
    const n = parseInt(hex, 16);
    return `rgb(${(n >> 16) & 255}, ${(n >> 8) & 255}, ${n & 255})`;
};

const SOURCE = [
    '(defn f [x :ptr<void> ^fat g :(fn [ptr<void>] nil)] :option<ptr<int>> x)',
    '(defn step [^state/GameState s ^&cols c ^f m] :ptr<ptr<ptr<void>>> s)',
    'def ^mut counter 0',
    '(< a b) ; a ^note and a :type in a comment',
    '"a ^str :int ptr<void>"',
].join('\n');

for (const scheme of ['dark', 'light']) {
test.describe(`editor highlighting (${scheme})`, () => {
    test.use({ colorScheme: scheme });

    test('annotations are purple and types are teal, each one token', async ({ page }) => {
        await page.goto('/try/');
        await page.waitForFunction(() => !!window.turmericApp && !!window._turiEditor,
                                   null, { timeout: 30_000 });
        await page.evaluate((src) => window._turiEditor.setValue(src), SOURCE);
        await page.waitForSelector('#editor .view-line span span');
        // Tokenization runs after the value is set; wait for the colouring.
        await page.waitForFunction(() => [...document.querySelectorAll('#editor .view-line span span')]
            .some(s => s.textContent === '^fat'), null, { timeout: 15_000 });

        const theme = (await page.evaluate(() => document.documentElement.getAttribute('data-theme')))
            === 'light' ? 'light' : 'dark';
        expect(theme, 'the page follows the browser colour scheme').toBe(scheme);
        const colours = PALETTE[theme];

        // Every rendered token as [text, colour]; spaces come through as nbsp.
        const spans = await page.evaluate(() =>
            [...document.querySelectorAll('#editor .view-line span span')].map(
                s => [s.textContent.replace(/ /g, ' ').trim(), getComputedStyle(s).color]));
        const colourOf = (text) => {
            const hit = spans.find(([t]) => t === text);
            return hit ? hit[1] : undefined;
        };

        // One token, caret and all: the whole symbol, in the annotation colour.
        for (const ann of ['^fat', '^state/GameState', '^&cols', '^f', '^mut']) {
            expect(colourOf(ann), `${ann} is one annotation token`).toBe(rgb(colours.annotation));
        }
        // One token, generic arguments and all: the whole type, in the type colour.
        for (const type of [':ptr<void>', 'ptr<void>', ':option<ptr<int>>', ':ptr<ptr<ptr<void>>>']) {
            expect(colourOf(type), `${type} is one type token`).toBe(rgb(colours.type));
        }

        // What did not change: less-than is still an operator, and a `^` or `:`
        // inside a comment or a string stays that comment or string.
        expect(colourOf('<')).toBe(rgb(colours.operator));
        expect(spans.find(([t]) => t.startsWith('; a ^note'))?.[1]).toBe(rgb(colours.comment));
        expect(spans.find(([t]) => t.startsWith('"a ^str'))?.[1]).toBe(rgb(colours.string));
    });
});
}
