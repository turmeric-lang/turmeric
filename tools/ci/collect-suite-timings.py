#!/usr/bin/env python3
"""Collect per-suite CTest timings into newline-delimited JSON.

Phase 2 of docs/archive/suite-timing-trends-plan.md.  Reads one or more JUnit
XML files written by `ctest --output-junit` and emits one JSON object per
registered suite on stdout.

Why N input files: CI runs ctest more than once per job (`tur_tests` is
RUN_SERIAL and gets its own invocation; the auxiliary suites run under -j), and
a single --output-junit path would let the second invocation overwrite the
first.  Each invocation writes its own XML and they are merged here.

Skip detection reads the `<system-out>` that ctest embeds per testcase -- which
it does even under --output-on-failure, so no -V/tee/LastTest.log plumbing is
needed.  Harnesses print `TUR_SKIP: <reason>` for a whole-suite skip and
`TUR_SKIP_PARTIAL: <reason>` when only part of the suite was skipped (the
latter stays a `pass` with a note, because the suite still did real work and
still has a meaningful duration).

Sharding: a job that ran only part of a corpus (TUR_TEST_SHARD="i/N", honored
by tests/run.sh and tests/run-jit.sh) tags its rows with shard_index and
shard_total.  Without that tag the shards of one run are N rows carrying the
same (run_id, suite) and ~1/N of the real duration each, which reads as one
suite that suddenly got faster and started reporting N times per run.  The tag
is what keeps them N distinct series instead.  Both keys are null on an
unsharded run, so every row written before sharding existed stays comparable.

$TUR_TEST_SHARD describes the JOB, not the suite: a suite that does not itself
honor it still gets the tag, and its rows are then replicas across shards.  That
is the honest reading -- the collector cannot know which harness sharded -- and
it is why a consumer should group by (suite, shard_index), never sum durations
across shards without checking.

`--suite-shard SUITE=i/N` is for the case that reading cannot express: one suite
in the job sliced its own work while the rest of the job ran whole.  The macOS
`aux` part is exactly that -- it runs a quarter of tur_generic_spec_matrix
(TUR_GSM_SHARD) alongside 160 suites that run in full -- so $TUR_TEST_SHARD
there would label all 160 as replicas of a slice they never ran, while leaving
it untagged makes the matrix's series drop ~4x with nothing in the data saying
why.  The flag tags that one row and leaves the others alone.

Usage:
    collect-suite-timings.py results-main.xml results-aux.xml > timings.jsonl
    collect-suite-timings.py --build-dir build results-*.xml
    TUR_TEST_SHARD=2/3 collect-suite-timings.py results-jit.xml
    collect-suite-timings.py --suite-shard tur_generic_spec_matrix=1/4 \
        results-aux.xml
"""

import argparse
import glob
import json
import os
import re
import sys
import time
import xml.etree.ElementTree as ET


def parse_shard(spec):
    """Parse an "i/N" shard spec into (index, total), 1-based.

    Returns (None, None) when the run was not sharded.

    Clamped exactly as tests/run.sh and tests/run-jit.sh clamp it -- a
    nonsense index snaps into range rather than erroring, and a total below 1
    means "not sharded".  Diverging here would let a row claim a slice the
    harness did not run: with TUR_TEST_SHARD=0/3 the harness runs shard 1/3,
    so this must say 1/3 too.
    """
    if not spec or "/" not in spec:
        return (None, None)
    left, _, right = spec.partition("/")
    total = int(right) if right.isdigit() else 1
    index = int(left) if left.isdigit() else 1
    if total < 1:
        total = 1
    if index < 1:
        index = 1
    if index > total:
        index = total
    if total <= 1:
        return (None, None)
    return (index, total)


SKIP_RE = re.compile(r"^TUR_SKIP:[ \t]*(.*)$", re.MULTILINE)
SKIP_PARTIAL_RE = re.compile(r"^TUR_SKIP_PARTIAL:[ \t]*(.*)$", re.MULTILINE)
# The fixture-suite census line, e.g.
#   summary: 2767 passed, 0 failed                          (tests/run.sh)
#   turi fixture summary: 1898 passed, 0 failed, 743 skipped of 2741 discovered
# Turning the counts into row fields is what lets a silent drop in what a
# suite runs show up as a trend instead of a number nobody reads
# (turi-suite-accounting-and-reporting-gaps, item 6).
SUMMARY_RE = re.compile(
    r"^(?:[\w -]*?)summary:[ \t]*(\d+) passed, (\d+) failed"
    r"(?:, (\d+) skipped)?(?: of (\d+) discovered)?",
    re.MULTILINE)


def warn(msg):
    print(f"collect-suite-timings: {msg}", file=sys.stderr)


def read_cmake_cache(build_dir):
    """Pull the build-shape dimensions out of CMakeCache.txt."""
    out = {}
    path = os.path.join(build_dir, "CMakeCache.txt")
    try:
        with open(path, "r", errors="replace") as fh:
            for line in fh:
                line = line.strip()
                if not line or line.startswith(("#", "//")):
                    continue
                key, sep, value = line.partition("=")
                if not sep:
                    continue
                # KEY:TYPE=VALUE
                out[key.split(":", 1)[0]] = value
    except OSError as e:
        warn(f"could not read {path}: {e}")
    return out


def read_compiler_id(build_dir):
    """Compose a real compiler dimension, e.g. 'AppleClang-21.0.0'.

    CMakeCache.txt only has CMAKE_C_COMPILER as a *path* (often /usr/bin/cc),
    which is useless as a trend key -- two runs with different compilers can
    both say 'cc'.  The id and version live in CMakeCCompiler.cmake instead.
    The CMake version in that path varies by runner image, so glob it.
    """
    pattern = os.path.join(build_dir, "CMakeFiles", "*", "CMakeCCompiler.cmake")
    matches = sorted(glob.glob(pattern))
    if not matches:
        warn(f"no CMakeCCompiler.cmake under {pattern}; cc dimension unknown")
        return None
    text = ""
    try:
        with open(matches[-1], "r", errors="replace") as fh:
            text = fh.read()
    except OSError as e:
        warn(f"could not read {matches[-1]}: {e}")
        return None

    def grab(var):
        m = re.search(rf'set\({var}\s+"([^"]*)"\)', text)
        return m.group(1) if m else ""

    cid = grab("CMAKE_C_COMPILER_ID")
    ver = grab("CMAKE_C_COMPILER_VERSION")
    if not cid:
        return None
    # Truncate to major.minor.patch -- AppleClang reports a 4-part build
    # number whose last component churns per Xcode point release and would
    # fragment the series for no analytical gain.
    short = ".".join(ver.split(".")[:3]) if ver else ""
    return f"{cid}-{short}" if short else cid


def cmake_bool(value):
    """CMake truthiness, as documented for if() constants."""
    if value is None:
        return False
    return str(value).strip().upper() in ("1", "ON", "YES", "TRUE", "Y")


def classify(testcase):
    """Map a ctest <testcase> to pass | fail | notrun.

    ctest emits status="run" for a pass, status="fail" with a <failure> child
    for a failure, and status="disabled"/"notrun" for entries it did not
    execute.  Checking for the <failure> child as well as the attribute means a
    schema change in either direction still reports a failure as a failure.
    """
    status = (testcase.get("status") or "").lower()
    if testcase.find("failure") is not None or status == "fail":
        return "fail"
    if status in ("disabled", "notrun"):
        return "notrun"
    return "pass"


def parse_summary(sysout):
    """The suite's own pass/fail/skip/discovered counts, or None."""
    m = SUMMARY_RE.search(sysout or "")
    if not m:
        return None
    passed, failed, skipped, discovered = m.groups()
    return {
        "passed": int(passed),
        "failed": int(failed),
        "skipped": int(skipped) if skipped is not None else None,
        "discovered": int(discovered) if discovered is not None else None,
    }


def parse_junit(path):
    """Yield (suite, status, duration_ms, skip_reason, partial_reason, counts)."""
    try:
        tree = ET.parse(path)
    except (OSError, ET.ParseError) as e:
        warn(f"could not parse {path}: {e}")
        return
    for tc in tree.getroot().iter("testcase"):
        name = tc.get("name")
        if not name:
            continue
        status = classify(tc)

        # JUnit `time` is seconds as a float.
        try:
            duration_ms = int(round(float(tc.get("time") or 0.0) * 1000))
        except ValueError:
            duration_ms = 0

        sysout = tc.findtext("system-out") or ""
        skip_reason = None
        partial_reason = None
        if status == "pass":
            m = SKIP_RE.search(sysout)
            if m:
                status = "skip"
                skip_reason = m.group(1).strip() or None
            p = SKIP_PARTIAL_RE.search(sysout)
            if p:
                partial_reason = p.group(1).strip() or None

        yield name, status, duration_ms, skip_reason, partial_reason, parse_summary(sysout)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("xml", nargs="+", help="JUnit XML file(s) from ctest --output-junit")
    ap.add_argument("--build-dir", default="build",
                    help="CMake build dir to read config dimensions from (default: build)")
    ap.add_argument("--shard", default=None,
                    help='shard this job ran, as "i/N" (default: $TUR_TEST_SHARD)')
    ap.add_argument("--suite-shard", action="append", default=[],
                    metavar="SUITE=i/N",
                    help="one suite ran a slice while the rest of the job ran "
                         "whole; tag only that suite's row (repeatable)")
    args = ap.parse_args()

    cache = read_cmake_cache(args.build_dir)
    shard_index, shard_total = parse_shard(
        args.shard if args.shard is not None else os.environ.get("TUR_TEST_SHARD"))
    row_base = {
        "sha": os.environ.get("GITHUB_SHA"),
        "branch": os.environ.get("GITHUB_REF_NAME"),
        "run_id": os.environ.get("GITHUB_RUN_ID"),
        "run_attempt": int(os.environ.get("GITHUB_RUN_ATTEMPT") or 0) or None,
        "ts": int(time.time()),
        "build_type": cache.get("CMAKE_BUILD_TYPE") or None,
        "os": os.environ.get("RUNNER_OS") or os.uname().sysname,
        "cc": read_compiler_id(args.build_dir),
        "nproc": os.cpu_count(),
        "jit": cmake_bool(cache.get("TUR_JIT")),
        "sanitize": cmake_bool(cache.get("TUR_DEBUG_SANITIZE")),
        "shard_index": shard_index,
        "shard_total": shard_total,
    }

    # --suite-shard is for the case the job-level tag cannot express: ONE suite
    # in the job ran a slice of its own work while the other 160 ran whole.
    # $TUR_TEST_SHARD is read from the environment and so applies to every row,
    # which is right when the job IS a shard and wrong here -- it would label
    # all 160 other suites as replicas of a slice they never ran.  Without a
    # per-suite tag the alternative is worse: the row stays untagged while
    # measuring a quarter of the work, and the series drops ~4x with nothing in
    # the data to explain it.
    suite_shards = {}
    for spec in args.suite_shard:
        suite, _, which = spec.partition("=")
        si, st = parse_shard(which)
        if not suite or not st:
            warn(f"ignoring --suite-shard {spec!r}: want SUITE=i/N with N>1")
            continue
        suite_shards[suite] = (si, st)

    seen = {}
    order = []
    for path in args.xml:
        for name, status, dur, reason, partial, census in parse_junit(path):
            if name in seen:
                # A -R/-E pattern drift could run one suite in both
                # invocations.  Two rows for one suite in one run would
                # corrupt any aggregate, so keep the first and say so.
                warn(f"duplicate suite {name!r} in {path}; keeping first occurrence")
                continue
            seen[name] = True
            order.append((name, status, dur, reason, partial, census))

    counts = {}
    for name, status, dur, reason, partial, census in order:
        counts[status] = counts.get(status, 0) + 1
        row = dict(row_base)
        row.update({
            "suite": name,
            "status": status,
            "skip_reason": reason,
            "duration_ms": dur,
        })
        if census:
            # Additive: only the fixture suites print a census line; every
            # other row simply lacks these keys.
            row.update(census)
        if partial:
            row["partial_skip_reason"] = partial
        if name in suite_shards:
            row["shard_index"], row["shard_total"] = suite_shards[name]
        print(json.dumps(row, sort_keys=True))

    if not order:
        warn("no testcases found in any input file")
        return 1
    # A --suite-shard naming a suite this job did not run means the flag and the
    # ctest patterns have drifted, and the row it was meant to label is now
    # untagged somewhere else -- say so rather than tagging nothing in silence.
    for suite in suite_shards:
        if suite not in seen:
            warn(f"--suite-shard named {suite!r}, which this job did not run")
    summary = ", ".join(f"{k}={v}" for k, v in sorted(counts.items()))
    where = f" for shard {shard_index}/{shard_total}" if shard_total else ""
    warn(f"emitted {len(order)} suite rows{where} ({summary})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
