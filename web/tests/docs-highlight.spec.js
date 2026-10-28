// Docs highlighter: types, kinds, modifiers and builtins.
//
// The docs pages and Try Turmeric's docs pane colour code with the highlighter
// in tools/genguides.py, and it reads a `^name` and a type the way the editor
// (web/main.js, tests/editor-highlight.spec.js) does: a modifier from the closed
// list is hl-ann (teal), a kind, a constraint and a type are violet, an extern-c
// C type is a type, and a builtin is hl-builtin (periwinkle).
//
// The highlighter ships inside a generated script, so the test takes it from the
// generator itself rather than from a built docs pack: no wasm and no docs build
// are needed, and the spec cannot drift from what the pages run.

import { test, expect } from '@playwright/test';
import { execFileSync } from 'node:child_process';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const REPO = path.dirname(path.dirname(path.dirname(fileURLToPath(import.meta.url))));
const CORE = execFileSync('python3', ['-c',
    'import sys; sys.path.insert(0, "tools"); import genguides; sys.stdout.write(genguides.GUIDE_JS_CORE)'],
    { cwd: REPO, encoding: 'utf8' });

// Every coloured token of one highlighted source, as [class, text].
async function tokens(page, src) {
    return page.evaluate((code) => {
        const box = document.createElement('div');
        box.innerHTML = window.turmericGuide.hl(code);
        return [...box.querySelectorAll('span')].map(s => [s.className, s.textContent]);
    }, src);
}

test.describe('docs highlighter', () => {
    test.beforeEach(async ({ page }) => {
        await page.setContent('<!doctype html><html><body></body></html>');
        await page.addScriptTag({ content: CORE });
    });

    test('a ^name is read by what it means', async ({ page }) => {
        const toks = await tokens(page,
            '(defn step [^fat g ^mut m ^f k ^^g n ^&cols c ^Monoid q] 0)\n'
            + '(defclass Comonad [^w] (extract [x] 0))\n'
            + '(extern-c sqrt [^float ^ptr<void>] :float)');
        const cls = (text) => toks.find(([, t]) => t === text)?.[0];
        for (const mod of ['^fat', '^mut'])
            expect(cls(mod), `${mod} is a modifier`).toBe('hl-ann');
        for (const kind of ['^f', '^^g', '^&cols', '^Monoid', '^w'])
            expect(cls(kind), `${kind} is a kind or constraint`).toBe('hl-kind');
        for (const ct of ['^float', '^ptr<void>'])
            expect(cls(ct), `${ct} is a type`).toBe('hl-type');
    });

    test('the flags of a type declaration are modifiers', async ({ page }) => {
        // caret-type-attributes-plan: `^copy` before the name is the same flag as
        // the trailing `:copy`, so it reads as a modifier, not as a kind.
        const toks = await tokens(page,
            '(defstruct ^copy ^heap Cell [n : int])\n'
            + '(defopaque ^linear ^sealed ^non-null Tok :ptr<void>)\n'
            + '(defdata ^affine ^no-auto-ctor Job (Job int))\n'
            + '(defdata ^move Pair [^f a] (Pair a))');
        const cls = (text) => toks.find(([, t]) => t === text)?.[0];
        for (const flag of ['^copy', '^heap', '^linear', '^sealed', '^non-null',
                            '^affine', '^no-auto-ctor', '^move'])
            expect(cls(flag), `${flag} is a modifier`).toBe('hl-ann');
        // A kind in the type-parameter vector is still a kind.
        expect(cls('^f')).toBe('hl-kind');
    });

    test('a type is one token, fused or spaced', async ({ page }) => {
        const toks = await tokens(page,
            '(defn hello [name : cstr] : void (println name))\n'
            + '(defn f [x :int p : ptr<void>] : option<ptr<int>> x)');
        const types = toks.filter(([c]) => c === 'hl-type').map(([, t]) => t);
        // The spaced form: the colon and the name are each a type token.
        expect(types).toContain('cstr');
        expect(types).toContain('void');
        expect(types).toContain('ptr<void>');
        expect(types).toContain('option<ptr<int>>');
        // The fused form is still one token, colon included.
        expect(types).toContain(':int');
        // Never a stranded generic argument list.
        expect(types.every(t => !/^</.test(t))).toBe(true);
    });

    test('a standalone colon is a type token whatever follows it', async ({ page }) => {
        const src = '(defn each [x : (w a) g : (fn [int] int) n : int] : (Option a) x)';
        const toks = await tokens(page, src);
        const colons = toks.filter(([c, t]) => c === 'hl-type' && t === ':');
        expect(colons.length).toBe((src.match(/ : /g) || []).length);
    });

    test('builtins are periwinkle, keywords stay gold, less-than is not a type', async ({ page }) => {
        const toks = await tokens(page, '(defn f [a b] (println (map g (< a b))))');
        const cls = (text) => toks.find(([, t]) => t === text)?.[0];
        expect(cls('println')).toBe('hl-builtin');
        expect(cls('map')).toBe('hl-builtin');
        expect(cls('defn')).toBe('hl-keyword');
        expect(toks.some(([c, t]) => c === 'hl-type' && t.includes('<'))).toBe(false);
    });
});
