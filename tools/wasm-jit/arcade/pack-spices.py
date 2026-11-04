#!/usr/bin/env python3
"""pack-spices.py -- the arcade's spice packages (raylib-web-playground-plan J2).

    pack-spices.py SPICES_CHECKOUT OUT_DIR NAME...
    pack-spices.py --native SPICES_CHECKOUT NAME...

One package per spice: OUT_DIR/<name>.json, holding the spice's src/ tree as
{"name", "version", "description", "deps", "files": {relpath: text}}, where
relpath is under src/ ("raylib/core.tur").  The page fetches the packages a
program needs, writes their files under /spices/<name>/src in the module's
filesystem and puts that directory on the module search path
(turi_wasm_add_include_dir) -- after the module has started, so a spice costs
a fetch when a program names it, never a relink.  spices.js beside this
script is that loader.

OUT_DIR/index.json lists every package: name, version, description, deps and
its file.  deps is the spice's non-optional :spices keys from its build.tur,
and every dep must be packed too: a package set is closed, so mounting a
spice and its deps is always enough.

Spice sources are Turmeric text; anything else under src/ is refused rather
than mangled into a string.

With --native, nothing is packed: it prints what the module has to link for
the named spices, from each one's `:build-opts` -- a line
`src NAME SOURCE INCLUDE_DIR...` per `:c-sources` file (raylib's
rl_web_loop.c, raygui's raygui_impl.c), and a line `hdr NAME HEADER` per `.h`
directly in a `:c-includes` dir, for /sysinclude (raygui.h).  Paths are
absolute.  A spice's C is code the page cannot add after start-up, so it is
linked into the module; its Turmeric sources stay packages.
"""
import json
import os
import re
import sys


def map_body(text, key):
    """The text inside `KEY #map{...}` (balanced braces), or None."""
    m = re.search(re.escape(key) + r'\s+#map\{', text)
    if not m:
        return None
    depth, i = 1, m.end()
    while i < len(text) and depth:
        c = text[i]
        if c == '"':
            i = text.index('"', i + 1)
        elif c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
        i += 1
    return text[m.end():i - 1]


def top_level_entries(body):
    """(key, entry-text) for each `"key" #map{...}` at depth 0 of body."""
    out = []
    for m in re.finditer(r'"([^"]+)"\s+#map\{', body):
        # Only keys at depth 0 of the :spices map.
        if body[:m.start()].count('{') != body[:m.start()].count('}'):
            continue
        entry = map_body(body[m.start():], '"%s"' % m.group(1))
        out.append((m.group(1), entry or ''))
    return out


def string_field(text, key):
    m = re.search(re.escape(key) + r'\s+"((?:[^"\\]|\\.)*)"', text)
    return m.group(1) if m else ''


def string_list(text, key):
    """The strings of `KEY [...]`, or []."""
    m = re.search(re.escape(key) + r'\s+\[([^\]]*)\]', text)
    return re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1)) if m else []


def native_inputs(root, name):
    """(sources, headers) of the spice's :build-opts, as absolute paths:
    sources is [(source, [include dirs])]."""
    d = os.path.join(root, 'spices', name)
    manifest = os.path.join(d, 'build.tur')
    if not os.path.isfile(manifest):
        sys.exit('pack-spices: no %s' % manifest)
    opts = map_body(open(manifest, encoding='utf-8').read(), ':build-opts') or ''
    incs = [os.path.abspath(os.path.join(d, i)) for i in string_list(opts, ':c-includes')]
    srcs = []
    for rel in string_list(opts, ':c-sources'):
        p = os.path.abspath(os.path.join(d, rel))
        if not os.path.isfile(p):
            sys.exit('pack-spices: %s: no C source %s' % (name, rel))
        srcs.append((p, incs))
    hdrs = []
    for i in incs:
        if not os.path.isdir(i):
            sys.exit('pack-spices: %s: no include dir %s' % (name, i))
        hdrs += [os.path.join(i, f) for f in sorted(os.listdir(i)) if f.endswith('.h')]
    return srcs, hdrs


def main_native(root, names):
    for n in names:
        srcs, hdrs = native_inputs(root, n)
        for src, incs in srcs:
            print(' '.join(['src', n, src] + incs))
        for h in hdrs:
            print('hdr %s %s' % (n, h))


def read_spice(root, name):
    d = os.path.join(root, 'spices', name)
    manifest = os.path.join(d, 'build.tur')
    if not os.path.isfile(manifest):
        sys.exit('pack-spices: no %s' % manifest)
    text = open(manifest, encoding='utf-8').read()
    deps = []
    body = map_body(text, ':spices')
    if body:
        for key, entry in top_level_entries(body):
            if not re.search(r':optional\s+true', entry):
                deps.append(key)
    src = os.path.join(d, 'src')
    files = {}
    for dirpath, _, names in os.walk(src):
        for f in sorted(names):
            p = os.path.join(dirpath, f)
            rel = os.path.relpath(p, src).replace(os.sep, '/')
            if not f.endswith(('.tur', '.tur.sweet')):
                sys.exit('pack-spices: %s: only Turmeric sources are packed, not %s' % (name, rel))
            files[rel] = open(p, encoding='utf-8').read()
    if not files:
        sys.exit('pack-spices: %s has no src/ files' % name)
    return {
        'name': name,
        'version': string_field(text, ':version'),
        'description': string_field(text, ':description'),
        'deps': deps,
        'files': dict(sorted(files.items())),
    }


def main(argv):
    if len(argv) >= 3 and argv[1] == '--native':
        return main_native(argv[2], argv[3:])
    if len(argv) < 4:
        sys.exit(__doc__.strip().splitlines()[2].strip())
    root, out, names = argv[1], argv[2], argv[3:]
    os.makedirs(out, exist_ok=True)
    pkgs = [read_spice(root, n) for n in names]
    packed = {p['name'] for p in pkgs}
    for p in pkgs:
        missing = [d for d in p['deps'] if d not in packed]
        if missing:
            sys.exit('pack-spices: %s needs %s, which is not packed (add it to the list)'
                     % (p['name'], ', '.join(missing)))
    index = []
    for p in pkgs:
        path = os.path.join(out, p['name'] + '.json')
        with open(path, 'w', encoding='utf-8') as f:
            json.dump(p, f, separators=(',', ':'))
        index.append({k: p[k] for k in ('name', 'version', 'description', 'deps')}
                     | {'file': p['name'] + '.json', 'files': len(p['files'])})
    with open(os.path.join(out, 'index.json'), 'w', encoding='utf-8') as f:
        json.dump({'spices': index}, f, indent=1)
        f.write('\n')
    print('pack-spices: %s (%s)' % (out, ', '.join(
        '%s %d files' % (p['name'], len(p['files'])) for p in pkgs)))


if __name__ == '__main__':
    main(sys.argv)
