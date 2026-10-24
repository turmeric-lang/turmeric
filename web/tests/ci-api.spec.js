import { test, expect } from '@playwright/test';
import { envKey, timingsFromRows } from '../ci-aggregate.js';

// /api/ci/* against a real D1: the local one the Playwright dev server runs
// (tests/ci-d1-setup.mjs wipes and migrates it, vite.config.js binds it, and
// playwright.config.js picks the report token). The page tests stub these
// endpoints; these hold the Worker to the shapes the stubs serve.
//
// The store is shared by every test in the run, so each test reports under an
// environment (cc) and run id of its own and asserts only on those.

const TOKEN = process.env.TUR_CI_TEST_TOKEN;
const auth = { Authorization: `Bearer ${TOKEN}` };

let seq = 0;
function uniq() {
  seq += 1;
  return `${process.pid}-${Date.now()}-${seq}`;
}

// Suite rows in one fresh environment. `perDay` rows per suite per UTC day,
// for `days` days ending today -- odd and even counts, so a median of the
// middle one and of the middle two are both exercised.
function suiteRows(cc, { days = 1, perDay = 3, suites = ['alpha', 'beta'] } = {}) {
  const today = Math.floor(Date.now() / 1000 / 86400) * 86400;
  const rows = [];
  let run = 0;
  for (let d = days - 1; d >= 0; d--) {
    const n = perDay + (d % 2);
    for (let i = 0; i < n; i++) {
      run += 1;
      for (const [k, suite] of suites.entries()) {
        rows.push({
          run_id: `${cc}-r${run}`, run_attempt: 1, branch: 'main',
          build_type: 'Debug', os: 'Linux', cc, nproc: 4, jit: true, sanitize: true,
          suite, shard_index: null, shard_total: null,
          ts: today - d * 86400 + 3600 + i * 600,
          sha: `${String(run % 10).repeat(7)}${'f'.repeat(33)}`,
          duration_ms: 1000 * (k + 1) + ((run * 37) % 500) + 0.5,
          status: i === 1 && k === 0 ? 'fail' : 'pass',
          skip_reason: null,
          passed: 10 + run, failed: 0, skipped: 1, discovered: null,
        });
      }
    }
  }
  return rows;
}

async function post(request, body, headers = auth) {
  return request.post('/api/ci/report', { headers, data: body });
}

async function meta(request) {
  const res = await request.get('/api/ci/meta');
  expect(res.status()).toBe(200);
  return res.json();
}

test.describe('/api/ci D1 store', () => {
  test.skip(!TOKEN, 'runs only under playwright.config.js, which sets the test token');

  test('a report round-trips into the meta and the timings', async ({ request }) => {
    const cc = `TestCC-${uniq()}`;
    const rows = suiteRows(cc);
    const res = await post(request, { timings: rows });
    expect(res.status()).toBe(200);
    const out = await res.json();
    expect(out.suite_rows).toBe(rows.length);
    expect(out.ingest).toBeGreaterThan(0);

    const m = await meta(request);
    expect(m.version).toBeGreaterThanOrEqual(out.ingest);
    const key = envKey(rows[0]);
    const env = m.envs.find((e) => e.key === key);
    expect(env).toBeTruthy();
    expect(env.breadth).toBe(2);
    expect(env.label).toContain(cc.split('-')[0]);

    // Followed through the redirect if another test has reported since.
    const t = await (await request.get(
      `/api/ci/timings?env=${encodeURIComponent(key)}&range=7d&v=${m.version}`)).json();
    expect(t).toEqual(timingsFromRows(rows, key, '7d'));
    expect(t.mode).toBe('run');
    expect(t.cells).toHaveLength(rows.length);
  });

  test('the same report twice overwrites rather than doubles', async ({ request }) => {
    const cc = `TestCC-${uniq()}`;
    const rows = suiteRows(cc);
    expect((await post(request, { timings: rows })).status()).toBe(200);
    const again = rows.map((r) => ({ ...r, duration_ms: r.duration_ms + 1 }));
    expect((await post(request, { timings: again })).status()).toBe(200);

    const key = envKey(rows[0]);
    const t = await (await request.get(
      `/api/ci/timings?env=${encodeURIComponent(key)}&range=7d`)).json();
    expect(t.cells).toHaveLength(rows.length);
    // The second report's values won.
    expect(t).toEqual(timingsFromRows(again, key, '7d'));
  });

  test('daily medians from SQL equal the JS rule the page stubs with', async ({ request }) => {
    const cc = `TestCC-${uniq()}`;
    const rows = suiteRows(cc, { days: 12, perDay: 3 });
    expect((await post(request, { timings: rows })).status()).toBe(200);
    const key = envKey(rows[0]);
    for (const range of ['30d', 'all']) {
      const t = await (await request.get(
        `/api/ci/timings?env=${encodeURIComponent(key)}&range=${range}`)).json();
      expect(t.mode).toBe('day');
      expect(t).toEqual(timingsFromRows(rows, key, range));
    }
  });

  test('the report endpoint refuses a bad token, a bad row and an empty body', async ({ request }) => {
    const rows = suiteRows(`TestCC-${uniq()}`);
    expect((await post(request, { timings: rows }, {})).status()).toBe(401);
    expect((await post(request, { timings: rows }, { Authorization: 'Bearer nope' })).status()).toBe(401);

    const bad = rows.map((r, i) => (i === 1 ? { ...r, suite: undefined } : r));
    const res = await post(request, { timings: bad });
    expect(res.status()).toBe(400);
    expect((await res.json()).error).toContain('timings[1]: field "suite"');

    expect((await post(request, {})).status()).toBe(400);
    expect((await post(request, { nonsense: [] })).status()).toBe(400);
    expect((await request.get('/api/ci/report')).status()).toBe(405);
  });

  test('a rejected report writes nothing', async ({ request }) => {
    const cc = `TestCC-${uniq()}`;
    const rows = suiteRows(cc);
    const before = (await meta(request)).envs.some((e) => e.key === envKey(rows[0]));
    expect(before).toBe(false);
    const res = await post(request, { timings: rows, loc: [{ run_id: 'x' }] });
    expect(res.status()).toBe(400);
    expect((await meta(request)).envs.some((e) => e.key === envKey(rows[0]))).toBe(false);
  });

  test('reads are versioned: stale redirects, current is immutable', async ({ request }) => {
    expect((await post(request, { timings: suiteRows(`TestCC-${uniq()}`) })).status()).toBe(200);
    const { version } = await meta(request);

    const stale = await request.get(`/api/ci/loc?v=${version - 1}`, { maxRedirects: 0 });
    expect(stale.status()).toBe(302);
    expect(stale.headers().location).toMatch(/^\/api\/ci\/loc\?v=\d+$/);
    expect(Number(stale.headers().location.split('=')[1])).toBeGreaterThanOrEqual(version);

    const bare = await request.get('/api/ci/docs', { maxRedirects: 0 });
    expect(bare.status()).toBe(302);

    const cur = await request.get(stale.headers().location, { maxRedirects: 0 });
    expect([200, 302]).toContain(cur.status());   // 302 only if a report raced in
    if (cur.status() === 200) {
      expect(cur.headers()['cache-control']).toContain('immutable');
    }
    const m = await request.get('/api/ci/meta');
    expect(m.headers()['cache-control']).not.toContain('immutable');
  });

  test('loc, docs and bench round-trip, with the bulky fields only on the newest row', async ({ request }) => {
    const id = uniq();
    const now = Math.floor(Date.now() / 1000);
    const loc = [0, 1].map((i) => ({
      run_id: `loc-${id}-${i}`, run_attempt: 1, ts: now - 100 + i, sha: `${i}`.repeat(40),
      code_lines: 100 + i, test_lines: 50, code_by_lang: { c: 90 + i }, test_by_lang: { c: 50 },
    }));
    const docs = [0, 1].map((i) => ({
      run_id: `docs-${id}-${i}`, run_attempt: 1, ts: now - 100 + i, sha: `${i}`.repeat(40),
      open_plans: 3, open_reports: 4 + i, spice_names: ['ansi', `s${i}`],
    }));
    const bench = [{
      kind: 'bench', run_id: `bench-${id}`, run_attempt: 1, ts: now,
      suite: `bench:${id}/jit/run_ms`, program: 'p.tur', engine: 'jit', metric: 'run_ms',
      value: 12.5, unit: 'ms', cv: 0.01, noisy: true, reps: 3, status: 'ok',
      os: 'Linux', nproc: 4,
    }];
    expect((await post(request, { loc, docs, bench })).status()).toBe(200);

    const gotLoc = (await (await request.get('/api/ci/loc')).json()).rows;
    const newest = gotLoc[gotLoc.length - 1];
    expect(newest.code_by_lang).toEqual({ c: 91 });
    expect(gotLoc.filter((r) => r.code_by_lang)).toHaveLength(1);

    const gotDocs = (await (await request.get('/api/ci/docs')).json()).rows;
    expect(gotDocs[gotDocs.length - 1].spice_names).toEqual(['ansi', 's1']);
    expect(gotDocs.filter((r) => r.spice_names)).toHaveLength(1);

    const nd = await request.get('/api/ci/bench?format=ndjson&days=1');
    expect(nd.headers()['content-type']).toContain('ndjson');
    expect(nd.headers()['cache-control']).toBe('no-store');
    const row = (await nd.text()).trim().split('\n').map((l) => JSON.parse(l))
      .find((r) => r.suite === bench[0].suite);
    // The shape bench-rows.py's --history reader takes.
    expect(row).toMatchObject({ kind: 'bench', noisy: true, value: 12.5, engine: 'jit' });
  });

  test('test counts come back carried forward', async ({ request }) => {
    expect((await post(request, { timings: suiteRows(`TestCC-${uniq()}`) })).status()).toBe(200);
    const c = await (await request.get('/api/ci/counts')).json();
    expect(c.timeline.length).toBeGreaterThan(0);
    const [ts, sha, total] = c.timeline[c.timeline.length - 1];
    expect(ts).toBeGreaterThan(0);
    expect(sha).toMatch(/^[0-9a-f]{12}$/);
    expect(total).toBeGreaterThan(0);
    expect(c.current.total).toBe(total);
  });
});
