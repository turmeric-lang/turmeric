#!/usr/bin/env python3
"""
tools/check-guide-pairs.py -- Verify toggle pairs in Turmeric guide markdown files
and spice READMEs.

Usage:
    python3 tools/check-guide-pairs.py docs/guides/                # all guides
    python3 tools/check-guide-pairs.py docs/guides/quickstart.md   # single file
    python3 tools/check-guide-pairs.py --spices                    # spice READMEs
    python3 tools/check-guide-pairs.py docs/guides/ --strict-unpaired

For each adjacent turmeric+sweet-exp block pair found, the checker reports:
  - The file and approximate line number
  - Whether the pair is non-empty on both sides
  - (if 'tur' binary available) parse-equality: both blocks parse to the same AST

Independently of pairing, every fenced block whose first form is
`(defpackage ...)` is shape-checked as a build.tur with `tur fetch --dry-run`
(no network). Mark a block ```turmeric no-manifest-check to opt out. This is
gap (1) of docs/archive/spice-guides-bare-brace-manifest-syntax.md: manifest
snippets are the ones most likely to be pasted verbatim, and 107 of them had
rotted into hard parse errors with nothing to notice.

The 'tur' binary is taken from --tur, then $PATH, then ./build/tur. Without it
both the parse-equality and manifest checks silently no-op.

With --strict-unpaired the checker also fails when a turmeric block is NOT
followed by an adjacent sweet-exp sibling. Mark a block ```turmeric no-check
to opt out (used for install / config / API signature blocks).

Exit code: 0 if all checks pass, 1 if any pair fails or no pairs are found
and --require-pairs is set.
"""

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


TURMERIC_OPEN_RE = re.compile(r'(?m)^```turmeric(?P<mods>[^\n]*)\n')
# Matched with .match(text, pos); the match is anchored at pos (no \A needed).
SWEET_AFTER_RE = re.compile(r'\s*```sweet-exp\n')

SPICES_ROOT = Path('../turmeric-spices/spices')


def _read_fenced_block(text: str, pos: int) -> tuple[str, int]:
    """Read a markdown fenced block whose opening fence's newline is at `pos`.

    Returns (content, end) where `end` is just past the closing fence line.

    A markdown block closes at a column-0 bare ``` line. Turmeric inline-C
    blocks use ``` to toggle a C span (```c opens; ``` or ```) closes) and may
    be indented or written inline, so we track the C span by scanning ``` runs
    and only treat a bare ``` as the block close when not inside a C span. This
    avoids the non-greedy-regex bug where a standalone turmeric block (or a
    nested ```c fence) merged content across real block boundaries.
    """
    n = len(text)
    i = pos
    line_start = pos
    in_c = False
    while i < n:
        if text.startswith('```', i):
            at_col0 = (i == line_start)
            after = text[i + 3] if i + 3 < n else '\n'
            if in_c:
                in_c = False
                i += 3
                continue
            if after.isalnum():           # info string -> opens a (C) span
                in_c = True
                i += 3
                continue
            if at_col0:                   # bare ``` at column 0 -> block close
                j = text.find('\n', i)
                end = (j + 1) if j != -1 else n
                return text[pos:i], end
            i += 3
            continue
        if text[i] == '\n':
            line_start = i + 1
        i += 1
    return text[pos:], n


def find_pairs(text: str) -> list[tuple[str, str, int]]:
    """Return list of (turmeric_src, sweet_exp_src, line_number) for each pair.

    A pair is a ```turmeric block (no modifiers) immediately followed -- only
    blank lines between -- by a ```sweet-exp block. Blocks are delimited with a
    fence-aware scanner so nested ```c inline-C and standalone turmeric blocks
    do not corrupt the boundaries.
    """
    pairs = []
    for m in TURMERIC_OPEN_RE.finditer(text):
        if (m.group('mods') or '').strip():
            continue  # e.g. ```turmeric no-check -- opted out
        line_no = text[:m.start()].count('\n') + 1
        tur_src, after = _read_fenced_block(text, m.end())
        sm = SWEET_AFTER_RE.match(text, after)
        if not sm:
            continue
        sweet_src, _ = _read_fenced_block(text, sm.end())
        pairs.append((tur_src, sweet_src, line_no))
    return pairs


def check_nonempty(tur_src: str, sweet_src: str) -> list[str]:
    errors = []
    if not tur_src.strip():
        errors.append('turmeric block is empty')
    if not sweet_src.strip():
        errors.append('sweet-exp block is empty')
    return errors


def check_ascii(tur_src: str, sweet_src: str) -> list[str]:
    errors = []
    for i, ch in enumerate(tur_src):
        if ord(ch) > 127:
            errors.append(f'turmeric block contains non-ASCII character at offset {i}: {repr(ch)}')
            break
    for i, ch in enumerate(sweet_src):
        if ord(ch) > 127:
            errors.append(f'sweet-exp block contains non-ASCII character at offset {i}: {repr(ch)}')
            break
    return errors


def try_parse_check(tur_src: str, sweet_src: str, tur_bin: str) -> list[str]:
    """Use 'tur parse-check' to compare ASTs. Returns error strings."""
    errors = []
    with tempfile.NamedTemporaryFile(suffix='.tur', mode='w', delete=False) as tf:
        tf.write(tur_src)
        tur_file = tf.name
    with tempfile.NamedTemporaryFile(suffix='.sweet', mode='w', delete=False) as sf:
        sf.write(sweet_src)
        sweet_file = sf.name
    try:
        result = subprocess.run(
            [tur_bin, 'parse-check', tur_file, sweet_file],
            capture_output=True, text=True, timeout=10,
        )
        if result.returncode != 0:
            errors.append(f'parse-check failed: {result.stderr.strip() or result.stdout.strip()}')
    except FileNotFoundError:
        pass  # tur binary not available; skip parse check
    except subprocess.TimeoutExpired:
        errors.append('parse-check timed out')
    finally:
        Path(tur_file).unlink(missing_ok=True)
        Path(sweet_file).unlink(missing_ok=True)
    return errors


# `lisp` is in the set because README.md fences its manifest snippets that way
# -- the front door, and 5 of the 107 rotted sites.
ANY_BLOCK_OPEN_RE = re.compile(r'(?m)^```(?P<lang>turmeric|sweet-exp|lisp)(?P<mods>[^\n]*)\n')
# A manifest block is one whose first real form is (defpackage ...) -- plain --
# or `defpackage ...` at column 0 -- sweet-exp.
DEFPACKAGE_RE = re.compile(r'(?m)^\s*\(?defpackage\b')


def find_manifest_blocks(text: str) -> list[tuple[str, str, int]]:
    """Return (src, lang, line_number) for every fenced block that is a build.tur.

    docs/archive/spice-guides-bare-brace-manifest-syntax.md, gap (1): a
    ```turmeric fence in a guide is unvalidated prose, so 107 manifest snippets
    across the guides and the README rotted into a hard parse error (bare
    `{...}` became curly-infix) without anything noticing. Manifests are the
    worst case for that -- they are the snippets most likely to be pasted
    verbatim by someone starting a project -- and unlike a general doc-compiler
    they are cheap to validate: `tur fetch --dry-run` reads and shape-checks the
    manifest without touching the network.

    `no-check` does NOT opt a block out here. That marker means "this block has
    no sweet-exp companion", which is true of almost every manifest snippet --
    and those are precisely the ones that rotted. Use `no-manifest-check` for a
    snippet that is deliberately not a valid manifest (an error example, a
    fragment shown mid-edit).
    """
    blocks = []
    for m in ANY_BLOCK_OPEN_RE.finditer(text):
        if 'no-manifest-check' in (m.group('mods') or '').split():
            continue
        src, _ = _read_fenced_block(text, m.end())
        if not DEFPACKAGE_RE.search(src):
            continue
        blocks.append((src, m.group('lang'), text[:m.start()].count('\n') + 1))
    return blocks


def check_manifest(src: str, lang: str, tur_bin: str) -> list[str]:
    """Shape-check one manifest snippet with `tur fetch --dry-run`.

    Only manifest-level diagnostics (those prefixed with the manifest filename)
    are treated as failures, and existence checks are excluded on top of that: a
    snippet legitimately names `:c-sources`/`:include-dirs`/`:path` entries that
    exist only in the reader's own project, and "not found" is a fact about this
    temp dir, not a defect in the doc. What remains is exactly the syntax and
    shape checking the report asked for.
    """
    RESOLUTION_ONLY = ('not found (resolved to',)
    name = 'build.tur.sweet' if lang == 'sweet-exp' else 'build.tur'
    with tempfile.TemporaryDirectory() as td:
        (Path(td) / name).write_text(src, encoding='utf-8')
        try:
            result = subprocess.run(
                [tur_bin, 'fetch', '--dry-run'],
                cwd=td, capture_output=True, text=True, timeout=20,
            )
        except FileNotFoundError:
            return []            # tur binary not available; skip
        except subprocess.TimeoutExpired:
            return ['manifest check timed out']
    out = (result.stderr or '') + (result.stdout or '')
    bad = [ln for ln in out.splitlines()
           if ln.startswith(name + ':')
           and not any(sub in ln for sub in RESOLUTION_ONLY)]
    return [f'manifest snippet does not parse: {bad[0]}'] if bad else []


def find_unpaired_turmeric(text: str) -> list[tuple[int, str]]:
    """
    Return list of (line_number, modifier_string) for every turmeric block
    that is NOT followed by an adjacent sweet-exp block and is NOT marked
    `no-check`.
    """
    unpaired = []
    for m in TURMERIC_OPEN_RE.finditer(text):
        mods = (m.group('mods') or '').strip()
        if 'no-check' in mods.split():
            continue
        _, after = _read_fenced_block(text, m.end())
        # Adjacent means only whitespace between the closing ``` and ```sweet-exp.
        if SWEET_AFTER_RE.match(text, after):
            continue
        line_no = text[:m.start()].count('\n') + 1
        unpaired.append((line_no, mods))
    return unpaired


# A fence line sitting inside a blockquote: `>` (possibly nested), then a
# fence opener or closer and NOTHING else on the line.  The trailing `$` is what
# keeps this off an inline code SPAN that merely talks about a fence -- e.g.
# package-management-guide.md's ```` ```turmeric no-manifest-check ```` ...``,
# where prose follows on the same line.  The info string is word-ish tokens for
# the same reason: a span's closing backticks never match `[\w.+-]`.
BLOCKQUOTED_FENCE_RE = re.compile(
    r'^[ \t]*>[ \t>]*(?:`{3,}|~{3,})[ \t]*'
    r'(?:[\w.+-]+(?:[ \t]+[\w.+-]+)*)?[ \t]*$'
)


def find_blockquoted_fences(text: str) -> list[int]:
    """Return the 1-based line number of every fence OPENER inside a
    blockquote.

    python-markdown's `fenced_code` is a preprocessor whose fence regex anchors
    at the start of a line, so a fence prefixed by `> ` is invisible to it. The
    blockquote processor then strips the `> ` and the body is rendered as
    ordinary markdown: the opening fence becomes literal text, a `#` shell
    comment becomes an `<h1>`, and `<angle>` placeholders become raw unclosed
    tags. Indented code blocks are unaffected -- those are core markdown,
    handled AFTER the blockquote prefix is stripped -- so `>     cmd` is the fix.

    Only openers are reported: the closer is the same construct, and naming
    both lines would double every diagnostic.
    """
    openers: list[int] = []
    inside = False
    for i, line in enumerate(text.splitlines(), start=1):
        if BLOCKQUOTED_FENCE_RE.match(line):
            if not inside:
                openers.append(i)
            inside = not inside
    return openers


def check_file(path: Path, tur_bin: str | None, verbose: bool,
               strict_unpaired: bool = False) -> tuple[int, int, int, int, int, int, int]:
    """Returns (pairs_found, pairs_ok, pairs_failed, unpaired_failed,
    manifests_found, manifests_failed, blockquoted_fences)."""
    text = path.read_text(encoding='utf-8')
    pairs = find_pairs(text)
    ok = failed = unpaired_failed = 0
    manifests = man_failed = 0
    for tur_src, sweet_src, line_no in pairs:
        errors: list[str] = []
        errors += check_nonempty(tur_src, sweet_src)
        errors += check_ascii(tur_src, sweet_src)
        if tur_bin and not errors:
            errors += try_parse_check(tur_src, sweet_src, tur_bin)
        if errors:
            failed += 1
            print(f'FAIL  {path}:{line_no}')
            for e in errors:
                print(f'      {e}')
        else:
            ok += 1
            if verbose:
                print(f'ok    {path}:{line_no}')

    if strict_unpaired:
        for line_no, mods in find_unpaired_turmeric(text):
            unpaired_failed += 1
            mods_str = f' {mods}' if mods else ''
            print(f'FAIL  {path}:{line_no}')
            print(f'      turmeric block (```turmeric{mods_str}) has no adjacent '
                  '```sweet-exp sibling')
            print(f'      add a sweet-exp companion, or mark as ```turmeric no-check')

    if tur_bin:
        for src, lang, line_no in find_manifest_blocks(text):
            manifests += 1
            errors = check_manifest(src, lang, tur_bin)
            if errors:
                man_failed += 1
                print(f'FAIL  {path}:{line_no}')
                for e in errors:
                    print(f'      {e}')
            elif verbose:
                print(f'ok    {path}:{line_no} (manifest)')

    bq_fences = 0
    for line_no in find_blockquoted_fences(text):
        bq_fences += 1
        print(f'FAIL  {path}:{line_no}')
        print("      fenced code block inside a blockquote: python-markdown's")
        print('      fenced_code never sees a fence prefixed by "> ", so the body')
        print('      renders as prose (# comments become headings, <x> becomes a')
        print('      raw tag) and the docs build fails on the malformed fragment')
        print('      use an INDENTED code block instead: ">     cmd"')

    return (len(pairs), ok, failed, unpaired_failed, manifests, man_failed,
            bq_fences)


def collect_md_files(paths: list[str], include_readme: bool) -> list[Path]:
    """Resolve CLI path arguments to a flat list of markdown files."""
    md_files: list[Path] = []
    for raw in paths:
        pt = Path(raw)
        if pt.is_dir():
            # Directory with .md files directly under it (guides).
            direct = sorted(
                f for f in pt.glob('*.md')
                if include_readme or f.stem != 'README'
            )
            md_files += direct
            # Directory of subdirs containing READMEs (spices/*).
            for sub in sorted(p for p in pt.iterdir() if p.is_dir()):
                readme = sub / 'README.md'
                if readme.is_file():
                    md_files.append(readme)
        elif pt.suffix == '.md':
            md_files.append(pt)
        else:
            print(f'warning: skipping {pt} (not a .md file or directory)', file=sys.stderr)
    # Dedupe while preserving order
    seen = set()
    unique = []
    for f in md_files:
        key = f.resolve()
        if key not in seen:
            seen.add(key)
            unique.append(f)
    return unique


def main() -> None:
    p = argparse.ArgumentParser(description='Check guide toggle pair equivalence.')
    p.add_argument('paths', nargs='*', help='Guide file(s) or directory of .md files')
    p.add_argument('--tur', default=None, help='Path to tur binary (default: auto-detect)')
    p.add_argument('--verbose', '-v', action='store_true')
    p.add_argument('--require-pairs', action='store_true',
                   help='Exit 1 if no pairs are found at all')
    p.add_argument('--strict-unpaired', action='store_true',
                   help='Also fail when a turmeric block has no adjacent '
                        'sweet-exp sibling (mark with `no-check` to opt out).')
    p.add_argument('--spices', action='store_true',
                   help=f'Shortcut: check spice READMEs under {SPICES_ROOT} '
                        'with --strict-unpaired.')
    args = p.parse_args()

    if args.spices:
        if not SPICES_ROOT.is_dir():
            print(f'error: {SPICES_ROOT} not found. Clone the sibling repo '
                  f'next to this checkout.', file=sys.stderr)
            sys.exit(1)
        args.paths.append(str(SPICES_ROOT))
        args.strict_unpaired = True

    if not args.paths:
        p.error('no paths given (use --spices or pass a path)')

    tur_bin = args.tur or shutil.which('tur')
    if not tur_bin:
        for cand in (Path('build/tur'), Path('build-release/tur')):
            if cand.is_file():
                tur_bin = str(cand.resolve())
                break

    md_files = collect_md_files(args.paths, include_readme=args.spices)

    total_pairs = total_ok = total_failed = total_unpaired = 0
    total_manifests = total_man_failed = 0
    total_bq_fences = 0
    for f in md_files:
        n, ok, fail, unpaired, manifests, man_failed, bq = check_file(
            f, tur_bin, args.verbose, strict_unpaired=args.strict_unpaired,
        )
        total_pairs += n
        total_ok += ok
        total_failed += fail
        total_unpaired += unpaired
        total_manifests += manifests
        total_man_failed += man_failed
        total_bq_fences += bq

    paired_guides = sum(
        1 for f in md_files
        if find_pairs(f.read_text(encoding='utf-8'))
    )

    print()
    print(f'Files checked    : {len(md_files)}')
    print(f'Files with pairs : {paired_guides}')
    print(f'Pairs found      : {total_pairs}')
    print(f'Pairs ok         : {total_ok}')
    print(f'Pairs failed     : {total_failed}')
    if args.strict_unpaired:
        print(f'Unpaired blocks  : {total_unpaired}')
    print(f'Manifests found  : {total_manifests}')
    print(f'Manifests failed : {total_man_failed}')
    print(f'Blockquoted fences: {total_bq_fences}')
    if tur_bin:
        print(f'Checker          : {tur_bin} (parse-check + manifest dry-run)')
    else:
        print('Checker          : basic only (tur binary not found; skipped parse-check)')

    if (total_failed > 0 or total_unpaired > 0
            or total_man_failed > 0 or total_bq_fences > 0):
        sys.exit(1)
    if args.require_pairs and total_pairs == 0:
        print('error: no pairs found', file=sys.stderr)
        sys.exit(1)


if __name__ == '__main__':
    main()
