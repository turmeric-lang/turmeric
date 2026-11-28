# A closure stored where it outlives its `let` is never freed

**Severity: low-medium (leak, one closure env per construction, 24 B in the
repros).** A `let`-bound capturing closure that ends up somewhere outliving the
`let` -- a struct handed out of the scope, a global, a lambda that is returned,
a vec -- has no owner. Its `let` cannot free it, since that would be a
use-after-free. Whatever now holds it did not mint it, so does not free it
either (`Binding.fnfield_owner`, the env drop glue).

What is left after
[fnfield-ctor-param-closure-leaks-in-caller](../archive/fnfield-ctor-param-closure-leaks-in-caller.md)
(2026-11-28), which made a closure stored in a struct that STAYS in its scope
free at the end of its `let`.

## Repros

`tests/fixtures/closure-in-escaping-struct-kept` carries the first three,
with a `known-leak` marker. Each closure is called after its `let` has ended,
so the leak-check gate still fails on a use-after-free there.

```turmeric
(defstruct Holder [run : (fn [int] int)  tag : int])
(defn keep [h : Holder] : Holder h)

;; 1. the struct leaves through a function that returns it
(defn leak-out [k : int] : Holder
  (let [cap (fn [x : int] : int (+ x k))
        h   (Holder cap 1)]
    (keep h)))

;; 2. its closure field is stored in a global (or pushed into a vec)
;; 3. the struct is captured by a lambda that is returned
(defn capture-out [k : int] : (fn [int] int)
  (let [cap (fn [x : int] : int (- x k))
        h   (Holder cap 3)]
    (fn [x : int] : int ((.run h) x))))
```

Two more shapes:

- **A closure pushed into a vec and stored in a struct.**
  `tests/fixtures/closure-slot-ptr-void-param-sinks` does this with `cap`
  (24 B). `main` freed it through the struct with nothing proving the struct
  was its last holder; since fnfield-struct-binder-frees-borrowed-closure it
  leaks.
- **A closure a generic call hands back**, `(Holder (ident f))`. The walk
  does not know whether `ident` returned its argument or something new.

A constructor function whose body contains inline C (which can keep its
closure parameter by its C name) is not trusted either. A closure passed to
one is not freed by the caller.

**An owner struct's copy that outlives its `let`.** The struct minted its
closure, so it is the owner. A copy outlives the `let`: captured by a returned
lambda, kept in a global, or handed to a function that keeps it. On `main`
that freed the closure under the copy, a use-after-free:
[fnfield-owner-struct-copy-outlives-scope](../archive/fnfield-owner-struct-copy-outlives-scope.md).
It leaks now. `tests/fixtures/fnfield-owner-struct-copy-kept` carries it, with
a `known-leak` marker pointing here.

## Root cause

Ownership of a closure env is a property of the binder that minted it: a `let`
frees what it bound when the escape walk (`closure_binding_escapes`,
`src/compiler/emit_core.c`) proves nothing else still holds it. A struct
frees only closures its own init minted. There is no move of a closure from
its `let` into whatever it escapes to.

## Fix directions

Owner structs only free when no copy outlives them (2026-11-28), so making
a struct the owner is safe now. Before that, direction 1 would have spread
the use-after-free above to every moved closure.

1. **Move a closure into the struct that takes it.** When a `let`-bound
   closure's only escape is into one constructor's closure field, and the
   closure is not used after that point, make that struct the owner
   (`fnfield_owner`) and drop the `let`'s free. Shapes 1 and 3 then free when
   the struct's last owner does. This needs a "last use" check the escape walk
   does not do today.
2. A closure stored in a vec or a global needs the container to own its
   elements, which is a container-ownership question, not this one.
