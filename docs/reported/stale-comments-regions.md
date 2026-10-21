# Regions comments still describe `--enable=regions`, a flag that no longer exists

**Severity:** low (comment accuracy). One item proposes merging three copies of
a code emitter whose comments exist to keep them in step. Filed 2026-10-17
from the "Trust the code over the comments" note in
[regions-internals-guide](../guides/regions-internals-guide.md). Checked
against `main` 9ac3cd43; line numbers are from that commit.

Regions graduated on 2026-09-05. They are on unless `TUR_REGIONS=0` is set
when `tur` runs: `g_opt_regions` defaults to `true`, `main.c:13274` reads the
variable, and `regions_enabled()` (`types.c:1795`) returns `g_opt_regions`.
There is no `--enable=regions`, no experiment row, and no TUR-W0060 for it.
The guide names three comments that predate this. A grep for `enable=regions` /
"behind a flag" over `src/`, `stdlib/`, `CMakeLists.txt` and `tests/*.sh`
finds eight that are stale. Three more mention the flag accurately, as
history (`region.h:16`, `main.c:13268`, `tests/run-regions-seam.sh:8`). A read of
the ctor emitters finds the rest.

## Read this before editing `src/runtime/region.h`

`region.h`, `arena.{h,c}`, `region.c` and `r7gc.c` are **pasted verbatim** into
emitted programs, comments included (`cmake/embed_region_runtime.cmake` ->
`emit_region_runtime_bodies`). So any edit to `region.h`, even to a comment:

- changes all 157 `tests/fixtures/*/expected.c` snapshots, every one of which
  carries the header text;
- changes the S2 canonical preamble. You must regenerate
  `src/runtime/generated/tur_rt_split{.c,_decls.h,_embed.c}` with
  `tools/gen-runtime-split.py` and commit the new hash, or the split
  disengages and CI's "Verify the S2 split path actually engages" step fails.

Batch items 1-4 into **one** change with one regen. Item 13 describes a
structural fix that removes this cost for good.

**Decided 2026-10-17: strip the comments (item 13's second bullet).** Land
that first. Its one-time regen then also absorbs items 1-4, and comment fixes
in these files cost nothing from then on.

## Changes

### 1. `region.h:34-35`: "shippable behind a flag"

- **Says:** "The Debug arena poisons reclaimed bytes, so a value that outlives
  its region crashes loudly under ASan ... That backstop is why this is
  shippable behind a flag at all; do not disable it."
- **Change:** "That backstop is part of why regions could graduate to on by
  default; do not disable it."

### 2. `region.h:25-31` and `:60-62`: R1-era narrative presented as current

- **Says:** "R1 ships the mechanism with the rule enforced the only way it can
  be before the escape check exists ... the plain `tur_region_pop` does not
  rewind at all. R3 replaces the caller's word with a proof." On
  `tur_region_pop`: "The conservative default, and what R1 uses everywhere".
- **Code:** R3 and R4 landed. The emitter calls `tur_region_pop_checked` when
  the static walk admits the result type (`emit_expr.c:8486`, `:8498`, and
  `emit_cps_ir.c:9932`, `:9938`). It calls plain
  `tur_region_pop` when the walk refuses or the body is void (`emit_expr.c:8501`,
  and the CPS twin at `emit_cps_ir.c:9940`). `TUR_REGION_RETIRE_TO` also uses it.
- **Change:** rewrite both in the present tense: "a pop rewinds only through
  `tur_region_pop_checked`, after both locks pass; `tur_region_pop` is what
  the emitter spells when the static walk refuses, and on the catch
  boundary."

### 3. `region.h:172-174`: "the r7rs-gc experiment's collector"

- **Code:** r7rs-gc graduated on 2026-09-25 (`experiments.c:509-515`).
- **Change:** "The r7rs collector reads these as roots".

### 4. `tur_region_pop_reclaim` has no caller in the product (`region.h:71-75`, `region.c`)

- **Code:** `tests/region_unit.c` is its only caller. The `expected.c` hits
  are the pasted declaration, not calls. Nothing in `src/compiler/` or
  `stdlib/` spells it. Its comment already says "Prefer `tur_region_pop_checked`".
- **Change (structure):** delete it and move the unit test onto
  `tur_region_pop_checked`. If it is meant as a public escape hatch, say that
  in its comment. The regen cost described above applies here too, so fold
  this into the same change as items 1-3.

### 5. `globals.h:364-370`: a comment that contradicts itself

- **Says:** "Off by default and gated behind `--enable=regions`: this is
  user-visible surface ..." and then, six lines later, "GRADUATED 2026-09-05:
  default true."
- **Change:** delete the "Off by default and gated ..." sentence. Keep the
  safety-rule sentence that follows it and the graduation line.

### 6. `types.h:1934-1936`: `regions_enabled`

- **Says:** "true when `--enable=regions` is on. Warns once per compile
  (TUR-W0060) on first consult".
- **Code:** `return g_opt_regions;`. It never warns.
- **Change:** "true unless TUR_REGIONS=0 was set when tur ran (the bisection
  hatch; see main.c)."

### 7. `emit_cps_ir.c:9652-9654`: "no default-path routing moves"

- **Says:** "that predicate is false unless `--enable=regions` is on, so no
  default-path routing moves."
- **Code:** `emit_binding_is_region_scope` (`emit_expr.c:7631`) returns true
  for `bt-scope` / `with-region` on every default build. So `force_direct`
  **does** change default-path routing for those two callees, and has done
  since graduation.
- **Change:** "true for `bt-scope` and `with-region` unless TUR_REGIONS=0, so
  a tail call to either always takes cps->direct". The rationale above it
  (they are effectively direct already) still justifies the choice, so keep
  it.

### 8. Three comments about "the flag off"

| Where | Says | Change to |
|---|---|---|
| `src/turi/preload.c:193` | "exactly what the compiled path does with `--enable=regions` off, the only mode the interpreter has" | "...does under TUR_REGIONS=0..." |
| `src/compiler/stdlib_autoload.c:58-61` | "with `--enable=regions` off the defn is an ordinary identity call ... the emitter opens a region around a call to it only when the flag is on" | "the emitter opens a region around a call to it unless TUR_REGIONS=0; the body itself is an identity forwarder" |
| `src/CMakeLists.txt:738-742` | "calls tur_region_alloc_or_malloc when --enable=regions is on ... with the flag off nothing references it" | "every emitted spine ctor calls it (regions are on by default); only a TUR_REGIONS=0 build leaves it unreferenced" |

### 9. The test comment in the top-level `CMakeLists.txt` (`CMakeLists.txt:350`)

- **Says:** "RM3 R2: --enable=regions with no region open must change nothing
  observable."
- **Code:** since graduation, `tests/run-regions-seam.sh` runs the population
  with `TUR_REGIONS=0`, keeping the off path green, and its own header says
  so.
- **Change:** "RM3: keep the TUR_REGIONS=0 bisection path green (see the
  script header)."

### 10. The heap ctor emitters: wrong count, wrong default (`emit_module.c:12058-12074`, `types.c:2681-2689`, `emit_module.c:20207-20220`)

- **Says:**
  - The base ctor in `emit_adt_typedef_and_ctors` and the monomorph ctor in
    `emit_registered_adt_app_rec` each say "the two mirror each other and a
    change to one belongs in both".
  - Both also say "Gated: a default build emits the identical `malloc` it
    always has" (and "Gated, so a default build emits the identical malloc").
  - The third copy in `emit_program_inner` says "All three emitters mirror
    each other", and quotes valgrind counts "with the flag on against ...
    with it off".
- **Code:** there are three heap ctor emitters, not two, and since graduation
  a default build spells `tur_region_alloc_or_malloc`, not `malloc`, at all
  three.
- **Change (comment):** fix the count in the two older comments, drop the two
  "Gated ... identical malloc" sentences, and turn the valgrind sentence into
  a reference to the archived plan.
- **Change (structure, preferred):** see item 13.

### 11. The two rewind fixtures say they rewind -- fixed 2026-10-21

`tests/fixtures/region-with-region` and `region-scope-value-survives` said "the
pop REWINDS" / "both locks pass and the pop rewinds" while every bracket
retired. Fixed under
[region-rewind-fixtures-never-rewind](../archive/region-rewind-fixtures-never-rewind.md)
(archived): the spines are typed, so the comments are true, and each fixture
pins its rewind count. Nothing left to do here.

### 12. `hamt.c:10`: a citation that resolves nowhere

- **Says:** "See docs/hamt-plan.md for full design and phase breakdown."
- **Code:** no `hamt-plan.md` exists anywhere under `docs/`. The file header
  also describes "Phase P1 v1" (reference counting, list collisions) as the
  current design.
- **Change:** drop the citation, or point it at the HAMT section of the
  guide that covers it now, if there is one. Re-check the header's design
  summary while there, because this file also carries the region store hooks
  (`tur_hamt_set`, `tur_hamt_transient_set`).

### 13. Structure: one heap-ctor body emitter, and comments stripped from the embedded runtime

Two changes would remove whole classes of the items above.

- **Merge the heap ctor body.** The three emitters in item 10 write the same
  body: `__r = (T *)<region_alloc_fn()>(sizeof(T))`, an optional tag store,
  one `adt_field_member_path` store per field, and `return __r`. Only the
  monomorph adds the wide-box field cast. Extract
  `emit_heap_ctor_body(Buf *, const AdtDef *, const CtorDef *, const char *c_name, const bool *wide_box)`
  and call it from all three, and from `emit_flush_dead_base_ctor_traps`'
  phantom base (`emit_module.c:5078`) if it fits. The "mirror each other"
  comments then have nothing left to warn about, and the next routing change,
  of the kind R2 needed twice, is made once. This is the same "one walk, or
  the two paths drift" argument `emit_region_scope_reclaims` already makes
  for the static lock.
- **Strip comments when embedding the runtime.** The pasted runtime sources
  carry their full prose into every emitted program. That is why a comment
  fix in `region.h` costs 157 snapshots and a split-hash regen, and probably
  also why items 1-4 survived graduation. Stripping `/* ... */` comments in
  `cmake/embed_region_runtime.cmake`, or with a small C lexer in
  `emit_embedded_runtime_source`, makes these files' comments free to
  maintain. It needs one regen when it lands, and then never again for a
  comment edit. Line directives are not affected, because none are emitted
  for these sources. **Decided 2026-10-17: do this.** Strip only `/* ... */`
  and `//` comments outside string and character literals, keep the line
  breaks, and leave `#` directives and `emit_embedded_runtime_source`'s
  local-`#include` dropping exactly as they are. The one regen covers
  the 157 snapshots and `tools/gen-runtime-split.py` with its new hash.

## When this is fixed

Remove the three examples from the guide's "Trust the code over the comments"
paragraph. (Its fixture sentence went when
`region-rewind-fixtures-never-rewind` was archived, 2026-10-21.)
