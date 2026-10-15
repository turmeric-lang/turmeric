#!/usr/bin/env python3
"""
tools/ci/collect-playwright-timings.py -- one timings row per Playwright suite.

The /ci dashboard is fed by tools/ci/collect-suite-timings.py, which reads the
JUnit XML `ctest --output-junit` writes: one <testcase> per SUITE.  The Try
Turmeric browser job runs Playwright, whose JUnit is one <testcase> per TEST,
and it uploaded no timings at all -- so `web_desktop` / `web_mobile` had no
row, no trend, and no way to show "ran red, ignored" while the job stayed
green behind continue-on-error
(docs/archive/try-turmeric-browser-suites-green-while-failing.md).

This aggregates each Playwright JUnit file into ONE row shaped like the ctest
rows (same keys, plus the census fields the fixture suites now carry):

    {"suite": "web_desktop", "status": "fail", "duration_ms": 91234,
     "passed": 40, "failed": 3, "skipped": 2, "discovered": 45, ...}

`status` is honest: "fail" when any test failed (even though the job step was
continue-on-error), "skip" with a reason when the file is missing or holds no
tests (a suite that never started -- the WebKit-download case; the dashboard
only knows pass / skip / fail, and the skip ledger is where a suite that did
not run belongs), "pass" otherwise.  A suite with skipped tests carries
`partial_skip_reason` so the skip ledger sees it.

Usage:
    python3 tools/ci/collect-playwright-timings.py \
        --suite web_desktop web/results-desktop.xml \
        --suite web_mobile  web/results-mobile.xml  > timings.jsonl

`--web-perf FILE` also turns the JSON that web/tests/web-perf.spec.js writes
(performance-audit-plan WP5) into one row per metric -- the time to "Ready",
to a first result, an LSP keystroke, ... -- with `suite: "web_perf:<metric>"`,
the value in `duration_ms`, and `kind: "web-perf"`, which keeps them out of
the dashboard's suite views (a measurement is not a suite run).  A missing
file writes nothing: the perf step is non-blocking and may not have run.
"""

import argparse
import json
import os
import sys
import time
import xml.etree.ElementTree as ET


def warn(msg):
    print(f"collect-playwright-timings: {msg}", file=sys.stderr)


def aggregate(path):
    """(status, duration_ms, passed, failed, skipped, discovered) for one file."""
    if not os.path.isfile(path):
        return "skip", 0, 0, 0, 0, 0
    try:
        root = ET.parse(path).getroot()
    except (OSError, ET.ParseError) as e:
        warn(f"could not parse {path}: {e}")
        return "skip", 0, 0, 0, 0, 0
    passed = failed = skipped = 0
    seconds = 0.0
    for tc in root.iter("testcase"):
        try:
            seconds += float(tc.get("time") or 0.0)
        except ValueError:
            pass
        if tc.find("failure") is not None or tc.find("error") is not None:
            failed += 1
        elif tc.find("skipped") is not None:
            skipped += 1
        else:
            passed += 1
    discovered = passed + failed + skipped
    if discovered == 0:
        return "skip", 0, 0, 0, 0, 0
    status = "fail" if failed else "pass"
    return status, int(round(seconds * 1000)), passed, failed, skipped, discovered


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--suite", nargs=2, action="append", metavar=("NAME", "XML"),
                    default=[], help="suite row name and its Playwright JUnit file")
    ap.add_argument("--web-perf", metavar="JSON",
                    help="web-perf.spec.js's result file: one row per metric")
    args = ap.parse_args()
    if not args.suite and not args.web_perf:
        ap.error("give at least one --suite or --web-perf")

    row_base = {
        "sha": os.environ.get("GITHUB_SHA"),
        "branch": os.environ.get("GITHUB_REF_NAME"),
        "run_id": os.environ.get("GITHUB_RUN_ID"),
        "run_attempt": int(os.environ.get("GITHUB_RUN_ATTEMPT") or 0) or None,
        "ts": int(time.time()),
        # The browser job is one shape: a Debug native tur for the docs, the
        # wasm bundle, Playwright on ubuntu.  Recorded so the rows separate
        # cleanly from the ctest matrices at query time.
        "build_type": "Debug",
        "os": os.environ.get("RUNNER_OS") or os.uname().sysname,
        "cc": "emscripten",
        "nproc": os.cpu_count(),
        "jit": False,
        "sanitize": False,
    }
    for name, path in args.suite:
        status, dur, passed, failed, skipped, discovered = aggregate(path)
        row = dict(row_base)
        row.update({
            "suite": name,
            "status": status,
            "skip_reason": "suite did not run (no JUnit output)" if status == "skip" else None,
            "duration_ms": dur,
            "passed": passed,
            "failed": failed,
            "skipped": skipped,
            "discovered": discovered,
        })
        if skipped:
            row["partial_skip_reason"] = f"{skipped} of {discovered} tests skipped"
        print(json.dumps(row, sort_keys=True))
    if args.web_perf:
        for row in web_perf_rows(args.web_perf, row_base):
            print(json.dumps(row, sort_keys=True))
    return 0


def web_perf_rows(path, row_base):
    """One row per metric in web-perf.spec.js's JSON; none if it is absent."""
    if not os.path.isfile(path):
        warn(f"no web-perf result at {path}; writing no web_perf rows")
        return []
    try:
        doc = json.load(open(path))
    except (OSError, ValueError) as e:
        warn(f"could not read {path}: {e}")
        return []
    rows = []
    for name, value in sorted((doc.get("metrics") or {}).items()):
        if not isinstance(value, (int, float)):
            continue
        row = dict(row_base)
        row.update({
            "suite": f"web_perf:{name}",
            "kind": "web-perf",
            "status": "pass",
            "skip_reason": None,
            "duration_ms": round(float(value), 1),
            "samples": doc.get("cold_loads"),
        })
        rows.append(row)
    return rows


if __name__ == "__main__":
    sys.exit(main())
