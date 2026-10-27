---
title: ^fat parameter `(fn ...)` annotation is lost across the defmodule boundary, breaking every caller of a typed fat-closure-taking defn
category: Reported
severity: high
description: A `(defn foo [^fat name : (fn [...] ...)] ...)` declared inside `(defmodule mod ...)` registers correctly in the same file, but any *caller* in a different file (importer, example, test) sees `name`'s type as the zero-arg placeholder `(fn [] : ?)`. The TUR-E0001 message at the call site is literally `expected (fn [] : ?), got <whatever-the-caller-passed>`. This silently bricks every spice that exports a fat-typed API through `defmodule (export ...)`.
---

# `^fat name : (fn ...)` annotation lost across defmodule export boundary

## Summary

Inside a `(defmodule mod (export name) ...)`, a `(defn name [^fat
arg : (fn [param-types] ret-type)] ret) ...)` registers and `tur check`
of the defining file passes -- the *defn body* successfully typechecks
calling `arg`. But the moment the importer (any other file, including
`tests/` or `examples/` inside the same spice tree) calls `name`, the
elaborator reports the fat-typed parameter as the zero-arg placeholder:

```
error [TUR-E0001]: function 'name' arg N: expected (fn [] : ?), got <caller's-type>
```

I.e. the rich `(fn [param-types] ret-type)` annotation never reaches the
caller. The exported signature degrades to "callable with anything"
followed by an immediate mismatch against whatever the call site
actually passes.

## Severity

High. This blocks every `tur-signal` spice module that exports an SF or
SF-consuming defn through `defmodule (export ...)`: `signal/core`'s
`sample` / `map-signal` / `pair-signals`, every oscillator/filter/shaper
SF the caller invokes, `signal/compose`'s `effects-chain` -- the entire
Phase 1-5 surface. The library-side `tur check` of those files passes,
so a casual look claims "all clean," but neither the Phase 1 example
nor `test_core` actually runs.

## Observed

Build: turmeric `main` at `21d11393`, fresh Debug `build/tur`,
turmeric-spices `signal` spice at `7ca0f3b` (post-rebuild).

### 1. `sample` (signal/core), called from the Phase 1 example

`spices/signal/src/signal/core.tur:53`:
```turmeric
(defmodule signal/core
  (export constant time-signal sample ...)
...
(defn sample [^fat sig : (fn [float] float) t : float] : float
  (sig t))
)
```

`spices/signal/examples/01_constant_and_time.tur`:
```turmeric
(defmodule signal/examples/01_constant_and_time
  (import signal/core :refer [constant time-signal sample]))

(defn main [] : int
  (let [c (constant 0.5)]
    (println (sample c 0.0)))
  0)
```

```sh
$ cd .../spices/signal && tur run examples/01_constant_and_time.tur
examples/01_constant_and_time.tur:12:22: error [TUR-E0001]:
  function 'sample' arg 1: expected (fn [] : ?), got ptr<void>
12 |     (println (sample c 0.0))
   |                      ^
```

`c` is a `(constant 0.5)` result -- a `ptr<void>` fat-closure box, which
is exactly what `sample`'s `^fat sig : (fn [float] float)` is meant to
accept. The error reveals that the elaborator at the call site believes
`sample` expects `(fn [] : ?)` in slot 0, not `(fn [float] float)`.

### 2. `__apply-sf` (signal/compose), called from `__chain-loop` in the same module

Less surprising but worth recording: even in the SAME defmodule, a
forward-declared call site sees the same `(fn [] : ?)` projection:

`spices/signal/src/signal/compose.tur:19-41`:
```turmeric
(defmodule signal/compose
  (export effects-chain)

(defn __apply-sf
  [^fat sf  : (fn [ptr<void>] #{} ptr<void>)
   ^fat sig : (fn [float]     #{} float)] : ptr<void>
  (sf sig))

(defn __chain-loop
  [effects : int
   ^fat sig : (fn [float] #{} float)
   i : int n : int] : ptr<void>
  (if (>= i n)
    sig
    (:: (__chain-loop effects
                      (__apply-sf (:: (vec-get effects i) :ptr<void>) sig)
                      ...) :ptr<void>)))
)
```

```
error [TUR-E0001]: function '__apply-sf' arg 1:
  expected (fn [] : ?), got ptr<void>
```

So the gap is *not* purely a cross-file import issue -- it's a
defmodule-internal lookup that also loses the `(fn ...)` annotation.

## Expected

Both call sites should accept their `^fat`-typed arguments. With these
defns lifted *out* of `defmodule` (the test-fixture style), the same
shapes elaborate cleanly: see
`tests/fixtures/pair-signals-typed/input.tur` (`call-pair` taking
`^fat f :(fn [:float] #{} :(Pair float float))`) and
`tests/fixtures/vec-typed-fat-closure-readback/input.tur` (`apply-sf`
taking `^fat sf :(fn [:ptr<void>] #{} :ptr<void>)`) -- both pass
elaboration. Same param shapes inside `(defmodule ...)` do not.

## Repro (minimal, end-to-end)

In the turmeric-spices repo at `spices/signal/` against turmeric `main`:

```sh
tur check src/signal/core.tur           # PASS (defn site)
tur run examples/01_constant_and_time.tur
# -> TUR-E0001 at the (sample c 0.0) call site
tur check src/signal/compose.tur
# -> TUR-E0001 at the (__apply-sf ...) call site inside __chain-loop
```

A self-contained repro outside the spice (paste into a tmp file in this
repo's tree so stdlib resolves):

```turmeric
(defmodule probe/m1
  (export takes-fn)

(defn takes-fn [^fat f : (fn [float] float)] : float
  (f 0.0))
)
```

Caller in any importer (a separate file with `(import probe/m1 ...)`)
gets `expected (fn [] : ?), got <whatever-was-passed>`.

## Workarounds (none clean)

- Drop the defmodule wrapper. Works (fixtures do this) but defeats the
  point of an exporting spice.
- Drop the `: (fn [...])` annotation on the `^fat` param. The defn no
  longer rejects callers, but its own body then loses the ability to
  invoke `f` -- `'f' is not a function or continuation`.
- Add a leading non-`^fat` positional parameter (`[dummy : int ^fat sf :
  (fn ...) ...]`). Surprisingly makes the *self-recursive* call site
  inside the same defmodule re-find the right type for that defn, but
  breaks the public arity and still doesn't fix the cross-file caller.

There is no workaround that keeps the spice's `(defmodule ... (export
...))` shape AND lets a caller invoke an exported `^fat`-typed defn.

## Root-cause direction (best guess)

The export pipeline almost certainly serialises the parameter list with
`^fat` retained as a metadata flag but flattens the inline type
annotation away -- leaving the slot's type as the default empty-fn
placeholder. Two likely places:

1. Whatever stage writes the import-resolution table from a
   `(export name1 name2 ...)` list. If it walks the defn's parameter
   AST without descending into `^fat`'s attached type expression, the
   exported signature collapses to "fn" with no param/return info.
2. The same elaborator pass that handles forward-reference inside a
   defmodule (so that `__chain-loop` referring to `__apply-sf` reads
   the registered signature) goes through the same projection, which
   would explain why the in-module forward call also fails.

A diagnostic: dump the registered signature for `signal/core/sample`
right after `(defmodule signal/core ...)` finishes parsing. If the
recorded parameter type for `sig` is `(fn [] : ?)` (vs. `(fn [float]
float)` for the equivalent fixture-style defn outside a defmodule),
that pins the bug to the export/registration step.

## Proposed fix

When a `defn` parameter carries `^fat` plus a `:(fn ...)` (or `: (fn
...)`) annotation, ensure the inline annotation is attached to the
parameter node BEFORE the defmodule-export pass copies the signature
into the module-level export table. Equivalently: make the
`^fat`-with-type case carry the annotation through whatever path the
non-`^fat` typed parameters already use (since plain typed `name : type`
params do round-trip through defmodule exports correctly today --
`__chain-loop`'s `[effects : int ... i : int n : int]` slots are
recovered fine by callers).

## Validation of a fix

- The Phase 1 example
  `spices/signal/examples/01_constant_and_time.tur` runs and prints
  `0.5 / 0.5 / 0.5 / 0 / 0.25 / 2`.
- `tur run tests/signal/test_core.tur` prints `PASS test_core`.
- `tur check spices/signal/src/signal/compose.tur` exits 0.
- A direct probe defn `(defn f [^fat sig : (fn [float] float)] :float
  (sig 0.0))` declared inside any `(defmodule probe/m1 ...)` can be
  imported and called from another file with a `(constant 1.0)` /
  `(time-signal)` / any captureless `(fn [t :float] :float ...)`
  argument without TUR-E0001.

## Related

- [[tur-signal-rebuild-plan]] -- this gap blocks the plan's Phase 1
  acceptance criterion "`examples/01_constant_and_time.tur` runs and
  matches hand-computed output" and the entire test matrix (test_osc /
  test_filter / test_shaper / test_envelope / test_compose). It is
  *additional to* the upstream codegen regression already tracked in
  [[vec-typed-fat-closure-readback-fixture-regressed-codegen]] -- both
  must clear before the spice's surface is exercisable.
- [[fat-closure-dispatch-does-not-handle-struct-return]] -- adjacent
  area (fat-closure dispatch / parameter type handling) but a distinct
  symptom.
- The same probe also exposes a second defmodule gap:
  `(make-struct Pair ...)` and the `(Pair float float)` type
  expression fail inside `(defmodule ...)` with `TUR-E0012` /
  "Pair is not a defined struct type". `stdlib/pair.tur`'s `pair`
  *function* (the constructor wrapper) still works, so the workaround
  applied in `signal/core` is `(pair a b)`. Worth filing separately
  if defstruct-vs-defmodule scoping is a different bug than this one;
  noted here for traceability.
