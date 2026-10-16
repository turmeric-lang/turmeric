#!/usr/bin/env python3
"""run.py -- render the CommonMark / GFM spec examples with tur/markdown.

markdown-library-plan 6.1.  The examples live in a sibling checkout of
turmeric-lang/commonmark-spec (CC-BY-SA, kept out of this repository); CI
checks it out at the commit in tests/markdown-spec/PINNED.

One Turmeric program is generated holding every example as a string literal.
It renders example k onward and prints each result between markers, so a
crash costs only the example it happened in: the run picks up after it.  The
program runs compiled (tur build), under the interpreter (tur --interpret)
or under the JIT (tur jit); the default is the first two.  Each result is compared with the expected HTML after normalisation
(below), and also byte for byte, which is reported but not ratcheted.

The ratchet (tests/markdown-spec/ratchet.txt) lists the examples that pass.
An example in it that fails on any engine run is a regression and fails the
run; an example that passes but is not in it is reported, and --update
rewrites the file to add it.  The list only grows.

  python3 tests/markdown-spec/run.py --tur build/tur --spec ../commonmark-spec
      [--engine compiled|interpret|jit|both|all] [--update] [--only c1,c2] [--show N]
"""
import argparse
import html
import html.parser
import json
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
RATCHET = os.path.join(HERE, "ratchet.txt")

# (ratchet key, path inside the spec checkout, prefix for example ids)
SPECS = [
    ("commonmark-0.31.2", "commonmark/0.31.2/spec.json", "c"),
    ("gfm-0.29-extensions", "gfm/0.29-gfm/extensions.json", "g"),
]

START = "\x02"
END = "\x03"


# ---- Normalisation ---------------------------------------------------------
# Two renderings that differ only where HTML does not care compare equal:
# attribute order, `<br />` against `<br>`, character references against the
# characters they stand for, runs of whitespace (outside <pre>), and
# whitespace next to a block-level tag.  Written for this runner; it follows
# the rules the CommonMark spec's own test runner states, not its code.

BLOCK_TAGS = frozenset("""
    address article aside blockquote body canvas caption col colgroup dd
    details div dl dt embed fieldset figcaption figure footer form h1 h2 h3
    h4 h5 h6 header hgroup hr iframe li main map nav object ol output p pre
    progress script section style table tbody td textarea tfoot th thead tr
    ul video
""".split())

WS = re.compile(r"\s+")


class _Normaliser(html.parser.HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.parts = []
        self.in_pre = 0
        self.last_block = True   # at a block boundary: drop leading space

    def _trim_tail(self):
        while self.parts and self.parts[-1].endswith((" ", "\n", "\t")):
            self.parts[-1] = self.parts[-1].rstrip()
            if not self.parts[-1]:
                self.parts.pop()

    def _tag(self, tag, attrs, close):
        if tag == "pre" and not close:
            self.in_pre += 1
        if tag in BLOCK_TAGS and not self.in_pre:
            self._trim_tail()
        if close:
            self.parts.append("</%s>" % tag)
        else:
            out = "<" + tag
            for k, v in sorted(attrs, key=lambda kv: kv[0]):
                out += " " + k
                if v is not None:
                    out += '="%s"' % html.escape(v, quote=True)
            self.parts.append(out + ">")
        if tag == "pre" and close:
            self.in_pre = max(0, self.in_pre - 1)
        self.last_block = tag in BLOCK_TAGS or tag == "br"

    def handle_starttag(self, tag, attrs):
        self._tag(tag, attrs, False)

    def handle_startendtag(self, tag, attrs):
        self._tag(tag, attrs, False)

    def handle_endtag(self, tag):
        self._tag(tag, [], True)

    def handle_data(self, data):
        if not self.in_pre:
            data = WS.sub(" ", data)
            if self.last_block:
                data = data.lstrip()
        self.parts.append(html.escape(data, quote=True))
        if data:
            self.last_block = False

    def handle_comment(self, data):
        self.parts.append("<!--%s-->" % data)
        self.last_block = False

    def handle_decl(self, decl):
        self.parts.append("<!%s>" % decl)
        self.last_block = False

    def unknown_decl(self, data):
        self.parts.append("<![%s]>" % data)
        self.last_block = False

    def handle_pi(self, data):
        self.parts.append("<?%s>" % data)
        self.last_block = False


def normalise(s):
    p = _Normaliser()
    p.feed(s)
    p.close()
    p._trim_tail()
    return "".join(p.parts)


# ---- The generated program ---------------------------------------------------

def tur_literal(s):
    """A Turmeric string literal holding exactly the bytes of s (UTF-8)."""
    out = []
    for b in s.encode("utf-8"):
        c = chr(b)
        if (c.isascii() and c.isalnum()) or c in " ,.:;-_":
            out.append(c)
        else:
            out.append("\\x%02x" % b)
    return '"' + "".join(out) + '"'


def load_examples(spec_dir):
    examples = []
    for key, rel, prefix in SPECS:
        path = os.path.join(spec_dir, rel)
        if not os.path.exists(path):
            continue
        with open(path, encoding="utf-8") as f:
            for e in json.load(f):
                examples.append({
                    "key": key,
                    "num": e["example"],
                    "id": "%s%d" % (prefix, e["example"]),
                    "markdown": e["markdown"],
                    "html": e["html"],
                    "ext": prefix == "g",
                    "section": e.get("section", ""),
                })
    return examples


def generate(examples, path):
    lines = [
        ";; GENERATED by tests/markdown-spec/run.py -- do not edit.",
        '(load "stdlib/markdown.tur")',
        ";; *args* is a cons list of cstr words; str.tur and list.tur read it.",
        '(load "stdlib/str.tur")',
        '(load "stdlib/list.tur")',
        "",
        ";; Render one example between the markers the runner reads.",
        ";; mark is the start marker line: \\x02 and the example's id.",
        "(defn md-spec-one [mark : cstr src : cstr ext : bool] : void",
        "  (let [s (string/from-cstr src)",
        "        o (if ext (markdown/default-opts) (markdown/commonmark-opts))",
        "        h (if ext (markdown/default-html-opts) (MdHtmlOpts (MdRawPass)))",
        "        d (markdown/parse s o)",
        "        r (markdown/render-html d h)]",
        "    (do",
        '      (println mark)',
        "      (println (string/to-cstr r))",
        '      (println "\\x03")',
        "      (string/release r)",
        "      (markdown/doc-free d)",
        "      (string/release s))))",
        "",
    ]
    n = len(examples)
    for i, e in enumerate(examples):
        lines.append("(defn md-spec-%d [] : void (md-spec-one %s %s %s))"
                     % (i, tur_literal(START + e["id"]), tur_literal(e["markdown"]),
                        "true" if e["ext"] else "false"))
    lines += [
        "",
        ";; Run example k and everything after it.",
        "(defn md-spec-from [k : int] : void",
        "  (do",
    ]
    for i in range(n):
        lines.append("    (when (<= k %d) (md-spec-%d))" % (i, i))
    lines += [
        "    nil))",
        "",
        "(defn main [] : int",
        "  (do",
        "    (md-spec-from (if (> (list-length *args*) 0) (cstr->parse-int (list-head *args*)) 0))",
        "    0))",
        "",
    ]
    with open(path, "w", encoding="ascii") as f:
        f.write("\n".join(lines))


def parse_output(out):
    """{id: html} for every example whose end marker was printed, and the id
    of an example that started but never ended (a crash), or None."""
    results, cur, buf = {}, None, []
    for line in out.split("\n"):
        if line.startswith(START):
            cur, buf = line[1:], []
        elif line == END and cur is not None:
            results[cur] = "\n".join(buf)
            cur = None
        elif cur is not None:
            buf.append(line)
    return results, cur


def run_engine(tur, engine, prog, workdir, n, timeout):
    """Render every example on one engine; returns {id: html} and {id: why}."""
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")
    if engine == "compiled":
        exe = os.path.join(workdir, "md-spec")
        b = subprocess.run([tur, "build", prog, "-o", exe], cwd=ROOT, env=env,
                           capture_output=True, text=True)
        if b.returncode != 0:
            sys.stderr.write(b.stdout + b.stderr)
            raise SystemExit("run-markdown-spec: tur build failed")
        cmd, sep = [exe], []
    elif engine == "jit":
        cmd, sep = [tur, "jit", prog], ["--"]
    else:
        cmd, sep = [tur, "--interpret", prog], []
    results, broken, k = {}, {}, 0
    while k < n:
        try:
            p = subprocess.run(cmd + sep + [str(k)], cwd=ROOT, env=env, timeout=timeout,
                               capture_output=True)
            out = p.stdout.decode("utf-8", "replace")
            got, crashed = parse_output(out)
            results.update(got)
            if p.returncode == 0 and crashed is None:
                break
            why = "exit %d: %s" % (p.returncode,
                                   p.stderr.decode("utf-8", "replace").strip()[-300:])
        except subprocess.TimeoutExpired as t:
            out = (t.stdout or b"").decode("utf-8", "replace")
            got, crashed = parse_output(out)
            results.update(got)
            why = "timed out (>%ds)" % timeout
        # Resume after the example that died: the one that started and never
        # ended, or else the first one after the last that finished.
        if crashed is not None:
            bad = IDS.index(crashed)
        else:
            done = [i for i in range(k, n) if IDS[i] in results]
            bad = max(done) + 1 if done else k
        if bad < n:
            broken[IDS[bad]] = why
        k = bad + 1
    return results, broken


IDS = []


def fmt_ranges(nums):
    nums = sorted(nums)
    out, i = [], 0
    while i < len(nums):
        j = i
        while j + 1 < len(nums) and nums[j + 1] == nums[j] + 1:
            j += 1
        out.append(str(nums[i]) if i == j else "%d-%d" % (nums[i], nums[j]))
        i = j + 1
    return " ".join(out)


def parse_ranges(text):
    nums = set()
    for part in text.split():
        if "-" in part:
            a, b = part.split("-")
            nums.update(range(int(a), int(b) + 1))
        elif part:
            nums.add(int(part))
    return nums


def read_ratchet():
    r = {key: set() for key, _, _ in SPECS}
    if os.path.exists(RATCHET):
        with open(RATCHET) as f:
            for line in f:
                line = line.split("#", 1)[0].strip()
                if ":" in line:
                    key, rest = line.split(":", 1)
                    r[key.strip()] = parse_ranges(rest)
    return r


def write_ratchet(r):
    with open(RATCHET, "w") as f:
        f.write("# tests/markdown-spec/ratchet.txt -- spec examples tur/markdown passes,\n"
                "# on every engine tests/run-markdown-spec.sh runs.  Maintained by\n"
                "# tests/markdown-spec/run.py --update; the list only grows.\n")
        for key, _, _ in SPECS:
            f.write("%s: %s\n" % (key, fmt_ranges(r.get(key, set()))))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tur", default=os.path.join(ROOT, "build", "tur"))
    ap.add_argument("--spec", default=os.path.join(os.path.dirname(ROOT), "commonmark-spec"))
    ap.add_argument("--engine", choices=["compiled", "interpret", "jit", "both", "all"],
                    default="both", help="both = compiled and interpret; all adds jit")
    ap.add_argument("--update", action="store_true")
    ap.add_argument("--only", default="")
    ap.add_argument("--show", type=int, default=0,
                    help="print input, expected and actual for the first N failures")
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--emit", default="",
                    help="write the generated program to this path and stop")
    args = ap.parse_args()

    examples = load_examples(args.spec)
    if not examples:
        raise SystemExit("run-markdown-spec: no examples under %s" % args.spec)
    if args.only:
        want = set(args.only.split(","))
        examples = [e for e in examples if e["id"] in want]
    IDS[:] = [e["id"] for e in examples]
    if args.emit:
        generate(examples, args.emit)
        return 0
    engines = {"both": ["compiled", "interpret"],
               "all": ["compiled", "interpret", "jit"]}.get(args.engine, [args.engine])

    with tempfile.TemporaryDirectory(prefix="md-spec-") as work:
        prog = os.path.join(work, "md-spec.tur")
        generate(examples, prog)
        per_engine = {}
        for eng in engines:
            per_engine[eng] = run_engine(args.tur, eng, prog, work, len(examples),
                                         args.timeout)

    ratchet = read_ratchet()
    passing = {key: set() for key, _, _ in SPECS}
    regressions, shown, strict = [], 0, 0
    for e in examples:
        ok_all = True
        for eng in engines:
            results, broken = per_engine[eng]
            got = results.get(e["id"])
            ok = got is not None and normalise(got) == normalise(e["html"])
            if got is not None and got == e["html"] and eng == engines[0]:
                strict += 1
            if not ok:
                ok_all = False
                if args.show and shown < args.show:
                    shown += 1
                    print("---- %s (%s, %s) ----" % (e["id"], e["section"], eng))
                    print("input:    %r" % e["markdown"])
                    print("expected: %r" % e["html"])
                    print("actual:   %r" % (got if got is not None else broken.get(e["id"], "no output")))
        if ok_all:
            passing[e["key"]].add(e["num"])
        elif e["num"] in ratchet.get(e["key"], set()):
            regressions.append(e["id"])

    total = len(examples)
    npass = sum(len(v) for v in passing.values())
    print("markdown spec: %d of %d examples pass (%s); %d byte-identical"
          % (npass, total, " + ".join(engines), strict))
    for key, _, _ in SPECS:
        new = passing[key] - ratchet.get(key, set())
        if new:
            print("  %s: %d newly passing: %s" % (key, len(new), fmt_ranges(new)))
    if args.update:
        write_ratchet({k: ratchet.get(k, set()) | passing[k] for k in passing})
        print("  ratchet updated")
    if regressions:
        print("FAIL: %d ratcheted example(s) no longer pass: %s"
              % (len(regressions), " ".join(regressions)))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
