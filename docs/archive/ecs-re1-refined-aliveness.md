# RE1 -- Strict aliveness accessors in `tur-ecs` (gap C2)

**Date:** 2026-07-26
**Phase:** RE1 of `ecs-refinement-typed-apis-plan.md`, on top of C2
(`refine-stateful-measures-plan.md`).
**Status:** DONE. The pattern is proven end-to-end, two compiler dependencies
were found and resolved along the way, harness integration landed, and the
accessor family shipped as `ecs/refined-world` (single-column facade) and
`ecs/sized-refined` (full stack) -- both registered in `build.tur :exports`.

## What RE1 delivers

An aliveness-refined accessor that will not compile against an entity whose
liveness has not been established in a `frozen` region:

```turmeric
(defn get-x! [^borrow w : GameWorld e : #refine{ x : Entity | (alive? w x) }] : int
  (.posx w))

(frozen w
  (if (alive? w e) (get-x! w e) -1))   ;; discharges from the guard: refine: 1 proven
```

`alive?` reads liveness through an OPAQUE handle (a malloc'd `gens` array, the
same shape as `sized-alive?`'s `gens[idx]` compare), so it is impure to the
purity walk and carries `#reads w`. `despawn!` takes `^unique ^mut w`, so a
`frozen` borrow makes it uncallable in the region (`TUR-E0200`) -- which is what
lets the guard discharge soundly.

Fixtures (verified with `--enable=refined --strict-refine` on a Debug `tur`):

- `spices/ecs/tests/refined-alive-frozen.tur` -- guard + read inside `frozen`:
  **refine: 1 proven**, runs -> `42`.
- `spices/ecs/tests/refined-alive-no-region.tur` -- the same read with NO
  region: **1 unknown -> TUR-W0372**. So `#reads` + `frozen` is load-bearing
  here, not inert.
- `spices/ecs/tests/errors/frozen-despawn-in-region.tur` (pre-existing) -- a
  despawn inside the region is `TUR-E0200`.

## Two compiler dependencies found + resolved (turmeric side, 2026-07-26)

1. **`#reads`-refined params could not codegen** -- the impure entry contract was
   `TUR-E0375`. Fixed by suppressing the (unemittable) entry contract for a
   `#reads`-measure predicate; the accessor's own internal check is the runtime
   backstop.
2. **The `frozen` MACRO did not compose with guard-discharge** -- macro expansion
   copies the body, so the path walk missed the crossing (pointer identity).
   Fixed with a source-span crossing match (`rt_form_ident`). Both are in the
   compiler repo with full validation; see
   `docs/archive/frozen-macro-breaks-refinement-guard-discharge.md`.

## RE1 work items -- all closed

- **Harness integration -- DONE; auto-run UNBLOCKED (2026-07-26).**
  `tur test` gained per-test directives (`;; tur-test-flags: --strict-refine`,
  `;; tur-test-expect-error: TUR-W0372`) -- a general, reusable feature.
  Auto-running the refined tests initially surfaced a serious compiler bug
  (compiling multiple refined files in one process crashed
  nondeterministically), which is now ROOT-CAUSED AND FIXED on the turmeric
  side: an uninitialized `TypeClassMethod.refine_class_binding` memo field
  read recycled arena junk on the second in-process compile (resolved report:
  turmeric `docs/archive/refined-multi-compile-memory-corruption.md`; found
  via the executed arena debug-poisoning/guard plan). A second crash of the
  same class (defdata error path leaving junk ctor slots -- what crashed the
  full ecs suite runner on the known sized-* skew failures) was fixed the
  same day. The refined tests now live FLAT in `tests/` and auto-run with
  everything else: `tur test tests` completes with all 6 refined tests
  passing (suite tally at the time: 64 tests / 41 passed / 23 failed -- the
  23 were the pre-existing `(Storage T)` skew set, none refined. That skew
  was itself fixed later the same day -- see the note at the bottom -- and
  the suite is now **66/66 green**).

- **RE1 (c) -- for-each aliveness refinement, DONE (macro shipped
  2026-07-26).** The hand-written refined LOOP was already proven
  (`tests/refined-loop-alive.tur` -- tail recursion + re-borrow + guard, runs
  -> 40 skipping a despawned entity; named-let/letrec work too). The ergonomic
  MACRO was blocked on macro-GENERATED refined guards/crossings not
  discharging; that is FIXED on the turmeric side (the crossing path walk now
  traverses macro expansions via `refine_note_macro_expansion`; resolved
  report: turmeric
  `docs/archive/macro-generated-refined-crossings-do-not-discharge.md`).
  `ecs/refined-world` now ships **`for-each-alive!`**: one macro generates the
  recursive loop (named-let, TCO'd, no `set!`), the frozen re-borrow, and the
  aliveness guard; the user's refined `rgworld-get-x!` read is spliced as the
  body and discharges PER-ENTITY. `tests/refined-foreach-alive.tur` (proves +
  runs -> 10, 30, skipping the despawned slot);
  `tests/refined-foreach-wrong-entity.tur` (a body reading a DIFFERENT entity
  than the proven binder stays TUR-W0372). Both auto-run in `tur test tests`.
- **Promotion to a shipped accessor family -- DONE (2026-07-26).** The fixtures
  above used a self-contained
  `GameWorld` facade. Promoting to a real `ecs` module meant a facade that owns a
  `WorldState` + storages and re-exports `alive?`/`despawn!`/`get!` with the
  encapsulation (private state field) the soundness argument needs. The
  `ecs/refined-world` module (below) is that promotion for the single-column
  facade, and the FULL-stack promotion SHIPPED 2026-07-26 as
  `ecs/sized-refined`: `(sized-defworld-refined W)` emits `<W>-alive?`
  (`#reads`) + `<W>-despawn!` (`^unique ^mut`),
  `(sized-defcomponent-accessor-refined W C)` emits the cap-gated refined
  `get-<C>!`, and `(for-each-alive W w n e body)` iterates with per-entity
  proofs -- against any `sized-defworld-mono` world, beside the unchanged
  forgiving family. Enabled by a turmeric-side fix letting macro templates
  emit `#reads` and substitute into `#refine{...}` predicates. Tests
  `refined-stack-*` (positive, foreach, no-region W0372, despawn-in-region
  E0200) auto-run; suite 70/70. (It was entangled with the
  `(Storage T)` compiler skew, which is now RESOLVED -- 2026-07-26, turmeric
  `struct_field_type_from_form` gained the assoc-type-projection dispatch and
  the SZ8 Size-literal placeholder; plus the tests' legacy by-value box
  triples were replaced with `defworld-box-helpers`. `tur test tests` is
  **66/66 green**, so the full accessor/for-each surface is validatable
  again and the follow-on is unblocked.)

## Shipped accessor module: `ecs/refined-world` (2026-07-26)

The `GameWorld` facade above is promoted to a real, reusable module,
`ecs/refined-world` (registered in `build.tur :exports`). It exports `RGWorld`
plus `rgworld-new` / `rgworld-spawn!` / `rgworld-despawn!` (`^unique ^mut`) /
`rgworld-alive?` (`#reads`) / `rgworld-get-x!` (refined) / `rgworld-set-x!`. The
backing liveness/component handles and the raw unwrap are PRIVATE.

Validated cross-module (the accessor + measure live in the module; the importer
guards in ITS OWN `frozen` region):

- `tests/refined-module-alive-frozen.tur` -- **1 proven**, runs -> `42`.
- `tests/refined-module-no-region.tur` -- no region -> **TUR-W0372**.

**Trust boundary (also on the module docstring).** `RGWorld` is an opaque affine
handle, so a `frozen` borrow locks out its `^unique ^mut` mutators (`TUR-E0200`)
for ordinary code. But `::` is a coercing cast: `(:: w :int)` unwraps the backing
handle and `(:: int RGWorld)` reconstructs an alias, so a caller that deliberately
does that -- or uses inline-C -- can despawn inside the region. That is the same
escape hatch inline-C already is and the same trusted boundary `#reads` rests on;
a hard adversarial guarantee would need a language feature (module-private
construction / a `::`-sealed newtype). Filed as
`docs/reported/frozen-region-aliasing-via-coercing-cast.md` (now archived at
`docs/archive/frozen-region-aliasing-via-coercing-cast.md`).
