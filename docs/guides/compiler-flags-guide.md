---
title: Compiler Flags
category: CLI Tools
description: Diagnostic and debug flags accepted by `tur`; list of removed `-X` feature flags
---

# Turmeric Compiler Flags

Every `-X` feature flag is an accept-and-warn no-op: the language features
they used to gate are unconditionally on. Passing `-X<name>` for any of the
16 names listed below still parses successfully (downstream `build.tur`
files keep compiling unchanged) but emits `TUR-W0050` and otherwise does
nothing.

The diagnostic and debug flags below (`--strict-effects`, `--keep-contracts`,
`--no-contracts`, `--dump-*`, `--emit-abi-trace`, `--warn-unused-result`,
`--lint-panic`, etc.) are real and still control compiler behavior.

---

## Removed Feature Flags

| Flag | Feature is now standard; see |
|---|---|
| `-Xlinear` | [substructural-types-guide.md](substructural-types-guide.md) |
| `-Xsubstructural` | [substructural-types-guide.md](substructural-types-guide.md) |
| `-Xunique-types` | [uniqueness-types-guide.md](uniqueness-types-guide.md) |
| `-Xgadt` | [gadts-guide.md](gadts-guide.md) |
| `-Xunion-types` | [union-intersection-types-guide.md](union-intersection-types-guide.md) |
| `-Xintersection-types` | [union-intersection-types-guide.md](union-intersection-types-guide.md) |
| `-Xeffect-types` | [effects-system-guide.md](effects-system-guide.md) |
| `-Xcontracts` | [contract-types-guide.md](contract-types-guide.md) |
| `-Xsessions` | [session-types-guide.md](session-types-guide.md) |
| `-Xdynamic-vars` | [dynamic-vars-guide.md](dynamic-vars-guide.md) |
| `-Xcallcc` | [effects-system-guide.md](effects-system-guide.md#continuations-callcc-escape) |
| `-Xsized-types` | [sized-types-guide.md](sized-types-guide.md) |
| `-Xdata-literals` | [data-literals-guide.md](data-literals-guide.md) |
| `-Xjson-reader` | [json-guide.md](json-guide.md) |
| `-Xschema-reader` | [schema-guide.md](schema-guide.md) |
| `-Xsymbols` | [symbols-guide.md](symbols-guide.md) |

Each prints `warning [TUR-W0050]: -X<name> is no longer needed; the
feature is on by default` and is otherwise ignored. The recognizer stays so
old `build.tur` files keep compiling; a future cleanup may retire it with
its own deprecation window.

> **Note.** The `-X<name>` prefix is retired for good -- none of the 16
> names above will be reused. For genuinely **experimental** features (the
> kind that are half-built, in-flux, or carry a known cost), the surface is
> `--enable=<name>` with a built-in expiry policy.
> See [experimental-flags-guide.md](experimental-flags-guide.md).

### Related behavior worth knowing

- **`--strict-effects` is opt-in.** Effect typing is always on, but the
  warnings on unannotated effectful functions fire only when you pass
  `--strict-effects` (or `-Werror=strict-effects`) explicitly.
- **Partial features are always-on at their current completion level.**
  Uniqueness types (UT0--UT3) and sized types (SZ0--SZ9; static
  checking covers folded-constant sizes, runtime assertions cover
  open-expression sizes) apply to every program. Their
  not-yet-shipped-bit diagnostics (`TUR-E0260`, etc.) fire unchanged.

---

## Diagnostic Flags

These are real flags, not deprecated.

### `--strict-effects`

Emits warnings on functions whose inferred effect row is non-empty but
whose signature carries no explicit `#fx{...}` annotation or `forall [e]`
quantification (`TUR-W0030`), and on a declared row variable that is always
bound to a concrete row (`TUR-W0032`). A `fn` literal is described by its
enclosing function -- *anonymous function in 'dfs-or'* -- rather than by the
compiler's internal name for it. Opt-in.

### `-Werror=strict-effects`

Promotes the `--strict-effects` warnings to errors, so a build fails on an
unannotated effectful function. Implies `--strict-effects`, the way gcc's
`-Werror=<x>` implies `-W<x>`. `--Werror=strict-effects` is accepted too.

### `--lint-effects` (deprecated)

An alias for `--strict-effects` that prints `TUR-W0050`. It used to be a
second, byte-identical copy of the `TUR-W0030` check, documented as the
"advisory" form of a strict flag that could not in fact be made strict.

### `--keep-contracts`

Retains contract checks in release builds (`just release`). Without this
flag, `assert!`/`require!`/`ensure!` calls are elided in release mode
(contracts are on by default in debug). Per-contract `^always` granularity
may be added later.

### `--no-contracts` (C2)

Strips **all** contracts at elaboration time -- the
`assert!`/`require!`/`ensure!`/`invariant!` macros and `{ x : T | pred }`
refinement-type checks -- so predicates are never evaluated. Folds
`contract-enabled?` to `false` and sets `TUR_CONTRACTS_ENABLED 0` in the
C preamble. Build a stripped program with `just release-stripped <file>`
(i.e. `tur --no-contracts build <file> -O2`).

### `--warn-unused-result` (R6a)

Warns when a `result`-shaped (`ptr<void>`) value is computed in statement
position and discarded. Silence with `ignore!`, a `let` binding, or by
using the value (e.g. the `?` operator). Off by default.

### `--lint-panic` (R6b)

Emits `TUR-W0038` at panic call sites (`panic`/`tur_panic`, the contract
macros, `result-unwrap`/`option-unwrap`). `*-unwrap` also gets a
soft-deprecation hint toward `*-must`. Silence with a
`;; #lint-panic-allow` comment (top-of-file = whole file;
immediately-preceding = single call). Off by default.

### `--dump-kinds`

Prints the kind of every bound type to stdout after the kind-checking
pass. Useful for debugging HKT typeclass definitions.

### `--dump-effects`

Prints the inferred effect row for every function after elaboration, e.g.:

```
run-twice : forall [e]. (fn [(fn [] #{e} int)] #{e} int)
```

### `--dump-sizes` (SZ8)

Prints the inferred type-level size index for each sized-GADT constructor
application, e.g. `size: SVCons : (SizedVec 2)`; an un-inferable index
prints `(SizedVec ?)`.

### `--emit-abi-trace`

Prints, for each resolved call site, which C-level ABI path the emitter
takes to reach the callee. Codegen-time diagnostic: fires for
`tur emit-c` and `tur build` (the `tur run` interpreter still emits C
under the hood, so it also prints). Output goes to **stderr**, one line
per call:

```
abi-trace <line>:<col> <callee> <path>[ <clone-name>]
```

The `<path>` is one of four forms:

| Path | Meaning | When |
|---|---|---|
| `concrete-clone` | A monomorphized variant is called. The clone's mangled name is appended (e.g. `cube__spec__double_double`). | A polymorphic global is instantiated at a concrete type whose C ABI differs from the carrier (Phases F/G/H). |
| `carrier` | The generic `int64_t` carrier ABI is used; the value round-trips through the universal 64-bit handle. | Genuinely polymorphic call sites, type-erased containers, and instantiations whose C type matches the carrier (e.g. `cube` at `:int`). |
| `dictionary` | A typeclass method is dispatched through the instance machinery (`dict_arg` is set on the call). The callee name is the instance impl, e.g. `__inst_Eq_eq__Tuple2`. | Any `(.method ...)` typeclass dispatch, whether resolved to a direct instance call (Phase H) or a dictionary vtable load. |
| `polymorphic-wrapper` | The call goes through a `tur_poly_fn_t` rank-2 wrapper rather than a direct C function. | Calling a `forall`-quantified function parameter inside a rank-2/rank-N body. |

Example, against a file that exercises all four (see
`tests/fixtures/emit-abi-trace/`):

```sh
$ tur emit-c --emit-abi-trace input.tur >/dev/null
...
abi-trace 26:11 cube carrier
abi-trace 29:11 cube concrete-clone cube__spec__double_double
abi-trace 17:12 f polymorphic-wrapper
abi-trace 36:9 __inst_Eq_eq__Tuple2 dictionary
```

Notes:

- The stdlib is prepended to every program, so the trace is dominated by
  stdlib carrier calls. Grep for your own function names to find the call
  sites that matter.
- Line numbers are emitted without a file id, so a stdlib line N and a
  user-file line N print the same prefix; disambiguate by the callee name.
- The classification mirrors what the emitter actually emits: a call is
  reported as `concrete-clone` exactly when `emit_call_name` would resolve
  it to a specialization.

Use this flag to verify that a fully-typed hot path is being
monomorphized rather than silently falling back to the carrier ABI --
specialization is best-effort and otherwise silent on fallback.

---

## See Also

- [drop-x-flags-plan.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/drop-x-flags-plan.md) -- the
  plan that retired the `-X` flag surface.
- Per-feature guides linked in the **Removed Feature Flags** table above.
