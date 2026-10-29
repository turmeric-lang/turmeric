#!/usr/bin/env python3
"""Report one CI run's metrics rows to the /ci store (D1, behind the site's
Worker), or backfill the history the retired `ci-metrics` branch holds.

docs/archive/ci-metrics-d1-plan.md. Reads the JSONL files the collectors in
tools/ci/ and benchmarks/audit/bench-rows.py write and POSTs them, as one JSON
body, to /api/ci/report (web/ci-api.js), which writes them in one D1
transaction. The endpoint is idempotent -- rows are keyed on run, attempt,
environment and suite -- so a retried or repeated report overwrites rather
than duplicates.

Usage:
    report-metrics.py timings.jsonl [--loc loc.jsonl] [--docs docs.jsonl]
                      [--bench bench.jsonl]
    report-metrics.py --backfill <checkout of the ci-metrics branch>

The branch was the store before D1, and it has since been deleted. D1 already
holds everything it had, so --backfill has nothing left to read; it is kept
for a checkout restored from a local copy. It is idempotent.

A missing or empty input file is a warning, not a failure: each array
is optional, and a docs-only push has counts but no timings. Nothing to report
at all exits 0.

Environment:
    CI_METRICS_TOKEN   the bearer token (required)
    CI_METRICS_URL     the endpoint (default https://turmeric-lang.com/api/ci/report)

Retries five times with backoff on a 5xx or a network error, never on a 4xx:
a 4xx names the bad row and field, and sending it again cannot fix it.
"""

import argparse
import glob
import json
import os
import sys
import time
import urllib.error
import urllib.request

DEFAULT_URL = "https://turmeric-lang.com/api/ci/report"
ATTEMPTS = 5
# A backfill request: ~750 KB of timing rows, well under the endpoint's 8 MB.
BACKFILL_CHUNK = 2000


def log(msg):
    print(f"report-metrics: {msg}", file=sys.stderr)


def read_rows(path, label):
    """The JSON rows in `path`, or [] (with a warning) when it is absent or
    empty. A torn line is skipped, as the page always did."""
    if not path:
        return []
    if not os.path.isfile(path) or os.path.getsize(path) == 0:
        log(f"{path} is missing or empty; no {label} rows")
        return []
    rows, torn = [], 0
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                rows.append(json.loads(line))
            except ValueError:
                torn += 1
    if torn:
        log(f"{path}: skipped {torn} unparseable line(s)")
    return rows


def post(url, token, body):
    """POST `body`; return the decoded response. Raises SystemExit on a 4xx or
    after the last retry."""
    data = json.dumps(body, separators=(",", ":")).encode()
    for attempt in range(1, ATTEMPTS + 1):
        req = urllib.request.Request(url, data=data, method="POST", headers={
            "Authorization": f"Bearer {token}",
            "Content-Type": "application/json",
            # Cloudflare's bot rules can challenge Python's default agent.
            "User-Agent": "turmeric-report-metrics/1",
        })
        try:
            with urllib.request.urlopen(req, timeout=120) as res:
                return json.loads(res.read())
        except urllib.error.HTTPError as e:
            detail = e.read().decode(errors="replace").strip()
            if 400 <= e.code < 500:
                sys.exit(f"report-metrics: rejected ({e.code}): {detail}")
            reason = f"HTTP {e.code}: {detail[:300]}"
        except (urllib.error.URLError, TimeoutError, ConnectionError) as e:
            reason = str(e)
        if attempt == ATTEMPTS:
            sys.exit(f"report-metrics: giving up after {ATTEMPTS} attempts ({reason})")
        delay = 2 ** attempt
        log(f"attempt {attempt}/{ATTEMPTS} failed ({reason}); retrying in {delay}s")
        time.sleep(delay)


def backfill(directory, url, token):
    """Every year file on the branch, in chunks, oldest first. The recent window
    (suite-timings-recent.jsonl) is a copy of the year file's tail, so it is
    not read."""
    files = {
        "timings": sorted(glob.glob(os.path.join(directory, "suite-timings-[0-9]*.jsonl"))),
        "loc": sorted(glob.glob(os.path.join(directory, "repo-loc-*.jsonl"))),
        "docs": sorted(glob.glob(os.path.join(directory, "docs-counts-*.jsonl"))),
        "bench": sorted(glob.glob(os.path.join(directory, "bench-*.jsonl"))),
    }
    if not any(files.values()):
        sys.exit(f"report-metrics: no metrics files in {directory}")
    totals = {}
    for kind, paths in files.items():
        rows = [r for p in paths for r in read_rows(p, kind)]
        totals[kind] = len(rows)
        for i in range(0, len(rows), BACKFILL_CHUNK):
            chunk = rows[i:i + BACKFILL_CHUNK]
            res = post(url, token, {kind: chunk})
            log(f"{kind}: rows {i + 1}-{i + len(chunk)} of {len(rows)} -> ingest {res['ingest']}")
    log("backfill done: " + ", ".join(f"{n} {k}" for k, n in totals.items()))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("timings", nargs="?", help="suite timing rows (JSONL)")
    ap.add_argument("--loc", help="repo line-count rows (JSONL)")
    ap.add_argument("--docs", help="doc-count rows (JSONL)")
    ap.add_argument("--bench", help="nightly benchmark rows (JSONL)")
    ap.add_argument("--backfill", metavar="DIR",
                    help="post every year file in a checkout of the ci-metrics branch")
    ap.add_argument("--url", default=os.environ.get("CI_METRICS_URL", DEFAULT_URL))
    args = ap.parse_args()

    token = os.environ.get("CI_METRICS_TOKEN", "")
    if not token:
        sys.exit("report-metrics: CI_METRICS_TOKEN is not set")

    if args.backfill:
        backfill(args.backfill, args.url, token)
        return
    if not args.timings:
        ap.error("a timings file (or --backfill) is required")

    body = {
        "timings": read_rows(args.timings, "suite timing"),
        "loc": read_rows(args.loc, "line-count"),
        "docs": read_rows(args.docs, "doc-count"),
        "bench": read_rows(args.bench, "benchmark"),
    }
    body = {k: v for k, v in body.items() if v}
    if not body:
        log("nothing to report")
        return
    res = post(args.url, token, body)
    log(f"ingest {res['ingest']}: {res['suite_rows']} suite, {res['loc_rows']} loc, "
        f"{res['docs_rows']} docs, {res['bench_rows']} bench row(s)")


if __name__ == "__main__":
    main()
