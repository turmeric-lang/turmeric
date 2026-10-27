# Variadic Tuple Type Plan (Option B)

## Status

**Deferred.** This plan is the long-form follow-up to
`docs/tuple-type-plan.md` (Option A, `Tuple2`..`Tuple5` as
`defstruct`-generated nominal types). Do not start this plan until
all of the following are true:

1. Option A has shipped (`stdlib/tuple.tur` with `Tuple2`..`Tuple5`,
   destructure path, `future-join` migrated off `TurTuple2`).
2. At least one of the gating signals below has appeared in real
   code:
   - A spice or stdlib module hits the `Tuple5` cap and asks for
     `Tuple6+`.
   - Error messages of the form `expected Tuple3[...] but got
     Tuple4[...]` become a real source of friction (i.e. arity is
     part of the type identity in a way that obscures the actual
     mismatch).
   - HKT / typeclass code wants to abstract over "any tuple"
     uniformly and can't because each `TupleN` is a distinct nominal
     type constructor.
3. The `Pair` -> `Tuple2` migration sweep called out in the Option A
   plan has either completed, or has been declared not worth doing.
   Either outcome is fine; the point is to avoid landing a third
   tuple representation while two are still co-existing.
   **Resolution:** partial sweep completed in Phase TP2. All stdlib
   internal ad-hoc inline-C "Pair" structs (`stdlib/parsec.tur`,
   `stdlib/seq/*`, `stdlib/comonad.tur`, `stdlib/arrow.tur`) now
   route through the canonical Tuple2 layout, and `Eq [Tuple2]` is
   shipped alongside `Eq [Pair]`.  The typed `Pair[A B]` defstruct
   and its public API (`tpair`, `pair-fst`, `pair-snd`, `pair-eq?`,
   `Eq [Pair]`) are kept as-is for backward compatibility -- the
   original Option A plan explicitly recommended against rip-out.

If none of these fire within ~6 months of Option A shipping, the
correct answer is to close this plan unbuilt. `defstruct` plus
`Tuple2`..`Tuple5` is sufficient for the use cases the language
actually has, and the cost of touching every `Type`-walking site in
the compiler is non-trivial.

## Scope and non-scope

In scope:
- A new primitive type kind `TY_TUPLE` with a variable-length array
  of element `Type *`. Same shape as `TY_VEC`'s parameterisation but
  with N element types instead of one.
- Tuple literal syntax (`(tuple a b c ...)`) lowering to a single
  packed runtime representation regardless of arity.
- Destructuring (`(let [[a b c ...] e] ...)`) routed through the
  general `TY_TUPLE` arm instead of N nominal-struct arms.
- Type-printer / unifier / GADT / typeclass-resolution updates
  needed to keep `Type`-walking code honest.
- Migration of `Tuple2`..`Tuple5` from Option A onto the new
  representation. The user-visible surface (`tuple`, `[[a b] ...]`,
  `:(Tuple3 ...)` annotations) is preserved.

Out of scope:
- HList in the Haskell sense (row-polymorphism / dependent length).
  `TY_TUPLE` is fixed-arity; the arity is a property of the type.
- Anonymous structural records with named fields. That is still a
  `defstruct` concern.
- Replacing `Pair`. That sweep is part of the Option A follow-up and
  should already be settled before this plan starts.
- Generalised pattern matching beyond `let`-with-vector-pattern. If
  `match` is to learn tuple patterns, that is a separate plan.

## Why this is hard

Adding a new `TypeKind` is touching ~every file under
`src/compiler/` that walks `Type`. From a quick survey of the
existing kinds (`TY_VEC`, `TY_SESSION_PAIR`, `TY_UNION`, etc.) the
required arms are at least:

| Site | Why it needs a new arm |
|------|------------------------|
| `src/compiler/types.h` | declare `TY_TUPLE`, element-array field |
| `src/compiler/types.c` (constructor, copy, equality) | build/compare `TY_TUPLE` nodes |
| Type printer (`type_to_string` or equivalent) | render `Tuple[int cstr float]` |
| Unifier (`elab_*.c`) | element-wise unify with arity check |
| GADT / skolem machinery (`SkolemEnv` etc.) | tuple parameters when used as GADT arms |
| Subtyping under `-Xunion-types` / `-Xintersection-types` | tuple is covariant in each element under usual rules |
| Codegen (`emit_*.c`) | one packed struct, layout independent of nominal name |
| Runtime printer / `derive-show` | format `(tuple 1 "x" 3.14)` |
| HKT plumbing (P-series memory) | tuple as a variadic type constructor |
| `Type`-walking utilities (free-tyvar scan, contains-kind, etc.) | recurse into element array |

The Session-pair work (`TY_SESSION_PAIR`) is the closest precedent:
that landed as a single new arm because it's a fixed-arity pair with
a single use site. `TY_TUPLE` does not get that simplification --
its whole purpose is to be variadic, so every walk that wants to
look at "the element type" instead has to loop.

The HKT plumbing (sized types phase status, HKT S1-S8 in memory) has
already done one full sweep of the `Type`-walking code; the cost
estimate here should be calibrated against how long that took, not
against an optimistic "just add an arm" framing.

## Surface design

The user-facing surface is unchanged from Option A. The whole point
of this plan is that the surface is forward-compatible. After
landing:

```turmeric
;; All of these continue to work exactly as in Option A:
(tuple 1 "x" 3.14)
(tuple 1 "x" 3.14 :ok 42 #t)        ; arity > 5 now legal
(let [[a b c d e f] (tuple 1 "x" 3.14 :ok 42 #t)] ...)

(defn split [s : cstr] : (Tuple cstr cstr)
  ...)

;; Tuple3 / Tuple4 / Tuple5 from Option A become type aliases for
;; the variadic form during migration; they may be retired later
;; once the sweep is done.
```

Two surface changes vs Option A:

1. `:(Tuple cstr cstr)` (no arity in the name) is allowed; arity is
   inferred from the element count. The `TupleN` aliases stay valid
   for at least one release.
2. There is no longer a hard cap. `(tuple 1 2 3 4 5 6 7)` typechecks.

## Implementation steps

Numbered roughly in dependency order; expect each to be its own
landed change.

1. **Add `TY_TUPLE` to `types.h`** with an element-`Type *` array
   field and arity. Mirror the layout decision used by `TY_VEC` /
   `TY_FORALL` for variable-length child arrays.
2. **Constructors, copy, structural equality.** Make sure the
   arena-allocation pattern matches existing variadic kinds.
3. **Type printer.** Render `Tuple[int cstr float]`. Make sure the
   `TupleN` aliases print the same way (or distinctly, decide which
   reduces confusion -- probably "same way" so error messages don't
   mix forms).
4. **Unifier.** Two `TY_TUPLE` unify iff arities match and elements
   unify pairwise. Arity-mismatch error message must include both
   tuple shapes.
5. **Codegen.** Decide on one packed struct layout (most likely
   heap-allocated; could try inline for small arities later). The
   Option A `defstruct` layout is the natural starting point --
   confirm that the runtime layout for `Tuple3` under Option A is
   bit-compatible so existing fixtures don't move.
6. **Reroute `(tuple ...)` constructor.** The Option A macro
   currently dispatches to `make-tupleN`. Replace with a single
   `make-tuple` runtime helper that takes an arity. Keep the old
   `make-tupleN` symbols as thin shims during migration.
7. **Reroute the destructure path in `elab_forms.c`.** The Option A
   work generalised the `TY_SESSION_PAIR` arm to recognise
   `Tuple2`/.../`Tuple5` structs. Replace that with a single
   `TY_TUPLE` arm; the `TupleN` struct recognition stays as a
   fallback for code that still uses the alias names.
8. **Migrate `Tuple2`..`Tuple5` to aliases.** `stdlib/tuple.tur`
   becomes a thin alias file rather than a generator. Run the full
   fixture suite -- this is the canary.
9. **HKT / typeclass interaction.** Verify that `Tuple` as a
   variadic type constructor doesn't break the kind checker.
   Partial application (e.g. `Tuple[int, _]`) is the most likely
   surprise -- decide whether it's a kind error or yields a
   curried-in-arity constructor.
10. **Fixtures.** Add `tests/fixtures/typed/tuple-variadic/`,
    `tuple-arity-mismatch/`, `tuple-print/`, `tuple-hkt/`. Confirm
    the Option A fixtures all still pass without modification --
    that is the "no surface regression" gate.
11. **Docs.** Update `docs/guides/`'s tuple section to mention the
    variadic form and the alias status of `TupleN`. No README
    rewrite.

## Risks and open questions

- **Partial application kind.** Does `Tuple[int, _, cstr]` mean
  anything? Likely no -- tuple arity is part of the type. Verify
  the kind checker rejects it cleanly.
- **`TupleN` alias half-life.** Keeping the aliases forever means
  the language has two ways to spell the same type. Removing them
  too soon breaks existing code. Pick a release window up front
  (e.g. "aliases stay through one minor release after Option B
  lands, then are removed").
- **Derive-show formatting.** `(Tuple3 1 "x" 3.14)` (Option A) vs
  `(tuple 1 "x" 3.14)` (Option B). Pick one and apply it
  consistently; the latter matches the constructor syntax and is
  the more honest representation under `TY_TUPLE`.
- **Performance.** `defstruct` already produces an arena struct; the
  question is whether a variadic `TY_TUPLE` has any reason to be
  slower. It shouldn't, but benchmark `future-join`-style hot paths
  before and after the migration to be sure.
- **Cap reintroduction.** If the implementation cost is even higher
  than estimated, a face-saving fallback is to land `TY_TUPLE` with
  an internal arity cap (say, 16) that nobody is expected to hit.
  Document the cap and where to raise it.

## Success criteria

- All Option A fixtures pass unchanged.
- `(tuple 1 2 3 4 5 6 7)` and `(let [[a b c d e f g] ...])` work.
- `Tuple2`..`Tuple5` continue to typecheck wherever they appeared
  pre-migration, as aliases.
- Type errors say `Tuple[...]` with element types listed, not
  `Tuple3` / `Tuple4`.
- `future-join` and other `TupleN` users continue to pass their
  fixtures without code changes.

## When to do this

See **Status** at the top. Short version: only when Option A is
visibly insufficient, and only after the `Pair` -> `Tuple2` sweep
has been resolved one way or the other. If `defstruct`-backed
`Tuple2..Tuple5` carry the language for a year without complaint,
this plan should be closed unbuilt -- the cost of touching every
`Type`-walking site is too high to pay on speculation.
