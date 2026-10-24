// ci-api.js -- /api/ci/*, the D1-backed store behind /ci.
//
// docs/upcoming/ci-metrics-d1-plan.md. CI POSTs each push's rows to
// /api/ci/report (tools/ci/report-metrics.py); the page reads chart-ready
// responses from the GET endpoints below. Every GET but /api/ci/meta carries
// the current ingest version (?v=), so its response never changes and is sent
// immutable: a repeat view between pushes downloads nothing but the meta.
//
// Bindings (wrangler.jsonc): CI_DB, the D1 database; CI_METRICS_TOKEN, the
// secret CI presents as a bearer token.

import { CONTENT_SECURITY_POLICY } from './csp.js';
import {
  DAY, RANGES, modeOf, envKey, orderEnvs, rangeCutoff, shapeRuns, shapeDays,
  shapeLatest, shapeCounts, slimCommitRows, LOC_BULKY, DOCS_BULKY, STATUSES,
} from './ci-aggregate.js';

const IMMUTABLE = 'public, max-age=31536000, immutable';

// One report is ~120 KB; a backfill chunk ~750 KB. Far above either, far
// below what a Worker will buffer.
const MAX_BODY = 8 * 1024 * 1024;

// D1 caps a bound value at 2 MB, and each array goes in as ONE bound JSON
// string. Anything bigger is split into several statements -- still in the same
// batch, so still one transaction.
const MAX_CHUNK = 1_000_000;

// ── WRITE PATH ──────────────────────────────────────────────────────────────

const str = (v) => typeof v === 'string' && v.length > 0;
const int = (v) => Number.isInteger(v);
const num = (v) => typeof v === 'number' && Number.isFinite(v);
const id = (v) => str(v) || int(v);
const bool = (v) => typeof v === 'boolean';

// One entry per report array. `cols` are [column, sql?]: the default SQL reads
// the same-named key off the row (`j` is the json_each row). `required` is
// checked in JS first, so a bad row is named in a 400 rather than surfacing as
// a constraint error from the middle of the batch.
const ENV_MATCH = `(SELECT id FROM env WHERE build_type = j.value ->> '$.build_type'
  AND os = j.value ->> '$.os' AND cc = j.value ->> '$.cc'
  AND nproc = j.value ->> '$.nproc' AND jit = coalesce(j.value ->> '$.jit', 0)
  AND sanitize = coalesce(j.value ->> '$.sanitize', 0))`;

const TABLES = {
  timings: {
    table: 'suite_run',
    key: ['run_id', 'run_attempt', 'env_id', 'suite', 'shard_index', 'shard_total'],
    cols: [
      ['run_id'], ['run_attempt'], ['env_id', ENV_MATCH], ['suite'],
      ['shard_index', `coalesce(j.value ->> '$.shard_index', 0)`],
      ['shard_total', `coalesce(j.value ->> '$.shard_total', 0)`],
      ['kind'], ['ts'], ['sha'], ['branch'], ['duration_ms'], ['status'],
      ['skip_reason'], ['partial_skip_reason'], ['passed'], ['failed'],
      ['skipped'], ['discovered'], ['samples'],
    ],
    required: {
      run_id: id, run_attempt: int, suite: str, ts: int, sha: str,
      duration_ms: num, status: (v) => STATUSES.includes(v),
      build_type: str, os: str, cc: str, nproc: int, jit: bool,
    },
  },
  loc: {
    table: 'repo_loc',
    key: ['run_id', 'run_attempt'],
    cols: [
      ['run_id'], ['run_attempt'], ['ts'], ['sha'], ['branch'],
      ['code_lines'], ['test_lines'], ['bench_lines'], ['example_lines'],
      ['generated_lines'], ['code_files'], ['test_files'], ['bench_files'],
      ['example_files'], ['generated_files'], ['uncounted_files'],
      ['code_by_lang', `j.value -> '$.code_by_lang'`],
      ['test_by_lang', `j.value -> '$.test_by_lang'`],
    ],
    required: { run_id: id, run_attempt: int, ts: int, sha: str },
  },
  docs: {
    table: 'docs_counts',
    key: ['run_id', 'run_attempt'],
    cols: [
      ['run_id'], ['run_attempt'], ['ts'], ['sha'], ['branch'],
      ['open_plans'], ['open_reports'], ['active_plans'], ['godot_plans'],
      ['spices_plans'], ['held_plans'], ['v1_plans'], ['turmeric_guides'],
      ['spice_guides'], ['spices'],
      ['spice_names', `j.value -> '$.spice_names'`],
      ['reports_filed'], ['reports_archived'],
    ],
    required: { run_id: id, run_attempt: int, ts: int, sha: str },
  },
  bench: {
    table: 'bench',
    key: ['run_id', 'run_attempt', 'suite'],
    cols: [
      ['run_id'], ['run_attempt'], ['suite'], ['ts'], ['sha'], ['branch'],
      ['program'], ['engine'], ['metric'], ['value'], ['unit'], ['cv'],
      ['noisy'], ['reps'], ['status'], ['os'], ['cc'], ['nproc'],
      ['build_type'], ['tur_version'],
    ],
    required: { run_id: id, run_attempt: int, suite: str, ts: int },
  },
};

// INSERT ... SELECT FROM json_each(?1): one statement per array, however many
// rows. `WHERE true` is SQLite's required disambiguation between an upsert's
// ON CONFLICT and a join's ON. A retried report, or a re-run of the same
// run_attempt, overwrites its rows instead of doubling them.
function upsertSQL({ table, key, cols }) {
  const names = cols.map(([c]) => c);
  const exprs = cols.map(([c, sql]) => sql ?? `j.value ->> '$.${c}'`);
  const set = names.filter((c) => !key.includes(c)).map((c) => `${c} = excluded.${c}`);
  return `INSERT INTO ${table} (${names.join(', ')})
    SELECT ${exprs.join(', ')} FROM json_each(?1) AS j WHERE true
    ON CONFLICT (${key.join(', ')}) DO UPDATE SET ${set.join(', ')}`;
}

const ENV_SQL = `INSERT OR IGNORE INTO env (build_type, os, cc, nproc, jit, sanitize)
  SELECT DISTINCT j.value ->> '$.build_type', j.value ->> '$.os', j.value ->> '$.cc',
         j.value ->> '$.nproc', coalesce(j.value ->> '$.jit', 0),
         coalesce(j.value ->> '$.sanitize', 0)
  FROM json_each(?1) AS j`;

const UPSERTS = Object.fromEntries(
  Object.entries(TABLES).map(([name, spec]) => [name, upsertSQL(spec)]),
);

// The first bad row and field, or null.
function invalid(name, rows) {
  if (!Array.isArray(rows)) return `${name}: expected an array`;
  const { required } = TABLES[name];
  for (let i = 0; i < rows.length; i++) {
    const r = rows[i];
    if (r == null || typeof r !== 'object' || Array.isArray(r)) return `${name}[${i}]: not an object`;
    for (const [field, ok] of Object.entries(required)) {
      if (!ok(r[field])) {
        return `${name}[${i}]: field "${field}" is missing or malformed (got ${JSON.stringify(r[field]) ?? 'nothing'})`;
      }
    }
  }
  return null;
}

// Rows -> JSON strings of at most MAX_CHUNK characters each.
function chunks(rows) {
  const out = [];
  let cur = [];
  let size = 2;
  for (const r of rows) {
    const s = JSON.stringify(r);
    if (cur.length && size + s.length + 1 > MAX_CHUNK) {
      out.push(`[${cur.join(',')}]`);
      cur = [];
      size = 2;
    }
    cur.push(s);
    size += s.length + 1;
  }
  if (cur.length) out.push(`[${cur.join(',')}]`);
  return out;
}

// Length-independent compare: comparing the header against itself on a length
// mismatch keeps the rejection from being measurably faster.
function tokenMatches(header, secret) {
  const enc = new TextEncoder();
  const got = enc.encode(header ?? '');
  const want = enc.encode(`Bearer ${secret}`);
  if (got.byteLength !== want.byteLength) {
    crypto.subtle.timingSafeEqual(want, want);
    return false;
  }
  return crypto.subtle.timingSafeEqual(got, want);
}

async function report(request, env) {
  if (!env.CI_METRICS_TOKEN) return error(503, 'CI_METRICS_TOKEN is not configured on this Worker');
  if (!tokenMatches(request.headers.get('Authorization'), env.CI_METRICS_TOKEN)) {
    return error(401, 'bad or missing bearer token');
  }
  const declared = Number(request.headers.get('Content-Length') ?? 0);
  if (declared > MAX_BODY) return error(413, `body over ${MAX_BODY} bytes`);
  const text = await request.text();
  if (text.length > MAX_BODY) return error(413, `body over ${MAX_BODY} bytes`);

  let body;
  try {
    body = JSON.parse(text);
  } catch (e) {
    return error(400, `body is not JSON: ${e.message}`);
  }
  if (body == null || typeof body !== 'object' || Array.isArray(body)) {
    return error(400, 'body must be an object of row arrays');
  }
  const unknown = Object.keys(body).filter((k) => !(k in TABLES));
  if (unknown.length) return error(400, `unknown key(s): ${unknown.join(', ')}`);

  const counts = {};
  for (const name of Object.keys(TABLES)) {
    const rows = body[name] ?? [];
    const bad = invalid(name, rows);
    if (bad) return error(400, bad);
    counts[name] = rows.length;
  }
  if (!Object.values(counts).some(Boolean)) return error(400, 'empty report: no rows in any array');

  const db = env.CI_DB;
  const stmts = [];
  for (const name of Object.keys(TABLES)) {
    for (const json of chunks(body[name] ?? [])) {
      if (name === 'timings') stmts.push(db.prepare(ENV_SQL).bind(json));
      stmts.push(db.prepare(UPSERTS[name]).bind(json));
    }
  }
  // Which run this report came from, for the record: the first row that says.
  const first = ['timings', 'loc', 'docs', 'bench'].map((k) => body[k]?.[0]).find(Boolean);
  stmts.push(db.prepare(`INSERT INTO ingest
      (at, run_id, run_attempt, sha, suite_rows, loc_rows, docs_rows, bench_rows)
      VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8) RETURNING id`)
    .bind(Math.floor(Date.now() / 1000), String(first.run_id), first.run_attempt,
      first.sha ?? null, counts.timings, counts.loc, counts.docs, counts.bench));

  // db.batch() is one transaction: the report lands whole or not at all.
  const results = await db.batch(stmts);
  const ingest = results[results.length - 1].results[0].id;
  return json({
    ingest,
    suite_rows: counts.timings,
    loc_rows: counts.loc,
    docs_rows: counts.docs,
    bench_rows: counts.bench,
  }, 'no-store');
}

// ── READ PATH ───────────────────────────────────────────────────────────────

async function currentVersion(db) {
  const r = await db.prepare('SELECT id, at FROM ingest ORDER BY id DESC LIMIT 1').first();
  return r ? { version: r.id, at: r.at } : { version: 0, at: null };
}

// Every environment that has suite rows, merged by the page's env key (two
// rows of `env` can share one when only sanitize differs), in picker order.
// Also each key's env ids, for the timings query.
async function listEnvs(db) {
  const { results } = await db.prepare(`SELECT e.id, e.build_type, e.os, e.cc, e.nproc, e.jit,
      (SELECT max(s.ts) FROM suite_run s WHERE s.env_id = e.id) AS ts
    FROM env e`).all();
  const live = results.filter((e) => e.ts != null);
  // Breadth: series the environment ran in its own last 8 days.
  const breadths = live.length ? await db.batch(live.map((e) => db.prepare(
    `SELECT count(DISTINCT suite || char(31) || shard_index || char(31) || shard_total) AS n
     FROM suite_run WHERE env_id = ?1 AND kind IS NULL AND ts >= ?2`,
  ).bind(e.id, e.ts - 8 * DAY))) : [];
  const byKey = new Map();
  live.forEach((e, i) => {
    const breadth = breadths[i].results[0].n;
    if (!breadth) return;   // only measurement rows (kind), no suites
    const key = envKey(e);
    const cur = byKey.get(key);
    if (!cur) byKey.set(key, { ...e, jit: !!e.jit, breadth, ids: [e.id] });
    else {
      cur.ts = Math.max(cur.ts, e.ts);
      cur.breadth = Math.max(cur.breadth, breadth);
      cur.ids.push(e.id);
    }
  });
  const ordered = orderEnvs([...byKey.values()]);
  return ordered.map((e) => ({ ...e, ids: byKey.get(e.key).ids }));
}

const inList = (ids) => ids.map((_, i) => `?${i + 1}`).join(', ');

// Daily medians in SQL: rank each (series, UTC day, status) bucket's durations
// and average the middle one or two. ci-aggregate.js dailyMedians() is the same
// rule in JS, and the round-trip test holds them equal.
function dailySQL(ids) {
  const n = ids.length;
  return `WITH b AS (
      SELECT suite, shard_index, shard_total, ts / 86400 AS day, status,
             duration_ms AS ms,
             row_number() OVER w AS rn,
             count(*) OVER (PARTITION BY suite, shard_index, shard_total, ts / 86400, status) AS n
      FROM suite_run
      WHERE env_id IN (${inList(ids)}) AND kind IS NULL AND ts >= ?${n + 1}
      WINDOW w AS (PARTITION BY suite, shard_index, shard_total, ts / 86400, status
                   ORDER BY duration_ms))
    SELECT suite, shard_index, shard_total, day, status, avg(ms) AS median, max(n) AS n
    FROM b WHERE rn IN ((n + 1) / 2, (n + 2) / 2)
    GROUP BY suite, shard_index, shard_total, day, status`;
}

async function timings(db, url, envs) {
  const key = url.searchParams.get('env');
  const range = url.searchParams.get('range') ?? '7d';
  if (!RANGES.includes(range)) return error(400, `range must be one of ${RANGES.join(', ')}`);
  const env = envs.find((e) => e.key === key);
  if (!env) return error(404, `no environment ${JSON.stringify(key)}`);
  const { ids } = env;
  const n = ids.length;

  const top = await db.prepare(`SELECT ts, sha FROM suite_run
      WHERE env_id IN (${inList(ids)}) AND kind IS NULL ORDER BY ts DESC LIMIT 1`)
    .bind(...ids).first();
  const latestTs = top?.ts ?? 0;
  const cutoff = rangeCutoff(latestTs, range);
  const mode = modeOf(range);

  const [body, latest] = await db.batch([
    mode === 'run'
      ? db.prepare(`SELECT suite, shard_index, shard_total, ts, sha, duration_ms, status
          FROM suite_run WHERE env_id IN (${inList(ids)}) AND kind IS NULL AND ts >= ?${n + 1}`)
        .bind(...ids, Number.isFinite(cutoff) ? cutoff : 0)
      : db.prepare(dailySQL(ids)).bind(...ids, Number.isFinite(cutoff) ? cutoff : 0),
    db.prepare(`SELECT suite, shard_index, shard_total, duration_ms, status, skip_reason,
        partial_skip_reason FROM suite_run
        WHERE env_id IN (${inList(ids)}) AND kind IS NULL AND ts = ?${n + 1}`)
      .bind(...ids, latestTs),
  ]);
  return {
    env: key,
    range,
    mode,
    ...(mode === 'run' ? shapeRuns(body.results) : shapeDays(body.results)),
    latest: shapeLatest(latest.results),
    latest_ts: latestTs,
    latest_sha: top?.sha ?? null,
  };
}

async function counts(db, envs) {
  const newest = (await db.prepare('SELECT max(ts) AS ts FROM suite_run').first())?.ts ?? 0;
  const [rows, names] = await db.batch([
    db.prepare(`SELECT s.suite, s.shard_index, s.shard_total, s.run_id, s.run_attempt, s.ts,
        s.sha, s.passed, s.failed, s.skipped, s.discovered,
        e.build_type, e.os, e.cc, e.nproc, e.jit
      FROM suite_run s JOIN env e ON e.id = s.env_id
      WHERE s.passed IS NOT NULL AND s.kind IS NULL`),
    db.prepare('SELECT DISTINCT suite FROM suite_run WHERE kind IS NULL AND ts >= ?1')
      .bind(newest - 8 * DAY),
  ]);
  return {
    ...shapeCounts(rows.results, envs.map((e) => e.key), names.results.map((r) => r.suite)),
    latest_ts: newest,
  };
}

const parseJSONCols = (rows, cols) => rows.map((r) => {
  for (const c of cols) if (r[c] != null) r[c] = JSON.parse(r[c]);
  return r;
});

async function loc(db) {
  const { results } = await db.prepare('SELECT * FROM repo_loc ORDER BY ts').all();
  return { rows: slimCommitRows(parseJSONCols(results, LOC_BULKY), LOC_BULKY) };
}

async function docs(db) {
  const { results } = await db.prepare('SELECT * FROM docs_counts ORDER BY ts').all();
  return { rows: slimCommitRows(parseJSONCols(results, DOCS_BULKY), DOCS_BULKY) };
}

// In the shape bench-rows.py wrote them, so its --history reader takes them
// unchanged.
const benchRow = (r) => ({ kind: 'bench', ...r, noisy: !!r.noisy });

async function bench(db, since) {
  const { results } = await db.prepare('SELECT * FROM bench WHERE ts >= ?1 ORDER BY ts')
    .bind(since).all();
  return results.map(benchRow);
}

// ── ROUTING ─────────────────────────────────────────────────────────────────

function json(body, cacheControl, status = 200) {
  return new Response(JSON.stringify(body), {
    status,
    headers: {
      'Content-Type': 'application/json; charset=utf-8',
      'Cache-Control': cacheControl,
      'Content-Security-Policy': CONTENT_SECURITY_POLICY,
    },
  });
}

function error(status, message) {
  return json({ error: message }, 'no-store', status);
}

// A versioned GET: redirect a stale or missing ?v= to the current one (so a
// stale tab cannot pin old data), then answer from the Workers cache or
// compute and store. A computed body that is itself a Response (an error) is
// passed through uncached.
async function versioned(request, url, db, ctx, compute) {
  const { version } = await currentVersion(db);
  if (url.searchParams.get('v') !== String(version)) {
    const to = new URL(url);
    to.searchParams.set('v', String(version));
    return new Response(null, {
      status: 302,
      headers: { Location: `${to.pathname}${to.search}`, 'Cache-Control': 'no-store' },
    });
  }
  const cache = globalThis.caches?.default;
  const hit = cache && await cache.match(url.toString());
  if (hit) return hit;
  const body = await compute(version);
  if (body instanceof Response) return body;
  const res = json(body, IMMUTABLE);
  if (cache) ctx?.waitUntil?.(cache.put(url.toString(), res.clone()));
  return res;
}

// listEnvs() is the costliest part of /api/ci/meta, and meta is fetched on
// every view, so the env list is cached per version under a key of its own.
async function envsFor(url, db, ctx, version) {
  const cache = globalThis.caches?.default;
  const key = `${url.origin}/api/ci/_envs?v=${version}`;
  const hit = cache && await cache.match(key);
  if (hit) return hit.json();
  const envs = await listEnvs(db);
  if (cache) {
    ctx?.waitUntil?.(cache.put(key, json(envs, IMMUTABLE)));
  }
  return envs;
}

export async function handleCiApi(request, env, ctx) {
  const url = new URL(request.url);
  const route = url.pathname.slice('/api/ci/'.length);
  const db = env.CI_DB;
  if (!db) return error(503, 'the CI_DB binding is not configured on this Worker');

  try {
    if (route === 'report') {
      if (request.method !== 'POST') return error(405, 'POST only');
      return await report(request, env);
    }
    if (request.method !== 'GET' && request.method !== 'HEAD') return error(405, 'GET only');

    if (route === 'meta') {
      const { version, at } = await currentVersion(db);
      const envs = await envsFor(url, db, ctx, version);
      return json({
        version,
        at,
        latest_ts: envs.reduce((m, e) => Math.max(m, e.ts), 0) || null,
        envs: envs.map(({ ids, ...e }) => e),
      }, 'public, max-age=30');
    }

    // The nightly tripwire's history: raw rows, never cached.
    if (route === 'bench' && url.searchParams.get('format') === 'ndjson') {
      const days = Math.max(1, Math.min(3660, Number(url.searchParams.get('days')) || 60));
      const rows = await bench(db, Math.floor(Date.now() / 1000) - days * DAY);
      return new Response(rows.map((r) => JSON.stringify(r)).join('\n') + (rows.length ? '\n' : ''), {
        headers: {
          'Content-Type': 'application/x-ndjson; charset=utf-8',
          'Cache-Control': 'no-store',
          'Content-Security-Policy': CONTENT_SECURITY_POLICY,
        },
      });
    }

    const reads = {
      timings: async (v) => timings(db, url, await envsFor(url, db, ctx, v)),
      counts: async (v) => counts(db, await envsFor(url, db, ctx, v)),
      loc: () => loc(db),
      docs: () => docs(db),
      bench: async () => ({ rows: await bench(db, 0) }),
    };
    if (route in reads) return await versioned(request, url, db, ctx, reads[route]);
    return error(404, `no route /api/ci/${route}`);
  } catch (e) {
    // D1 errors carry the SQL failure in the message; nothing secret is in it.
    return error(500, `${e.name}: ${e.message}`);
  }
}
