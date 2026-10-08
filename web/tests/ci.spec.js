import { test, expect } from '@playwright/test';

// /ci — the CI suite-timings dashboard.
//
// These hit the live worker route, which proxies the `ci-metrics` branch on
// GitHub. That is deliberate: the proxy is the part most likely to break
// silently (a renamed branch, a year rollover), and a fixture would hide it.

test.describe('CI metrics dashboard', () => {
  // The line-count and docs panels are driven from FIXTURES, unlike everything
  // else here. repo-loc-<year>.jsonl and docs-counts-<year>.jsonl are absent
  // from the ci-metrics branch until the first publish after the panels
  // shipped, so against live data these assertions would be skipped exactly
  // when they are new -- which is when they are worth most. The proxy that
  // serves each file keeps its own live test below.
  //
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
    return rows.map(([sha, code, tests, bench, example, generated], i) => JSON.stringify({
      ts: now - (rows.length - 1 - i) * 5 * 3600,
      sha,
      code_lines: code,
      test_lines: tests,
      bench_lines: bench,
      example_lines: example,
      generated_lines: generated,
    })).join('\n');
  }

  function docsFixture() {
    const now = Math.floor(Date.now() / 1000);
    const rows = [
      ['aaaaaaa1111111111111111111111111111aaaa1', 28, 25, 10, 15, 3, 0],
      ['bbbbbbb2222222222222222222222222222bbbb2', 29, 26, 10, 15, 3, 1],
      ['ccccccc3333333333333333333333333333cccc3', 30, 27, 10, 15, 3, 2],
      ['ddddddd4444444444444444444444444444dddd4', 30, 29, 10, 14, 3, 3],
    ];
    return rows.map(([sha, open_plans, open_reports, active, held, v1, spices], i) => JSON.stringify({
      ts: now - (rows.length - 1 - i) * 5 * 3600,
      sha,
      open_plans,
      open_reports,
      active_plans: active,
      held_plans: held,
      v1_plans: v1,
      spices_plans: spices,
    })).join('\n');
  }

  async function stubLoc(page) {
    const body = locFixture();
    await page.route('**/api/ci-loc*', (route) => route.fulfill({
      status: 200,
      headers: { 'Content-Type': 'application/x-ndjson', 'X-Metrics-Year': '2026' },
      body,
    }));
  }

  async function stubDocs(page) {
    const body = docsFixture();
    await page.route('**/api/ci-docs*', (route) => route.fulfill({
      status: 200,
      headers: { 'Content-Type': 'application/x-ndjson', 'X-Metrics-Year': '2026' },
      body,
    }));
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
    // (active + held + v1 + spices) -- but only once a row published AFTER a
    // new bucket lands carries that bucket.  A row published before the
    // spices_plans field existed has no spices_plans, so its three buckets
    // sum to less than the total (the spices plans are in the tree but
    // uncategorized in that row).  Assert strict equality when every bucket
    // is present, and the weaker >= while the live row is in transition, so a
    // stale row does not redden a PR that did not touch the collector.
    const PLAN_BUCKETS = ['active_plans', 'held_plans', 'v1_plans', 'spices_plans'];
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

    // Stubbed so a legitimately-absent repo-loc / docs-counts file does not
    // show up here as a 502 in the console. The degraded paths have their own
    // tests.
    await stubLoc(page);
    await stubDocs(page);
    await page.goto('/ci');

    await expect(page.locator('#ci-body')).toBeVisible({ timeout: 20_000 });
    await expect(page.locator('#ci-state')).toBeHidden();

    // Shared chrome still renders on this page.
    await expect(page.locator('site-nav nav')).toBeVisible();
    await expect(page.locator('site-footer a[href="/ci"]')).toHaveCount(1);

    // Three tabs, Tests active by default.
    await expect(page.locator('.ci-tab')).toHaveCount(3);
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

    // The line-count chart is on the Code tab. Switch to it and verify.
    await page.locator('#ci-tab-code').click();
    await expect(page.locator('#ci-loc-chart .ci-series-line').first()).toBeVisible();
    await expect(page.locator('#ci-loc-legend .ci-legend-toggle')).toHaveCount(5);

    // The docs chart is on the Reports tab. Switch to it and verify.
    await page.locator('#ci-tab-reports').click();
    await expect(page.locator('#ci-docs-chart .ci-series-line').first()).toBeVisible();
    await expect(page.locator('#ci-docs-legend .ci-legend-toggle')).toHaveCount(5);
    await expect(page.locator('#ci-docs-tiles .ci-tile')).toHaveCount(4);

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
    await env.selectOption({ index: 1 });
    await expect(page.locator('#ci-table tbody')).not.toHaveText('');
    expect(await page.locator('#ci-table tbody').innerHTML()).not.toBe(before);
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
    await expect(page.locator('#ci-chart .ci-point, #ci-chart .ci-point-fail').first()).toBeVisible();

    await page.locator('#ci-range').selectOption('all');
    await expect(page).toHaveURL(/range=all/);
  });

  test('the line-count panel charts product vs test lines', async ({ page }) => {
    await stubLoc(page);
    await stubDocs(page);
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

    // Change mode replots each series against its own first value in range, so
    // the two land on one axis -- the whole point of having the mode.
    await page.locator('.ci-seg-btn[data-loc-mode="change"]').click();
    await expect(page).toHaveURL(/locmode=change/);
    // ... and must not have moved the timings chart's own toggle.
    await expect(page).not.toHaveURL(/[?&]scale=log/);
    await expect(page.locator('#ci-loc-chart .ci-axis-title'))
      .toHaveText(/change since/);
    await expect(page.locator('#ci-loc-chart .ci-axis-text').first())
      .toHaveText(/%$/);

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
    await page.route('**/api/ci-loc*', (route) => route.fulfill({
      status: 200,
      headers: { 'Content-Type': 'application/x-ndjson' },
      body: JSON.stringify({
        ts: Math.floor(Date.now() / 1000), sha: 'fffffff9999999999999999999999999999fffff',
        code_lines: 407783, test_lines: 168879, bench_lines: 13528,
        example_lines: 4144, generated_lines: 1556131,
      }),
    }));
    await page.goto('/ci');
    await expect(page.locator('#ci-body')).toBeVisible({ timeout: 20_000 });

    await page.locator('#ci-tab-code').click();
    await expect(page.locator('#ci-loc-chart .ci-empty')).toContainText(/one push/i);
    await expect(page.locator('#ci-loc-chart .ci-series-line')).toHaveCount(0);
    // The headline and the picker still work off that one row.
    await expect(page.locator('#ci-loc-sub')).toContainText('407,783 product');
    await expect(page.locator('#ci-loc-legend .ci-legend-toggle')).toHaveCount(5);
  });

  // The route legitimately 502s until the first publish after this shipped, so
  // the degraded path is the one users see first. It must cost one panel, not
  // the page.
  test('a missing line-count file costs only its own panel', async ({ page }) => {
    const errors = [];
    page.on('pageerror', (e) => errors.push(e.message));

    await page.route('**/api/ci-loc*', (route) => route.fulfill({
      status: 502, body: 'no line counts available\n',
    }));
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
    await stubLoc(page);
    await stubDocs(page);
    await page.goto('/ci');
    await expect(page.locator('#ci-body')).toBeVisible({ timeout: 20_000 });

    await page.locator('#ci-tab-reports').click();
    await expect(page).toHaveURL(/tab=reports/);

    // The headline reads the NEWEST row.
    await expect(page.locator('#ci-docs-sub')).toContainText('30 open plans');
    await expect(page.locator('#ci-docs-sub')).toContainText('29 open reports');

    // Two series by default (open reports + active plans).
    await expect(page.locator('#ci-docs-chart .ci-series-line')).toHaveCount(2);
    const legend = page.locator('#ci-docs-legend .ci-legend-toggle');
    await expect(legend).toHaveCount(5);
    await expect(legend.nth(0)).toHaveAttribute('aria-pressed', 'true');
    await expect(legend.nth(1)).toHaveAttribute('aria-pressed', 'true');
    await expect(legend.nth(2)).toHaveAttribute('aria-pressed', 'false');

    // Tiles show the current counts.
    await expect(page.locator('#ci-docs-tiles .ci-tile')).toHaveCount(4);
    await expect(page.locator('#ci-docs-tiles .ci-tile').nth(0)).toContainText('30');
    await expect(page.locator('#ci-docs-tiles .ci-tile').nth(1)).toContainText('29');

    // Adding held plans adds a third line and records it in the URL.
    await legend.nth(2).click();
    await expect(page.locator('#ci-docs-chart .ci-series-line')).toHaveCount(3);
    await expect(page).toHaveURL(/docs=open_reports%2Cactive_plans%2Cheld_plans/);

    // Its own tooltip, on its own hit area.
    await page.locator('#ci-docs-hit').hover();
    const tip = page.locator('#ci-docs-tooltip');
    await expect(tip).toBeVisible();
    await expect(tip.locator('.ci-tooltip-head .mono')).toHaveText(/^[0-9a-f]{7}$/);
  });

  test('a missing docs file costs only its own panel', async ({ page }) => {
    const errors = [];
    page.on('pageerror', (e) => errors.push(e.message));

    await page.route('**/api/ci-docs*', (route) => route.fulfill({
      status: 502, body: 'no doc counts available\n',
    }));
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

  test('layout does not overflow horizontally at 480px', async ({ page }) => {
    await page.setViewportSize({ width: 480, height: 900 });
    await stubLoc(page);
    await stubDocs(page);
    await page.goto('/ci');
    await expect(page.locator('#ci-body')).toBeVisible({ timeout: 20_000 });

    const overflow = await page.evaluate(
      () => document.documentElement.scrollWidth - document.documentElement.clientWidth,
    );
    expect(overflow).toBeLessThanOrEqual(1);
  });
});
