// Try Turmeric editor: syntax highlighting of annotations and types.
//
// `^annotations` and types share one violet: a type -- `:ptr<void>`, with its
// generic arguments, or `: cstr` spelled with a space -- is one token, as an
// annotation is.
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
    '(defn hello [name : cstr] : void)',
    '(defn pick [m : option<ptr<int>>] : int m)',
].join('\n');

for (const scheme of ['dark', 'light']) {
test.describe(`editor highlighting (${scheme})`, () => {
    test.use({ colorScheme: scheme });

    test('annotations and types are violet, each one token', async ({ page }) => {
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

        // The spaced spelling, `name : cstr` and `] : void`: the type name is
        // a type token even though the colon stands apart from it.
        for (const type of ['cstr', 'void', 'option<ptr<int>>']) {
            expect(colourOf(type), `${type} after a spaced colon is a type token`).toBe(rgb(colours.type));
        }

        // What did not change: less-than is still an operator, and a `^` or `:`
        // inside a comment or a string stays that comment or string.
        expect(colourOf('<')).toBe(rgb(colours.operator));
        expect(spans.find(([t]) => t.startsWith('; a ^note'))?.[1]).toBe(rgb(colours.comment));
        expect(spans.find(([t]) => t.startsWith('"a ^str'))?.[1]).toBe(rgb(colours.string));
    });

    test('a keyword is a whole symbol: apply-to is not the builtin apply', async ({ page }) => {
        // Hyphenated, namespaced and punctuated names that start with a keyword
        // or builtin, and the bare words themselves. [source line, symbol, palette key].
        const CASES = [
            ['(apply-to f x)', 'apply-to', 'foreground'],
            ['(map-new)', 'map-new', 'foreground'],
            ['(read-line)', 'read-line', 'foreground'],
            ['(for-each f l)', 'for-each', 'foreground'],
            ['(do-m x)', 'do-m', 'foreground'],
            ['(when-let [a b] a)', 'when-let', 'foreground'],
            ['(def-struct P)', 'def-struct', 'foreground'],
            ['(int-to-str n)', 'int-to-str', 'foreground'],
            ['(true-if a b)', 'true-if', 'foreground'],
            ['(rc/of-thing x)', 'rc/of-thing', 'foreground'],
            ['(set-field! c n 1)', 'set-field!', 'foreground'],
            ['(string/adopt-cstr s)', 'string/adopt-cstr', 'foreground'],
            ['(string->cstr s)', 'string->cstr', 'foreground'],
            ['(println *args*)', '*args*', 'foreground'],
            // still keywords when they stand alone
            ['(apply f x)', 'apply', 'keywordOperator'],
            ['(map f l)', 'map', 'keywordOperator'],
            ['(rc/of x)', 'rc/of', 'keywordOperator'],
            ['(defn f [] 1)', 'defn', 'keyword'],
            ['(let [x 1] x)', 'let', 'keyword'],
            ['(f nil)', 'nil', 'type'],
            // and the operators are untouched
            ['(- a 1)', '-', 'operator'],
            ['(<= a b)', '<=', 'operator'],
        ];
        await page.goto('/try/');
        await page.waitForFunction(() => !!window.turmericApp && !!window._turiEditor,
                                   null, { timeout: 30_000 });
        await page.evaluate((src) => window._turiEditor.setValue(src), CASES.map(c => c[0]).join('\n'));
        await page.waitForFunction((n) => document.querySelectorAll('#editor .view-line').length >= n,
                                   CASES.length, { timeout: 15_000 });
        await page.waitForTimeout(500);

        const theme = (await page.evaluate(() => document.documentElement.getAttribute('data-theme')))
            === 'light' ? 'light' : 'dark';
        const colours = PALETTE[theme];

        // Per source line, every rendered token as [text, colour]. Monaco merges
        // neighbouring tokens of one colour into one span, so a symbol is "whole"
        // when one span contains all of it; a split leaves no such span.
        const lines = await page.evaluate(() =>
            [...document.querySelectorAll('#editor .view-line')]
                .sort((a, b) => a.getBoundingClientRect().top - b.getBoundingClientRect().top)
                .map(l => [...l.querySelectorAll(':scope > span > span')].map(
                    s => [s.textContent.replace(/\u00a0/g, ' '), getComputedStyle(s).color])));

        CASES.forEach(([src, sym, key], i) => {
            const hit = lines[i].find(([t]) => t.includes(sym));
            expect(hit, `${sym} in ${src} is one token`).toBeTruthy();
            expect(hit[1], `${sym} in ${src}`).toBe(rgb(colours[key]));
        });
    });
});
}
