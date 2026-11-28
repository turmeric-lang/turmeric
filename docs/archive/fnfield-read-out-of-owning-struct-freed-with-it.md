# A struct that owns its closure frees it although a field read was stored

> **RESOLVED 2026-11-28.** Before a struct local is let free its fn-fields,
> `elab_let` asks `elab_fnfield_fields_escape` (`src/compiler/emit_core.c`)
> whether a closure it holds can outlive the `let`, and records the answer
> (`Binding.fnfield_fields_escape`). A fn-field read that is called -- also
> through the local the elaborator binds a call head to -- or lent to a
> `^borrow` / inferred non-retaining parameter keeps it; any other read lets
> it out: stored, returned, passed on, a `match` on the struct, a capture of
> it. Such a local frees nothing, and `fnfield_value_owned` no longer calls a
> function that returns it an owner, so `wrap-keep`'s caller frees nothing
> either. A `perform` is not an escape here -- what a resume re-runs is the
> drop's own business.
>
> A `^copy` struct had the same hole one level up: every copy carries the
> closure pointers, and the struct-local drop's guards only see moves, so
> `(vec-push! held c)` or `(keep-field c)` left a dangling copy. Its walk is
> strict -- a whole-value use is an escape too -- and follows the copies it
> can see the end of: an alias the let confines, a lambda capturing it that
> the let confines and whose body only reads it, and a callee parameter that
> only reads its copy, or reads it and hands it back to a local that is
> confined in turn (`esc_copy_param_status`; recursion counts as escaping).
> Those are the shapes `cps-byvalue-struct-closure-field-param` / `-value`
> and `fnfield-struct-binder-borrowed-closure` free, and still do.
>
> Pinned by `fnfield-read-kept-not-freed` (leak-checked, both engines): the
> local and constructor-function shapes above, a read only called or lent
> to a calling parameter (still freed), and three `^copy` copies that leave
> (through a global `Vec`, a callee that keeps its copy, a callee that keeps
> its field read through an alias).

## As filed

**Severity: high (use-after-free).** Found 2026-11-28 fixing
[let-bound-closure-shared-with-fnfield-struct-leaks](../archive/let-bound-closure-shared-with-fnfield-struct-leaks.md);
reproduces on `main` before that change, and is not touched by it.

## Repro

```turmeric
(defstruct H [raw : int handler : (fn [int] int)])
(def ^mut saved : (fn [int] int) (fn [x : int] : int x))
(defn main [] : int
  (let [n 7]
    (let [h (H 4 (fn [x : int] : int (+ x n)))]
      (set! saved (.handler h))
      (println ((.handler h) 0))))
  (println (saved 1))        ; reads the closure h freed
  0)
```

Built as `tests/run-leak-check.sh` builds it: `heap-use-after-free` in `main`
at the `(saved 1)` call. The same happens through a constructor function --
`(defn wrap-keep [f] (let [h (H 4 f)] (set! saved (.handler h)) h))` -- where
the caller's local owns the result (the field holds a fresh poly-carrier box)
and frees the box `saved` still points at.

## Root cause

`drops_fn_fields` (`src/compiler/elab_forms.c`, the local-struct-drop loop in
`elab_let`) asks whether the local is moved, consumed, or leaves its scope as a
whole (`fnfield_local_leaves_scope`), and whether its init owns the closures
(`fnfield_value_owned`). Nothing asks whether a closure was READ OUT of a field
and kept somewhere -- `(.handler h)` is a copy of the pointer, not a move, so
`is_field_consumed` does not see it. The function-result stamping
(`elab_stamp_fnfield_ret`) has the same gap for a returned local.

## Fix directions

- Before flagging, walk the let for reads of a boxed fn-field of the local
  that are not a call head (or a local only called): that is the question
  `esc_holder_confined` (`src/compiler/emit_core.c`) answers for a holder,
  with the whole-value uses left to the existing guards. Record the answer on
  the binding so `fnfield_value_owned` refuses a returned local whose field
  escaped.
- Expect a few leak-checked fixtures that pass a field read to a
  non-retaining callee to start leaking; admit `^borrow` and inferred
  non-retaining parameters the way the closure walk does.
