#!/usr/bin/env python3
"""release_prepare.py -- bump the version and draft the release notes.

Steps 0-5 of the `/cut-*-release` skills, non-interactively, for
`.github/workflows/release-cut.yml`: fold `[Unreleased]`, compute NEW, draft
the CHANGELOG entry and the README "Latest release" line, write the version
stamps.  It stops there: no commit, no tag, no deploy.

The CHANGELOG entry and README sentence are drafted by an LLM over the Mistral
API (OpenAI-compatible chat completions).  Models are tried in order; the first
whose answer passes validation wins.  Nobody reads the draft before it ships,
so the validators are the gate: with --strict, a run where no model passes
exits 2 having written nothing.  Without it, a rough subject-line draft is
written instead -- a deliberate choice for a provider outage, never a silent
fallback.

Standard library only: the workflow runs this before any pip install.

Usage:
  tools/release_prepare.py --bump patch|minor|major [--apply] [--out DIR]

  --apply        write VERSION, stdlib/VERSION, wasm_glue.h, sw.js,
                 CHANGELOG.md, README.md (default: print the draft only)
  --out DIR      write new_version, pr_body.md, draft.json into DIR
  --strict       exit 2, writing nothing, if no model produced a valid draft
  --range A..B   commit range to draft from (default vOLD..HEAD); with
                 --old, lets the drafter be evaluated against a past release

Environment:
  MISTRAL_API_KEY   API key; absent -> rough draft, or exit 2 with --strict
  MISTRAL_BASE_URL  default https://api.mistral.ai/v1
  RELEASE_MODELS    comma-separated model order (default: DEFAULT_MODELS)
"""

import argparse
import datetime
import json
import os
import re
import subprocess
import sys
import textwrap
import time
import urllib.error
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Measured on v0.62.0..v0.63.0 and v0.61.0..v0.62.0 against the hand-cut
# entries (docs/upcoming/ci-release-workflows-plan.md, "Model choice").
DEFAULT_MODELS = ["zai-glm-latest@low", "mistral-medium-latest", "mistral-small-latest"]

SECTIONS = ["Added", "Changed", "Deprecated", "Removed", "Fixed", "Docs"]

# Per-commit body cap.  Commit bodies here are long and explanatory; the first
# paragraph or two carries the user-visible claim, the rest is mechanism.
BODY_LINES = 12

# The hand-cut entries run 20-70 words a bullet.  Past this, the model is
# narrating the implementation, which is what the commit bodies are full of.
MAX_BULLET_WORDS = 110


def sh(*args):
    return subprocess.run(args, cwd=ROOT, check=True, capture_output=True,
                          text=True).stdout


def read(rel):
    with open(os.path.join(ROOT, rel), encoding="utf-8") as f:
        return f.read()


def write(rel, text):
    with open(os.path.join(ROOT, rel), "w", encoding="utf-8") as f:
        f.write(text)


def bump_version(old, level):
    major, minor, patch = (int(x) for x in old.split("."))
    if level == "major":
        return f"{major + 1}.0.0"
    if level == "minor":
        return f"{major}.{minor + 1}.0"
    return f"{major}.{minor}.{patch + 1}"


# --- CHANGELOG structure ----------------------------------------------------

ENTRY_RE = re.compile(r"^## \[", re.M)


def split_changelog(text):
    """-> (header, unreleased_body_or_None, [entry, ...]) with entries verbatim."""
    starts = [m.start() for m in ENTRY_RE.finditer(text)]
    if not starts:
        raise SystemExit("CHANGELOG.md has no '## [' entries")
    header = text[:starts[0]]
    chunks = [text[a:b] for a, b in zip(starts, starts[1:] + [len(text)])]
    unreleased = None
    if chunks and chunks[0].startswith("## [Unreleased]"):
        unreleased = chunks.pop(0).split("\n", 1)[1].strip() if "\n" in chunks[0] else ""
    return header, unreleased, chunks


def unreleased_demands(unreleased):
    """The bump level an [Unreleased] note insists on, or None."""
    if not unreleased:
        return None
    low = unreleased.lower()
    for level in ("major", "minor"):
        if re.search(rf"\b(must|needs to|has to) be an? {level}\b", low):
            return level
    return None


# --- commit log -------------------------------------------------------------

def commit_log(rev_range):
    raw = sh("git", "log", "--no-merges", "--reverse",
             "--pretty=format:%x1e%h%x1f%s%x1f%b", rev_range)
    commits = []
    for rec in raw.split("\x1e"):
        if not rec.strip():
            continue
        sha, subj, body = (rec.split("\x1f") + ["", ""])[:3]
        body_lines = [l for l in body.strip().splitlines()
                      if not l.startswith(("Co-Authored-By:", "Signed-off-by:"))]
        if len(body_lines) > BODY_LINES:
            body_lines = body_lines[:BODY_LINES] + ["[...]"]
        commits.append({"sha": sha.strip(), "subject": subj.strip(),
                        "body": "\n".join(body_lines).strip()})
    return commits


def is_release_commit(c):
    return re.match(r"chore: (release|bump version to) v\d", c["subject"]) is not None


def render_commits(commits):
    out = []
    for c in commits:
        out.append(f"### {c['sha']} {c['subject']}")
        if c["body"]:
            out.append(c["body"])
        out.append("")
    return "\n".join(out)


# --- the LLM draft ----------------------------------------------------------

SYSTEM_PROMPT = """\
You write release notes for Turmeric, a statically typed Lisp that compiles to C.
The audience is USERS of the language, not its contributors.

Given the commits since the last release, write:

1. changelog: the body of one CHANGELOG.md entry, in Markdown.
   - Only these `### ` subsections, in this order, each omitted when empty:
     Added, Changed, Deprecated, Removed, Fixed, Docs.
     (Added = new features, stdlib modules, CLI subcommands, guides.
      Changed = behavior changes, renames, semantic shifts.
      Fixed = bug fixes.  Removed = deleted features or APIs.
      Docs = only non-trivial documentation; new guides usually go in Added.)
   - Each bullet starts `- **Short bold title.**` followed by one to three
     sentences -- under 60 words -- in the voice of the examples: terse,
     concrete, naming the actual functions, flags and forms in backticks.
   - Say WHAT changed for a user, not HOW it was implemented: no internal
     function names, data structures, measurements of the fix, plan names
     or stage labels (P0, T3, RM3, ...), fixture or test names.
   - Consolidate: one bullet per user-visible theme, never one per commit.
     Aim for 3-10 bullets in total; a release with few commits may need 1-3.
   - A breaking change is a `Changed` bullet whose title begins `BREAKING --`.
   - Leave out anything a user cannot observe: CI, test harness, fixture
     snapshots, timeouts, refactors, plan/report bookkeeping, dependency
     bumps, release chores.
   - Never invent a feature, name or number that the commits do not state.
   - Wrap lines at about 80 columns.  ASCII only: write `--`, never an em dash.
     U.S. spelling: "behavior", "honors", "initialize".
   - Do NOT include the `## [version] -- date` heading.

2. readme: ONE sentence for README's "Latest release" line, naming the one or
   two most significant changes.  Terse and action-oriented, like the example.
   No version number and no "Release:" / "Patch release:" lead-in -- start
   with the change itself.  Ends with a period, ASCII only.

Names: the browser REPL at turmeric-lang.com is "Try Turmeric"; the compiler
is `tur`; the interpreter is `turi`; packages are "spices".

Reply with a JSON object and nothing else: {"changelog": "...", "readme": "..."}
"""


def build_user_prompt(old, new, level, examples, readme_line, unreleased, commits):
    parts = [f"Release: v{old} -> v{new} ({level} bump).", ""]
    parts.append("## Style examples: the two most recent CHANGELOG entries\n")
    parts.extend(e.rstrip() + "\n" for e in examples)
    parts.append("## The current README line (for its voice)\n")
    parts.append(readme_line + "\n")
    if unreleased:
        parts.append("## Pending [Unreleased] notes -- fold every bullet in, keeping its subsection\n")
        parts.append(unreleased + "\n")
    parts.append(f"## Commits since v{old} ({len(commits)}, oldest first)\n")
    parts.append(render_commits(commits))
    return "\n".join(parts)


def chat(base_url, key, spec, messages, timeout=900):
    """`spec` is MODEL or MODEL@EFFORT.  Accepted efforts differ per model
    (measured 2026-10-05): mistral-medium / -small take none|high (default
    none), zai-glm takes low|high|max and reasons by default."""
    model, _, effort = spec.partition("@")
    body = {"model": model,
            "temperature": 0.2,
            "response_format": {"type": "json_object"},
            "messages": messages}
    if effort:
        body["reasoning_effort"] = effort
    req = urllib.request.Request(
        base_url.rstrip("/") + "/chat/completions",
        data=json.dumps(body).encode(),
        headers={"Authorization": f"Bearer {key}",
                 "Content-Type": "application/json",
                 "Accept": "application/json"})
    last = None
    for attempt in range(4):
        try:
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                data = json.load(resp)
            content = data["choices"][0]["message"]["content"]
            # Reasoning models answer with typed chunks ("thinking", "text");
            # only the text chunks are the reply.
            if isinstance(content, list):
                content = "".join(ch.get("text", "") for ch in content
                                  if isinstance(ch, dict)
                                  and ch.get("type") == "text")
            return content
        except urllib.error.HTTPError as e:
            last = f"HTTP {e.code}: {e.read()[:300]!r}"
            if e.code not in (429, 500, 502, 503, 504):
                break
        except (urllib.error.URLError, TimeoutError, KeyError, IndexError) as e:
            last = repr(e)
        time.sleep(5 * (attempt + 1))
    raise RuntimeError(last)


ASCII_FIX = {"\u2014": "--", "\u2013": "--", "\u2018": "'", "\u2019": "'",
             "\u201c": '"', "\u201d": '"', "\u2026": "...", "\u2192": "->",
             "\u00a0": " ", "\u2011": "-"}


def asciify(s):
    for k, v in ASCII_FIX.items():
        s = s.replace(k, v)
    return s


def strip_fences(s):
    s = s.strip()
    m = re.match(r"^```(?:json)?\s*(.*?)\s*```$", s, re.S)
    return m.group(1) if m else s


def validate(draft, evidence=""):
    """-> (changelog, readme) normalised, or raise ValueError.

    `evidence` is the text the draft was written from; a diagnostic code the
    draft cites must appear in it or in the tree, or the model made it up."""
    if not isinstance(draft, dict):
        raise ValueError("not a JSON object")
    cl, rd = draft.get("changelog"), draft.get("readme")
    if not isinstance(cl, str) or not isinstance(rd, str):
        raise ValueError("missing 'changelog' or 'readme' string")
    cl = asciify(cl).strip()
    rd = " ".join(asciify(rd).split())
    # A model that echoes the heading anyway: drop it rather than fail.
    cl = re.sub(r"^## \[[^\]]*\][^\n]*\n+", "", cl)
    heads = re.findall(r"^#+ .*$", cl, re.M)
    if not heads:
        raise ValueError("changelog has no ### subsections")
    order = []
    for h in heads:
        m = re.fullmatch(r"### (\w+)", h.strip())
        if not m or m.group(1) not in SECTIONS:
            raise ValueError(f"unexpected heading {h!r}")
        order.append(SECTIONS.index(m.group(1)))
    if order != sorted(order) or len(set(order)) != len(order):
        raise ValueError(f"subsections out of order: {heads}")
    bullets = re.findall(r"^- ", cl, re.M)
    if not bullets:
        raise ValueError("changelog has no bullets")
    if len(bullets) > 16:
        raise ValueError(f"{len(bullets)} bullets -- not consolidated")
    for b in re.split(r"(?m)^- ", cl)[1:]:
        words = len(b.split("\n###")[0].split())
        if words > MAX_BULLET_WORDS:
            title = re.match(r"\*\*(.*?)\*\*", b.strip())
            raise ValueError(f"bullet {title.group(1) if title else b[:40]!r} "
                             f"is {words} words; keep each under 60")
    starts = [b.strip()[:60] for b in re.split(r"(?m)^- ", cl)[1:]]
    if len(starts) != len(set(starts)):
        raise ValueError("a bullet is repeated")
    for code in sorted(set(re.findall(r"TUR-[EW]\d{4}", cl + rd))):
        if code not in evidence and not code_in_tree(code):
            raise ValueError(f"{code} appears in neither the commits nor the tree")
    if not rd or len(rd) > 400 or "\n" in rd:
        raise ValueError("readme is not one short sentence")
    rd = re.sub(r"^\**((latest|patch|minor|major) )?release:\**\s*", "", rd,
                flags=re.I)
    rd = re.sub(r"^`?v?\d+\.\d+\.\d+`?\s*--\s*", "", rd).lstrip("- ")
    if not rd.endswith("."):
        rd += "."
    nonascii = sorted({c for c in cl + rd if ord(c) > 127})
    if nonascii:
        raise ValueError(f"non-ASCII characters: {nonascii}")
    return reflow(cl), rd


def code_in_tree(code):
    return subprocess.run(["git", "grep", "-q", "-F", code, "--", "src"],
                          cwd=ROOT).returncode == 0


def reflow(cl):
    """Re-lay the entry out the way CHANGELOG.md is written: a blank line
    around each heading, none between bullets, bullets wrapped at 80 columns
    with a two-space continuation.  Models vary on all three; the file
    should not."""
    out = []
    for block in re.split(r"(?m)^(?=### |- )", cl):
        block = block.strip()
        if not block:
            continue
        if block.startswith("### "):
            out.append(("h", block.splitlines()[0]))
            rest = "\n".join(block.splitlines()[1:]).strip()
            if rest:
                out.append(("p", rest))
        else:
            out.append(("b", " ".join(block[2:].split())))
    lines = []
    for kind, text in out:
        if kind == "h":
            if lines:
                lines.append("")
            lines += [text, ""]
        elif kind == "b":
            lines += textwrap.wrap(text, 80, initial_indent="- ",
                                   subsequent_indent="  ",
                                   break_on_hyphens=False, break_long_words=False)
        else:
            lines += [text, ""]
    return "\n".join(lines).strip()


def fallback_draft(commits, unreleased):
    """The plan's original rough categorizer: subject lines, bucketed."""
    buckets = {"Added": [], "Changed": [], "Fixed": []}
    for c in commits:
        s = c["subject"]
        if re.match(r"(?i)(feat|add)\b", s):
            buckets["Added"].append(s)
        elif re.match(r"(?i)fix", s):
            buckets["Fixed"].append(s)
        else:
            buckets["Changed"].append(s)
    out = []
    if unreleased:
        out.append(unreleased.strip())
    for name, items in buckets.items():
        if items:
            out.append(f"### {name}\n\n" + "\n".join(f"- {asciify(i)}" for i in items))
    return ("\n\n".join(out),
            "TODO -- one sentence naming the most significant change.")


def draft_notes(models, old, new, level, examples, readme_line, unreleased,
                commits, log=sys.stderr):
    key = os.environ.get("MISTRAL_API_KEY", "")
    base = os.environ.get("MISTRAL_BASE_URL", "https://api.mistral.ai/v1")
    user = build_user_prompt(old, new, level, examples, readme_line,
                             unreleased, commits)
    attempts = []
    if not key:
        attempts.append(("(none)", "MISTRAL_API_KEY is not set"))
    else:
        for model in models:
            t0 = time.time()
            messages = [{"role": "system", "content": SYSTEM_PROMPT},
                        {"role": "user", "content": user}]
            # One correction round: a draft that fails validation goes back
            # with the reason.  Over-splitting a large release is the usual
            # failure, and the model fixes it when told the count.
            for round_ in (1, 2):
                try:
                    raw = chat(base, key, model, messages)
                except RuntimeError as e:
                    print(f"release_prepare: {model} failed: {e}", file=log)
                    attempts.append((model, str(e)))
                    break
                try:
                    cl, rd = validate(json.loads(strip_fences(raw)), user)
                except (ValueError, json.JSONDecodeError) as e:
                    print(f"release_prepare: {model} round {round_}: {e}",
                          file=log)
                    attempts.append((model, f"round {round_}: {e}"))
                    messages += [{"role": "assistant", "content": raw},
                                 {"role": "user", "content":
                                  f"That draft was rejected: {e}. Fix it -- "
                                  "at most 10 bullets, consolidated by theme, each under 60 words -- "
                                  "and reply with the corrected JSON object only."}]
                    continue
                print(f"release_prepare: {model} ok in {time.time() - t0:.0f}s",
                      file=log)
                return {"model": model, "changelog": cl, "readme": rd,
                        "attempts": attempts}
    cl, rd = fallback_draft(commits, unreleased)
    return {"model": None, "changelog": cl, "readme": rd, "attempts": attempts}


# --- applying it ------------------------------------------------------------

README_RE = re.compile(r"^\*\*Latest release:\*\* `v[^`]*` -- .*$", re.M)


def apply(old, new, date, notes, header, entries):
    write("VERSION", new)
    write("stdlib/VERSION", new)

    glue = read("src/web/wasm_glue.h")
    glue2 = re.sub(r'(TURMERIC_VERSION ")[^"]*(")', rf"\g<1>{new}\g<2>", glue)
    if glue2 == glue:
        raise SystemExit("src/web/wasm_glue.h: TURMERIC_VERSION not found")
    write("src/web/wasm_glue.h", glue2)

    # Matched by shape, not by OLD, so a bump re-syncs a drifted literal --
    # but only on the CACHE_VERSION line: the file's comments quote old
    # versions as history.
    sw = read("web/public/sw.js")
    sw2, n = re.subn(r"(CACHE_VERSION = 'tur-try-v1-)\d+\.\d+\.\d+",
                     rf"\g<1>{new}", sw)
    if n != 1:
        raise SystemExit("web/public/sw.js: CACHE_VERSION literal not found")
    write("web/public/sw.js", sw2)

    entry = f"## [{new}] -- {date}\n\n{notes['changelog'].strip()}\n\n"
    write("CHANGELOG.md", header + entry + "".join(entries))

    readme = read("README.md")
    line = f"**Latest release:** `v{new}` -- {notes['readme']}"
    readme2, n = README_RE.subn(lambda _m: line, readme, count=1)
    if n != 1:
        raise SystemExit("README.md: no '**Latest release:**' line")
    write("README.md", readme2)


def pr_body(old, new, level, notes, commits):
    lines = [f"Release **v{new}** ({level} bump from v{old}), cut by "
             "`release-cut.yml`. This PR is opened and merged by the workflow "
             "once Try Turmeric has deployed; it exists because `main` takes "
             "changes only through a pull request.", ""]
    if notes["model"]:
        lines.append(f"The CHANGELOG entry and README line were drafted by "
                     f"`{notes['model']}` from the {len(commits)} commits below "
                     "and passed the validators. Prose fixes are a follow-up "
                     "commit to `CHANGELOG.md` (and the release body) -- they "
                     "never need a retag.")
    else:
        lines.append("> [!WARNING]\n> Cut with `allow_rough_notes`: no model "
                     "drafted the notes, so the CHANGELOG entry is the raw commit "
                     "subjects and the README line is a TODO. Rewrite both in a "
                     "follow-up commit.")
    if notes["attempts"]:
        lines += ["", "<details><summary>Rejected drafts and failed calls</summary>", ""]
        lines += [f"- `{m}`: {e}" for m, e in notes["attempts"]]
        lines += ["", "</details>"]
    lines += ["", f"<details><summary>Commits since v{old}</summary>", ""]
    lines += [f"- {c['sha']} {c['subject']}" for c in commits]
    lines += ["", "</details>", ""]
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--bump", choices=["patch", "minor", "major"], required=True)
    ap.add_argument("--apply", action="store_true")
    ap.add_argument("--out")
    ap.add_argument("--old", help="override the old version (default: VERSION)")
    ap.add_argument("--range", dest="rev_range",
                    help="commit range (default vOLD..HEAD)")
    ap.add_argument("--changelog", default="CHANGELOG.md",
                    help="CHANGELOG to take style examples from")
    ap.add_argument("--models", default=os.environ.get("RELEASE_MODELS", ""))
    ap.add_argument("--date", default=datetime.date.today().isoformat())
    ap.add_argument("--strict", action="store_true",
                    help="exit 2, writing nothing, unless a model's draft passed")
    args = ap.parse_args()

    old = args.old or read("VERSION").strip()
    new = bump_version(old, args.bump)
    models = [m.strip() for m in args.models.split(",") if m.strip()] or DEFAULT_MODELS

    header, unreleased, entries = split_changelog(read(args.changelog))
    demanded = unreleased_demands(unreleased)
    order = ["patch", "minor", "major"]
    if demanded and order.index(demanded) > order.index(args.bump):
        raise SystemExit(f"CHANGELOG [Unreleased] says the next release must be "
                         f"a {demanded} bump; refusing a {args.bump} bump.")

    rev_range = args.rev_range or f"v{old}..HEAD"
    commits = [c for c in commit_log(rev_range) if not is_release_commit(c)]
    if not commits:
        raise SystemExit(f"no commits in {rev_range}; nothing to release")

    readme_m = README_RE.search(read("README.md"))
    readme_line = readme_m.group(0) if readme_m else ""
    notes = draft_notes(models, old, new, args.bump, entries[:2], readme_line,
                        unreleased, commits)

    failed = args.strict and not notes["model"]
    if args.apply and not failed:
        apply(old, new, args.date, notes, header, entries)
    if args.out:
        os.makedirs(args.out, exist_ok=True)
        with open(os.path.join(args.out, "new_version"), "w") as f:
            f.write(new)
        with open(os.path.join(args.out, "pr_body.md"), "w") as f:
            f.write(pr_body(old, new, args.bump, notes, commits))
        with open(os.path.join(args.out, "draft.json"), "w") as f:
            json.dump(notes, f, indent=2)
    print(f"## [{new}] -- {args.date}\n\n{notes['changelog']}\n\n"
          f"README: {notes['readme']}\n\n(model: {notes['model']})")
    if failed:
        print("release_prepare: no model produced a valid draft; nothing written "
              "(--strict).  Attempts:", file=sys.stderr)
        for m, e in notes["attempts"]:
            print(f"  {m}: {e}", file=sys.stderr)
        sys.exit(2)


if __name__ == "__main__":
    main()
