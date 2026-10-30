// The JSPI spike's driver (raylib-web-playground-plan prerequisite 3; the
// depth report).  Copy to web/tests/ and the harness to web/public/jspi-harness/
// with the modules under <JSPI_MOD>/ to run it.  Drives
// web/public/jspi-harness/index.html against a module directory under
// web/public/jspi-harness/<JSPI_MOD>/:
//   JSPI_PROGS=a.tur,b.tur  run each program JSPI_REPEAT times (one boot)
//   JSPI_DEPTHS=300,600     a non-tail recursion per depth, fresh page each
import { test } from '@playwright/test';
import fs from 'node:fs';
import path from 'node:path';

const dir = '/jspi-harness/' + (process.env.JSPI_MOD || 'main');
const process_worker = !!process.env.JSPI_WORKER;

async function boot(page) {
    await page.goto('/jspi-harness/index.html');
    if (process.env.JSPI_WORKER) return page.evaluate((d) => window.bootWorker(d), dir);
    return page.evaluate((d) => window.boot(d), dir);
}

test('programs', async ({ page }) => {
    test.setTimeout(900_000);
    const progs = (process.env.JSPI_PROGS || '').split(',').filter(Boolean);
    if (!progs.length) return;
    const env = await boot(page);
    console.log(`ENV ${JSON.stringify(env)}`);
    const reps = Number(process.env.JSPI_REPEAT || 1);
    for (const f of progs) {
        const src = fs.readFileSync(f, 'utf8');
        for (let i = 0; i < reps; i++) {
            const r = await page.evaluate(([s, w]) => (w ? window.runInWorker(s) : window.runProgram(s)), [src, process_worker]);
            console.log(`PROG ${path.basename(f)} run${i}: answer=${r.answer} wall=${r.wall.toFixed(0)}ms out=${JSON.stringify(r.out.slice(0, 160))} err=${JSON.stringify(r.err.slice(0, 200))}`);
        }
    }
});

test('depth', async ({ page }) => {
    test.setTimeout(900_000);
    const depths = (process.env.JSPI_DEPTHS || '').split(',').filter(Boolean).map(Number);
    for (const d of depths) {
        await boot(page);
        const r = await page.evaluate(([s, w]) => (w ? window.runInWorker(s) : window.runProgram(s)),
            [`(defn deep [n : int] : int (if (= n 0) 0 (+ 1 (deep (- n 1)))))\n(println (deep ${d}))`, process_worker]);
        console.log(`DEPTH ${d}: ${r.out.trim() === String(d) ? 'ok' : r.answer + ' ' + r.err.slice(0, 120)}`);
    }
});
