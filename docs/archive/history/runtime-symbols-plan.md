# Runtime Symbols (`:Sym`) -- Plan (SYM0--SYM6)

> **Status:** Complete -- SYM0--SYM6 all implemented (type machinery, per-TU
> codegen, cross-TU interning, map-literal + typeclass integration, stdlib
> surface, dynamic `str->sym`, docs + fixtures: `sym-eq-basic`, `sym-stdlib`,
> `sym-map-key`, `sym-dynamic`, plus a `build-project-sym-cross-tu` project
> test). Two phases deviate from the original design below (see their "As built"
> notes): **SYM2** uses external weak linkage rather than a per-build
> `symbols.c` aggregator + `.tur-syms` manifests; **SYM5** ships `str->sym` as a
> load-on-demand module (`stdlib/sym-dynamic.tur`) backed by a process-global
> intern table (`src/runtime/symbols.c`), seeded from the static literal records
> by a codegen-emitted startup constructor.
>
> Reader already produces `F_KEYWORD` forms and the
> compiler already interns the name via the `Symbol` table; this plan adds a
> runtime value type so `:foo` becomes a first-class expression whose value is
> a pointer-identity-equal interned symbol.
>
> **Prerequisites:** None at the type-system layer. The map-literal integration
> (SYM3) builds on the existing `-Xdata-literals` lowering in
> `elab_toplevel.c:dl_normalize_map_key`.
>
> **Flag:** `-Xsymbols`. All phases are gated so the work can land
> incrementally without disturbing the existing syntactic uses of `:foo`.
>
> **Last updated:** 2026-05-31

---

## Motivation

`:foo` syntax already exists in Turmeric -- it parses cleanly, the symbol name
is interned at read time, and the form (`F_KEYWORD`) is consumed in a number
of syntactic positions:

- Type annotations: `:int`, `:cstr`, `:Sym`
- Import directives: `:refer [...]`, `:as foo`
- Struct field references: `(:name p)`
- Map-literal keys: `#map{:foo 1}`
- ADT/data-constructor tags in some surfaces

What does **not** exist today is a runtime value for a keyword. In expression
position `:foo` has no type and no representation; `#map{:foo 1}` decays the
keyword to a string literal at elaboration time
(`elab_toplevel.c:dl_normalize_map_key` lowers `F_KEYWORD` to
`(hamt/hash-str "foo")`), so even map keys are really hashed strings, not
interned symbols.

This plan adds `:Sym`, a first-class runtime symbol type. Two distinct
keywords with the same name are guaranteed to be the **same pointer**
(content-and-identity-equal), so equality and hashing are pointer operations
-- no string compare, no allocation, no runtime intern table on the hot path.

Goals:

- Make `:foo` a valid expression of type `:Sym` whose runtime value is a
  unique, deduplicated pointer.
- Replace the `hamt/hash-str` decay path in map literals so map keys can be
  typed `:Sym` end-to-end with pointer-equal hashing.
- Provide stdlib `Hash`/`Eq` instances for `:Sym` plus a `sym->str` accessor.

Non-goals for the prototype:

- A general user-extensible reader-tag system (`'foo`, `#'foo`, etc.).
- Garbage-collected dynamic symbols. Symbols are static, process-lifetime
  values; the dynamic-construction helper in SYM5 is opt-in.
- Replacing the existing syntactic uses of `:foo` -- annotations, `:refer`,
  field accessors, etc. all keep working unchanged.

---

## Background: What Is Already Shipped

| Component | Location | Notes |
|---|---|---|
| `F_KEYWORD` reader tag | `src/compiler/forms.h:112` | Reader emits `F_KEYWORD` with the interned `Symbol*` payload. |
| `Symbol` table | `src/compiler/symtab.*` | Compile-time string interning; the same `Symbol*` is shared across all references to `:foo` in one compilation unit. |
| `form_keyword` constructor | `src/compiler/forms.c:48` | Allocates the form node in the arena. |
| Keyword consumers (compile-time) | `src/compiler/elab_macros.c` (5 sites), `elab_module.c:410+`, `elab_typeclasses.c:440+` | All current uses inspect `F_KEYWORD` in non-expression positions (annotations, options, headers). |
| Map-literal lowering | `src/compiler/elab_toplevel.c:169` (`dl_normalize_map_key`) | Currently rewrites `F_KEYWORD` keys to `(hamt/hash-str "foo")`. SYM3 replaces this path. |
| `Hash` typeclass | `stdlib/typeclass-hash.tur` | Provides `Hash[int]`, `Hash[cstr]`, etc. Adding `Hash[Sym]` slots in alongside. |

The reader and interning work are effectively free for this plan; the new
work is a runtime representation, a codegen path, a type, and the map-key
integration.

---

## Design Decisions

### Runtime Representation

A `:Sym` value is a non-null `const struct __tur_sym *` pointer to a static,
process-lifetime record in `.rodata`:

```c
struct __tur_sym {
    uint64_t hash;   /* xxHash64 of name; precomputed at codegen */
    uint32_t len;    /* byte length, excluding NUL */
    uint32_t _pad;
    char     name[]; /* NUL-terminated UTF-8 */
};
```

Equality is `a == b`. Hashing reads the `hash` field directly. `sym->str`
returns `&s->name[0]` as `:cstr`. The header is precomputed by the compiler
and emitted once per distinct keyword.

Rationale for the header form (vs. a bare `const char *`):

- `Hash[Sym]` is a single load, not a hash-of-pointer (which loses string
  identity across processes -- relevant for any future serialization story).
- `sym->str` is free; printing and debug output don't need a strlen.
- The pointer is still unique per name, so eq is still `==`.

### Symbol Type (`TY_SYM`)

A new TypeKind `TY_SYM` is added. It is a nullary type (no parameters), prints
as `:Sym`, and lives alongside `TY_CSTR`, `TY_BOOL`, etc. The parser already
treats `:Sym` as a type annotation; the type elaborator gains a single new
case mapping that token to `TY_SYM`.

`Sym` is **not** a subtype of `cstr`. Conversion is explicit via `sym->str`.

### Codegen-Time Interning

Within a single translation unit, every distinct `:foo` lowers to a reference
to one static record:

```c
static const struct {
    uint64_t hash;
    uint32_t len;
    uint32_t _pad;
    char     name[4];
} __tur_sym_foo = { 0xDEAD..., 3, 0, "foo" };

#define __TUR_SYM_FOO ((const struct __tur_sym *)&__tur_sym_foo)
```

The compiler walks the `Symbol` table at the end of codegen and emits one
record per keyword that appeared in the unit. The `name` field is mangled
(`__tur_sym_` + percent-encoded UTF-8) so the C identifier is unique and
ASCII-only even for keywords containing punctuation.

### Cross-TU Interning

A single program may include many `.c` files emitted by `tur build`. Two
strategies were considered for ensuring that `:foo` defined in TU A and
`:foo` defined in TU B share the same pointer:

| Strategy | Pros | Cons |
|---|---|---|
| Per-build `symbols.c` aggregator | Single source of truth, no linker quirks, smaller code. | Build needs a final pass that scans all emitted units for referenced keywords; layering breaks if a TU is compiled in isolation. |
| Weak-linkage records in every TU | Each TU is self-contained; linker folds duplicates via `weak`/`linkonce_odr`. | Relies on `__attribute__((weak))` / `selectany` semantics being uniform across the supported toolchains; folding is per-name, so the mangling must be stable. |

**Decision: per-build `symbols.c` aggregator.** Turmeric's build already
orchestrates the C output and the `tur build <dir>` flow has a natural place
to emit a final aggregated unit (next to the existing globals/runtime files).
Per-file isolated compilation (`tur emit-c <file>`) keeps its TU-local
records as `static`, which is fine because such output is for inspection and
single-binary builds, not multi-TU linking.

### Map-Literal Integration

`elab_toplevel.c:dl_normalize_map_key` currently has three branches:

```c
if (F_INT)     -> pass through
if (F_KEYWORD) -> (hamt/hash-str "name")
else /*F_STR*/ -> (hamt/hash-str <str>)
```

Under `-Xsymbols`, the keyword branch instead emits a `:Sym` literal:

```c
if (F_KEYWORD) -> sym-literal "name"   ;; produces :Sym at type level
```

`Hash[Sym]` is a one-field load (already precomputed), so the map's hashing
contract is unchanged. The map's key type becomes `:Sym` rather than the
opaque hashed `:int`, which means `(get m :foo)` type-checks against the
declared key type instead of relying on the elaboration-time rewrite.

The string-key branch is left exactly as-is; this is not a refactor of map
literals, only a change to how keyword keys flow.

### Relationship to `:foo` in Syntactic Positions

Adding a runtime value for `:foo` does **not** change any of its current
syntactic uses:

- Type annotations live in annotation slots that the elaborator inspects
  before expression elaboration.
- `:refer`, `:as`, struct-field selectors, and ADT tags are consumed by
  specific elaborator passes that already match `F_KEYWORD` directly.

The only behavioral change for existing programs is in **expression
position**, where `:foo` was previously a syntax error (or silently misused);
under `-Xsymbols` it now type-checks as `:Sym`.

### Interaction with `Hash` / `Eq` Typeclasses

Two new instances are needed:

```turmeric
(definstance Hash [Sym] (hash [x] : int
  ```c
  return (int64_t)((const struct __tur_sym *)x)->hash;
  ```))

(definstance Eq [Sym] (eq? [x y] : bool
  ```c
  return (int64_t)(x == y);
  ```))
```

Both inline-C; no allocation, no string compare.

---

## Architecture Overview

```
   :foo  (reader)
     |
     v
   F_KEYWORD form  (already has interned Symbol*)
     |
     +-- syntactic position?  ---> existing elaborators (unchanged)
     |
     +-- expression position?
              |
              v
        elab_keyword_expr  (SYM0)
              |
              v
        EX_SYM_LIT : TY_SYM
              |
              v
        emit_sym_lit  (SYM1)
              |
              v
        reference to static __tur_sym_<mangled>
              |
              v
        symbols.c aggregator emits one record per keyword (SYM2)
```

---

## Phase SYM0: Type Machinery

Adds the `:Sym` type and an expression-position elaboration for `F_KEYWORD`,
behind `-Xsymbols`. No codegen yet -- a stub that aborts at emit time is
acceptable.

### Changes

- `src/compiler/types.h`: add `TY_SYM` to the `TypeKind` enum; add
  `TYPE_SYM` global singleton; teach `type_print`, `type_eq`, `type_arity`.
- `src/compiler/elab_types.c`: map the `:Sym` annotation token to
  `TYPE_SYM`.
- `src/compiler/elab_core.c`: in the form-elaboration switch, add an
  expression-position `F_KEYWORD` branch (gated on
  `e->flags.symbols_enabled`) that produces a new `EX_SYM_LIT` expression
  carrying the interned `Symbol*`.
- `src/compiler/expr.h`: add `EX_SYM_LIT` with payload `const Symbol *sym`.

### Acceptance Criteria

- `tur check` accepts `(let [x :foo] x)` under `-Xsymbols` with inferred
  type `:Sym`; rejects it without the flag (existing behavior).
- `tur check` still accepts `(:name p)`, `:refer`, type annotations
  unchanged with and without the flag.
- New diagnostic when `-Xsymbols` is off and a keyword appears in
  expression position: "keyword in expression position requires
  `-Xsymbols`".

---

## Phase SYM1: Codegen (per-TU)

Lowers `EX_SYM_LIT` to a reference to a TU-local static record. Output is
self-contained; cross-TU folding comes in SYM2.

### Changes

- `src/compiler/emit_core.c`: add `emit_sym_lit` that:
  - registers the `Symbol*` in a per-emit `seen_symbols` set,
  - emits a stable mangled C identifier (`__tur_sym_` + percent-encoded
    name) referenced as `(const struct __tur_sym *)&__tur_sym_<mangled>`.
- New emit epilogue pass: walk `seen_symbols`, emit one `static const`
  record per entry, each with precomputed `xxHash64(name)` (reuse the
  existing `tur_hamt_hash_str` driver).
- `src/runtime/symbols.h`: define `struct __tur_sym` so user inline-C and
  the typeclass instances can include it.

### Acceptance Criteria

- `tur emit-c` on a single-file program containing `:foo :foo :bar` emits
  exactly two static records.
- `eq?(:foo :foo)` and `(= (hash :foo) (hash :foo))` both hold at runtime.
- Address of `:foo` is identical when produced from two different
  expressions in the same TU.
- New fixture: `tests/fixtures/sym-eq-basic/` verifies pointer equality
  and hash equality.

---

## Phase SYM2: Cross-TU Interning

Promotes the per-TU records into a single aggregated unit so multi-file
builds share one record per keyword across TUs.

### As built (deviation from the design below)

The shipped implementation uses **external weak linkage** rather than the
aggregator design originally sketched in this section:

- `sym_codegen_emit(out, external_weak)` gains a linkage parameter.
  `emit_implementation` (the separate-compilation / `tur build <dir>` path)
  passes `external_weak = true`, so each record is emitted as
  `__attribute__((weak)) const struct { ... } __tur_sym_<mangled> = { ... };`.
  Two TUs that both reference `:foo` emit identical definitions; the linker
  folds the weak duplicates to one object, so `:foo` is a single pointer
  program-wide. `emit_program` (single-file / `emit-c`) passes
  `external_weak = false`, keeping the `static` form so the output stays
  self-contained.
- The generated header forward-declares `struct __tur_sym;` (under
  `-Xsymbols`) so an exported signature with a `:Sym` parameter/result refers
  to a single file-scope tag instead of declaring it in prototype scope.
- `sym_codegen_reset()` is called at the top of `emit_implementation` as well
  as `emit_program`; the registry now owns a `strdup`'d copy of each name
  (the per-TU elaboration arena is freed between TUs, so a borrowed
  `Symbol->name` would dangle).
- Verified by `tests/run-build-project.sh`
  (`build-project-sym-cross-tu` + `...-single-symbol`): a two-module project
  where both modules return `:foo` exits 0 (pointer-equal across TUs) and
  `nm` finds exactly one `__tur_sym_foo`.

No `.tur-syms` manifest, no `symbols.c` aggregator, and no link-list changes
were needed. The original aggregator design is retained below for reference.

### Changes (original design -- not taken)

- `tur build <dir>` (project mode) gains a final emit step:
  - Each TU emits its `seen_symbols` set as a sidecar `.tur-syms` manifest
    next to the `.c` file.
  - A new `symbols.c` aggregator is generated from the union of all
    manifests with one `const struct __tur_sym` per unique keyword,
    `extern`-visible.
  - Per-TU emit switches from `static` storage to `extern` references.
- `tur emit-c <file>` (single-file) keeps the SYM1 `static` form -- the
  output remains a self-contained `.c`.
- The aggregator is included in the build's link list (Justfile recipe and
  the manifest-driven build descent already have a hook for runtime
  glue files).

### Acceptance Criteria

- Building a two-file program where both files reference `:foo` produces a
  single `__tur_sym_foo` symbol in the final binary (verify via `nm`).
- `(eq? :foo :foo)` returns true even when the two references live in
  different TUs.
- Single-file `tur emit-c` output still compiles and runs in isolation.

---

## Phase SYM3: Map-Literal Integration

Replaces the `(hamt/hash-str "name")` decay in `dl_normalize_map_key` so
keyword map keys are first-class `:Sym` values.

### As built

The shipped implementation matches the design with two additions the original
plan did not anticipate:

- **Typeclass dispatch had to learn `TY_SYM`.** `eq?`/`hash` (and the map's
  `Hash[K]`/`MapKey[K]` resolution) classify the receiver's `TypeKind` as
  `KIND_STAR` (nullary, exact-match) vs `KIND_ARROW` (constructor). `TY_SYM`
  was missing from every "is primitive" set, so a `:Sym` receiver was treated
  as `KIND_ARROW` and matched the *first* non-primitive instance (e.g.
  `MutableMap`) -- a mis-dispatch that crashed. `TY_SYM` is now in the
  primitive sets in `elab_typeclasses.c` (dispatch) and `typeclass.c`
  (by-key lookup), and in every instance-name `type_suffix` switch
  (`elab_typeclasses.c`, `emit_stmt.c`, `emit_core.c`) so the instance is named
  `__inst_<Class>_<m>_Sym` rather than the fallback `__inst_..._T`.
- **The Sym instances live in `sym.tur`, not the per-class files.** Instance
  dict singletons are emitted unconditionally for every *registered* instance,
  and their inline-C references `struct __tur_sym`. Putting `Eq[Sym]` /
  `Hash[Sym]` / `MapKey[Sym]` in the always-loaded `typeclass-eq.tur` /
  `typeclass-hash.tur` / `map.tur` would therefore force `struct __tur_sym`
  (and dead instance code) into *every* program and churn all codegen
  snapshots. Instead all three live in `sym.tur` (auto-loaded only under
  `-Xsymbols`), and `builtin_kind_home_basename` credits `TY_SYM` to
  `sym.tur` so they pass the orphan-instance check there. Non-`-Xsymbols`
  builds are untouched (zero `__tur_sym` references).
- `MapKey[Sym]` boxes the symbol pointer as the int carrier and uses the
  plain integer (identity) comparator `tur_int_carrier_eq_` -- correct because
  interned symbols compare by pointer. `EX_REINTERPRET` (the map carrier
  box/unbox) now accepts `TY_SYM` as a pointer-sized scalar.

Verified: `#map{:foo 10 :bar 20}` keyed by `:Sym`, `map-get` / `map-has?`,
and bare `(eq? :foo :foo)` / `(hash :foo)` all dispatch correctly (fixture
`tests/fixtures/sym-map-key`). The original design notes follow.

### Changes

- `src/compiler/elab_toplevel.c:dl_normalize_map_key`: under `-Xsymbols`,
  the `F_KEYWORD` branch returns a Sym literal form rather than synthesizing
  a `hamt/hash-str` call.
- `stdlib/typeclass-hash.tur`: add `Hash[Sym]` and `Eq[Sym]` instances
  (one-line inline-C each).
- The map's key type inferred from `#map{:foo 1 :bar 2}` becomes
  `Map[Sym, V]` rather than the current `Map[int, V]`-via-hash.

### Backwards Compatibility

- Without `-Xsymbols`, map literals retain the existing `hamt/hash-str`
  lowering. A program written against the old behavior compiles unchanged.
- The string-key branch (`#map{"foo" 1}`) is untouched in either mode.

### Acceptance Criteria

- `(get #map{:foo 1} :foo)` returns `1` and is typed
  `Option[int]` over `Map[Sym, int]`.
- `(eq? :foo (first (keys #map{:foo 1})))` returns true (pointer-equal).
- Existing map-literal fixtures continue to pass with `-Xsymbols` off.
- A new fixture `tests/fixtures/sym-map-key/` exercises the typed surface.

---

## Phase SYM4: Stdlib Surface

Adds the small set of stdlib helpers that make `:Sym` ergonomic.

### Changes

- `stdlib/sym.tur` (new):
  - `(sym->str s :Sym) :cstr` -- returns the embedded name (no copy).
  - `(sym=? a :Sym b :Sym) :bool` -- inline-C pointer compare; convenience
    over `eq?` for code that does not want to go through the typeclass.
  - Module docstring + per-defn docstrings per the `;;;` standard.
- `tools/gendocs.py` picks the new module up automatically; no special
  handling needed.

### Acceptance Criteria

- `(sym->str :hello)` returns `"hello"` with no allocation (verify by
  reading the same pointer twice across calls).
- Fixture: round-trip a Sym through a function parameter typed `:Sym` and
  confirm pointer identity is preserved.

---

## Phase SYM5: Dynamic Construction (Opt-In)

The static interning model covers literal keywords. SYM5 adds an opt-in
helper for constructing symbols from strings at runtime -- useful for
deserialization or REPL-style tools.

### As built

- `src/runtime/symbols.{c,h}`: a process-global, mutex-guarded open-addressed
  table. `tur_sym_intern(s, len)` returns an existing record or allocates a
  fresh process-lifetime one; `tur_sym_register(rec)` registers a static
  record (first name wins). The hash matches codegen via `tur_hamt_hash_str`.
- **`str->sym` lives in `stdlib/sym-dynamic.tur`, not `sym.tur`, and is *not*
  auto-loaded.** Load it with `(load "stdlib/sym-dynamic.tur")`. This keeps the
  literal-only surface (`sym.tur`) free of any runtime-table dependency:
  programs that only use `:foo` literals never link `symbols.c` and never run a
  startup constructor (verified: a literal-only program emits zero
  `tur_sym_*` references). `symbols.c` is auto-linked (like `hamt.c`) only when
  `str->sym`'s body -- with its `__tur_autolink__` marker -- is emitted.
- **Seeding:** `sym_codegen_emit` emits an `__attribute__((constructor))` that
  calls `tur_sym_register` for each static record, so `str->sym("foo")` returns
  the same pointer as the literal `:foo` (`(eq? :foo (str->sym "foo"))` holds).
  The constructor is gated on `str->sym` being defined in the TU
  (`g_sym_intern_used`, set in `emit_fn_def`), so literal-only programs emit
  none. Weak-folded cross-TU records register idempotently (first wins).
- **Known limitation:** in a multi-module build, `str->sym("foo")` matches a
  literal `:foo` only if that literal appears in a TU that also defines/loads
  `str->sym` (or is weak-folded with such a TU). A `:foo` that lives solely in
  a module which never loads `sym-dynamic.tur` is not seeded, so `str->sym`
  would allocate a distinct record for it. Single-file programs are unaffected.

Verified: `(eq? :hello (str->sym "hello"))`, two interns of the same dynamic
name are pointer-equal, distinct names are distinct, `sym->str` round-trips,
a 1,000,000-iteration intern loop returns one stable pointer, and 8 threads
interning the same name concurrently get one pointer with no TSan data race.

### Changes (original design)

- `src/runtime/symbols.c`: add a process-global hash table guarded by a
  mutex; `tur_sym_intern(const char *s, uint32_t len)` returns a stable
  `const struct __tur_sym *`. Entries allocated by this path live for the
  process lifetime (same lifetime contract as static records, so callers
  cannot tell the difference).
- `stdlib/sym.tur`: add `(str->sym s :cstr) :Sym` thin wrapper.
- Document that `str->sym` is non-deterministic w.r.t. allocation count
  and should not be used in hot paths -- prefer literal keywords.

### Acceptance Criteria

- `(eq? :foo (str->sym "foo"))` returns true.
- A million-iteration loop of `(str->sym "foo")` allocates exactly one
  record.
- Thread-safety: concurrent `str->sym` calls on the same name return the
  same pointer (smoke test under TSan via the existing TSan fixture
  harness).

### Risks / Open Questions

- Whether to expose `str->sym` at all, given the entire point of the
  static path is to avoid runtime allocation. The plan ships it as opt-in
  rather than the default surface. If the runtime intern table proves
  unused, SYM5 can be reverted without affecting SYM0--SYM4.

---

## Phase SYM6: Documentation and Test Suite

- `docs/guides/symbols-guide.md` covering the `:foo` literal, the
  expression-vs-syntactic distinction, the map-literal change, and the
  pointer-identity equality model.
- Cross-link from `docs/guides/data-literals-guide.md` (the map-key
  section gets a one-paragraph note about the SYM3 typing change).
- Cross-link from CLAUDE.md ("Stdlib Layout" gains `sym.tur`).
- Fixture coverage summary:
  - `sym-eq-basic` -- SYM1 pointer equality
  - `sym-cross-tu` -- SYM2 cross-TU folding (multi-file fixture)
  - `sym-map-key` -- SYM3 typed map keys
  - `sym-stdlib` -- SYM4 `sym->str` round-trip
  - `sym-dynamic` -- SYM5 `str->sym` interning (optional)

### Acceptance Criteria

- `bash tests/run.sh` passes with `-Xsymbols` on by default (gated via a
  fixture-level marker once SYM0--SYM3 land).
- `just docs` regenerates the guide and API reference cleanly.

---

## Open Design Questions

1. **Should `:Sym` print with the leading colon?** Printing `:foo` reads
   naturally and round-trips through the reader. Decision: yes.

2. **Should `eq?` short-circuit `:Sym` to pointer compare without going
   through the typeclass?** Marginal speedup at the cost of a special
   case in the elaborator. Decision: no -- let the `Eq[Sym]` instance do
   the work; the typeclass dispatch is itself a static call here.

3. **What is the alternate sigil story?** This plan deliberately does not
   introduce `#:foo` or `'foo`. If a use case for a distinct "value-only"
   sigil emerges later, it can be added without changing the runtime
   model -- it would just produce the same `EX_SYM_LIT`.

4. **Should map literals require `-Xsymbols` to keep their current
   behavior?** No. SYM3 only activates the new lowering when `-Xsymbols`
   is on; without it, the existing `hamt/hash-str` path is unchanged.

5. **Cross-TU manifest format.** `.tur-syms` is sketched as a newline-
   separated list of percent-encoded names. If the build needs structured
   metadata (e.g. precomputed hashes for deterministic builds), upgrade
   to a small s-expression format. Defer until SYM2 is implemented.

---

## Out of Scope

- Generalized reader macros for custom literal syntax.
- Symbol-namespacing (`:foo/bar` as a distinct first-class concept --
  syntactically allowed, but treated as a single opaque name for now).
- Reflection over the symbol table at runtime (`all-symbols`, etc.).
- Garbage collection of dynamically constructed symbols.
