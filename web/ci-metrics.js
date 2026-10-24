// ci-metrics.js — /ci, the CI suite-timings dashboard.
//
// Named ci-metrics rather than ci because a root-level `ci.js` shadows the
// /ci route: Vite resolves the extensionless request to the module, not to
// ci/index.html.
//
// Reads chart-ready JSON from /api/ci/* (ci-api.js, a D1 database CI reports
// into) and renders it as hand-built SVG. No charting library: the site bundles
// nothing from a CDN, and the shapes here are simple enough that a dependency
// would cost more than it saves.
//
// Every read but /api/ci/meta carries the meta's ingest version (?v=), so the
// responses are immutable and a repeat view between pushes downloads only the
// meta. The server does the reduction: one environment and range per request,
// one point per run for 1d/7d and a daily median per suite for wider ranges.
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
import { DAY, STATUSES, rangeCutoff } from './ci-aggregate.js';

const API = '/api/ci';

// A request that has not answered in this long is reported, not waited on: a
// spinner that never resolves reads as a broken page.
const FETCH_TIMEOUT_MS = 30_000;

// The nightly benchmarks (performance-audit-plan WP6): one chart per view,
// each at most five series -- the palette's cap -- so a view is one metric on
// one engine. [key, title, metric, engine].
const BENCH_VIEWS = [
  ['run_ms/jit', 'Run time: tur jit', 'run_ms', 'jit'],
  ['run_ms/cc', 'Run time: the tur build binary', 'run_ms', 'cc'],
  ['run_ms/interp', 'Run time: tur --interpret', 'run_ms', 'interp'],
  ['build_ms/cc', 'Build time: tur build (front end + cc)', 'build_ms', 'cc'],
  ['emit_ms/cc', 'Front end: tur emit-c', 'emit_ms', 'cc'],
  ['binary_bytes/cc', 'Binary size', 'binary_bytes', 'cc'],
];
const BENCH_DEFAULT = 'run_ms/jit';

// Fixed categorical order from vars.css. Assigned by suite name, never by
// rank, so filtering the selection does not repaint the survivors. Five is the
// cap: a sixth series would have to be an invented hue, so the picker refuses
// instead and the sparkline grid covers the long tail.
const SERIES_VARS = ['--chart-1', '--chart-2', '--chart-3', '--chart-4', '--chart-5'];
const MAX_SERIES = SERIES_VARS.length;

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
// by kind" -- and the other plan buckets are there so the breakdown of the plan
// total is visible without being the default view. `active_plans` is the STRAY
// set: plans at the top level of docs/upcoming/, filed under no topic
// directory -- not the total. `godot_plans` and `spices_plans` are the topic
// directories docs/upcoming/godot/ and spices/. `retired_plans` is derived on
// load from `held_plans` + `v1_plans`, the status directories hold/ and v1/
// that were flattened away; it charts as history that falls to 0. The total is
// not a separate series because a sum line on the same chart as its addends is
// a visual tautology.
const DOCS_SERIES = [
  ['open_reports',  'Open reports',    '--chart-1'],
  ['active_plans',  'Stray plans',     '--chart-2'],
  ['godot_plans',   'Godot plans',     '--chart-3'],
  ['retired_plans', 'Held / v1 plans', '--chart-4'],
  ['spices_plans',  'Spice plans',     '--chart-5'],
];
const DOCS_DEFAULT = ['open_reports', 'active_plans'];

// The disjoint plan buckets a docs row carries. The headline total is their
// sum -- stray plus each topic directory -- so it always equals the breakdown
// printed under it. A row published before a bucket existed lacks it; that
// bucket counts 0 there.
const PLAN_BUCKETS = ['active_plans', 'godot_plans', 'spices_plans', 'held_plans', 'v1_plans'];
const planTotal = (r) => PLAN_BUCKETS.reduce((n, k) => n + (r[k] ?? 0), 0);

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

const TABS = ['tests', 'counts', 'code', 'spices', 'reports', 'bench'];

// ── STATE ───────────────────────────────────────────────────────────────────

const state = {
  // /api/ci/meta: the ingest version every other read carries, and when it
  // landed.
  version: 0,
  ingestAt: null,
  // The /api/ci/timings response for (env, range), and its cells decoded into
  // series name -> [{ ts, ms, status, sha?, n? }] (`points`) over the x domain
  // `xs`. In `day` mode a point is one suite's median for one UTC day.
  timings: null,
  points: new Map(),
  xs: [],
  envs: [],           // [{ key, label, ts, breadth }], from the meta
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
  // The Test counts timeline (/api/ci/counts), fetched the first time its tab
  // opens: no other tab reads it.
  counts: null,
  countsError: null,
  // Line counts: one row per push to main, no env dimension. Empty until the
  // first push is reported, which the panel says out loud rather than drawing
  // an empty axis.
  loc: [],
  locKeys: new Set(LOC_DEFAULT),
  locError: null,
  // Open plans and reports: same shape as loc -- one row per push to main, no
  // env dimension.
  docs: [],
  docsKeys: new Set(DOCS_DEFAULT),
  docsError: null,
  // Nightly benchmarks: one row per program, engine and metric per night, from
  // the nightly arm64 workflow. Fetched the first time the tab opens.
  bench: null,
  benchError: null,
  benchView: BENCH_DEFAULT,
  // Which tab is visible. Tests is the default; the others render their charts
  // on first reveal so the SVG picks up the real panel width instead of the
  // 860px fallback a hidden panel forces.
  tab: 'tests',
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

// A binary's size, decimal units (what `ls -l` and the WP3/WP5 tables use).
function fmtBytes(n) {
  if (n == null || Number.isNaN(n)) return '--';
  if (n >= 1e6) return `${(n / 1e6).toFixed(2)} MB`;
  if (n >= 1e3) return `${(n / 1e3).toFixed(1)} KB`;
  return `${Math.round(n)} B`;
}

function fmtBytesTick(n) {
  if (n === 0) return '0';
  const trim = (x) => String(+x.toFixed(1));
  if (n >= 1e6) return `${trim(n / 1e6)}MB`;
  if (n >= 1e3) return `${trim(n / 1e3)}KB`;
  return `${Math.round(n)}B`;
}

const esc = (s) => String(s).replace(/[&<>"']/g, (c) => (
  { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]
));

// ── DATA ────────────────────────────────────────────────────────────────────

// Decode a /api/ci/timings response into state.points and state.xs. The
// response indexes suites and runs to stay small; everything downstream wants
// a series name -> points map, as it always has.
function setTimings(t) {
  const points = new Map();
  const push = (name, p) => {
    let a = points.get(name);
    if (!a) points.set(name, (a = []));
    a.push(p);
  };
  let xs;
  if (t.mode === 'run') {
    for (const [s, r, ms, st] of t.cells) {
      const [ts, sha] = t.runs[r];
      push(t.suites[s], { ts, ms, status: STATUSES[st], sha });
    }
    xs = t.runs.map(([ts]) => ts);
  } else {
    // A day's point sits at its UTC noon, so it lands mid-day on the axis.
    const at = (day) => day * DAY + DAY / 2;
    for (const [s, day, ms, n, st] of t.cells) {
      push(t.suites[s], { ts: at(day), ms, status: STATUSES[st], n });
    }
    xs = [...new Set(t.cells.map((c) => at(c[1])))].sort((a, b) => a - b);
  }
  for (const a of points.values()) a.sort((x, y) => x.ts - y.ts);
  state.timings = t;
  state.points = points;
  state.xs = xs;
}

const dayMode = () => state.timings?.mode === 'day';

// The newest row anchors each dataset's range (see rangeCutoff), so a quiet
// week still shows its last data.
const latestOf = (rows) => rows.reduce((m, r) => Math.max(m, r.ts), 0);

function locRows() {
  const cutoff = rangeCutoff(latestOf(state.loc), state.range);
  return state.loc.filter((r) => r.ts >= cutoff);
}

function docsRows() {
  const cutoff = rangeCutoff(latestOf(state.docs), state.range);
  return state.docs.filter((r) => r.ts >= cutoff);
}

function benchRows() {
  const rows = state.bench ?? [];
  const cutoff = rangeCutoff(latestOf(rows), state.range);
  return rows.filter((r) => r.ts >= cutoff);
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

// suite -> [{ ts, ms, status, sha?, n? }], ascending by ts, with the status
// filter applied. Status filtering is deliberately NOT applied to the run axis
// (state.xs) -- a run where a suite failed is still a run.
function bySuite() {
  const m = new Map();
  for (const [name, pts] of state.points) {
    const kept = pts.filter((p) => state.statuses.has(p.status));
    if (kept.length) m.set(name, kept);
  }
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
    bench: q.get('bench'),
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
  if (state.benchView !== BENCH_DEFAULT) q.set('bench', state.benchView);
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
  const t = state.timings;
  if (!t?.latest_sha) return;

  const short = t.latest_sha.slice(0, 7);
  document.getElementById('ci-provenance').innerHTML = `
    <span>Latest run</span>
    <a class="mono ci-link"
       href="${GITHUB_URL}/commit/${esc(t.latest_sha)}">${esc(short)}</a>
    <span class="dot-sep">/</span>
    <span>${esc(fmtDateTime(t.latest_ts))}</span>
    <span class="dot-sep">/</span>
    <span>Ingest <span class="mono">#${esc(state.version)}</span>${state.ingestAt
      ? `, ${esc(fmtDateTime(state.ingestAt))}` : ''}</span>`;
}

function renderTiles() {
  const latest = state.timings.latest;

  const total = latest.reduce((a, r) => a + r.ms, 0);
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
      `${state.timings.suites.length} seen in range`),
    tile(
      failed.length ? 'is-fail' : 'is-clean',
      failed.length ? 'circle-x' : 'circle-check',
      failed.length ? 'Failing' : 'All passing',
      String(failed.length),
      failed.length ? failed.map((r) => r.suite).join(', ') : 'No failures in the latest run',
    ),
    tile(skipped.length ? 'is-skip' : '', 'circle-minus', 'Skipped',
      String(skipped.length), 'Fully or partially skipped'),
  ].join('');
}

// ── RENDER: FILTERS ─────────────────────────────────────────────────────────

function renderFilters() {
  const allSuites = state.timings.suites;

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

  document.getElementById('ci-env').onchange = async (e) => {
    const select = e.target;
    select.disabled = true;
    let failed = null;
    try {
      await loadTimings(select.value, state.range);
      state.env = select.value;
      state.suites = defaultSuites();
    } catch (err) {
      failed = err;
    }
    render();   // redraws the select, enabled, on the environment in effect
    if (failed) notice(`Could not load that environment (${failed.message}).`);
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
//   directLabels  false where a title already names the one series
//   fmtX      (x) => string               -- the tooltip head; a date for a
//             chart of daily points, where a time of day would be invented
function drawLineChart({
  host, tooltip, idPrefix, series, xs, log = false,
  ticksY, fmtY, fmtTickY, footer, label, directLabels = true, fmtX = fmtDateTime,
}) {
  const x0 = xs[0];
  const x1 = xs[xs.length - 1];

  const W = Math.max(360, host.clientWidth || 860);
  const H = 320;
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

  wireHover({ host, tooltip, idPrefix, series, sx, fmtY, fmtX, pad, ih });
}

function wireHover({ host, tooltip, idPrefix, series, sx, fmtY, fmtX, pad, ih }) {
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
        <span>${esc(fmtX(best))}</span>
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
  const bucketed = bySuite();

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
  const xs = state.xs;
  const days = dayMode();

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
    fmtX: days ? fmtDate : fmtDateTime,
    footer: days
      ? `${xs.length} day${xs.length === 1 ? '' : 's'} · daily median per suite`
      : `${xs.length} run${xs.length === 1 ? '' : 's'}`,
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

// ── RENDER: NIGHTLY BENCHMARKS ──────────────────────────────────────────────

// A program's short name: the file, or a fixture's directory for input.tur.
function benchLabel(program) {
  const parts = String(program).split('/');
  let stem = parts[parts.length - 1].replace(/\.tur$/, '');
  if ((stem === 'input' || stem === 'main') && parts.length > 1) stem = parts[parts.length - 2];
  return stem;
}

function renderBench() {
  const host = document.getElementById('ci-bench-chart');
  const sub = document.getElementById('ci-bench-sub');
  const legend = document.getElementById('ci-bench-legend');
  const pick = document.getElementById('ci-bench-view');
  if (!host) return;

  if (!pick.options.length) {
    pick.innerHTML = BENCH_VIEWS.map(([key, title]) =>
      `<option value="${esc(key)}">${esc(title)}</option>`).join('');
    pick.onchange = () => {
      state.benchView = pick.value;
      renderBench();
      writeURL();
    };
  }
  pick.value = state.benchView;

  if (state.benchError) {
    host.innerHTML = `<div class="ci-empty">${esc(state.benchError)}</div>`;
    sub.textContent = '';
    legend.innerHTML = '';
    return;
  }
  if (!state.bench) {
    host.innerHTML = '<div class="ci-empty">Loading the nightly benchmarks…</div>';
    sub.textContent = '';
    legend.innerHTML = '';
    return;
  }

  const [, title, metric, engine] =
    BENCH_VIEWS.find(([key]) => key === state.benchView) ?? BENCH_VIEWS[0];
  // One runner shape, the newest row's: a number from a different (os, nproc)
  // is not comparable, the same rule the Tests tab's environment lock keeps.
  const latest = state.bench[state.bench.length - 1];
  const shape = (r) => `${r.os}|${r.nproc}`;
  const rows = benchRows().filter((r) => shape(r) === shape(latest)
    && r.metric === metric && r.engine === engine
    && r.status === 'ok' && r.value != null);
  const programs = [...new Set(rows.map((r) => r.program))].sort().slice(0, 5);
  const xs = [...new Set(rows.map((r) => r.ts))].sort((a, b) => a - b);
  const noisy = rows.filter((r) => r.noisy).length;

  sub.textContent = `Nightly, ${latest.os ?? '?'} with ${latest.nproc ?? '?'} cores,`
    + ` Release tur, best of ${latest.reps ?? 3} after a warm-up, output checked against`
    + ` its pin. ${rows.length} measurement${rows.length === 1 ? '' : 's'} in range`
    + `${noisy ? `, ${noisy} noisy (reps disagree by more than 10%)` : ''}.`
    + ' A night above 1.5x its trailing median is flagged on the nightly run.';

  if (xs.length < 2) {
    host.innerHTML = `<div class="ci-empty">${rows.length
      ? 'Only one night in this range -- widen it, or wait for a second night.'
      : 'Nothing measured for this view yet.'}</div>`;
    legend.innerHTML = '';
    return;
  }

  const series = programs.map((program, i) => ({
    key: program,
    label: benchLabel(program),
    color: `var(--chart-${i + 1})`,
    pts: rows.filter((r) => r.program === program)
      .map((r) => ({ x: r.ts, y: r.value, sha: r.sha })),
  }));
  const bytes = metric === 'binary_bytes';

  drawLineChart({
    host,
    tooltip: document.getElementById('ci-bench-tooltip'),
    idPrefix: 'ci-bench',
    series,
    xs,
    ticksY: (lo, hi) => (bytes ? decimalTicks(lo, hi, 5) : niceTicks(lo, hi, 5)),
    fmtY: bytes ? fmtBytes : fmtDuration,
    fmtTickY: bytes ? fmtBytesTick : fmtTick,
    footer: `${xs.length} night${xs.length === 1 ? '' : 's'}`,
    label: title,
  });

  const newest = (program) => {
    const own = rows.filter((r) => r.program === program);
    return own.length ? own[own.length - 1].value : null;
  };
  legend.innerHTML = series.map((s) => `
    <span class="ci-legend-item">
      <span class="swatch" style="background:${s.color}"></span>
      ${esc(s.label)}
      <span class="count">${esc((bytes ? fmtBytes : fmtDuration)(newest(s.key)))}</span>
    </span>`).join('');
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
    tile('', 'clipboard-list', 'Open plans', String(planTotal(latest)),
      [`${latest.active_plans ?? 0} stray`, latest.godot_plans ? `${latest.godot_plans} godot` : '',
       latest.held_plans ? `${latest.held_plans} held` : '',
       latest.v1_plans ? `${latest.v1_plans} v1` : '', `${latest.spices_plans ?? 0} spices`]
        .filter(Boolean).join(', ')),
    tile('', 'circle-dot', 'Open reports', String(latest.open_reports),
      'Findings in docs/reported/'),
    tile('', 'layers', 'Total open work',
      String(planTotal(latest) + latest.open_reports),
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
    ? `${esc(planTotal(latest))} open plans / ${esc(latest.open_reports)} open reports
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
//
// The server builds this tab's timeline (ci-aggregate.js suiteCounts() and
// countTimeline()): each suite counted once, from its home environment, and
// every suite's newest count carried forward to each run. The page filters it
// to the range and draws it.

function renderCounts() {
  const tiles = document.getElementById('ci-counts-tiles');
  const host = document.getElementById('ci-counts-chart');
  const sub = document.getElementById('ci-counts-sub');

  if (state.countsError || !state.counts) {
    tiles.innerHTML = '';
    sub.textContent = '';
    host.innerHTML = `<div class="ci-empty">${esc(state.countsError ?? 'Loading test counts…')}</div>`;
    return;
  }

  const { current, silent } = state.counts;
  const timeline = state.counts.timeline.map(([ts, sha, total, failedHere]) => (
    { ts, sha, total, failedHere: !!failedHere }));
  if (!timeline.length) {
    tiles.innerHTML = '';
    sub.textContent = '';
    host.innerHTML = '<div class="ci-empty">No suite prints a test count yet.</div>';
    return;
  }

  const cutoff = rangeCutoff(state.counts.latest_ts, state.range);
  const inRange = timeline.filter((p) => p.ts >= cutoff);
  const first = inRange[0];
  const last = inRange[inRange.length - 1];

  // Headline: each current suite's newest count -- the same rule the timeline
  // applies at its last point, so the tile and the line end on one number.
  const tile = (cls, icon, label, value, note) => `
    <div class="ci-tile ${cls}">
      <div class="ci-tile-label"><t-icon name="${icon}"></t-icon>${esc(label)}</div>
      <div class="ci-tile-value">${value}</div>
      <div class="ci-tile-note">${esc(note)}</div>
    </div>`;
  tiles.innerHTML = [
    tile('', 'list-checks', 'Tests', esc(fmtCount(current.total)),
      `Across ${current.suites} suite${current.suites === 1 ? '' : 's'} that print a count`),
    tile(current.failed ? 'is-fail' : '', current.failed ? 'circle-x' : 'circle-check',
      'Failing tests', esc(fmtCount(current.failed)),
      current.failed ? 'In each suite\'s newest run' : 'None in any suite\'s newest run'),
    tile(current.skipped ? 'is-skip' : '', 'circle-minus', 'Skipped tests',
      esc(fmtCount(current.skipped)), 'Counted, but not run'),
    tile('', 'trending-up', 'Change in range',
      esc(inRange.length > 1 ? fmtSignedCount(last.total - first.total) : '--'),
      inRange.length > 1 ? `Since ${fmtDate(first.ts)}` : 'Needs two runs in range'),
  ].join('');

  // Suites that ran but printed no census line are not in any of this.
  sub.textContent = `Every suite that prints a test count, each at its newest count as of `
    + `each run.${silent ? ` ${silent} other suites print none and are not counted.` : ''}`;

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

  const cutoff = rangeCutoff(latestOf(state.docs), state.range);
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
              title="${esc(s.suite)} — mean ${esc(fmtDuration(s.mean))}, ${s.runs} ${dayMode() ? 'days' : 'runs'}">
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

  // Over 30 days and up, every point is a suite's median for one day, so the
  // columns are statistics over days, and say so.
  const days = dayMode();
  const colLabel = (key, label) => (days
    ? { runs: 'Days', mean: 'Mean of daily medians', p90: 'p90 of daily medians' }[key] ?? label
    : label);
  const head = COLS.map(([key, label]) => `
    <th scope="col">
      <button type="button" data-col="${key}">${colLabel(key, label)}${
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
  const latest = state.timings.latest;

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
      add('skip', r.skip_reason || 'no reason recorded', r.suite);
    } else if (r.partial_skip_reason != null) {
      add('partial', r.partial_skip_reason, r.suite);
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
  const stats = suiteStats(bySuite());
  const top = stats
    .sort((a, b) => (a.runs > 1 ? 0 : 1) - (b.runs > 1 ? 0 : 1) || b.mean - a.mean)
    .slice(0, MAX_SERIES)
    .map((s) => s.suite);
  // Always MAX_SERIES long: slot index is the color, so the array is fixed
  // length and short selections are padded with holes.
  return Array.from({ length: MAX_SERIES }, (_, i) => top[i] ?? null);
}

function render() {
  const stats = suiteStats(bySuite());
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
  renderBench();
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

// A transient problem worth saying, without taking the page down: it goes
// where the provenance line was, until the next render puts that back.
function notice(message) {
  document.getElementById('ci-provenance').textContent = message;
}

// GET /api/ci/<path>, pinned to the meta's ingest version. A versioned URL's
// response never changes, so the browser keeps it; between pushes a reload
// costs only the meta. A stale version is redirected to the current one, so a
// tab left open across a push still gets current data.
async function fetchJSON(path, params = {}) {
  const q = new URLSearchParams({ ...params, v: String(state.version) });
  const res = await fetch(`${API}/${path}?${q}`, {
    signal: AbortSignal.timeout(FETCH_TIMEOUT_MS),
  });
  if (!res.ok) {
    let detail = res.statusText;
    try { detail = (await res.json()).error ?? detail; } catch { /* not JSON */ }
    throw new Error(`${res.status} ${detail}`.trim());
  }
  return res.json();
}

// One (environment, range) pair is one request, kept for the session: the
// response is immutable for this version, so going back to a pair is free.
const timingsMemo = new Map();

async function loadTimings(env, range) {
  const key = `${env}\u0000${range}`;
  let t = timingsMemo.get(key);
  if (!t) {
    t = await fetchJSON('timings', { env, range });
    timingsMemo.set(key, t);
  }
  setTimings(t);
}

// The Test counts and Benchmarks tabs are the only readers of their data, so
// it is fetched the first time either opens rather than on every view.
let countsLoad = null;
function ensureCounts() {
  countsLoad ??= fetchJSON('counts')
    .then((c) => { state.counts = c; })
    .catch((err) => {
      state.countsError = `Test counts are unavailable (${err.message}).`;
    })
    .then(() => { if (state.tab === 'counts') renderCounts(); });
  return countsLoad;
}

let benchLoad = null;
function ensureBench() {
  benchLoad ??= fetchJSON('bench')
    .then(({ rows }) => {
      state.bench = rows.sort((a, b) => a.ts - b.ts);
      if (!state.bench.length) state.benchError = 'No benchmark rows have been published yet.';
    })
    .catch((err) => {
      state.benchError = `Benchmarks are unavailable (${err.message}). The`
        + ' nightly arm64 workflow publishes them.';
    })
    .then(() => { if (state.tab === 'bench') renderBench(); });
  return benchLoad;
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
  if (tab === 'counts') { renderCounts(); ensureCounts(); }
  if (tab === 'code') renderLoc();
  if (tab === 'spices') renderSpices();
  if (tab === 'reports') { renderDocsTiles(); renderDocs(); renderFlow(); renderGuides(); }
  if (tab === 'bench') { renderBench(); ensureBench(); }
  writeURL();
}

async function boot() {
  let meta;
  try {
    const res = await fetch(`${API}/meta`, { signal: AbortSignal.timeout(FETCH_TIMEOUT_MS) });
    if (!res.ok) throw new Error(`${res.status} ${res.statusText}`);
    meta = await res.json();
  } catch (err) {
    fail(`Could not load CI metrics (${err.message}). The data is published from
          pushes to main; try again in a few minutes.`);
    return;
  }
  state.version = meta.version;
  state.ingestAt = meta.at;
  state.envs = meta.envs;
  if (!state.envs.length) {
    fail('No timing rows have been published yet.');
    return;
  }

  const url = readURL();
  state.env = state.envs.some((e) => e.key === url.env) ? url.env : state.envs[0].key;
  if (url.range && RANGES.some(([v]) => v === url.range)) state.range = url.range;

  // The timings, line counts and doc counts at once, and settled rather than
  // raced: the line and doc counts are independent, so they must neither
  // delay the timings nor be able to take the page down with them.
  const [timings, loc, docs] = await Promise.allSettled([
    loadTimings(state.env, state.range),
    fetchJSON('loc'),
    fetchJSON('docs'),
  ]);

  if (timings.status === 'rejected') {
    fail(`Could not load CI timings (${timings.reason.message}). The data is
          published from pushes to main; try again in a few minutes.`);
    return;
  }

  if (loc.status === 'fulfilled') {
    state.loc = loc.value.rows;
    if (!state.loc.length) state.locError = 'No line counts have been published yet.';
  } else {
    state.locError = `Line counts are unavailable (${loc.reason.message}).`;
  }

  if (docs.status === 'fulfilled') {
    state.docs = docs.value.rows;
    for (const r of state.docs) {
      r.retired_plans = (r.held_plans ?? 0) + (r.v1_plans ?? 0);
    }
    if (!state.docs.length) state.docsError = 'No doc counts have been published yet.';
  } else {
    state.docsError = `Doc counts are unavailable (${docs.reason.message}).`;
  }

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
  if (url.bench && BENCH_VIEWS.some(([key]) => key === url.bench)) state.benchView = url.bench;

  // A shared link can name a suite this environment and range never ran; its
  // slot is left empty rather than charted as nothing.
  const known = new Set(state.timings.suites);
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
  // A new range is one request for this environment; the other tabs filter
  // what they already have.
  const range = document.getElementById('ci-range');
  range.value = state.range;
  range.onchange = async (e) => {
    const want = e.target.value;
    range.disabled = true;
    let failed = null;
    try {
      await loadTimings(state.env, want);
      state.range = want;
      const kept = new Set(state.timings.suites);
      state.suites = state.suites.map((s) => (s && kept.has(s) ? s : null));
      if (!selected().length) state.suites = defaultSuites();
    } catch (err) {
      failed = err;
      range.value = state.range;
    }
    range.disabled = false;
    render();
    // After the render, which would otherwise put the provenance line back
    // over it.
    if (failed) notice(`Could not load that range (${failed.message}).`);
  };

  // Tab bar: static HTML, wired here.
  for (const t of document.querySelectorAll('.ci-tab')) {
    t.onclick = () => switchTab(t.dataset.tab);
  }

  const search = document.getElementById('ci-spark-search');
  search.addEventListener('input', () => {
    state.sparkFilter = search.value;
    renderSparks(suiteStats(bySuite()));
  });

  render();
  switchTab(state.tab);

  // Re-render (rather than scale) on resize so text never distorts.
  const redraw = new Map([
    [document.getElementById('ci-chart'), renderChart],
    [document.getElementById('ci-counts-chart'), renderCounts],
    [document.getElementById('ci-loc-chart'), renderLoc],
    [document.getElementById('ci-docs-chart'), renderDocs],
    [document.getElementById('ci-flow-chart'), renderFlow],
    [document.getElementById('ci-guides-chart'), renderGuides],
    [document.getElementById('ci-spices-chart'), renderSpices],
    [document.getElementById('ci-bench-chart'), renderBench],
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
