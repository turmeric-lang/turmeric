# A `cond` whose first test is a bare boolean folds a later `(= x 0)` to false

**Severity: high** (silent miscompile -- wrong branch taken, no diagnostic).

**Status: RESOLVED 2026-10-11.** Found 2026-10-10 implementing
[xml-html-parsers-plan](xml-html-parsers-plan.md) on `tur`
v0.64.0. Worked around in turmeric-spices `spices/xml/src/xml/sax.tur`
(`__run`) with nested `if`s; the workaround can go once the spices pin a `tur`
with this fix. The cause was not `cond`: every template macro evaluated its
caller's arguments at expansion time. See [Resolution](#resolution).

## Repro

````turmeric
(defn get [n : int] : int
  ```c
  return n;
  ```)
(defn flag [n : int] : bool
  ```c
  return n == 99;
  ```)
(defn classify [n : int] : cstr
  (let [rc      (get n)
        stopped (flag n)]
    (cond stopped "s" (= rc 0) "zero" :else "other")))
(defn main [] : int
  (println (classify 0))   ; prints "other" -- should be "zero"
  0)
````

The emitted C has `if (false)` where `(= rc 0)` should be. Variants:

| Body | Result for 0 |
| --- | --- |
| `(cond stopped "s" (= rc 0) "zero" :else "other")` | `other` (wrong) |
| `(cond stopped "s" (< rc 0) "neg" (= 0 rc) "zero" :else "other")` | `other` (wrong) |
| `(cond (= stopped true) "s" (< rc 0) "neg" (= rc 0) "zero" :else "other")` | `zero` |
| `(if stopped "s" (if (< rc 0) "neg" (if (= rc 0) "zero" "other")))` | `zero` |
| `(cond (< rc 0) "neg" (= rc 0) "zero" :else "other")` | `zero` |

So it needs a bare symbol as a test that is not the first clause's
`(= ...)`; the hand-written nested `if` the macro should expand to is fine.

## Where to look

`cond` (`stdlib/macros.tur:44`) compares each clause head with `:else` and
`else` at expansion time; a bare-symbol test reaches `(= (first clauses) else)`
with `else` unbound in the macro environment. The fold to `false` suggests the
equality test of the *next* clause is being treated as macro-time-known (the
`(= x y)` shape matches the macro's own `(= (first clauses) ...)` check, or a
narrowing fact from the bare-symbol branch is applied to the wrong
expression). Bisect between the macro expansion and the elaborator's constant
folding / occurrence narrowing.

## Resolution

**Root cause.** A plain (non-`^syntax`) macro parameter is *substituted* into
the template before the template runs through the compile-time evaluator
(`elab_expand_macro` -> `substitute_params` -> `elab_eval_macro_form`,
`src/compiler/elab_macros.c`). The evaluator then could not tell the caller's
code from the template's own, and evaluated both. In `cond`, the clause list
`(stopped "s" (= rc 0) "zero" :else "other")` reached `(first clauses)` as a
call to `stopped`, whose arguments were evaluated -- and `(= rc 0)` is the CT
`=` builtin, a structural comparison of the symbol `rc` with `0`: `false`.
`tur expand` showed it directly: `(if stopped "s" (cond false "zero" ...))`.
A test that is not a bare symbol hid it only because a list whose head is not
a symbol was returned unevaluated (an earlier patch for the same class).

The same mechanism had two more shapes, both pinned by the new fixture:

| Shape | Before |
| --- | --- |
| `(for [x (some n)] (if (= x rc) 1 0))` | the body's `(= x rc)` folded to `false` |
| `(when c (println (second x)))`, a user `second` | `second` is a gate-listed CT builtin, so the argument routed the whole template through the CT evaluator, whose `do` returned the body as a bare list: "expression in call head has type `nil`" |

`do-m`, `hamt-of`, `set-of` and `hamt-assoc-each__` had each been fixed for
this one at a time with `^syntax` parameters.

**Fix.** `substitute_params` wraps each argument it substitutes in an
`F_UNQUOTE` stamped `PROV_MACRO_ARG` (`macro_arg_datum`). `ct_eval_form`
already returns a bare unquote's inner unevaluated; `ct_eval_quasiquote`
treats the stamp as literal data; `template_needs_ct_eval` no longer looks
inside an argument, so an argument cannot decide whether the template is
evaluated; a spliced `~@rest` splices each element still marked; a call whose
head is a caller-supplied argument evaluates the template's arguments but never
dispatches on the head; the CT `let`/`letrec`/`fn` forms read a marked name as
its symbol. `strip_macro_arg_data` removes every surviving wrapper before
`elab_expand_macro` returns. Type positions (`F_TYPE_ANN`, `F_CONTRACT_TYPE`)
substitute unmarked, since the evaluator hands them back untouched.

Builtins still see an argument as a value -- `(first clauses)` is the first
clause -- they just never evaluate inside it.

**Pinned by** `tests/fixtures/macro-arg-not-ct-evaluated` (every case above;
it fails to compile on the old expander). Fixture suite: 3724 passed, 0
failed.

## Follow-up 2026-10-11: two CT idioms the fix broke

Running turmeric-spices' `ecs` suite against the fix found two macro idioms
that relied on substituted arguments being re-evaluated, both in `ecs`'s world
and system macros:

- **`(quote param)`** -- the documented way a macro kept a parameter as data
  for a CT walker (`defxsystem`'s `(quote clauses)`). The CT `quote` returned
  the `PROV_MACRO_ARG` wrapper itself, a one-element form, so the walker saw
  one item. `quote` now yields the argument (`macro_arg_unwrap`).
- **A recursive splice with unquoted arguments** --
  `~@(sized-mono-world-fields ~cap-expr ~(rest comps))`. A bare unquote
  evaluates to its inner UNevaluated, and substitution used to finish the job:
  the inner `(empty? comps)` evaluated the substituted `(rest comps)` in place.
  With the argument now data, `empty?` saw the unevaluated call, never became
  true, and expansion recursed until the compiler overflowed its stack (three
  `ecs` compile-fail fixtures then "compiled clean", and one test crashed the
  compiler). When the CT evaluator is about to expand a user macro inside a
  splice, an argument written `~x` is now evaluated first (`ct_eval_call`,
  `splice_macro_call`).

**Pinned by** `tests/fixtures/macro-arg-ct-idioms` (it stack-overflows the
compiler before this follow-up); the `ecs` spice suite passes again.
