# `signal/compose` hand-writes inline-C Vec readers on a stale premise

**Status: RESOLVED 2026-09-16.** Fixed in
[turmeric-spices#74](https://github.com/turmeric-lang/turmeric-spices/pull/74).
`signal/compose.tur` now has **zero** inline-C blocks: `__vec-get-i` and
`__vec-len-i` are gone and `__chain-loop` / `effects-chain` call stdlib
`vec-get` / `vec-len`, confirming the premise -- `vec.tur` is on the shared
autoload list and project mode sees it.

Verified here against a freshly built `tur` v0.49.1 rather than taken from the
merge commit: `tur test tests` from `spices/signal` is `6 tests, 6 passed,
0 failed`, `test_compose` among them (empty / single / multi / captureless
stages).

Two things the fix found that this report did not predict:

- **The hand-rolled copies had already drifted.** The local struct declared
  `size_t len; size_t cap;` where stdlib declares `int64_t`. Same width on a
  64-bit host, which is exactly why it went unnoticed -- and exactly the silent
  miscompile this report argued the duplication invites.
- **The Vec must be read through a `(Vec ptr<void>)` ascription, not a typed
  element.** Spelling the parameter as the type callers actually build with
  `vec-of` -- `(Vec (fn [(fn [float] float)] (fn [float] float)))` -- and
  letting `vec-get` return the element type compiles clean and then takes
  **SIGBUS** before printing anything: the elements are fat closure boxes, the
  shape the module header's PR #288 note is about. Measured, recorded in the
  file, and not to be retried.

**The interpreter coverage this report was about is NOT collectible yet, for a
second reason it did not know about.** `tur --interpret` accepts no `-I` and
runs no spice auto-discovery, so a spice test that imports sibling modules
cannot be interpreted at all -- `tur --interpret tests/signal/test_compose.tur`
from `spices/signal` fails `module 'signal/core' not found`, and `-I src` is
taken as the *filename*. Removing the inline C was still the necessary half (a
fixture carrying it is PASS-skipped no matter what else is fixed); the CLI half
is filed separately as
[interpret-takes-no-include-path-or-spice-discovery](interpret-takes-no-include-path-or-spice-discovery.md).

---

**Severity: low.** Nothing is wrong at runtime. What it costs is interpreter
coverage: `tests/run-turi.sh` PASS-skips any fixture whose program contains a
user inline-C block, so every fixture that loads `signal/compose` is skipped
under `--interpret` for two helpers that no longer need to exist.

**Found** 2026-09-11 investigating lattice-vocabulary-plan L4, which had to
read this module closely.

## The premise, and why it is stale

`spices/signal/src/signal/compose.tur` defines `__vec-get-i` and `__vec-len-i`
as inline C that reinterprets the `Vec` runtime layout by hand:

```turmeric
;;; Project-mode compilation auto-loads only stdlib/macros.tur, so
;;; stdlib/vec.tur's `vec-get` is not in scope here. Inline a minimal reader
;;; that matches the Vec runtime layout (data/len/cap).
(defn __vec-get-i [v : int i : int] : int
  ```c struct { int64_t *data; size_t len; size_t cap; } *vec = (void*)(intptr_t)v;
  ...
  ```)
```

The auto-load list is **shared** by single-file and project mode --
`src/compiler/stdlib_autoload.c`'s own header says so ("Shared by compile_to_c
(single-file) and compile_to_h / compile_to_implementation (project-mode
multi-file) so spice code in `tur build .` sees `Cons`, `tnil?`, `Option`, etc.
without explicit imports") -- and `vec.tur` is on it.

Confirmed against the tree rather than the header: `spices/plot/src/plot/core.tur`
and `spices/linalg/src/linalg/mat.tur` both call stdlib `vec-get` directly in
project mode, and neither defines its own.

## Why it costs something

Beyond the duplication, the two helpers hand-reinterpret
`{ int64_t *data; size_t len; size_t cap; }`. That layout is not this module's
to know: a change to the `Vec` representation would miscompile here silently
while every stdlib caller kept working.

And the inline C is what puts the module's fixtures outside `run-turi.sh`.

## Fix -- DONE

Delete both helpers; call `vec-get` / `vec-len`. The call sites are
`__chain-loop` and `effects-chain`, both in the same file. Carried out as
written, with the one correction recorded at the top: the Vec is read through a
`(Vec ptr<void>)` ascription, because a typed element type SIGBUSes on fat
closure boxes.

The pairing this section asked for -- "check that the module's own tests then
run under `--interpret`, since that is the point" -- could not be done, and the
reason is not inline C. `tur --interpret` has no include path and no spice
auto-discovery, so no multi-module spice test can be interpreted at all. Filed
as [interpret-takes-no-include-path-or-spice-discovery](interpret-takes-no-include-path-or-spice-discovery.md).

## Not this

L4 also asked whether `effects-chain`'s hand-written `__chain-loop` should
become `mconcat` over the endomorphism monoid (it is one). It should not --
`definstance` heads must be plain type names, so the endomorphism needs a
`defstruct` wrapper, and `effects-chain`'s signature takes an untyped `Vec` of
raw SF carriers, so the rewrite adds a wrapping pass rather than removing a
recursion. See that plan's L4 entry. The Vec readers are a separate and
straightforwardly good change.
