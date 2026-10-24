import { test, expect } from '@playwright/test';
import {
  orderEnvs, summarizeEnvs, timingsFromRows, countsFromRows, slimCommitRows,
  LOC_BULKY, DOCS_BULKY,
} from '../ci-aggregate.js';

// /ci — the CI suite-timings dashboard.
//
// The page tests run on FIXTURES, served at /api/ci/* in the shapes the Worker
// sends. The stub builds each response with ci-aggregate.js -- the module the
// Worker itself builds them with -- so a stubbed response cannot drift from a
// real one. The Worker's D1 side has its own round-trip tests in
// ci-api.spec.js; the `request` tests below still cover the old branch proxy
// while it is kept.

test.describe('CI metrics dashboard', () => {
  test.beforeEach(async ({ page }) => { await stubApi(page); });

  // Timestamps are relative to now, a few hours apart, so the rows land inside
  // every range the page offers -- including the 7-day default and `1d`.
  function locFixture() {
    const now = Math.floor(Date.now() / 1000);
    const rows = [
      ['aaaaaaa1111111111111111111111111111aaaa1', 405000, 167000, 13300, 4140, 1550000],
      ['bbbbbbb2222222222222222222222222222bbbb2', 406200, 167400, 13300, 4140, 1551000],
      ['ccccccc3333333333333333333333333333cccc3', 406100, 168900, 13500, 4140, 1556000],
      ['ddddddd4444444444444444444444444444dddd4', 407783, 168879, 13528, 4144, 1556131],
    ];
    return rows.map(([sha, code, tests, bench, example, generated], i) => ({
      ts: now - (rows.length - 1 - i) * 5 * 3600,
      sha,
      code_lines: code,
      test_lines: tests,
      bench_lines: bench,
      example_lines: example,
      generated_lines: generated,
    }));
  }

  function docsFixture() {
    const now = Math.floor(Date.now() / 1000);
    // The guide counts follow the plan columns; spices/spice_guides are null on
    // the first row on purpose -- published without the spices checkout, which
    // must read as "not measured", never as a drop to zero. The last two
    // columns are the running report totals (filed, archived), absent on the
    // first row as on every row published before collect-docs.py counted them;
    // filed - archived is that row's open_reports.
    const rows = [
      ['aaaaaaa1111111111111111111111111111aaaa1', 28, 28, 10, 15, 3, 0, 150, null, null, null],
      ['bbbbbbb2222222222222222222222222222bbbb2', 29, 26, 10, 15, 3, 1, 152, 19, 100, 74],
      ['ccccccc3333333333333333333333333333cccc3', 30, 27, 10, 15, 3, 2, 154, 20, 103, 76],
      ['ddddddd4444444444444444444444444444dddd4', 30, 29, 10, 14, 3, 3, 155, 21, 106, 77],
    ];
    return rows.map(([sha, open_plans, open_reports, active, held, v1, spices, tg, sg, rf, ra], i) => ({
      ts: now - (rows.length - 1 - i) * 5 * 3600,
      sha,
      open_plans,
      open_reports,
      active_plans: active,
      held_plans: held,
      v1_plans: v1,
      spices_plans: spices,
      turmeric_guides: tg,
      spice_guides: sg,
      spices: sg == null ? null : 40 + i,
      spice_names: sg == null ? null : ['ansi', 'json', 'raylib'],
      ...(rf == null ? {} : { reports_filed: rf, reports_archived: ra }),
    }));
  }

  // Nightly benchmark rows (performance-audit-plan WP6), in the shape
  // benchmarks/audit/bench-rows.py writes: three nights, two programs under
  // tur jit (one night of one of them noisy), and one cc binary size.
  function benchFixture() {
    const now = Math.floor(Date.now() / 1000);
    const base = { kind: 'bench', os: 'macOS', nproc: 3, status: 'ok', reps: 3, build_type: 'Release' };
    const rows = [];
    for (let i = 0; i < 3; i++) {
      const ts = now - (2 - i) * 6 * 3600;
      const sha = `${'abc'[i].repeat(7)}${'0'.repeat(33)}`;
      rows.push({ ...base, ts, sha, engine: 'jit', metric: 'run_ms', unit: 'ms',
                  program: 'benchmarks/audit/sized/cps-tramp-resume-8m.tur',
                  suite: 'bench:cps-tramp-resume-8m/jit/run_ms',
                  value: 1090 + 5 * i, cv: 0.01, noisy: false });
      rows.push({ ...base, ts, sha, engine: 'jit', metric: 'run_ms', unit: 'ms',
                  program: 'benchmarks/audit/sized/mut-map-reassign-1m.tur',
                  suite: 'bench:mut-map-reassign-1m/jit/run_ms',
                  value: 2250 - 10 * i, cv: i === 2 ? 0.14 : 0.01, noisy: i === 2 });
      rows.push({ ...base, ts, sha, engine: 'cc', metric: 'binary_bytes', unit: 'bytes',
                  program: 'benchmarks/audit/sized/mut-map-reassign-1m.tur',
                  suite: 'bench:mut-map-reassign-1m/cc/binary_bytes',
                  value: 48144 + i, cv: null, noisy: false });
    }
    return rows;
  }

  // Two environments, 26 suites, six runs an hour apart. The Linux runs
  // alternate between two runner CPUs, the newest on the EPYC; the macOS rows
  // predate the CPU field and carry none.
  const EPYC = 'AMD EPYC 7763 64-Core Processor';
  const XEON = 'Intel(R) Xeon(R) Platinum 8370C CPU @ 2.80GHz';
  function timingsFixture() {
    const now = Math.floor(Date.now() / 1000);
    const envs = [
      { os: 'Linux', cc: 'GNU-13.3.0', nproc: 4, jit: false },
      { os: 'macOS', cc: 'AppleClang-21.0.0', nproc: 3, jit: false },
    ];
    const suites = [
      'tur_tests', 'turi_fixture_tests', 'tur_jit_smoke', 'tur_spice_resolver_tests',
      ...Array.from({ length: 22 }, (_, n) => `quick_${n}`),
    ];
    const rows = [];
    for (const [e, env] of envs.entries()) {
      for (let i = 0; i < 6; i++) {
        suites.forEach((suite, k) => {
          // Run i=0 is the newest: tur_tests grows by two tests a run (and
          // counts three more on macOS), so the Test counts tab has a trend
          // and an environment switch has something to change.
          const counted = suite === 'tur_tests'
            ? { discovered: 1400 - i * 2 + e * 3, passed: 1390 - i * 2 + e * 3, failed: 0, skipped: 10 }
            : suite === 'turi_fixture_tests'
              ? { discovered: 3600, passed: 2700, failed: 0, skipped: 900 }
              // A census with no `discovered`: counted as passed + failed.
              : suite === 'tur_spice_resolver_tests'
                ? { passed: 80, failed: 0 }
                : {};
          rows.push({
            branch: 'main', build_type: 'Debug', ...env, ts: now - i * 3600,
            sha: 'a'.repeat(40), run_id: String(i), suite, status: 'pass',
            // Differs per environment so switching it visibly redraws the table.
            duration_ms: 1000 * (suites.length - k) * (e + 1) + i * 10 * (e + 1),
            shard_index: null, shard_total: null, skip_reason: null, ...counted,
            ...(e === 0 ? { cpu: i % 2 ? XEON : EPYC } : {}),
          });
        });
      }
    }
    return rows;
  }

  // Serve /api/ci/* from fixtures. `fail` maps an endpoint name to a status to
  // answer it with instead. A later call replaces an earlier one: Playwright
  // tries the most recently registered route first.
  async function stubApi(page, {
    timings = timingsFixture(), loc = locFixture(), docs = docsFixture(),
    bench = benchFixture(), fail = {},
  } = {}) {
    const latest = timings.reduce((m, r) => Math.max(m, r.ts), 0);
    await page.route('**/api/ci/**', (route) => {
      const url = new URL(route.request().url());
      const name = url.pathname.split('/').pop();
      if (fail[name]) {
        return route.fulfill({
          status: fail[name],
          contentType: 'application/json',
          body: JSON.stringify({ error: `stubbed ${name} failure` }),
        });
      }
      const p = url.searchParams;
      const body = {
        meta: () => ({
          version: 1, at: latest, latest_ts: latest,
          envs: orderEnvs(summarizeEnvs(timings)),
        }),
        timings: () => timingsFromRows(timings, p.get('env'), p.get('range')),
        counts: () => ({ ...countsFromRows(timings), latest_ts: latest }),
        loc: () => ({ rows: slimCommitRows(loc, LOC_BULKY) }),
        docs: () => ({ rows: slimCommitRows(docs, DOCS_BULKY) }),
        bench: () => ({ rows: bench }),
      }[name];
      if (!body) return route.fulfill({ status: 404, body: '{}' });
      return route.fulfill({
        status: 200, contentType: 'application/json', body: JSON.stringify(body()),
      });
    });
  }

  test('worker proxies the timings NDJSON', async ({ request }) => {
    const res = await request.get('/api/ci-timings');
    expect(res.status()).toBe(200);
    expect(res.headers()['content-type']).toContain('ndjson');
    expect(res.headers()['x-metrics-year']).toMatch(/^\d{4}$/);

    const lines = (await res.text()).trim().split('\n');
    expect(lines.length).toBeGreaterThan(100);

    const row = JSON.parse(lines[0]);
    for (const key of ['suite', 'duration_ms', 'ts', 'os', 'cc', 'status', 'sha']) {
      expect(row).toHaveProperty(key);
    }
  });

  // repo-loc-<year>.jsonl does not exist until the first push to main after the
  // panel shipped, and the route then legitimately 502s. So this asserts the
  // schema when there is data and SKIPS loudly when there is not -- rather than
  // passing either way, which would make the test worthless once it does exist.
  test('worker proxies the recent timings window', async ({ request }) => {
    const res = await request.get('/api/ci-timings?window=recent');
    test.skip(res.status() === 502,
      'no suite-timings-recent.jsonl on ci-metrics yet (first publish is pending)');

    expect(res.status()).toBe(200);
    expect(res.headers()['content-type']).toContain('ndjson');
    const lines = (await res.text()).trim().split('\n');
    const row = JSON.parse(lines[lines.length - 1]);
    for (const key of ['ts', 'sha', 'suite', 'duration_ms', 'status', 'os']) {
      expect(row).toHaveProperty(key);
    }
  });

  test('worker proxies the line-count NDJSON', async ({ request }) => {
    const res = await request.get('/api/ci-loc');
    test.skip(res.status() === 502,
      'no repo-loc-<year>.jsonl on ci-metrics yet (first publish is pending)');

    expect(res.status()).toBe(200);
    expect(res.headers()['content-type']).toContain('ndjson');
    expect(res.headers()['x-metrics-year']).toMatch(/^\d{4}$/);

    const lines = (await res.text()).trim().split('\n');
    const row = JSON.parse(lines[lines.length - 1]);
    for (const key of ['ts', 'sha', 'code_lines', 'test_lines', 'generated_lines']) {
      expect(row).toHaveProperty(key);
    }
    // The split is the point of the panel: product and test are both real and
    // the committed machine output is quarantined from both, not folded in.
    expect(row.code_lines).toBeGreaterThan(10_000);
    expect(row.test_lines).toBeGreaterThan(10_000);
    expect(row.generated_lines).toBeGreaterThan(row.code_lines);
  });

  test('worker proxies the docs-count NDJSON', async ({ request }) => {
    const res = await request.get('/api/ci-docs');
    test.skip(res.status() === 502,
      'no docs-counts-<year>.jsonl on ci-metrics yet (first publish is pending)');

    expect(res.status()).toBe(200);
    expect(res.headers()['content-type']).toContain('ndjson');
    expect(res.headers()['x-metrics-year']).toMatch(/^\d{4}$/);

    const lines = (await res.text()).trim().split('\n');
    const row = JSON.parse(lines[lines.length - 1]);
    for (const key of ['ts', 'sha', 'open_plans', 'open_reports', 'active_plans', 'held_plans', 'v1_plans']) {
      expect(row).toHaveProperty(key);
    }
    expect(row.open_plans).toBeGreaterThanOrEqual(0);
    expect(row.open_reports).toBeGreaterThanOrEqual(0);
    // The total open_plans is the recursive count under docs/upcoming, which
    // is exactly the sum of the per-bucket breakdowns the collector emits
    // (stray + held + v1 + spices + godot) -- but only once a row published AFTER a
    // new bucket lands carries that bucket.  A row published before the
    // spices_plans field existed has no spices_plans, so its three buckets
    // sum to less than the total (the spices plans are in the tree but
    // uncategorized in that row).  Assert strict equality when every bucket
    // is present, and the weaker >= while the live row is in transition, so a
    // stale row does not redden a PR that did not touch the collector.
    const PLAN_BUCKETS = ['active_plans', 'held_plans', 'v1_plans', 'spices_plans', 'godot_plans'];
    const known = PLAN_BUCKETS.filter((k) => k in row);
    const sum = known.reduce((s, k) => s + row[k], 0);
    if (known.length === PLAN_BUCKETS.length) {
      expect(row.open_plans).toBe(sum);
    } else {
      expect(row.open_plans).toBeGreaterThanOrEqual(sum);
    }
  });

  test('page renders every section without console errors', async ({ page }) => {
    const errors = [];
    page.on('pageerror', (e) => errors.push(e.message));
    page.on('console', (m) => { if (m.type() === 'error') errors.push(m.text()); });

    await page.goto('/ci');

    await expect(page.locator('#ci-body')).toBeVisible({ timeout: 20_000 });
    await expect(page.locator('#ci-state')).toBeHidden();

    // Shared chrome still renders on this page.
    await expect(page.locator('site-nav nav')).toBeVisible();
    await expect(page.locator('site-footer a[href="/ci"]')).toHaveCount(1);

    // Six tabs, Tests active by default.
    await expect(page.locator('.ci-tab')).toHaveCount(6);
    await expect(page.locator('#ci-tab-tests')).toHaveClass(/is-active/);

    // Four stat tiles, and a provenance line naming the latest commit.
    await expect(page.locator('#ci-tiles .ci-tile')).toHaveCount(4);
    await expect(page.locator('#ci-provenance .mono').first()).toHaveText(/^[0-9a-f]{7}$/);

    // The chart drew real geometry, and the legend accounts for every series.
    const lines = page.locator('#ci-chart .ci-series-line');
    await expect(lines.first()).toBeVisible();
    const seriesCount = await lines.count();
    expect(seriesCount).toBeGreaterThan(0);
    expect(seriesCount).toBeLessThanOrEqual(5);
    await expect(page.locator('#ci-legend .ci-legend-item')).toHaveCount(seriesCount);

    // Tests per run and per suite are on the Test counts tab.
    // Counted, not toBeVisible: the fixture's spice count never changes, and a
    // flat line's zero-height box reads as hidden.
    await page.locator('#ci-tab-counts').click();
    await expect(page.locator('#ci-counts-chart .ci-series-line')).toHaveCount(1);

    // The line-count chart is on the Code tab. Switch to it and verify.
    await page.locator('#ci-tab-code').click();
    await expect(page.locator('#ci-loc-chart .ci-series-line').first()).toBeVisible();
    await expect(page.locator('#ci-loc-legend .ci-legend-toggle')).toHaveCount(5);

    // The docs chart is on the Reports tab. Switch to it and verify.
    await page.locator('#ci-tab-reports').click();
    await expect(page.locator('#ci-docs-chart .ci-series-line').first()).toBeVisible();
    await expect(page.locator('#ci-docs-legend .ci-legend-toggle')).toHaveCount(5);
    await expect(page.locator('#ci-docs-tiles .ci-tile')).toHaveCount(6);
    await expect(page.locator('#ci-guides-chart .ci-series-line')).toHaveCount(2);
    await expect(page.locator('#ci-flow-chart path.ci-bar-pos, #ci-flow-chart path.ci-bar-neg')
      .first()).toBeVisible();

    // Back on the Tests tab, the sparkline grid and table are present.
    await page.locator('#ci-tab-tests').click();
    await expect(page.locator('#ci-sparks .ci-spark').first()).toBeVisible();
    await expect(page.locator('#ci-table tbody tr').first()).toBeVisible();
    await expect(page.locator('#ci-skips').first()).not.toBeEmpty();

    expect(errors).toEqual([]);
  });

  test('the environment lock offers each build environment and switching redraws', async ({ page }) => {
    await page.goto('/ci');
    await expect(page.locator('#ci-body')).toBeVisible({ timeout: 20_000 });

    const env = page.locator('#ci-env');
    const options = await env.locator('option').allTextContents();
    expect(options.length).toBeGreaterThan(1);
    // Labels name the OS and compiler, since those are what make runs
    // incomparable across environments.
    expect(options.join(' ')).toMatch(/macOS|Linux/);

    const before = await page.locator('#ci-table tbody').innerHTML();
    // Switching is a request for that environment's timings, so the redraw
    // lands when it answers.
    await env.selectOption({ index: 1 });
    await expect(page.locator('#ci-table tbody')).not.toHaveText('');
    await expect.poll(() => page.locator('#ci-table tbody').innerHTML()).not.toBe(before);
    await expect(page).toHaveURL(/env=/);
  });

  test('removing a series does not recolor the survivors', async ({ page }) => {
    await page.goto('/ci');
    await expect(page.locator('#ci-body')).toBeVisible({ timeout: 20_000 });

    const swatches = page.locator('#ci-legend .ci-legend-item .swatch');
    await expect(swatches).toHaveCount(5);
    const secondColor = await swatches.nth(1).evaluate(
      (el) => getComputedStyle(el).backgroundColor,
    );

    // Drop the first series; the one that was second keeps its own color.
    await page.locator('#ci-legend .ci-legend-item').first().click();
    await expect(swatches).toHaveCount(4);
    const nowFirst = await swatches.first().evaluate(
      (el) => getComputedStyle(el).backgroundColor,
    );
    expect(nowFirst).toBe(secondColor);
  });

  test('scale toggle and status filter are reflected in the URL', async ({ page }) => {
    await page.goto('/ci');
    await expect(page.locator('#ci-body')).toBeVisible({ timeout: 20_000 });

    await page.locator('.ci-seg-btn[data-scale="log"]').click();
    await expect(page).toHaveURL(/scale=log/);
    await expect(page.locator('#ci-chart .ci-series-line').first()).toBeVisible();

    await page.locator('.ci-chip[data-status="skip"]').click();
    await expect(page.locator('.ci-chip[data-status="skip"]')).toHaveAttribute('aria-pressed', 'false');
    await expect(page).toHaveURL(/status=/);
  });

  // The default is the last 7 days, so a bare /ci carries no range param and
  // `all` is the one that has to be written down. The range <select> is in the
  // shared tab bar, visible on every tab.
  test('range defaults to 7 days, offers 1 day, and only non-defaults hit the URL', async ({ page }) => {
    await page.goto('/ci');
    await expect(page.locator('#ci-body')).toBeVisible({ timeout: 20_000 });

    await expect(page.locator('#ci-range')).toHaveValue('7d');
    await expect(page).not.toHaveURL(/range=/);

    const values = await page.locator('#ci-range option').evaluateAll(
      (os) => os.map((o) => o.value),
    );
    expect(values).toEqual(['all', '90d', '30d', '7d', '1d']);

    await page.locator('#ci-range').selectOption('1d');
    await expect(page).toHaveURL(/range=1d/);
    // A narrower window is still a window: the chart redraws rather than
    // emptying, because the cutoff is relative to the newest run, not to now.
    // Live data can leave a single run in a 1-day window, and a one-point
    // series is a zero-length `M x,y` path that Playwright reports as hidden.
    // So assert on what is always drawn -- the series path in the DOM and at
    // least one run marker -- rather than on the line's own visibility.
    await expect(page.locator('#ci-chart .ci-series-line').first()).toBeAttached();
    await expect(page.locator('#ci-chart .ci-point, #ci-chart .ci-point-raw, #ci-chart .ci-point-fail').first()).toBeVisible();

    await page.locator('#ci-range').selectOption('all');
    await expect(page).toHaveURL(/range=all/);
  });

  test('the benchmarks panel charts the nightly rows, one engine per view', async ({ page }) => {
    await page.goto('/ci');
    await expect(page.locator('#ci-body')).toBeVisible({ timeout: 20_000 });

    await page.locator('#ci-tab-bench').click();
    await expect(page).toHaveURL(/tab=bench/);

    // Default view, run time under tur jit: the fixture's two programs, each
    // named in the legend, and the noisy night counted in the subtitle.
    await expect(page.locator('#ci-bench-chart .ci-series-line')).toHaveCount(2);
    await expect(page.locator('#ci-bench-legend .ci-legend-item')).toHaveCount(2);
    await expect(page.locator('#ci-bench-legend')).toContainText('mut-map-reassign-1m');
    await expect(page.locator('#ci-bench-sub')).toContainText('1 noisy');

    // Binary size is a cc view of its own, kept in the URL, in bytes.
    await page.locator('#ci-bench-view').selectOption('binary_bytes/cc');
    await expect(page).toHaveURL(/bench=binary_bytes%2Fcc/);
    await expect(page.locator('#ci-bench-chart .ci-series-line')).toHaveCount(1);
    await expect(page.locator('#ci-bench-legend')).toContainText('KB');

    // Its own tooltip, on its own hit area.
    await page.locator('#ci-bench-hit').hover();
    await expect(page.locator('#ci-bench-tooltip')).toBeVisible();
  });

  test('the benchmarks panel says so when nothing has been published', async ({ page }) => {
    await stubApi(page, { fail: { bench: 502 } });
    await page.goto('/ci?tab=bench');
    await expect(page.locator('#ci-body')).toBeVisible({ timeout: 20_000 });
    await expect(page.locator('#ci-bench-chart .ci-empty')).toContainText('Benchmarks are unavailable');
    // The rest of the page is unaffected.
    await page.locator('#ci-tab-tests').click();
    await expect(page.locator('#ci-tiles .ci-tile')).toHaveCount(4);
  });

  test('the line-count panel charts product vs test lines', async ({ page }) => {
    await page.goto('/ci');
    await expect(page.locator('#ci-body')).toBeVisible({ timeout: 20_000 });

    // The line-count chart is on the Code tab.
    await page.locator('#ci-tab-code').click();

    // The headline reads the NEWEST row, not the newest in range.
    await expect(page.locator('#ci-loc-sub')).toContainText('407,783 product');
    await expect(page.locator('#ci-loc-sub')).toContainText('168,879 test');

    // Two series by default, each named in the legend -- never color alone.
    await expect(page.locator('#ci-loc-chart .ci-series-line')).toHaveCount(2);
    const legend = page.locator('#ci-loc-legend .ci-legend-toggle');
    await expect(legend).toHaveCount(5);
    await expect(legend.nth(0)).toHaveAttribute('aria-pressed', 'true');
    await expect(legend.nth(1)).toHaveAttribute('aria-pressed', 'true');
    await expect(legend.nth(2)).toHaveAttribute('aria-pressed', 'false');

    // Adding the generated bucket adds a third line and records it in the URL.
    await legend.nth(4).click();
    await expect(page.locator('#ci-loc-chart .ci-series-line')).toHaveCount(3);
    await expect(page).toHaveURL(/loc=code_lines%2Ctest_lines%2Cgenerated_lines/);

    // Counts only: the panel has no percent-change mode, so no toggle, and an
    // old ?locmode=change link is ignored rather than honoured.
    await expect(page.locator('#ci-panel-code .ci-seg-btn')).toHaveCount(0);
    await expect(page.locator('#ci-loc-chart .ci-axis-text', { hasText: /%$/ }))
      .toHaveCount(0);

    // Its own tooltip, on its own hit area.
    await page.locator('#ci-loc-hit').hover();
    const tip = page.locator('#ci-loc-tooltip');
    await expect(tip).toBeVisible();
    await expect(tip.locator('.ci-tooltip-head .mono')).toHaveText(/^[0-9a-f]{7}$/);
    await expect(page.locator('#ci-tooltip')).toBeHidden();
  });

  // What the panel shows the day the first row lands: one point is a dot and no
  // line, which reads as a broken chart unless it says otherwise.
  test('a single line-count row says so instead of drawing a lone dot', async ({ page }) => {
    await stubApi(page, {
      loc: [{
        ts: Math.floor(Date.now() / 1000), sha: 'fffffff9999999999999999999999999999fffff',
        code_lines: 407783, test_lines: 168879, bench_lines: 13528,
        example_lines: 4144, generated_lines: 1556131,
      }],
    });
    await page.goto('/ci');
    await expect(page.locator('#ci-body')).toBeVisible({ timeout: 20_000 });

    await page.locator('#ci-tab-code').click();
    await expect(page.locator('#ci-loc-chart .ci-empty')).toContainText(/one push/i);
    await expect(page.locator('#ci-loc-chart .ci-series-line')).toHaveCount(0);
    // The headline and the picker still work off that one row.
    await expect(page.locator('#ci-loc-sub')).toContainText('407,783 product');
    await expect(page.locator('#ci-loc-legend .ci-legend-toggle')).toHaveCount(5);
  });

  // A failed panel must cost that panel, not the page.
  test('a failed line-count request costs only its own panel', async ({ page }) => {
    const errors = [];
    page.on('pageerror', (e) => errors.push(e.message));

    await stubApi(page, { fail: { loc: 502 } });
    await page.goto('/ci');
    await expect(page.locator('#ci-body')).toBeVisible({ timeout: 20_000 });

    await page.locator('#ci-tab-code').click();
    await expect(page.locator('#ci-loc-chart .ci-empty')).toContainText(/unavailable/i);
    await expect(page.locator('#ci-loc-legend')).toBeEmpty();
    // The timings chart is untouched.
    await page.locator('#ci-tab-tests').click();
    await expect(page.locator('#ci-chart .ci-series-line').first()).toBeVisible();
    expect(errors).toEqual([]);
  });

  test('the docs panel charts open plans and reports', async ({ page }) => {
    await page.goto('/ci');
    await expect(page.locator('#ci-body')).toBeVisible({ timeout: 20_000 });

    await page.locator('#ci-tab-reports').click();
    await expect(page).toHaveURL(/tab=reports/);

    // The headline reads the NEWEST row.
    await expect(page.locator('#ci-docs-sub')).toContainText('30 open plans');
    await expect(page.locator('#ci-docs-sub')).toContainText('29 open reports');

    // Two series by default (open reports + stray plans).
    await expect(page.locator('#ci-docs-chart .ci-series-line')).toHaveCount(2);
    const legend = page.locator('#ci-docs-legend .ci-legend-toggle');
    await expect(legend).toHaveCount(5);
    await expect(legend.nth(0)).toHaveAttribute('aria-pressed', 'true');
    await expect(legend.nth(1)).toHaveAttribute('aria-pressed', 'true');
    await expect(legend.nth(2)).toHaveAttribute('aria-pressed', 'false');

    // Tiles show the current counts.
    await expect(page.locator('#ci-docs-tiles .ci-tile')).toHaveCount(6);
    await expect(page.locator('#ci-docs-tiles .ci-tile').nth(0)).toContainText('30');
    // The total is the sum of the buckets under it, led by the stray plans.
    await expect(page.locator('#ci-docs-tiles .ci-tile').nth(0)).toContainText('10 stray');
    await expect(page.locator('#ci-docs-tiles .ci-tile').nth(1)).toContainText('29');

    // Adding Godot plans adds a third line and records it in the URL.
    await legend.nth(2).click();
    await expect(page.locator('#ci-docs-chart .ci-series-line')).toHaveCount(3);
    await expect(page).toHaveURL(/docs=open_reports%2Cactive_plans%2Cgodot_plans/);

    // Its own tooltip, on its own hit area.
    await page.locator('#ci-docs-hit').hover();
    const tip = page.locator('#ci-docs-tooltip');
    await expect(tip).toBeVisible();
    await expect(tip.locator('.ci-tooltip-head .mono')).toHaveText(/^[0-9a-f]{7}$/);
  });

  test('the Test counts tab charts total tests', async ({ page }) => {
    await page.goto('/ci');
    await expect(page.locator('#ci-body')).toBeVisible({ timeout: 20_000 });

    await page.locator('#ci-tab-counts').click();
    await expect(page).toHaveURL(/tab=counts/);

    // Each suite counted once, at its newest count: 1,400 + 3,600 + 80.
    const tiles = page.locator('#ci-counts-tiles .ci-tile');
    await expect(tiles).toHaveCount(4);
    await expect(tiles.filter({ hasText: 'Tests' }).first()).toContainText('5,080');
    await expect(tiles.filter({ hasText: 'Skipped tests' })).toContainText('910');
    await expect(tiles.filter({ hasText: 'Change in range' })).toContainText('+10');
    await expect(page.locator('#ci-counts-sub')).toContainText('23 other suites');

    // The total, one line, with its own hover.
    await expect(page.locator('#ci-counts-chart .ci-series-line')).toHaveCount(1);
    await page.locator('#ci-counts-hit').hover();
    await expect(page.locator('#ci-counts-tooltip')).toBeVisible();

    // Not environment-locked: no picker of its own. The shared Range select
    // is carried into whichever tab is open, so it does not count.
    await expect(page.locator('#ci-panel-counts select:not(#ci-range)')).toHaveCount(0);
    await expect(page.locator('#ci-panel-counts #ci-range')).toBeVisible();
  });

  // A push runs only the jobs its diff needs, so the newest run often executed
  // a few suites. The total carries every other suite's newest count forward
  // rather than dropping to what that one run happened to execute.
  test('a run that executed only some suites does not dip the total', async ({ page }) => {
    await stubApi(page, {
      timings: timingsFixture().filter((r) => !(r.run_id === '0' && r.suite !== 'tur_tests')),
    });
    await page.goto('/ci?tab=counts');
    await expect(page.locator('#ci-body')).toBeVisible({ timeout: 20_000 });

    const tiles = page.locator('#ci-counts-tiles .ci-tile');
    await expect(tiles.filter({ hasText: 'Tests' }).first()).toContainText('5,080');
    // The line's last point sits where the others do, not at 1,400.
    const ys = await page.locator('#ci-counts-chart circle.ci-point').evaluateAll(
      (els) => els.map((el) => Number(el.getAttribute('cy'))));
    expect(ys.length).toBe(6);
    expect(Math.abs(ys[ys.length - 1] - ys[ys.length - 2])).toBeLessThan(2);
  });

  test('the Reports tab charts reports archived minus filed', async ({ page }) => {
    await page.goto('/ci?tab=reports');
    await expect(page.locator('#ci-body')).toBeVisible({ timeout: 20_000 });

    // +2, -1, -2 across three pushes. The first push predates the running
    // totals, so its net comes from the open count and the range cannot claim
    // a split.
    await expect(page.locator('#ci-flow-sub')).toContainText(/Net [−-]1 in this range/);
    await expect(page.locator('#ci-flow-chart path.ci-bar-pos, #ci-flow-chart path.ci-bar-neg')
      .first()).toBeVisible();
    // Polarity is named, never color alone.
    await expect(page.locator('#ci-flow-legend .ci-legend-item')).toHaveCount(2);
    await expect(page.locator('#ci-flow-chart .ci-zero-line')).toHaveCount(1);

    await page.locator('#ci-flow-hit').hover();
    const tip = page.locator('#ci-flow-tooltip');
    await expect(tip).toBeVisible();
    await expect(tip.locator('.ci-tooltip-row').first()).toContainText('Net');
  });

  test('the report flow names the split when every push carries it', async ({ page }) => {
    await stubApi(page, { docs: docsFixture().slice(1) });
    await page.goto('/ci?tab=reports');
    await expect(page.locator('#ci-body')).toBeVisible({ timeout: 20_000 });

    // 3 filed + 2 archived, then 3 filed + 1 archived: the backlog grew by 3.
    await expect(page.locator('#ci-flow-sub')).toContainText('3 archived, 6 filed');
    await expect(page.locator('#ci-flow-sub')).toContainText(/net [−-]3/);
    await expect(page.locator('#ci-flow-chart path.ci-bar-pos')).toHaveCount(0);
  });

  test('the Reports tab counts Turmeric guides and spice guides', async ({ page }) => {
    await page.goto('/ci?tab=reports');
    await expect(page.locator('#ci-body')).toBeVisible({ timeout: 20_000 });

    const tiles = page.locator('#ci-docs-tiles .ci-tile');
    await expect(tiles.filter({ hasText: 'Turmeric guides' })).toContainText('155');
    await expect(tiles.filter({ hasText: 'Spice guides' })).toContainText('21');
    await expect(page.locator('#ci-guides-sub')).toContainText('155 Turmeric guides');
    // Two lines; the null spice-guide row is skipped, not plotted as zero.
    await expect(page.locator('#ci-guides-chart .ci-series-line')).toHaveCount(2);
    await expect(page.locator('#ci-guides-legend .ci-legend-item')).toHaveCount(2);
  });

  test('the Spices tab shows the spice count, its trend and the names', async ({ page }) => {
    await page.goto('/ci');
    await expect(page.locator('#ci-body')).toBeVisible({ timeout: 20_000 });

    await page.locator('#ci-tab-spices').click();
    await expect(page).toHaveURL(/tab=spices/);
    await expect(page.locator('#ci-spices-tiles .ci-tile').first()).toContainText('43');
    await expect(page.locator('#ci-spices-chart .ci-series-line')).toHaveCount(1);
    await expect(page.locator('#ci-spices-list span')).toHaveText(['ansi', 'json', 'raylib']);
  });

  test('the Spices tab says "not measured" when no row carries the field', async ({ page }) => {
    await stubApi(page, {
      docs: docsFixture().map(({ spices, spice_guides, spice_names, ...r }) => r),
    });
    await page.goto('/ci?tab=spices');
    await expect(page.locator('#ci-body')).toBeVisible({ timeout: 20_000 });
    await expect(page.locator('#ci-spices-tiles .ci-tile').first()).toContainText('--');
    await expect(page.locator('#ci-spices-chart .ci-empty')).toContainText(/not measured/i);
  });

  // The server answers one (environment, range) pair per request. The page
  // asks for exactly what it shows, never twice in a session, and fetches the
  // Test counts and Benchmarks data only when their tab opens.
  test('one request per environment and range, and lazy tabs load on first open', async ({ page }) => {
    const calls = [];
    page.on('request', (r) => {
      const u = new URL(r.url());
      if (u.pathname.startsWith('/api/ci/')) calls.push(`${u.pathname.slice(8)}${u.search}`);
    });
    const of = (name) => calls.filter((c) => c.startsWith(name));

    await page.goto('/ci');
    await expect(page.locator('#ci-body')).toBeVisible({ timeout: 20_000 });
    expect(of('meta')).toHaveLength(1);
    expect(of('timings')).toHaveLength(1);
    expect(of('timings')[0]).toMatch(/range=7d/);
    expect(of('timings')[0]).toMatch(/v=1/);
    expect(of('loc')).toHaveLength(1);
    expect(of('docs')).toHaveLength(1);
    expect(of('counts')).toHaveLength(0);
    expect(of('bench')).toHaveLength(0);
    await expect(page.locator('#ci-provenance')).toContainText('Ingest #1');

    await page.locator('#ci-range').selectOption('1d');
    await expect.poll(() => of('timings').length).toBe(2);
    await page.locator('#ci-range').selectOption('7d');
    await page.locator('#ci-range').selectOption('1d');
    await expect(page.locator('#ci-chart .ci-series-line').first()).toBeAttached();
    expect(of('timings')).toHaveLength(2);

    await page.locator('#ci-tab-counts').click();
    await expect(page.locator('#ci-counts-chart .ci-series-line')).toHaveCount(1);
    await page.locator('#ci-tab-tests').click();
    await page.locator('#ci-tab-counts').click();
    expect(of('counts')).toHaveLength(1);
  });

  // 30 days and up chart one point per suite per day -- its median -- so the
  // page says so rather than calling a day a run.
  test('wide ranges chart daily medians and label them', async ({ page }) => {
    await page.goto('/ci?range=30d');
    await expect(page.locator('#ci-body')).toBeVisible({ timeout: 20_000 });
    await expect(page.locator('#ci-chart .ci-axis-title')).toContainText('daily median');
    await expect(page.locator('#ci-table thead')).toContainText('Days');
    await expect(page.locator('#ci-table thead')).toContainText('Mean of daily medians');

    await page.locator('#ci-range').selectOption('7d');
    await expect(page.locator('#ci-chart .ci-axis-title')).toContainText('runs');
    await expect(page.locator('#ci-table thead')).toContainText('Runs');
  });

  test('a failed range request keeps the current range and says why', async ({ page }) => {
    await page.goto('/ci');
    await expect(page.locator('#ci-body')).toBeVisible({ timeout: 20_000 });
    await page.route('**/api/ci/timings?*range=all*', (route) => route.fulfill({
      status: 503, contentType: 'application/json', body: '{"error":"stubbed"}',
    }));
    await page.locator('#ci-range').selectOption('all');
    await expect(page.locator('#ci-provenance')).toContainText('Could not load that range (503 stubbed)');
    await expect(page.locator('#ci-range')).toHaveValue('7d');
    await expect(page.locator('#ci-range')).toBeEnabled();
    await expect(page.locator('#ci-chart .ci-series-line').first()).toBeVisible();
    await expect(page).not.toHaveURL(/range=/);
  });

  // A response the page cannot render (here, timings with no `latest`) throws
  // inside boot() after every fetch succeeded. It must still end the spinner.
  test('an exception while booting is shown, not left in the console', async ({ page }) => {
    await page.route('**/api/ci/timings?*', (route) => route.fulfill({
      status: 200, contentType: 'application/json',
      body: JSON.stringify({ mode: 'run', suites: [], runs: [], cells: [] }),
    }));
    await page.goto('/ci');
    await expect(page.locator('#ci-state')).toHaveClass(/is-error/, { timeout: 20_000 });
    await expect(page.locator('#ci-state')).toContainText('The dashboard failed to load');
    await expect(page.locator('#ci-body')).toBeHidden();
  });

  test('a failed meta request says so instead of spinning', async ({ page }) => {
    await stubApi(page, { fail: { meta: 503 } });
    await page.goto('/ci');
    await expect(page.locator('#ci-state')).toHaveClass(/is-error/, { timeout: 20_000 });
    await expect(page.locator('#ci-state')).toContainText('503');
    await expect(page.locator('#ci-body')).toBeHidden();
  });

  test('a failed docs request costs only its own panel', async ({ page }) => {
    const errors = [];
    page.on('pageerror', (e) => errors.push(e.message));

    await stubApi(page, { fail: { docs: 502 } });
    await page.goto('/ci');
    await expect(page.locator('#ci-body')).toBeVisible({ timeout: 20_000 });

    await page.locator('#ci-tab-reports').click();
    await expect(page.locator('#ci-docs-chart .ci-empty')).toContainText(/unavailable/i);
    await expect(page.locator('#ci-docs-legend')).toBeEmpty();
    // The timings chart is untouched.
    await page.locator('#ci-tab-tests').click();
    await expect(page.locator('#ci-chart .ci-series-line').first()).toBeVisible();
    expect(errors).toEqual([]);
  });

  test('hovering the plot opens a tooltip', async ({ page }) => {
    await page.goto('/ci');
    await expect(page.locator('#ci-body')).toBeVisible({ timeout: 20_000 });

    await page.locator('#ci-hit').hover();
    const tip = page.locator('#ci-tooltip');
    await expect(tip).toBeVisible();
    await expect(tip.locator('.ci-tooltip-head .mono')).toHaveText(/^[0-9a-f]{7}$/);
    await expect(tip.locator('.ci-tooltip-row').first()).toBeVisible();
  });

  test('sparkline filter narrows the grid and clicking charts a suite', async ({ page }) => {
    await page.goto('/ci');
    await expect(page.locator('#ci-body')).toBeVisible({ timeout: 20_000 });

    const sparks = page.locator('#ci-sparks .ci-spark');
    const total = await sparks.count();
    expect(total).toBeGreaterThan(20);

    await page.locator('#ci-spark-search').fill('jit');
    await expect(sparks).not.toHaveCount(total);

    // Deliberately NOT sparks.first(): `toggleSuite` DESELECTS a sparkline
    // that is already charted, which REMOVES its legend item rather than
    // adding one. defaultSuites() pre-selects the five slowest suites and
    // renderSparks sorts by mean descending, so the first match of a filter
    // is frequently one of them -- `jit` picks `tur_jit_fixture_tests`, the
    // slowest suite in the default environment, whenever the Linux jit leg
    // is the broadest env. Reading live data, that made this assertion flip
    // from pass to fail with no change to the test. An unselected sparkline
    // is what "clicking charts a suite" actually means. If every jit suite
    // is already selected, widen the filter until one is not.
    let target = page.locator('#ci-sparks .ci-spark:not(.is-selected)').first();
    if (!await target.isVisible()) {
      await page.locator('#ci-spark-search').fill('');
      target = page.locator('#ci-sparks .ci-spark:not(.is-selected)').first();
    }
    const name = await target.getAttribute('data-suite');
    await target.click();
    await expect(page.locator(`#ci-legend .ci-legend-item[data-suite="${name}"]`)).toHaveCount(1);
  });

  // Drag across the plot from 30% of its width to past its right edge: the
  // fixture's six runs are an hour apart, so that keeps the newest four.
  // Raw mouse moves do not scroll, so bring the plot into view first.
  async function dragZoom(page, hitId = '#ci-hit') {
    await page.locator(hitId).scrollIntoViewIfNeeded();
    const box = await page.locator(hitId).boundingBox();
    const y = box.y + box.height / 2;
    await page.mouse.move(box.x + box.width * 0.3, y);
    await page.mouse.down();
    await page.mouse.move(box.x + box.width * 0.7, y, { steps: 5 });
    await page.mouse.move(box.x + box.width + 30, y, { steps: 5 });
    await page.mouse.up();
  }

  test('dragging across a chart zooms every tab, and the zoom round-trips', async ({ page }) => {
    await page.goto('/ci');
    await expect(page.locator('#ci-body')).toBeVisible({ timeout: 20_000 });
    const footer = page.locator('#ci-chart .ci-axis-title');
    await expect(footer).toContainText('6 runs');
    await expect(page.locator('#ci-zoom')).toBeHidden();
    await dragZoom(page);
    await expect(page.locator('#ci-zoom')).toBeVisible();
    await expect(page).toHaveURL(/from=\d+&to=\d+/);
    await expect(page.locator('#ci-zoom')).toBeVisible();
    await expect(footer).toContainText('4 runs');
    // The table reads the same window as the chart.
    await expect(page.locator('#ci-table tbody tr').first().locator('td').nth(1)).toHaveText('4');

    // The window is the page's, not the chart's: the line counts, five hours
    // apart, have one row inside it.
    await page.locator('.ci-tab[data-tab="code"]').click();
    await expect(page.locator('#ci-loc-chart .ci-empty')).toContainText('Only one measurement');

    // A shared link reopens on the window.
    await page.reload();
    await expect(page.locator('#ci-body')).toBeVisible({ timeout: 20_000 });
    await expect(page.locator('#ci-zoom')).toBeVisible();
    await page.locator('.ci-tab[data-tab="tests"]').click();
    await expect(footer).toContainText('4 runs');

    // Double-click zooms out; so does the chip.
    await page.locator('#ci-hit').dblclick();
    await expect(page.locator('#ci-zoom')).toBeHidden();
    await expect(page).not.toHaveURL(/from=/);
    await expect(footer).toContainText('6 runs');

    await dragZoom(page);
    await expect(page.locator('#ci-zoom')).toBeVisible();
    await page.locator('#ci-zoom').click();
    await expect(page.locator('#ci-zoom')).toBeHidden();
    await expect(footer).toContainText('6 runs');
  });

  test('a click on the plot is not a zoom, and a new range clears one', async ({ page }) => {
    await page.goto('/ci');
    await expect(page.locator('#ci-body')).toBeVisible({ timeout: 20_000 });
    await page.locator('#ci-hit').click();
    await expect(page.locator('#ci-zoom')).toBeHidden();

    await dragZoom(page);
    await expect(page.locator('#ci-zoom')).toBeVisible();
    await page.locator('#ci-range').selectOption('1d');
    await expect(page.locator('#ci-zoom')).toBeHidden();
    await expect(page).not.toHaveURL(/from=/);
  });

  test('the duration chart opens on the trend and can show raw runs', async ({ page }) => {
    await page.goto('/ci');
    await expect(page.locator('#ci-body')).toBeVisible({ timeout: 20_000 });

    // Trend: one band and one line per suite, the runs as faded dots.
    await expect(page.locator('.ci-seg-btn[data-smooth="trend"]')).toHaveClass(/is-active/);
    await expect(page.locator('#ci-chart .ci-series-band')).toHaveCount(5);
    await expect(page.locator('#ci-chart .ci-series-line')).toHaveCount(5);
    await expect(page.locator('#ci-chart .ci-point-raw').first()).toBeAttached();
    await expect(page).not.toHaveURL(/smooth=/);
    await page.locator('#ci-hit').hover();
    await expect(page.locator('#ci-tooltip .ci-tooltip-row .trend').first()).toHaveText(/^~/);

    await page.locator('.ci-seg-btn[data-smooth="raw"]').click();
    await expect(page).toHaveURL(/smooth=raw/);
    await expect(page.locator('#ci-chart .ci-series-band')).toHaveCount(0);
    await expect(page.locator('#ci-chart .ci-point').first()).toBeVisible();

    await page.reload();
    await expect(page.locator('#ci-body')).toBeVisible({ timeout: 20_000 });
    await expect(page.locator('.ci-seg-btn[data-smooth="raw"]')).toHaveClass(/is-active/);
    await expect(page.locator('#ci-chart .ci-series-band')).toHaveCount(0);
  });

  // One slow runner at the start of the range used to be the whole "Change":
  // first point against last read -90% for a suite that never moved.
  test('the table\'s change ignores one outlier at the edge of the range', async ({ page }) => {
    const now = Math.floor(Date.now() / 1000);
    const timings = Array.from({ length: 9 }, (_, i) => ({
      branch: 'main', build_type: 'Debug', os: 'Linux', cc: 'GNU-13.3.0', nproc: 4, jit: false,
      ts: now - i * 3600, sha: 'b'.repeat(40), run_id: String(i), suite: 'steady', status: 'pass',
      duration_ms: i === 8 ? 10_000 : 1000, shard_index: null, shard_total: null, skip_reason: null,
    }));
    await page.unrouteAll();
    await stubApi(page, { timings });
    await page.goto('/ci');
    await expect(page.locator('#ci-body')).toBeVisible({ timeout: 20_000 });
    const row = page.locator('#ci-table tbody tr[data-suite="steady"]');
    await expect(row.locator('td').nth(5)).toHaveText('0.0%');
  });

  test('the runner CPU is named on hover and can be filtered out', async ({ page }) => {
    await page.goto('/ci');
    await expect(page.locator('#ci-body')).toBeVisible({ timeout: 20_000 });

    const chips = page.locator('.ci-chip[data-cpu]');
    await expect(chips).toHaveCount(2);
    await expect(chips.nth(0)).toContainText('EPYC 7763');
    await expect(chips.nth(0)).toContainText('3');
    await expect(chips.nth(0)).toHaveAttribute('title', EPYC);

    // The right edge snaps to the newest run, which ran on the EPYC.
    await page.locator('#ci-hit').scrollIntoViewIfNeeded();
    const box = await page.locator('#ci-hit').boundingBox();
    await page.mouse.move(box.x + box.width - 2, box.y + box.height / 2);
    await expect(page.locator('#ci-tooltip .ci-tooltip-note')).toHaveText(EPYC);

    // Leave the Xeon runs out: three of six remain, and the choice is linked.
    await page.locator(`.ci-chip[data-cpu="${XEON}"]`).click();
    await expect(page.locator(`.ci-chip[data-cpu="${XEON}"]`)).toHaveAttribute('aria-pressed', 'false');
    await expect(page).toHaveURL(/nocpu=/);
    await expect(page.locator('#ci-table tbody tr').first().locator('td').nth(1)).toHaveText('3');
    // The last one on stays on.
    await page.locator(`.ci-chip[data-cpu="${EPYC}"]`).click();
    await expect(page.locator(`.ci-chip[data-cpu="${EPYC}"]`)).toHaveAttribute('aria-pressed', 'true');

    await page.reload();
    await expect(page.locator('#ci-body')).toBeVisible({ timeout: 20_000 });
    await expect(page.locator(`.ci-chip[data-cpu="${XEON}"]`)).toHaveAttribute('aria-pressed', 'false');
  });

  test('an environment with no recorded CPU shows no CPU filter', async ({ page }) => {
    await page.goto('/ci');
    await expect(page.locator('#ci-body')).toBeVisible({ timeout: 20_000 });
    const mac = await page.locator('#ci-env option').evaluateAll(
      (os) => os.find((o) => o.textContent.includes('macOS')).value);
    await page.locator('#ci-env').selectOption(mac);
    await expect(page.locator('.ci-chip[data-status="pass"]')).toBeVisible();
    await expect(page.locator('.ci-chip[data-cpu]')).toHaveCount(0);
  });

  test('the Relative axis charts each suite as % change from its start', async ({ page }) => {
    await page.goto('/ci');
    await expect(page.locator('#ci-body')).toBeVisible({ timeout: 20_000 });
    await page.locator('.ci-seg-btn[data-scale="relative"]').click();
    await expect(page).toHaveURL(/scale=relative/);
    await expect(page.locator('#ci-chart .ci-zero-line')).toHaveCount(1);
    await expect(page.locator('#ci-chart .ci-axis-text', { hasText: /^0%$/ })).toHaveCount(1);
    await expect(page.locator('#ci-chart .ci-axis-title')).toContainText('change from each suite');

    await page.locator('#ci-hit').hover();
    await expect(page.locator('#ci-tooltip .ci-tooltip-row .val').first()).toHaveText(/%.*·/);

    await page.reload();
    await expect(page.locator('#ci-body')).toBeVisible({ timeout: 20_000 });
    await expect(page.locator('.ci-seg-btn[data-scale="relative"]')).toHaveClass(/is-active/);
  });

  test('layout does not overflow horizontally at 480px', async ({ page }) => {
    await page.setViewportSize({ width: 480, height: 900 });
    await page.goto('/ci');
    await expect(page.locator('#ci-body')).toBeVisible({ timeout: 20_000 });

    const overflow = await page.evaluate(
      () => document.documentElement.scrollWidth - document.documentElement.clientWidth,
    );
    expect(overflow).toBeLessThanOrEqual(1);
  });
});
