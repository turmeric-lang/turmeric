#!/usr/bin/env python3
"""
tests/tools/check_collect_docs.py -- tools/ci/collect-docs.py must count a
report as archived however it left docs/reported/.

/ci charts reports archived minus reports filed from the running totals
`reports_filed` / `reports_archived`, which the collector reads off
`git log --first-parent -m --no-renames -- docs/reported/`.  The usual archive,
`git mv docs/reported/x.md docs/archive/x.md`, is a rename (R100) on the whole
tree; under that pathspec it must still count as archived.

The merge case is the one that caught a real bug: with rename detection on,
reports written from the same template made git pair a report archived in a
merge with a different one filed in it, as a "rename" inside docs/reported/,
and both vanished from the totals.  This builds throwaway repos and pins the
totals and the invariant /ci relies on:

    reports_filed - reports_archived == open_reports

Needs git; skips cleanly without it.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
COLLECT = os.path.join(ROOT, 'tools', 'ci', 'collect-docs.py')

if shutil.which('git') is None:
    print('SKIP check-collect-docs -- git not available')
    sys.exit(0)

fails = 0


def git(repo, *args):
    subprocess.run(['git', '-C', repo, *args], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def write(repo, rel, text):
    path = os.path.join(repo, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w') as f:
        f.write(text)


def report(name):
    # Enough lines that a one-line edit stays a high-similarity rename.
    return f'# {name}\n\nsummary\n\nrepro\n\nroot cause\n\nfix directions\n'


def collect(repo):
    env = {k: v for k, v in os.environ.items() if k != 'GITHUB_SHA'}
    out = subprocess.run([sys.executable, COLLECT, '--repo', repo,
                          '--spices-repo', os.path.join(repo, 'no-spices')],
                         check=True, stdout=subprocess.PIPE, env=env).stdout
    return json.loads(out)


def expect(what, row, filed, archived, open_reports):
    global fails
    got = (row['reports_filed'], row['reports_archived'], row['open_reports'])
    if got != (filed, archived, open_reports):
        fails += 1
        print(f'FAIL check-collect-docs ({what}): (filed, archived, open) = {got}, '
              f'want {(filed, archived, open_reports)}')
    elif filed is not None and filed - archived != open_reports:
        fails += 1
        print(f'FAIL check-collect-docs ({what}): filed - archived != open_reports')


tmp = tempfile.mkdtemp(prefix='collect-docs-')
try:
    repo = os.path.join(tmp, 'repo')
    os.makedirs(repo)
    git(repo, 'init', '-q', '-b', 'main')
    git(repo, 'config', 'user.email', 'ci@example.invalid')
    git(repo, 'config', 'user.name', 'ci')
    git(repo, 'config', 'commit.gpgsign', 'false')

    # Four reports, an index, and an archive -- none of the last two counted.
    for n in ('a', 'b', 'c', 'd'):
        write(repo, f'docs/reported/{n}.md', report(n))
    write(repo, 'docs/reported/README.md', '| report |\n')
    write(repo, 'docs/archive/old.md', report('old'))
    git(repo, 'add', '-A')
    git(repo, 'commit', '-qm', 'file four reports')
    expect('four filed', collect(repo), 4, 0, 4)

    # The usual archive: a pure move, which git calls R100 on the whole tree.
    git(repo, 'mv', 'docs/reported/a.md', 'docs/archive/a.md')
    git(repo, 'commit', '-qm', 'archive a')
    expect('pure git mv to docs/archive/', collect(repo), 4, 1, 3)

    # Archived with a resolution note added in the same commit.
    git(repo, 'mv', 'docs/reported/b.md', 'docs/archive/b.md')
    with open(os.path.join(repo, 'docs/archive/b.md'), 'a') as f:
        f.write('\nResolved.\n')
    git(repo, 'add', '-A')
    git(repo, 'commit', '-qm', 'archive b with a note')
    expect('move plus edit', collect(repo), 4, 2, 2)

    # A slug change inside docs/reported/ counts as one filed and one archived:
    # renames are off, and the net -- what /ci charts -- is unchanged.
    git(repo, 'mv', 'docs/reported/c.md', 'docs/reported/c-renamed.md')
    git(repo, 'commit', '-qm', 'rename c')
    expect('rename inside docs/reported/', collect(repo), 5, 3, 2)

    # A branch that files one report and archives another, merged with a merge
    # commit: -m diffs the merge against main, so both land exactly once --
    # and, the two being from one template, are not mistaken for a rename.
    git(repo, 'checkout', '-qb', 'topic')
    write(repo, 'docs/reported/e.md', report('e'))
    git(repo, 'add', '-A')
    git(repo, 'commit', '-qm', 'file e')
    git(repo, 'mv', 'docs/reported/d.md', 'docs/archive/d.md')
    git(repo, 'commit', '-qm', 'archive d')
    git(repo, 'checkout', '-q', 'main')
    write(repo, 'docs/reported/f.md', report('f'))
    git(repo, 'add', '-A')
    git(repo, 'commit', '-qm', 'file f on main')
    git(repo, 'merge', '-q', '--no-ff', '--no-edit', 'topic')
    expect('merged branch', collect(repo), 7, 4, 3)

    # A shallow clone cannot see the history, so it must not guess.
    shallow = os.path.join(tmp, 'shallow')
    subprocess.run(['git', 'clone', '-q', '--depth', '1', f'file://{repo}', shallow],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    expect('shallow clone', collect(shallow), None, None, 3)
finally:
    shutil.rmtree(tmp, ignore_errors=True)

if fails:
    print(f'check-collect-docs: {fails} case(s) failed')
    sys.exit(1)
print('PASS check-collect-docs (move, move + edit, in-place rename, merge, shallow)')
