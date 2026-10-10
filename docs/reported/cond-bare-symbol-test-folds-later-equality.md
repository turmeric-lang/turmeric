# A `cond` whose first test is a bare boolean folds a later `(= x 0)` to false

**Severity: high** (silent miscompile -- wrong branch taken, no diagnostic).

**Status: OPEN.** Found 2026-10-10 implementing
[xml-html-parsers-plan](../upcoming/spices/xml-html-parsers-plan.md) on `tur`
v0.64.0. Worked around in turmeric-spices `spices/xml/src/xml/sax.tur`
(`__run`) with nested `if`s.

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
