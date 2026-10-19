# Control forms: `quote` does not quote lists, `defer` scopes differ between engines, `return` and literal `match` are unchecked

**Severity: medium.** Found 2026-10-19 checking the examples for the control
forms' API reference (`stdlib/forms/control.tur`). Every repro was run on both
engines.

## 1. `quote` of a list evaluates it

```turmeric
(println (quote (+ 1 2)))    ; 3 on both engines (also '(+ 1 2))
```

`(quote x)` is documented as returning `x` without evaluating it
(`src/compiler/elab_toplevel.c`, `case F_QUOTE`), and a quoted symbol is right
(`(sym->str (quote hello))` is `hello`), but a quoted list is elaborated as code.
There is no way to write list data with `quote` in a function body.

## 2. `defer` fires at different times in the two engines

```turmeric
(defn f [] : int
  (do (defer (println "d")) (println "body"))
  (println "after") 0)
```

Compiled: `body`, `d`, `after`. Interpreted: `body`, `after`, `d`. The same split
holds for a `defer` in a `when` body, and in a `while` body: compiled fires it at
the end of every pass (`0 d 1 d 2 d after`), the interpreter at the end of the
function (`0 1 2 after d d d`). A `defer` directly in a `let` body or the function
body agrees. `tests/fixtures/defer-in-loop` pins only the compiled behaviour.

## 3. `defer` captures its variables when it is registered

```turmeric
(let [^mut x 10]
  (defer (println x))   ; prints 10
  (set! x 20))
```

`tests/fixtures/defer-mutated-binding` has the comment "Should print 20, not 10"
on exactly this program; it prints 10 on both engines, and the fixture has no
`expected.stdout`, only the codegen snapshot, so nothing asserts either answer.
Either the cleanup should see the mutation or the comment is wrong.

## 4. `return` does not check its value against the function's type

`(defn f [] : int (return "a") 1)` compiles. Compiled it returns the string's
address as an `int` (a `-Wint-conversion` warning from the C compiler); the
interpreter returns the string.

## 5. A `match` on literals with no matching arm crashes

```turmeric
(defn g [n : int] : cstr (match n 0 "zero" 1 "one"))
(println (g 5))
```

Compiled: segmentation fault. Interpreted: `match: no arm matched`. A missing
constructor of an algebraic data type is a compile error, but missing literal
coverage is not diagnosed (a wildcard `_` is not required).

## 6. `^tailcall` is not checked by the interpreter

`(defn f [n : int] : int (if (= n 0) 0 (+ 1 (^tailcall (f (- n 1))))))` is
TUR-E0716 compiled and runs (returns 3 for `(f 3)`) under `--interpret`.

## 7. Vector destructuring in `let` binds the vector

`(let [[a b] (vec-of 7 8)] a)` has type `(Vec int)`: `a` is the whole vector, not
7. (`elab_forms.c` types the elements `(vi == 0) ? init_v->type : TY_INT`, the
session-pair rule, for every right-hand side.) Either destructure the elements or
reject the pattern.

## Fix directions

1. Make `quote` of a list produce list data (or reject it with a diagnostic).
2. Give the interpreter the same `defer` scopes as the compiled program (a
   scope per `do` / `when` / `while` pass), and pin both with fixtures.
3. Decide whether a `defer` sees later mutation; fix the comment or the
   behaviour, and give `defer-mutated-binding` an `expected.stdout`.
4. Check a `return` value against the enclosing function's type.
5. Require a wildcard (or an `else`) for a literal `match`, or make the compiled
   fall-through a defined error.
6. Run the TUR-E0716 check in the interpreter's elaboration too.
7. Destructure vector elements, or reject `[a b]` on a vector.
