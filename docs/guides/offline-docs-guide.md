---
title: Offline Docs Guide
category: CLI Tools
description: How the guides and API reference reach you without a network -- the docs pack, Try Turmeric's in-app docs browser, and the `tur doc` / `tur docs` commands
---

# Offline docs

Turmeric's documentation is available with no network, in three places, from
one artifact. This guide covers what that artifact is, how it is produced, and
how to reach the docs from each of the three.

## The short version

| You want | Use |
|---|---|
| One symbol's docstring, in a terminal | `tur doc <symbol>` |
| The rendered guides and API pages, in a terminal | `tur docs --open` (or `--serve`) |
| The rendered guides and API pages, in the browser | The **Docs** button in [Try Turmeric](https://turmeric-lang.com/try) |
| Same, on a plane | The same button. Try precaches everything on install. |

Nothing above needs a connection once the relevant piece is installed, and
none of it is opt-in.

## The docs pack

`tur run docs` produces two renderings of the same documentation:

- `docs/html/` -- the pages turmeric-lang.com serves, with site chrome.
- `web/public/docs-pack/` -- the **docs pack**: the same article bodies with
  no chrome at all, plus an `index.json` describing them.

```
web/public/docs-pack/
  index.json            version, nav tree, search strings, precache manifest
  guide.css guide.js    shared guide typography and code-block runtime
  guides/<slug>.html    article body only -- no <head>, no nav, no CSS
  api/<module>.html     same, one per stdlib module
  spices/<spice>.html   same, when ../turmeric-spices/ is present
```

Both renderings come out of the same generators, and each page's body is
rendered exactly once and wrapped twice. That is the point: the website and
Try Turmeric's in-app pane cannot drift apart, because there is no second
renderer to drift from.

The pack is a build output and is gitignored, like `turmeric.wasm` and
`doc-names.json` beside it.

### `index.json` is the contract

```jsonc
{
  "version": "0.38.0",
  "generated": "2026-08-26T01:35:38Z",
  "guides": [{ "slug": "...", "path": "...", "title": "...", "category": "...",
               "description": "...", "bytes": 15435, "words": "...",
               "added": "2026-08-26", "updated": "2026-09-21" }],
  "api":    [{ "slug": "...", "module": "...", "symbols": ["..."], ... }],
  "spices": [ ... ],
  "files":  ["guides/....html", "api/....html", "guide.css", ...]
}
```

- `words` is a lowercase search blob per page -- title, category, description,
  every heading, exported symbol names, and the opening prose. The docs pane
  searches it directly, so there is no separate search index to build or ship.
  It is capped rather than carrying full bodies: whole pages would put ~2.5 MB
  of text into a file the pane fetches on open and the service worker
  precaches, to answer searches that headings already answer well.
- `added` and `updated` are the two ends of that page's git history -- the day
  its source file was first committed, and the day it was last committed to --
  read while the pack is generated, in one traversal. The pane's **Recently
  Added** and **Recently Updated** sections are the guides' fields sorted, and they
  are here rather than in each doc's front matter so that a guide cannot forget
  to carry a date -- or keep claiming to be new because it still does. A page
  git has no commit for (an untracked draft, a build from a release tarball
  with no history) carries neither and is simply never listed as recent;
  `genpack.py` prints the coverage, so a pack built outside a checkout says so
  rather than quietly shipping an empty section. A page whose two dates are
  equal has not been touched since it arrived, and Recently Updated leaves it
  out -- Recently Added has already said what there is to say about it.
- `files` doubles as the service worker's **precache manifest**. One list, so a
  page cannot be in the pack but missing from precache.
- `version` is stamped from `VERSION`, and the pane displays it -- which makes
  "are my offline docs stale?" answerable at a glance.

### The size budget

`tools/genpack.py` fails the build if the raw pack exceeds **16 MB** (images
referenced by guides are copied in and counted). Today it is about 4.8 MB.

The budget exists to protect a decision, not to hedge one. Try Turmeric
precaches the whole pack unconditionally -- there is no "download docs for
offline" toggle -- so an over-budget pack has to be a build error here rather
than silent PWA bloat someone discovers on a phone.

**If the budget trips, shrink the pack. Never make precaching conditional.**
In descending order of preference:

1. compress or drop oversized images;
2. exclude a doc *category* from the pack -- the pack is Try's docs, and it
   does not have to carry every generated page the website carries;
3. split rarely-read reference pages into a second, lazily-fetched tier the
   pane loads on demand.

Each of those keeps "installed means complete" true for what the pack claims
to contain. A toggle does not.

## Reading docs in Try Turmeric

Open the docs browser with the **Docs** toolbar button, the `:docs` REPL
meta-command, or a `#doc=` URL. It opens *over* the REPL and never navigates:
your editor buffer, console history, and WASM session all survive a docs
session untouched.

```
:docs                    open the browser where you left it
:docs hkt-guide          jump to a guide
:docs tur-list           jump to a stdlib module's API page
```

Inside the pane:

- **Nav** is pinned at the top with the **Quickstart**, the page the pane also
  opens on when you have not been anywhere else yet. Below it, two dated lists:
  **Recently Added**, the ten newest guides in the pack, and under it **Recently
  Updated**, the ten most recently edited. Both list guides only -- they answer
  "what is new to read?", and an API module is reference the tree below already
  files -- and a guide that has not been touched
  since it landed appears only in the first. Each is collapsed on load and opens
  on click; "what changed since I last looked?" is an occasional question, and
  twenty permanent rows above the tree is a steep way to answer it. They are the
  in-app counterparts of the two cards at the top of the website's guides index
  -- same pages, same counts, same dates. Below them, the tree itself: guides by
  category, API modules by group, and spices when this build carries their
  pages.
- **Search** takes one query and returns two kinds of result: pages (from the
  pack) and symbols (from `doc-names.json`). Picking a symbol routes to the
  existing docstring panel.
- **Cross-links between guides stay in the pane.** Links out to the wider web
  open in a new tab, so the REPL session is never replaced.
- **Every runnable code block has "Load into editor"**, which drops the snippet
  in the buffer and closes the pane -- a guide is two clicks from being a
  program you are running.

### Deep links

`/try/#doc=guides/hkt-guide` restores a docs location, and
`/try/#doc=api/tur-list#map` restores it scrolled to an anchor. The key
composes with the `#code=` share hash rather than replacing it, so a link can
carry both a snippet and the page explaining it.

### Offline

The service worker precaches the pack on install, alongside the wasm. No
toggle, no opt-in, no first-run prompt, and no state in which the docs pane
exists but its content does not -- installing Try Turmeric means having the
docs, in the same way it already means having the compiler.

Two details worth knowing:

- **`/docs-pack/*` is cache-first**, not network-first. The pack is versioned
  by the service worker cache name, and `CACHE_VERSION` already rotates per
  release, so staleness is bounded by release cadence -- the same as the wasm
  -- and no page waits on a network timeout to render.
- **A partial pack is reported, not hidden.** The install is failure-tolerant
  per URL, so a flaky first load can leave the pack short. The worker records
  a `pack-status` (expected / cached / version) that the pane reads back; if
  it is short, the pane says `N of M pages cached -- reconnect to finish` and
  asks the worker to fetch the missing entries on the next online load, rather
  than waiting for a version bump.

Visiting a `/docs/html/...` page while offline now gets a small "you're
offline" page linking to the same page inside Try, instead of the REPL shell.

## Reading docs from a terminal

### `tur doc <symbol>` -- one symbol, no install needed

```sh
$ tur doc arc-new
arc-new -- allocate an Arc holding an int payload, strong count 1.

Parameters:
  v -- the value to share

Returns:
  A new Arc with strong count 1 and the weak sentinel held.

Example:
  (arc-get (arc-new 42))  ; => 42

Since: 2026-08-20
```

This works out of the box on every install: it reads the docstring table in
`<stdlib>/docstrings.tur`, which ships with the stdlib the binary already
needs. Builtins and special forms (`let`, `defn`, `match`, ...) answer from the
compiler's own table. `--json` prints `{"name": ..., "doc": ...}` for tooling.

An unknown symbol exits non-zero -- a lookup that prints nothing and succeeds
is indistinguishable from one that worked.

### `tur docs` -- the rendered guides

```sh
$ tur docs                       # where are they?
$ tur docs --open                # open them in a browser
$ tur docs --serve               # serve them from localhost:8137
$ tur docs --serve --port 9000
```

It looks for the rendered docs in, in order:

1. `$TUR_DOCS_DIR`
2. `<prefix>/share/doc/turmeric` -- installed alongside the binary
3. `<repo>/docs/html` -- a source checkout

`--serve` exists for a specific reason: some browsers refuse cross-page
navigation under `file://`, which breaks every link between guides. The server
is loopback-only, GET-only, and handles one connection at a time -- it is a way
to read your own documentation, not a web server.

With no docs installed, `tur docs` names the two ways to get them and exits
non-zero. `tur doc <symbol>` still works; it needs none of this.

### Installing the rendered docs

Each release attaches `turmeric-docs-<version>.tar.gz`, containing `docs/html/`
and the docs pack. Unpack it and point `TUR_DOCS_DIR` at the directory holding
`guides/` and `api/`:

```sh
tar -xzf turmeric-docs-v0.38.0.tar.gz
export TUR_DOCS_DIR="$PWD/turmeric-docs-v0.38.0/docs/html"
tur docs --open
```

In a source checkout, `tur run docs` writes `docs/html/` and `tur docs` finds it
with no configuration. To build the archive locally: `tur run docs-tarball`.

## Regenerating and checking the pack

```sh
tur run docs          # site HTML + docs pack, then the pack checks
tur run docs-tarball  # the above, plus turmeric-docs-<version>.tar.gz
```

`tur run docs` runs the three generators with `--emit-pack`, then
`tools/genpack.py`, which is the pass that runs once the whole pack is on disk.
It:

1. merges each generator's manifest sidecar into `index.json`;
2. rewrites cross-links into the pack's `#doc=` URL space -- only it knows what
   the finished pack contains, so only it can tell an in-pack link from a
   website one -- and reports the ones it could not resolve;
3. checks every fragment is well-formed and carries no page chrome;
4. sweeps files the manifest does not claim, so a deleted guide's fragment
   cannot linger in the pack and get precached forever;
5. enforces the size budget.

Useful flags:

```sh
python3 tools/genpack.py web/public/docs-pack/ --max-bytes 1000000   # try the budget
python3 tools/genpack.py web/public/docs-pack/ --strict-links        # fail on dead links
```

`--strict-links` is off by default because guides legitimately link to pages
the pack does not carry (plans under `docs/upcoming/`, notes under
`docs/archive/`). Those links are reported on every run.

### Dependencies

The guide and spice generators need the Python `markdown` package:

```sh
python3 -m pip install -r tools/requirements.txt
```

Without it they stop with that instruction rather than half-generating.

## Where things live

| Path | What |
|---|---|
| `tools/genguides.py` | guide markdown -> site pages + pack fragments |
| `tools/gendocs.py` | stdlib docstrings -> API pages + pack fragments + `docstrings.tur` |
| `tools/genspices.py` | sibling spice checkout -> spice pages + pack fragments |
| `tools/packlib.py` | fragment writing, search strings, pack link rewriting |
| `tools/genpack.py` | merge, check, budget |
| `tools/check-pack-spices.py` | refuse to deploy a pack with no spice pages |
| `web/public/sw.js` | precache and serve the pack offline |
| `web/main.js` | the docs pane (search `In-app docs browser`) |
| `web/tests/docs-pane.spec.js` | pane behaviour |
| `web/tests/docs-offline.spec.js` | the offline guarantee |
| `tests/run-tur-docs.sh` | `tur doc` and `tur docs` |

## Notes for contributors

**Spices are optional, and the pack tells the truth about that.** The pack's
`spices` section only lists pages this build actually produced, so a checkout
without `../turmeric-spices/` yields a pane with no spice nav rather than
entries that 404 offline. "Installed means complete" stays true for whatever
the pack claims to contain.

**A deploy is the one place that leniency is wrong.** `genspices.py` reports a
missing checkout as a warning and moves on, so `tur run docs` exits 0 having
produced no spice pages -- and a deploy of that pack drops every spice from the
site's spice pages, the offline pack and the search index without failing
anything. `tur run deploy-web` therefore runs `tools/check-pack-spices.py` over the
built pack and refuses to publish one with no spice pages. `docs` itself stays
lenient, because CI has no sibling checkout and legitimately builds a
spice-less pack.

**Finding the sibling checkout from a worktree.** `genspices.py` resolves
`../turmeric-spices` from the directory the recipe runs in. In a plain clone that is
the sibling of the checkout, which is what the path expects. A **worktree of a
bare checkout sits one level further in** -- the worktrees are subdirectories of
the bare repo -- so `../turmeric-spices` lands inside the bare repo rather than
beside it, and needs a bridge symlink there pointing at the real checkout:

```sh
ln -sfn ../turmeric-spices <bare-repo>/turmeric-spices
# verify, from inside a worktree:
python3 -c "from pathlib import Path; print(Path('../turmeric-spices').resolve())"
```

A broken bridge is quiet: the warning names the symlink's *resolved* target, so
it reads like a missing clone. `Generating docs for 0 spices` in the build log
and `spices: 0` in `index.json` are the symptoms to grep for.

**The offline spec runs against a production build on its own port.**
Playwright's `context.setOffline()` and its request routing both intercept
*before* the service worker, so under either one an offline navigation dies
before the worker can serve from cache -- neither can test this at all. The
spec therefore starts its own server and stops it mid-test. It also asserts the
*unconditional* guarantee rather than a happy path: it loads `/try/` once
online **without ever opening the docs pane**, and only then goes offline.
Visiting docs first would pass even against network-first caching, which is
exactly the behaviour the offline work replaced.

**Guide typography is emitted, not duplicated.** `guide.css` and `guide.js` in
the pack come from the same constants the site's guide pages inline. If you
change how a guide looks or how its code blocks behave, change it in
`tools/genguides.py` and both follow.
