import { test, expect } from '@playwright/test';
import { submitAtPrompt } from './repl-helpers.js';

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

async function waitForReady(page) {
    await expect(page.locator('#wasm-status-text')).toHaveText('Ready', { timeout: 30_000 });
}

async function setCode(page, code) {
    await page.evaluate((c) => window._turiEditor.setValue(c), code);
}

async function runAndGetConsole(page) {
    const before = await page.locator('#console').textContent();
    await page.click('#run-btn');
    // Wait for new content to appear
    await page.waitForFunction(
        (prev) => document.getElementById('console').textContent !== prev,
        before,
        { timeout: 10_000 }
    );
    return page.locator('#console').textContent();
}

async function hasNoEvalError(page) {
    // console-error spans that come from the Turmeric evaluator start with #<error
    const errors = await page.locator('#console .console-error').allTextContents();
    return errors.every(e => !e.startsWith('#<error'));
}

// ---------------------------------------------------------------------------
// Core smoke tests
// ---------------------------------------------------------------------------

test.describe('Try Turmeric smoke tests', () => {
    test('page loads and WASM initializes', async ({ page }) => {
        const jsErrors = [];
        page.on('console', msg => {
            if (msg.type() === 'error') jsErrors.push(msg.text());
        });

        await page.goto('/try/');
        await waitForReady(page);

        expect(jsErrors.filter(e => e.includes('TypeError') || e.includes('Failed to'))).toHaveLength(0);
    });

    test('evaluates (+ 1 2) and shows result 3', async ({ page }) => {
        await page.goto('/try/');
        await waitForReady(page);
        await setCode(page, '(+ 1 2)');
        await page.click('#run-btn');
        await expect(page.locator('#console')).toContainText('3', { timeout: 10_000 });
        await expect(page.locator('#console .console-error')).toHaveCount(0);
    });

    test('evaluates println and shows output', async ({ page }) => {
        await page.goto('/try/');
        await waitForReady(page);
        await setCode(page, '(println "hello from test")');
        await page.click('#run-btn');
        await expect(page.locator('#console')).toContainText('hello from test', { timeout: 10_000 });
    });

    test('REPL input evaluates expression in same context', async ({ page }) => {
        await page.goto('/try/');
        await waitForReady(page);

        // Define a function via the editor
        await setCode(page, '(defn double [x :int] (* x 2))');
        await page.click('#run-btn');
        await page.waitForTimeout(500);

        // Call it from the REPL — same env
        await submitAtPrompt(page, '(double 21)');

        await expect(page.locator('#console')).toContainText('42', { timeout: 10_000 });
    });

    test('Run invokes a top-level main entry point', async ({ page }) => {
        await page.goto('/try/');
        await waitForReady(page);

        // A program that defines `main` should have it invoked (like `tur run`),
        // showing its computed value -- not the bare `#<fn main>` closure.
        await setCode(page, '(defn main [] :int (+ 1 1))');
        await page.click('#run-btn');
        await expect(page.locator('#console')).toContainText('2', { timeout: 10_000 });
        await expect(page.locator('#console')).not.toContainText('#<fn main>');
    });

    test('Run invokes main and shows its printed output', async ({ page }) => {
        await page.goto('/try/');
        await waitForReady(page);

        await setCode(page, '(defn main [] :int\n  (println "from main")\n  0)');
        await page.click('#run-btn');
        await expect(page.locator('#console')).toContainText('from main', { timeout: 10_000 });
        await expect(page.locator('#console')).not.toContainText('#<fn main>');
    });

    test('Force update clears caches and navigates off the cached URL', async ({ page }) => {
        await page.goto('/try/');
        // Deliberately NOT waitForReady: this is about the overflow menu and
        // the service worker, and nothing here touches the evaluator. Waiting
        // on the WASM boot only made the case unrunnable in a checkout without
        // a built public/turmeric.wasm -- which is any fresh worktree, and is
        // why the assertion below went unverified locally when it changed.
        await page.waitForFunction(() => !!window.turmericApp, null, { timeout: 30_000 });

        // Stub the destructive bits so the test observes intent without actually
        // unregistering the SW or navigating away.
        await page.evaluate(() => {
            window.__cachesDeleted = [];
            window.__reloaded = false;
            if (window.caches) {
                caches.keys = async () => ['stale-cache'];
                caches.delete = async (k) => { window.__cachesDeleted.push(k); return true; };
            }
            if (navigator.serviceWorker) {
                navigator.serviceWorker.getRegistrations = async () => [];
            }
            // `location.reload` / `location.replace` are non-configurable in
            // current Chromium, so they cannot be stubbed; the app goes through
            // hardNavigate, which defers to this hook when a test installs one.
            window.__turiReload = () => { window.__reloaded = true; };
            window.__navigatedTo = null;
            window.__turiNavigate = (url) => { window.__navigatedTo = url; };
        });

        // Open the overflow menu, retrying until it is wired. `turmericApp` is
        // assigned at module scope, long before init() attaches this button's
        // listener, so a single click can land on nothing. Click only while the
        // menu is closed -- clicking blindly in a poll would toggle it shut
        // again on the iteration after it opened.
        await expect.poll(async () => {
            if (!(await page.locator('#more-menu').isVisible())) {
                await page.locator('#more-btn').click().catch(() => {});
            }
            return page.locator('#more-menu').isVisible();
        }, { timeout: 20_000 }).toBe(true);
        await page.locator('.more-item[data-action="force-update"]').click();

        // A NAVIGATION, not a reload: unregistering the worker and emptying
        // Cache Storage does not touch WebKit's own HTTP/page cache, which in
        // an iOS standalone web app can answer location.reload() with the very
        // HTML this is trying to replace.
        await expect.poll(() => page.evaluate(() => window.__navigatedTo))
            .toMatch(/^\/try\/\?u=\d+$/);
        expect(await page.evaluate(() => window.__reloaded)).toBe(false);
        expect(await page.evaluate(() => window.__cachesDeleted)).toContain('stale-cache');
    });

    test('REPL history navigates with arrow keys', async ({ page }) => {
        await page.goto('/try/');
        await waitForReady(page);

        await submitAtPrompt(page, '(+ 1 1)');
        await page.waitForTimeout(300);

        await submitAtPrompt(page, '(+ 2 2)');
        await page.waitForTimeout(300);

        // The arrows still walk history after an evaluation. This is the case
        // that broke in c2mp: the arrows and the completion list want the same
        // keys, and with two listeners the winner depends on registration
        // order. One handler decides, so this is a claim about that handler.
        const promptValue = () => page.evaluate(() => window._turiRepl.value());

        await page.keyboard.press('ArrowUp');
        await expect.poll(promptValue).toBe('(+ 2 2)');

        await page.keyboard.press('ArrowUp');
        await expect.poll(promptValue).toBe('(+ 1 1)');

        await page.keyboard.press('ArrowDown');
        await expect.poll(promptValue).toBe('(+ 2 2)');
    });
});

// ---------------------------------------------------------------------------
// Example dropdown smoke tests
// ---------------------------------------------------------------------------

test.describe('Examples', () => {
    // Load the page once per describe block via beforeEach
    test.beforeEach(async ({ page }) => {
        await page.goto('/try/');
        await waitForReady(page);
    });

    test('hello world — prints Hello, Turmeric!', async ({ page }) => {
        await setCode(page, '(println "Hello, Turmeric!")');
        await runAndGetConsole(page);
        await expect(page.locator('#console')).toContainText('Hello, Turmeric!');
        expect(await hasNoEvalError(page)).toBe(true);
    });

    test('math — last expression (* 16 16) returns 256', async ({ page }) => {
        await setCode(page, [
            '(+ 1 2 3)',
            '(- 10 5)',
            '(* 2 3 4)',
            '(/ 100 4)',
            '(* 16 16)',
        ].join('\n'));
        await runAndGetConsole(page);
        await expect(page.locator('#console')).toContainText('256');
        expect(await hasNoEvalError(page)).toBe(true);
    });

    test('factorial — (factorial 10) returns 3628800', async ({ page }) => {
        await setCode(page, [
            '(defn factorial [n :int] :int',
            '  (if (<= n 1) 1 (* n (factorial (- n 1)))))',
            '(factorial 10)',
        ].join('\n'));
        await runAndGetConsole(page);
        await expect(page.locator('#console')).toContainText('3628800');
        expect(await hasNoEvalError(page)).toBe(true);
    });

    test('fibonacci — (fib 10) returns 55', async ({ page }) => {
        await setCode(page, [
            '(defn fib [n :int] :int',
            '  (if (<= n 1) n (+ (fib (- n 1)) (fib (- n 2)))))',
            '(fib 10)',
        ].join('\n'));
        await runAndGetConsole(page);
        await expect(page.locator('#console')).toContainText('55');
        expect(await hasNoEvalError(page)).toBe(true);
    });

    test('type annotations — (add 1 2) returns 3', async ({ page }) => {
        await setCode(page, [
            '(defn add [a :int b :int] :int (+ a b))',
            '(add 1 2)',
        ].join('\n'));
        await runAndGetConsole(page);
        await expect(page.locator('#console')).toContainText('3');
        expect(await hasNoEvalError(page)).toBe(true);
    });

    test('higher-order — (twice inc 5) returns 7', async ({ page }) => {
        await setCode(page, [
            '(defn twice [f :(fn [a] a) x :a] :a (f (f x)))',
            '(defn inc [x :int] :int (+ x 1))',
            '(twice inc 5)',
        ].join('\n'));
        await runAndGetConsole(page);
        await expect(page.locator('#console')).toContainText('7');
        expect(await hasNoEvalError(page)).toBe(true);
    });

    test('closures — make-adder(5)(10) prints 15', async ({ page }) => {
        await setCode(page, [
            '(defn make-adder [x :int] (fn [y :int] (+ x y)))',
            '(let [add5 (make-adder 5)]',
            '  (println (add5 10)))',
        ].join('\n'));
        await runAndGetConsole(page);
        await expect(page.locator('#console')).toContainText('15');
        expect(await hasNoEvalError(page)).toBe(true);
    });

    test('effects — (perform Ask) resumes with 41 → prints 42', async ({ page }) => {
        await setCode(page, [
            '(defeffect Ask [] :int)',
            '(defn use-ask [] :int',
            '  (+ 1 (perform (Ask))))',
            '(println (handle (use-ask)',
            '  (Ask [] k) (resume k 41)))',
        ].join('\n'));
        await runAndGetConsole(page);
        await expect(page.locator('#console')).toContainText('42');
        expect(await hasNoEvalError(page)).toBe(true);
    });

    test('pattern matching — factorial(5) returns 120', async ({ page }) => {
        await setCode(page, [
            '(defn factorial [n :int] :int',
            '  (match n',
            '    0 1',
            '    1 1',
            '    _ (* n (factorial (- n 1)))))',
            '(factorial 5)',
        ].join('\n'));
        await runAndGetConsole(page);
        await expect(page.locator('#console')).toContainText('120');
        expect(await hasNoEvalError(page)).toBe(true);
    });
});
