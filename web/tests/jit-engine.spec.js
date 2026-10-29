import { test, expect } from '@playwright/test';
import { submitAtPrompt } from './repl-helpers.js';

/* wasm-jit-plan P4: the Compile engine, end to end in the page.
 *
 * Compile runs the buffer through the browser JIT module (jit-worker.js,
 * turmeric-jit.wasm): the program is compiled to C and run on MIR's
 * interpreter, so inline C works where the interpreter refuses it.  The
 * module is loaded on first use, so the first Compile run also covers the
 * lazy boot, and the shared stdlib package (stdlib.data) both modules read.
 *
 * Needs turmeric-jit.{js,wasm} in web/public (`tur run wasm` builds them). */

async function gotoTry(page) {
    await page.goto('/try/');
    await expect(page.locator('#wasm-status-text')).toHaveText('Ready', { timeout: 30_000 });
    await page.waitForFunction(() => !!window._turiEditor && !!window.turmericApp);
}

async function pickEngine(page, engine) {
    await page.click('#lang-btn');
    await page.locator(`#lang-engines input[value="${engine}"]`).check();
    await page.keyboard.press('Escape');
}

async function run(page, code) {
    await page.evaluate((c) => window._turiEditor.setValue(c), code);
    await page.evaluate(() => window.turmericApp.runCode());
}

const consoleEl = (page) => page.locator('#console');

async function clearConsole(page) {
    await page.evaluate(() => window.turmericApp.clearConsole());
}

test.describe('The Compile engine', () => {
    test('runs the inline-C example, picked from the menus', async ({ page }) => {
        await gotoTry(page);
        await expect(page.locator('#engine-chip')).toBeHidden();

        await pickEngine(page, 'compile');
        await expect(page.locator('#engine-chip')).toBeVisible();

        await page.click('#examples-btn');
        await page.click('#examples-menu [data-example="inline-c"]');
        await clearConsole(page);
        await page.click('#run-btn');

        // First Compile: the module boots, then compiles and runs the program.
        await expect(consoleEl(page)).toContainText('5.20216', { timeout: 60_000 });
        await expect(consoleEl(page)).toContainText('8');
        await expect(consoleEl(page).locator('.console-engine')).toContainText('compiled');
        await expect(consoleEl(page)).not.toContainText('Error');

        // The choice survives a reload.
        await page.reload();
        await expect(page.locator('#wasm-status-text')).toHaveText('Ready', { timeout: 30_000 });
        await expect(page.locator('#engine-chip')).toBeVisible();
    });

    test('the interpreter refuses inline C and points at Compile', async ({ page }) => {
        await gotoTry(page);
        await pickEngine(page, 'interpret');
        await page.click('#examples-btn');
        await page.click('#examples-menu [data-example="inline-c"]');
        await clearConsole(page);
        await page.click('#run-btn');
        await expect(consoleEl(page)).toContainText('inline-C not supported in interpreter mode');
        await expect(consoleEl(page)).toContainText('pick Compile');
    });

    test('a stack overflow is reported, and the next run gets a fresh module', async ({ page }) => {
        await gotoTry(page);
        await pickEngine(page, 'compile');
        await clearConsole(page);
        await run(page, `(defn deep [n : int] : int (if (= n 0) 0 (+ 1 (deep (- n 1)))))
(println (deep 100000))`);
        await expect(consoleEl(page)).toContainText('stack overflow: recursion too deep', { timeout: 60_000 });
        await expect(consoleEl(page)).toContainText('restarts on the next Run');

        await clearConsole(page);
        await run(page, '(println (* 6 7))');
        await expect(consoleEl(page)).toContainText('42', { timeout: 60_000 });
        await expect(consoleEl(page).locator('.console-engine')).toContainText('compiled');
    });

    test('a linear-stack overflow is reported as one, not as a memory fault', async ({ page }) => {
        // A 1 MiB local reaches the bottom of the module's 16 MiB stack at
        // about depth 16, long before the JS stack limit (a 64 KiB one, at
        // depth 250, still met Chromium's worker limit first).  Unchecked, that
        // wrote below the stack and trapped as "memory access out of bounds";
        // the link's STACK_OVERFLOW_CHECK=2 makes it an abort the worker
        // reports as the overflow (docs/reported/wasm-linear-stack-overflow-unchecked.md).
        await gotoTry(page);
        await pickEngine(page, 'compile');
        await clearConsole(page);
        await run(page, '(defn deep [n : int] : int ```c volatile char buf[1048576]; '
                      + 'buf[n % 1048576] = (char)n; if (n == 0) return 0; '
                      + 'return deep(n - 1) + (buf[n % 1048576] & 1); ```)\n'
                      + '(println (deep 100000))');
        await expect(consoleEl(page)).toContainText('stack overflow: recursion too deep', { timeout: 60_000 });
        await expect(consoleEl(page)).not.toContainText('memory access out of bounds');
        await expect(consoleEl(page)).not.toContainText('Aborted(');

        await clearConsole(page);
        await run(page, '(println (* 6 7))');
        await expect(consoleEl(page)).toContainText('42', { timeout: 60_000 });
    });

    test('an inline-C defn at the prompt compiles under Compile (P6)', async ({ page }) => {
        // wasm-jit-plan P6: with Compile picked, a prompt turn goes to the
        // Compile engine's own session, where an inline-C defn the interpreter
        // cannot run (a loop) compiles on its first call -- C1 of
        // aot-compiled-repl-plan, on MIR's interpreter.
        const defn = '(defn c-mix [a : int b : int] : int ```c int64_t t = a; '
                   + 'for (int i = 0; i < 2; i++) t += i; return t * 31 + b; ```)';
        await gotoTry(page);

        // The interpreter's session refuses the body and points at Compile.
        await pickEngine(page, 'interpret');
        await submitAtPrompt(page, defn);
        await submitAtPrompt(page, '(c-mix 3 4)');
        await expect(consoleEl(page)).toContainText('pick Compile', { timeout: 30_000 });

        await pickEngine(page, 'compile');
        await clearConsole(page);
        await submitAtPrompt(page, defn);
        await expect(consoleEl(page)).toContainText('#<fn c-mix>', { timeout: 60_000 });
        await submitAtPrompt(page, '(c-mix 3 4)');
        await expect(consoleEl(page).locator('.console-result').last()).toHaveText('128', { timeout: 60_000 });
        // The session carries on: the compiled defn is a native now.
        await submitAtPrompt(page, '(+ (c-mix 1 1) 7)');
        await expect(consoleEl(page).locator('.console-result').last()).toHaveText('70');

        // A Compile Run is a program of its own and leaves the prompt's
        // session alone (one that falls back to the interpreter does too:
        // turi_wasm_run_program sets the session aside).
        await run(page, '(println (+ 40 2))');
        await expect(consoleEl(page)).toContainText('42', { timeout: 60_000 });
        await submitAtPrompt(page, '(c-mix 3 4)');
        await expect(consoleEl(page).locator('.console-result').last()).toHaveText('128');
    });

    test('compiled prompt turns keep globals, print structures, survive a panic (C2)', async ({ page }) => {
        // aot-compiled-repl-plan C2 on the page: with "Compile prompt turns
        // too" ticked, every prompt turn runs as a compiled program -- the
        // session's definitions plus the turn -- in the JIT module.
        await gotoTry(page);
        await pickEngine(page, 'compile');
        await page.click('#lang-btn');
        await expect(page.locator('#engine-turns-row')).toBeVisible();
        await page.locator('#engine-turns').check();
        await page.keyboard.press('Escape');
        await clearConsole(page);

        const last = () => consoleEl(page).locator('.console-result').last();
        await submitAtPrompt(page, '(def ^mut n 5)');
        await expect(last()).toHaveText('5', { timeout: 60_000 });
        await submitAtPrompt(page, '(defn bump [] : int (do (set! n (+ n 1)) n))');
        await expect(last()).toHaveText('#<fn bump>', { timeout: 60_000 });
        await submitAtPrompt(page, '(bump)');
        await expect(last()).toHaveText('6', { timeout: 60_000 });
        // A slot keeps the global across turns: the next bump sees 6.
        await submitAtPrompt(page, '(bump)');
        await expect(last()).toHaveText('7', { timeout: 60_000 });
        // Results print through Show, or their constructors.
        await submitAtPrompt(page, '[1 2 3]');
        await expect(last()).toHaveText('[1 2 3]', { timeout: 60_000 });
        await submitAtPrompt(page, '(defstruct Pt [x : int y : int])');
        await submitAtPrompt(page, '(Pt 1 2)');
        await expect(last()).toHaveText('Pt { x = 1, y = 2 }', { timeout: 60_000 });
        // A panic ends the turn, not the session.
        await submitAtPrompt(page, '(panic "boom")');
        await expect(consoleEl(page)).toContainText('boom', { timeout: 60_000 });
        await submitAtPrompt(page, 'n');
        await expect(last()).toHaveText('7', { timeout: 60_000 });

        // Unticking starts the prompt's session afresh.
        await page.click('#lang-btn');
        await page.locator('#engine-turns').uncheck();
        await page.keyboard.press('Escape');
        await expect(consoleEl(page)).toContainText('starts afresh');
    });

    test('Stop ends a runaway compiled program and keeps the session', async ({ page }) => {
        await gotoTry(page);
        await pickEngine(page, 'interpret');
        // A definition in the interpreter's session, which a Compile Stop must
        // not take with it -- the two engines have separate workers.
        await submitAtPrompt(page, '(def kept 41)');

        await pickEngine(page, 'compile');
        await clearConsole(page);
        // The exit is never taken; it dates from when MIR's inliner asserted
        // on a function without one
        // (docs/archive/mir-inliner-asserts-on-function-with-no-return.md).
        // Clicked, not awaited through runCode(): a runaway run settles only
        // when something stops it.
        await page.evaluate((c) => window._turiEditor.setValue(c),
            `(defn spin [n : int] : int (if (< n 0) n (spin (+ n 1))))
(println (spin 0))`);
        await page.click('#run-btn');
        await expect(page.locator('#stop-btn')).toBeVisible({ timeout: 60_000 });
        await page.click('#stop-btn');
        await expect(consoleEl(page)).toContainText('Stopped');
        await expect(page.locator('#stop-btn')).toBeHidden();

        await pickEngine(page, 'interpret');
        await clearConsole(page);
        await submitAtPrompt(page, '(+ kept 1)');
        await expect(consoleEl(page)).toContainText('42');
    });
});
