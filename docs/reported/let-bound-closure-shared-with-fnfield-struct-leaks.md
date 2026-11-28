# A closure shared with a closure-field struct has no owner

**Severity: low (leak, 24 B per closure).** This is what the fix for
[fnfield-struct-binder-frees-borrowed-closure](../archive/fnfield-struct-binder-frees-borrowed-closure.md)
left behind. That fix stopped a struct local from freeing a closure it does not
own, which was a use-after-free or a double free. But in the shapes below
nothing else frees the closure either, so it now leaks instead.

## Repro

```turmeric
(defstruct H [raw : int handler : (fn [int] int)])
(defn main [] : int
  (let [n 5
        f (fn [x : int] : int (* x n))]
    (let [h (H 1 f)]
      (println ((.handler h) 2)))
    (println (f 3)))
  0)
```

Built as `tests/run-leak-check.sh` builds it, this program reports a 24-byte
leak: `f`'s env box. Before the fix, `h` freed that box and `(f 3)` then read
freed memory.

The same leak happens when:

- a closure **parameter** is stored in a struct (`(let [h (E 1 f)] ...)` in a
  function taking `f`), when the caller's closure is a let-bound local;
- a let-bound closure is stored on every pass of a `while`;
- a function stores its closure parameter and returns the struct.

All four are pinned by `tests/fixtures/fnfield-struct-binder-shared-closure`,
which carries `known-leak`.

## Root cause

A let-bound closure local is never freed by its own `let`. It has an owner only
when something takes it over: a struct that stores it is that owner when
storing is the closure's single use (`fnfield_release_shared_takers`,
`src/compiler/elab_forms.c`). When the closure is used anywhere else, no
binding owns it.

## Fix directions

Give a let-bound capturing closure a scope-exit free of its own, under the same
escape guards the struct local has. A struct that stores it then borrows it,
and does not take it over. `spawn_owns_env` (the `async` case) already marks
the one consumer that takes a let-bound closure's env over.
