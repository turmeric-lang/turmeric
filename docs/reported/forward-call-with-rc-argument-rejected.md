# A call to a later `defn` that takes an `rc<T>` is rejected as "expected int"

**Severity: low-medium (expressiveness).** A function can't call another one
defined later in the file if the callee takes an `rc<T>`. The call fails with
`TUR-E0001: expected int, got rc<<adt>>`. Mutual recursion through an
`rc`-taking pair can't be written at all. The same forward call with a struct,
ADT or `(Vec T)` parameter compiles.

Found 2026-10-16 writing the CommonMark block parser (markdown-library-plan
M1). It threads its state as a `^borrow st : rc<MdSt>` through about 100
functions, so `stdlib/markdown/block.tur` defines every callee before its
callers. `stdlib/markdown/html.tur` folds the renderer's recursion into one
self-recursive function.

## Repro

```turmeric
(defstruct S [n : int])
(defn a [^borrow st : rc<S> k : int] : int (if (= k 0) (.n st) (b st (- k 1))))
(defn b [^borrow st : rc<S> k : int] : int (a st k))
(defn main [] : int (let [st (rc/of (S 7))] (do (println (a st 3)) 0)))
```

```
error [TUR-E0001]: function 'b' arg 1: expected int, got rc<<adt>>
```

With `[s : S ...]` in place of the `rc<S>` parameters, the pair compiles and
runs.

## Root cause

- **The forward declaration types the parameter as `int`.**
  `fwd_decl_scan_params` (`src/compiler/elab_core.c:170`) commits only kinds a
  bare type name determines. Any compound annotation, `rc<S>` included, is left
  as the `TY_INT` placeholder (`elab_core.c:228`, `:231`).
- **The call check has no `rc` exemption.** It accepts a struct, an ADT, a
  session or a type application where that placeholder stands
  (`src/compiler/elab_call.c:8564` and the hatches around it), because each
  is one machine word at runtime. There is no such hatch for `TY_RC`, so the
  forward call is checked against `int`.

## Fix directions

1. **Forward-declare the parameter's real type.** Resolve `rc<T>`, `weak<T>`
   and `^borrow` in the pre-pass. That covers ownership too: whether the
   argument is borrowed or moved at the call depends on the callee's real
   signature, which the placeholder hides.
2. **Add a `TY_RC` hatch** beside the `TY_ADT` one. It would be cheaper but
   unsafe: a call checked against an `int` parameter cannot know the callee
   borrows, so the caller may treat the rc as moved, or skip an increment it
   owes. Direction 1 avoids this.
