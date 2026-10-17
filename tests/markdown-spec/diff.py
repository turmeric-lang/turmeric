#!/usr/bin/env python3
"""tests/markdown-spec/diff.py -- compare tur/markdown's HTML with the reference implementations'.

The spec's examples (tests/markdown-spec/run.py) show only the cases someone
wrote down.  This renders far more Markdown both ways and reports every
document whose HTML differs:

  - every Markdown file tracked in this repository, and every README in the
    sibling ../turmeric-spices checkout when there is one;
  - random documents built from Markdown's punctuation, tags, entities,
    percent escapes and non-ASCII letters, from fixed seeds, so a run is
    reproducible.

It makes two comparisons, and each one skips on its own when its reference
is missing:

  - Plain CommonMark against commonmark.js 0.31.2, the implementation
    tur/markdown was ported from, pinned by tests/markdown-spec/oracle/
    package-lock.json.  Install it with `npm ci` in that directory.
    oracle/render.js patches the places where commonmark.js departs from the
    spec, so every difference reported here is a finding.
  - The GFM extensions (markdown/default-opts and the tag filter) against
    cmark-gfm, GitHub's renderer, at the commit oracle/cmark-gfm.pin names,
    checked out in ../cmark-gfm.  This script builds it, with
    oracle/gfmrender.c, in a temporary directory.  cmark-gfm's core is
    CommonMark 0.29, not 0.31.2, so a document counts as a GFM finding only
    when the two renderers agree on it with the extensions off -- and agree
    on it with its tildes taken out too, because with strikethrough on
    cmark-gfm reads past a ~ when it judges a * or _ run, which can bring the
    two versions' emphasis rules (****foo****, say) into play.  Two
    differences between cmark and commonmark.js are normalised away: cmark
    writes ' in an attribute as &#x27;, and drops a tab before a line break.
    The documents are the same files, plus random ones built for the
    extensions: pipes and table-shaped lines, tildes, URLs, email addresses
    and task boxes.

Usage:
    python3 tests/markdown-spec/diff.py [--tur PATH] [--seeds 1,2,3]
                                        [--docs N] [--no-corpus] [--show N]
                                        [--cmark-gfm DIR]

Exit status: 0 when every document matches (or the run skips), 1 otherwise.
"""

import argparse
import os
import random
import re
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


# The GFM documents' vocabulary: table syntax, tildes, the shapes the
# autolink extension looks for (a scheme, www., an @, the punctuation it
# trims) and task boxes, then enough Markdown around them to interact with.
GFM_TOKENS = (
    ['|', '|', ' | ', '||', '\\|', '|-|', '|---|', '-|-', ':-:', '-:', ':-', ' :--- ',
     '~', '~~', '~~~', '~~~~', 'www.', 'www.a.b', 'www.x_y.z', 'www.a.b/c(d)',
     'http://', 'https://x.y/z', 'http://a.b.', 'ftp://a.b', 'HTTP://A.B', 'mailto:',
     'xmpp:', '@', 'a@b.c', 'a.b@c.d', 'x+y@z.org', 'foo@bar', '@x.y', '.', ',', ')',
     '(', '&amp;', '&lt;', ';', '&x;', '?', '!', ':', '"', "'", '_', '*', '**',
     '- [ ] ', '- [x] ', '* [X] ', '1. [ ] ', '[ ]', '[x]', '[X]', '<script>',
     '<title>', '<style>x</style>', '<xmp>', '</textarea>', '<b>', '`', '``', '[',
     ']', '](', '(/u)', '\\', '\\~', ' ', ' ', '  ', '\t', '\n', '\n', '\n',
     '\n\n', 'a', 'b', 'foo', 'x y', '> ', '- ', '    ', '#', '<', '>', '---', '-',
     '\u00e9', '\u00df', '\u3000', '\u00a0', '<http://a.b>', '<a@b.c>'])
CELL_TOKENS = GFM_TOKENS[:60] + ['a', 'b', ' ', 'x']


def gfm_docs(seed, count):
    """Random documents for the GFM comparison; about a third are table-shaped."""
    rng = random.Random(seed)

    def row(n):
        s = '|'.join(''.join(rng.choice(CELL_TOKENS) for _ in range(rng.randint(0, 3)))
                     for _ in range(n))
        return ('|' if rng.random() < 0.5 else '') + s + ('|' if rng.random() < 0.5 else '')

    def text(lo, hi):
        return ''.join(rng.choice(GFM_TOKENS) for _ in range(rng.randint(lo, hi)))

    for i in range(count):
        if rng.random() < 0.35:
            cols = rng.randint(1, 4)
            lines = [text(0, 3)] if rng.random() < 0.5 else []
            lines.append(row(cols))
            delims = [rng.choice(['-', '---', ':-', '-:', ':-:', ' - ', '--', ':', ''])
                      for _ in range(rng.randint(max(1, cols - 1), cols + 1))]
            lines.append(('|' if rng.random() < 0.5 else '') + '|'.join(delims)
                         + ('|' if rng.random() < 0.5 else ''))
            for _ in range(rng.randint(0, 4)):
                lines.append(row(rng.randint(1, cols + 1)) if rng.random() < 0.5 else text(0, 4))
            yield ('gfm table seed %d #%d' % (seed, i),
                   '\n'.join(lines) + rng.choice(['', '\n']))
        else:
            yield ('gfm random seed %d #%d' % (seed, i), text(1, 40))


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


def show(name, src, ours, theirs, who):
    i = 0
    while i < min(len(ours), len(theirs)) and ours[i] == theirs[i]:
        i += 1
    print('---- %s (first difference at byte %d) ----' % (name, i))
    print('input:         %r' % src[:400])
    print('tur:           %r' % ours[max(0, i - 60):i + 120])
    print('%-14s %r' % (who, theirs[max(0, i - 60):i + 120]))


def cmark_gfm_problem(src):
    """Why ../cmark-gfm cannot be used, or None when it is at the pinned commit."""
    with open(os.path.join(ORACLE, 'cmark-gfm.pin')) as f:
        pin = f.readline().strip()
    if not os.path.isfile(os.path.join(src, 'extensions', 'table.c')):
        return 'no cmark-gfm checkout at %s' % src
    head = subprocess.run(['git', '-C', src, 'rev-parse', 'HEAD'],
                          capture_output=True).stdout.decode().strip()
    if head != pin:
        return '%s is at %s, not %s (oracle/cmark-gfm.pin)' % (src, head[:12] or '?', pin[:12])
    return None


def build_cmark_gfm(src, work):
    """Build cmark-gfm's static libraries and oracle/gfmrender.c; the driver's path."""
    build = os.path.join(work, 'cmark-gfm')
    run(['cmake', '-S', src, '-B', build, '-DCMAKE_BUILD_TYPE=Release',
         '-DCMARK_TESTS=OFF', '-DCMARK_SHARED=OFF', '-DCMAKE_POLICY_VERSION_MINIMUM=3.5'],
        'cmake configure cmark-gfm')
    run(['cmake', '--build', build, '-j', str(os.cpu_count() or 2),
         '--target', 'libcmark-gfm_static', 'libcmark-gfm-extensions_static'],
        'cmake build cmark-gfm')
    exe = os.path.join(work, 'gfmrender')
    run([os.environ.get('CC', 'cc'), '-O2', '-o', exe, os.path.join(ORACLE, 'gfmrender.c'),
         '-I', os.path.join(src, 'src'), '-I', os.path.join(src, 'extensions'),
         '-I', os.path.join(build, 'src'), '-I', os.path.join(build, 'extensions'),
         os.path.join(build, 'extensions', 'libcmark-gfm-extensions.a'),
         os.path.join(build, 'src', 'libcmark-gfm.a')], 'cc gfmrender.c')
    return exe


def run_bundle(cmd, docs, work, name, what):
    """Write docs as one bundle and run cmd over it: an argument ending in '='
    gets the bundle's path appended, and with none the bundle is piped to
    stdin.  Each document's HTML."""
    bundle = os.path.join(work, name)
    with open(bundle, 'w', encoding='utf-8', newline='') as f:
        for _, src in docs:
            f.write(src)
            f.write(SEP)
    argv = [a + bundle if a.endswith('=') else a for a in cmd]
    if argv != cmd:
        return split_output(run(argv, what), len(docs), what)
    with open(bundle, 'rb') as f:
        proc = subprocess.run(cmd, stdin=f, capture_output=True)
    if proc.returncode != 0:
        sys.stderr.write('%s failed (exit %d):\n%s\n' % (what, proc.returncode,
                         proc.stderr.decode('utf-8', 'replace')[-2000:]))
        sys.exit(1)
    return split_output(proc.stdout.decode('utf-8', 'replace'), len(docs), what)


def cmark_norm(html):
    """The two places cmark's HTML differs from commonmark.js's for one input."""
    return re.sub(r'[ \t]+\n', '\n', html.replace('&#x27;', "'"))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--tur', default=os.path.join(REPO, 'build', 'tur'))
    ap.add_argument('--seeds', default='1,2,3')
    ap.add_argument('--docs', type=int, default=10000, help='random documents per seed')
    ap.add_argument('--no-corpus', action='store_true', help='random documents only')
    ap.add_argument('--show', type=int, default=10, help='differences to print')
    ap.add_argument('--cmark-gfm', default=os.path.join(os.path.dirname(REPO), 'cmark-gfm'),
                    help='the cmark-gfm checkout (default: the sibling ../cmark-gfm)')
    args = ap.parse_args()

    node = shutil.which('node')
    cm_skip = None
    if node is None:
        cm_skip = 'node not found'
    elif not os.path.isdir(os.path.join(ORACLE, 'node_modules', 'commonmark')):
        cm_skip = ('commonmark.js not installed; run `npm ci` in %s'
                   % os.path.relpath(ORACLE, REPO))
    gfm_skip = cmark_gfm_problem(args.cmark_gfm)
    if gfm_skip is None and shutil.which('cmake') is None:
        gfm_skip = 'cmake not found'
    if cm_skip and gfm_skip:
        print('markdown diff: SKIP (%s; %s)' % (cm_skip, gfm_skip))
        return 0
    if not os.access(args.tur, os.X_OK):
        sys.stderr.write('markdown diff: %s is not built\n' % args.tur)
        return 1

    corpus = [] if args.no_corpus else list(corpus_docs())
    seeds = [int(s) for s in args.seeds.split(',') if s]
    docs = corpus + [d for seed in seeds for d in random_docs(seed, args.docs)]
    gdocs = corpus + [d for seed in seeds for d in gfm_docs(seed, args.docs)]
    failed = False

    with tempfile.TemporaryDirectory(prefix='tur-md-diff-') as work:
        exe = os.path.join(work, 'render')
        run([args.tur, 'build', os.path.join(HERE, 'render.tur'), '-o', exe],
            'tur build render.tur')

        if cm_skip:
            print('markdown diff: commonmark.js comparison SKIP (%s)' % cm_skip)
        else:
            ours = run_bundle([exe, '--in='], docs, work, 'core.md', 'render.tur')
            theirs = split_output(run([node, os.path.join(ORACLE, 'render.js'),
                                       os.path.join(work, 'core.md')], 'render.js'),
                                  len(docs), 'render.js')
            bad = [k for k in range(len(docs)) if ours[k] != theirs[k]]
            for k in bad[:args.show]:
                show(docs[k][0], docs[k][1], ours[k], theirs[k], 'commonmark.js')
            print('markdown diff: %d of %d documents differ from commonmark.js 0.31.2'
                  ' (%d files, %d random from seeds %s)'
                  % (len(bad), len(docs), len(corpus), len(docs) - len(corpus), args.seeds))
            failed = failed or bool(bad)

        if gfm_skip:
            print('markdown diff: cmark-gfm comparison SKIP (%s)' % gfm_skip)
        else:
            gfmrender = build_cmark_gfm(args.cmark_gfm, work)
            ours_core = run_bundle([exe, '--in='], gdocs, work, 'gfm.md', 'render.tur')
            ours_gfm = run_bundle([exe, '--gfm', '--in='], gdocs, work, 'gfm.md',
                                  'render.tur --gfm')
            ref_core = run_bundle([gfmrender, 'core'], gdocs, work, 'gfm.md', 'gfmrender core')
            ref_gfm = run_bundle([gfmrender, 'gfm'], gdocs, work, 'gfm.md', 'gfmrender gfm')
            bare = [(name, src.replace('~', '')) for name, src in gdocs]
            ours_bare = run_bundle([exe, '--in='], bare, work, 'bare.md', 'render.tur')
            ref_bare = run_bundle([gfmrender, 'core'], bare, work, 'bare.md', 'gfmrender core')
            skipped = {k for k in range(len(gdocs))
                       if cmark_norm(ours_core[k]) != cmark_norm(ref_core[k])
                       or cmark_norm(ours_bare[k]) != cmark_norm(ref_bare[k])}
            bad = [k for k in range(len(gdocs)) if k not in skipped
                   and cmark_norm(ours_gfm[k]) != cmark_norm(ref_gfm[k])]
            for k in bad[:args.show]:
                show(gdocs[k][0], gdocs[k][1], cmark_norm(ours_gfm[k]),
                     cmark_norm(ref_gfm[k]), 'cmark-gfm')
            print('markdown diff: %d of %d documents differ from cmark-gfm with the GFM'
                  ' extensions on (%d files, %d random from seeds %s; %d left out, where'
                  ' CommonMark 0.29 and 0.31.2 differ)'
                  % (len(bad), len(gdocs) - len(skipped), len(corpus),
                     len(gdocs) - len(corpus), args.seeds, len(skipped)))
            failed = failed or bool(bad)

    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
