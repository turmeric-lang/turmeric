/* The prompt's own intelligence -- completion, hover, meta-commands, and the
 * keys they contend for. (editor-intelligence-follow-through-plan, track W)
 *
 * Everything the language server knows used to reach the Monaco editor and
 * nothing reached the `turi>` prompt, which is where a beginner types the
 * most. These are the claims that made the prompt worth rebuilding.
 */
import { test, expect } from '@playwright/test';
import { typeAtPrompt, submitAtPrompt } from './repl-helpers.js';

async function waitForReady(page) {
    page.on('pageerror', err => console.log(`PAGE ERROR: ${err.stack}`));
    await expect(page.locator('#wasm-status-text')).toHaveText('Ready', { timeout: 30_000 });
}

/** The prompt only talks to the language server once it has booted, and the
 *  server boots lazily on first focus. */
async function waitForLsp(page) {
    await page.locator('#repl-input').click();
    await page.waitForFunction(
        () => window._turiLsp && window._turiLsp.isAvailable(),
        null, { timeout: 60_000 });
}

test.describe('Try Turmeric prompt intelligence', () => {

    // -----------------------------------------------------------------------
    // W4: meta-commands
    // -----------------------------------------------------------------------

    test('a colon at the start of a line offers the REPL vocabulary', async ({ page }) => {
        await page.goto('/try/');
        await waitForReady(page);

        const labels = await page.evaluate(() => window._turiRepl.metaCompletions(':'));
        expect(labels.join(' ')).toContain(':help');
        expect(labels.join(' ')).toContain(':doc <sym>');
        expect(labels.join(' ')).toContain(':explain [code]');
    });

    test('a colon mid-expression offers nothing', async ({ page }) => {
        await page.goto('/try/');
        await waitForReady(page);

        // `:foo` inside a form is a keyword literal and a map key
        // (`#map{:name name}`); the REPL's vocabulary there would be noise.
        const inForm = await page.evaluate(() => window._turiRepl.metaCompletions('(f :'));
        expect(inForm).toEqual([]);

        const inMap = await page.evaluate(() => window._turiRepl.metaCompletions('#map{:'));
        expect(inMap).toEqual([]);
    });

    test(':doc takes a symbol, so completion after it offers symbols', async ({ page }) => {
        await page.goto('/try/');
        await waitForReady(page);

        const labels = await page.evaluate(() => window._turiRepl.metaCompletions(':doc co'));
        expect(labels.length).toBeGreaterThan(0);
        // Symbols, not commands. Not "nothing starting with a colon": `::`
        // (the ascription form) is a documented symbol, and rightly offered
        // after `:doc :`.
        expect(labels.some(l => /^:[a-z]/.test(l))).toBe(false);
        // And narrowed by what is typed, before the list is capped.
        expect(labels.every(l => l.toLowerCase().startsWith('co'))).toBe(true);
        expect(labels).toContain('cons');
    });

    test(':help is generated from the command table, aligned', async ({ page }) => {
        await page.goto('/try/');
        await waitForReady(page);

        await submitAtPrompt(page, ':help');
        const text = await page.locator('#console').textContent();
        // Every command in the table has a row, and the summaries line up --
        // which is the property a hand-written block loses the first time
        // someone adds a command.
        for (const name of [':help', ':type', ':doc', ':docs', ':reset', ':explain']) {
            expect(text).toContain(name);
        }
    });

    // -----------------------------------------------------------------------
    // W1: the keys the completion list contends for
    // -----------------------------------------------------------------------

    test('Escape dismisses the prompt popup without closing the docs pane', async ({ page }) => {
        await page.goto('/try/');
        await waitForReady(page);

        // The page has document-level Escape handlers. Dismissing a popup at
        // the prompt must not also tear down whatever else is open -- c2mp
        // verified the same shape by opening the list over a running program.
        await submitAtPrompt(page, ':docs');
        await expect(page.locator('#docs-overlay')).toBeVisible({ timeout: 20_000 });

        // Not typeAtPrompt: it clicks #repl-input, and the docs overlay is a
        // full-window backdrop over it, so the click is intercepted and
        // Playwright waits out the test's 60 s for it to become clickable.
        // Submitting `:docs` left the keyboard focus in the prompt, which is
        // the state this test is about.
        await page.evaluate(() => { window._turiRepl.focus(); window._turiRepl.setValue('(co'); });
        await page.keyboard.press('Escape');
        await page.waitForTimeout(300);

        await expect(page.locator('#docs-overlay')).toBeVisible();
    });

    test('Enter submits when no suggestion list is open', async ({ page }) => {
        await page.goto('/try/');
        await waitForReady(page);

        await submitAtPrompt(page, '(+ 40 2)');
        await expect(page.locator('#console')).toContainText('42', { timeout: 10_000 });
        expect(await page.evaluate(() => window._turiRepl.value())).toBe('');
    });

    // -----------------------------------------------------------------------
    // W2: the prompt may only offer what the session can evaluate
    // -----------------------------------------------------------------------

    test('an editor-only defn is not offered at the prompt until it is Run', async ({ page }) => {
        await page.goto('/try/');
        await waitForReady(page);
        await waitForLsp(page);

        const NAME = 'zzz-editor-only-fn';
        await page.evaluate((n) => window._turiEditor.setValue(
            `(defn ${n} [x : int] : int x)`), NAME);
        // Give the editor's own analysis time to see it, so a miss below is
        // about the prompt's document and not about timing.
        await page.waitForTimeout(1500);

        const before = await page.evaluate((n) =>
            window._turiRepl.completions('(' + n.slice(0, 6)), NAME);
        expect(before.includes(NAME)).toBe(false);

        // A Run is how a tab's definitions become callable at the prompt, so
        // it is also how they become offerable there.
        await page.click('#run-btn');
        await page.waitForTimeout(2000);

        await expect.poll(async () => {
            const after = await page.evaluate((n) =>
                window._turiRepl.completions('(' + n.slice(0, 6)), NAME);
            return after.includes(NAME);
        }, { timeout: 20_000 }).toBe(true);
    });

    test('a name defined at the prompt is offered afterwards', async ({ page }) => {
        await page.goto('/try/');
        await waitForReady(page);
        await waitForLsp(page);

        await submitAtPrompt(page, '(defn zzz-prompt-fn [x : int] : int x)');
        await page.waitForTimeout(1000);

        await expect.poll(async () =>
            (await page.evaluate(() => window._turiRepl.completions('(zzz-pro')))
                .includes('zzz-prompt-fn'),
            { timeout: 20_000 }).toBe(true);
    });

    test('a line that failed to evaluate defines nothing the prompt offers', async ({ page }) => {
        await page.goto('/try/');
        await waitForReady(page);
        await waitForLsp(page);

        // A type error, which the evaluator refuses. An unknown callee
        // (`(nope x)`) is not one: it warns TUR-W0040 and defines the fn,
        // to be dispatched at run time.
        await submitAtPrompt(page, '(defn zzz-broken-fn [x : int] : int (+ x "a"))');
        await page.waitForTimeout(1500);

        const session = await page.evaluate(() => window._turiRepl.sessionSource());
        expect(session).not.toContain('zzz-broken-fn');
    });

    // -----------------------------------------------------------------------
    // W3: hover in the transcript
    // -----------------------------------------------------------------------

    test('hover answers on an echoed prompt line and stays silent on program stdout',
         async ({ page }) => {
        await page.goto('/try/');
        await waitForReady(page);

        // An echoed `turi> ...` line: `println` there IS a symbol reference.
        await submitAtPrompt(page, '(println "println")');
        await expect(page.locator('#console')).toContainText('println', { timeout: 10_000 });

        const echo = page.locator('#console .console-prompt').first();
        await expect(echo).toBeVisible();

        // And a line the program printed: text that happens to spell a symbol
        // is not a reference to it. A card there would be a lie about what
        // that text is.
        const stdout = page.locator('#console .console-output').last();
        await stdout.hover();
        await page.waitForTimeout(500);
        await expect(page.locator('#console-hover-card')).toBeHidden();
    });
});
