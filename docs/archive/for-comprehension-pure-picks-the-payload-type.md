# `for` over Option fails when its body is a value of a user type

**Severity: low-medium (a stdlib form unusable for most element types).**
Found 2026-10-28 building xml-html-followups-plan follow-up D1b.

## Repro

```turmeric
(defdata ^heap Pt (P int) (Q))
(defn f [] : (Option Pt) (for [x (some 3)] (P x)))
(defn main [] : int
  (match (f) (Some p) (match p (P n) (println n) (Q) (println 0)) (None) (println -1))
  0)
```

```
error [TUR-E0015]: no instance of typeclass 'Applicative' for type 'Pt' (method '.pure')
```

The same `for` with an `int` body works
(`tests/fixtures/for-comprehension-stdlib-option`). `for` desugars its body
to `(.pure body)`, and the dot form dispatches on its first argument; when
that is erased (`int`) the return type decides, but when it is a concrete
type the checker looks for `Applicative [Pt]`.

A second limit, in the same form: the monad comes only from an enclosing
`defn`'s declared return type. A `for` passed as an argument to a function
taking `(Option int)`, or bound in a `let` with that annotation, is
`ambiguous method dispatch: '.pure' matches 3 instances`; wrapping it in
`((fn [] : (Option int) (for ...)))` works.

## Where it bit

tur-html's hiccup rewrites a `for` inside a literal (plan D1b). Its body is
`Html`, so it cannot use the `for` macro: the walker expands `for` over
`Option` into nested `match`es itself.

## Fix directions

`.pure` is return-directed, like `.empty`: dispatch it on the expected
result type always, never on its argument; and carry an argument's or a
`let` annotation's expected type into it as a `defn`'s return type is.

## Resolution (2026-10-29)

Both halves, in src/compiler/elab_typeclasses.c:

- A return-directed method (`pure`, `empty`: the class variable only in the
  result) reached through the dot form with a concrete payload no instance
  matches is dispatched on its expected type, as an erased payload already
  was -- `(.pure (P x))` no longer asks for `Applicative [Pt]`.
- With no expected type at all (a `for` that is an argument), the lambda a
  dot-method continues into carries the monad its receiver fixes, `(Option
  b)` for a `bind` over an Option, as `Elab.cont_head_hint`: a hint only
  return-directed methods consult, and only without an expected type, so no
  other call reads the open payload as a binding. The `pure` dispatched on it
  is typed from the class method's signature (`a` from the argument, `f` from
  the hint), and `bind`'s `(f b)` takes `b` from the lambda's result.

Pinned by `tests/fixtures/for-comprehension-user-type-and-argument` (both
engines): a user-type body, a `for` as an argument with and without `:when`,
nested bindings, an annotated `let`. tur-html's hiccup walker can use the
`for` macro instead of expanding `for` into `match`es itself.
