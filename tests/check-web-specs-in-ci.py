#!/usr/bin/env python3
"""check-web-specs-in-ci.py -- every Try Turmeric spec is run by CI, or says why not.

A Playwright spec runs in CI only if a workflow's `npx playwright test`
command names it: the desktop step passes an explicit path list, so a spec
file that is not on it passes by not running.  `repl-intelligence.spec.js`
sat outside that list for weeks while four of its tests rotted
(docs/archive/try-turmeric-specs-outside-the-ci-path-list-have-rotted.md).

This check reads every `npx playwright test` command under
.github/workflows/ and counts a spec as run when:

  - a command names it (`tests/x.spec.js` or a bare `x.spec.js`), or
  - a `--project=mobile` command names no spec at all: the mobile project's
    testMatch is `mobile.*.spec.js`, so every such file runs.

Every other `web/tests/*.spec.js` must have a line in
`web/tests/specs-not-in-ci.txt` -- `<file> -- <reason>` -- and a line there
for a spec CI does run, or for a file that does not exist, is an error too,
so the list cannot go stale.  Part of tests/check-docs-lint.sh (Docs lint in
CI, and the pre-push hook).
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SPECS = ROOT / "web" / "tests"
EXCLUDE = SPECS / "specs-not-in-ci.txt"
WORKFLOWS = ROOT / ".github" / "workflows"


def commands(text):
    """Each `npx playwright test ...` command, continuation lines joined."""
    lines = text.splitlines()
    out = []
    for i, line in enumerate(lines):
        m = re.search(r"npx playwright test\b(.*)$", line)
        if not m:
            continue
        cmd = m.group(1)
        indent = len(line) - len(line.lstrip())
        j = i + 1
        # A YAML plain scalar continues on more-indented lines.
        while j < len(lines) and lines[j].strip() and \
                len(lines[j]) - len(lines[j].lstrip()) > indent and \
                not lines[j].lstrip().startswith(("#", "-", "id:", "if:", "env:")):
            cmd += " " + lines[j].strip()
            j += 1
        out.append(cmd)
    return out


def main():
    run = set()
    for wf in sorted(WORKFLOWS.glob("*.yml")):
        for cmd in commands(wf.read_text()):
            named = re.findall(r"(?:tests/)?([A-Za-z0-9_.-]+\.spec\.js)\b", cmd)
            run.update(named)
            if not named and "--project=mobile" in cmd:
                run.update(p.name for p in SPECS.glob("mobile.*.spec.js"))

    excluded = {}
    errors = []
    if EXCLUDE.exists():
        for n, raw in enumerate(EXCLUDE.read_text().splitlines(), 1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            name, sep, reason = line.partition(" -- ")
            name = name.strip()
            if not sep or not reason.strip():
                errors.append(f"{EXCLUDE.relative_to(ROOT)}:{n}: "
                              f"'{line}' needs '<file> -- <reason>'")
                continue
            excluded[name] = n

    specs = sorted(p.name for p in SPECS.glob("*.spec.js"))
    for name in specs:
        if name not in run and name not in excluded:
            errors.append(f"web/tests/{name}: no CI workflow runs it -- add it to a "
                          f"`npx playwright test` path list, or give it a line in "
                          f"{EXCLUDE.relative_to(ROOT)} saying why not")
    for name, n in sorted(excluded.items()):
        if name not in specs:
            errors.append(f"{EXCLUDE.relative_to(ROOT)}:{n}: {name} does not exist")
        elif name in run:
            errors.append(f"{EXCLUDE.relative_to(ROOT)}:{n}: {name} is run by CI; "
                          f"delete its line")

    if errors:
        for e in errors:
            print(f"FAIL {e}")
        return 1
    print(f"PASS check-web-specs-in-ci ({len(specs)} specs: "
          f"{len([s for s in specs if s in run])} run by CI, {len(excluded)} excluded)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
