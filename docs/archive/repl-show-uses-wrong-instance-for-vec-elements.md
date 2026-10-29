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

## Resolution (2026-10-29)

Both shapes, plus a third the fix found (`src/turi/eval.c`):

1. **Element instance.** `turi_call_show_named` now checks the instance's
   type-parameter constraints against the receiver's element types
   (`typeclass_instance_constraints_satisfied`) before calling it. A `(Vec Q)`
   whose `Q` has no `Show` is walked in C instead (`show_vec_render`). Each
   element is shown through its own `Show` if it has one, and otherwise in
   its default repr: `[Q { y = 1 } Q { y = 2 }]`, as the compiled REPL
   prints it. The elements come back typed through `turi_vec_elem`
   (`collections_native.c`, the `vec-get` read).
2. **A `def`'s value.** A `def` turn takes the type tag and full type from
   its initializer, not the unit-typed def node: `(def w [1 2 3])` shows
   `[1 2 3]`.
3. **A Vec of Vecs.** `Show [Vec]`'s loop shows an element through
   `(show (:: (vec-get v i) A))`, with `A` seeded from the receiver. The inner
   Vec's loop then read the outer `A` (`(Vec int)`) for its own `int`s and
   called `vec-len` on 1. That was a SEGV on the Debug build, and a silent
   process exit on Release, where the prompt printed nothing. A Vec whose
   elements are Vecs takes the same C walk, recursively: `[[1 2] [3]]`. A
   nested Vec of structs printed garbage before; it now prints
   `[[Q { y = 1 }] [Q { y = 2 }]]`.

Elements that `Show [Vec]` shows right (ints, strings, structs with an
instance) still go through it, so `["a" "b"]` keeps showing `[a b]`. The
same check covers `Set` and `Map`, but only `Vec` has the C walk: a Map with
a value type that has no `Show` falls back to the default repr of the whole
value.

Pinned by `tests/turi/repl-smoke.sh` (ctest `tur_repl_smoke`).
