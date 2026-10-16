# Extras to break out into their own repos

> Status: proposed (2026-10-16). Nothing moved. This is the audit and the
> sequencing; each extraction is its own small PR plus a matching PR in the
> new repo.

## Why

`turmeric-lang/turmeric` is the compiler, runtime, stdlib, the test suite and
the Try Turmeric site. A handful of top-level directories ride along with it
but share none of its build, release cadence or contributors' expertise:
editor packs, the version manager, a Homebrew formula, a cross-language
benchmark suite and an AWS CDK app. Moving them out shrinks the repo a
contributor has to read, lets each piece release on its own clock (a Vim
plugin fix should not wait on a compiler tag), and makes conventional
installs possible (plugin managers and `brew tap` both want a repo whose root
*is* the plugin/tap).

The org already did this for spices (`turmeric-lang/turmeric-spices`) and the
SMT-LIB corpus, so the pattern and the tooling for it exist.

## Audit

Measured on `main` @ `0da34b6b1`. "Coupling" is what in *this* repo reads the
directory; it is the work an extraction has to carry.

| Directory | Files | What it is | Verdict |
|---|---|---|---|
| `vim-syntax/` | 4 | Vim/Neovim syntax + colorschemes | **Extract** (editors) |
| `editors/vim-turmeric/` | 4 | A second, older Vim pack (104-line syntax vs 152) | **Merge into the above, then extract** |
| `emacs/` | 3 | `turmeric-mode` + light/dark themes | **Extract** (editors) |
| `vscode-syntax-ext/` | 14 | VS Code syntax/theme extension (`turmeric-syntax` 0.2.0, publisher `fith-lang`) | **Extract** (editors) |
| `editors/vscode-turmeric/` | 5 | VS Code *debug* extension (`turmeric-debug` 0.1.0) with its own copy of the grammar | **Merge with the above, then extract** |
| `tvm/` | 5 | Version manager (`tvm.sh`, `install.sh`, tests) | **Extract**, with care (below) |
| `Formula/` | 1 | Homebrew formula (HEAD build) | **Extract** to `homebrew-turmeric` |
| `performance-comparison/` | 213 | C/Rust/Haskell/Clojure/Racket/Python vs Turmeric | **Extract** |
| `infra/bench-box/` | 19 | AWS CDK app that runs the benchmarks | **Extract with the above** |
| `benchmarks/` | 164 | Turmeric-only micro benchmarks + `audit/` corpus | **Keep** (nightly CI runs `benchmarks/audit/`) |
| `tutorials/` | 3 | YAML read by Try Turmeric and a ctest | Keep (data for `web/`) |
| `examples/` | 32 | Example programs, gated by baselines in CMake | Keep for now |
| `validation/` | 3 | Python Fibonacci cross-check | Keep, tiny; revisit |
| `.devcontainer/` | 2 | Must live in the repo it builds | Keep |
| `external/mir/` | 99 | Vendored MIR (`VENDORED.md`) | Keep |
| `web/` | 86 | Try Turmeric, the marketing site, the worker | Keep: wasm build, docs pack and CI page are all coupled to the compiler |
| `docs/archive/` | ~23 MB | Resolved reports and plans | Keep; it is the paper trail |
| `mobile-try.png` (root) | 1 | Stray screenshot at the repo root | Move under `docs/` or delete |

Not worth extracting: `tests/fixtures/` (100 MB, but it *is* the compiler's
test suite), `src/`, `stdlib/`, `tools/`.

## Per-extraction notes

### 1. Editors (`turmeric-lang/editors`, or one repo per editor)

Two problems come before any move:

- **Duplicates.** There are two Vim packs (`vim-syntax/`,
  `editors/vim-turmeric/`) and two VS Code extensions
  (`vscode-syntax-ext/`, `editors/vscode-turmeric/`), each pair with a
  different copy of the grammar. Pick the survivor of each pair in this repo
  first (the newer grammar wins; `vim-syntax` and `vscode-syntax-ext` are the
  ones the guides point at), fold the other's extras in (the debug-adapter
  wiring from `vscode-turmeric`, the filetype plugin from `vim-turmeric`),
  and only then move.
- **Repo shape.** Vim plugin managers and MELPA want the plugin at the repo
  root, so a monorepo is a poor fit for those two. Recommendation: **one
  monorepo `turmeric-lang/editors`** for the first cut (cheapest, one place
  for the shared token palette), and split `vim-turmeric` / `turmeric-mode`
  out only when someone wants to publish to a plugin index. The monorepo can
  also carry thin top-level `vim-turmeric`-style subtree mirrors later.

Coupling to carry:

- `tests/run-editor-syntax.sh` (ctest `tur_editor_syntax`) loads the Vim pack
  against sample `.tur` files and structure-checks the TextMate grammar. It
  moves with the packs. The samples it uses from this repo must be vendored
  or fetched; the test must not need a compiler checkout.
- **Palette sync.** `web/main.js` (~line 2266) says the editor themes carry
  the same token colors as the web theme, so a color change there is a change
  in three editor packs. That is a cross-repo contract after the move. Make
  it checkable: export the palette from one file in this repo (a small JSON
  under `web/`), and have the editors repo's CI diff its themes against that
  file at a pinned compiler tag. Do not leave it as a comment.
- Docs: `docs/guides/vim-guide.md`, `vscode-guide.md`, `lsp-guide.md`,
  `debugging-guide.md`, `security-guide.md` give `cp -r vim-syntax/...` and
  `cd vscode-syntax-ext` instructions. Rewrite them to `git clone` the new
  repo (and prefer `Plug 'turmeric-lang/editors', {'rtp': 'vim'}` style
  lines). Guides link by GitHub URL, so the links must point at the new repo.
- Smaller refs: `CODEOWNERS`, `tools/ci/collect-loc.py` (LOC buckets),
  `docs/upcoming/github-org-move-plan.md` and `security-audit-plan.md`.

### 2. tvm (`turmeric-lang/tvm`)

tvm is the advertised install path: `turmeric-lang.com/install` bootstraps
tvm and then a checksum-verified release. So this is the extraction that can
break first-time installs; do it last of the "easy" ones and behind a
compatibility period.

What does **not** need to change: tvm fetches *release assets* and the
release API from `TVM_GH_REPO` (default `turmeric-lang/turmeric`, one
function in `tvm.sh`). Those stay in the compiler repo, because releases are
cut there. Moving the scripts does not move the releases.

What does:

- **The bootstrap pins tvm to the release tag.** The `/install` handler in
  `web/worker.js` downloads `tvm/tvm.sh` and `tvm/install.sh` from
  `raw.githubusercontent.com/<repo>/<TAG>/tvm/`. After the move that path
  does not exist in the compiler repo at any new tag. Fix by having
  `release.yml` **attach `tvm.sh` and `install.sh` (and their checksums) as
  release assets**, fetched from the tvm repo at a pinned tag recorded in the
  release workflow. The worker then downloads them from the release, which
  keeps today's property: the installer a user runs is the one that was
  tested with that compiler release.
- **Old tags.** `/install` for a release cut before the move would look for
  `tvm/` at that tag and still find it (git history is not rewritten), so old
  tags keep working. Only tags after the move need the asset.
- **CI.** `.github/workflows/ci.yml` has a `tvm` job (matrix of OSes,
  `sh tvm/tests/run.sh`, `sh tvm/tests/install-script.sh`). It moves to the
  tvm repo. Keep a *thin* job here that runs the tvm repo's install test
  against a freshly built compiler release candidate, so a compiler change
  that breaks the install path is still caught before a tag. `CMakeLists.txt`
  has a ctest hook for `tests/run-install.sh` (that one tests `tur install`,
  not tvm, and stays).
- **Docs and links:** `README.md`, `web/index.html` (the Install step links
  `tvm/README.md`), `docs/guides/tvm-guide.md`, `quickstart.md`,
  `releases-and-installation-guide.md`, `mise-asdf-guide.md`,
  `introducing-saffron.md`, the worker's own error text (it prints
  `.../blob/main/tvm/README.md`), `src/compiler/pkg.c`'s install hint (that
  one is a `turmeric-lang.com/install.sh` URL and is unaffected).
- `CODEOWNERS` entry for `/tvm/`; the `tvm` OS matrix mention in `ci.yml`
  comments.

### 3. Homebrew tap (`turmeric-lang/homebrew-turmeric`)

`Formula/turmeric.rb` is HEAD-only (`brew install --HEAD`). The README
currently has to say "the tap URL is not optional" because there is no
`homebrew-turmeric` repo, so `brew tap turmeric-lang/turmeric` cannot resolve
by name. A dedicated tap repo fixes that and removes the long tap-URL
incantation. The formula keeps building from `turmeric-lang/turmeric` `main`,
so nothing about releases changes. Update the README, `releases-and-installation-guide.md`
and `mise-asdf-guide.md`, `CODEOWNERS`, and the `Formula/` mention in
`CLAUDE.md`. Keep a one-release note in the README for people who tapped by
URL.

### 4. Benchmarks suite and bench box (`turmeric-lang/benchmarks`)

`performance-comparison/` (213 files, nine language columns) and
`infra/bench-box/` (CDK app that runs it on a dedicated instance) are the
largest non-compiler payload and need toolchains (JVM, Rust, GHC, Racket) no
other part of the repo does.

- Keep `benchmarks/` here: the nightly arm64 job runs
  `benchmarks/audit/run-corpus.py` and publishes the series the `/ci` page
  charts, so it is part of the compiler's regression signal.
- Coupling to carry: `Justfile` (`PERF := "performance-comparison"` and the
  `bench-box-*` recipes), `docs/guides/performance-guide.md`,
  `docs/notes/benchmark-runtime-setup.md`, `docs/design/aws-bench-box-design.md`,
  `benchmarks/audit/corpus.txt` and `merge-sweeps.py` (which read
  `performance-comparison/`), `.gitignore`, and `tools/ci/collect-loc.py`.
- The bench box clones and builds Turmeric on the instance; it needs only a
  ref to build, so it has no hard dependency on living in-tree.
- `Justfile` recipes become thin wrappers that clone the benchmarks repo, or
  are dropped in favor of running `just` in the new repo.

## Order

Smallest blast radius first; each step is independently shippable and
reversible until the in-tree copy is deleted.

1. **Dedupe in place** (no new repo): choose the surviving Vim pack and VS
   Code extension, merge the duplicates, delete the losers, update guides.
   Also move `mobile-try.png` out of the root.
2. **Homebrew tap**: create `homebrew-turmeric`, copy `Formula/`, update the
   README, delete `Formula/` after one release.
3. **Editors**: create `turmeric-lang/editors` with history
   (`git filter-repo --path ... ` on a scratch clone, so blame survives),
   add the palette-sync check, move the test, rewrite the guides, delete the
   in-tree copies.
4. **Benchmarks + bench box**: same recipe; `Justfile` and docs updated.
5. **tvm**: last, because it touches the install path. Land the
   release-asset change (`release.yml`, `web/worker.js`) and cut a release
   with it *before* deleting `tvm/` from this repo; verify
   `curl -fsSL turmeric-lang.com/install | sh` end to end on macOS and Linux
   from a clean `$HOME`; only then remove `tvm/` and the in-tree CI job.

## Common mechanics for every extraction

- Preserve history with `git filter-repo --path <dir>` on a throwaway clone;
  do not hand-copy files.
- Leave a stub `README` pointing at the new repo in the old location for one
  release, then delete it.
- Sweep references by anchored pattern (path, not bare name), and run
  `tests/check-docs-lint.sh`; do not do a blind find-and-replace.
- New repos get `CODEOWNERS`, a license, and CI that does not need a
  compiler checkout unless it genuinely tests against one.
- Each extraction is a normal PR against `main` that branches from `main`;
  no stacking. The new repo's first PR is separate.

## Open questions

- One `editors` repo or one repo per editor, if MELPA / a Vim plugin index is
  a near-term goal?
- Is `examples/` (32 files, gated by CMake baselines) worth moving to a
  `turmeric-examples` repo, or does the baseline check make it compiler
  territory? Left in place by this plan.
- `docs/archive/` is 23 MB of mostly text. History, not payload; no action
  proposed.
