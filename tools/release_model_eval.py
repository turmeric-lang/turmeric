#!/usr/bin/env python3
"""release_model_eval.py -- replay past releases through release_prepare's drafter.

For each past release vOLD..vNEW and each model spec, draft the CHANGELOG
entry and README line exactly as `release_prepare.py` would have at vOLD
(CHANGELOG and README taken from the vOLD tag, so the hand-cut answer is not
in the prompt), with that model alone, and write a Markdown report that puts
every draft beside the hand-cut entry.  This is how the "Model choice" table
in docs/archive/ci-release-workflows-plan.md is measured.

Usage:
  tools/release_model_eval.py --models zai-glm-latest@low,mistral-large-4 \\
      [--release 0.62.0:0.63.0 ...] [--repeat N] [--out DIR]
  tools/release_model_eval.py --list-models
  tools/release_model_eval.py --style-history
  tools/release_model_eval.py --merge A/results.json B/results.json [--out DIR]

Environment:
  MISTRAL_API_KEY   required (or --key-file FILE: a bare key, or a dotenv
                    file such as ~/.vibe/.env)
  MISTRAL_BASE_URL  default https://api.mistral.ai/v1
"""

import argparse
import json
import os
import re
import statistics
import subprocess
import sys
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import release_prepare as rp  # noqa: E402

# The two releases the 2026-10-05 measurement used, plus a recent one.
DEFAULT_RELEASES = ["0.61.0:0.62.0", "0.62.0:0.63.0", "0.66.0:0.67.0"]


def at_tag(tag, path):
    return subprocess.run(["git", "show", f"{tag}:{path}"], cwd=rp.ROOT,
                          check=True, capture_output=True, text=True).stdout


def level_of(old, new):
    o, n = [tuple(int(x) for x in v.split(".")) for v in (old, new)]
    return "major" if n[0] != o[0] else "minor" if n[1] != o[1] else "patch"


def list_models(base, key):
    req = urllib.request.Request(base.rstrip("/") + "/models",
                                 headers={"Authorization": f"Bearer {key}"})
    with urllib.request.urlopen(req, timeout=60) as resp:
        data = json.load(resp)
    for m in sorted(data.get("data", []), key=lambda m: m["id"]):
        aliases = ", ".join(a for a in m.get("aliases", []) if a != m["id"])
        print(m["id"] + (f"  (aliases: {aliases})" if aliases else ""))


def run_one(spec, old, new):
    _, unreleased, entries = rp.split_changelog(at_tag(f"v{old}", "CHANGELOG.md"))
    m = rp.README_RE.search(at_tag(f"v{old}", "README.md"))
    commits = [c for c in rp.commit_log(f"v{old}..v{new}")
               if not rp.is_release_commit(c)]
    log = []

    class Log:
        def write(self, s):
            log.append(s)

        def flush(self):
            pass

    t0 = time.time()
    notes = rp.draft_notes([spec], old, new, level_of(old, new), entries[:2],
                           m.group(0) if m else "", unreleased, commits, log=Log())
    return {"spec": spec, "release": f"{old}..{new}", "commits": len(commits),
            "seconds": round(time.time() - t0, 1), "ok": notes["model"] is not None,
            "attempts": notes["attempts"], "changelog": notes["changelog"],
            "readme": notes["readme"], "log": "".join(log).strip()}


def hand_cut(new):
    _, _, entries = rp.split_changelog(at_tag(f"v{new}", "CHANGELOG.md"))
    m = rp.README_RE.search(at_tag(f"v{new}", "README.md"))
    return entries[0].strip(), m.group(0) if m else ""


def drafted_by(new):
    """Who wrote vNEW's published entry: the release-cut workflow's model, or
    a person (anything before the workflow's first cut, v0.63.1)."""
    author = subprocess.run(["git", "log", "-1", "--format=%an", f"v{new}"],
                            cwd=rp.ROOT, capture_output=True, text=True).stdout
    return "workflow (GLM draft)" if "github-actions" in author else "hand-cut"


def style(cl, rd):
    """Shape of one entry -- what "the house voice" is made of, in numbers."""
    cl = re.sub(r"^## \[[^\]]*\][^\n]*\n+", "", cl.strip())
    bullets = [b.split("\n###")[0] for b in re.split(r"(?m)^- ", cl)[1:]]
    words = sorted(len(b.split()) for b in bullets) or [0]
    titles = sorted(len(m.group(1).split()) for b in bullets
                    for m in [re.match(r"\*\*(.*?)\*\*", b.strip(), re.S)] if m) or [0]
    ticks = sum(b.count("`") // 2 for b in bullets)
    return {"bullets": len(bullets),
            "words_med": statistics.median(words), "words_max": words[-1],
            "title_med": statistics.median(titles),
            "ticks_per_bullet": round(ticks / max(len(bullets), 1), 1),
            "sections": "/".join(h[4:7] for h in re.findall(r"^### \w+", cl, re.M)),
            "readme_words": len(rd.split())}


STYLE_COLS = ["bullets", "words_med", "words_max", "title_med",
              "ticks_per_bullet", "sections", "readme_words"]
STYLE_HEAD = ("| Bullets | Words/bullet (median) | Words/bullet (max) | Title words "
              "| `code` per bullet | Sections | README words |")


def style_row(label, s):
    return f"| {label} | " + " | ".join(str(s[c]) for c in STYLE_COLS) + " |"


def style_history(first="0.55.0"):
    """Every published entry from `first` on, labelled hand-cut or GLM."""
    text = at_tag("origin/main", "CHANGELOG.md")
    _, _, entries = rp.split_changelog(text)
    out = ["| Release | Drafted by |" + STYLE_HEAD[1:],
           "|---|---|" + "---|" * len(STYLE_COLS)]
    lo = tuple(int(x) for x in first.split("."))
    for e in reversed(entries):
        v = re.match(r"## \[([\d.]+)\]", e)
        if not v or tuple(int(x) for x in v.group(1).split(".")) < lo:
            continue
        try:
            rd = rp.README_RE.search(at_tag(f"v{v.group(1)}", "README.md")).group(0)
        except (subprocess.CalledProcessError, AttributeError):
            rd = ""
        out.append(style_row(f"{v.group(1)} | {drafted_by(v.group(1))}", style(e, rd)))
    return "\n".join(out)


def report(results, releases):
    out = ["# Release-notes model eval", ""]
    out += ["| Model | Release | Commits | Time | Result |", "|---|---|---|---|---|"]
    for r in results:
        rounds = sum(1 for _, e in r["attempts"] if e.startswith("round"))
        verdict = ("pass" + (f" after {rounds} rejection(s)" if rounds else "")
                   if r["ok"] else "FAIL: " + "; ".join(e for _, e in r["attempts"])[:200])
        out.append(f"| `{r['spec']}` | {r['release']} | {r['commits']} | "
                   f"{r['seconds']} s | {verdict} |")
    out += ["", "## Style", "",
            "Same release, same model, two runs should look alike; and both "
            "should look like the published entry.", "",
            "| Release | Draft |" + STYLE_HEAD[1:],
            "|---|---|" + "---|" * len(STYLE_COLS)]
    for old, new in releases:
        cl, rd = hand_cut(new)
        out.append(style_row(f"{old}..{new} | published ({drafted_by(new)})",
                             style(cl, rd)))
        for r in results:
            if r["release"] == f"{old}..{new}" and r["ok"]:
                out.append(style_row(f"{old}..{new} | `{r['spec']}`",
                                     style(r["changelog"], r["readme"])))
    for old, new in releases:
        cl, rd = hand_cut(new)
        out += ["", f"## v{old}..v{new}", "",
                f"### Published ({drafted_by(new)})", "", cl, "", f"README: {rd}"]
        for r in results:
            if r["release"] == f"{old}..{new}":
                out += ["", f"### `{r['spec']}` ({r['seconds']} s, "
                        f"{'pass' if r['ok'] else 'FAIL -- fallback draft'})", "",
                        r["changelog"], "", f"README: {r['readme']}"]
    return "\n".join(out) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--models", default="zai-glm-latest@low,mistral-large-4")
    ap.add_argument("--release", action="append",
                    help="OLD:NEW, repeatable (default: %s)" % " ".join(DEFAULT_RELEASES))
    ap.add_argument("--repeat", type=int, default=1)
    ap.add_argument("--out", default="build/release-model-eval")
    ap.add_argument("--key-file")
    ap.add_argument("--list-models", action="store_true")
    ap.add_argument("--style-history", action="store_true",
                    help="print the shape of every published entry and exit")
    ap.add_argument("--merge", nargs="+", metavar="RESULTS_JSON",
                    help="rebuild --out/report.md from earlier runs' results.json")
    args = ap.parse_args()
    releases = [tuple(r.split(":")) for r in (args.release or DEFAULT_RELEASES)]

    if args.style_history:
        print(style_history())
        return
    if args.merge:
        results = []
        for path in args.merge:
            with open(path) as f:
                results += json.load(f)
        results.sort(key=lambda r: r["spec"])
        os.makedirs(args.out, exist_ok=True)
        with open(os.path.join(args.out, "report.md"), "w") as f:
            f.write(report(results, releases))
        print(os.path.join(args.out, "report.md"))
        return

    if args.key_file:
        # A bare key, or a dotenv file with a MISTRAL_API_KEY= line
        # (Mistral Vibe keeps it in ~/.vibe/.env).
        with open(os.path.expanduser(args.key_file)) as f:
            text = f.read().strip()
        for line in text.splitlines():
            name, eq, val = line.partition("=")
            if eq and name.strip().removeprefix("export ") == "MISTRAL_API_KEY":
                text = val.strip().strip("'\"")
        os.environ["MISTRAL_API_KEY"] = text
    key = os.environ.get("MISTRAL_API_KEY", "")
    if not key:
        raise SystemExit("MISTRAL_API_KEY is not set (or pass --key-file)")
    base = os.environ.get("MISTRAL_BASE_URL", "https://api.mistral.ai/v1")
    if args.list_models:
        list_models(base, key)
        return

    specs = [s.strip() for s in args.models.split(",") if s.strip()]
    os.makedirs(args.out, exist_ok=True)
    results = []
    for old, new in releases:
        for spec in specs:
            for _ in range(args.repeat):
                r = run_one(spec, old, new)
                print(f"{spec:28} {old}..{new}  {r['seconds']:6.1f} s  "
                      f"{'pass' if r['ok'] else 'FAIL'}", file=sys.stderr)
                results.append(r)
                # Saved as it goes: a run is many minutes of API calls.
                with open(os.path.join(args.out, "results.json"), "w") as f:
                    json.dump(results, f, indent=2)
    with open(os.path.join(args.out, "report.md"), "w") as f:
        f.write(report(results, releases))
    print(os.path.join(args.out, "report.md"))


if __name__ == "__main__":
    main()
