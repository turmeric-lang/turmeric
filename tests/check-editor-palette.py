#!/usr/bin/env python3
"""check-editor-palette.py -- the editor packs carry the palette.

web/editor-palette.json is the one written-down copy of the token colors.
Every editor theme (Vim, Emacs, VS Code) and the Monaco theme in web/main.js
must contain each of its colors.  The editor packs live in their own repos, so
this script takes explicit files and does not assume a compiler checkout:

    check-editor-palette.py PALETTE.json dark=FILE [dark=FILE ...] light=FILE ...
    check-editor-palette.py PALETTE.json --main-js web/main.js [dark=FILE ...]

A color is "carried" when its #RRGGBB hex appears in the file (any case).
--main-js also checks that the Monaco rules map each token to the palette
color, so the palette cannot drift from the web theme either.
"""
import json
import re
import sys

# palette key -> Monaco rule token
MONACO_TOKENS = {
    "comment": "comment",
    "string": "string",
    "number": "number",
    "keyword": "keyword",
    "keywordOperator": "keyword.operator",
    "annotation": "annotation",
    "type": "type",
    "operator": "operator",
    "delimiter": "delimiter",
    "foreground": "identifier",
}


def monaco_block(src, name):
    """Return the text of MONACO_THEMES.<name>: { ... } from main.js."""
    start = src.index("const MONACO_THEMES")
    m = re.compile(r"^    %s: \{" % name, re.M).search(src, start)
    if not m:
        raise SystemExit("main.js: no MONACO_THEMES.%s block" % name)
    nxt = re.compile(r"^    (dark|light): \{|^\};", re.M).search(src, m.end())
    return src[m.end():nxt.start() if nxt else len(src)]


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    palette = json.load(open(argv[1]))
    main_js = None
    targets = []
    rest = argv[2:]
    while rest:
        a = rest.pop(0)
        if a == "--main-js":
            main_js = rest.pop(0)
        elif "=" in a and a.split("=", 1)[0] in ("dark", "light"):
            targets.append(tuple(a.split("=", 1)))
        else:
            print("unrecognized argument: " + a)
            return 2

    bad = []
    for theme, path in targets:
        text = open(path, encoding="utf-8").read().lower()
        for key, hexv in palette[theme].items():
            if key.startswith("_"):
                continue
            if "#" + hexv.lower() not in text and hexv.lower() not in text:
                bad.append("%s: %s theme is missing %s (#%s)" % (path, theme, key, hexv))

    if main_js:
        src = open(main_js, encoding="utf-8").read()
        for theme in ("dark", "light"):
            block = monaco_block(src, theme)
            for key, tok in MONACO_TOKENS.items():
                m = re.search(r"token:\s*'%s'\s*,\s*foreground:\s*'([0-9A-Fa-f]{6})'" % re.escape(tok), block)
                if not m:
                    bad.append("main.js: %s theme has no '%s' rule" % (theme, tok))
                elif m.group(1).lower() != palette[theme][key].lower():
                    bad.append("main.js: %s '%s' is %s, palette says %s"
                               % (theme, tok, m.group(1), palette[theme][key]))
            for key, color in (("background", "editor.background"), ("foreground", "editor.foreground")):
                m = re.search(r"'%s'\s*:\s*'#([0-9A-Fa-f]{6})'" % re.escape(color), block)
                if not m or m.group(1).lower() != palette[theme][key].lower():
                    bad.append("main.js: %s %s is %s, palette says %s"
                               % (theme, color, m.group(1) if m else None, palette[theme][key]))

    if bad:
        for b in bad:
            print("FAIL " + b)
        return 1
    print("PASS check-editor-palette (%d theme file(s)%s)"
          % (len(targets), " + main.js" if main_js else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
