# The interpreter REPL shows a Vec of structs through the wrong Show, or as an address

**Severity:** medium-low (wrong display at the `tur repl` prompt; no wrong value
in a program). Found 2026-10-29 while matching the compiled REPL's printer
(aot-compiled-repl-plan C2, point 8) against the interpreter's.

## Repro

```turmeric
(defstruct P [x : int])
(definstance Show [P] (show [p] : String (string/from-cstr "P!")))
(defstruct Q [y : int])
[(Q 1) (Q 2)]
(def w [1 2 3])
(def v [(P 1)])
v
```

`tur repl` prints:

```
=> [P! P!]
=> 88167089007792
=> 88167089009760
=> [P!]
```

Expected `[Q { y = 1 } Q { y = 2 }]` (Q has no Show, so each element in its
default repr), `[1 2 3]`, `[P!]` and `[P!]`. Without `P` and its instance,
`[(Q 1) (Q 2)]` prints a bare address too.

`tur repl --enable=compiled-repl` prints the expected four lines: it picks the
printer from the result's static type when it builds the turn's program
(`emit_repr`, `src/turi/compiled_turn.c`).

## Two shapes

1. **Element instance.** The display reaches `Show [Vec]` by its head tag
   (`turi_try_show_by_tag`, `src/turi/eval.c`), and the instance's
   `vec-show-loop` shows each element with `(show (:: (vec-get v i) A))`. For
   an element type with no instance of its own the ascription does not stop
   at "no instance": it lands on another struct's (`P`'s) or, with none in
   the session, the display falls back to the raw carrier. The constraint
   `[(Show A)]` on the instance is never checked against `(Vec Q)` before the
   call.
2. **A `def`'s value.** For a turn ending in `(def w ...)` the display gets
   the def's unit-typed node, not the initializer's type, so the head tag it
   looks up is not `Vec` and the value prints as its carrier. A bare `w` on
   the next turn prints `[1 2 3]`.

## Fix directions

- Check the instance's constraints (`typeclass_instance_constraints_satisfied`)
  for the result's full type before calling it, and fall back to the default
  repr when they do not hold; or walk the elements in C with each one's own
  repr, as the compiled REPL does.
- For a `def` turn, use the initializer's type, as `turi_eval_impl` step 7
  already does for the compiled turn (`last_type`).
