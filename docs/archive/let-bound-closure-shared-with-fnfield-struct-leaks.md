# A closure shared with a closure-field struct has no owner

> **RESOLVED 2026-11-28.** The closure's own scope frees it now -- the
> caller's scope, for a parameter -- when every struct holding it is a local
> that is only read. Two uses that hand a closure `b` on stopped counting as
> escapes in the shared escape walk (`binding_escapes_impl`,
> `src/compiler/emit_core.c`), where the walk sees the whole `let`:
>
> - **HOLDER**: a `let` binds `h` to a by-value struct built with `b` in a boxed
>   fn-field, or to the result of a call that only puts `b` into the struct it
>   returns (`Binding.fnfield_ret_param_mask`, stamped by
>   `elab_stamp_fnfield_ret` -- `(defn wrap [f] (H 2 f))`, through a returned
>   local, or through another such call), and `h` is only read: its fn-fields
>   called (also through the local the elaborator binds a call head to), its
>   scalar fields read (`esc_holder_confined`). A holder that frees its
>   fn-fields itself (`drops_fn_fields`) owns `b` and is refused, unless the
>   field holds a fresh poly-carrier box over `b`, whose drop never frees the
>   env.
> - **CAPTURE**: a `let` binds a lambda that captures `b`, the lambda does not
>   escape the `let`, and its body does not let `b` escape
>   (`esc_capture_confined`). Closure env glue never frees a captured closure
>   except a moved-in `^fat` one, which is refused. This also frees
>   `(let [f (fn ...) g (fn [y] (f y))] (g 1))`, which leaked `f` with no struct
>   anywhere.
>
> `let_binding_env_freeable` asks the same of a sibling's init through
> `closure_binding_escapes_let_init`, and the nonretain inference picks it up
> unchanged, so `(defn use-param [f] (let [h (H 1 f)] ((.handler h) 10)))`
> leaves its caller's closure to the caller. The single-use take-over is
> unchanged: a struct that is a closure's only use still owns and frees it.
>
> Pinned by `fnfield-struct-binder-shared-closure` (its `known-leak` marker is
> gone: all five shapes are leak-clean) and `fnfield-struct-holder-closure-freed`
> (siblings, a capture, parameters, three constructor functions, the
> single-use owner, and negatives that keep the closure alive through a global
> `Vec`, all under ASan). Found on the way and filed, not fixed:
> [fnfield-read-out-of-owning-struct-freed-with-it](../reported/fnfield-read-out-of-owning-struct-freed-with-it.md)
> (a use-after-free on `main`, in the owning case this change does not touch)
> and [fn-param-set-into-fn-global-fails-cc](../reported/fn-param-set-into-fn-global-fails-cc.md).

## As filed

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
