---
title: Release Process
category: Contributor Notes
description: How Turmeric releases are cut -- the three cut-*-release commands, their shared nine-step flow, the ordering constraints that make a failed cut recoverable, and what the release workflow does once the tag is pushed.
---

# Release Process

This guide explains how a Turmeric release is cut, from the first
precondition check to the final GitHub Release. It covers the three
Claude commands that drive the flow -- `/cut-major-release`,
`/cut-minor-release`, and `/cut-patch-release` -- and the automated
release workflow that takes over once the tag is pushed.

For installing Turmeric from a release, see
[Releases and Installation](releases-and-installation-guide.md).

---

## The three commands

| Command | Version bump | When to use |
|---|---|---|
| `/cut-patch-release` | `MAJOR.MINOR.(PATCH+1)` | Bug fixes only |
| `/cut-minor-release` | `MAJOR.(MINOR+1).0` | New features, non-breaking changes |
| `/cut-major-release` | `(MAJOR+1).0.0` | Breaking changes that require user code edits |

All three share the same nine-step flow. The only differences are the
version arithmetic (Step 1), the changelog classification emphasis
(Step 2), and one extra guard in the major command that refuses to
proceed if the commits since the last tag don't actually contain
breaking changes.

The commands live in `.claude/commands/` and are run from a clean
checkout on `main`. They are designed to be safe: every destructive
operation is gated behind a precondition check and a user confirmation,
and the ordering ensures a failure at any step is recoverable without
having published a broken release.

---

## Preconditions (verified before anything changes)

Before touching any file, the command runs six checks in parallel and
reports the findings. If any check fails, it stops and asks the user to
fix the situation before proceeding.

1. **Clean working tree** (`git status --porcelain`) -- uncommitted
   changes would be swept into the release commit. The user must commit
   or stash first.
2. **On `main`** (`git rev-parse --abbrev-ref HEAD`) -- releases are
   cut from `main`, not a feature branch.
3. **Not behind origin** (`git fetch origin main` then
   `git rev-list --left-right --count origin/main...HEAD`) -- a local
   `main` that is behind origin would push a release missing commits
   that already landed.
4. **Current version** (`cat VERSION`) -- the old version, used as the
   baseline for the bump and the changelog range.
5. **Last tag matches VERSION** (`git describe --tags --abbrev=0 --match 'v*'`)
   -- the most recent release tag should be `v<VERSION>`. A mismatch
   means something is out of sync and is surfaced before proceeding.
6. **Commits since last tag** (`git log v<OLD>..HEAD --oneline`) --
   there must be at least one commit. A release with zero new commits
   is refused.

### Advisory: experiment expiries (never blocking)

The command also runs `./build/tur experiments` (or reads
`EXPERIMENTS[]` in `src/runtime/experiments.c`) and lists any
experiment whose `expires_at` is at or before the version being cut.

This is a **notice, not a precondition**. It never blocks the release.
The author decides separately whether to graduate the experiment
(delete its row; the feature becomes always-on), shelve it, or bump
`expires_at` -- in this release or a later one.

Treating this as a gate has stranded two releases. The command is
explicit about this: `expires_at` is a deadline, not an earliest date,
and there is no registry check in the preconditions. See
[experimental-flags-guide.md](experimental-flags-guide.md#expiry-policy).

---

## The nine-step flow

### Step 0: Fold in any pending `[Unreleased]` section

If `CHANGELOG.md` has a `## [Unreleased]` section, its bullets are
folded into the new release entry (keeping their `### Added` /
`### Changed` / `### Fixed` grouping), and the `[Unreleased]` heading
and any "next release must be a ..." note are deleted. The section
never survives a release.

If the note says the next release must be a minor or major bump, the
patch command stops and tells the user to run `/cut-minor-release` or
`/cut-major-release` instead. A behavior change flagged there must not
ship under a patch version.

### Step 1: Compute the new version

Read `VERSION`, parse `MAJOR.MINOR.PATCH`, and compute the new version:

- **Patch**: `MAJOR.MINOR.(PATCH+1)` -- e.g. `0.14.0` -> `0.14.1`
- **Minor**: `MAJOR.(MINOR+1).0` -- e.g. `0.12.0` -> `0.13.0`
- **Major**: `(MAJOR+1).0.0` -- e.g. `0.16.0` -> `1.0.0`

For a major bump, the command confirms with the user that the breaking
changes since `v<OLD>` actually justify a major-version bump (vs. a
minor). If they don't, it refuses and suggests `/cut-minor-release`.

### Step 2: Draft the CHANGELOG entry

Run `git log v<OLD>..HEAD --pretty=format:'%h %s'` to get the commit
list since the last tag, then classify each commit:

- **Breaking changes** (major only) -- renames, signature changes,
  removed APIs, semantic shifts requiring user code edits. This section
  leads the entry for a major release.
- **Added** -- new features, stdlib modules, CLI subcommands
- **Changed** -- non-breaking behavior changes
- **Fixed** -- bug fixes
- **Removed** -- deletions of features, modules, or APIs
- **Docs** -- documentation-only changes (only if non-trivial)
- **Internal** -- skipped (CI, refactors, dependency bumps)

When a commit subject is ambiguous, the command runs
`git show --stat <sha>` to see what files changed. Internal commits
are skipped -- the changelog audience is users, not the git log.

The entry is formatted to match the existing `CHANGELOG.md` style:

```
## [NEW] -- YYYY-MM-DD

### Breaking changes
- ...

### Added
- ...

### Changed
- ...

### Fixed
- ...
```

Empty subsections are omitted. For a major release, each Breaking
changes item includes a short before/after snippet or migration note.

### Step 3: Draft the README "Latest release" line

`README.md` line 5 has a one-sentence summary of the current release.
The command replaces it with a new sentence for the new version,
highlighting the most significant change. For a major release, this
names the most significant breaking change.

### Step 4: Confirm with the user

The command shows the user:

- The OLD -> NEW version transition
- The full drafted CHANGELOG entry
- The new README "Latest release" line
- The list of commits that informed the changelog

It then asks via `AskUserQuestion` whether to proceed, edit the
changelog, edit the README line, or cancel. Nothing is committed
without explicit confirmation.

### Step 5: Apply file changes (no git operations yet)

Six files are updated in parallel:

1. **`VERSION`** -- write the new version string.
2. **`stdlib/VERSION`** -- write the same version. `tur` reads this
   stamp to tell its own stdlib from another release's; a stale one
   makes a mismatched stdlib look like a match and makes the correct
   one warn. `ctest -R tur_stdlib_version_stamp` fails if the two
   files disagree.
3. **`src/web/wasm_glue.h`** -- update the `TURMERIC_VERSION` define.
4. **`web/public/sw.js`** -- update the `CACHE_VERSION` literal. Vite
   rewrites this token at build time, but the in-tree literal is the
   dev/no-build fallback and silently drifts a release behind if
   skipped.
5. **`CHANGELOG.md`** -- insert the new entry after the header.
6. **`README.md`** -- replace the "Latest release" line.

After applying, the command runs `git diff --stat` and shows what
changed. No commit is made yet.

### Step 6: Commit locally

```sh
git add VERSION stdlib/VERSION src/web/wasm_glue.h web/public/sw.js CHANGELOG.md README.md
git commit -m "chore: release v<NEW>

<one-paragraph summary>

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

Then create an **annotated** tag (not signed):

```sh
git tag -a "v<NEW>" -m "Release v<NEW>"
```

The tag is annotated (`-a`), not signed (`-s`). This is a recorded
decision, not an oversight. What a consumer downloads is a release
asset, and those carry
[build provenance](https://docs.github.com/actions/security-for-github-actions/using-artifact-attestations)
-- signed through Sigstore with a short-lived certificate minted from
the release job's OIDC token, binding each asset to the workflow,
repo, commit, and run that built it. That is the signature that
protects users, and it needs no key anyone has to hold or rotate. A
signed tag protects something narrower (who cut the release, to
someone reading git history) and needs a long-lived key whose loss or
absence turns every future cut into a hard failure.

The tag exists locally only. Nothing is pushed yet. If the web deploy
in the next step fails, the local tag can be deleted and the cut
retried without having published a broken release.

### Step 7: Build and deploy web

```sh
just deploy-web
```

This runs `just wasm` (which runs `just docs`), then `just web-deps`,
then `npm run build`, then `wrangler deploy` to push the web app to
Cloudflare. The user must already be authenticated with `wrangler`.

This regenerates `web/public/turmeric.{js,wasm}` and
`web/public/doc-names.json`. These are gitignored build outputs and
are **not** committed. `git status` stays clean through this step.
There is no follow-up "regenerate web artifacts" commit -- that habit
left every release tag carrying the previous release's binary.

If the deploy fails:

- Report the failure.
- Delete the local tag: `git tag -d v<NEW>`.
- Do **not** delete the commit -- the user can amend or fix forward.
- Stop. Do not proceed to the next step.

### Step 8: Push commit and tag

Only after a successful deploy:

```sh
git push origin main
git push origin "v<NEW>"
```

The tag push triggers `.github/workflows/release.yml`.

### Step 9: Verify

```sh
gh run list --workflow=release.yml --limit 1
```

The command reports the run ID and status, the new version, the bump
commit SHA, the release workflow URL, and the Cloudflare deploy
confirmation. It does not block waiting for the workflow to finish --
it takes 1-2 minutes per matrix leg.

---

## Why the ordering matters

The steps are ordered so that a failure at any point is recoverable:

```
Preconditions → file changes → commit → tag (local) → deploy web → push tag
```

- **File changes before commit**: the bump, changelog, and README are
  in one atomic commit. If something is wrong, nothing has been
  committed yet.
- **Commit before tag**: the tag points at a commit that already
  contains the version bump. The release workflow checks out the tag,
  so it sees the right sources.
- **Tag is local before deploy**: if the deploy fails, the tag can be
  deleted and the cut retried. No broken release is published.
- **Deploy before push**: a deploy failure doesn't strand a tag on the
  remote. The tag only reaches origin once the web app is live.
- **Push includes the bump commit**: the GitHub Actions release
  workflow sees the right sources because the tag points at the bump
  commit.

The awkward state this ordering exists to avoid is a cut that dies
partway through: a tag pushed to the remote with no matching release,
or a release whose web app still shows the previous version. Both
are recoverable but messy; the ordering makes them rare.

---

## What the release workflow does

The tag push triggers `.github/workflows/release.yml`, which has four
jobs:

### Build (three matrix legs)

For `linux-x86_64`, `linux-aarch64`, and `macos-arm64`:

1. Check out the tagged commit.
2. Install `libedit` (package manager differs by platform).
3. Configure and build with CMake in Release mode.
4. Run `tur --version` as a smoke test.
5. Package `bin/tur` + `lib/*.a` + `include/turi/*.h` +
   `share/turmeric/stdlib/` into a prefix-layout `tar.gz`.
6. Unpack the archive and compile a two-line program with it. `tur
   --version` proves only that the binary starts; this step proves it
   can actually compile. Several releases passed the version check
   while being unable to compile anything.
7. Upload the artifact.

### Build (Windows)

Windows is a separate job rather than a fourth matrix leg because
every step needs `shell: msys2 {0}` and the toolchain arrives through
`setup-msys2`. It does the same steps plus one Windows-specific check:
verifying the binary is self-contained. A default MinGW build can
depend on `libwinpthread-1.dll` and `edit.dll` from the MSYS2 tree,
which a user without MSYS2 on `PATH` would hit as a silent exit 127.
The job both reads the import table (`ldd`, rejecting anything
resolved inside MSYS2) and runs `tur --version` with MSYS2 stripped
from `PATH`.

### Package docs

Platform-independent, so it runs once:

1. Check out with full history (`fetch-depth: 0`) -- the doc generator
   dates each guide from the commit that added it.
2. Install doc generator dependencies with `--require-hashes`.
3. Run `genguides.py`, `genspices.py`, `gendocs.py`, and `genpack.py`
   to build the rendered docs and a docs pack.
4. Package `docs/html` and `web/public/docs-pack` into
   `turmeric-docs-v<NEW>.tar.gz` -- what `tur docs --open` reads once
   unpacked.
5. Upload the artifact.

### Release

This job runs only if all three build jobs and the docs job succeed
(not merely avoid cancellation -- checking only for `cancelled` let
failed legs through in v0.56.1 and v0.56.2, publishing partial
releases with a missing platform and no indication of it):

1. Download all artifacts.
2. Generate `sha256sums.txt`.
3. **Attest build provenance** -- signs each asset through Sigstore
   with a short-lived certificate minted from the job's OIDC token,
   binding it to the workflow, repo, commit, and run. This is placed
   after the checksums step (so `sha256sums.txt` is attested too) and
   before the release is created (so a failure here strands the tag
   rather than publishing unattested assets). A consumer verifies
   with:
   ```sh
   gh attestation verify turmeric-<tag>-<target>.tar.gz \
     --repo rjungemann/turmeric
   ```
4. Create the GitHub Release with auto-generated notes and upload all
   artifacts.

---

## Things the commands refuse to do

- Bypass any precondition without explicit user override.
- Refuse or delay a release because an experiment's `expires_at` is at
  or past the version being cut (advisory only).
- Push the tag before the deploy succeeds.
- Skip the changelog or README updates.
- Use `git push --force` for any step.
- Amend a commit that has already been pushed.
- Cut a major release when there are no actual breaking changes
  (suggests `/cut-minor-release` instead).

---

## Iterating on the workflow itself

To test changes to `release.yml` without burning real version numbers,
use a throwaway tag:

```sh
git tag v0.0.0-test1
git push origin v0.0.0-test1

gh run list --workflow=release.yml --limit 1
gh run watch <run-id>

# Clean up:
gh release delete v0.0.0-test1 --cleanup-tag --yes
git tag -d v0.0.0-test1
```

Increment the suffix (`-test2`, `-test3`, ...) per iteration so each
failed attempt's history is preserved. The workflow also supports
`workflow_dispatch` for a dry run that builds and uploads artifacts
without creating a release.

---

## See also

- [Releases and Installation](releases-and-installation-guide.md) --
  installing from a release, what's in the tarball, what works outside
  the source repo.
- [Experimental Flags](experimental-flags-guide.md) -- the experiment
  registry and expiry policy.
- `.claude/commands/cut-{major,minor,patch}-release.md` -- the
  command definitions this guide describes.
- `.github/workflows/release.yml` -- the release pipeline.
