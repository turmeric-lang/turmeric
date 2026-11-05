#!/usr/bin/env python3
"""doc-names.py -- the doc table the arcade's hover falls back to.

raylib-web-playground-plan P5, editor item "hover docs for builtins". Try
Turmeric's language client shows a builtin's or a library function's one-line
summary when the server has no hover for it (web/lsp-client.js
docFallbackHover), from web/public/doc-names.json. That file is the whole
stdlib plus every spice in turmeric-spices, 1.2 MB. The arcade wants the
stdlib and only the spices its engine packages, so this writes that subset,
in the same shape -- [{name, summary, kind, [spice]}], sorted by name, the
stdlib winning a collision -- from the same parser (tools/gendocs.py), and
build.sh puts it beside the module as doc-names.json.

    python3 tools/wasm-jit/arcade/doc-names.py TUR_SPICES OUT.json SPICE...
"""
import contextlib
import io
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'tools'))
import gendocs  # noqa: E402


def parse_tree(src: Path) -> list:
    """Every .tur file under src, parsed and named as gendocs names them."""
    modules = []
    for f in gendocs.collect_tur_files(src):
        try:
            module = gendocs.parse_tur_file(f)
        except Exception as e:  # gendocs warns and goes on; so does this
            print(f'doc-names.py: warning: could not parse {f}: {e}', file=sys.stderr)
            continue
        rel = Path(f).resolve().relative_to(src.resolve()).parts
        module['subdir'] = rel[0] if len(rel) > 1 else None
        gendocs.apply_path_fallback_name(module, rel)
        modules.append(module)
    return modules


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    spices_root, out = Path(sys.argv[1]), Path(sys.argv[2])
    with contextlib.redirect_stdout(io.StringIO()):   # gendocs narrates
        entries = gendocs.collect_doc_entries(parse_tree(ROOT / 'stdlib'))
        seen = {e['name'] for e in entries}
        for name in sys.argv[3:]:
            src = spices_root / 'spices' / name / 'src'
            if not src.is_dir():
                print(f'doc-names.py: no {src}', file=sys.stderr)
                return 1
            for e in gendocs.collect_doc_entries(parse_tree(src), spice=name):
                if e['name'] not in seen:
                    seen.add(e['name'])
                    entries.append(e)
    entries.sort(key=lambda e: e['name'].lower())
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(entries, ensure_ascii=True, separators=(',', ':')), encoding='utf-8')
    print(f'doc-names.py: {out} ({len(entries)} names)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
