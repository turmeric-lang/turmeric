# `tur.lock` records `:resolved` but checks out `:ref`

**Severity:** medium -- the lockfile detects drift now, but still cannot pin
against it. A `:ref` naming a branch means every fresh fetch takes whatever
that branch points at, and the consumer is asked to approve the change rather
than being held to the commit they locked.

**Status:** **RESOLVED 2026-10-01.** The detection half shipped as C-3 of
[security-audit-plan](https://github.com/turmeric-lang/turmeric/blob/main/docs/upcoming/security-audit-plan.md);
the pinning half landed with the fix direction below, both of its hazards
handled, and the `--frozen` flag it recommended pairing with it.

- `pkg_git_fetch_pinned` (`src/compiler/pkg.c`) clones from `:ref` as before,
  then moves to the lock's `:resolved`: a depth-1 fetch of the bare SHA first,
  and when the server refuses that (no `uploadpack.allowReachableSHA1InWant`
  -- a plain `git daemon`, or the file:// remote the test uses), the cloned
  branch's full history, looked up there.  **Neither step falls back to the
  branch tip.**  A commit that cannot be had is an error that names
  `tur fetch --update`, and the clone is removed so a later run cannot read
  the tip as the pinned tree.  The pin is applied only when the lock row's
  `:url` and `:ref` still match the manifest's (an edited `:ref` is a request
  for something else) and `--update` was not passed; a `:resolved` that is not
  a 40/64-hex commit id is refused before it reaches a git command line.
- `tur fetch --frozen` fetches exactly what the lock pins and never writes it:
  a dependency with no row, or a `:url` / `:ref` / commit / tree that differs,
  fails with "tur.lock would change".  `--frozen --update` is a usage error.
- `tests/run-spice-fetch.sh`: case 11 now asserts that a fresh fetch after the
  branch moved checks out the PINNED tree (the upstream `backdoor` module is
  absent) and leaves `:resolved` / `:sha256` alone -- it used to assert the
  fetch failed the integrity check; 11b rewrites and garbage-collects the
  upstream history and asserts the fetch fails and keeps no clone; 16-18 cover
  `--frozen`.  21 passed, 0 failed.
- The consuming-spices and security guides no longer say `:resolved` is
  recorded but unused.

## Repro

```sh
# A consumer pinned to a BRANCH -- the common case, and what `tur add` writes
# when no --ref is given.
cat build.tur
#   :spices #map{ "widget" #map{:url "https://..." :ref "main"} }

tur fetch                     # pins :resolved "3e56f6a..." and a tree hash
grep resolved tur.lock        # :resolved "3e56f6a5d20ba23da41d0fd1d08a706cef3f6e58"

# Upstream pushes to main. Then, on a fresh clone:
rm -rf spices/ && tur fetch
#   spice: integrity check FAILED for 'widget'     <- C-3, this is new
#   ... tur fetch --update

tur fetch --update            # the only way forward
grep resolved tur.lock        # a DIFFERENT commit; the recorded one is gone
```

There is no way to say "give me the commit the lockfile recorded". The
recorded commit is the one thing `tur fetch` will not fetch.

## Root cause

`pkg_git_fetch(it->url, it->ref, dest)` (`src/compiler/pkg.c`, in
`pkg_fetch_all`) is passed the **manifest's** `:ref`, never the lock row's
`:resolved`. The clone it builds is:

```c
buf_puts(&cmd, "git clone --depth 1 ");
if (ref) { buf_puts(&cmd, "--branch "); pkg_cmd_arg(&cmd, ref); }
```

`git clone --branch` takes a branch or tag name and cannot take a commit SHA,
so checking out `:resolved` is not a parameter change -- it needs a second
step after the clone.

Grepping `le->resolved` across the tree finds it used only to print
(`pkg.c` "using cached", `main.c`'s `tur audit` listing) and in
`pkg_cmake_verify_lock`, which compares it for **cmake** deps but never for
spices. So the field is recorded, displayed, and -- for spices -- otherwise
inert.

## Fix direction

Give `pkg_git_fetch` a `pinned_sha` parameter, used when the lock has one and
`--update` was not passed. The existing already-cloned branch is nearly the
shape needed:

```c
git -C <dest> fetch --depth 1 origin -- <sha> && git -C <dest> checkout <sha>
```

Two things to get right, and they are why this was not landed with the rest of
C-3:

- **Fetching a bare SHA is not universal.** GitHub, GitLab and modern
  self-hosted git enable `uploadpack.allowReachableSHA1InWant`, but a plain
  `git daemon` or an older server does not, and the fetch fails outright.
- **The obvious fallback defeats the point.** Retrying with `:ref` when the
  SHA fetch fails silently converts a pin back into branch-tracking -- the
  behaviour this change exists to remove -- and it would do so on exactly the
  hosts least likely to be trustworthy. So the fallback has to be a hard error
  with a diagnostic, or an explicit opt-out flag, not an automatic retry.

Worth pairing with a `tur fetch --frozen` (the `npm ci` / `cargo --locked`
shape): fail if anything would change the lockfile at all. That is the flag CI
should be running, and it is a smaller change than the SHA checkout.

## Test

`tests/run-lock-integrity.sh` already stands up a local `file://` upstream and
walks the pin/drift/update cycle; a case asserting "fetch checks out the
recorded commit even after the branch has moved" drops straight into it.

## Guide upkeep

`docs/guides/consuming-spices-guide.md` says, accurately, that "a clone tracks
the branch or tag in `:ref`; the `:resolved` commit is recorded but not used to
check out", and advises using tags over branches. That sentence is the one to
delete when this is fixed.
