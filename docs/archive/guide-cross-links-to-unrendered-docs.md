# Rendered guides link to docs that were never rendered -- four dead links on the site

**Severity: low** (docs) -- four links in three published guides 404 on
turmeric-lang.com. Surfaced by `tools/genpack.py`'s link pass (OD1), which
reports every cross-link it cannot resolve inside the docs pack.

## Repro

```sh
just docs
```

The pack build ends with:

```
  note: 4 cross-link(s) in 3 page(s) point outside the pack and were left
        pointing at the website:
        guides/macros-guide.html: ../upcoming/hold/row-types-followups-plan.md,
                                  macro-system-direction-plan.md
        guides/performance-guide.html: memory-management-guide.md
        guides/stateful-refinements-guide.html:
                                  ../archive/borrow-param-passed-as-unique-mut-undiagnosed.md
```

Each resolves to a 404 on the live site:

- `https://turmeric-lang.com/docs/html/guides/memory-management-guide.html`
- `https://turmeric-lang.com/docs/html/upcoming/macro-system-direction-plan.md`

## Root cause

Two distinct problems that the link pass reports together.

**1. A link to a guide that does not exist.** `docs/guides/performance-guide.md:365`
says `See [memory-management-guide.md](memory-management-guide.md).` There is no
`docs/guides/memory-management-guide.md`, and there is no record of one having
been removed -- the link looks like it was written against a planned guide.
`render_guide`'s link rewrite (tools/genguides.py) turns any local `.md` href
into a sibling `.html` without checking the target exists, so this has shipped
as a dead link since it was written.

**2. Links from `docs/guides/` into the rest of `docs/`.** `../upcoming/...md`
and `../archive/...md` resolve, from a page at `/docs/html/guides/`, to
`/docs/html/upcoming/...` and `/docs/html/archive/...`. Those paths have never
existed: `docs/html/` only ever contains `api/`, `guides/`, and `spices/`.
These were dead before OD5 narrowed the `web/public/docs` symlink and are
equally dead after it -- OD5 did not cause them, and the narrowing is not what
needs reverting.

## Fix direction

1. `performance-guide.md`: either write the missing guide or repoint the link.
   The surrounding paragraph is about RC and arena allocation, so
   `substructural-types-guide.md` or `opaques-guide.md` is the likely intent --
   worth confirming with the author rather than guessing.
2. The `../upcoming/` and `../archive/` links: these are references to internal
   planning notes from published guides, which is a smell in itself. Either
   drop them, or point them at GitHub blob URLs
   (`https://github.com/turmeric-lang/turmeric/blob/main/docs/upcoming/...`), which
   is what the guides already do for `CLAUDE.md`.
3. Once the tree is clean, run the pack build with `--strict-links` in CI so a
   new dead cross-link fails rather than being reported and scrolled past:

   ```sh
   python3 tools/genpack.py web/public/docs-pack/ --strict-links
   ```

---

## Resolution (2026-08-26)

Fixed. `just docs` (`./build/tur run docs`) now runs the pack build with
`--strict-links` and exits 0 on a clean tree; a new dead cross-link fails the
docs build rather than printing a note.

### It was eight links in five pages, not four in three

The report's list came from the pack's link pass, which only sees pages that
made it into the pack. A `grep` across `docs/guides/` for cross-directory
hrefs found four more of the same shape, and arming `--strict-links` surfaced
two beyond that:

| Page | Link | Listed in report |
| --- | --- | --- |
| `macros-guide.md` | `../upcoming/hold/row-types-followups-plan.md` | yes |
| `macros-guide.md` | `macro-system-direction-plan.md` | yes |
| `performance-guide.md` | `memory-management-guide.md` | yes |
| `stateful-refinements-guide.md` | `../archive/borrow-param-passed-as-unique-mut-undiagnosed.md` | yes |
| `refinement-solver-internals-guide.md` | `solver-extension-plan.md` | no |
| `test-suite-portability-guide.md` | `../archive/history/fat-captures-borrowed-read-uninitialized.md` | no |
| `test-suite-portability-guide.md` | `../reported/sanitizer-gate-not-armed-in-ci.md` | no |
| `test-suite-portability-guide.md` | `../reported/rc-ref-conversion-and-weak-upgrade-leak.md` | no |

The last one had a second problem the pack could not have reported: its target
was resolved and moved to `docs/archive/` at some point, so even the GitHub
blob URL had to be repointed rather than mechanically rewritten. Worth knowing
for the next pass -- a `../reported/` link rots twice, once when the docs are
rendered and again when the report is archived.

### The missing guide was not missing

Fix direction 1 says the `memory-management-guide.md` link's intent is
"`substructural-types-guide.md` or `opaques-guide.md` -- worth confirming with
the author rather than guessing". Neither. `docs/guides/gc-guide.md` exists and
its own description is "How memory is managed in Turmeric -- reference
counting, the Bacon-Rajan cycle collector, arenas, and what is (and isn't)
GC-managed", which is the paragraph the link sits under, point for point (RC
being a refcount bump, cycles not being reclaimed, `(gc-auto!)` being opt-in).
The link was written against the right *content* under a name the guide never
had. It now points at `gc-guide.md`, plus `ownership-guide.md` for the by-value
/ borrow bullet in the same list.

### The rest follow the convention the guides already had

`../upcoming/`, `../archive/`, and `../reported/` are not rendered -- `docs/html/`
only ever holds `api/`, `guides/`, and `spices/` -- so those links were dead
before the OD5 symlink narrowing and equally dead after, exactly as the report
says. Fix direction 2 offers "drop them or point them at GitHub blob URLs".
Blob URLs, since `ecs-vs-haskell-ecs.md`, `refinement-types-guide.md`, and
`docs/guides/README.md` were *already* doing that for the same kind of
reference -- the four broken ones were the stragglers, not a new policy.

### The gate (fix direction 3)

`--strict-links` already existed in `tools/genpack.py`; nothing invoked it. It
is now on in the Justfile `docs` recipe, which is what CI runs
(`.github/workflows/ci.yml`, "Generate stdlib docs"), so the website build and
a local `just docs` fail the same way. Verified both directions: the recipe
exits 0 as it stands, and exits 1 naming the page and href when a dead link is
present.
