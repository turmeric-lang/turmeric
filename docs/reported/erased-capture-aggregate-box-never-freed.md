# A by-value aggregate captured as a generic's erased `T` is boxed into the closure env and never freed

**Severity: low** (a fixed 8-to-N-byte leak per closure built, where N is the
aggregate's size; correct output). Found 2026-10-14 while pinning
await-parks-only-to-the-nearest-c-frame. Not specific to `await`: a lambda that
never suspends leaks the same box.

## Repro

```turmeric
(defstruct P [a : int b : int])
(defn gid [T] [x : T] : T x)
(defn apply3 [f : (fn [int] int) x : int] : int (f x))
(defn gen-cap [T] [v : T m : int] : int
  (* 2 (apply3 (fn [y : int] : int (let [w (gid v)] (+ y 600))) (+ m 1))))
(defn main [] : nil (println (+ 7 (gen-cap (P 3 4) 2))))
```

Prints 1215. Built with `-fsanitize=address` and run with leak detection on:
`SUMMARY: AddressSanitizer: 16 byte(s) leaked in 1 allocation(s)`.

## Root cause

The lambda inside `gen-cap` is emitted once, against the base env layout
(`struct __env_N { ...; int64_t v; }`): a `T` capture is the int64 carrier
word. The specialization at `T := P` fills that field through the
by-value-aggregate arm of the env fill (`src/compiler/emit_expr.c`, the
`__tur_cbox` block, hkt-generic-nested-bind-result-type): it mallocs a copy of
`v` and stores the pointer. The env's drop glue (`drop_glue___env_N`) belongs to
the shared base layout, where the field is a plain word, so it frees the env
and never the box. At `T := int` the same field holds the value itself, so the
glue cannot simply free the word either.

## Fix directions

1. Give the box an owner per instance: a bit in the env header (or a second
   drop-glue variant chosen at the fill site) saying which carrier fields hold
   a box the env owns.
2. Specialize the lambda with its enclosing generic, so a `T := P` instance
   gets an env whose field is the struct itself (the spec-suffixed env layout
   machinery, `env_name_override`, already exists for float results).

Pinned by `tests/fixtures/await-in-generic-capturing-lambda-struct`
(`known-leak`).
