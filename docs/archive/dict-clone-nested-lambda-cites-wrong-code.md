# The dict-clone nested-lambda error cites TUR-E0311, an unrelated code

**RESOLVED 2026-10-17** -- see [Resolution](#resolution-2026-10-17). Pinned by `errors/forall-dict-directly-applied-lambda`.

**Severity:** low (diagnostic). The message ends in "(TUR-E0311)", but it is
emitted without a code, and the registered TUR-E0311 means something else.

## Repro

```turmeric
(defclass Describe [a] (describe [x : a] : cstr))
(definstance Describe [int] (describe [x] "an int"))
(definstance Describe [bool] (describe [x] "a bool"))
(defn say [A] [(Describe A)] [x : A] : cstr
  (let [f (fn [y : A] : cstr (describe y))]
    ((fn [z : A] : cstr (f z)) x)))
(defn use-both [f (forall [a] [(Describe a)] (-> a cstr))] : int
  (println (f 42))
  (println (f true))
  0)
(defn main [] : int (use-both say))
```

```
error: forall-dict-pass: 'say' dispatches a typeclass method on its constrained
type variable from inside a directly-applied nested lambda that cannot be
lowered ... (TUR-E0311)
$ tur explain TUR-E0311
TUR-E0311: unknown :engine value
```

## Root cause

The guard in `src/compiler/elab_call.c:make_dict_clone` writes the code
into its message text and emits through an uncoded `diag_emit` call. Tooling
that filters by code (`--json-diagnostics`, the LSP) does not
see it. A user who runs `tur explain` on the code in the message gets the
`:engine` text.

## Fix directions

- Register a dedicated code for this limitation, emit it with
  `diag_emit_with_code`, and give it `tur explain` text. See
  [diagnostics-internals-guide](../guides/diagnostics-internals-guide.md) for
  the checklist.

## Resolution (2026-10-17)

Registered TUR-E0313 (`TUR_E0313_DICT_PASS_NESTED_LAMBDA`) with `tur explain`
text, and the guard in `make_dict_clone` emits it through
`diag_emit_with_code`; the "(TUR-E0311)" suffix is gone from the message.
