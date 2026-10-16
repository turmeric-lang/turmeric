# Spices guide coverage: gap analysis and writing plan

> **Status:** Proposed (2026-10-05)
> **Scope:** turmeric-spices repo (spice-specific guides) and turmeric repo
> (cross-cutting / language-level guides)
> **Track:** post-v1, incremental -- each guide is independently landable.

## 0. Summary

The spices repo ships 48 spices but only 10 spice-specific guides. The
turmeric repo adds 23 more guides covering individual spices or
cross-cutting topics, but 25+ spices still have no guide at all. This
plan identifies the highest-value missing guides, prioritized by README
complexity, dependency centrality, and user-facing impact, and proposes
a writing order.

Guides for individual spices live in the spices repo (rendered to
spices.turmeric-lang.com). Cross-cutting or multi-spice guides live in
the turmeric repo (rendered to turmeric-lang.com/docs/html/guides/).

## 1. What exists today (verified 2026-10-05)

### Spices repo guides (10)

ecs-cross-world, frame, linalg, notebook, plutovg, secret, session,
watch, websocket-client, websocket-server

### Turmeric repo spice-specific guides (23)

arrows-guide, ecs-guide, ecs-storage-guide, ecs-vs-haskell-ecs,
ecs-vs-haskell-ecs, frame-guide, httpd-async-guide, httpd-guide,
httpd-middleware-guide, httpd-tls-guide, json-guide, mise-asdf-guide,
notebook-guide, regex-guide, running-tests-guide, stats-guide,
test-runner-contract, test-suite-portability-guide, thread-pool-guide,
tourist-routing-guide, tourist-session-guide, tur-watch-guide,
websocket-guide, web-stack-guide

### Spices with no guide in either repo (25+)

crdt, ansi, nng, opengl, signal, msgpack, scscm, tls, ecs-raylib,
tidal, tourist-ws, valkey, http, glsl, zlib, raylib, c-dsl, plot,
postgres, sqlite, rtmidi, tourist-session-valkey, osc, rtaudio, png,
ws-core, wav, math, template, sdf-raylib, raygui

### Spices with no README at all (3)

raygui, sdf-raylib, template

## 2. Dependency graph (which spices compose)

```
ecs-raylib    -> ecs, raylib
http          -> json
httpd         -> http, json, tls
msgpack       -> json
nng           -> msgpack
notebook      -> ansi, png, watch
plot          -> plutovg
raygui        -> raylib
raylib        -> math
sdf-raylib    -> math
stats         -> frame, math
tourist       -> httpd, template, http
tourist-session -> tourist, httpd, valkey
tourist-session-valkey -> valkey
tourist-ws    -> tourist, httpd, json
ws-server     -> httpd, json, tls
```

## 3. Prioritized writing order

### Tier 1 -- High-value, high-complexity spices with no guide

These have the largest READMEs but no guide, meaning users are left
reading API-level docs for complex topics.

| Priority | Spice | README lines | Guide location | Rationale |
|----------|-------|-------------|----------------|-----------|
| G1 | crdt | 462 | spices repo | Largest README, CRDTs are inherently complex; users need a walkthrough beyond API docs |
| G2 | ansi | 346 | spices repo | Foundational for terminal output; notebook depends on it; broad audience |
| G3 | nng | 287 | spices repo | NNG messaging wrapper, complex async patterns; depends on msgpack |
| G4 | opengl | 261 | spices repo | Graphics is hard to start; guide covers context setup, shaders, binding model |
| G5 | signal | 244 | spices repo | Event/signal-slot system; likely used across many spices |

### Tier 2 -- Dependency hubs (guides here unlock other spices)

Small READMEs but high fan-out: a guide here scaffolds understanding
for everything that builds on top.

| Priority | Spice | README lines | Guide location | Rationale |
|----------|-------|-------------|----------------|-----------|
| G6 | raylib | 82 | spices repo | ecs-raylib, raygui, sdf-raylib all build on it; small effort, high leverage |
| G7 | math | 48 | spices repo | raylib, stats, sdf-raylib depend on it; foundational |
| G8 | template | no README | spices repo | tourist depends on it; needs README first, then a guide on the templating model |
| G9 | valkey | 99 | spices repo | tourist-session and tourist-session-valkey depend on it; Redis-compatible |
| G10 | msgpack | 201 | spices repo | nng depends on it; serialization is cross-cutting |

### Tier 3 -- Spices with no README (prerequisite blockers)

**Status: READMEs written (2026-10-05).** The three missing READMEs have
been added to the spices repo. Guides for these spices can now proceed.

| Priority | Spice | Action | Status | Rationale |
|----------|-------|--------|--------|-----------|
| G11 | raygui | README first, then short guide | README done | Depends on raylib; once raylib has a guide (G6), raygui can follow |
| G12 | sdf-raylib | README first | README done | Depends on math; niche but worth documenting |
| G13 | template | README first (shared with G8) | README done | Dependency of tourist; should be prioritized |

### Tier 4 -- Cross-cutting / multi-spice guides

These don't map to a single spice but fill real gaps in the
end-to-end story.

| Priority | Title | Guide location | Rationale |
|----------|-------|----------------|-----------|
| G14 | Building a web application with tourist + httpd + tls + sessions | turmeric repo | Flagship use case; tourist-routing-guide and tourist-session-guide cover pieces but nothing ties them together |
| G15 | Graphics pipeline: raylib + ecs-raylib + opengl | turmeric repo | How the graphics spices compose, when to use which |
| G16 | Serialization in Turmeric: json vs msgpack | turmeric repo | When to use each, performance tradeoffs, interop |
| G17 | Terminal applications: ansi + notebook + watch | turmeric repo | Building interactive TUIs; notebook has a guide but ansi doesn't and the composition isn't documented |

## 4. Recommended first batch

If only five guides are written first, these deliver the most value:

1. **G1 -- crdt**: biggest gap, most complex topic, hardest to use from README alone
2. **G2 -- ansi**: foundational, unblocks notebook understanding, broad audience
3. **G6 -- raylib**: small effort, high leverage (unlocks raygui, ecs-raylib, sdf-raylib)
4. **G14 -- tourist web stack end-to-end**: flagship use case, no cohesive guide exists
5. **G8/G13 -- template**: README done; guide can proceed since tourist depends on it

## 5. README-guide linking convention

**Every spice README should link to its corresponding guide(s) in a
"See also" section.** This is now done for all existing spices that have
guides (12 READMEs updated 2026-10-05). The convention going forward:

- **When a new guide is written**, the corresponding spice README must
  be updated to link to it in its "See also" section.
- **Spices-repo guides** are linked as
  `https://spices.turmeric-lang.com/docs/html/guides/<name>.html`.
- **Turmeric-repo guides** are linked as
  `https://turmeric-lang.com/docs/html/guides/<name>.html`.
- A spice with guides in both repos links to both.
- The "See also" section follows the pattern:
  ```
  ## See also

  - [Guide](<url>)
  - [API reference](api/)
  - Source: <https://github.com/turmeric-lang/turmeric-spices/tree/main/spices/<name>>
  ```
- If a spice has multiple guides, list each on its own line with a
  descriptive label (e.g. `[ECS guide]`, `[ECS storage guide]`).

This convention is part of every guide-writing task in this plan, not
a separate step. A guide is not considered complete until its README
links to it.

## 6. Explicitly not doing

- **Rewriting existing guides.** The 33 existing guides are not in scope;
  this plan only covers gaps.
- **Auto-generating guides from READMEs.** Guides are hand-written
  walkthroughs, not API regurgitation. A README is a reference; a guide
  is a narrative.
- **Guides for spices that are experimental or slated for removal.**
  If a spice is on the deprecation path, a guide is not worth the effort.
- **Moving existing turmeric-repo spice guides into the spices repo.**
  The split between spices-repo guides (spice-specific) and
  turmeric-repo guides (cross-cutting) is intentional; existing guides
  stay where they are.

## 7. Open questions

- **Should the spices-repo guide generator (`tools/genspices.py`) list
  guides that don't exist yet as "coming soon"?** This would surface the
  gap to users but requires a placeholder mechanism.
- **Should cross-cutting guides (G14-G17) be linked from the relevant
  spice pages on spices.turmeric-lang.com?** Currently the spices site
  only links to spices-repo guides; cross-references to turmeric-repo
  guides would help discovery.
- **Is there a standard guide template?** The existing guides vary in
  structure. A shared template (intro, getting started, common patterns,
  pitfalls) would speed up writing and improve consistency.
