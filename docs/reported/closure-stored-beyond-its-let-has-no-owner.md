# A closure stored where it outlives its `let` is never freed

**Narrowed 2026-11-28.** Repro 1 is fixed: a `let`-bound lambda whose only
destination is the struct the next binding builds is moved into it
(`Binding.fnfield_moved_into`). The struct then owns it, as it would a lambda
written at that argument, and frees it at the end of its last owner's `let`.
`tests/fixtures/closure-moved-into-struct` (leak-checked) pins it, and
`closure-in-escaping-struct-kept` went from 80 to 56 B leaked (on `main` that
fixture is a use-after-free).

**Factories, 2026-11-28.** A function whose tail is a lambda
(`returns_fresh_closure`) hands its caller a fresh env, and the caller frees
it. That used to require scalar captures and a scalar result. Now it is
enough that the env's drop glue frees only the box
(`closure_env_drop_frees_box_only`) and that an ADT, applied or `cstr` result
cannot point into it. A factory capturing a `^mut` cell, a closure, a struct
or a handle -- the spices' `count-then-stop` and `sax-transform-handler` --
is freed by its caller.

- Nine fixtures leak less and two go clean: `fat-box-ascribed-aggregate-return`
  and `poly-combinator-application-element-inference`, which carry
  `requires.leak-check` now. `fresh-closure-nonscalar-captures` pins the rule.
- The `xml`, `html`, `dom-core` and `signal` shards pass. Under
  `TUR_SANITIZE_PROGRAMS=1` there is no memory-safety error, and leaks went
  down 896 B in `xml`, about 55 KB in `html` and 160 B in `dom-core`.
- What the env captured is still not freed: a `^mut` cell, a lambda the
  factory built, a struct's closures. That is "Let a lambda's env own...",
  below.

**Env ownership, 2026-11-28.** The env of a lambda can now OWN a capture
(`Closure.owned_capture_mask`), and its drop glue frees it: `TUR_CLOSURE_DROP`
for a lambda, each closure field for a struct. The capture must be a lambda
or a closure-owning struct, bound right before the lambda -- the next
binding, or the `let`'s result -- and named nowhere else. Its own `let` must
not be freeing it anyway, and the lambda's uses of it must keep it in; the
lambda's result must not be able to reach it.

Repro 3, an owner struct captured by a returned lambda, and a closure a
factory built (`twice-of`'s `step`) are freed now
(`closure-env-owns-capture`, leak-checked). `closure-in-escaping-struct-kept`
runs clean and lost its `known-leak` marker. `closure-env-owns-capture-refused`
(known-leak) pins the refusals: the lambda hands the capture back, stores it,
hands back the struct, or shares it with a second lambda.

Excluded: `letrec` members and lambdas that capture a sibling -- two
mutually capturing members would each own the other.

**A bug caught on the way:** `Closure` comes from arena memory that is not
zeroed. The first build of this read garbage mask bits from three allocation
sites, which turned a leak-checked fixture into a use-after-free. Each site
now initialises it.

The move is refused, and the lambda still leaks, when:

- the lambda is used again;
- another closure field of the struct is borrowed, since the struct would
  then free that one too;
- something evaluated between the two could perform, since a multi-shot
  resume would build two structs around one lambda.

`tests/fixtures/closure-move-refused` pins those (known-leak); a wrong move
there is a double free.

**Still open after all of the above:**
- repro 2, a closure kept in a global or a vec;
- `closure-slot-ptr-void-param-sinks`'s closure, pushed into a vec and stored
  in a struct;
- a generic call's result;
- an owner struct whose copy is kept in a global (the overwritten initial
  value of a `^mut` global included);
- the refused moves and captures;
- a `^mut` cell captured by a returned lambda.

**Checked for the move:** every fixture's closure frees, emitted against the
previous commit, changed only in `closure-in-escaping-struct-kept`.
turmeric-spices `24d2bbd` (782 files that emit) changed in one free, in
`xml/tests/sax_errors.tur`. `count-starts`' handler lambda is freed now: the
parser's `__deliver-chunk` only calls it, through a thunk it hands to
`with-region`, which the escape walk follows since the same day's
fnfield-owner-struct-copy-outlives-scope fix. The `xml` shard passes, and its
tests under `TUR_SANITIZE_PROGRAMS=1` show the same 8 leak reports as on
`main` and no memory-safety error. One of those reports is 24 B smaller.

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

1. ~~**Move a closure into the struct that takes it.**~~ Done for the
   adjacent-binding shape (above). It could widen: a constructor further
   down the `let`, or one whose other arguments are calls, needs a "nothing
   between could perform" check that does not exist yet. A conditional
   constructor would then leak on the branch not taken.
2. ~~**Let a lambda's env own a struct it captures.**~~ Done (above) for a
   capture bound right before the lambda. A `^mut` cell a returned lambda
   captures is still never freed: its owner is the cell machinery
   (mut-cell-is-never-freed), which frees a cell only when every capturing
   lambda dies in its scope.
3. A closure stored in a vec or a global (repro 2) needs the container to
   own its elements, which is a container-ownership question, not this one.
4. A generic call's result (`(Holder (ident f))`): `ident` keeps its argument
   nowhere but its result (`resalias_fnfield_param_mask` has the bit when the
   argument is a struct), so the same inference for a closure argument would
   let the struct's init be fresh when `f` is moved in.
