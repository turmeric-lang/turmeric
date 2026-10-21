# An unannotated method receiver leaves an `(Option T)` result untyped at a call

**Severity: low (misleading diagnostic; annotating the receiver avoids it).**
Found 2026-10-21 adding `categories [r] : (Option int)` to tur-plot's
`Renderer` class (plot-renderer-typeclass-plan P4).

A class method whose parameter is written without a type, `[r]`, and whose
result is an `(Option ...)`, cannot be used where an `(Option ...)` is
expected when it is called on a concrete value:

```turmeric
(defclass K [a] (cats [r] : (Option int)))
(definstance K [int] (cats [r] (some r)))
(defn main [] : int (do (println (unwrap-or (cats 3) -1)) 0))
```

| Class signature | `(unwrap-or (cats 3) -1)` |
| --- | --- |
| `(cats [r : a] : (Option int))` | prints `3` |
| `(cats [r] : (Option int))` | `TUR-E0001: function 'unwrap-or' arg 1: expected (Option A), got (? ?)` |
| `(cats [r] : (Option cstr))` (struct instance) | same `(? ?)` |
| `(cats [r] : (Option a))` | `cannot infer type for return-directed method 'cats'; add a type ascription` |

The same holds with a default body, `(cats [r] : (Option int) (none))`, and
for a struct instance. Ascribing the call, `(:: (cats 3) (Option int))`,
works, and so does calling it through `open` on an existential. A method
returning a non-parameterised type such as a struct (`(bounds [r] : BBox)`)
is unaffected.

## Root cause (partly traced)

An unannotated parameter does not become the class variable `a`, so the
signature mentions `a` only in its result, or not at all. The last row's
message shows the checker then classifies the method as return-directed
(`call_dispatched_constraint_class`, `src/compiler/elab_call.c`, is the
classifier the emitter shares), and with no expected type there is nothing to
key the result on: the applied result stays a def-less `(? ?)` instead of the
declared `(Option int)`. This is the concrete-call sibling of the archived
`class-var-applied-result-untyped-in-constrained-generic`.

## Fix directions

1. Treat an unannotated first parameter of a class method as the class
   variable -- every example in `defclass`'s docs annotates it as `a`, and
   no other reading of an untyped receiver is useful.
2. Failing that, reject the unannotated parameter at the `defclass`
   ("method parameter `r` needs a type; write `[r : a]`") rather than
   accepting a signature whose calls cannot be typed.
3. Either way, a signature-declared concrete result (`(Option int)`) should
   type the call even when dispatch is return-directed.
