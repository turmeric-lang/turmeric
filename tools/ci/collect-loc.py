#!/usr/bin/env python3
"""Count tracked lines of code, split product / test / generated, as one JSON row.

Published to the `ci-metrics` orphan branch alongside the suite timings (see
tools/ci/publish-timings.sh) and charted by /ci.  One row per push to `main`,
NOT one per matrix leg: a line count is a property of the commit, so unlike a
suite duration it has no (os, cc, nproc) dimension and must not be published
once per runner.

WHY NOT `wc -l {src,stdlib}/**/*.{c,h,tur}`:

  * It counts files git does not track -- a build tree under src/, an editor
    backup, a stale .c somebody forgot to delete -- so the number moves without
    a commit moving it.
  * It counts GENERATED files as if a person wrote them.  In this repo that is
    not a rounding error: tests/fixtures/*/expected.c is ~1.54M lines, nine
    times every hand-written line in the tree put together, and it jumps by
    hundreds of thousands of lines whenever the emitter changes a preamble.
    Folded into "test code" it would be the only thing the chart ever showed.
  * A brace glob is shell-specific (bash needs globstar; sh has no `**` at all)
    and silently expands to the literal pattern when it matches nothing, so a
    renamed directory reads as zero rather than as an error.

So: enumerate `git ls-files`, keep what a known code extension claims, drop the
generated paths explicitly, and classify the rest by path.  Every file that is
counted lands in exactly one bucket, and the row carries the counts the buckets
were derived from so a reader can check the arithmetic rather than trust it.

Lines are physical lines, blanks and comments included, so the number stays
comparable with the one anybody measures by hand.  Stripping blanks or comments
would need a comment grammar per language and a second argument about which of
them counts.  The one deliberate divergence from `wc -l` is a file whose last
line has no trailing newline: that is a line, and it is counted.

Usage:
    collect-loc.py                      # one JSON object on stdout
    collect-loc.py --repo /path/to/repo
    collect-loc.py --explain            # per-bucket, per-language breakdown to stderr
"""

import argparse
import json
import os
import subprocess
import sys
import time

# Extension -> language label.  An extension that is not here is not code and is
# not counted: Markdown, .stdout/.diag fixture expectations, the requires.*
# markers, images, lockfiles, SMT-LIB corpora.  Adding a language means adding a
# row here, which is the point -- the set of things that count is a list, not a
# side effect of a glob.
LANGUAGES = {
    ".c": "c", ".h": "c", ".cc": "c++", ".cpp": "c++", ".hpp": "c++",
    ".tur": "turmeric", ".sweet": "turmeric",
    ".py": "python",
    ".sh": "shell", ".bash": "shell",
    ".js": "javascript", ".mjs": "javascript", ".ts": "typescript",
    ".css": "css",
    ".el": "elisp", ".vim": "vim", ".scm": "scheme", ".clj": "clojure",
    ".rkt": "racket", ".rs": "rust", ".go": "go", ".java": "java",
    ".cmake": "cmake",
}

# Extensionless files that are nonetheless code, matched on basename.
BASENAME_LANGUAGES = {
    "CMakeLists.txt": "cmake",
    "Justfile": "just",
    "Dockerfile": "docker",
}

# GENERATED -- machine output that happens to be COMMITTED.  Counted into its
# own bucket so it is visible and auditable, and excluded from both of the
# series anybody reads.  Matched as a path prefix, or as a basename when the
# entry starts with "*/".
#
# Build output that is gitignored (docs/api/, web/public/turmeric.js, every
# build dir) needs no rule: `git ls-files` never reports it in the first place.
# These are the files a person has to regenerate and commit.
GENERATED = (
    "*/expected.c",                  # codegen snapshots, ~1.55M lines
    "tests/benchmarks/output/",      # emitted C kept for inspection
    "src/runtime/generated/",        # tur_rt_split*, tools/gen-runtime-split.py
    "stdlib/docstrings.tur",         # tools/gendocs.py --emit-tur
)

# BENCH -- measurement programs.  Not product code (nothing ships them) and not
# test code (nothing asserts on them), so they get their own bucket rather than
# quietly inflating whichever neighbour they are nearest.  Ahead of TEST so the
# handful under tests/benchmarks/ is counted for what it is.
BENCH = (
    "benchmarks/",
    "tests/benchmarks/",
)

# TEST -- code whose job is to check the rest.  Prefixes, or "*/<suffix>".
TEST = (
    "tests/",
    "web/tests/",
    "validation/",
    "*/conftest.py",
    "*.spec.js",
)

# EXAMPLE -- sample programs that exist to be read.  Same reasoning as BENCH.
EXAMPLE = (
    "examples/",
    "tutorials/",
)


def matches(path, patterns):
    """Prefix match, or basename/suffix match for a `*/x` or `*.x` pattern."""
    for pat in patterns:
        if pat.startswith("*"):
            if path.endswith(pat[1:]):
                return True
        elif path == pat or path.startswith(pat):
            return True
    return False


def language_of(path):
    base = os.path.basename(path)
    if base in BASENAME_LANGUAGES:
        return BASENAME_LANGUAGES[base]
    _, ext = os.path.splitext(base)
    return LANGUAGES.get(ext.lower())


def bucket_of(path):
    """Exactly one bucket per counted file.  Order is the precedence."""
    if matches(path, GENERATED):
        return "generated"
    if matches(path, BENCH):
        return "bench"
    if matches(path, TEST):
        return "test"
    if matches(path, EXAMPLE):
        return "example"
    return "code"


def tracked_files(repo):
    out = subprocess.run(
        ["git", "-C", repo, "ls-files", "-z"],
        check=True, stdout=subprocess.PIPE,
    ).stdout
    return [p for p in out.decode("utf-8", "replace").split("\0") if p]


def count_lines(path):
    """Physical lines, as `wc -l` counts them: one per newline.

    A final line with no trailing newline is still a line, so it is added back.
    Read as bytes -- a source file in a stray encoding must not abort the run.
    """
    try:
        with open(path, "rb") as fh:
            data = fh.read()
    except OSError:
        return 0
    if not data:
        return 0
    n = data.count(b"\n")
    return n if data.endswith(b"\n") else n + 1


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


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--repo", default=".", help="repository root (default: cwd)")
    ap.add_argument("--explain", action="store_true",
                    help="write a per-bucket, per-language breakdown to stderr")
    args = ap.parse_args()

    buckets = {}
    langs = {}
    skipped = 0

    for rel in tracked_files(args.repo):
        lang = language_of(rel)
        if lang is None:
            skipped += 1
            continue
        n = count_lines(os.path.join(args.repo, rel))
        b = bucket_of(rel)
        slot = buckets.setdefault(b, {"lines": 0, "files": 0})
        slot["lines"] += n
        slot["files"] += 1
        langs.setdefault(b, {}).setdefault(lang, 0)
        langs[b][lang] += n

    def lines(b):
        return buckets.get(b, {}).get("lines", 0)

    def files(b):
        return buckets.get(b, {}).get("files", 0)

    row = {
        "sha": git_sha(args.repo),
        "branch": os.environ.get("GITHUB_REF_NAME"),
        "run_id": os.environ.get("GITHUB_RUN_ID"),
        "run_attempt": int(os.environ.get("GITHUB_RUN_ATTEMPT") or 0) or None,
        "ts": int(time.time()),
        # The two series /ci charts by default.
        "code_lines": lines("code"),
        "test_lines": lines("test"),
        # Accounted for, not folded in.  Charting them is opt-in on the page.
        "bench_lines": lines("bench"),
        "example_lines": lines("example"),
        "generated_lines": lines("generated"),
        "code_files": files("code"),
        "test_files": files("test"),
        "bench_files": files("bench"),
        "example_files": files("example"),
        "generated_files": files("generated"),
        # Tracked files no language claimed -- docs, fixture expectations,
        # markers, assets.  Present so "why is the file count low" has an
        # answer in the data.
        "uncounted_files": skipped,
        # Per-language, product and test only: the long tail of the other
        # buckets is noise and the row is appended on every push.
        "code_by_lang": dict(sorted(langs.get("code", {}).items())),
        "test_by_lang": dict(sorted(langs.get("test", {}).items())),
    }

    print(json.dumps(row, sort_keys=True))

    if args.explain:
        for b in ("code", "test", "bench", "example", "generated"):
            print(f"{b:>10}  {lines(b):>9,} lines  {files(b):>5} files",
                  file=sys.stderr)
            for lang, n in sorted(langs.get(b, {}).items(),
                                  key=lambda kv: -kv[1]):
                print(f"{'':>10}  {n:>9,}            {lang}", file=sys.stderr)
        print(f"{'uncounted':>10}  {'':>9}         {skipped:>5} files",
              file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
