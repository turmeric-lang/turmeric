---
description: Cut a new patch release. Bump VERSION, update CHANGELOG + README, deploy web, push tag.
argument-hint: (no arguments)
allowed-tools: Bash, Read, Edit, AskUserQuestion
---

# Cut a new patch release

Run a full patch-version release of Turmeric. Order matters: the commit
must contain the bumped version + CHANGELOG + README before the tag is
created, the web deploy must succeed before the tag is pushed (so a
deploy failure doesn't strand a tag), and the tag push must include the
bump commit so the GitHub Actions release workflow sees the right
sources.

## The release worktree -- set this up FIRST

**The cut does not run in the user's own checkout.** The repo is **bare**: its
worktrees are subdirectories of it, and the one the user works in (`personal/`)
is protected -- `permissions.deny` rules plus a `PreToolUse` hook
(`~/.claude/hooks/no-personal-worktree.sh`) refuse every read, write and shell
command that names it, in **every** permission mode, `bypassPermissions`
included.

Nor is there a worktree sitting on `main` to borrow. `personal/` is on a branch
literally named `personal`, and the local `main` branch is a leftover pointing
at the *previous* release's bump commit -- 59 commits behind `origin/main` when
this was written. So "switch to `main` and pull" is not an option, and dragging
that stale branch forward under a working tree is precisely the harm the guard
exists to prevent.

Cut from a fresh **detached** worktree at `origin/main`:

```sh
ROOT="$(git rev-parse --path-format=absolute --git-common-dir)"   # the bare repo
WT="$ROOT/release-cut"

git -C "$ROOT" fetch origin main
git -C "$ROOT" worktree add --detach "$WT" origin/main
```

Run every step below against that directory: git commands as
`git -C "$WT" ...`, the file edits in step 5 at absolute paths under `$WT`, and
`just deploy-web` in step 7 with `$WT` as its working directory. `cd "$WT"` is
fine -- it is `personal/` alone that is off limits. **Detached HEAD is correct
and expected**: the branch pointer moves on the server in step 8, never locally.

Two consequences of a *fresh* worktree, worth knowing before you trip over
them:

- **There is no `./build/tur`.** The experiment-expiry advisory below must read
  `EXPERIMENTS[]` in `src/runtime/experiments.c` rather than run the binary. It
  is advisory either way -- do not build the compiler just to satisfy it.
- **There is no `node_modules/` and no emsdk cache**, so step 7's
  `just deploy-web` pays for the full `web-deps` install and wasm build.
  `wrangler`'s credentials live in the user's home directory, so authentication
  carries over untouched.

## Preconditions (verify before doing anything destructive)

Run these in parallel and report findings before proceeding:

1. `git status --porcelain` -- working tree must be clean. A worktree
   created a moment ago is clean by construction, so anything here means
   you are pointed at the wrong directory.
2. `git worktree list` -- confirm you are in the release worktree from the
   section above and **not** in `personal/`. HEAD is detached; that is the
   intended state. Do **not** require HEAD to be on `main`, and do not
   check out or move the local `main` branch to satisfy a branch-name
   check.
3. `git rev-parse HEAD` and `git rev-parse origin/main` -- must be equal. A
   worktree created at `origin/main` straight after a fetch cannot be
   behind, so an inequality means `main` moved while you were working:
   stop, remove the worktree, and start the section above again.
4. `cat VERSION` -- current version (the old version).
5. `git describe --tags --abbrev=0 --match 'v*'` -- the most recent
   release tag. Should match `v<VERSION>`; if not, surface the mismatch
   to the user before proceeding.
6. `git log v<OLD>..HEAD --oneline` -- there must be at least one
   commit since the last tag. If zero, refuse to release.

If any check fails, stop and report. Do not proceed without the user
explicitly overriding.

### Advisory: experiment expiries (NEVER blocking)

Run `./build/tur experiments` (or read `EXPERIMENTS[]` in
`src/runtime/experiments.c`) and list any row whose `expires_at` is at or
before the version being cut.

**This is a notice, not a precondition. It NEVER blocks the release.** Report
the rows and continue with the cut. The author decides separately whether to
graduate, shelve, or bump `expires_at` -- in this release or a later one.

`expires_at` is a deadline, not an earliest date, and there is no registry
check anywhere in this file's preconditions. Do not invent one: treating this
as a gate has stranded two releases.

## Step 0: Check for a pending `[Unreleased]` section

If `CHANGELOG.md` has a `## [Unreleased]` section, read it first. If it says
the next release must be a **minor** (or major) bump, **stop** and tell the
user to run `/cut-minor-release` (or `/cut-major-release`) instead -- a
behaviour change that is flagged there must not ship under a patch version.
Otherwise fold its bullets into the entry drafted in Step 2 and delete the
`[Unreleased]` heading; the section never survives a release.

## Step 1: Compute the new version

Read `VERSION`. Parse `MAJOR.MINOR.PATCH`. Compute `NEW = MAJOR.MINOR.(PATCH+1)`.

Example: `0.14.0` -> `0.14.1`.

## Step 2: Draft the CHANGELOG entry

Run `git log v<OLD>..HEAD --pretty=format:'%h %s'` to get the commit
list since the last tag.

Classify each commit into one of:
- **Added** -- new features, new stdlib modules, new CLI subcommands
- **Changed** -- behavior changes, renames, semantic shifts in existing features
- **Fixed** -- bug fixes (commits starting with `fix:`, "fix", or referencing a bug)
- **Removed** -- deletions of features, modules, or APIs
- **Docs** -- documentation-only changes (only include if non-trivial)
- **Internal** -- skip from changelog (CI, refactors with no user-visible effect, dependency bumps)

Skim each commit's subject line and, when ambiguous, run
`git show --stat <sha>` to see what files changed. Don't include every
internal commit -- the changelog audience is users, not the git log.

Format the new entry to match the existing CHANGELOG style
(`docs/CHANGELOG.md` for reference is `CHANGELOG.md` at repo root):

```
## [NEW] -- YYYY-MM-DD

### Added
- ...

### Changed
- ...

### Fixed
- ...
```

Use today's date (`date +%Y-%m-%d`). Omit empty subsections. Aim for
3-10 bullets total -- consolidate related commits into one bullet rather
than copying every subject line. Lead each bullet with a short bolded
title where appropriate, matching the existing entries' style.

## Step 3: Draft the README "Latest release" line

`README.md` line 5 has:

```
**Latest release:** `v<OLD>` -- <one-sentence summary>.
```

Replace with:

```
**Latest release:** `v<NEW>` -- <one-sentence summary of the most significant
change in this release>.
```

Pick the one or two most significant items from the changelog and
write one sentence. Match the existing voice (terse, action-oriented).

## Step 4: Confirm with the user

Show the user:
- The OLD -> NEW version transition
- The full CHANGELOG entry you drafted
- The new README "Latest release" line
- The list of commits that informed the changelog

Use `AskUserQuestion` with options:
- **Proceed**: continue with steps 5-9 as drafted
- **Edit the changelog**: ask the user what to change, then re-show
- **Edit the README line**: ask the user for the replacement sentence
- **Cancel**: stop without making any changes

Do not proceed past this step without explicit confirmation.

## Step 5: Apply file changes (no git operations yet)

In parallel:
1. Write `NEW` to `VERSION` (no trailing newline beyond the existing format).
1b. Write `NEW` to `stdlib/VERSION` too. `tur` reads this stamp to tell its
   own stdlib from another release's; a stale one makes a mismatched stdlib
   look like a match AND makes the correct one warn. `ctest -R
   tur_stdlib_version_stamp` fails if the two files disagree.
2. Edit `src/web/wasm_glue.h` -- update the `TURMERIC_VERSION "<OLD>"`
   define to `TURMERIC_VERSION "<NEW>"`.
2b. Edit `web/public/sw.js` -- update the `CACHE_VERSION = 'tur-try-v1-<OLD>'`
   literal to `<NEW>`. Vite rewrites this token at build time, so the deployed
   worker is correct either way, but the in-tree literal is the dev/no-build
   fallback and silently drifts a release behind if you skip it.
3. Edit `CHANGELOG.md` -- insert the new entry immediately after the
   `# Changelog\n\nAll notable changes...\n` header and before the
   existing `## [<OLD>]` entry. Keep one blank line between entries.
4. Edit `README.md` line 5 -- replace the "Latest release" line.

After applying, run `git diff --stat` and show the user what changed.
Do not commit yet.

## Step 6: Commit locally

```sh
git add VERSION stdlib/VERSION src/web/wasm_glue.h web/public/sw.js CHANGELOG.md README.md
git commit -m "$(cat <<'EOF'
chore: release v<NEW>

<one-paragraph summary copied from the README "Latest release" sentence
or the changelog's most significant items>

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

Then create the annotated tag pointing at this new commit:

```sh
git tag -a "v<NEW>" -m "Release v<NEW>"
```

The tag is **annotated, not signed** (`-a`, not `-s`). That is a recorded
decision, not an oversight -- C-4 in
[docs/upcoming/security-audit-plan.md](../../docs/upcoming/security-audit-plan.md):

- What a consumer downloads is a **release asset**, and those carry
  [build provenance](https://docs.github.com/actions/security-for-github-actions/using-artifact-attestations)
  as of WP7 -- signed through Sigstore with a short-lived certificate minted
  from the release job's OIDC token, binding each asset to the workflow, repo,
  commit and run that built it. `gh attestation verify <asset> --repo
  turmeric-lang/turmeric` checks it. That is the signature that protects
  users, and it needs no key anyone has to hold or rotate.
  - **The owner is bound into the signature, so verification depends on the
    asset's vintage -- and the flag changes, not just its value.** Measured
    on v0.59.0 after the 2026-10-02 org move: `--repo rjungemann/turmeric`
    and `--repo turmeric-lang/turmeric` both return HTTP 404, because the
    attestation lives in the owning *account's* index
    (`users/rjungemann/attestations/...`) and a transfer does not move it,
    while the repo-scoped endpoint resolves through the current owner. The
    form that works for a pre-move asset is
    **`gh attestation verify <asset> --owner rjungemann`** (exit 0).
    Releases cut from v0.60.0 on use `--repo turmeric-lang/turmeric`. If a
    user reports a 404 here, it is the wrong flag, not a bad download.
- A signed **tag** protects something narrower: it proves who cut the release,
  to someone reading the git history. It needs a long-lived GPG or SSH key on
  the release machine, and a key that is lost, leaked, or simply not present
  turns every future `cut-*-release` into a hard failure at this step.

If you do want signed tags, set up a signing key first and verify it works
*before* changing anything here:

```sh
git config --global user.signingkey <key-id>   # or a path, for SSH signing
git config --global gpg.format ssh             # SSH signing only
echo test | git tag -s -m test tur-signing-probe && git tag -v tur-signing-probe
git tag -d tur-signing-probe
```

Only once that probe passes, change the `-a` above to `-s`. Do not switch it
speculatively: an unsigned release is recoverable, a cut that dies partway
through is the awkward state this file's step ordering exists to avoid.

Both the commit and the tag land on **detached HEAD**, which is why nothing
in this step names a branch. The tag exists locally only; nothing is pushed
yet. If the web deploy in step 7 fails, you can delete the local tag and try
again without having published a broken release.

## Step 7: Build and deploy web

```sh
just deploy-web
```

This runs `just wasm` (which runs `just docs`), then `just web-deps`,
then `npm run build`, then `wrangler deploy ...` to push the web app
to Cloudflare. The user must already be authenticated with `wrangler`.

This regenerates `web/public/turmeric.{js,wasm}` and `web/public/doc-names.json`.
They are **gitignored build outputs -- do NOT commit them.** `git status` stays
clean through this step; if it does not, something else changed and is worth
looking at. There is no follow-up "regenerate web artifacts" commit any more:
that habit is what left every release tag carrying the previous release's
binary, and (until v0.32.3) a doc-name index missing the release's own new
stdlib symbols.

If `just deploy-web` fails:
- Report the failure to the user.
- Run `git tag -d v<NEW>` to remove the local tag.
- Do NOT delete the commit -- the user can amend or fix forward.
- Stop. Do not proceed to step 8.

## Step 8: Push commit and tag

Only after a successful deploy:

```sh
git push origin HEAD:main
git push origin "v<NEW>"
```

`HEAD:main` because HEAD is detached: there is no local `main` at this commit
to push, and the stale local `main` must not be dragged forward just to make
`git push origin main` work. **Never** `git update-ref refs/heads/main` or
`git branch -f main` -- that repoints a branch under working trees that have
not moved, the exact failure the protected-worktree guard exists to prevent.

**The user's own checkout does not move.** Their `personal` branch, and the
stale local `main`, stay where they were until they pull. Say so when you
report, or a correctly cut release looks like it never happened locally.

The tag push triggers `.github/workflows/release.yml`, which builds
the three platform binaries (linux-x86_64, linux-aarch64, macos-arm64),
packages the rendered documentation as `turmeric-docs-v<NEW>.tar.gz`
(OD4 -- what `tur docs --open` reads once unpacked), and publishes the
GitHub Release. The docs job runs from the generators in `tools/`; it
does not rewrite `stdlib/docstrings.tur`, so it cannot dirty the tree.

## Step 9: Verify

Wait briefly and then check:

```sh
gh run list --workflow=release.yml --limit 1
```

Report the run ID and status to the user. Optionally watch the run
with `gh run watch <id>` if the user wants real-time progress, or
just tell them to follow it on the Actions page. Do not block
waiting for the release workflow to finish -- it takes 1-2 minutes
per matrix leg and the user can check on it themselves.

**On the first release cut after the 2026-10-02 org move**, verify a published
asset's attestation once the workflow finishes -- O6 item 4 of
[docs/upcoming/github-org-move-plan.md](../../docs/upcoming/github-org-move-plan.md),
the one path no test covers:

```sh
gh release download "v<NEW>" --repo turmeric-lang/turmeric -p '*macos-arm64.tar.gz'
gh attestation verify "turmeric-v<NEW>-macos-arm64.tar.gz" --repo turmeric-lang/turmeric
```

A 404 means the post-move form is `--owner turmeric-lang` instead, and
`README.md` plus all three `cut-*-release.md` files are documenting the wrong
command. Report that rather than leaving it wrong -- a verification failure
here reads to users as a compromised download.

End by reporting:
- The new version
- The commit SHA of the bump commit
- The Release workflow run URL
- The Cloudflare deploy URL or "deployed" confirmation
- A reminder that the release page will populate with tarballs once
  the workflow finishes (link to releases page)
- That their own checkout is unchanged -- `main` moved on the server only,
  and they need to pull to see it

## Step 10: Remove the release worktree

Step 7 leaves roughly 800 MB of `node_modules/`, emsdk output and wasm build
products in the worktree, and nothing needs it once the tag is pushed:

```sh
ROOT="$(git rev-parse --path-format=absolute --git-common-dir)"
git -C "$ROOT" worktree remove --force "$ROOT/release-cut"
```

`--force` because those build outputs are untracked, and from outside the
worktree so git is not deleting the directory it is standing in. Do this
**after** step 9 has reported: if the release workflow fails and a corrected
tag has to go out, the worktree is where that happens.

## Things to refuse

- Refuse to bypass any precondition without explicit user override.
- **Never** refuse or delay a release because an experiment's `expires_at` is
  at or past the version being cut. That is advisory (see the Advisory section
  above); surface it and proceed.
- Refuse to push the tag before the deploy succeeds.
- Refuse to skip the changelog/README updates -- they're load-bearing
  for users discovering the release.
- Refuse to use `git push --force` for any step here.
- Refuse to amend a commit that has already been pushed.
- Refuse to run any part of the cut inside the user's `personal/` worktree,
  and refuse to work around the hook that blocks it.
- Refuse to move a local branch to make a push look like a fast-forward
  (`git update-ref refs/heads/main`, `git branch -f main`, `git checkout main
  && git reset --hard origin/main`). Push `HEAD:main` from detached HEAD.
