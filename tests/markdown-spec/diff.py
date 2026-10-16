#!/usr/bin/env python3
"""tests/markdown-spec/diff.py -- compare tur/markdown's HTML with commonmark.js's.

The spec's examples (tests/markdown-spec/run.py) show only the cases someone
wrote down.  This renders far more Markdown both ways and reports every
document whose HTML differs:

  - every Markdown file tracked in this repository, and every README in the
    sibling ../turmeric-spices checkout when there is one;
  - random documents built from Markdown's punctuation, tags, entities,
    percent escapes and non-ASCII letters, from fixed seeds, so a run is
    reproducible.

The reference is commonmark.js 0.31.2, the implementation tur/markdown was
ported from, pinned by tests/markdown-spec/oracle/package-lock.json.  Install
it with `npm ci` in that directory; without node or without it, the run
PASS-skips.  oracle/render.js patches the places where commonmark.js departs
from the spec, so every difference reported here is a finding.

Usage:
    python3 tests/markdown-spec/diff.py [--tur PATH] [--seeds 1,2,3]
                                        [--docs N] [--no-corpus] [--show N]

Exit status: 0 when every document matches (or the run skips), 1 otherwise.
"""

import argparse
import os
import random
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
ORACLE = os.path.join(HERE, 'oracle')
SEP = '\x1e'

# The random documents' vocabulary.  U+00A0 is left out: commonmark.js reads it
# as whitespace in two places render.js cannot patch (see its comment).
TOKENS = (
    ['*', '*', '**', '_', '__', '***', '[', ']', '](', '(', ')', '![', '`', '``',
     '<', '>', '&amp;', '&#65;', '&#x;', '&nosuch;', '&', '\\', '\\*', '\\\n',
     ' ', ' ', ' ', '  ', '\n', '\n', '\n\n', 'a', 'foo', 'b', 'x y',
     'http://x.y', '<http://a.b>', '<a@b.c>', '"', "'", '#', ' # ', '-', '- ',
     '1. ', '> ', '    ', '\t', '<div>', '</div>', '<b>', '</b>', '<!-- c -->',
     '[ref]', '[ref]: /u "t"\n', '[Ref][]', '[x][ref]', '(/u)', '(/u "t")',
     '(<a b>)', '</u', 'é', '“', 'ß', 'ẞ', '.', '!', '?',
     ':', '~', '|', '<a href="x">', '<?p?>', '<![CDATA[x]]>', '<!X y>', '=',
     '---', '```', '~~~', '\r\n', '\r']
    # Inline syntax again, so emphasis and links get long runs to work on.
    + ['*', '**', '_', '__', '[', ']', '](', '![', '`', 'a', 'b', ' ', '(/u)',
       '[ref]', '*a*', '**b**', '[x](/y)'] * 2
    # Percent escapes, and destinations that need encoding.
    + ['%', '%20', '%zz', '%C3%A4', '%2', '(/a%2Fb)', '(/%e2%82%ac x)', '(<%>)',
       '[r]: /%41%\n', '\\%', '\\(', '(/a\\)b)', '(/a(b)c)', '(/a\\ b)', '&#37;',
       '&percnt;', '(/a&amp;b)', '(/é)'] * 3)


def random_docs(seed, count):
    rng = random.Random(seed)
    for i in range(count):
        n = rng.randint(1, 40)
        yield ('random seed %d #%d' % (seed, i),
               ''.join(rng.choice(TOKENS) for _ in range(n)))


def git_md_files(root):
    """The Markdown files git tracks under root, or [] when root is not a checkout."""
    try:
        out = subprocess.run(['git', '-C', root, 'ls-files', '-z', '--', '*.md'],
                             capture_output=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return []
    return [os.path.join(root, p) for p in out.decode().split('\0') if p]


def corpus_docs():
    paths = [p for p in git_md_files(REPO)
             if not os.path.relpath(p, REPO).startswith('external' + os.sep)]
    spices = os.path.join(os.path.dirname(REPO), 'turmeric-spices')
    paths += [p for p in git_md_files(spices) if os.path.basename(p) == 'README.md']
    for p in sorted(paths):
        try:
            with open(p, 'rb') as f:
                text = f.read().decode('utf-8')
        except (OSError, UnicodeDecodeError):
            continue  # both renderers would read it differently; not a finding
        if SEP not in text:
            yield os.path.relpath(p, os.path.dirname(REPO)), text


def run(cmd, what):
    proc = subprocess.run(cmd, capture_output=True)
    if proc.returncode != 0:
        sys.stderr.write('%s failed (exit %d):\n%s%s\n'
                         % (what, proc.returncode, proc.stdout.decode('utf-8', 'replace')[-1000:],
                            proc.stderr.decode('utf-8', 'replace')[-2000:]))
        sys.exit(1)
    return proc.stdout.decode('utf-8', 'replace')


def split_output(text, n, what):
    parts = text.split(SEP)
    if len(parts) != n + 1:
        sys.stderr.write('%s wrote %d documents, expected %d\n' % (what, len(parts) - 1, n))
        sys.exit(1)
    return parts[:n]


def show(name, src, ours, theirs):
    i = 0
    while i < min(len(ours), len(theirs)) and ours[i] == theirs[i]:
        i += 1
    print('---- %s (first difference at byte %d) ----' % (name, i))
    print('input:        %r' % src[:400])
    print('tur:          %r' % ours[max(0, i - 60):i + 120])
    print('commonmark.js %r' % theirs[max(0, i - 60):i + 120])


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--tur', default=os.path.join(REPO, 'build', 'tur'))
    ap.add_argument('--seeds', default='1,2,3')
    ap.add_argument('--docs', type=int, default=10000, help='random documents per seed')
    ap.add_argument('--no-corpus', action='store_true', help='random documents only')
    ap.add_argument('--show', type=int, default=10, help='differences to print')
    args = ap.parse_args()

    node = shutil.which('node')
    if node is None:
        print('markdown diff: SKIP (node not found)')
        return 0
    if not os.path.isdir(os.path.join(ORACLE, 'node_modules', 'commonmark')):
        print('markdown diff: SKIP (commonmark.js not installed; run `npm ci` in %s)'
              % os.path.relpath(ORACLE, REPO))
        return 0
    if not os.access(args.tur, os.X_OK):
        sys.stderr.write('markdown diff: %s is not built\n' % args.tur)
        return 1

    docs = [] if args.no_corpus else list(corpus_docs())
    n_corpus = len(docs)
    seeds = [int(s) for s in args.seeds.split(',') if s]
    for seed in seeds:
        docs.extend(random_docs(seed, args.docs))

    with tempfile.TemporaryDirectory(prefix='tur-md-diff-') as work:
        bundle = os.path.join(work, 'bundle.md')
        with open(bundle, 'w', encoding='utf-8', newline='') as f:
            for _, src in docs:
                f.write(src)
                f.write(SEP)
        exe = os.path.join(work, 'render')
        run([args.tur, 'build', os.path.join(HERE, 'render.tur'), '-o', exe],
            'tur build render.tur')
        ours = split_output(run([exe, '--in=' + bundle], 'render.tur'), len(docs), 'render.tur')
        theirs = split_output(run([node, os.path.join(ORACLE, 'render.js'), bundle],
                                  'render.js'), len(docs), 'render.js')

    bad = [k for k in range(len(docs)) if ours[k] != theirs[k]]
    for k in bad[:args.show]:
        show(docs[k][0], docs[k][1], ours[k], theirs[k])
    print('markdown diff: %d of %d documents differ from commonmark.js 0.31.2'
          ' (%d files, %d random from seeds %s)'
          % (len(bad), len(docs), n_corpus, len(docs) - n_corpus, args.seeds))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
