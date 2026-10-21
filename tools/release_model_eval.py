#!/usr/bin/env python3
"""release_model_eval.py -- replay past releases through release_prepare's drafter.

For each past release vOLD..vNEW and each model spec, draft the CHANGELOG
entry and README line exactly as `release_prepare.py` would have at vOLD
(CHANGELOG and README taken from the vOLD tag, so the hand-cut answer is not
in the prompt), with that model alone, and write a Markdown report that puts
every draft beside the hand-cut entry.  This is how the "Model choice" table
in docs/upcoming/ci-release-workflows-plan.md is measured.

Usage:
  tools/release_model_eval.py --models zai-glm-latest@low,mistral-large-4 \\
      [--release 0.62.0:0.63.0 ...] [--repeat N] [--out DIR]
  tools/release_model_eval.py --list-models

Environment:
  MISTRAL_API_KEY   required (or --key-file)
  MISTRAL_BASE_URL  default https://api.mistral.ai/v1
"""

import argparse
import json
import os
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


def report(results, releases):
    out = ["# Release-notes model eval", ""]
    out += ["| Model | Release | Commits | Time | Result |", "|---|---|---|---|---|"]
    for r in results:
        rounds = sum(1 for _, e in r["attempts"] if e.startswith("round"))
        verdict = ("pass" + (f" after {rounds} rejection(s)" if rounds else "")
                   if r["ok"] else "FAIL: " + "; ".join(e for _, e in r["attempts"])[:200])
        out.append(f"| `{r['spec']}` | {r['release']} | {r['commits']} | "
                   f"{r['seconds']} s | {verdict} |")
    for old, new in releases:
        cl, rd = hand_cut(new)
        out += ["", f"## v{old}..v{new}", "", "### Hand-cut", "", cl, "",
                f"README: {rd}"]
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
    args = ap.parse_args()

    if args.key_file:
        with open(os.path.expanduser(args.key_file)) as f:
            os.environ["MISTRAL_API_KEY"] = f.read().strip()
    key = os.environ.get("MISTRAL_API_KEY", "")
    if not key:
        raise SystemExit("MISTRAL_API_KEY is not set (or pass --key-file)")
    base = os.environ.get("MISTRAL_BASE_URL", "https://api.mistral.ai/v1")
    if args.list_models:
        list_models(base, key)
        return

    releases = [tuple(r.split(":")) for r in (args.release or DEFAULT_RELEASES)]
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
    with open(os.path.join(args.out, "results.json"), "w") as f:
        json.dump(results, f, indent=2)
    with open(os.path.join(args.out, "report.md"), "w") as f:
        f.write(report(results, releases))
    print(os.path.join(args.out, "report.md"))


if __name__ == "__main__":
    main()
