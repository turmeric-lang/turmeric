# A struct that owns its closure frees it although a field read was stored

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
