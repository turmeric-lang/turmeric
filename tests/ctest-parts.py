#!/usr/bin/env python3
"""tests/ctest-parts.py -- the ctest `part` split, and the check that it holds.

The `test` job in .github/workflows/ci.yml runs the registered ctest suites in
several parts, one job each, so that a RUN_SERIAL barrier in one part does not
hold up the rest.  The parts must between them run every registered test:
a test in NO part is a suite that silently stopped running, and CI goes green
without it.

That invariant used to be prose in ci.yml -- "Verify after adding a suite:
`ctest -N` totals must satisfy all == fixtures + r7rs + aux" -- which is to say
it was nobody's job.  `--no-tests=error` catches only a pattern that empties a
part completely, not one that loses a test out of the middle, and the patterns
got harder to eyeball when the two slow suites moved out of `aux`.  So the
table below is the ONE place the patterns live: ci.yml reads each pattern from
here (`pattern`), and `--check` asserts against `ctest -N` that the table still
covers the registered set.

This follows tests/run-shard-partition.sh, which guards the same shape for the
fixture corpus, including its central rule: membership answers come from the
harness itself -- here, `ctest -N` evaluated with the part's own pattern --
never from a second copy of the enumeration.  A copy would agree with itself
forever while drifting from what CI actually runs.

Usage
-----
    python3 tests/ctest-parts.py pattern aux --leg linux        # for ci.yml
    python3 tests/ctest-parts.py nightly-pattern --for macos    # for the nightly
    python3 tests/ctest-parts.py --check [build-dir]            # the assertion
    python3 tests/ctest-parts.py --list

Exit 0 if the table covers the registered set on every leg, 1 if not, and 0
with a SKIP line when there is no configured build tree to ask.
"""

import os
import re
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# -- the table ---------------------------------------------------------------
#
# Each part is (kind, pattern): "include" is ctest -R, "exclude" is ctest -E.
# `shards` is how many jobs run that part, each over a slice of the suite's own
# work (not of the ctest name, which is why the name legitimately appears in
# more than one job).
#
# The two legs differ on purpose, and the reason is the macOS runner pool, not
# macOS CPU: macOS jobs for this repo wait 40-80 minutes to START while every
# ubuntu job starts at t+0.  So on Linux the fix for a serial barrier is more
# jobs, and on macOS it is less work -- adding a macOS job spends the scarce
# resource.  See docs/archive/ci-aux-suite-latency-plan.md section 3.
PARTS = {
    "linux": {
        "fixtures": ("include", r"^tur_tests$"),
        "r7rs":     ("include", r"r7rs"),
        # `aux` is exclude-based, so a newly registered suite lands here by
        # default -- which is the property that makes a dropped suite hard.
        # Adding a name to this pattern WITHOUT adding a part that runs it is
        # the drift --check exists to catch.
        "aux":      ("exclude", r"^tur_tests$|r7rs|^tur_generic_spec_matrix$|^tur_emitted_float_conversions$|^tur_jit_fixture_tests$|^tur_repl_spice_jit$"),
        "fconv":    ("include", r"^tur_emitted_float_conversions$"),
        # 4066 live cells at ~1.5s of CPU each, RUN_SERIAL because it fans out
        # internally.  Two jobs, TUR_GSM_SHARD=i/2, which between them run the
        # whole matrix.
        "gsm":      ("include", r"^tur_generic_spec_matrix$", 2),
    },
    "macos": {
        "fixtures": ("include", r"^tur_tests$"),
        "r7rs":     ("include", r"r7rs"),
        # tur_generic_spec_matrix and turi_fixture_tests stay INSIDE aux here,
        # each running one quarter of its work (TUR_GSM_SHARD / TUR_TURI_SHARD),
        # so macOS keeps arm64 coverage of both without a fourth macOS job
        # queueing behind the first three.  The other three quarters are the
        # nightly's job.  tur_shard_partition is excluded outright -- see
        # LEG_INVARIANT, it cannot answer differently here.
        "aux":      ("exclude", r"^tur_tests$|r7rs|^tur_emitted_float_conversions$|^tur_shard_partition$|^tur_jit_fixture_tests$|^tur_repl_spice_jit$"),
    },
}

# Tests deliberately in no per-PR part on a leg.  Every entry needs a reason and
# a place the coverage actually comes from: this is the one way --check can pass
# with a registered test unrun, so an undocumented entry is the hole it is
# meant to find.
NIGHTLY_ONLY = {
    "macos": {
        "tur_emitted_float_conversions":
            "a pure clang-AST lint over emitted C, and clang runs on both legs "
            "-- the macOS copy is near-redundant per-PR.  Covered in full by "
            "the nightly arm64 leg (.github/workflows/nightly-arm64.yml).",
    },
    "linux": {},
}

# Suites a leg runs PARTIALLY: in a part, but over a slice of their own work.
# Unlike NIGHTLY_ONLY these still run per-PR, so completeness is satisfied -- but
# the nightly has to make up the rest, and nothing else in the tree records that.
# `shard` is the slice, and it must match what ci.yml actually sets; --check
# compares the two, so changing one without the other fails rather than quietly
# leaving three quarters of the matrix to nobody.
SAMPLED = {
    "macos": {
        "tur_generic_spec_matrix": (
            "1/4",
            "4066 cells that each compile and RUN a program, and the bug class "
            "is ABI-sensitive, so arm64 coverage is load-bearing -- but a "
            "fourth macOS job would queue 40+ min behind the first three.  A "
            "quarter per PR, all of it nightly.",
        ),
        "turi_fixture_tests": (
            "1/4",
            "the fixture corpus through the tree-walking interpreter: 283s on "
            "macOS and, with the matrix sharded, the last RUN_SERIAL barrier in "
            "the aux part.  It executes programs, so arm64 is worth sampling "
            "rather than dropping -- but the compiled path already runs the same "
            "corpus in full on both legs, so the marginal per-PR value of a "
            "second macOS engine pass over it is low.  A quarter per PR, all of "
            "it nightly.",
        ),
    },
    "linux": {},
}

# Suites a leg does not run AND the nightly does not either, because the answer
# cannot differ between legs -- not "cheaper elsewhere" but "the same by
# construction".  A row here is a stronger claim than NIGHTLY_ONLY and needs the
# mechanism, not a cost argument: if a platform could change the verdict, even in
# principle, the suite belongs in NIGHTLY_ONLY instead so something still runs it.
LEG_INVARIANT = {
    "macos": {
        "tur_shard_partition":
            "enumeration only -- its own header says it never invokes $TUR.  It "
            "asks run-jit.sh for the fixture NAMES a shard would run "
            "(TUR_TEST_LIST) and does set algebra on them; nothing is compiled "
            "or executed.  The enumeration applies only TUR_TEST_FILTER and the "
            "shard ordinal over a glob of tests/fixtures/, with no requires.* "
            "marker evaluation and no platform probing, so the disjoint/complete "
            "verdict is a function of the fixture tree alone and is identical on "
            "every platform.  Linux runs it; a nightly copy would re-derive the "
            "same answer.",
    },
    "linux": {},
}

# Suites a DIFFERENT per-PR job runs in full on both legs, so no `test` part
# runs them.  Since TUR_JIT defaulted ON (2026-10-02) the `test` job builds the
# engine too, which registers these two; left in `aux` they would re-run the
# whole fixture corpus through `tur jit` there, a second copy of what the `jit`
# job already runs on the same two OSes.  --check confirms each name is in the
# named job's own `ctest -R` pattern in ci.yml, so a row cannot outlive the
# coverage it claims.  They are only registered on a TUR_JIT build, so a row
# naming an unregistered test is not stale on a -DTUR_JIT=OFF tree.
OTHER_JOB = {
    "tur_jit_fixture_tests": (
        "jit",
        "the fixture corpus through the MIR engine: ~440s on Linux and up to "
        "~940s on a 3-core mac, RUN_SERIAL.  The `jit` job exists to run it.",
    ),
    "tur_repl_spice_jit": (
        "jit",
        "the in-process spice build behind `tur repl --engine jit`; runs "
        "beside tur_jit_fixture_tests in the `jit` job.",
    ),
}


def ci_job_ctest_pattern(job):
    """The `ctest ... -R '<pattern>'` the ci.yml job `job` runs, or None.

    Read from the job's own block: from its `  <job>:` header to the next
    top-level job header.  The first single-quoted -R argument wins.
    """
    try:
        with open(os.path.join(REPO, CI_WORKFLOW)) as f:
            text = f.read()
    except OSError:
        return None
    m = re.search(r"^  %s:\n(.*?)(?=^  [a-z][a-z0-9-]*:\n|\Z)" % re.escape(job),
                  text, re.M | re.S)
    if not m:
        return None
    r = re.search(r"-R\s+'([^']+)'", m.group(1))
    return r.group(1) if r else None


# Where --check reads the sample sizes from, to compare against SAMPLED, and the
# env var each sampled suite's slice is spelled with.  A suite sampled per-PR has
# to appear here, or nothing checks that the table and the workflow agree.
CI_WORKFLOW = os.path.join(".github", "workflows", "ci.yml")
SAMPLE_VARS = {
    "tur_generic_spec_matrix": "TUR_GSM_SHARD",
    "turi_fixture_tests": "TUR_TURI_SHARD",
}


def ci_sample_shard(var):
    """The i/N ci.yml sets for `var`, or None if that line is not found.

    Matches the first quoted i/N on the variable's own line.  Same-line and
    shape-agnostic on purpose: a tighter pattern encoding the surrounding GitHub
    expression would break on a reformat.  Finding nothing makes --check FAIL
    rather than pass, so a reformat that does defeat it is loud.
    """
    pat = re.compile(r"^\s*%s:[^\n]*?'(\d+/\d+)'" % re.escape(var), re.M)
    try:
        with open(os.path.join(REPO, CI_WORKFLOW)) as f:
            m = pat.search(f.read())
    except OSError:
        return None
    return m.group(1) if m else None


def part_pattern(leg, part):
    try:
        return PARTS[leg][part][1]
    except KeyError:
        sys.stderr.write("ctest-parts: no part %r on leg %r (have: %s)\n"
                         % (part, leg, ", ".join(sorted(PARTS.get(leg, {})))))
        raise SystemExit(2)


def part_shards(leg, part):
    entry = PARTS[leg][part]
    return entry[2] if len(entry) > 2 else 1


def nightly_pattern(leg):
    """A ctest -R pattern for everything `leg` does NOT cover in full per PR.

    Derived, not written down twice: it is exactly the suites this leg skips
    (NIGHTLY_ONLY) plus the ones it only samples (SAMPLED).  So moving a suite
    back onto the PR leg, or adding a new one to either table, moves the
    nightly's coverage with it and cannot leave a gap behind.

    LEG_INVARIANT is deliberately NOT included: those are not uncovered, they
    are answered identically by the other leg, so a nightly copy would re-derive
    the same verdict at the cost of running it.
    """
    names = sorted(set(NIGHTLY_ONLY.get(leg, {})) | set(SAMPLED.get(leg, {})))
    if not names:
        sys.stderr.write("ctest-parts: leg %r covers everything per PR; the "
                         "nightly has nothing to run\n" % leg)
        raise SystemExit(2)
    return "|".join("^%s$" % n for n in names)


def _ctest_n(build, *args):
    """`ctest -N` with `args`, as a set of test names.

    The EXIT STATUS is a signal, not the line count: an empty result from a bad
    --test-dir or a missing ctest looks exactly like a part that selects
    nothing, and the second is a finding while the first is a broken
    invocation.  `ctest -N -R <no match>` exits 0 (it prints "Total Tests: 0"),
    so a non-zero status here really is an error.  run-shard-partition.sh
    records the same lesson from a CI round spent on "(Failed)" and no reason.
    """
    out = subprocess.run(["ctest", "-N", "--test-dir", build] + list(args),
                         capture_output=True, text=True, cwd=REPO)
    if out.returncode != 0:
        print("FAIL check-ctest-partition -- `ctest -N %s` exited %d; this is a "
              "broken invocation, not a finding about the parts."
              % (" ".join(args), out.returncode))
        for line in (out.stderr or out.stdout or "").splitlines()[:4]:
            print("     %s" % line)
        raise SystemExit(1)
    return _names(out.stdout)


def registered(build):
    """Every test name ctest reports for this build tree."""
    return _ctest_n(build)


def selected(build, kind, pattern):
    """The test names one part's own pattern selects."""
    return _ctest_n(build, "-R" if kind == "include" else "-E", pattern)


# `Test   #1: name` ... `Test #177: name` -- ctest RIGHT-ALIGNS the number, so
# the run of spaces before `#` varies with the width of the largest test number.
# A `startswith("Test #")` reads only the widest ones: with 177 tests it found
# #100-#177 and silently dropped the first 99, which looks exactly like 99
# suites missing from every part.
TEST_LINE = re.compile(r"^\s*Test\s+#\d+:\s*(\S+)\s*$")


def _names(text):
    names = set()
    for line in text.splitlines():
        m = TEST_LINE.match(line)
        if m:
            names.add(m.group(1))
    return names


def check(build):
    if not os.path.exists(os.path.join(REPO, build, "CTestTestfile.cmake")):
        print("SKIP check-ctest-partition -- %s is not a configured build tree"
              % build)
        return 0

    all_tests = registered(build)
    if not all_tests:
        print("FAIL check-ctest-partition -- `ctest -N` listed no tests at all")
        return 1

    fail = 0
    for leg in sorted(PARTS):
        members = {}          # test name -> [part, ...]
        for part, entry in sorted(PARTS[leg].items()):
            kind, pattern = entry[0], entry[1]
            got = selected(build, kind, pattern)
            if not got:
                print("FAIL check-ctest-partition -- %s/%s selects NO test "
                      "(pattern %r); --no-tests=error would fail this job"
                      % (leg, part, pattern))
                fail = 1
            for name in got:
                members.setdefault(name, []).append(part)

        nightly = NIGHTLY_ONLY.get(leg, {})
        invariant = LEG_INVARIANT.get(leg, {})
        # The two ways a leg may legitimately not run a registered suite. Both
        # need a written reason; the difference is whether anything else has to
        # cover it (nightly: yes, invariant: the other leg already did).
        omitted = dict(nightly)
        omitted.update(invariant)
        omitted.update({n: why for n, (_job, why) in OTHER_JOB.items()})

        # Completeness -- the dangerous half.  A test in no part is a suite
        # that stopped running, and nothing else in CI would notice.
        for name in sorted(all_tests - set(members)):
            if name in omitted:
                continue
            print("FAIL check-ctest-partition -- %s: test '%s' is in NO part, "
                  "so this leg never runs it.\n"
                  "     Add it to a part's pattern, or record it in "
                  "NIGHTLY_ONLY['%s'] (the nightly covers it), "
                  "LEG_INVARIANT['%s'] (the other leg's answer is the same by "
                  "construction) or OTHER_JOB (another per-PR job runs it) "
                  "with the reason."
                  % (leg, name, leg, leg))
            fail = 1

        # A LEG_INVARIANT row must name a suite the OTHER leg actually runs --
        # otherwise "the other leg already answered this" is false and nothing
        # runs it anywhere, which is the one claim in this table that could
        # silently drop a suite from all of CI.
        for name in sorted(invariant):
            others = [o for o in PARTS if o != leg]
            covered = any(
                name in {m for p, e in PARTS[o].items()
                         for m in selected(build, e[0], e[1])}
                for o in others)
            if not covered:
                print("FAIL check-ctest-partition -- %s: LEG_INVARIANT names "
                      "'%s' as answered by another leg, but no other leg runs "
                      "it either, so nothing in CI does.  Move it to "
                      "NIGHTLY_ONLY, or put it back in a part."
                      % (leg, name))
                fail = 1

        # Disjointness -- the cheap half: duplicated work, not lost signal.
        # Allowed only where the table says the suite is sharded across parts.
        for name, parts in sorted(members.items()):
            if len(parts) == 1:
                continue
            shards = sum(part_shards(leg, p) for p in parts)
            print("FAIL check-ctest-partition -- %s: test '%s' is in %d parts "
                  "(%s) and runs %d time(s); only a part declaring shards may "
                  "repeat a test."
                  % (leg, name, len(parts), ", ".join(parts), shards))
            fail = 1

        # A stale row hides a test that IS covered, or names one that no longer
        # exists -- both make the table lie about coverage.
        for name in sorted(omitted):
            which = ("LEG_INVARIANT" if name in invariant
                     else "OTHER_JOB" if name in OTHER_JOB else "NIGHTLY_ONLY")
            if name not in all_tests and name in OTHER_JOB:
                continue    # a TUR_JIT=OFF tree; see OTHER_JOB
            if name not in all_tests:
                print("FAIL check-ctest-partition -- %s: %s names "
                      "'%s', which is not a registered test; delete the row."
                      % (leg, which, name))
                fail = 1
            elif name in members:
                print("FAIL check-ctest-partition -- %s: %s names "
                      "'%s', but part(s) %s already run it; delete the row."
                      % (leg, which, name, ", ".join(members[name])))
                fail = 1

        # An OTHER_JOB row must name a suite that job's own -R pattern selects,
        # or "another job runs it" is false and nothing does.
        for name, (job, _why) in sorted(OTHER_JOB.items()):
            pat = ci_job_ctest_pattern(job)
            if pat is None:
                print("FAIL check-ctest-partition -- %s: OTHER_JOB says the "
                      "'%s' job runs '%s', but no `ctest -R '...'` was found in "
                      "that job in %s" % (leg, job, name, CI_WORKFLOW))
                fail = 1
            elif not re.search(pat, name):
                print("FAIL check-ctest-partition -- %s: OTHER_JOB says the "
                      "'%s' job runs '%s', but that job's -R pattern %r does "
                      "not select it, so nothing in CI runs it."
                      % (leg, job, name, pat))
                fail = 1

        # A SAMPLED row must name a test that IS in a part (it runs, partially)
        # and must agree with the slice ci.yml sets.  The two drifting apart is
        # the quiet failure: the table would promise the nightly covers three
        # quarters while ci.yml ran a half, or a third.
        for name, (shard, _why) in sorted(SAMPLED.get(leg, {}).items()):
            if name not in all_tests:
                print("FAIL check-ctest-partition -- %s: SAMPLED names '%s', "
                      "which is not a registered test; delete the row."
                      % (leg, name))
                fail = 1
            elif name not in members:
                print("FAIL check-ctest-partition -- %s: SAMPLED names '%s', "
                      "but no part runs it -- a sampled suite still runs "
                      "per-PR.  Move the row to NIGHTLY_ONLY instead."
                      % (leg, name))
                fail = 1
            # Only the macOS leg is sampled in ci.yml today; a sampled suite on
            # another leg would need its own expression there before this could
            # compare anything, so say so rather than silently checking nothing.
            if leg != "macos":
                continue
            var = SAMPLE_VARS.get(name)
            if not var:
                print("FAIL check-ctest-partition -- %s: SAMPLED names '%s' but "
                      "SAMPLE_VARS has no env var for it, so nothing checks that "
                      "the table and %s agree.  Add the row."
                      % (leg, name, CI_WORKFLOW))
                fail = 1
                continue
            actual = ci_sample_shard(var)
            if actual is None:
                print("FAIL check-ctest-partition -- %s: could not find %s in "
                      "%s to compare against SAMPLED['%s']['%s'] = %s"
                      % (leg, var, CI_WORKFLOW, leg, name, shard))
                fail = 1
            elif actual != shard:
                print("FAIL check-ctest-partition -- %s: SAMPLED says '%s' runs "
                      "%s per PR but %s sets %s=%s.  The nightly's coverage is "
                      "computed from this table, so the two must agree."
                      % (leg, name, shard, CI_WORKFLOW, var, actual))
                fail = 1

        if not fail:
            n_jobs = sum(part_shards(leg, p) for p in PARTS[leg])
            notes = []
            if nightly:
                notes.append("%d nightly-only" % len(nightly))
            if SAMPLED.get(leg):
                notes.append("%d sampled" % len(SAMPLED[leg]))
            if invariant:
                notes.append("%d leg-invariant" % len(invariant))
            other = [n for n in OTHER_JOB if n in all_tests]
            if other:
                notes.append("%d run by another job" % len(other))
            print("  ok  %s -- %d registered test(s) across %d part(s) / %d "
                  "job(s)%s" % (leg, len(all_tests), len(PARTS[leg]), n_jobs,
                                " (%s)" % ", ".join(notes) if notes else ""))

    if fail:
        return 1
    print("PASS check-ctest-partition")
    return 0


def main(argv):
    if not argv or argv[0] in ("-h", "--help"):
        print(__doc__)
        return 0
    if argv[0] == "--list":
        for leg in sorted(PARTS):
            for part, entry in sorted(PARTS[leg].items()):
                n = part_shards(leg, part)
                print("%-6s %-9s %-7s %s%s"
                      % (leg, part, entry[0], entry[1],
                         "  (x%d shards)" % n if n > 1 else ""))
        return 0
    if argv[0] == "--check":
        return check(argv[1] if len(argv) > 1
                     else os.environ.get("TUR_BUILD_DIR", "build"))
    if argv[0] == "nightly-pattern":
        if "--for" not in argv:
            sys.stderr.write("ctest-parts: nightly-pattern needs --for <leg>\n")
            return 2
        print(nightly_pattern(argv[argv.index("--for") + 1]))
        return 0
    if argv[0] == "pattern":
        if len(argv) < 2:
            sys.stderr.write("ctest-parts: pattern needs a part name\n")
            return 2
        part = argv[1]
        leg = "linux"
        if "--leg" in argv:
            leg = argv[argv.index("--leg") + 1]
        print(part_pattern(leg, part))
        return 0
    sys.stderr.write("ctest-parts: unknown mode %r\n" % argv[0])
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
