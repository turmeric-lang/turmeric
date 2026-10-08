# ASCII art diagrams that wanted to be Mermaid -- support, inventory, conversions

**Status: RESOLVED.** Mermaid support shipped in the guide pipeline, all nine
convertible diagrams converted, and the six that should stay ASCII given a fence
that stops the code-block line-height from breaking them.

**Severity when filed: low.** Nothing was broken. One concrete correctness
consequence justified filing it as a finding rather than a style preference:
`compiler-internals.md` drew the same pass pipeline twice, 500 lines apart, and
the two copies had **drifted** in both directions.

## What shipped

### 1. Mermaid rendering in the guides

Guides had no Mermaid support at all -- the string `mermaid` did not appear
anywhere in the repo. Three changes in [`tools/genguides.py`](../../tools/genguides.py)
plus one call site in `web/main.js` give it to both consumers of the one
rendering pass:

- **`render_mermaid_blocks`** rewrites the `<pre><code class="language-mermaid">`
  that python-markdown's `fenced_code` emits into the `<pre class="mermaid">`
  mermaid looks for. Entity escaping is deliberately **kept**: mermaid reads
  `textContent`, which the browser has already decoded, so `A --&gt; B` reaches
  the parser as `A --> B`, and the block stays valid HTML in the meantime.
- **`renderMermaid(root)`** joins `highlightGuideCode` and `initSyntaxToggles`
  on `window.turmericGuide` -- same shape (takes a root, idempotent), so the
  site pages call it inline and Try Turmeric's docs pane calls it against each
  freshly rendered fragment.
- **CSS** for `pre.mermaid`, both before and after render.

#### Mermaid is loaded lazily, and is not vendored

`import()` from `cdn.jsdelivr.net` (already `preconnect`ed for the Iosevka
faces), pinned to `mermaid@11`, fired only when the subtree actually contains a
diagram. The reason is the docs pack: `web/public/docs-pack/` is **6.3 MB** and
the service worker precaches it **wholesale and unconditionally**
(`web/public/sw.js`). Mermaid's bundle is ~3 MB, so vendoring it would grow
every offline install by roughly half for a feature a minority of pages use.

The cost is that **diagrams do not render with no network** -- the offline docs
pane, a docs tarball read off disk. That is why the escaped source is left in
the `<pre>`: the import fails, the `catch` stamps `.mermaid-unrendered`, and the
block degrades to its own readable source text rather than to an empty box.

| Case | Result |
| --- | --- |
| Normal load | `data-processed="true"`, real `<svg>`, guide palette (gold borders, `#EAE0D2` text) |
| CDN unreachable | `.mermaid-unrendered`, no `<svg>`, `innerText` is the diagram source, one `console.warn` |
| One malformed block among nine | only that block is stamped; the other eight still draw |

If offline diagram rendering is later wanted, the seam is already right: drop
`mermaid.min.js` into the pack from `genguides.py --emit-pack` and `genpack.py`
sweeps it into `files`, hence into the precache, with no other change. That is a
**budget decision, not a plumbing one**.

### 2. A ```ascii fence for the art that cannot convert

`body { line-height: 1.6 }` in `docs/html/api/style.css` is inherited by `pre`.
That is right for reading code and **stays** -- but it is exactly what breaks
ASCII art: consecutive `|` glyphs get a 0.6em gap, so a vertical stroke reads as
a dotted stutter instead of a line. A directory tree's gutter, a diagnostic's
caret column and a grammar's aligned alternatives all depend on those glyphs
touching.

So: blocks fenced ` ```ascii ` get `line-height:1.15`; every other block keeps
1.6. The rule of thumb the fence encodes --

- **A graph** (nodes, labeled edges, branches, cross-links) -> ` ```mermaid `.
- **Preformatted text whose vertical alignment carries meaning** (a tree of
  names, a grammar, pasted tool output) -> ` ```ascii `.
- **Everything else** -> the language fence it already had.

### 3. Nine conversions and six retags

| Converted to Mermaid | Diagram |
| --- | --- |
| `docs/guides/compiler-internals.md` | `flowchart TD` -- the pass pipeline, merged from two drifted copies |
| `docs/guides/gc-guide.md` | `block-beta` -- memory-management layer stack |
| `docs/guides/refinement-solver-internals-guide.md` | `flowchart TD` -- RT1/RT2/RT3 with artifact nodes and all three verdicts |
| `docs/guides/advanced-type-system-rationale.md` | `flowchart LR` -- feature dependency graph |
| `docs/upcoming/hold/ci-release-workflows-plan.md` | `sequenceDiagram` -- four workflows and a human |
| `docs/upcoming/hold/linalg-spice-followups-plan.md` | `flowchart LR` -- release DAG with the cross-edges |
| `docs/upcoming/release-in-actions-plan.md` | `flowchart TD` -- seven steps, `subgraph` for the private phase |
| `docs/upcoming/hold/stats-formula-plan.md` | `flowchart TD` -- module pipeline |
| `docs/upcoming/aot-compiled-repl-plan.md` | `flowchart TD` -- REPL eval pipeline |

| Retagged ` ```ascii ` | Why it stays |
| --- | --- |
| `docs/guides/compiler-internals.md` | `src/` directory tree |
| `docs/guides/releases-and-installation-guide.md` | install-layout tree with `#` annotations |
| `docs/guides/repl.md` | `.tur-repl-cache/` layout tree |
| `docs/guides/snake-game-tutorial.md` | example project tree |
| `docs/guides/refinement-types-guide.md` | BNF grammar -- text, not a graph |
| `docs/guides/serializable-continuations-guide.md` | pasted compiler diagnostic; must stay byte-exact |

## What the conversions recovered

Three of them moved facts out of prose and into the picture, which is the part
that was actually worth doing:

**`compiler-internals.md` -- the drift.** Both blocks drew the pass pipeline and
they disagreed:

| Stage | `:19` | `:549` |
| --- | --- | --- |
| stdlib forms prepended in `main.c` | **absent** | present |
| SRFI prune (`src/passes/srfi_prune.c`) | present | **absent** |

Neither was wrong on purpose -- `:549` predated `srfi_prune`, `:19` never picked
up the stdlib-prepend note. `:549`'s only genuine addition was the data type on
each edge (`Form[]`, `Expr*`, `Buf`), which Mermaid puts on the edge label, so
the two merged with nothing lost. The `## Data flow summary` section now
cross-references the one diagram instead of redrawing it.

**`linalg-spice-followups-plan.md` -- five stranded edges.** The block was a
`+--` tree, but the thing described is not a tree: five dependencies were
cross-links the tree shape could not express, so they sat underneath as prose
("SM1 (PCA) blocked on LB0 + LB2", "LS3 is blocked on LB4"). They are edges now,
with the blocked-on tasks as edge labels, and the duplicated prose line was
deleted -- keeping both is how the two drift apart.

**`refinement-solver-internals-guide.md` -- two lost branches.** The first
conversion pass silently dropped the `INVALID` and `UNKNOWN` verdicts (see
below). All three are on the diagram now.

## Two bugs found by rendering, not by reading

Both were caught because every diagram was rendered in a browser before being
called done. Neither would have shown up in a diff review.

1. **`mermaid.run({nodes})` rejects on the first failure and abandons the
   batch.** One typo in one block stamped all nine on the page as unrendered,
   including the eight that had drawn correctly. `renderMermaid` now runs one
   node at a time and catches per node. `suppressErrorRendering: true` goes with
   it: mermaid's default is to replace a failed diagram with a "Syntax error"
   bomb graphic, which destroys the source text that is the whole point of the
   fallback.

2. **Two conversions silently truncated their source block.** Replacing a block
   by matching its opening text leaves any trailing lines orphaned *inside* the
   new fence, where they are no longer ASCII art and not valid Mermaid either.
   It happened twice -- `refinement-solver-internals-guide.md` lost the
   `INVALID` and `UNKNOWN` verdict lines, `advanced-type-system-rationale.md`
   lost four feature groups. **Replace these blocks by fence line range, not by
   matching a prefix of the body**, and audit the result for lines that are
   neither a Mermaid keyword nor an edge.

Also worth knowing before writing a `sequenceDiagram`: **`;` is a statement
separator in message text.** `P->>P: bump VERSION; regenerate docs` splits at
the `;` and the remainder fails to parse, with an error that names a column in a
line that looks fine. Use a comma.

## Scope

Swept `docs/upcoming/` (39 plans), `docs/guides/` (143), `docs/reported/` (34)
and `docs/archive/` (1610).

| Tree | Diagram runs found | Action |
| --- | --- | --- |
| `docs/upcoming/` | 5 | all 5 converted |
| `docs/guides/` | 11 | 4 converted, 6 retagged ` ```ascii `, 1 already fine |
| `docs/reported/` | 0 | -- |
| `docs/archive/` | 123 | **none** -- those plans are done; rewriting them is churn |

## Unverified

The site sends `Cross-Origin-Embedder-Policy: require-corp`
(`web/public/_headers`). Module imports are always CORS-fetched and jsdelivr
sends `Access-Control-Allow-Origin: *`, so this *should* satisfy COEP -- but
every browser check here ran against a plain local server with no COEP header.
**Confirm on a real deploy** (or a preview with the headers applied) before
assuming a guide diagram renders in production. If it does not, the fix is the
vendoring path described above, which sidesteps the cross-origin question
entirely.

## Guide upkeep

The Mermaid support is developer-facing tooling, not language surface, so it has
no guide of its own. If a guide ever documents the docs pipeline, three facts
are worth carrying: ` ```mermaid ` fences render, they **do not render offline**
by design, and ` ```ascii ` is the fence for art that must keep its vertical
alignment.
