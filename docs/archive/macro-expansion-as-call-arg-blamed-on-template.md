# A type error on a macro call used as an argument points into the macro's template

**Severity: low (misleading diagnostic; the error itself is right).**
Found 2026-10-21 typing tur-plot's renderer data as `(Vec Point)`
(plot-renderer-typeclass-plan P4).

When a macro call is an argument to a function and the expansion as a whole
has the wrong type, the TUR-E0001 is reported at the **root form of the
macro's template**, in the file that defines the macro, with no
"in expansion of macro" note pointing back at the user's call:

```turmeric
(defstruct P :copy [x : float  y : float])
(defstruct Q :copy [a : float])
(defn total [ps : (Vec P)] : int (vec-len ps))
(defn main [] : int
  (total (vec-of (Q 3.25))))
```

```
stdlib/vec.tur:501:5: error [TUR-E0001]: function 'total' arg 1: expected (Vec P), got (Vec Q)
501 |     (list let
    |     ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^ ... (a caret run several hundred columns wide)
```

The user's file and line are not mentioned anywhere. Compare a mismatch
**inside** the expansion, which is attributed correctly:

```turmeric
(let [v (vec-of (P 1.25 2.5) (Q 3.25))] (total v))
```

```
a.tur:5:32: error [TUR-E0001]: function 'vec-push!' arg 2: expected P, got Q
a.tur:5:11: note: in expansion of macro 'vec-of' -- the diagnostics above are inside code this call generated
```

Any macro hits it, not just `vec-of`: the arg check reads the span of the
expression the expansion elaborated to.

## Root cause

`src/compiler/elab_call.c` (the macro branch of call elaboration, around the
`elab_form(e, expanded)` call): the expanded Form keeps the template's spans
("macro-expansion provenance: template spans survive expansion"), so the
Expr it elaborates to carries the span of the template's root form --
`stdlib/vec.tur:501`'s `(list let ...)` for `vec-of`. The only re-spanning
is for a top-level `definstance`. The "in expansion of macro" note is
appended only when an error is raised **during** the expand+elaborate
window; this one is raised afterwards, by the enclosing call's argument
check, which uses the returned Expr's span.

## Fix directions

- Give the expansion's root the call site's span: either re-span the root
  expanded Form to `call->span` (as the `definstance` case already does), or
  set the returned Expr's span to `call->span`. The expansion replaces the
  call, so the call's span is the honest location for the whole value; inner
  subforms keep their template spans. Check first that `elab_form` cannot
  return a shared Expr node (a macro expanding to one of its own arguments
  returns the argument's Expr) -- re-spanning the Form is the safer of the
  two.
- Snapshot churn: an Expr span can reach emitted `#line` / diagnostics in
  `expected.c` and `expected.diag`; regenerate in the same change.

## Resolved 2026-10-21

The macro branch of call elaboration (`src/compiler/elab_call.c`) now gives
the expansion's ROOT form the call site's span, by a shallow copy of the root
(the root can be one of the call's own argument forms handed back unchanged,
so it is not written in place). Inner subforms keep their template spans, so
a diagnostic inside the expansion still lands in the macro with the
"in expansion of macro" note. Expression position only: a top-level
statement root (a macro-emitted `defn`) keeps its file id, which credits the
definition to the macro's module; the `definstance` re-span stays as it was.

The program above now reports
`input.tur:5:10: error [TUR-E0001]: function 'total' arg 1: expected (Vec P), got (Vec Q)`
with the caret under `(vec-of (Q 3.25))`.

Fixture: `tests/fixtures/errors/macro-arg-mismatch-blames-call-site`
(`unexpected.diag` pins that `stdlib/vec.tur` is no longer named).
