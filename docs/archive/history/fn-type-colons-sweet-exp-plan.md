---
title: "fn-type Colon Codemod -- Sweet-Exp Coverage -- Plan"
category: Tooling (Plan)
description: Close the documented gap in tools/rewrite_fn_type_colons.py so it rewrites (fn ...) inner-type colons inside sweet-exp top-level forms (.tur.sweet sources and ```sweet-exp doc blocks), not just paren-delimited declarations.
---

# Plan: fn-type Colon Codemod -- Sweet-Exp Coverage

**Status: LANDED (PR [#322](https://github.com/turmeric-lang/turmeric/pull/322), commit `f8c12dba`, 2026-06-10).**
All phases S1-S5 shipped: slot processors refactored to `_items` form,
`_walk_implicit_seq` added with sweet-exp auto-detection + `force_sweet`,
`.tur.sweet` files and ` ```sweet-exp ` markdown fences routed through it,
corpus cases (`sweet-basic`, `sweet-effects`, `sweet-lambda-untouched`,
`sweet-md`) added with idempotency checks, and the docs/guides + stdlib
re-sweep is a clean no-op (`check-guide-pairs.py` reports 0 failed).
A follow-up plan covers `definstance`/`defclass`/`defprotocol` method
signatures, which the current `_walk_implicit_seq` does not reach (still
"missed, never corrupted"). Retained here for historical context.

This plan closes the **known gap** recorded in the "Drop leading colons
inside `(fn ...)` types" Phase 2 work (see
[still-in-flight-plan.md](../still-in-flight-plan.md) and PR
[#270](https://github.com/turmeric-lang/turmeric/pull/270)): the
`tools/rewrite_fn_type_colons.py` codemod only finds `(fn ...)` *type*
expressions that sit inside a **paren-delimited declaration**. A
*sweet-exp* top-level form -- `defn foo [...] ...` with no enclosing
parens -- is not recognised, so fn-type inner colons in `.tur.sweet`
sources and ` ```sweet-exp ` documentation blocks are left on the legacy
spelling.

This is the inner-type analogue of the work
`tools/spaced-types-rewrite.py` already did for outer annotations: that
tool carries a sweet-exp `_walk_implicit_seq` pass and a sibling
`spaced-types-rewrite-md.py` for fenced blocks. We port the same shape
to the fn-type codemod.

---

## Motivation

The fn-type codemod rewrites the redundant leading colon on inner
parameter/result types of an `(fn ...)` type:

```turmeric
(defn either-map [^fat f : (fn [int] :int) e : int] : int ...)
;; ->
(defn either-map [^fat f : (fn [int] int) e : int] : int ...)
```

It is **position-driven**: `walk()` dispatches on paren `Form` heads
(`defn`/`fn`/`defstruct`/`let`/`defalias`/`definstance`/`::`/`the`) and,
at each genuine *type slot*, rewrites any `(fn ...)` it finds. That works
for traditional s-expression sources, which is why the v0.18.x sweep of
`stdlib/` and `docs/guides/` succeeded.

In sweet-exp, the outer declaration form drops its parens and is laid out
by indentation:

```
#lang sweet-exp

defn run-twice [f :(fn [] #{e} :int)] #{e} :int
  {f() + f()}
```

Here the top-level `defn` is **not** a paren `Form` -- it is a flat run
of sibling nodes (`defn`, `run-twice`, the `[...]` bracket, `#{e}`,
`:int`, then the body block). `walk()` never sees a `defn`-headed paren,
so it never descends to the parameter's type slot. The `(fn [] #{e} :int)`
sub-form *is* paren-wrapped, but when `walk()` reaches it standalone it
matches `fn` (in `DEFN_HEADS`) and treats it as a **lambda value**, whose
fused `:int` return type is correctly left untouched. Net effect: the
inner colon is **missed** (an under-rewrite), never corrupted.

We hit this concretely during the guide sweep: the
`effects-system-guide.md` ` ```sweet-exp ` toggle counterpart of a
` ```turmeric ` block had to be fixed **by hand** to keep the toggle pair
byte-equivalent. That manual patch is the smell this plan removes.

### Why "missed, not corrupted" matters

The gap is purely a coverage hole, so there is **no correctness urgency**
-- the parser still accepts the legacy `:int` spelling (Phase 1 landed
the lenient path). This plan is about *completeness* so the eventual
Phase 4 (removing the lenient `F_KEYWORD` branch) can rely on a clean
ecosystem, and so `.tur.sweet` sources and sweet-exp doc blocks migrate
without hand-editing.

---

## Current State

### The tool

`tools/rewrite_fn_type_colons.py`:

- `tokenize` / `parse` -- shared shape with `spaced-types-rewrite.py`;
  preserves trivia verbatim; treats ` ``` ... ``` ` fences and strings as
  opaque.
- `walk(form)` -- dispatches on paren `Form` heads to per-construct
  handlers (`_process_signature`, `_process_struct`, `_process_let`,
  `_process_instance`, `_process_alias`, `_process_ascribe`).
- `rewrite_fn_type` / `_strip_slot_seq` / `process_type_slot` -- the
  inner-type rewriter; handles `:kw`, bare `: (T)`, `#{}` effect sets,
  and nested `(fn ...)`; never strips a non-fn type application's own
  args.
- `rewrite_markdown` -- rewrites only fences whose info string is in
  `_TUR_INFO = {turmeric, tur, scheme, lisp, clojure}`. **`sweet-exp` is
  deliberately excluded** today.
- `rewrite_source` -- tokenises, parses, walks top-level `Form`s. There
  is **no** implicit-sequence pass.

### The reference implementation (spaced-types)

`tools/spaced-types-rewrite.py` already solved the structurally identical
problem for outer annotations:

- `_walk_implicit_seq(nodes, ...)` (~line 789) scans a *flat* sibling
  sequence for `defn`/`defmacro`/`fn`/`defstruct`/`let`/`let*`/`loop`
  head symbols and applies the same param-vec / return-type / binding-vec
  rewriters to the items that follow.
- `rewrite_source` detects sweet-exp via a `#lang sweet-exp`
  `TK_LANG_LINE` token (~line 877) and runs `_walk_implicit_seq` in
  addition to the paren walk.
- `iter_files` already includes `.tur.sweet`.
- `spaced-types-rewrite-md.py` is a thin wrapper that imports
  `rewrite_source` as a library and pipes each fenced block (including,
  notably, the `TURMERIC_LANGS` set) through it.

So the fn-type codemod can follow the same blueprint nearly line-for-line.

---

## Design

### Refactor: slot processors usable from both walks

Today the per-construct handlers take a `Form` and call
`code_children(form)`. Factor the *body* of each handler so its
slot-finding logic operates on a **list of code items** (the
`(orig_index, node)` pairs) plus a start index, independent of whether
those items came from a `Form`'s children or from a flat top-level
sequence. Concretely:

- `_process_signature_items(items, start, *, has_name)` -- the param-vec
  + return-type slot logic, returning the index where the body begins.
- Keep `_process_signature(form, ...)` as a thin adapter that calls the
  `_items` variant with `code_children(form)`.

This mirrors how `spaced-types` shares `rewrite_param_vec` /
`rewrite_struct_field_vec` / `rewrite_let_binding_vec` between its two
walks.

### Add `_walk_implicit_seq`

Port the spaced-types pass, but call the fn-type slot processors:

1. Filter the node list to non-trivia `(idx, node)` items.
2. Scan for a head **symbol** whose value is in `DEFN_HEADS`,
   `STRUCT_HEADS`, `LET_HEADS`, or `ALIAS_HEADS`.
3. For each, consume the optional name (defn/defmacro/defstruct/named-let)
   and the leading bracket(s), running the matching slot processor over
   them; then the return-type slot; then continue scanning. The body
   block (subsequent indented sibling forms) is walked by the normal
   paren walk, since each body *expression* is still paren/bracket
   delimited.

Brackets and the `(fn ...)` type forms themselves are still explicitly
delimited in sweet-exp, so only the **outer declaration head** needs the
flat-sequence treatment; the inner rewriter is unchanged.

### Sweet-exp detection in `rewrite_source`

Add, after the paren walk:

```python
is_sweet = any(
    isinstance(t, Tok) and t.kind == TK_LANG_LINE and "sweet-exp" in t.text
    for t in tree if isinstance(t, Tok)
)
if is_sweet:
    _walk_implicit_seq(tree, stats)
```

Give `rewrite_source` an optional `force_sweet=False` parameter so
callers (the `.tur.sweet` file path and the markdown ` ```sweet-exp `
fence path, neither of which necessarily carry a `#lang` line) can opt
in explicitly.

### `.tur.sweet` files

`iter_files` already yields `.md`; extend it (or special-case in `main`)
so `.tur.sweet` files are routed through `rewrite_source(...,
force_sweet=True)`. A `#lang sweet-exp` header, when present, makes the
auto-detection redundant but harmless.

### Markdown `sweet-exp` fences

In `rewrite_markdown`, when the fence info string is `sweet-exp` (or
`sweet`), call `rewrite_source(body, force_sweet=True)` rather than
skipping. Keep the existing `_TUR_INFO` set for the traditional fences.
This is what would have let the tool fix the `effects-system-guide.md`
toggle counterpart automatically.

### What stays out of scope

- **Nested sweet-exp lambdas in bodies.** A `fn [x : int] : int ...`
  *value* nested by indentation inside a body is still a value, not a
  type; the implicit-seq pass only rewrites type slots, so it will not
  touch these (and must not). Document the same "missed, never
  corrupted" guarantee for any indentation-nested type form the flat scan
  does not reach, and add a corpus case proving non-corruption.
- **Neoteric / curly-infix.** Type annotations never appear inside `f(x)`
  or `{a + b}`, so these readers need no special handling.

---

## Phases

### Phase S1 -- Refactor slot processors to `_items` form

- [x] Extract `_process_signature_items` (and, if needed,
      `_process_struct`/`_process_let` item variants) so slot-finding is
      decoupled from `Form`.
- [x] Keep the existing `Form`-based handlers as adapters; no behaviour
      change. Re-run `tests/codemod/run-fn-type-colons.sh` -- still 4/4.

### Phase S2 -- Implicit-sequence pass + detection

- [x] Add `_walk_implicit_seq`; wire it into `rewrite_source` behind
      sweet-exp auto-detection plus a `force_sweet` flag.
- [x] Route `.tur.sweet` files through `force_sweet=True` in
      `iter_files`/`main`.

### Phase S3 -- Markdown `sweet-exp` fences

- [x] Teach `rewrite_markdown` to rewrite `sweet-exp`/`sweet` fences via
      `force_sweet=True`, leaving prose and non-Turmeric fences alone.

### Phase S4 -- Corpus + regression

- [x] Add sweet-exp corpus cases under
      `tests/codemod/fn-type-colons/` using `.tur.sweet` before/after
      pairs (extend `run-fn-type-colons.sh` to also accept
      `before.tur.sweet`/`after.tur.sweet`):
  - `sweet-basic` -- `defn` param + return fn type, indentation body.
  - `sweet-effects` -- `#{e}` effect set in the fn type.
  - `sweet-lambda-untouched` -- a top-level sweet `defn` whose body is a
    `fn [...] ...` lambda value; asserts the value is **not** rewritten.
  - `sweet-md` -- a small `.md` with paired ` ```turmeric ` /
    ` ```sweet-exp ` blocks; asserts both sides converge.
- [x] Confirm idempotency (`--check` clean on every `after.*`).

### Phase S5 -- Re-sweep + de-manualise

- [x] Re-run the codemod over `docs/guides/` including `sweet-exp`
      fences; verify it now reaches the `effects-system-guide.md`
      counterpart that was hand-patched, and that the result is a no-op
      (already migrated) -- i.e. the tool would have produced the same
      bytes.
- [x] Run `python3 tools/check-guide-pairs.py --tur build/tur
      docs/guides/` -- 0 pairs failed.
- [x] Sweep any `.tur.sweet` sources in `stdlib/`, `examples/`, and
      (when present) `../turmeric-spices/`.
- [x] Update the Phase 2 bullet in `still-in-flight-plan.md`: drop the
      "Known gap" note and mark the sweet-exp coverage done.

---

## Validation

- `tests/codemod/run-fn-type-colons.sh` -- all corpus cases (s-expr +
  sweet) pass; tool is idempotent.
- `bash tests/run.sh` -- still `0 failed`. (The rewrite is
  annotation-spelling only; codegen snapshots stay byte-identical, as in
  the original sweep.)
- `tools/check-guide-pairs.py` -- 0 pairs failed before and after.
- Grep gate: after the sweep, no ` :(fn \[` / `: (fn \[` ... `:`-inner
  occurrences remain in swept sweet-exp sources/blocks (excluding
  `docs/archive/` and intentionally-legacy `tests/fixtures/` sources).

## Risks and edge cases

- **Flat-scan over-reach.** The implicit-seq scan must stop consuming at
  the right boundary so it does not mistake a body symbol for a new
  declaration head. Mirror the spaced-types boundary handling exactly and
  cover it with the `sweet-lambda-untouched` corpus case.
- **Mixed files.** A file can interleave traditional and sweet forms
  (rare, but legal). Running both the paren walk and the implicit-seq
  walk over the same tree must not double-rewrite -- the `rewritten`
  flag on `Tok` already guards keyword stripping; verify the
  bare-`:`-deletion path is likewise guarded (it sets `rewritten=True`).
- **Indentation-nested type forms.** If a fn *type* appears only reachable
  by deeper indentation than the top-level scan visits, it will be
  missed. Acceptable under the "missed, never corrupted" guarantee;
  note it and revisit only if real sources need it.

## References

- `tools/rewrite_fn_type_colons.py` -- the tool to extend.
- `tools/spaced-types-rewrite.py` (`_walk_implicit_seq`, sweet detection)
  and `tools/spaced-types-rewrite-md.py` -- the blueprint.
- `tests/codemod/run-fn-type-colons.sh` + `fn-type-colons/` corpus.
- [still-in-flight-plan.md](../still-in-flight-plan.md) -- "Drop leading
  colons inside `(fn ...)` types", Phase 2.
- The CLAUDE.md "Sweet-Expression Style" section -- the syntax being
  migrated.
