// ci-aggregate.js -- the /ci data rules, shared by the Worker and the page.
//
// The Worker (ci-api.js) builds its responses with these; the page
// (ci-metrics.js) decodes them with the same module; and the page tests build
// their route stubs with it too, so a stubbed response has exactly the shape a
// real one does. One implementation of each rule, not a port per side.
//
// Pure functions only: no DOM, no D1, no Worker globals.

export const DAY = 86400;

export const RANGES = ['all', '90d', '30d', '7d', '1d'];

// The 1d and 7d views draw one point per run, so a tooltip names the commit.
// Wider ranges draw one point per suite per UTC day (the daily median), which
// keeps a response the same size however much history there is.
export const RUN_RANGES = ['1d', '7d'];
export const modeOf = (range) => (RUN_RANGES.includes(range) ? 'run' : 'day');

export const STATUSES = ['pass', 'skip', 'fail'];
export const statusCode = (s) => Math.max(0, STATUSES.indexOf(s));

// build_type is single-valued today but need not stay so, so it is part of the
// key. It is only shown in the label when the data actually varies. sanitize is
// NOT in the key: it never was, and the key is what ?env= links carry.
export function envKey(r) {
  return [r.build_type, r.os, r.cc, r.nproc, r.jit ? 'jit' : 'nojit'].join('|');
}

// A sharded job (TUR_TEST_SHARD="i/N") reports the same suite once per shard,
// each with ~1/N of the work.  Keyed on the bare name they would land in one
// series as N points per run at a fraction of the real duration -- a suite that
// appears to have got faster and noisier on the day sharding was switched on.
// The shard is therefore part of the series identity, so each slice trends
// against its own history.  Unsharded rows (shard_total null or 0, which is
// every row written before sharding existed) keep the bare name and their
// continuity.
export function seriesName(r) {
  return r.shard_total ? `${r.suite} [${r.shard_index}/${r.shard_total}]` : r.suite;
}

// `AppleClang-21.0.0` -> `AppleClang 21`, `GNU-13.3.0` -> `GNU 13.3`.
export function prettyCC(cc) {
  const dash = cc.lastIndexOf('-');
  if (dash < 0) return cc;
  const name = cc.slice(0, dash);
  const parts = cc.slice(dash + 1).split('.');
  const ver = parts[1] && parts[1] !== '0'
    ? `${parts[0]}.${parts[1]}`
    : parts[0];
  return `${name} ${ver}`;
}

// The environment picker. `envs` is [{ build_type, os, cc, nproc, jit, ts,
// breadth }] -- one per env key, ts its newest row, breadth how many series it
// ran in the last week. Returns [{ key, label, ts, breadth }], best first.
//
// Still-active environments first, then the BROADEST of those -- not simply
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
export function orderEnvs(envs) {
  const buildTypes = new Set(envs.map((e) => e.build_type));
  const out = envs.map((e) => {
    const bits = [e.os, prettyCC(e.cc), `${e.nproc} cores`];
    if (e.jit) bits.push('JIT');
    if (buildTypes.size > 1) bits.unshift(e.build_type);
    return { key: envKey(e), label: bits.join(' · '), ts: e.ts, breadth: e.breadth };
  });
  const newest = out.reduce((m, e) => Math.max(m, e.ts), 0);
  out.sort((a, b) => (
    (a.ts >= newest - DAY ? 0 : 1) - (b.ts >= newest - DAY ? 0 : 1)
    || b.breadth - a.breadth
    || b.ts - a.ts
    || a.label.localeCompare(b.label)
  ));
  return out;
}

// The env summaries orderEnvs() takes, from raw suite rows. The Worker gets
// these from SQL; the page tests get them from their fixtures through here.
export function summarizeEnvs(rows) {
  const seen = new Map();
  for (const r of rows) {
    const key = envKey(r);
    let e = seen.get(key);
    if (!e) {
      seen.set(key, (e = {
        build_type: r.build_type, os: r.os, cc: r.cc, nproc: r.nproc, jit: !!r.jit,
        ts: r.ts, series: new Map(),
      }));
    }
    e.ts = Math.max(e.ts, r.ts);
    const name = seriesName(r);
    e.series.set(name, Math.max(e.series.get(name) ?? 0, r.ts));
  }
  return [...seen.values()].map(({ series, ...e }) => ({
    ...e,
    breadth: [...series.values()].filter((t) => t >= e.ts - 8 * DAY).length,
  }));
}

// Relative to the newest row, not to wall-clock now: if CI has been quiet for
// two days, "last 24 hours" must still show the last day that has data rather
// than an empty chart. Each dataset therefore anchors on its own latest row.
export function rangeCutoff(latestTs, range) {
  if (range === 'all') return -Infinity;
  return latestTs - parseInt(range, 10) * DAY;
}

// ── TIMINGS ─────────────────────────────────────────────────────────────────

// `rows`: one environment's suite rows in range, each { suite, shard_index,
// shard_total, ts, sha, duration_ms, status, cpu? }. One point per run:
//   suites  series names, sorted
//   cpus    the runners' CPU models seen, sorted
//   runs    [[ts, sha12, cpuIdx]], ascending -- the x domain, one per run;
//           cpuIdx is -1 for a row from before the model was recorded
//   cells   [[suiteIdx, runIdx, ms, statusCode]]
//
// The CPU is per run, not per cell, because a `ts` is one job's upload and a
// job is one machine.
export function shapeRuns(rows) {
  const suites = [...new Set(rows.map(seriesName))].sort();
  const sIdx = new Map(suites.map((s, i) => [s, i]));
  const cpus = [...new Set(rows.map((r) => r.cpu).filter(Boolean))].sort();
  const cIdx = new Map(cpus.map((c, i) => [c, i]));
  const runAt = new Map();
  for (const r of rows) {
    const run = runAt.get(r.ts);
    if (!run) runAt.set(r.ts, { sha: String(r.sha ?? '').slice(0, 12), cpu: r.cpu || null });
    else if (!run.cpu && r.cpu) run.cpu = r.cpu;
  }
  const ts = [...runAt.keys()].sort((a, b) => a - b);
  const rIdx = new Map(ts.map((t, i) => [t, i]));
  const cells = rows.map((r) => [
    sIdx.get(seriesName(r)), rIdx.get(r.ts), r.duration_ms, statusCode(r.status),
  ]);
  cells.sort((a, b) => a[0] - b[0] || a[1] - b[1]);
  const runs = ts.map((t) => {
    const { sha, cpu } = runAt.get(t);
    return [t, sha, cpu ? cIdx.get(cpu) : -1];
  });
  return { suites, cpus, runs, cells };
}

// "AMD EPYC 7763 64-Core Processor" -> "EPYC 7763", "Intel(R) Xeon(R)
// Platinum 8370C CPU @ 2.80GHz" -> "Xeon Platinum 8370C", "Apple M1 (Virtual)"
// -> "Apple M1 (Virtual)". For chips and tooltips; the full string stays in
// the data and in the chip's title.
export function prettyCPU(cpu) {
  if (!cpu) return 'not recorded';
  const short = cpu
    .replace(/\((R|TM)\)/gi, '')
    .replace(/\s+@\s*[\d.]+\s*GHz/i, '')
    .replace(/\b(\d+-Core|CPU|Processor)\b/gi, '')
    .replace(/^(AMD|Intel)\s+/i, '')
    .replace(/\s+/g, ' ')
    .trim();
  return short || cpu;
}

// The daily medians, from raw rows: one per series, UTC day and status. The
// Worker computes the same thing in SQL (ci-api.js DAILY_SQL); this is what
// the page tests stub with, and the D1 round-trip test holds the two equal.
export function dailyMedians(rows) {
  const groups = new Map();
  for (const r of rows) {
    const day = Math.floor(r.ts / DAY);
    const key = `${seriesName(r)}\u0000${day}\u0000${r.status}`;
    let g = groups.get(key);
    if (!g) {
      groups.set(key, (g = {
        suite: r.suite, shard_index: r.shard_index ?? 0, shard_total: r.shard_total ?? 0,
        day, status: r.status, ms: [],
      }));
    }
    g.ms.push(r.duration_ms);
  }
  return [...groups.values()].map(({ ms, ...g }) => {
    ms.sort((a, b) => a - b);
    const n = ms.length;
    const median = n % 2 ? ms[(n - 1) / 2] : (ms[n / 2 - 1] + ms[n / 2]) / 2;
    return { ...g, median, n };
  });
}

// `medians`: [{ suite, shard_index, shard_total, day, status, median, n }].
//   suites  series names, sorted
//   cells   [[suiteIdx, day, medianMs, n, statusCode]], day = UTC days since 1970
export function shapeDays(medians) {
  const suites = [...new Set(medians.map(seriesName))].sort();
  const sIdx = new Map(suites.map((s, i) => [s, i]));
  const cells = medians.map((m) => [
    sIdx.get(seriesName(m)), m.day, m.median, m.n, statusCode(m.status),
  ]);
  cells.sort((a, b) => a[0] - b[0] || a[1] - b[1] || a[4] - b[4]);
  return { suites, cells };
}

// Every row of the environment's newest run: the tiles, the skip ledger and the
// provenance line all read it.
export function shapeLatest(rows) {
  return rows.map((r) => ({
    suite: seriesName(r),
    ms: r.duration_ms,
    status: r.status,
    skip_reason: r.skip_reason ?? null,
    partial_skip_reason: r.partial_skip_reason ?? null,
  })).sort((a, b) => a.suite.localeCompare(b.suite));
}

// The whole /api/ci/timings body from one environment's raw rows -- what the
// page tests stub with. The Worker assembles the same object from SQL.
export function timingsFromRows(allRows, key, range) {
  const rows = allRows.filter((r) => envKey(r) === key);
  const latestTs = rows.reduce((m, r) => Math.max(m, r.ts), 0);
  const cutoff = rangeCutoff(latestTs, range);
  const inRange = rows.filter((r) => r.ts >= cutoff);
  const mode = modeOf(range);
  const newest = rows.filter((r) => r.ts === latestTs);
  return {
    env: key,
    range,
    mode,
    ...(mode === 'run' ? shapeRuns(inRange) : shapeDays(dailyMedians(inRange))),
    latest: shapeLatest(newest),
    latest_ts: latestTs,
    latest_sha: newest[0]?.sha ?? null,
  };
}

// ── TEST COUNTS ─────────────────────────────────────────────────────────────

// How many tests a suite row counted, or null when the suite printed no census
// line (collect-suite-timings.py's SUMMARY_RE). `discovered` when the harness
// reports it; otherwise what it ran plus what it skipped.
export function testCount(r) {
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
//
// `envOrder` is orderEnvs()'s keys, the tie-break between equal homes.
export function suiteCounts(rows, envOrder) {
  const rowsBy = new Map();   // suite -> envKey -> rows
  for (const r of rows) {
    if (testCount(r) == null) continue;
    let envs = rowsBy.get(r.suite);
    if (!envs) rowsBy.set(r.suite, (envs = new Map()));
    const k = envKey(r);
    if (!envs.has(k)) envs.set(k, []);
    envs.get(k).push(r);
  }
  const envRank = new Map(envOrder.map((k, i) => [k, i]));
  const out = new Map();
  for (const [suite, envs] of rowsBy) {
    const whole = (rs) => rs.filter((r) => !r.shard_total).length;
    const [, envRows] = [...envs].sort((a, b) => (
      whole(b[1]) - whole(a[1]) || b[1].length - a[1].length
      || (envRank.get(a[0]) ?? 0) - (envRank.get(b[0]) ?? 0)
    ))[0];
    const byRun = new Map();
    const unsharded = envRows.filter((r) => !r.shard_total);
    for (const r of unsharded.length ? unsharded : envRows) {
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

// Totals over time, one point per CI run: { ts, sha, total, failedHere }. Every
// suite contributes its NEWEST count as of that run, not only the suites the
// run itself executed -- a push runs only the jobs its diff needs, so a plain
// per-run sum sawtooths between ~3.7k and ~9k with no test added or removed.
//
// Two edges keep the carry-forward honest. A suite last seen more than a week
// before the newest data has been removed, so it leaves the sum after its last
// run instead of being carried to the end. And the points start once every
// suite present on the data's first day has reported: before that, the sum is
// missing suites that simply had not run yet.
export function countTimeline(series) {
  const events = [];
  for (const [suite, pts] of series) for (const p of pts) events.push({ suite, ...p });
  events.sort((a, b) => a.ts - b.ts);
  if (!events.length) return [];
  const newest = events[events.length - 1].ts;
  const lastTs = new Map([...series].map(([s, pts]) => [s, pts[pts.length - 1].ts]));
  const firstTs = [...series.values()].map((pts) => pts[0].ts);
  const dataStart = Math.min(...firstTs);
  const warm = Math.max(...firstTs.filter((t) => t <= dataStart + DAY));

  const cur = new Map();
  const snaps = new Map();   // run -> snapshot after that run's last event
  for (const e of events) {
    cur.set(e.suite, e);
    for (const s of cur.keys()) {
      const end = lastTs.get(s);
      if (end < e.ts && end < newest - 7 * DAY) cur.delete(s);   // retired
    }
    const snap = { ts: e.ts, sha: e.sha, total: 0, failedHere: snaps.get(e.run)?.failedHere ?? false };
    for (const p of cur.values()) snap.total += p.count;
    snap.failedHere = snap.failedHere || e.failed > 0;
    snaps.set(e.run, snap);
  }
  return [...snaps.values()].filter((p) => p.ts >= warm).sort((a, b) => a.ts - b.ts);
}

// The whole /api/ci/counts body. `rows` are counted suite rows from every
// environment; `suiteNames` every suite (counted or not) seen in the last
// week, for the "N other suites print none" note.
//   timeline  [[ts, sha12, total, failedHere 0|1]], ascending
//   current   each live suite's newest count, summed
export function shapeCounts(rows, envOrder, suiteNames) {
  const series = suiteCounts(rows, envOrder);
  const timeline = countTimeline(series);
  let current = { suites: 0, total: 0, failed: 0, skipped: 0 };
  if (series.size) {
    const newestTs = Math.max(...[...series.values()].map((pts) => pts[pts.length - 1].ts));
    const live = [...series.values()]
      .map((pts) => pts[pts.length - 1])
      .filter((p) => p.ts >= newestTs - 7 * DAY);
    current = {
      suites: live.length,
      total: live.reduce((a, p) => a + p.count, 0),
      failed: live.reduce((a, p) => a + p.failed, 0),
      skipped: live.reduce((a, p) => a + p.skipped, 0),
    };
  }
  const silent = new Set(suiteNames);
  for (const name of series.keys()) silent.delete(name);
  return {
    timeline: timeline.map((p) => [p.ts, String(p.sha ?? '').slice(0, 12), p.total, p.failedHere ? 1 : 0]),
    current,
    silent: silent.size,
  };
}

// /api/ci/counts from raw suite rows -- the page tests' stub.
export function countsFromRows(allRows) {
  const envOrder = orderEnvs(summarizeEnvs(allRows)).map((e) => e.key);
  const newest = allRows.reduce((m, r) => Math.max(m, r.ts), 0);
  const names = allRows.filter((r) => r.ts >= newest - 8 * DAY).map((r) => r.suite);
  return shapeCounts(allRows.filter((r) => r.passed != null), envOrder, names);
}

// ── PER-COMMIT ROWS ─────────────────────────────────────────────────────────

// The loc and docs panels chart a handful of scalar fields per push, and read
// the bulky ones (the by-language breakdowns, the spice names) only from the
// newest row that has them. So only that row keeps them.
export function slimCommitRows(rows, bulky) {
  const sorted = rows.slice().sort((a, b) => a.ts - b.ts);
  const keepAt = new Map();
  for (const k of bulky) {
    for (let i = sorted.length - 1; i >= 0; i--) {
      if (sorted[i][k] != null) { keepAt.set(k, i); break; }
    }
  }
  return sorted.map((r, i) => {
    const out = {};
    for (const [k, v] of Object.entries(r)) {
      if (v == null || k === 'run_id' || k === 'run_attempt' || k === 'branch') continue;
      if (bulky.includes(k) && keepAt.get(k) !== i) continue;
      out[k] = v;
    }
    return out;
  });
}

export const LOC_BULKY = ['code_by_lang', 'test_by_lang'];
export const DOCS_BULKY = ['spice_names'];
