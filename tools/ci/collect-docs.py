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
        print(f"{'guides':>12}  {turmeric_guides:>4}", file=sys.stderr)
        print(f"{'spice guides':>12}  {spice_guides!s:>4}", file=sys.stderr)
        print(f"{'spices':>12}  {len(names) if names is not None else None!s:>4}",
              file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
