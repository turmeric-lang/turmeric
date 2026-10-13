#!/usr/bin/env python3
"""Count open plans and reports, split by status, as one JSON row.

Published to the `ci-metrics` orphan branch alongside the suite timings and
line counts (see tools/ci/publish-timings.sh) and charted by /ci.  One row per
push to `main`, NOT one per matrix leg: a doc count is a property of the
commit, so like collect-loc.py it has no (os, cc, nproc) dimension.

The project tracks open work in two directories under `docs/`:

  docs/upcoming/   -- plans (work intended but not yet done).  Top-level files
                      are active; `hold/` parks plans waiting on a dependency;
                      `v1/` holds milestone-scoped plans; `spices/` (with its
                      own `v1/`) holds plans moved in from turmeric-spices so
                      all open work lives in one tree.
  docs/reported/   -- open bug reports / findings.  Resolved reports are moved
                      to docs/archive/ (see CLAUDE.md), so everything left here
                      is open by construction.

A `.keep` sentinel keeps an empty directory in git; it is excluded from the
count.  A `README.md` in either directory is an index, not a finding or a
plan, so it is excluded too.  Only `.md` files are counted: a stray image or
marker is not a plan.

It also counts guides and spices, which live in two repos: Turmeric guides in
this one (docs/guides/), spices and spice guides in the sibling turmeric-spices
checkout (spices/*/build.tur; docs/guides/ and spices/*/docs/guides/).  When the
spices checkout is absent those fields are null rather than 0, so /ci can say
"not measured" instead of charting a collapse to zero.

And it counts report FLOW, not just stock: `reports_filed` and
`reports_archived` are running totals of the reports that have ever entered and
left docs/reported/ on main's first-parent line.  /ci differences them between
two rows to chart reports archived minus reports filed.  Walking first parents
with -m diffs every merge against the main it landed on, so the totals are a
sum of tree diffs and `reports_filed - reports_archived == open_reports` holds
exactly.  A plain path-limited `git log` breaks that: its history
simplification drops side branches, and on this repo it came out 14 short.  A report filed and closed
on a branch before merging never touched main, and is counted by neither.  A
rename inside docs/reported/ is neither filed nor archived.  Both fields are
null in a shallow clone, whose history would undercount them.

Usage:
    collect-docs.py                      # one JSON object on stdout
    collect-docs.py --repo /path/to/repo
    collect-docs.py --spices-repo /path/to/turmeric-spices
    collect-docs.py --explain            # per-bucket breakdown to stderr
"""

import argparse
import json
import os
import subprocess
import sys
import time


def git_sha(repo):
    env_sha = os.environ.get("GITHUB_SHA")
    if env_sha:
        return env_sha
    try:
        return subprocess.run(
            ["git", "-C", repo, "rev-parse", "HEAD"],
            check=True, stdout=subprocess.PIPE,
        ).stdout.decode().strip()
    except subprocess.CalledProcessError:
        return None


def count_md(repo, subdir):
    """Count .md files directly under `repo/subdir`, excluding subdirectories.

    A plan in a subdirectory (hold/, v1/) belongs to that sub-bucket, not to
    the top-level count, so the top-level number is the active set.
    """
    base = os.path.join(repo, subdir)
    if not os.path.isdir(base):
        return 0
    n = 0
    for entry in os.listdir(base):
        if entry.startswith("."):
            continue
        if entry == "README.md":
            continue
        path = os.path.join(base, entry)
        if os.path.isfile(path) and entry.endswith(".md"):
            n += 1
    return n


def count_md_tree(repo, subdir):
    """Count .md files anywhere under `repo/subdir`, recursively.

    Used for the total, which is the sum the page headlines.
    """
    base = os.path.join(repo, subdir)
    if not os.path.isdir(base):
        return 0
    n = 0
    for root, _dirs, files in os.walk(base):
        for f in files:
            if f.startswith("."):
                continue
            if f == "README.md":
                continue
            if f.endswith(".md"):
                n += 1
    return n


def is_report(path):
    """A top-level docs/reported/*.md finding, as count_md counts them."""
    prefix = "docs/reported/"
    if not path.startswith(prefix):
        return False
    name = path[len(prefix):]
    return ("/" not in name and name.endswith(".md")
            and name != "README.md" and not name.startswith("."))


def report_flow(repo):
    """(filed, archived): reports that ever entered / left docs/reported/.

    Running totals along HEAD's first-parent line.  Returns (None, None) when
    they cannot be counted honestly: a shallow clone (its history is cut off,
    so both totals would come out short) or no git at all.
    """
    def git(*args):
        return subprocess.run(["git", "-C", repo, *args], check=True,
                              stdout=subprocess.PIPE,
                              stderr=subprocess.DEVNULL).stdout.decode()
    try:
        if git("rev-parse", "--is-shallow-repository").strip() != "false":
            return (None, None)
        log = git("log", "--first-parent", "-m", "-M", "--name-status",
                  "--format=", "--diff-filter=ADR", "--", "docs/reported/")
    except (OSError, subprocess.CalledProcessError):
        return (None, None)
    filed = archived = 0
    for line in log.splitlines():
        parts = line.split("\t")
        status = parts[0][:1]
        if status == "A" and len(parts) == 2:
            filed += is_report(parts[1])
        elif status == "D" and len(parts) == 2:
            archived += is_report(parts[1])
        elif status == "R" and len(parts) == 3:
            # Inside the pathspec both ends are usually reports, and a rename is
            # then neither event.  A move out to docs/archive/ shows up as a D,
            # not an R, because the pathspec hides its destination.
            archived += is_report(parts[1]) and not is_report(parts[2])
            filed += is_report(parts[2]) and not is_report(parts[1])
    return (filed, archived)


def spice_names(spices_repo):
    """Sorted names of the spices: each directory under spices/ with a manifest."""
    base = os.path.join(spices_repo, "spices")
    if not os.path.isdir(base):
        return None
    return sorted(
        d for d in os.listdir(base)
        if os.path.isfile(os.path.join(base, d, "build.tur"))
        or os.path.isfile(os.path.join(base, d, "build.tur.sweet"))
    )


def count_spice_guides(spices_repo):
    """Guides in the spices repo: top-level docs/guides plus each spice's own."""
    n = count_md_tree(spices_repo, "docs/guides")
    base = os.path.join(spices_repo, "spices")
    if os.path.isdir(base):
        for d in os.listdir(base):
            n += count_md_tree(base, os.path.join(d, "docs/guides"))
    return n


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--repo", default=".", help="repository root (default: cwd)")
    ap.add_argument("--spices-repo", default=None,
                    help="turmeric-spices checkout (default: ../turmeric-spices "
                         "relative to --repo, if present)")
    ap.add_argument("--explain", action="store_true",
                    help="write a per-bucket breakdown to stderr")
    args = ap.parse_args()

    active_plans = count_md(args.repo, "docs/upcoming")
    held_plans = count_md(args.repo, "docs/upcoming/hold")
    v1_plans = count_md(args.repo, "docs/upcoming/v1")
    spices_plans = count_md_tree(args.repo, "docs/upcoming/spices")
    open_reports = count_md(args.repo, "docs/reported")
    open_plans = count_md_tree(args.repo, "docs/upcoming")
    reports_filed, reports_archived = report_flow(args.repo)

    spices_repo = args.spices_repo or os.path.join(
        os.path.abspath(args.repo), "..", "turmeric-spices")
    names = spice_names(spices_repo) if os.path.isdir(spices_repo) else None
    turmeric_guides = count_md_tree(args.repo, "docs/guides")
    spice_guides = count_spice_guides(spices_repo) if names is not None else None

    row = {
        "sha": git_sha(args.repo),
        "branch": os.environ.get("GITHUB_REF_NAME"),
        "run_id": os.environ.get("GITHUB_RUN_ID"),
        "run_attempt": int(os.environ.get("GITHUB_RUN_ATTEMPT") or 0) or None,
        "ts": int(time.time()),
        # The two series /ci charts by default.
        "open_plans": open_plans,
        "open_reports": open_reports,
        # Breakdown so the chart can toggle them in.  The total is the sum:
        # open_plans == active_plans + held_plans + v1_plans + spices_plans.
        "active_plans": active_plans,
        "held_plans": held_plans,
        "v1_plans": v1_plans,
        "spices_plans": spices_plans,
        # Report flow: running totals, differenced by /ci.  Null in a shallow
        # clone.  reports_filed - reports_archived == open_reports.
        "reports_filed": reports_filed,
        "reports_archived": reports_archived,
        # Guides and spices.  The spice fields are null when the spices repo
        # was not checked out, which is "not measured", not "zero".
        "turmeric_guides": turmeric_guides,
        "spice_guides": spice_guides,
        "spices": len(names) if names is not None else None,
        "spice_names": names,
    }

    print(json.dumps(row, sort_keys=True))

    if args.explain:
        print(f"{'open_reports':>12}  {open_reports:>4}", file=sys.stderr)
        print(f"{'open_plans':>12}  {open_plans:>4}", file=sys.stderr)
        print(f"{'  active':>12}  {active_plans:>4}", file=sys.stderr)
        print(f"{'  held':>12}  {held_plans:>4}", file=sys.stderr)
        print(f"{'  v1':>12}  {v1_plans:>4}", file=sys.stderr)
        print(f"{'  spices':>12}  {spices_plans:>4}", file=sys.stderr)
        print(f"{'filed':>12}  {reports_filed!s:>4}", file=sys.stderr)
        print(f"{'archived':>12}  {reports_archived!s:>4}", file=sys.stderr)
        print(f"{'guides':>12}  {turmeric_guides:>4}", file=sys.stderr)
        print(f"{'spice guides':>12}  {spice_guides!s:>4}", file=sys.stderr)
        print(f"{'spices':>12}  {len(names) if names is not None else None!s:>4}",
              file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
