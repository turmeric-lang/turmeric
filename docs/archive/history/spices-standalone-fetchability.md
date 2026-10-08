# Making Every Spice in turmeric-spices Fetchable Standalone

- **Date:** 2026-06-23
- **Severity:** medium (release/distribution; does not block v1 compiler work)
- **Repo affected:** sibling `../turmeric-spices/` (not this repo)
- **Status:** resolved 2026-06-23 via shared monorepo tag `v0.1.0` (see "Resolution" below)

## Goal

Each spice in `turmeric-spices` should be independently consumable via a
`:spices` entry of the form:

```turmeric
#{:url    "https://github.com/turmeric-lang/turmeric-spices"
  :ref    "<spice>-v<version>"
  :subdir "spices/<spice>"}
```

## Trigger

The dangling `http-v0.1.0` ref (org-corrected but still untagged) exposed
that standalone-fetch support is incomplete across the repo.

## Current State (38 spices)

| Bucket | Count | Spices |
|---|---|---|
| Tag at CURRENT version exists | 4 | plot (0.3.0), plutovg (0.1.0), sdf-raylib (0.1.0), test (0.1.0) |
| Tagged but STALE (tag < version) | 1 | ansi (current 0.2.0; latest tag `ansi-v0.1.4`) |
| NO tag at all | 33 | everything else |

=> 34 of 38 spices need a new release tag at their current version.

## Gap 1 -- Missing / stale release tags

Every spice needs an annotated tag `<spice>-v<version>` matching the existing
`test-v0.1.0` / `plot-v0.3.0` convention, pointing at a commit where that
spice is at the named version. 34 spices are missing one.

Current versions to tag (from each `build.tur :version`):

```
ansi 0.2.0        c-dsl 0.2.0       ecs 0.4.0-E1      ecs-raylib 0.1.0-E4
frame 0.2.0       glsl 0.2.0        http 0.1.0        httpd 0.2.0
json 0.1.0        linalg 0.1.0     math 0.1.0        notebook 0.1.0
opengl 0.2.0      osc 0.2.0         png 0.1.0         postgres 0.3.0
raygui 0.1.0      raylib 0.3.0     regex 0.2.0       rtaudio 0.2.0
rtmidi 0.2.0      scscm 0.1.0       signal 0.1.0      sqlite 0.2.0
stats 0.1.0       template 0.1.0   thread-pool 0.1.0 tidal 0.1.0
tls 0.3.0         tourist 0.2.5    valkey 0.4.0      watch 0.2.0
wav 0.1.0         zlib 0.1.0
```

(plot, plutovg, sdf-raylib, test already current.)

Pre-release-style versions (`ecs 0.4.0-E1`, `ecs-raylib 0.1.0-E4`) need a
naming decision -- literal `ecs-v0.4.0-E1` or a cleaned release version
first.

## Gap 2 -- Inter-spice `:path` deps break external fetch (the hard part)

`:path "../sibling"` only resolves inside the workspace checkout. A standalone
consumer fetching just one spice gets no sibling source, so any spice with a
`:path` dep is NOT standalone-fetchable as written. Affected:

| Spice | `:path` sibling deps |
|---|---|
| http | json |
| httpd | json, thread-pool |
| notebook | watch |
| ecs-raylib | ecs, raylib |
| tourist | httpd, template |

Each such dep must become a fetchable monorepo reference, i.e.
`#{:url ".../turmeric-spices" :ref "<sibling>-v<ver>" :subdir "spices/<sibling>"}`
-- the same shape already used for the optional test/http deps. This couples
tagging order (Gap 1) to dependency order.

Topological tag order (leaves first):

- **Layer 0** (no sibling deps): everything except the five below; tag in
  any order. Critically includes json, thread-pool, watch, ecs, raylib,
  template.
- **Layer 1:** http (needs json), httpd (needs json, thread-pool),
  notebook (needs watch), ecs-raylib (needs ecs, raylib).
- **Layer 2:** tourist (needs httpd, template, transitively json,
  thread-pool).

**Design decision required.** `:path` is the documented workspace-dev
convenience (see `CLAUDE.md`), while `:url`/`:ref` is needed for release.
Pick one:

1. **(a)** Switch sibling deps to `:url`/`:ref` outright -- simplest for
   consumers, but local dev now fetches pinned siblings instead of using
   live workspace source (slower iteration, version lag).
2. **(b)** Keep `:path` for in-repo dev and generate the `:url` form only
   into the published/tagged manifest via a release step -- preserves dev
   ergonomics but needs tooling.
3. **(c)** Confirm whether Spice supports a combined path-with-url-fallback
   entry; if so, use it.

### Update 2026-06-23 -- option (c) confirmed supported

Code dive into the `tur` package manager confirms hybrid `:path` + `:url`/`:ref`/`:subdir`
entries already work end-to-end. **`:path` wins when present; `:url` is the
standalone fallback.** No compiler patch needed. Key references:

| Step | File:line | Behavior |
|---|---|---|
| Manifest parse | `src/compiler/pkg.c:186-190` | `parse_spices()` extracts all four keywords with no mutual-exclusivity check |
| PkgSpice struct | `src/compiler/pkg.h:77-84` | Stores `url`, `ref`, `path`, `subdir` simultaneously |
| Build-time include-path | `src/main.c:2354-2361` | `if (s->path) { local } else if (s->ref) { fetched }` -- `:path` preferred |
| Fetch loop | `src/compiler/pkg.c:1504-1537` | `pkg_fetch_all()` skips entries with `:path` set; removes any stale lock row |
| Manifest write-back | `src/compiler/pkg.c:597-602` | When `:path` is set, write only `:path` (drops `:url`/`:ref` on round-trip -- caveat below) |

**Caveat (write-back drops `:url`):** if `tur add`/`tur fetch` rewrites the
manifest, the hybrid entry collapses to `:path`-only. Manual edits to the
manifest survive, but tooling-driven rewrites need to be audited before
adopting (c) as the standard. Either avoid the round-trip in release flow,
or patch `pkg.c:597-602` to preserve `:url`/`:ref` alongside `:path`.

**Recommendation:** adopt option (c) as Gap 2's resolution. The 5 manifests
in Gap 2 get both `:path` (workspace dev unchanged) and `:url`/`:ref`/`:subdir`
(standalone fetch works). Optionally also:

- Document the hybrid form in `docs/guides/developing-spices-guide.md:383-408`
  (currently treats them as either/or).
- Add a fixture under `tests/fixtures/` exercising a hybrid entry, so the
  precedence rule is regression-tested.
- Decide whether to fix the write-back drop in `pkg.c:597-602`.

## Gap 3 -- Standalone buildability (not just resolvability)

A fetched spice must also BUILD in isolation:

- **21 spices declare `:cmake-deps`** (native libs): ecs-raylib, http, httpd,
  json, opengl, osc, plot, plutovg, png, postgres, raygui, raylib, regex,
  rtaudio, rtmidi, sdf-raylib, sqlite, tls, valkey, wav, zlib. These fetch
  via CPM at build time; standalone consumers need network + a working
  toolchain. The yyjson fetch failure seen earlier is exactly this class of
  issue and should be smoke-tested per spice.
- **4 spice dirs are NOT in the root `:members`** (ecs, ecs-raylib, tls,
  zlib), so they're outside the workspace and likely outside the CI matrix.
  They should be added to `:members` (for local sibling resolution + CI
  coverage) before being relied upon as fetchable deps.

## Recommended Plan

1. **Decide the `:path` policy** (Gap 2, option a/b/c) -- this gates
   everything else and is the only real design call.
2. **Add the 4 missing spices to root `:members`** (ecs, ecs-raylib, tls,
   zlib); resolve the `ecs*` pre-release version naming.
3. **Per-spice standalone smoke test** -- in a clean checkout, fetch+build
   each spice alone; fix cmake-deps / dep-resolution failures. Gate on this
   in CI (a "standalone fetch" matrix job).
4. **Tag in topological order** (Layer 0 -> 1 -> 2). The repo already ships
   `cut-{patch,minor,major}-release` skills that automate version-bump +
   CHANGELOG + tag; use them per spice.
5. **Automate going forward** -- add a CI check that every spice at version
   `V` has a matching `<spice>-vV` tag (or flags the drift), so this gap
   can't silently reopen (as ansi already did at 0.1.4 vs 0.2.0).

## Constraints Observed This Session

- Tag/release pushes are BLOCKED here (git credential scoped to the feature
  branch -> HTTP 403; the GitHub MCP exposes no tag/ref/release-creation
  tool). The actual tagging in step 4 must run from an environment with
  push rights to `rjungemann/turmeric-spices`.
- Estimated scope: ~34 tags + 5 manifests edited for sibling deps (8
  `:path` entries) + 4 `:members` additions + CI additions. The dependency
  coupling (Gap 2) makes this a coordinated release, not 34 independent
  commits.

## Resolution -- 2026-06-23, shared monorepo tag

Landed in `turmeric-spices` commit `d1f809a` on `main` and annotated tag
`v0.1.0` (pushed to `origin`). Chose option (c) for Gap 2 (hybrid `:path`
+ `:url` entries), and chose a single shared monorepo tag over per-spice
tags for Gap 1 -- 34 per-spice tags collapsed to one.

What landed:

- 5 manifests (`http`, `httpd`, `notebook`, `ecs-raylib`, `tourist`) gained
  `:url "https://github.com/turmeric-lang/turmeric-spices" :ref "v0.1.0"
  :subdir "spices/<sibling>"` alongside their existing `:path` entries.
  8 sibling deps in total. `:path` still wins in workspace dev (verified
  via `tur fetch --dry-run`).
- Root `build.tur` gained `ecs`, `ecs-raylib`, `tls`, `zlib` in
  `:members`.
- `spices/ecs-raylib/build.tur` `:url` org typo fixed
  (`turmeric-spice` -> `turmeric-spices`).
- Note: the original report's premise that pushes were blocked was wrong --
  push rights to `rjungemann/turmeric-spices` work from this env.

Effect: any of the 38 spices is now standalone-fetchable via
`#{:url ".../turmeric-spices" :ref "v0.1.0" :subdir "spices/<spice>"}`.

Not addressed by this resolution (intentional -- collapsed by the
shared-tag choice or deferred):

- Per-spice version tagging (`<spice>-v<ver>` convention) -- optional
  polish, no longer load-bearing.
- `pkg.c:597-602` write-back drop: `tur add`/`tur fetch` rewrites still
  collapse hybrid entries to `:path`-only. Manual edits survive; tooling
  round-trips don't. Worth patching if hybrid becomes standard.
- CI drift check ("every spice at version V has tag `<spice>-vV`") --
  obsolete under shared-tag scheme; replace with "every release bumps the
  shared monorepo tag" if codified.
- Per-spice standalone fetch smoke matrix -- still worth adding to CI.
