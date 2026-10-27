# Letrec and Named-Let Plan

A nested-scope binding primitive where the bound name(s) are visible
inside their own initializer(s) -- enabling self-recursion in a local
scope, mutual recursion between adjacent bindings, and Racket-style
named let as thin sugar.

This plan is independent of, but **synergistic with**,
[internal-define-plan.md](internal-define-plan.md). That plan ships
`define` with sequential (let\*) semantics and accepts a self-reference
wart. This plan removes the wart and adds named let in the same stroke,
sharing one primitive between both surfaces.

## Scope and non-scope

In scope:
- A nested `letrec` primitive in the elaborator: a binding form where
  every name in the binding group is pre-registered in the inner scope
  *before* any initializer is elaborated.
- A surface form for it. Two options under "Design questions" below;
  pick one before implementation.
- **Named let** as sugar: `(let name [bindings...] body...)` desugars
  to `(letrec [name (fn [params] body)] (name initial-args))`.
- **Upgrade path for internal `define`**: if shipped together with or
  after that plan, adjacent internal `define`s in the same body window
  become a single letrec group, removing the v1 self-recursion
  restriction.

Out of scope:
- Replacing the existing `let`. Sequential (let\*) semantics stay the
  default; letrec is a separate primitive.
- Letrec\* (sequential initialization with full forward visibility) --
  the distinction from letrec matters in lazy languages and for some
  side-effect ordering subtleties; Turmeric is eager and the
  initializers are evaluated left-to-right anyway, so plain letrec
  semantics ("name visible everywhere in the binding group") covers
  what users actually need.
- Top-level letrec. `defn` already does mutual recursion via pass-1
  forward declarations; no new top-level form is needed.
- Variadic / rest parameters in named let. Match `let`'s binding-vector
  shape exactly: `[name1 init1 name2 init2 ...]`.

## Current state

Turmeric **already implements letrec semantics at top level**, just
not under that name. `elab_toplevel.c:620-754` runs a pass-1 walk that
pre-registers every top-level `defn` as a forward declaration (arity +
return kind only) in the global scope before pass-2 elaborates any
body. That is exactly why mutually recursive top-level functions work:

```turmeric
(defn even? [n : int] : bool (if (= n 0) true  (odd?  (- n 1))))
(defn odd?  [n : int] : bool (if (= n 0) false (even? (- n 1))))
```

Pass-1 registers `even?` and `odd?` as forward decls; pass-2 elaborates
each body with both names already in scope; `elab_defn`
(`elab_fns.c:354`) recognises the forward-decl shape and patches the
binding's type when the real definition lands.

There is **no equivalent pre-registration step for nested scopes
today**. `elab_let` (`elab_forms.c:9`) walks the binding vector
strictly left-to-right, adding each binding to the inner scope only
*after* its initializer is elaborated -- this is what makes `let`
sequential. To get letrec, we need a similar pre-registration pass
inside the nested scope.

Other nested binding forms (`fn` params, `defn` params) don't need
changes -- params have no initializers, so letrec semantics are
vacuous there.

## Design questions

### Surface syntax

Two reasonable options:

**Option A: `letrec` as a distinct form.**

```turmeric
(letrec [f (fn [n] (if (= n 0) 1 (* n (f (- n 1)))))
         g (fn [n] (if (f n) :yes :no))]
  (g 5))
```

Pros: explicit, no surprises, matches Scheme/Racket muscle memory,
makes the semantic switch visible at the use site.

Cons: a new top-level form name. Users have to learn it. Small
duplication with `let` (parser, annotation handling).

**Option B: a `let` annotation, e.g. `(let ^rec [...] body)` or
`(let* [...] body)` / `(let-rec [...] body)`.**

Pros: same form family, fewer keywords.

Cons: an annotation that changes binding semantics is easy to miss
when reading. Turmeric's existing `^mut` / `^persistent` etc. are
per-binding annotations on individual names; a whole-form semantics
flag is a different category and shouldn't share the syntax.

**Recommendation: Option A.** Distinct form, distinct name. It's the
mainstream Scheme/Racket choice and the one users coming from those
languages will reach for. Treat the implementation primitive
(`elab_letrec`) as internally callable; named-let and grouped
`define`s both desugar through it without touching its surface.

### Named let surface

```turmeric
(let loop [n 10 acc 0]
  (if (= n 0) acc (loop (- n 1) (+ acc n))))
```

The disambiguation is positional: `let` followed by a *symbol* (rather
than a vector) is named let; otherwise it's the existing `let`.
`elab_let` already inspects `call->as.list.items[1]`; adding a
"is this a symbol?" branch is one extra check.

Desugar:

```turmeric
(letrec [loop (fn [n acc]
                (if (= n 0) acc (loop (- n 1) (+ acc n))))]
  (loop 10 0))
```

Type annotations on bindings carry through to the lambda parameter
list verbatim: `(let loop [n :int 10 acc :int 0] ...)` ->
`(fn [n :int acc :int] ...)`. Return type for the lambda has no
surface slot in named let; default to inferred or require the user to
ascribe at the lambda level if they need it. (Open question for
review: should named let accept a `:ret` keyword between the binding
vector and body, mirroring `defn`?)

### Grouped internal `define` semantics

If `internal-define-plan.md` has shipped (or ships in parallel),
**adjacent internal `define` forms in the same body window** collapse
into one letrec group:

```turmeric
(defn run []
  (define even? (fn [n] (if (= n 0) true  (odd?  (- n 1)))))
  (define odd?  (fn [n] (if (= n 0) false (even? (- n 1)))))
  (even? 10))
```

This rewrites to:

```turmeric
(defn run []
  (letrec [even? (fn [n] ...)
           odd?  (fn [n] ...)]
    (even? 10)))
```

A non-`define` form between two `define`s ends the group:

```turmeric
(define a ...)
(define b ...)   ;; group 1: [a b] in one letrec
(some-expr)
(define c ...)   ;; group 2: [c] in its own letrec, after group 1's body
```

The rewrite produces nested letrecs, which is the obvious extension of
the v1 sequential rewrite. This is a **behaviour change** for code
written against internal-define v1 only in the edge case where a v1
user wrote `(define f ...)` expecting it to *fail* the self-reference
check -- if that test now succeeds, code that was an error becomes
valid. Strictly additive; nothing previously-working breaks.

## Implementation strategy

### 1. The `letrec` primitive

New file `src/compiler/elab_letrec.c` (or extend `elab_forms.c` --
file-local helpers either way). Algorithm:

1. Parse the binding vector exactly like `let`: extract
   `[annotations name [:type] init]` quads per binding.
2. **Pass A (pre-register):** for each binding, create a `Binding*`
   with a *placeholder* type and add it to the inner scope. The
   placeholder follows the pattern `elab_toplevel.c:714` already uses
   for top-level forward decls: if the init form is `(fn [...] :ret ...)`,
   peek at its arity and return type and synthesize a `TY_FN` stub;
   otherwise default to `TY_UNKNOWN` or the user-supplied `:type`.
3. **Pass B (elaborate inits):** elaborate each init in the inner
   scope (so every name is visible). Patch each binding's type with
   the elaborated init's type, matching `elab_defn`'s forward-decl
   patching at `elab_fns.c:1266-1284`.
4. **Pass C (elaborate body):** elaborate the body sequence with the
   inner scope, identical to `let`.
5. Emit as `EX_LET` (or a new `EX_LETREC` if codegen needs to
   distinguish -- see "Codegen" below).

The arity-peek in step 2 is the load-bearing trick. It only works
when the init is a literal `(fn ...)` form. For non-fn inits (e.g.
`(letrec [x (some-call)] ...)`), self-reference inside `x`'s init
genuinely can't work in an eager language -- the value doesn't exist
yet. Emit a diagnostic:

```
error: 'x' is referenced inside its own non-function init in a letrec
       binding. Self-reference in letrec only works when the init is a
       (fn ...) literal, because the recursive call is delayed until
       the function is invoked.
   help: wrap the init in a thunk: (letrec [x (fn [] ...x...)] ...)
         or rewrite as a top-level (def x ...) if no self-reference is
         needed.
```

This matches Racket's runtime behaviour (Racket *allows* non-fn
self-referential letrec but binds the name to an "undefined" sentinel
that errors on access). We choose to reject statically because
Turmeric has no such sentinel and slipping one in would be a
significant runtime change.

### 2. Codegen

Open question: does letrec need a new `EX_LETREC` kind, or can it
reuse `EX_LET`? The values are the same shape -- one binding vector,
one body -- and the C output is identical: declare each binding's
storage at the top of the C scope, assign each init, evaluate body.
The difference between `let` and `letrec` is purely the *order in
which the elaborator populates the scope during type checking*; by
the time we hand off to codegen, both look the same.

Recommendation: reuse `EX_LET`. No codegen change. The fixture sweep
(see Testing) confirms this empirically.

If the borrow checker / linear-types pass cares about init order
(it might, for `^linear` or `^unique` bindings), revisit and add
`EX_LETREC`. Flag this for the borrow-check reviewer during
implementation.

### 3. Named let dispatch

In `elab_let` (`elab_forms.c:9`), before the existing binding-vec
parse, add:

```c
if (call->as.list.len >= 4 &&
    call->as.list.items[1]->tag == F_SYM &&
    call->as.list.items[2]->tag == F_VEC) {
    return elab_named_let(e, call);
}
```

`elab_named_let` builds the desugared Form tree (`letrec [...] (name args)`)
and re-enters `elab_form` on it. Pure desugar; no new Expr nodes.

### 4. Compile-time mirror

`ct_eval_form` in `elab_macros.c:410` handles compile-time `let`
during macro expansion. Mirror the same pre-register pass for
compile-time `letrec` and named let. Pattern identical to step 1 but
operating on `CtValue` / `CtEnv` instead of `Binding` / `Scope`.

### 5. Internal-define integration (if both plans land)

In the `splice_internal_defines` helper from
[internal-define-plan.md](internal-define-plan.md), change the
grouping rule:

- v1 rule: each `(define name init)` becomes one `let` wrapping the rest.
- v2 rule: a *run* of adjacent `define` forms becomes one `letrec`
  wrapping the rest.

Drop the v1 "self-reference scan" diagnostic from
internal-define-plan; letrec semantics make it unnecessary. Keep the
non-fn-init diagnostic (it's the same restriction at the underlying
letrec level).

## Diagnostics

- Non-fn init with self-reference: see step 1 above.
- Duplicate binding name within a letrec group:
  `letrec: '<name>' is bound twice in the same binding group`.
- Named let with non-vec second argument (after parsing as named let):
  `let: named let requires a binding vector: (let <name> [bindings...] body...)`.
- Forward reference to a *later* binding inside an earlier init when
  the earlier init is non-fn: same diagnostic as the self-ref case,
  because the later binding hasn't been initialised yet either.

## Testing

New fixtures under `tests/fixtures/`:

- `letrec-self-recursive/` -- single self-recursive fn.
- `letrec-mutual/` -- mutually recursive `even?`/`odd?`.
- `letrec-shadows-outer/` -- inner letrec name shadows an outer
  binding correctly.
- `letrec-typed-bindings/` -- `:type` annotations on letrec bindings.
- `letrec-non-fn-self-ref/` -- the diagnostic fixture (asserts the
  error message text).
- `letrec-non-fn-no-self-ref/` -- non-fn init that does NOT
  self-reference must still work (e.g. `(letrec [x 1] x)` -- this is
  effectively a let, but should not be rejected).
- `named-let-loop/` -- the canonical countdown loop.
- `named-let-typed/` -- typed binding shapes.
- `named-let-shadowing/` -- name shadows an outer binding and inner
  body sees the loop binding.
- `internal-define-mutual/` -- (only if internal-define has shipped)
  two adjacent `define`s reference each other.

Per CLAUDE.md's strict rule on `expected.c`: regenerate every
fixture's snapshot after wiring `elab_letrec`. Verify the no-letrec
codegen path is byte-identical to pre-change output by running the
fixture suite *before* adding letrec-specific fixtures.

## Risks

1. **Borrow checker / linear types on pre-registered bindings.**
   `^linear` / `^unique` / `^affine` bindings have move-tracking state
   attached. Pre-registering a binding before its init means the
   binding's "initial state" is observed before it has a value. The
   borrow checker likely needs a hint that letrec bindings are
   "defined but not yet initialised" during the init pass.
   Mitigation: prohibit linear-family annotations on `letrec`
   bindings in v1 and emit a diagnostic. Lift the restriction in a
   follow-up after talking to the borrow-check reviewer.

2. **Type inference circularity for non-annotated inits.** If
   `(letrec [f (fn [n] (g n)) g (fn [n] (f n))] ...)` is written
   without `:ret` annotations, the arity-peek in step 1 has no return
   type to use for the placeholder. Either (a) require `:ret`
   annotations on letrec-bound fns when they participate in mutual
   recursion, or (b) iterate the type-patching pass to a fixpoint.
   Recommendation: start with (a) -- a clean error message asking for
   `:ret` -- and only do (b) if it becomes painful.

3. **Codegen reuse assumption.** Reusing `EX_LET` for letrec assumes
   the borrow checker and effect passes don't care about init order.
   If they do, we need `EX_LETREC` and a parallel codegen path.
   Mitigation: the fixture sweep catches the discrepancy; if it
   triggers, the work is mechanical (clone the `EX_LET` codegen,
   rename, adjust).

4. **Surface confusion: three binding forms.** `let`, `letrec`, named
   let all do related things. Mitigation: a short
   `docs/guides/binding-forms.md` covering when to reach for which,
   linked from each form's error messages.

5. **Macro-expansion ordering.** Compile-time letrec in
   `ct_eval_form` needs the same pre-register-then-elaborate pattern.
   Forgetting this silently breaks any macro that uses `letrec`
   internally. Mitigation: a fixture that uses `letrec` inside a
   `defmacro` body and asserts the expansion result.

## Effort estimate

Medium. The pre-register-then-elaborate pattern is well-understood
(it exists at top level), but porting it to nested scope touches
move-tracking, linear-type state, and possibly the borrow checker.
Realistic budget:

- `elab_letrec` primitive + tests: 1 day.
- Named let sugar + tests: half a day (once `letrec` exists).
- Compile-time mirror + macro-body fixture: half a day.
- Borrow checker / linear-types reconciliation: unknown -- could be
  zero, could be a day. Defer with the v1 linear-annotation
  prohibition above.
- Internal-define integration (if applicable): half a day.
- Fixture regen + verification: half a day.

Total: 3-4 days assuming the borrow-checker path is clean; up to a
week if `^linear` / `^unique` letrec bindings need real support.

## Recommendation

Ship internal `define` (let\* semantics, with self-reference
diagnostic) first as a small, low-risk patch. Land letrec + named let
as a follow-up when there is appetite for the larger change. At that
point:

- Remove internal-define's self-reference diagnostic.
- Switch internal-define's grouped rewrite from sequential `let` to
  `letrec`.
- Add named let.

Three idioms (`define`, `letrec`, named let) covering the spectrum
from "I just want a local binding" to "I want a tail-recursive loop"
to "I want grouped mutually recursive helpers," all sharing one
implementation primitive.

The honest counter-recommendation: if internal-define hasn't shipped
yet and there is appetite for both plans, doing them together is
cheaper than serially -- the internal-define splice helper is
trivially adapted from "wrap each define in a let" to "wrap each
define-run in a letrec," and the user never sees a version with the
self-reference wart. Decide on appetite, not on technical sequencing.
