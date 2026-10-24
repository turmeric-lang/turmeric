# `let*` duplicates `let`, and the quickstart says `let` is not sequential

**Severity:** low. No wrong code: a redundant special form, and one guide that
teaches the opposite of what `let` does.

**Status:** open. Filed 2026-10-24 while asking whether `let` should become
`let*` and `let*` be deprecated. The answer to the first half is that it
already is; this report is the cleanup that follows.

**Scope: the Turmeric dialects only (`turmeric`, `saffron`, and their
readers).** `#lang r7rs` must keep to the spec -- `let` parallel, `let*`
sequential -- and nothing here changes it.

## What is true today

- **`let` is already sequential.** Each value sees the names bound before it:
  `(let [x (+ n 1) y (* x 2) z (- y x)] (+ x (+ y z)))` compiles and gives 16
  for `n = 3`. The `let` docstring says so (`stdlib/forms/control.tur`).
- **`let*` is a spelling of the same thing.** `elab_letstar`
  (`src/compiler/elab_forms.c:2887`) rewrites `(let* [a 1 b 2] body)` to
  `(let [a 1] (let [b 2] body))` and hands it to `elab_let`. Its docstring
  already says the two "behave the same" and that it exists "for code written
  in the Scheme style".
- **Neither costs anything at run time.** The emitted C differs only in block
  structure -- `let` declares its locals in one block, `let*` nests one block
  per binding with a few extra temporaries -- which a C compiler reduces to
  the same code. Deprecating `let*` is therefore purely a language-surface
  decision.
- **r7rs does not route through Turmeric `let*`.** `lower_letstar`
  (`src/compiler/scheme_lower.c:2567`) lowers Scheme's `(let* ((a 1)) ...)`
  straight to nested Turmeric `let`s. Only a *vector*-form `(let* [ ... ])`
  inside an r7rs file falls through to the Turmeric form
  (`scheme_lower.c:2570`, "Turmeric let*").

## Proposed direction

Deprecate `let*` in the Turmeric dialects in favour of a single sequential
`let`:

- A lifecycle warning from `elab_letstar` ("`let*` is `let`; write `let`"),
  **not** emitted for an r7rs program -- the vector-form passthrough above
  must stay silent there, or be left alone entirely.
- `let*`'s docstring and `binding-forms-guide`'s `let*` section say it is
  deprecated; the form keeps working until a removal is decided.
- Existing uses to migrate: no stdlib code (the only stdlib hits are
  `let*`'s own docs in `stdlib/forms/control.tur` and the generated
  `stdlib/docstrings.tur`). Five fixtures: `let-star`,
  `letstar-typed-binding-spaced`, `let-bound-generic-call-result-keeps-its-tyvar`,
  `hamt-lisp-show`, `hkt-free-stdlib`. The first two test `let*` itself and
  should stay until removal, then expect the warning.

## 1. Guides: teach the sequential `let`, not nested `let`s

The house style is one sequential `let`, not a tower of nested ones.

- **`docs/guides/quickstart.md:77-78` is wrong.** It says: "All bindings in
  the same `let` share the same scope; they cannot refer to each other. Nest
  `let` forms when you need sequential bindings." Both sentences are false.
  Replace with: each binding sees the ones before it, so one `let` does the
  job, with an example like `(let [x 10 y (* x 2)] (+ x y))` (and its
  sweet-exp pair).
- `docs/guides/binding-forms-guide.md:235-262` (`let*` -- the Scheme spelling
  of `let`): mark it deprecated alongside the warning; it already tells new
  code to write `let`.
- `docs/guides/formatter-guide.md:89` lists `let*` among the forms it
  aligns; keep while the form exists.
- Lists of special forms (`syntax-guide.md:1041`, `vim-guide.md:61,127`,
  `compiler-internals.md:276`, `elaborator-internals-guide.md:67`) stay
  accurate as long as `let*` parses; revisit on removal.
- **Not a target:** `async-await-guide.md:336` recommends a nested `let` to
  *end a binding's scope before an `await`*. That nesting is about scope, not
  sequencing, and is correct.

No `web/` tutorial, the tour, or the README repeats the quickstart's claim
(checked: `web/tutorials.js`, `web/tour/`, `README.md`).

## 2. Nested `let`s that could be one sequential `let`

A `let` whose **entire body is another `let`** can be flattened with no change
in meaning:

```turmeric
(let [a (f x)]                (let [a (f x)
  (let [b (g a)]        =>          b (g a)]
    (h a b)))                   (h a b))
```

Found with a small scanner (outer `let` whose only body form is a `let`;
inline-C fences, strings and comments skipped; sweet-exp files not scanned):

| Where | Chains | Notes |
| --- | --- | --- |
| stdlib (not `r7rs/`) | 9 | `sized.tur:402,406`, `sized-matrix.tur:388`, `range.tur:532`, `session.tur:113,151,152,177`, `map.tur:967` (inside a macro template) |
| benchmarks | 4 | `hamt-bench-{transient,lookup,snapshot,insert}.tur` |
| guides (```turmeric blocks) | 31 | most in `session-types-guide.md` (8); also tourist-web-stack, structs (3 each), substructural-types, structured-concurrency, error-handling, channels-and-select (2 each), and one each in sized-types, opaques, httpd, developing-spices, c-integration, binding-forms, backtracking, arrows, advanced-type-system-rationale |
| test fixtures | ~1030 in 373 files | see below |

Counts include the inner links of a chain (a three-deep chain counts twice).

**Linear and session code flattens safely.** The session examples re-bind a
linear channel step by step (`ch` -> `(send ch v)` -> `[r ch] (recv ch)`),
the biggest group above. `session-echo-rpc` rewritten with every chain
flattened (destructuring `[r ch]` included) compiles and prints the same
`10 20 30`, and a flat `let` that reuses a consumed channel is still
`TUR-E0005 use-after-move`. So linearity is checked the same way.

**When not to flatten:**

- The inner `let` is not the outer one's only body form (statements run
  between the bindings). Merging would move them.
- The nesting ends a scope on purpose: a binding that must be released, or
  must not be live, before the rest runs (the async guide's `await` case; a
  `ref` / `rc` / owning binding whose release point matters).
- A named let (`(let loop [...] ...)`) on either side.
- **Test fixtures**, as a rule. They pin behaviour, several exercise nesting
  itself (scope, shadowing, drop order), and a rewrite moves `expected.c`
  snapshots. Flatten one only when the fixture's subject is unrelated to
  binding structure and it is being edited anyway.

Suggested order: the quickstart fix first (it is actively wrong), then the
guides' examples (readers copy them), then stdlib and benchmarks, regenerating
any snapshot that moves in the same change.
