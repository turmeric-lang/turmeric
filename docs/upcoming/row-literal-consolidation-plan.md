# Plan: Consolidate Row Literals on `#{...}` (v1)

> Status: planning
> Tracks: compiler reader/typer + `-Xdata-literals` surface
> Replaces: `#row{...}` as the HKT row-type literal

## Motivation

There are currently two reader forms that both denote a closed row of
labels at the type level:

| Form | Position today | Source |
|---|---|---|
| `#{Foo Bar}` | effect-row slot of `defn` signatures | `stdlib/effects.tur`, capability tags `FS` / `Net` / `Proc` / `Rand` |
| `#row{Foo Bar}` | type-expression position, gated by `-Xdata-literals` | `spices/ecs` (`(Query #row{Pos Vel} #row{Pos})`) |

The two forms are the same kind of thing — a closed row over labels —
but parse only in disjoint positions. Now that the data-literal program
has settled on `#set{...}` and `#map{...}` for the typed Set/Map literals
(`DL1`, see `set.tur:352`, `map.tur:725`), `#{...}` is no longer needed
as a set-literal escape hatch and can be reclaimed as the *one* row
literal that works in every row-typed position.

The end state we want:

```turmeric
;; Effect row, unchanged spelling:
(defn read-config [p : cstr] #{FS} : cstr ...)

;; HKT row argument, new spelling (was #row{Pos Vel}):
(defn integrate [q : (Query #{Pos Vel} #{Pos})] : nil ...)
```

One reader form, one AST node, one pretty-printer output. `#row{...}`
becomes a deprecated parser alias for a release, then is removed.

---

## Non-goals

- Changing the semantics of effect rows or HKT rows.  Both remain
  closed rows over label sets; only the spelling consolidates.
- Touching `#set{...}` / `#map{...}` / `#\` literals.
- Introducing open-row syntax (e.g. `#{Foo | r}`).  That is a separate
  design; this plan just unblocks it by parking the row literal on a
  shorter form first.
- Ungating row literals from `-Xdata-literals`.  HKT row arguments
  still live behind the flag; only the spelling changes.

---

## Prerequisite changes

These are ordered by dependency.

### P1. Reader: emit one row-literal node for `#{...}`

Today the reader treats `#{...}` as a position-specialized token that
only the `defn` effect slot accepts.  Change it to emit a generic
`RowLit` AST node (the same node `#row{...}` already produces under
`-Xdata-literals`).

- The effect slot of `defn` continues to require a `RowLit` and rejects
  anything else, so existing diagnostics survive.
- Reader change is independent of the typer.

### P2. Typer: accept `RowLit` in type-expression position

Wherever the typer currently accepts `#row{...}` as a row-kinded type
argument (e.g. `(Query #row{Pos Vel} #row{Pos})`), make it accept the
same `RowLit` node regardless of which surface form produced it.

- Keep this behind `-Xdata-literals`.  The flag still gates HKT row
  literals as a *type-position* feature; the effect-slot use of
  `#{...}` is not gated and must remain ungated.
- Kind inference is unchanged: the slot already advertises row kind,
  so the literal binds at row kind in either position.

### P3. Parser alias: `#row{...}` → `RowLit`

Keep `#row{...}` parsing for one release as a deprecated alias.

- Emit a one-time advisory the first time a file uses `#row{...}`:
  `note: #row{...} is deprecated; spell row literals as #{...}.`
- The advisory is *not* an error and does not gate compilation.

### P4. Pretty-printer and error messages

Canonicalize on `#{...}` in:
- type-error rendering (`(Query #{Pos Vel} #{Pos})`),
- `tur explain` / docstring examples,
- the `docs/guides/data-literals-guide.md` row section.

### P5. Codemod

Ship a one-shot rewrite (`tur fmt --rewrite-row-literals` or a small
shell loop over `rg '#row\{'`) that turns `#row{...}` into `#{...}` in
`.tur` sources.  Idempotent; safe to run more than once.

### P6. Deprecation + removal

- Release `N`: P1–P5 land.  Both spellings parse; `#{...}` is canonical;
  `#row{...}` advises.
- Release `N+1`: `#row{...}` becomes a hard error suggesting `#{...}`.
- Release `N+2`: `#row{...}` reader path is deleted.

---

## In-repo surface affected

Files that need codemod + re-read after P1–P3 land:

- `spices/ecs/src/ecs/query.tur` (docstring examples, lines 44, 201–220)
- `spices/ecs/tests/poly-call-row.tur` (line 11)
- `spices/ecs/tests/query-typed.tur` (lines 15–19)
- `spices/ecs/README.md` (lines 74–78, 105 — also the `-Xdata-literals`
  note still applies, just update the spelling)

No stdlib files reference `#row{...}` today, so the stdlib churn is
limited to docstrings and the `data-literals-guide.md` row section.

---

## Risks and open questions

1. **Syntax budget.**  Collapsing rows onto `#{...}` permanently spends
   the `#{...}` slot.  This is the deliberate trade — we get a consistent
   row spelling and forfeit `#{...}` as ever meaning anything else
   (untyped sets, namespace maps, etc.).  `#set{...}` / `#map{...}`
   already cover the typed-collection use cases, so the cost is small.

2. **Visual signposting in HKT positions.**  `(Query #row{Pos Vel}
   #row{Pos})` self-labels as "row, row"; `(Query #{Pos Vel} #{Pos})` is
   terser and a casual reader may not immediately see the row kind.
   Mitigation: keep type-error rendering verbose (`row #{Pos Vel}` in
   diagnostics), and lean on hover / `tur explain` for inline kind.
   Re-evaluate after one release of real use; if confusion is common,
   re-introduce `#row{...}` as a *preferred* alias rather than reversing
   the consolidation.

3. **Open-row syntax later.**  When open rows arrive (e.g.
   `#{Foo | r}`), they live naturally inside the consolidated literal
   without further reader changes.  Worth confirming the reader's
   `RowLit` node has room for a tail-variable slot before P1 lands, so
   we don't have to revisit the AST shape.

4. **Effect-slot ambiguity.**  None expected — the effect slot of
   `defn` is positional and only ever accepts a `RowLit`.  But once the
   reader stops position-specializing `#{...}`, double-check that an
   accidental `#{...}` in *expression* position (where rows have no
   meaning) produces a clear "rows are types, not values" error rather
   than a generic parse failure.

---

## Acceptance

- `tur check` on `spices/ecs/tests/query-typed.tur` succeeds with the
  file rewritten to `#{...}` and `-Xdata-literals` set.
- `tur check` on a fresh file that uses `#{FS}` in the effect slot
  succeeds *without* `-Xdata-literals` (effect-row use stays ungated).
- A file that mixes both spellings parses, with one advisory per
  `#row{...}` occurrence.
- `data-literals-guide.md` shows `#{...}` as the canonical row spelling
  alongside `#set{...}` / `#map{...}`.
