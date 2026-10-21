# A call through a closure value does not compare a typed pointer's pointee

**Severity:** low-medium. Filed 2026-10-21 as the residue of
[typed-ptr-pointee-not-checked-at-calls](../archive/typed-ptr-pointee-not-checked-at-calls.md),
which compares the pointee of a `ptr<T>` / `rc<T>` / `weak<T>` at a call to a
defn (the callee above or below the call), at a defn's return and at a `let`
annotation (`handle_targets_conflict`, `src/compiler/elab_call.c`).

## Repro

```turmeric
(defstruct Foo [a : int])
(defstruct Bar [b : int])
(defn mk-bar [] : ptr<Bar>
  ```c
  return 0;
  ```)
(defn main [] : int
  (let [f (fn [x : ptr<Foo>] : int 1)]
    (f (mk-bar))))          ; accepted; a defn with the same signature is TUR-E0001
```

`tur check` is silent. The same call to `(defn f [x : ptr<Foo>] ...)` is
`function 'f' arg 1: expected ptr<Foo>, got ptr<Bar>`.

## Root cause

Not traced to a line. `handle_targets_conflict` (`src/compiler/elab_call.c`) is
applied where a call to a named defn is checked (`elab_call_fn_inner`), reading
the declared parameter type off the fn type's `arg_full_types` or the defn's
parameter binding. A call whose head is a let-bound lambda does not reach that
check with a declared full type: the lambda has no `source_fn_def` binding, and
its fn type does not appear to record a `ptr<T>` parameter in `arg_full_types`.

## Fix directions

Record a lambda's `ptr<T>` / `rc<T>` parameter types in its fn type's
`arg_full_types` (the field exists for compound types) and apply
`handle_targets_conflict` in the closure-call path; a negative fixture beside
`errors/typed-ptr-pointee-mismatch`.

## Resolved 2026-10-21

The fix direction as written. A lambda's parameter annotation is recorded on
its fn type's `arg_full_types` (`elab_fn`, `src/compiler/elab_fns.c`) when it
is a typed `ptr<T>`, or an `rc<T>` / `weak<T>` with a known target, beside
the nominal and parametric types that were already recorded. A call through
the let-bound lambda then reaches the same `handle_targets_conflict` check as
a call to a defn, and the program above is
`function 'f' arg 1: expected ptr<Foo>, got ptr<Bar>`.

Fixtures: `tests/fixtures/errors/typed-ptr-pointee-mismatch-closure`,
`tests/fixtures/errors/rc-pointee-mismatch-closure`, and the positive
`tests/fixtures/typed-ptr-pointee-closure-controls` (same pointee, untyped
`ptr<void>` in either direction, `rc<S>` given an `rc<S>`).
