// ci-metrics.js — /ci, the CI suite-timings dashboard.
//
// Named ci-metrics rather than ci because a root-level `ci.js` shadows the
// /ci route: Vite resolves the extensionless request to the module, not to
// ci/index.html.
//
// Reads NDJSON from /api/ci-timings and /api/ci-loc (worker.js proxies the
// `ci-metrics` orphan branch) and renders it as hand-built SVG. No charting
// library: the site bundles nothing from a CDN, and the shapes here are simple
// enough that a dependency would cost more than it saves.
//
// THE ONE RULE THIS FILE ENFORCES: suite timings are only comparable within a
// fixed (build_type, os, cc, nproc, jit) tuple. Rather than document that and
// hope, the environment <select> is a hard lock — every view derives from
// `state.env`, and nothing on the page ever aggregates across two of them.
//
// Line counts are the exception, and deliberately sit OUTSIDE that lock: a
// count is a property of the commit, so it has no runner dimension to be
// incomparable across. The env <select> therefore does not touch that panel.

import './icons.js'; // <t-icon>
import { GITHUB_URL } from './repo.js';

const API = '/api/ci-timings';
const LOC_API = '/api/ci-loc';
const DOCS_API = '/api/ci-docs';

// Fixed categorical order from vars.css. Assigned by suite name, never by
// rank, so filtering the selection does not repaint the survivors. Five is the
// cap: a sixth series would have to be an invented hue, so the picker refuses
// instead and the sparkline grid covers the long tail.
const SERIES_VARS = ['--chart-1', '--chart-2', '--chart-3', '--chart-4', '--chart-5'];
const MAX_SERIES = SERIES_VARS.length;

const STATUSES = ['pass', 'skip', 'fail'];
const STATUS_ICON = { pass: 'check', skip: 'minus', fail: 'x' };

const RANGES = [
  ['all', 'All time'],
  ['90d', 'Last 90 days'],
  ['30d', 'Last 30 days'],
  ['7d',  'Last 7 days'],
  ['1d',  'Last 24 hours'],
];

// A week is the window a regression is still worth chasing in: `main` takes
// several pushes a day, so "all time" opens on hundreds of runs compressed into
// 860px, where a 2% step change is one pixel and invisible. Omitted from the URL
// (writeURL) so a bare /ci link means this, and ?range=all is a choice someone
// made.
const DEFAULT_RANGE = '7d';

// The line-count series, in palette order. `code` and `test` are the two the
// panel opens on -- the question is "how much product, how much test" -- and the
// rest are there so the buckets add up to the repo rather than quietly
// disappearing. `generated` is committed machine output (mostly the ~1.55M lines
// of tests/fixtures/*/expected.c), which is why it is not folded into `test`:
// nobody wrote it, and it moves by six figures whenever the emitter does.
const LOC_SERIES = [
  ['code_lines',      'Product code', '--chart-1'],
  ['test_lines',      'Test code',    '--chart-2'],
  ['bench_lines',     'Benchmarks',   '--chart-3'],
  ['example_lines',   'Examples',     '--chart-4'],
  ['generated_lines', 'Generated',    '--chart-5'],
];
const LOC_DEFAULT = ['code_lines', 'test_lines'];

// The open-work series, in palette order. `open_reports` and `active_plans`
// are the two the panel opens on -- the question is "how much open work, split
// by kind" -- and `held_plans` / `v1_plans` / `spices_plans` are there so the
// breakdown of the plan total is visible without being the default view. The
// total open_plans is the sum of the four plan buckets; it is not a separate
// series because a sum line on the same chart as its addends is a visual
// tautology.
const DOCS_SERIES = [
  ['open_reports',  'Open reports',  '--chart-1'],
  ['active_plans',  'Active plans',  '--chart-2'],
  ['held_plans',    'Held plans',    '--chart-3'],
  ['v1_plans',      'v1 plans',      '--chart-4'],
  ['spices_plans',  'Spices plans',  '--chart-5'],
];
const DOCS_DEFAULT = ['open_reports', 'active_plans'];

// Count series that come from the same docs-counts rows but are not "open
// work": guides (Turmeric's own, and the spices repo's) and the spice count.
// Fixed, not toggleable -- each chart has one or two lines, so a picker would
// be more chrome than data. The spice fields are null on a row published
// without the spices checkout; such rows are dropped from these charts rather
// than plotted as a collapse to zero.
const GUIDE_SERIES = [
  ['turmeric_guides', 'Turmeric guides', '--chart-1'],
  ['spice_guides',    'Spice guides',    '--chart-2'],
];
const SPICE_SERIES = [
  ['spices', 'Spices', '--chart-1'],
];

// Macro-suites: the ctest suites that print a test count, grouped by what they
// exercise. First match wins and the last row matches everything, so a newly
// counted suite always lands in some group -- check which before trusting it.
// Five rows, one per palette slot, in palette order.
const MACRO_SUITES = [
  ['fixtures', 'Fixtures', '--chart-1', /^tur_tests$|fixture_tests$/,
    'tests/fixtures/ through each engine: compiled, interpreter, JIT'],
  ['browser', 'Browser', '--chart-2', /^web_/,
    'Try Turmeric and the site, under Playwright'],
  ['spices', 'Spices', '--chart-3', /spice|scscm/,
    'Resolving, fetching and building spices'],
  ['runtime', 'Compiler and runtime', '--chart-4',
    /_seam$|_unit$|multi_module|leak|ctor_alias|demangle|eval_sandbox/,
    'Seams, unit tests, multi-module builds and leak gates'],
  ['tooling', 'CLI and tooling', '--chart-5', /./,
    'The tur subcommands, and every counted suite not above'],
];

function macroOf(suite) {
  return MACRO_SUITES.find(([, , , re]) => re.test(suite));
}

const TABS = ['tests', 'counts', 'code', 'spices', 'reports'];

// Ranges the 8-day recent window can answer. Anything wider needs the full log.
const RECENT_RANGES = ['1d', '7d'];

// ── STATE ───────────────────────────────────────────────────────────────────

const state = {
  rows: [],
  envs: [],           // [{ key, label, ts }]
  env: null,          // env key
  // Fixed-length slot array, one per palette color. A deselected suite leaves
  // a null HOLE rather than compacting, because the slot index IS the color:
  // compacting would repaint every survivor when you remove a series.
  suites: new Array(MAX_SERIES).fill(null),
  statuses: new Set(STATUSES),
  range: DEFAULT_RANGE,
  scale: 'linear',
  sort: { col: 'delta', dir: 'desc' },
  sparkFilter: '',
  year: null,
  // Line counts: a separate NDJSON file, one row per push to main, no env
  // dimension. Empty when /api/ci-loc has nothing yet (the file does not exist
  // until the first publish after this shipped), which the panel says out loud
  // rather than drawing an empty axis.
  loc: [],
  locKeys: new Set(LOC_DEFAULT),
  locError: null,
  // Open plans and reports: same shape as loc -- one NDJSON row per push to
  // main, no env dimension. Empty until /api/ci-docs has its first row.
  docs: [],
  docsKeys: new Set(DOCS_DEFAULT),
  docsError: null,
  // Which tab is visible. Tests is the default; the others render their charts
  // on first reveal so the SVG picks up the real panel width instead of the
  // 860px fallback a hidden panel forces.
  tab: 'tests',
  // True while `rows` is only the recent window (/api/ci-timings?window=recent)
  // rather than the full year. The full log is 40+ MB; the page opens on 7 days.
  windowed: false,
};

// ── FORMATTING ──────────────────────────────────────────────────────────────

function fmtDuration(ms) {
  if (ms == null || Number.isNaN(ms)) return '--';
  if (ms < 1000) return `${Math.round(ms)} ms`;
  const s = ms / 1000;
  if (s < 60) return `${s < 10 ? s.toFixed(1) : Math.round(s)} s`;
  const m = Math.floor(s / 60);
  const rem = Math.round(s - m * 60);
  return `${m}m ${String(rem).padStart(2, '0')}s`;
}

// Compact form for axis ticks, where horizontal room is scarce.
function fmtTick(ms) {
  if (ms === 0) return '0';
  if (ms < 1000) return `${Math.round(ms)}ms`;
  const s = ms / 1000;
  if (s < 60) return `${s < 10 ? s.toFixed(1) : Math.round(s)}s`;
  return `${Math.round(s / 60)}m`;
}

function fmtDate(ts) {
  return new Date(ts * 1000).toLocaleDateString(undefined, {
    month: 'short', day: 'numeric',
  });
}

function fmtDateTime(ts) {
  return new Date(ts * 1000).toLocaleString(undefined, {
    month: 'short', day: 'numeric', hour: '2-digit', minute: '2-digit',
  });
}

function fmtPct(x) {
  const sign = x > 0 ? '+' : '';
  return `${sign}${Math.abs(x) < 10 ? x.toFixed(1) : Math.round(x)}%`;
}

function fmtCount(n) {
  return n == null || Number.isNaN(n) ? '--' : Math.round(n).toLocaleString();
}

// A change in a count: the sign carries the meaning, so zero is the only value
// shown without one.
function fmtSignedCount(n) {
  if (n == null || Number.isNaN(n)) return '--';
  return `${n > 0 ? '+' : n < 0 ? '−' : ''}${Math.abs(Math.round(n)).toLocaleString()}`;
}

// Axis form for line counts, where `1,552,973` does not fit under a tick.
// Decimals are kept rather than rounded away: on a cropped axis the ticks can
// be 250 lines apart, and `406k / 406k / 407k` would read as a broken scale.
function fmtCountTick(n) {
  if (n === 0) return '0';
  const trim = (x) => String(+x.toFixed(2));
  const a = Math.abs(n);
  if (a >= 1e6) return `${trim(n / 1e6)}M`;
  if (a >= 1e3) return `${trim(n / 1e3)}k`;
  return String(Math.round(n));
}

// `AppleClang-21.0.0` -> `AppleClang 21`, `GNU-13.3.0` -> `GNU 13.3`.
function prettyCC(cc) {
  const dash = cc.lastIndexOf('-');
  if (dash < 0) return cc;
  const name = cc.slice(0, dash);
  const parts = cc.slice(dash + 1).split('.');
  const ver = parts[1] && parts[1] !== '0'
    ? `${parts[0]}.${parts[1]}`
    : parts[0];
  return `${name} ${ver}`;
}

const esc = (s) => String(s).replace(/[&<>"']/g, (c) => (
  { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]
));

// ── DATA ────────────────────────────────────────────────────────────────────

function parseNDJSON(text) {
  const out = [];
  for (const line of text.split('\n')) {
    const t = line.trim();
    if (!t) continue;
    try {
      out.push(JSON.parse(t));
    } catch {
      // A torn final line is expected while CI is mid-append. Drop it.
    }
  }
  return out;
}

// build_type is single-valued today but need not stay so, so it is part of the
// key. It is only shown in the label when the data actually varies.
function envKey(r) {
  return [r.build_type, r.os, r.cc, r.nproc, r.jit ? 'jit' : 'nojit'].join('|');
}

// A sharded job (TUR_TEST_SHARD="i/N") reports the same suite once per shard,
// each with ~1/N of the work.  Keyed on the bare name they would land in one
// series as N points per run at a fraction of the real duration -- a suite that
// appears to have got faster and noisier on the day sharding was switched on.
// The shard is therefore part of the series identity, so each slice trends
// against its own history.  Unsharded rows (shard_total null, which is every
// row written before sharding existed) keep the bare name and their continuity.
function seriesName(r) {
  return r.shard_total ? `${r.suite} [${r.shard_index}/${r.shard_total}]` : r.suite;
}

function buildEnvs(rows) {
  const seen = new Map();
  for (const r of rows) {
    const key = envKey(r);
    const cur = seen.get(key);
    if (!cur) seen.set(key, { key, row: r, ts: r.ts, suites: new Set([seriesName(r)]) });
    else {
      if (r.ts > cur.ts) cur.ts = r.ts;
      cur.suites.add(seriesName(r));
    }
  }

  const buildTypes = new Set(rows.map((r) => r.build_type));
  const envs = [...seen.values()].map(({ key, row, ts, suites }) => {
    const bits = [row.os, prettyCC(row.cc), `${row.nproc} cores`];
    if (row.jit) bits.push('JIT');
    if (buildTypes.size > 1) bits.unshift(row.build_type);
    return { key, label: bits.join(' · '), ts, breadth: suites.size };
  });

  const newest = envs.reduce((m, e) => Math.max(m, e.ts), 0);

  // Still-active environments first, then the BROADEST of those — not simply
  // the freshest, which is how the page used to open on three sparklines.
  //
  // The JIT legs are their own environment (jit is part of the tuple) but
  // register only ~3 ctest suites, and they publish seconds apart from the
  // ~176-suite legs in the same workflow. Ordering on `ts` alone therefore
  // decided the default view on which of two near-simultaneous uploads landed
  // last: a coin flip between the whole dashboard and 2% of it.
  //
  // "Active" is a day rather than an exact tie because the macOS 5-core runner
  // is a different image that appears intermittently; it should rank below
  // today's runs without being hidden.
  const DAY = 86400;
  envs.sort((a, b) => (
    (a.ts >= newest - DAY ? 0 : 1) - (b.ts >= newest - DAY ? 0 : 1)
    || b.breadth - a.breadth
    || b.ts - a.ts
    || a.label.localeCompare(b.label)
  ));
  return envs;
}

// Relative to the newest row in `rows`, not to wall-clock now: if CI has been
// quiet for two days, "last 24 hours" must still show the last day that has
// data rather than an empty chart. Each dataset therefore anchors on its own
// latest row.
function rangeCutoff(rows) {
  if (state.range === 'all') return -Infinity;
  const days = parseInt(state.range, 10);
  const latest = rows.reduce((m, r) => Math.max(m, r.ts), 0);
  return latest - days * 86400;
}

// Every view starts here. Status filtering is deliberately NOT applied to the
// env/run axis — a run where a suite failed is still a run.
function envRows() {
  const cutoff = rangeCutoff(state.rows);
  return state.rows.filter((r) => envKey(r) === state.env && r.ts >= cutoff);
}

function locRows() {
  const cutoff = rangeCutoff(state.loc);
  return state.loc.filter((r) => r.ts >= cutoff);
}

function docsRows() {
  const cutoff = rangeCutoff(state.docs);
  return state.docs.filter((r) => r.ts >= cutoff);
}

// The newest docs row that actually carries `key`, for the headline numbers:
// the newest row may predate the field (published before it shipped) or lack the
// spices checkout, and "--" there is more honest than a zero.
function latestDocWith(key) {
  for (let i = state.docs.length - 1; i >= 0; i--) {
    if (state.docs[i][key] != null) return state.docs[i];
  }
  return null;
}

// suite -> [{ ts, ms, status, sha, run_id }], ascending by ts.
function bySuite(rows) {
  const m = new Map();
  for (const r of rows) {
    if (!state.statuses.has(r.status)) continue;
    const key = seriesName(r);
    let a = m.get(key);
    if (!a) m.set(key, (a = []));
    a.push({
      ts: r.ts,
      ms: r.duration_ms,
      status: r.status,
      sha: r.sha,
      run_id: r.run_id,
    });
  }
  for (const a of m.values()) a.sort((x, y) => x.ts - y.ts);
  return m;
}

function mean(xs) { return xs.reduce((a, b) => a + b, 0) / xs.length; }

function quantile(sorted, q) {
  if (!sorted.length) return 0;
  const pos = (sorted.length - 1) * q;
  const lo = Math.floor(pos);
  const hi = Math.ceil(pos);
  return lo === hi ? sorted[lo] : sorted[lo] + (sorted[hi] - sorted[lo]) * (pos - lo);
}

function suiteStats(series) {
  const stats = [];
  for (const [suite, pts] of series) {
    const ms = pts.map((p) => p.ms);
    const sorted = [...ms].sort((a, b) => a - b);
    const first = ms[0];
    const last = ms[ms.length - 1];
    stats.push({
      suite,
      runs: pts.length,
      mean: mean(ms),
      p90: quantile(sorted, 0.9),
      last,
      delta: last - first,
      deltaPct: first > 0 ? ((last - first) / first) * 100 : 0,
      status: pts[pts.length - 1].status,
      pts,
    });
  }
  return stats;
}

// Color is bound to the suite's SLOT, which it holds until it is deselected.
// Removing another series never recolors it.
function colorOf(suite) {
  const i = state.suites.indexOf(suite);
  return i < 0 ? null : `var(${SERIES_VARS[i]})`;
}

// Selected suites in slot order, holes dropped.
function selected() {
  return state.suites.filter(Boolean);
}

function selectSuite(suite) {
  if (state.suites.includes(suite)) return;
  const free = state.suites.indexOf(null);
  if (free < 0) return;
  state.suites[free] = suite;
}

function deselectSuite(suite) {
  const i = state.suites.indexOf(suite);
  if (i >= 0) state.suites[i] = null;
}

// ── URL STATE ───────────────────────────────────────────────────────────────

function readURL() {
  const q = new URLSearchParams(location.search);
  return {
    env: q.get('env'),
    // Keep empty entries: they are slot holes, not junk.
    suites: q.get('suites') ? q.get('suites').split(',') : null,
    statuses: q.get('status') ? q.get('status').split(',').filter(Boolean) : null,
    range: q.get('range'),
    scale: q.get('scale'),
    loc: q.get('loc') ? q.get('loc').split(',').filter(Boolean) : null,
    docs: q.get('docs') ? q.get('docs').split(',').filter(Boolean) : null,
    tab: q.get('tab'),
  };
}

function writeURL() {
  const q = new URLSearchParams();
  if (state.env) q.set('env', state.env);
  // Holes are serialized as empty entries ("a,,c") so slots — and therefore
  // colors — survive a reload or a shared link.
  if (selected().length) {
    q.set('suites', state.suites.map((s) => s ?? '').join(',').replace(/,+$/, ''));
  }
  if (state.statuses.size !== STATUSES.length) {
    q.set('status', [...state.statuses].join(','));
  }
  if (state.range !== DEFAULT_RANGE) q.set('range', state.range);
  if (state.scale !== 'linear') q.set('scale', state.scale);
  // Only when it differs from the default pair, so a bare link stays bare.
  const loc = LOC_SERIES.map(([k]) => k).filter((k) => state.locKeys.has(k));
  if (loc.join(',') !== LOC_DEFAULT.join(',')) q.set('loc', loc.join(','));
  const docs = DOCS_SERIES.map(([k]) => k).filter((k) => state.docsKeys.has(k));
  if (docs.join(',') !== DOCS_DEFAULT.join(',')) q.set('docs', docs.join(','));
  if (state.tab !== 'tests') q.set('tab', state.tab);
  history.replaceState(null, '', q.toString() ? `?${q}` : location.pathname);
}

// ── SVG HELPERS ─────────────────────────────────────────────────────────────

// CSS custom properties do not resolve in SVG presentation attributes, only in
// the style property — hence style="stroke:var(--chart-1)" throughout.
const svgEl = (tag, attrs, style) => {
  const a = Object.entries(attrs)
    .map(([k, v]) => `${k}="${esc(v)}"`)
    .join(' ');
  return `<${tag} ${a}${style ? ` style="${style}"` : ''} />`;
};

// Durations are not decimal: a plain 1/2/5 ladder puts ticks on 3.3-minute
// boundaries, which render as 3m / 7m / 10m / 13m and read as an error. Step
// on values that are round in time units instead.
const TICK_STEPS_MS = [
  1, 2, 5, 10, 25, 50, 100, 250, 500,
  1_000, 2_000, 5_000, 10_000, 15_000, 30_000,
  60_000, 120_000, 300_000, 600_000, 900_000, 1_800_000,
  3_600_000, 7_200_000, 21_600_000,
];

function niceTicks(lo, hi, count) {
  if (hi <= lo) return [lo];
  const span = hi - lo;
  const step = TICK_STEPS_MS.find((s) => span / s <= count)
    ?? TICK_STEPS_MS[TICK_STEPS_MS.length - 1];
  const out = [];
  for (let v = Math.ceil(lo / step) * step; v <= hi + 1e-9; v += step) out.push(v);
  return out;
}

function logTicks(lo, hi) {
  const out = [];
  for (let e = Math.floor(Math.log10(lo)); e <= Math.ceil(Math.log10(hi)); e++) {
    for (const m of [1, 3]) {
      const v = m * Math.pow(10, e);
      if (v >= lo && v <= hi) out.push(v);
    }
  }
  return out.length >= 2 ? out : [lo, hi];
}

// Counts, unlike durations, ARE decimal, so the ordinary 1/2/2.5/5 ladder is
// the right one here — the TICK_STEPS_MS table above exists only because time
// is not base ten.
function decimalTicks(lo, hi, count) {
  if (hi <= lo) return [lo];
  const span = hi - lo;
  const mag = Math.pow(10, Math.floor(Math.log10(span / count)));
  const step = [1, 2, 2.5, 5, 10].map((m) => m * mag).find((s) => span / s <= count)
    ?? mag * 10;
  const out = [];
  for (let v = Math.ceil(lo / step) * step; v <= hi + step * 1e-9; v += step) {
    out.push(v);
  }
  return out;
}

// Whole numbers only: the bar chart counts reports, and a tick at 0.5 (or the
// 2.5 the decimal ladder allows) is a label for a value that cannot occur.
function intTicks(lo, hi, count) {
  const t = decimalTicks(lo, hi, count);
  const step = Math.max(1, Math.ceil(t.length > 1 ? t[1] - t[0] : 1));
  const out = [];
  for (let v = Math.ceil(lo / step) * step; v <= hi + 1e-9; v += step) out.push(v);
  return out;
}

// ── RENDER: PROVENANCE + TILES ──────────────────────────────────────────────

function renderProvenance() {
  const rows = state.rows.filter((r) => envKey(r) === state.env);
  const latestTs = rows.reduce((m, r) => Math.max(m, r.ts), 0);
  const latest = rows.find((r) => r.ts === latestTs);
  if (!latest) return;

  const short = latest.sha.slice(0, 7);
  document.getElementById('ci-provenance').innerHTML = `
    <span>Latest run</span>
    <a class="mono ci-link"
       href="${GITHUB_URL}/commit/${esc(latest.sha)}">${esc(short)}</a>
    <span class="dot-sep">/</span>
    <span>${esc(fmtDateTime(latestTs))}</span>
    <span class="dot-sep">/</span>
    <span>${state.rows.length.toLocaleString()} rows on
      <span class="mono">${state.windowed
        ? 'suite-timings-recent.jsonl'
        : `suite-timings-${esc(state.year ?? '')}.jsonl`}</span></span>`;
}

function renderTiles() {
  const rows = state.rows.filter((r) => envKey(r) === state.env);
  const latestTs = rows.reduce((m, r) => Math.max(m, r.ts), 0);
  const latest = rows.filter((r) => r.ts === latestTs);

  const total = latest.reduce((a, r) => a + r.duration_ms, 0);
  const failed = latest.filter((r) => r.status === 'fail');
  const skipped = latest.filter(
    (r) => r.status === 'skip' || r.partial_skip_reason != null,
  );

  const tile = (cls, icon, label, value, note) => `
    <div class="ci-tile ${cls}">
      <div class="ci-tile-label">
        ${icon ? `<t-icon name="${icon}"></t-icon>` : ''}${esc(label)}
      </div>
      <div class="ci-tile-value">${value}</div>
      <div class="ci-tile-note">${esc(note)}</div>
    </div>`;

  document.getElementById('ci-tiles').innerHTML = [
    tile('', 'clock', 'Total wall time', esc(fmtDuration(total)),
      'Sum of every suite in the latest run'),
    tile('', 'layers', 'Suites', String(latest.length),
      `${new Set(rows.map(seriesName)).size} seen in range`),
    tile(
      failed.length ? 'is-fail' : 'is-clean',
      failed.length ? 'circle-x' : 'circle-check',
      failed.length ? 'Failing' : 'All passing',
      String(failed.length),
      failed.length ? failed.map(seriesName).join(', ') : 'No failures in the latest run',
    ),
    tile(skipped.length ? 'is-skip' : '', 'circle-minus', 'Skipped',
      String(skipped.length), 'Fully or partially skipped'),
  ].join('');
}

// ── RENDER: FILTERS ─────────────────────────────────────────────────────────

function renderFilters() {
  const allSuites = [...new Set(
    state.rows.filter((r) => envKey(r) === state.env).map(seriesName),
  )].sort();

  const chosen = selected();
  const atCap = chosen.length >= MAX_SERIES;

  document.getElementById('ci-filters').innerHTML = `
    <div class="ci-field ci-field--env">
      <label class="ci-field-label" for="ci-env">
        Environment <span class="hint">— timings compare only within one</span>
      </label>
      <select class="ci-select" id="ci-env">
        ${state.envs.map((e) => `
          <option value="${esc(e.key)}"${e.key === state.env ? ' selected' : ''}>
            ${esc(e.label)}
          </option>`).join('')}
      </select>
    </div>

    <div class="ci-field">
      <label class="ci-field-label" for="ci-add-suite">
        Suites
        <span class="hint">— ${chosen.length} of ${MAX_SERIES}${
          atCap ? ', deselect one to add another' : ''
        }</span>
      </label>
      <select class="ci-select" id="ci-add-suite"${atCap ? ' disabled' : ''}>
        <option value="">${atCap ? 'Maximum reached' : 'Add a suite…'}</option>
        ${allSuites
          .filter((s) => !state.suites.includes(s))
          .map((s) => `<option value="${esc(s)}">${esc(s)}</option>`)
          .join('')}
      </select>
    </div>

    <div class="ci-field">
      <span class="ci-field-label">Status</span>
      <div class="ci-chips">
        ${STATUSES.map((s) => `
          <button type="button" class="ci-chip" data-status="${s}"
                  aria-pressed="${state.statuses.has(s)}">
            <span class="dot" style="background:var(--status-${s})"></span>${s}
          </button>`).join('')}
      </div>
    </div>

`;

  document.getElementById('ci-env').onchange = (e) => {
    state.env = e.target.value;
    state.suites = defaultSuites();
    render();
  };

  document.getElementById('ci-add-suite').onchange = (e) => {
    if (!e.target.value) return;
    selectSuite(e.target.value);
    render();
  };

  for (const btn of document.querySelectorAll('.ci-chip[data-status]')) {
    btn.onclick = () => {
      const s = btn.dataset.status;
      if (state.statuses.has(s)) {
        // Never let the last one go — an empty status set is an empty page.
        if (state.statuses.size > 1) state.statuses.delete(s);
      } else {
        state.statuses.add(s);
      }
      render();
    };
  }
}

// ── THE CHART ───────────────────────────────────────────────────────────────
//
// Both time-series charts on this page go through here. They differ only in
// units and in what a point knows about itself; the axis conventions — the pixel
// thinning on x, the tick ladder on y, the collision-avoiding direct labels, the
// one-tooltip-per-x hover — are the part worth having exactly once. A second
// hand-rolled chart is how a page ends up with two subtly different notions of
// what a gridline means.
//
//   host      element to draw into (its clientWidth sets the viewBox width)
//   tooltip   the absolutely-positioned tooltip element, a sibling of `host`
//   idPrefix  namespaces the crosshair/hit-area ids, since two charts coexist
//   series    [{ key, label, color, pts: [{ x, y, fail?, sha? }] }], ascending x
//   xs        the full x domain, so the axis holds still as series come and go
//   ticksY    (lo, hi) => number[]        -- used when `log` is false
//   fmtY      (v) => string               -- tooltip values
//   fmtTickY  (v) => string               -- axis labels
//   footer    axis-title text under the plot
//   label     the SVG's aria-label
//   height    plot height; a small multiple passes less than the default
//   directLabels  false where a title already names the one series
function drawLineChart({
  host, tooltip, idPrefix, series, xs, log = false,
  ticksY, fmtY, fmtTickY, footer, label, height = 320, directLabels = true,
}) {
  const x0 = xs[0];
  const x1 = xs[xs.length - 1];

  const W = Math.max(360, host.clientWidth || 860);
  const H = height;
  const showLabels = directLabels && series.length <= 4 && W >= 640;
  const pad = { t: 18, r: showLabels ? 128 : 24, b: 40, l: 62 };
  const iw = W - pad.l - pad.r;
  const ih = H - pad.t - pad.b;

  const values = series.flatMap((s) => s.pts.map((p) => p.y));
  const vmax = Math.max(...values);
  const vmin = Math.min(...values);

  // Linear always includes zero. Not a style choice: a cropped axis is the
  // oldest way to make noise look like a trend, and both series here are read
  // against each other.
  let yLo;
  let yHi;
  if (log) {
    // Log needs a positive floor; durations and counts bottom out at 1.
    yLo = Math.max(1, vmin * 0.7);
    yHi = vmax * 1.3;
  } else {
    // Math.min/max with 0 so a series that dips below zero keeps its baseline
    // on the chart; for all-positive data this is yLo = 0 and yHi = vmax *
    // 1.08, as it has always been.
    yLo = Math.min(0, vmin);
    yHi = Math.max(0, vmax);
    const slack = (yHi - yLo) * 0.08 || 1;
    yHi += slack;
    if (yLo < 0) yLo -= slack;
  }

  const sx = (x) => (x1 === x0 ? pad.l + iw / 2 : pad.l + ((x - x0) / (x1 - x0)) * iw);
  const sy = (v) => {
    if (!log) return pad.t + ih - ((v - yLo) / (yHi - yLo)) * ih;
    const lv = Math.log10(Math.max(v, yLo));
    return pad.t + ih - ((lv - Math.log10(yLo)) / (Math.log10(yHi) - Math.log10(yLo))) * ih;
  };

  const parts = [];

  // Grid + y axis. Recessive: hairline rules, dim monospace labels.
  for (const t of (log ? logTicks(yLo, yHi) : ticksY(yLo, yHi))) {
    const y = sy(t);
    parts.push(svgEl('line', {
      class: 'ci-grid-line', x1: pad.l, x2: pad.l + iw, y1: y, y2: y,
    }));
    parts.push(`<text class="ci-axis-text" x="${pad.l - 10}" y="${y + 3}"
      text-anchor="end">${esc(fmtTickY(t))}</text>`);
  }

  // X axis: one tick per run, thinned by PIXEL distance. Thinning by index
  // instead lets labels collide, because runs cluster in time — several
  // pushes in an afternoon land almost on top of each other.
  const X_GAP = 62;
  const keep = [];
  for (const x of xs) {
    if (!keep.length || sx(x) - sx(keep[keep.length - 1]) >= X_GAP) keep.push(x);
  }
  // The most recent run is the one worth labeling, so make room for it.
  const lastX = xs[xs.length - 1];
  if (keep[keep.length - 1] !== lastX) {
    while (keep.length && sx(lastX) - sx(keep[keep.length - 1]) < X_GAP) keep.pop();
    keep.push(lastX);
  }
  for (const x of keep) {
    parts.push(`<text class="ci-axis-text" x="${sx(x)}" y="${pad.t + ih + 18}"
      text-anchor="middle">${esc(fmtDate(x))}</text>`);
  }
  parts.push(`<text class="ci-axis-title" x="${pad.l}" y="${H - 4}">
    ${esc(footer)}${log ? ' · log scale' : ''}</text>`);

  // Series, in the fixed palette order.
  const labelSlots = [];
  for (const s of series) {
    const d = s.pts
      .map((p, i) => `${i ? 'L' : 'M'}${sx(p.x).toFixed(1)},${sy(p.y).toFixed(1)}`)
      .join(' ');
    parts.push(`<path class="ci-series-line" d="${d}" style="stroke:${s.color}" />`);

    for (const p of s.pts) {
      if (p.fail) {
        // A red run is a fact about the trend line, so it gets its own mark.
        parts.push(svgEl('circle', {
          class: 'ci-point-fail', cx: sx(p.x), cy: sy(p.y), r: 5,
        }));
      } else {
        parts.push(svgEl('circle', {
          class: 'ci-point', cx: sx(p.x), cy: sy(p.y), r: 3.5,
        }, `fill:${s.color}`));
      }
    }

    if (showLabels) {
      const last = s.pts[s.pts.length - 1];
      labelSlots.push({
        text: s.label, color: s.color, y: sy(last.y), x: sx(last.x) + 10,
      });
    }
  }

  // Direct labels, pushed apart so they never collide.
  labelSlots.sort((a, b) => a.y - b.y);
  const GAP = 15;
  for (let i = 1; i < labelSlots.length; i++) {
    if (labelSlots[i].y - labelSlots[i - 1].y < GAP) {
      labelSlots[i].y = labelSlots[i - 1].y + GAP;
    }
  }
  const overflow = labelSlots.length && labelSlots[labelSlots.length - 1].y - (pad.t + ih);
  if (overflow > 0) for (const l of labelSlots) l.y -= overflow;
  for (const l of labelSlots) {
    const name = l.text.length > 17 ? `${l.text.slice(0, 16)}…` : l.text;
    parts.push(`<text class="ci-direct-label" x="${l.x}" y="${l.y}"
      style="fill:${l.color}">${esc(name)}</text>`);
  }

  // Hover layer: one crosshair + one shared tooltip per run.
  parts.push(svgEl('line', {
    class: 'ci-crosshair', id: `${idPrefix}-cross`,
    x1: 0, x2: 0, y1: pad.t, y2: pad.t + ih, opacity: 0,
  }));
  parts.push(svgEl('rect', {
    class: 'ci-hit', id: `${idPrefix}-hit`,
    x: pad.l, y: pad.t, width: iw, height: ih,
  }));

  host.innerHTML = `<svg viewBox="0 0 ${W} ${H}" width="${W}" height="${H}"
    role="img" aria-label="${esc(label)}">${parts.join('')}</svg>`;

  wireHover({ host, tooltip, idPrefix, series, sx, fmtY, pad, ih });
}

function wireHover({ host, tooltip, idPrefix, series, sx, fmtY, pad, ih }) {
  const svg = host.querySelector('svg');
  const hit = host.querySelector(`#${idPrefix}-hit`);
  const cross = host.querySelector(`#${idPrefix}-cross`);
  if (!svg || !hit) return;

  // Snap only to x values a DRAWN series has a point at. The axis spans every
  // run in the environment, but about a quarter of those carry none of the
  // selected suites (other jobs in the env upload on their own clock), and
  // snapping to one of them hid the tooltip over a stretch of plot that still
  // showed lines -- including, on 2026-10-08's data, the plot's center.
  const xs = [...new Set(series.flatMap((s) => s.pts.map((p) => p.x)))];

  const hide = () => {
    tooltip.hidden = true;
    cross.setAttribute('opacity', 0);
  };

  hit.addEventListener('mouseleave', hide);
  hit.addEventListener('mousemove', (ev) => {
    const box = svg.getBoundingClientRect();
    // The SVG is width:100% with a fixed viewBox, so map client px back into
    // user units before comparing against the scale.
    const ux = ((ev.clientX - box.left) / box.width) * svg.viewBox.baseVal.width;

    let best = xs[0];
    let bestD = Infinity;
    for (const x of xs) {
      const d = Math.abs(sx(x) - ux);
      if (d < bestD) { bestD = d; best = x; }
    }

    const cx = sx(best);
    cross.setAttribute('x1', cx);
    cross.setAttribute('x2', cx);
    cross.setAttribute('opacity', 1);

    const rows = [];
    let sha = '';
    for (const s of series) {
      const p = s.pts.find((q) => q.x === best);
      if (!p) continue;
      if (p.sha) sha = p.sha;
      rows.push(`
        <div class="ci-tooltip-row">
          <span class="dot" style="background:${s.color}"></span>
          <span class="name">${esc(s.label)}</span>
          <span class="val">${esc(fmtY(p.y))}</span>
        </div>`);
    }
    if (!rows.length) { hide(); return; }

    tooltip.innerHTML = `
      <div class="ci-tooltip-head">
        <span>${esc(fmtDateTime(best))}</span>
        <span class="mono">${esc(sha.slice(0, 7))}</span>
      </div>${rows.join('')}`;
    tooltip.hidden = false;

    // Keep the tooltip inside the panel; flip it left near the right edge.
    const scale = box.width / svg.viewBox.baseVal.width;
    const px = cx * scale;
    const flip = px + tooltip.offsetWidth + 20 > box.width;
    tooltip.style.left = `${flip ? px - tooltip.offsetWidth - 14 : px + 14}px`;
    tooltip.style.top = `${Math.max(0, (pad.t + ih / 2) * scale - tooltip.offsetHeight / 2)}px`;
  });
}

// ── THE BAR CHART ───────────────────────────────────────────────────────────
//
// One column per time bucket, for a quantity with a sign. Above the zero line
// in one hue, below it in the other, so polarity reads by position first and
// color second. The axis vocabulary is drawLineChart's -- hairline grid, dim
// tick labels, x labels thinned by pixel distance -- and so is the hover: one
// tooltip, for the bucket under the pointer.
//
//   buckets   [{ label, y, head, rows: [[name, value, color?]] }], in x order
//   ticksY    (lo, hi) => number[]
//   fmtTickY  (v) => string
//   footer    axis-title text under the plot
//   label     the SVG's aria-label
function drawBarChart({
  host, tooltip, idPrefix, buckets, ticksY, fmtTickY, footer, label,
  posColor, negColor,
}) {
  const W = Math.max(360, host.clientWidth || 860);
  const H = 280;
  const pad = { t: 18, r: 24, b: 40, l: 62 };
  const iw = W - pad.l - pad.r;
  const ih = H - pad.t - pad.b;

  // Zero is always on the axis -- it is the line the whole chart is read
  // against -- and an all-zero range still gets one unit either side of it.
  const ys = buckets.map((b) => b.y);
  let yLo = Math.min(0, ...ys);
  let yHi = Math.max(0, ...ys);
  if (yHi === yLo) { yLo = -1; yHi = 1; }
  const slack = (yHi - yLo) * 0.08;
  if (yHi > 0) yHi += slack;
  if (yLo < 0) yLo -= slack;
  const sy = (v) => pad.t + ih - ((v - yLo) / (yHi - yLo)) * ih;

  // Columns are capped at 24px and never fill their band: the air between
  // them is what makes each one a day rather than a slab of a histogram.
  const bw = iw / buckets.length;
  const colW = Math.max(2, Math.min(24, bw * 0.6));
  const bx = (i) => pad.l + i * bw + (bw - colW) / 2;

  const parts = [];
  for (let i = 0; i < buckets.length; i++) {
    parts.push(svgEl('rect', {
      class: 'ci-band-hover', 'data-i': i,
      x: pad.l + i * bw, y: pad.t, width: bw, height: ih,
    }));
  }
  for (const t of ticksY(yLo, yHi)) {
    const y = sy(t);
    parts.push(svgEl('line', {
      class: t === 0 ? 'ci-zero-line' : 'ci-grid-line',
      x1: pad.l, x2: pad.l + iw, y1: y, y2: y,
    }));
    parts.push(`<text class="ci-axis-text" x="${pad.l - 10}" y="${y + 3}"
      text-anchor="end">${esc(fmtTickY(t))}</text>`);
  }

  // Square where the column meets zero, a 4px round at its data end.
  const y0 = sy(0);
  buckets.forEach((b, i) => {
    if (!b.y) return;
    const x = bx(i);
    const yv = sy(b.y);
    const h = Math.abs(yv - y0);
    const r = Math.min(4, colW / 2, h);
    const up = b.y > 0;
    const k = up ? r : -r;   // the round turns back toward the zero line
    const d = `M${x},${y0} V${yv + k} Q${x},${yv} ${x + r},${yv} H${x + colW - r}`
      + ` Q${x + colW},${yv} ${x + colW},${yv + k} V${y0} Z`;
    parts.push(`<path class="${up ? 'ci-bar-pos' : 'ci-bar-neg'}" d="${d}"
      style="fill:${up ? posColor : negColor}" />`);
  });

  // X labels thinned by pixel distance, keeping the newest bucket.
  const X_GAP = 62;
  const keep = [];
  const cx = (i) => pad.l + i * bw + bw / 2;
  for (let i = 0; i < buckets.length; i++) {
    if (!keep.length || cx(i) - cx(keep[keep.length - 1]) >= X_GAP) keep.push(i);
  }
  const last = buckets.length - 1;
  if (keep[keep.length - 1] !== last) {
    while (keep.length && cx(last) - cx(keep[keep.length - 1]) < X_GAP) keep.pop();
    keep.push(last);
  }
  for (const i of keep) {
    parts.push(`<text class="ci-axis-text" x="${cx(i)}" y="${pad.t + ih + 18}"
      text-anchor="middle">${esc(buckets[i].label)}</text>`);
  }
  parts.push(`<text class="ci-axis-title" x="${pad.l}" y="${H - 4}">${esc(footer)}</text>`);

  parts.push(svgEl('rect', {
    class: 'ci-hit', id: `${idPrefix}-hit`, x: pad.l, y: pad.t, width: iw, height: ih,
  }));

  host.innerHTML = `<svg viewBox="0 0 ${W} ${H}" width="${W}" height="${H}"
    role="img" aria-label="${esc(label)}">${parts.join('')}</svg>`;

  const svg = host.querySelector('svg');
  const hit = host.querySelector(`#${idPrefix}-hit`);
  const bands = [...host.querySelectorAll('.ci-band-hover')];
  const hide = () => {
    tooltip.hidden = true;
    for (const band of bands) band.classList.remove('is-on');
  };
  hit.addEventListener('mouseleave', hide);
  hit.addEventListener('mousemove', (ev) => {
    const box = svg.getBoundingClientRect();
    const ux = ((ev.clientX - box.left) / box.width) * svg.viewBox.baseVal.width;
    const i = Math.max(0, Math.min(last, Math.floor((ux - pad.l) / bw)));
    const b = buckets[i];
    bands.forEach((band, j) => band.classList.toggle('is-on', j === i));
    tooltip.innerHTML = `
      <div class="ci-tooltip-head"><span>${esc(b.head)}</span></div>
      ${b.rows.map(([name, value, color]) => `
        <div class="ci-tooltip-row">
          <span class="dot" style="background:${color ?? 'transparent'}"></span>
          <span class="name">${esc(name)}</span>
          <span class="val">${esc(value)}</span>
        </div>`).join('')}`;
    tooltip.hidden = false;
    const scale = box.width / svg.viewBox.baseVal.width;
    const px = cx(i) * scale;
    const flip = px + tooltip.offsetWidth + 20 > box.width;
    tooltip.style.left = `${flip ? px - tooltip.offsetWidth - 14 : px + 14}px`;
    tooltip.style.top = `${Math.max(0, (pad.t + ih / 2) * scale - tooltip.offsetHeight / 2)}px`;
  });
}

// ── RENDER: SUITE DURATIONS ─────────────────────────────────────────────────

function renderChart() {
  const host = document.getElementById('ci-chart');
  const sub = document.getElementById('ci-chart-sub');
  const legend = document.getElementById('ci-legend');
  const rows = envRows();
  const bucketed = bySuite(rows);

  const drawn = selected()
    .map((s) => ({ suite: s, pts: bucketed.get(s) ?? [] }))
    .filter((s) => s.pts.length);

  sub.textContent = state.envs.find((e) => e.key === state.env)?.label ?? '';

  if (!drawn.length) {
    host.innerHTML = `<div class="ci-empty">
      No data for the selected suites in this environment and range.
    </div>`;
    legend.innerHTML = '';
    return;
  }

  // The x domain comes from every run in the environment, not just the
  // selected suites, so the axis holds still while you swap series in and out.
  const xs = [...new Set(rows.map((r) => r.ts))].sort((a, b) => a - b);

  drawLineChart({
    host,
    tooltip: document.getElementById('ci-tooltip'),
    idPrefix: 'ci',
    series: drawn.map(({ suite, pts }) => ({
      key: suite,
      label: suite,
      color: colorOf(suite),
      pts: pts.map((p) => ({
        x: p.ts, y: p.ms, fail: p.status === 'fail', sha: p.sha,
      })),
    })),
    xs,
    log: state.scale === 'log',
    ticksY: (lo, hi) => niceTicks(lo, hi, 5),
    fmtY: fmtDuration,
    fmtTickY: fmtTick,
    footer: `${xs.length} run${xs.length === 1 ? '' : 's'}`,
    label: 'Suite duration over time',
  });

  renderLegend(drawn);
}

function renderLegend(drawn) {
  // Always present for >= 2 series; for one, the panel title already names it.
  document.getElementById('ci-legend').innerHTML = drawn.map(({ suite }) => `
    <button type="button" class="ci-legend-item" data-suite="${esc(suite)}"
            title="Remove ${esc(suite)} from the chart">
      <span class="swatch" style="background:${colorOf(suite)}"></span>
      ${esc(suite)}
      <span class="x">×</span>
    </button>`).join('');

  for (const b of document.querySelectorAll('#ci-legend .ci-legend-item')) {
    b.onclick = () => {
      deselectSuite(b.dataset.suite);
      render();
    };
  }
}

// ── RENDER: LINE COUNTS ─────────────────────────────────────────────────────

function renderLoc() {
  const host = document.getElementById('ci-loc-chart');
  const legend = document.getElementById('ci-loc-legend');
  const sub = document.getElementById('ci-loc-sub');

  if (state.locError) {
    host.innerHTML = `<div class="ci-empty">${esc(state.locError)}</div>`;
    legend.innerHTML = '';
    sub.textContent = '';
    return;
  }

  const rows = locRows();
  const xs = rows.map((r) => r.ts);
  const latest = state.loc[state.loc.length - 1];

  // The headline is the newest commit measured, not the newest in range: the
  // range narrows the trend, never what the repo currently is.
  sub.innerHTML = latest
    ? `${esc(fmtCount(latest.code_lines))} product / ${esc(fmtCount(latest.test_lines))} test
       tracked lines at
       <a class="mono ci-link"
          href="${GITHUB_URL}/commit/${esc(latest.sha ?? '')}"
          >${esc((latest.sha ?? '').slice(0, 7))}</a>.
       Blank lines and comments included; committed machine output is counted
       separately.`
    : '';

  // One point draws a dot and no line, which reads as a broken chart rather
  // than as "one push so far", so say it instead -- and say which of the two
  // it is, since widening the range only helps when there is more outside it.
  if (rows.length < 2) {
    host.innerHTML = `<div class="ci-empty">
      ${state.loc.length > rows.length
        ? 'Only one measurement in this range -- widen it to see a trend.'
        : 'Only one push has been measured so far. A trend needs two.'}
    </div>`;
    renderLocLegend();
    return;
  }

  const series = LOC_SERIES
    .filter(([key]) => state.locKeys.has(key))
    .map(([key, label, cssVar]) => ({
      key,
      label,
      color: `var(${cssVar})`,
      pts: rows.map((r) => ({ x: r.ts, y: r[key] ?? 0, sha: r.sha })),
    }));

  drawLineChart({
    host,
    tooltip: document.getElementById('ci-loc-tooltip'),
    idPrefix: 'ci-loc',
    series,
    xs,
    ticksY: (lo, hi) => decimalTicks(lo, hi, 5),
    fmtY: fmtCount,
    fmtTickY: fmtCountTick,
    footer: `${xs.length} commit${xs.length === 1 ? '' : 's'}`,
    label: 'Tracked lines of code over time',
  });

  renderLocLegend();
}

// Doubles as the series picker: the swatch and label identify, the number is
// the latest value, and clicking toggles. Never color alone.
function renderLocLegend() {
  const latest = state.loc[state.loc.length - 1];
  document.getElementById('ci-loc-legend').innerHTML = LOC_SERIES.map(
    ([key, label, cssVar]) => {
      const on = state.locKeys.has(key);
      return `
        <button type="button" class="ci-legend-item ci-legend-toggle"
                data-loc="${esc(key)}" aria-pressed="${on}">
          <span class="swatch" style="background:${on ? `var(${cssVar})` : 'var(--text-dim)'}"></span>
          ${esc(label)}
          <span class="count">${esc(latest ? fmtCount(latest[key]) : '--')}</span>
        </button>`;
    },
  ).join('');

  for (const b of document.querySelectorAll('#ci-loc-legend .ci-legend-toggle')) {
    b.onclick = () => {
      const key = b.dataset.loc;
      if (state.locKeys.has(key)) {
        // Never let the last one go — an empty picker is an empty chart, and
        // an empty `loc` query param cannot round-trip through the URL.
        if (state.locKeys.size > 1) state.locKeys.delete(key);
      } else {
        state.locKeys.add(key);
      }
      renderLoc();
      writeURL();
    };
  }
}

// ── RENDER: OPEN PLANS AND REPORTS ──────────────────────────────────────────

function renderDocsTiles() {
  const latest = state.docs[state.docs.length - 1];
  const host = document.getElementById('ci-docs-tiles');
  if (!latest) {
    host.innerHTML = '';
    return;
  }

  const latestGuides = latestDocWith('turmeric_guides');
  const latestSpiceGuides = latestDocWith('spice_guides');

  const tile = (cls, icon, label, value, note) => `
    <div class="ci-tile ${cls}">
      <div class="ci-tile-label">
        ${icon ? `<t-icon name="${icon}"></t-icon>` : ''}${esc(label)}
      </div>
      <div class="ci-tile-value">${value}</div>
      <div class="ci-tile-note">${esc(note)}</div>
    </div>`;

  host.innerHTML = [
    tile('', 'clipboard-list', 'Open plans', String(latest.open_plans),
      `${latest.active_plans} active, ${latest.held_plans} held, ${latest.v1_plans} v1, ${latest.spices_plans} spices`),
    tile('', 'circle-dot', 'Open reports', String(latest.open_reports),
      'Findings in docs/reported/'),
    tile('', 'layers', 'Total open work',
      String(latest.open_plans + latest.open_reports),
      'Plans plus reports'),
    tile('', 'book-open', 'Turmeric guides',
      latestGuides ? String(latestGuides.turmeric_guides) : '--',
      'In docs/guides/'),
    tile('', 'book-open', 'Spice guides',
      latestSpiceGuides ? String(latestSpiceGuides.spice_guides) : '--',
      latestSpiceGuides ? 'In the turmeric-spices repo' : 'Not measured yet'),
    tile('', 'clock', 'Last measured',
      esc(fmtDate(latest.ts)),
      esc((latest.sha ?? '').slice(0, 7))),
  ].join('');
}

function renderDocs() {
  const host = document.getElementById('ci-docs-chart');
  const legend = document.getElementById('ci-docs-legend');
  const sub = document.getElementById('ci-docs-sub');

  if (state.docsError) {
    host.innerHTML = `<div class="ci-empty">${esc(state.docsError)}</div>`;
    legend.innerHTML = '';
    sub.textContent = '';
    return;
  }

  const rows = docsRows();
  const xs = rows.map((r) => r.ts);
  const latest = state.docs[state.docs.length - 1];

  sub.innerHTML = latest
    ? `${esc(latest.open_plans)} open plans / ${esc(latest.open_reports)} open reports
       at
       <a class="mono ci-link"
          href="${GITHUB_URL}/commit/${esc(latest.sha ?? '')}"
          >${esc((latest.sha ?? '').slice(0, 7))}</a>.
       Plans live in <code>docs/upcoming/</code>; reports in
       <code>docs/reported/</code>.`
    : '';

  if (rows.length < 2) {
    host.innerHTML = `<div class="ci-empty">
      ${state.docs.length > rows.length
        ? 'Only one measurement in this range -- widen it to see a trend.'
        : 'Only one push has been measured so far. A trend needs two.'}
    </div>`;
    renderDocsLegend();
    return;
  }

  const series = DOCS_SERIES
    .filter(([key]) => state.docsKeys.has(key))
    .map(([key, label, cssVar]) => ({
      key,
      label,
      color: `var(${cssVar})`,
      pts: rows.map((r) => ({ x: r.ts, y: r[key] ?? 0, sha: r.sha })),
    }));

  drawLineChart({
    host,
    tooltip: document.getElementById('ci-docs-tooltip'),
    idPrefix: 'ci-docs',
    series,
    xs,
    ticksY: (lo, hi) => decimalTicks(lo, hi, 5),
    fmtY: fmtCount,
    fmtTickY: fmtCountTick,
    footer: `${xs.length} commit${xs.length === 1 ? '' : 's'}`,
    label: 'Open plans and reports over time',
  });

  renderDocsLegend();
}

function renderDocsLegend() {
  const latest = state.docs[state.docs.length - 1];
  document.getElementById('ci-docs-legend').innerHTML = DOCS_SERIES.map(
    ([key, label, cssVar]) => {
      const on = state.docsKeys.has(key);
      return `
        <button type="button" class="ci-legend-item ci-legend-toggle"
                data-docs="${esc(key)}" aria-pressed="${on}">
          <span class="swatch" style="background:${on ? `var(${cssVar})` : 'var(--text-dim)'}"></span>
          ${esc(label)}
          <span class="count">${esc(latest ? fmtCount(latest[key]) : '--')}</span>
        </button>`;
    },
  ).join('');

  for (const b of document.querySelectorAll('#ci-docs-legend .ci-legend-toggle')) {
    b.onclick = () => {
      const key = b.dataset.docs;
      if (state.docsKeys.has(key)) {
        if (state.docsKeys.size > 1) state.docsKeys.delete(key);
      } else {
        state.docsKeys.add(key);
      }
      renderDocs();
      writeURL();
    };
  }
}

// ── RENDER: TEST COUNTS ─────────────────────────────────────────────────────

// How many tests a suite row counted, or null when the suite printed no census
// line (collect-suite-timings.py's SUMMARY_RE). `discovered` when the harness
// reports it; otherwise what it ran plus what it skipped.
function testCount(r) {
  if (r.passed == null) return null;
  return r.discovered ?? (r.passed + (r.failed ?? 0) + (r.skipped ?? 0));
}

const runKey = (r) => (r.run_id != null ? `${r.run_id}|${r.run_attempt ?? ''}` : `ts|${r.ts}`);

// suite -> [{ ts, run, sha, count, passed, failed, skipped }], ascending.
//
// Each suite is counted ONCE, from its home environment: the one it has the
// most whole (unsharded) runs in. Counting it in every environment would add
// tur_tests up twice for running on Linux and macOS, and the environment lock
// the timings need would hide the browser suites, which only run under
// emscripten. A suite that only ever runs sharded is summed across its shards
// per run, so it is not dropped for having no whole run.
function suiteCounts() {
  const rowsBy = new Map();   // suite -> envKey -> rows
  for (const r of state.rows) {
    if (testCount(r) == null) continue;
    let envs = rowsBy.get(r.suite);
    if (!envs) rowsBy.set(r.suite, (envs = new Map()));
    const k = envKey(r);
    if (!envs.has(k)) envs.set(k, []);
    envs.get(k).push(r);
  }
  const envRank = new Map(state.envs.map((e, i) => [e.key, i]));
  const out = new Map();
  for (const [suite, envs] of rowsBy) {
    const whole = (rows) => rows.filter((r) => !r.shard_total).length;
    const [, rows] = [...envs].sort((a, b) => (
      whole(b[1]) - whole(a[1]) || b[1].length - a[1].length
      || (envRank.get(a[0]) ?? 0) - (envRank.get(b[0]) ?? 0)
    ))[0];
    const byRun = new Map();
    const unsharded = rows.filter((r) => !r.shard_total);
    for (const r of unsharded.length ? unsharded : rows) {
      const key = runKey(r);
      const p = byRun.get(key);
      const v = { count: testCount(r), passed: r.passed, failed: r.failed ?? 0, skipped: r.skipped ?? 0 };
      if (!p) byRun.set(key, { ts: r.ts, run: key, sha: r.sha, ...v });
      else {
        // Another shard of the same run: add it in.
        p.ts = Math.max(p.ts, r.ts);
        for (const k of ['count', 'passed', 'failed', 'skipped']) p[k] += v[k];
      }
    }
    out.set(suite, [...byRun.values()].sort((a, b) => a.ts - b.ts));
  }
  return out;
}

// Totals over time, one point per CI run: { ts, sha, total, macro: {id: n},
// failedHere }. Every suite contributes its NEWEST count as of that run, not
// only the suites the run itself executed -- a push runs only the jobs its
// diff needs, so a plain per-run sum sawtooths between ~3.7k and ~9k with no
// test added or removed.
//
// Two edges keep the carry-forward honest. A suite last seen more than a week
// before the newest data has been removed, so it leaves the sum after its last
// run instead of being carried to the end. And the points start once every
// suite present on the data's first day has reported: before that, the sum is
// missing suites that simply had not run yet in the loaded window.
function countTimeline(series) {
  const events = [];
  for (const [suite, pts] of series) for (const p of pts) events.push({ suite, ...p });
  events.sort((a, b) => a.ts - b.ts);
  if (!events.length) return [];
  const newest = events[events.length - 1].ts;
  const lastTs = new Map([...series].map(([s, pts]) => [s, pts[pts.length - 1].ts]));
  const firstTs = [...series.values()].map((pts) => pts[0].ts);
  const dataStart = Math.min(...firstTs);
  const warm = Math.max(...firstTs.filter((t) => t <= dataStart + 86400));

  const cur = new Map();
  const snaps = new Map();   // run -> snapshot after that run's last event
  for (const e of events) {
    cur.set(e.suite, e);
    for (const s of cur.keys()) {
      const end = lastTs.get(s);
      if (end < e.ts && end < newest - 7 * 86400) cur.delete(s);   // retired
    }
    const snap = { ts: e.ts, sha: e.sha, total: 0, macro: {}, failedHere: snaps.get(e.run)?.failedHere ?? false };
    for (const [s, p] of cur) {
      snap.total += p.count;
      const id = macroOf(s)[0];
      snap.macro[id] = (snap.macro[id] ?? 0) + p.count;
    }
    snap.failedHere = snap.failedHere || e.failed > 0;
    snaps.set(e.run, snap);
  }
  return [...snaps.values()].filter((p) => p.ts >= warm).sort((a, b) => a.ts - b.ts);
}

function renderCounts() {
  const tiles = document.getElementById('ci-counts-tiles');
  const host = document.getElementById('ci-counts-chart');
  const sub = document.getElementById('ci-counts-sub');
  const grid = document.getElementById('ci-macro-grid');
  const gridSub = document.getElementById('ci-macro-sub');

  const series = suiteCounts();
  if (!series.size) {
    tiles.innerHTML = '';
    sub.textContent = '';
    host.innerHTML = '<div class="ci-empty">No suite prints a test count yet.</div>';
    grid.innerHTML = '';
    gridSub.textContent = '';
    return;
  }

  const timeline = countTimeline(series);
  const cutoff = rangeCutoff(state.rows);
  const inRange = timeline.filter((p) => p.ts >= cutoff);

  // Headline: each current suite's newest count -- the same rule the timeline
  // applies at its last point, so the tile and the line end on one number.
  const newestTs = Math.max(...[...series.values()].map((pts) => pts[pts.length - 1].ts));
  const current = [...series]
    .map(([suite, pts]) => ({ suite, ...pts[pts.length - 1] }))
    .filter((p) => p.ts >= newestTs - 7 * 86400);
  const sum = (rows, k) => rows.reduce((a, p) => a + p[k], 0);
  const total = sum(current, 'count');
  const failed = sum(current, 'failed');
  const skipped = sum(current, 'skipped');
  const first = inRange[0];
  const last = inRange[inRange.length - 1];

  const tile = (cls, icon, label, value, note) => `
    <div class="ci-tile ${cls}">
      <div class="ci-tile-label"><t-icon name="${icon}"></t-icon>${esc(label)}</div>
      <div class="ci-tile-value">${value}</div>
      <div class="ci-tile-note">${esc(note)}</div>
    </div>`;
  tiles.innerHTML = [
    tile('', 'list-checks', 'Tests', esc(fmtCount(total)),
      `Across ${current.length} suite${current.length === 1 ? '' : 's'} that print a count`),
    tile(failed ? 'is-fail' : '', failed ? 'circle-x' : 'circle-check', 'Failing tests',
      esc(fmtCount(failed)), failed ? 'In each suite\'s newest run' : 'None in any suite\'s newest run'),
    tile(skipped ? 'is-skip' : '', 'circle-minus', 'Skipped tests',
      esc(fmtCount(skipped)), 'Counted, but not run'),
    tile('', 'trending-up', 'Change in range',
      esc(inRange.length > 1 ? fmtSignedCount(last.total - first.total) : '--'),
      inRange.length > 1 ? `Since ${fmtDate(first.ts)}` : 'Needs two runs in range'),
  ].join('');

  // Suites that ran but printed no census line are not in any of this.
  const silent = new Set(state.rows.map((r) => r.suite));
  for (const name of series.keys()) silent.delete(name);
  sub.textContent = `Every suite that prints a test count, each at its newest count as of `
    + `each run.${silent.size ? ` ${silent.size} other suites print none and are not counted.` : ''}`;

  // ── total
  if (inRange.length < 2) {
    host.innerHTML = `<div class="ci-empty">${timeline.length > inRange.length
      ? 'Only one run in this range -- widen it to see a trend.'
      : 'Only one run has been counted so far. A trend needs two.'}</div>`;
  } else {
    drawLineChart({
      host,
      tooltip: document.getElementById('ci-counts-tooltip'),
      idPrefix: 'ci-counts',
      series: [{
        key: 'total', label: 'Tests', color: 'var(--chart-1)',
        pts: inRange.map((p) => ({ x: p.ts, y: p.total, fail: p.failedHere, sha: p.sha })),
      }],
      xs: inRange.map((p) => p.ts),
      ticksY: (lo, hi) => decimalTicks(lo, hi, 5),
      fmtY: fmtCount,
      fmtTickY: fmtCountTick,
      footer: `${inRange.length} run${inRange.length === 1 ? '' : 's'}`,
      label: 'Total tests over time',
      directLabels: false,
    });
  }

  // ── per macro-suite: small multiples, each on its own zero-based scale. On
  // one shared axis the fixtures' ~11k flattens every other group to the floor.
  const members = new Map();
  for (const p of current) {
    const id = macroOf(p.suite)[0];
    if (!members.has(id)) members.set(id, []);
    members.get(id).push(p);
  }
  const groups = MACRO_SUITES.filter(([id]) => members.has(id));
  gridSub.textContent = `${groups.length} groups. Each chart has its own scale, from zero; `
    + 'open a group to see which suites it adds up.';
  grid.innerHTML = groups.map(([id, name, cssVar, , desc], i) => {
    const ms = members.get(id).sort((a, b) => b.count - a.count || a.suite.localeCompare(b.suite));
    const now = sum(ms, 'count');
    const change = inRange.length > 1 ? (last.macro[id] ?? 0) - (first.macro[id] ?? 0) : null;
    return `
      <div class="ci-multiple" data-macro="${esc(id)}">
        <div class="ci-multiple-head">
          <div>
            <h3 class="ci-multiple-title">
              <span class="swatch" style="background:var(${cssVar})"></span>${esc(name)}
            </h3>
            <p class="ci-multiple-desc">${esc(desc)}</p>
          </div>
          <div class="ci-multiple-figure">
            <span class="ci-multiple-value">${esc(fmtCount(now))}</span>
            <span class="ci-multiple-change">${change == null ? '' : esc(fmtSignedCount(change))}</span>
          </div>
        </div>
        <div class="ci-chart-wrap">
          <div class="ci-chart" id="ci-macro-chart-${i}"></div>
          <div class="ci-tooltip" id="ci-macro-tooltip-${i}" hidden></div>
        </div>
        <details class="ci-multiple-members">
          <summary>${ms.length} suite${ms.length === 1 ? '' : 's'}</summary>
          <ul>${ms.map((m) => `
            <li><span class="name">${esc(m.suite)}</span>
                <span class="val">${esc(fmtCount(m.count))}</span></li>`).join('')}
          </ul>
        </details>
      </div>`;
  }).join('');

  groups.forEach(([id, name, cssVar], i) => {
    const chart = document.getElementById(`ci-macro-chart-${i}`);
    if (inRange.length < 2) {
      chart.innerHTML = '<div class="ci-empty">Needs two runs in range.</div>';
      return;
    }
    drawLineChart({
      host: chart,
      tooltip: document.getElementById(`ci-macro-tooltip-${i}`),
      idPrefix: `ci-macro-${i}`,
      series: [{
        key: id, label: name, color: `var(${cssVar})`,
        pts: inRange.map((p) => ({ x: p.ts, y: p.macro[id] ?? 0, sha: p.sha })),
      }],
      xs: inRange.map((p) => p.ts),
      ticksY: (lo, hi) => decimalTicks(lo, hi, 3),
      fmtY: fmtCount,
      fmtTickY: fmtCountTick,
      footer: '',
      label: `${name}: tests over time`,
      height: 170,
      directLabels: false,
    });
  });
}

// ── RENDER: REPORT FLOW ─────────────────────────────────────────────────────

// The pushes that moved docs/reported/, as [{ ts, filed, archived, net }]: one
// per docs row, against the row before it. Rows carrying the running totals
// (collect-docs.py's reports_filed / reports_archived) give the split exactly.
// Older rows only have the open count, whose fall IS archived minus filed --
// every report that leaves the directory was archived, every one that enters
// was filed -- so the net is still exact there; only the split is unknown.
function reportFlows() {
  const out = [];
  for (let i = 1; i < state.docs.length; i++) {
    const a = state.docs[i - 1];
    const b = state.docs[i];
    if (a.reports_filed != null && b.reports_filed != null
        && a.reports_archived != null && b.reports_archived != null) {
      const filed = b.reports_filed - a.reports_filed;
      const archived = b.reports_archived - a.reports_archived;
      out.push({ ts: b.ts, filed, archived, net: archived - filed });
    } else if (a.open_reports != null && b.open_reports != null) {
      out.push({ ts: b.ts, filed: null, archived: null, net: a.open_reports - b.open_reports });
    }
  }
  return out;
}

// Local calendar days -- the reader's, since "what happened Tuesday" is asked
// in their timezone -- or Monday-start weeks once a range spans more days than
// columns fit. Every bucket from the start of the range to the newest push is
// present, a quiet day as an empty slot, so the x axis is time and not a list.
function flowBuckets(flows, fromTs) {
  if (!flows.length) return [];
  const startOf = (ts, weekly) => {
    const d = new Date(ts * 1000);
    d.setHours(0, 0, 0, 0);
    if (weekly) d.setDate(d.getDate() - ((d.getDay() + 6) % 7));
    return d;
  };
  const lastTs = flows[flows.length - 1].ts;
  const firstTs = Math.min(fromTs, flows[0].ts);
  const weekly = (lastTs - firstTs) / 86400 > 60;
  const buckets = [];
  const end = startOf(lastTs, weekly);
  for (const d = startOf(firstTs, weekly); d <= end; d.setDate(d.getDate() + (weekly ? 7 : 1))) {
    buckets.push({ start: d.getTime() / 1000, weekly, pushes: 0, net: 0, filed: 0, archived: 0, split: true });
  }
  for (const f of flows) {
    // Last bucket that starts at or before the push.
    let b = null;
    for (const c of buckets) if (c.start <= f.ts) b = c;
    if (!b) continue;
    b.pushes += 1;
    b.net += f.net;
    if (f.filed == null) b.split = false;
    else { b.filed += f.filed; b.archived += f.archived; }
  }
  return buckets;
}

function renderFlow() {
  const host = document.getElementById('ci-flow-chart');
  const sub = document.getElementById('ci-flow-sub');
  const legend = document.getElementById('ci-flow-legend');

  if (state.docsError) {
    host.innerHTML = `<div class="ci-empty">${esc(state.docsError)}</div>`;
    sub.textContent = '';
    legend.innerHTML = '';
    return;
  }

  const cutoff = rangeCutoff(state.docs);
  const flows = reportFlows().filter((f) => f.ts >= cutoff);
  // From the start of the range, or of the data when the range reaches past it.
  const buckets = flowBuckets(flows, Math.max(cutoff, state.docs[0]?.ts ?? 0));

  const net = flows.reduce((a, f) => a + f.net, 0);
  const split = flows.every((f) => f.filed != null);
  const filed = flows.reduce((a, f) => a + (f.filed ?? 0), 0);
  const archived = flows.reduce((a, f) => a + (f.archived ?? 0), 0);
  sub.innerHTML = flows.length
    ? `${split ? `${esc(fmtCount(archived))} archived, ${esc(fmtCount(filed))} filed in this
         range: net <strong>${esc(fmtSignedCount(net))}</strong>.`
        : `Net <strong>${esc(fmtSignedCount(net))}</strong> in this range.`}
       Above the line, a ${buckets[0]?.weekly ? 'week' : 'day'} closed more reports than
       it opened; below it, the backlog grew.`
    : '';

  legend.innerHTML = [
    ['var(--chart-2)', 'More archived than filed'],
    ['var(--chart-1)', 'More filed than archived'],
  ].map(([color, name]) => `
    <span class="ci-legend-item">
      <span class="swatch" style="background:${color}"></span>${esc(name)}
    </span>`).join('');

  if (!flows.length) {
    host.innerHTML = `<div class="ci-empty">${state.docs.length > 1
      ? 'No push in this range -- widen it to see the flow.'
      : 'Only one push has been measured so far. A flow needs two.'}</div>`;
    return;
  }

  const dayLabel = (b) => (b.weekly ? `wk ${fmtDate(b.start)}` : fmtDate(b.start));
  drawBarChart({
    host,
    tooltip: document.getElementById('ci-flow-tooltip'),
    idPrefix: 'ci-flow',
    buckets: buckets.map((b) => ({
      label: dayLabel(b),
      y: b.net,
      head: `${b.weekly ? 'Week of ' : ''}${fmtDate(b.start)} · ${b.pushes} push${b.pushes === 1 ? '' : 'es'}`,
      rows: [
        ['Net', fmtSignedCount(b.net),
          b.net > 0 ? 'var(--chart-2)' : b.net < 0 ? 'var(--chart-1)' : null],
        ...(b.split
          ? [['Archived', fmtCount(b.archived)], ['Filed', fmtCount(b.filed)]]
          : [['Split', 'not recorded']]),
      ],
    })),
    ticksY: (lo, hi) => intTicks(lo, hi, 5),
    fmtTickY: (v) => fmtSignedCount(v),
    footer: `${flows.length} push${flows.length === 1 ? '' : 'es'} · reports archived minus filed per ${
      buckets[0].weekly ? 'week' : 'day'}`,
    label: `Reports archived minus reports filed, per ${buckets[0].weekly ? 'week' : 'day'}`,
    posColor: 'var(--chart-2)',
    negColor: 'var(--chart-1)',
  });
}

// ── RENDER: SPICES AND GUIDES ───────────────────────────────────────────────

// One fixed-series trend chart over the docs-counts rows. Rows whose field is
// null (published without the spices checkout, or before the field existed) are
// left out of that series rather than drawn as zero.
function drawCountChart({ prefix, defs, unit, label, host, legend }) {
  const rows = docsRows().filter((r) => defs.some(([k]) => r[k] != null));
  if (state.docsError) {
    host.innerHTML = `<div class="ci-empty">${esc(state.docsError)}</div>`;
    legend.innerHTML = '';
    return;
  }
  const latestOf = (k) => latestDocWith(k)?.[k];
  legend.innerHTML = defs.map(([key, name, cssVar]) => `
    <span class="ci-legend-item">
      <span class="swatch" style="background:var(${cssVar})"></span>${esc(name)}
      <span class="count">${esc(latestOf(key) != null ? fmtCount(latestOf(key)) : '--')}</span>
    </span>`).join('');

  if (rows.length < 2) {
    host.innerHTML = `<div class="ci-empty">${rows.length
      ? 'Only one measurement in this range -- widen it to see a trend.'
      : 'Not measured yet. These counts start with the first push after they shipped.'}</div>`;
    return;
  }
  drawLineChart({
    host,
    tooltip: document.getElementById(`${prefix}-tooltip`),
    idPrefix: prefix,
    // A series with no measured point at all (the spice guides, before the
    // spices checkout existed) is left out: the chart reads pts[i].y.
    series: defs
      .map(([key, name, cssVar]) => ({
        key,
        label: name,
        color: `var(${cssVar})`,
        pts: rows.filter((r) => r[key] != null).map((r) => ({ x: r.ts, y: r[key], sha: r.sha })),
      }))
      .filter((s) => s.pts.length),
    xs: rows.map((r) => r.ts),
    ticksY: (lo, hi) => decimalTicks(lo, hi, 5),
    fmtY: fmtCount,
    fmtTickY: fmtCountTick,
    footer: `${rows.length} ${unit}${rows.length === 1 ? '' : 's'}`,
    label,
  });
}

function renderGuides() {
  const sub = document.getElementById('ci-guides-sub');
  const t = latestDocWith('turmeric_guides');
  const s = latestDocWith('spice_guides');
  sub.innerHTML = t
    ? `${esc(t.turmeric_guides)} Turmeric guides in <code>docs/guides/</code>`
      + (s ? `; ${esc(s.spice_guides)} spice guides in the turmeric-spices repo.` : '.')
    : '';
  drawCountChart({
    prefix: 'ci-guides', defs: GUIDE_SERIES, unit: 'commit',
    label: 'Guide counts over time',
    host: document.getElementById('ci-guides-chart'),
    legend: document.getElementById('ci-guides-legend'),
  });
}

function renderSpices() {
  const latest = latestDocWith('spices');
  const guides = latestDocWith('spice_guides');
  const tile = (icon, label, value, note) => `
    <div class="ci-tile">
      <div class="ci-tile-label"><t-icon name="${icon}"></t-icon>${esc(label)}</div>
      <div class="ci-tile-value">${value}</div>
      <div class="ci-tile-note">${esc(note)}</div>
    </div>`;
  document.getElementById('ci-spices-tiles').innerHTML = [
    tile('package', 'Spices', latest ? String(latest.spices) : '--',
      latest ? 'Libraries in the turmeric-spices repo' : 'Not measured yet'),
    tile('book-open', 'Spice guides', guides ? String(guides.spice_guides) : '--',
      'Guides in the turmeric-spices repo'),
    tile('clipboard-list', 'Spice plans', String(state.docs.at(-1)?.spices_plans ?? '--'),
      'Open plans filed under docs/upcoming/spices/'),
  ].join('');

  document.getElementById('ci-spices-sub').innerHTML = latest
    ? `${esc(latest.spices)} spices at
       <a class="mono ci-link" href="${GITHUB_URL}/commit/${esc(latest.sha ?? '')}"
          >${esc((latest.sha ?? '').slice(0, 7))}</a>.`
    : '';
  drawCountChart({
    prefix: 'ci-spices', defs: SPICE_SERIES, unit: 'commit',
    label: 'Spice count over time',
    host: document.getElementById('ci-spices-chart'),
    legend: document.getElementById('ci-spices-legend'),
  });

  const names = latestDocWith('spice_names')?.spice_names ?? [];
  document.getElementById('ci-spices-list').innerHTML = names.length
    ? names.map((n) => `<span>${esc(n)}</span>`).join('')
    : '<div class="ci-empty">Not measured yet.</div>';
}

// ── RENDER: SPARKLINE GRID ──────────────────────────────────────────────────

function renderSparks(stats) {
  const host = document.getElementById('ci-sparks');
  const q = state.sparkFilter.trim().toLowerCase();
  const shown = (q ? stats.filter((s) => s.suite.toLowerCase().includes(q)) : stats)
    .slice()
    .sort((a, b) => b.mean - a.mean);

  if (!shown.length) {
    host.innerHTML = '<div class="ci-empty">No suites match that filter.</div>';
    return;
  }

  const W = 96;
  const H = 24;
  const P = 3;

  host.innerHTML = shown.map((s) => {
    // Per-suite y scale: a shared one would flatten all but the largest few.
    const ms = s.pts.map((p) => p.ms);
    const lo = Math.min(...ms);
    const hi = Math.max(...ms);
    const span = hi - lo || 1;
    const n = ms.length;
    const px = (i) => (n === 1 ? W / 2 : P + (i / (n - 1)) * (W - P * 2));
    const py = (v) => H - P - ((v - lo) / span) * (H - P * 2);
    const d = ms.map((v, i) => `${i ? 'L' : 'M'}${px(i).toFixed(1)},${py(v).toFixed(1)}`).join(' ');
    const on = state.suites.includes(s.suite);
    const color = on ? colorOf(s.suite) : 'var(--text-dim)';

    return `
      <button type="button" class="ci-spark${on ? ' is-selected' : ''}"
              data-suite="${esc(s.suite)}"
              title="${esc(s.suite)} — mean ${esc(fmtDuration(s.mean))}, ${s.runs} runs">
        <span class="ci-spark-name">${esc(s.suite)}</span>
        <svg viewBox="0 0 ${W} ${H}" aria-hidden="true">
          <path class="ci-spark-line" d="${d}" style="stroke:${color}" />
          ${svgEl('circle', { cx: px(n - 1), cy: py(ms[n - 1]), r: 2 }, `fill:${color}`)}
        </svg>
        <span class="ci-spark-val">${esc(fmtDuration(s.last))}</span>
      </button>`;
  }).join('');

  for (const b of host.querySelectorAll('.ci-spark')) {
    b.onclick = () => toggleSuite(b.dataset.suite);
  }
}

function toggleSuite(suite) {
  if (state.suites.includes(suite)) {
    deselectSuite(suite);
  } else if (state.suites.includes(null)) {
    selectSuite(suite);
  } else {
    // At the cap, evict slot 0 rather than silently doing nothing. The other
    // four keep their slots, and so their colors.
    state.suites[0] = suite;
  }
  render();
}

// ── RENDER: TABLE ───────────────────────────────────────────────────────────

const COLS = [
  ['suite', 'Suite'],
  ['runs',  'Runs'],
  ['mean',  'Mean'],
  ['p90',   'p90'],
  ['last',  'Latest'],
  ['delta', 'Change'],
  ['status', 'Last status'],
];

function renderTable(stats) {
  const { col, dir } = state.sort;
  const sign = dir === 'asc' ? 1 : -1;
  const sorted = stats.slice().sort((a, b) => {
    if (col === 'suite' || col === 'status') {
      return sign * String(a[col]).localeCompare(String(b[col]));
    }
    // Change sorts by magnitude — the biggest movers, up or down, come first.
    if (col === 'delta') return sign * (Math.abs(a.delta) - Math.abs(b.delta));
    return sign * (a[col] - b[col]);
  });

  const head = COLS.map(([key, label]) => `
    <th scope="col">
      <button type="button" data-col="${key}">${label}${
        col === key ? `<span class="arrow">${dir === 'asc' ? '↑' : '↓'}</span>` : ''
      }</button>
    </th>`).join('');

  const body = sorted.map((s) => {
    const cls = s.delta > 0 ? 'ci-delta-up' : s.delta < 0 ? 'ci-delta-down' : 'ci-delta-flat';
    const change = s.runs < 2
      ? '<span class="ci-delta-flat">--</span>'
      : `<span class="${cls}">${esc(fmtPct(s.deltaPct))}</span>`;
    return `
      <tr class="${state.suites.includes(s.suite) ? 'is-selected' : ''}"
          data-suite="${esc(s.suite)}">
        <td title="${esc(s.suite)}">${esc(s.suite)}</td>
        <td>${s.runs}</td>
        <td>${esc(fmtDuration(s.mean))}</td>
        <td>${esc(fmtDuration(s.p90))}</td>
        <td>${esc(fmtDuration(s.last))}</td>
        <td>${change}</td>
        <td>${statusPill(s.status)}</td>
      </tr>`;
  }).join('');

  const table = document.getElementById('ci-table');
  table.innerHTML = `<thead><tr>${head}</tr></thead><tbody>${body}</tbody>`;

  for (const b of table.querySelectorAll('th button')) {
    b.onclick = () => {
      const key = b.dataset.col;
      state.sort = key === state.sort.col
        ? { col: key, dir: state.sort.dir === 'asc' ? 'desc' : 'asc' }
        : { col: key, dir: key === 'suite' || key === 'status' ? 'asc' : 'desc' };
      render();
    };
  }
  for (const tr of table.querySelectorAll('tbody tr')) {
    tr.onclick = () => toggleSuite(tr.dataset.suite);
  }
}

function statusPill(status) {
  const icon = STATUS_ICON[status] ?? 'circle';
  return `<span class="ci-pill ${esc(status)}">
    <t-icon name="${icon}"></t-icon>${esc(status)}</span>`;
}

// ── RENDER: SKIP LEDGER ─────────────────────────────────────────────────────

function renderSkips() {
  const rows = state.rows.filter((r) => envKey(r) === state.env);
  const latestTs = rows.reduce((m, r) => Math.max(m, r.ts), 0);
  const latest = rows.filter((r) => r.ts === latestTs);

  // The two reason fields are nullable by different conventions: skip_reason
  // is an explicit null on most rows, partial_skip_reason is absent entirely.
  const groups = new Map();
  const add = (kind, reason, suite) => {
    const key = `${kind} ${reason}`;
    if (!groups.has(key)) groups.set(key, { kind, reason, suites: [] });
    groups.get(key).suites.push(suite);
  };

  for (const r of latest) {
    if (r.status === 'skip') {
      add('skip', r.skip_reason || 'no reason recorded', seriesName(r));
    } else if (r.partial_skip_reason != null) {
      add('partial', r.partial_skip_reason, seriesName(r));
    }
  }

  const host = document.getElementById('ci-skips');
  if (!groups.size) {
    host.innerHTML = `<div class="ci-empty">
      Every suite ran in full in the latest build. Nothing skipped.
    </div>`;
    return;
  }

  host.innerHTML = [...groups.values()]
    .sort((a, b) => b.suites.length - a.suites.length)
    .map((g) => `
      <div class="ci-skip-group">
        <div class="ci-skip-reason">
          <!-- A partial skip is a coverage gap, not a failure: warn, not fail. -->
          <span class="ci-pill ${g.kind === 'skip' ? 'skip' : 'warn'}">
            <t-icon name="${g.kind === 'skip' ? 'minus' : 'triangle-alert'}"></t-icon>
            ${g.kind === 'skip' ? 'skipped' : 'partial'}
          </span>
          <span class="text">${esc(g.reason)}</span>
        </div>
        <div class="ci-skip-suites">
          ${g.suites.sort().map((s) => `<span>${esc(s)}</span>`).join('')}
        </div>
      </div>`).join('');
}

// ── ORCHESTRATION ───────────────────────────────────────────────────────────

// Top N by mean duration: the five biggest suites sit in the same magnitude
// band, which is what lets the chart default to a linear axis and still read.
//
// Suites with a single run in range are ranked BELOW every suite that has a
// history, however long that one run was. A new suite's first measurement is
// usually its largest -- a cold cache, nothing warmed -- so by mean alone it
// takes the top slot on the day it lands and draws a lone dot with no line.
// Two ctest suites landing at once is enough to empty the default chart, which
// is exactly what happened on 2026-10-01. They stay one click away in the
// sparkline grid; they are just not what the page opens on.
function defaultSuites() {
  const stats = suiteStats(bySuite(envRows()));
  const top = stats
    .sort((a, b) => (a.runs > 1 ? 0 : 1) - (b.runs > 1 ? 0 : 1) || b.mean - a.mean)
    .slice(0, MAX_SERIES)
    .map((s) => s.suite);
  // Always MAX_SERIES long: slot index is the color, so the array is fixed
  // length and short selections are padded with holes.
  return Array.from({ length: MAX_SERIES }, (_, i) => top[i] ?? null);
}

function render() {
  const stats = suiteStats(bySuite(envRows()));
  renderProvenance();
  renderTiles();
  renderFilters();
  renderChart();
  renderCounts();
  renderLoc();
  renderDocsTiles();
  renderDocs();
  renderFlow();
  renderGuides();
  renderSpices();
  renderSparks(stats);
  renderTable(stats);
  renderSkips();
  writeURL();

  const search = document.getElementById('ci-spark-search');
  if (search && search.value !== state.sparkFilter) search.value = state.sparkFilter;
}

function fail(message) {
  const el = document.getElementById('ci-state');
  el.className = 'ci-state is-error';
  el.innerHTML = `<t-icon name="circle-x"></t-icon><span>${esc(message)}</span>`;
}

// Wire one segmented control. Scoped by attribute so the two on the page
// cannot each answer for the other's chart.
function wireSegToggle(attr, apply) {
  for (const b of document.querySelectorAll(`.ci-seg-btn[${attr}]`)) {
    b.onclick = () => {
      apply(b.getAttribute(attr));
      for (const o of document.querySelectorAll(`.ci-seg-btn[${attr}]`)) {
        o.classList.toggle('is-active', o === b);
      }
      writeURL();
    };
  }
}

async function fetchNDJSON(url) {
  const res = await fetch(url);
  if (!res.ok) throw new Error(`${res.status} ${res.statusText}`);
  return {
    year: res.headers.get('X-Metrics-Year'),
    rows: parseNDJSON(await res.text()),
  };
}

// Show one tab panel and hide the others. Re-renders the newly visible chart
// so its SVG picks up the real panel width instead of the 860px fallback a
// hidden panel (clientWidth 0) forces.
function switchTab(tab) {
  state.tab = tab;
  for (const t of document.querySelectorAll('.ci-tab')) {
    const on = t.dataset.tab === tab;
    t.classList.toggle('is-active', on);
    t.setAttribute('aria-selected', on);
  }
  for (const p of document.querySelectorAll('.ci-tab-panel')) {
    p.hidden = p.id !== `ci-panel-${tab}`;
  }
  // A chart drawn into a hidden panel has a fallback viewBox width; redraw now
  // that the panel is visible so it fits the real column.
  if (tab === 'counts') renderCounts();
  if (tab === 'code') renderLoc();
  if (tab === 'spices') renderSpices();
  if (tab === 'reports') { renderDocsTiles(); renderDocs(); renderFlow(); renderGuides(); }
  writeURL();
}

// Replace the recent window with the full year, keeping the selected
// environment. Only reachable from a wide range or an aged-out shared link.
async function loadFull() {
  const full = await fetchNDJSON(API);
  if (!full.rows.length) throw new Error('empty');
  state.rows = full.rows;
  state.year = full.year;
  state.windowed = false;
  state.envs = buildEnvs(state.rows);
}

async function boot() {
  // All three files at once, and settled rather than raced: the line counts
  // and doc counts are independent files, so they must neither delay the
  // timings nor be able to take the page down with them. A fresh ci-metrics
  // branch legitimately has no repo-loc or docs-counts file at all, and that
  // costs one panel each.
  //
  // Timings: the recent window first. The full year is 40+ MB and the page opens
  // on 7 days, so downloading it all up front was most of the load time. A missing
  // window (the publisher has not written one yet) falls back to the full log.
  const firstUrl = new URLSearchParams(location.search).get('range');
  const wantsFull = firstUrl && !RECENT_RANGES.includes(firstUrl);
  const fetchTimings = async () => {
    if (!wantsFull) {
      try {
        const recent = await fetchNDJSON(`${API}?window=recent`);
        if (recent.rows.length) return { ...recent, windowed: true };
      } catch { /* fall through to the full log */ }
    }
    return { ...(await fetchNDJSON(API)), windowed: false };
  };
  const [timings, loc, docs] = await Promise.allSettled([
    fetchTimings(),
    fetchNDJSON(LOC_API),
    fetchNDJSON(DOCS_API),
  ]);

  if (timings.status === 'rejected') {
    fail(`Could not load CI timings (${timings.reason.message}). The data is
          published from pushes to main; try again in a few minutes.`);
    return;
  }

  state.year = timings.value.year;
  state.rows = timings.value.rows;
  state.windowed = timings.value.windowed;
  if (!state.rows.length) {
    fail('No timing rows have been published yet.');
    return;
  }

  if (loc.status === 'fulfilled') {
    state.loc = loc.value.rows.sort((a, b) => a.ts - b.ts);
    if (!state.loc.length) {
      state.locError = 'No line counts have been published yet.';
    }
  } else {
    state.locError = `Line counts are unavailable (${loc.reason.message}). They are`
      + ' published on each push to main, starting with the first one after'
      + ' this panel shipped.';
  }

  if (docs.status === 'fulfilled') {
    state.docs = docs.value.rows.sort((a, b) => a.ts - b.ts);
    if (!state.docs.length) {
      state.docsError = 'No doc counts have been published yet.';
    }
  } else {
    state.docsError = `Doc counts are unavailable (${docs.reason.message}). They are`
      + ' published on each push to main, starting with the first one after'
      + ' this panel shipped.';
  }

  state.envs = buildEnvs(state.rows);

  const url = readURL();
  // A shared link can name an environment or suite the recent window has already
  // aged out. Rather than silently open on a different one, fetch the full log.
  if (state.windowed) {
    const missingEnv = url.env && !state.envs.some((e) => e.key === url.env);
    const known0 = new Set(state.rows.map(seriesName));
    const missingSuite = (url.suites ?? []).some((n) => n && !known0.has(n));
    if (missingEnv || missingSuite) {
      try { await loadFull(); } catch { /* keep the window; defaults apply */ }
    }
  }
  state.env = state.envs.some((e) => e.key === url.env) ? url.env : state.envs[0].key;
  if (url.range && RANGES.some(([v]) => v === url.range)) state.range = url.range;
  if (url.scale === 'log') state.scale = 'log';
  if (url.statuses) {
    const valid = url.statuses.filter((s) => STATUSES.includes(s));
    if (valid.length) state.statuses = new Set(valid);
  }
  if (url.loc) {
    const valid = url.loc.filter((k) => LOC_SERIES.some(([key]) => key === k));
    if (valid.length) state.locKeys = new Set(valid);
  }
  if (url.docs) {
    const valid = url.docs.filter((k) => DOCS_SERIES.some(([key]) => key === k));
    if (valid.length) state.docsKeys = new Set(valid);
  }
  if (url.tab && TABS.includes(url.tab)) state.tab = url.tab;

  const known = new Set(state.rows.map(seriesName));
  state.suites = url.suites
    ? Array.from({ length: MAX_SERIES }, (_, i) =>
        (known.has(url.suites[i]) ? url.suites[i] : null))
    : new Array(MAX_SERIES).fill(null);
  if (!selected().length) state.suites = defaultSuites();

  document.getElementById('ci-state').hidden = true;
  document.getElementById('ci-body').hidden = false;

  for (const b of document.querySelectorAll('.ci-seg-btn[data-scale]')) {
    b.classList.toggle('is-active', b.getAttribute('data-scale') === state.scale);
  }
  wireSegToggle('data-scale', (v) => { state.scale = v; renderChart(); });

  // The range <select> is static HTML shared across all tabs, so it is wired
  // here rather than in renderFilters (which only owns the Tests-tab controls).
  const range = document.getElementById('ci-range');
  range.value = state.range;
  range.onchange = async (e) => {
    state.range = e.target.value;
    if (state.windowed && !RECENT_RANGES.includes(state.range)) {
      range.disabled = true;
      document.getElementById('ci-provenance').textContent = 'Loading full history…';
      try {
        await loadFull();
      } catch (err) {
        // Stay on what we have: the window covers 7 days, so say so and snap back.
        state.range = DEFAULT_RANGE;
        range.value = state.range;
        document.getElementById('ci-provenance').textContent =
          `Could not load the full history (${err.message}).`;
      }
      range.disabled = false;
    }
    render();
  };

  // Tab bar: static HTML, wired here.
  for (const t of document.querySelectorAll('.ci-tab')) {
    t.onclick = () => switchTab(t.dataset.tab);
  }

  const search = document.getElementById('ci-spark-search');
  search.addEventListener('input', () => {
    state.sparkFilter = search.value;
    renderSparks(suiteStats(bySuite(envRows())));
  });

  render();
  switchTab(state.tab);

  // Re-render (rather than scale) on resize so text never distorts.
  const redraw = new Map([
    [document.getElementById('ci-chart'), renderChart],
    [document.getElementById('ci-counts-chart'), renderCounts],
    [document.getElementById('ci-macro-grid'), renderCounts],
    [document.getElementById('ci-loc-chart'), renderLoc],
    [document.getElementById('ci-docs-chart'), renderDocs],
    [document.getElementById('ci-flow-chart'), renderFlow],
    [document.getElementById('ci-guides-chart'), renderGuides],
    [document.getElementById('ci-spices-chart'), renderSpices],
  ]);
  let raf = 0;
  const pending = new Set();
  const ro = new ResizeObserver((entries) => {
    for (const e of entries) pending.add(redraw.get(e.target));
    cancelAnimationFrame(raf);
    raf = requestAnimationFrame(() => {
      for (const fn of pending) fn?.();
      pending.clear();
    });
  });
  for (const el of redraw.keys()) ro.observe(el);
}

boot();
