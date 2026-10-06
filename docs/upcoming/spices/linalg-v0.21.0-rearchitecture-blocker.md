---
title: linalg rearchitecture against turmeric v0.21.0 — DONE
category: Spice-uplift blocker (Track C / U4 prerequisite)
status: RESOLVED — all 6 modules migrated, check-clean, 36-test suite green
verified-on: turmeric 0.21.0, main @ a826bce (post #465/#466/#467; built from source)
verified-by: turmeric-spices Claude (Track C, branch claude/track-c-turmeric-yi27rv)
---

# linalg — rearchitected for v0.21.0 (resolved)

## Resolution (2026-06-20)

linalg is fully migrated. All six `src/linalg/*.tur` modules `tur check`
clean and the rewritten `tests/linalg.tur` (36 checks) passes under
`tur test tests`. The #465 (separate-compilation codegen) and #466 (legacy
`:int`-pointer migration diagnostics/guide) changes landed in turmeric main
were the enabling work referenced as the blockers.

**The model that shipped** (dynamic modules `vec`/`mat`/`solve`/`decomp`/`fmt`):

- A vector is a typed `lavec` struct owning a heap `(Vec float)`; a matrix is a
  `mat` owning a row-major `(Vec float)`. Both are move-only — readers take the
  receiver `^borrow`, the `*-free` consume it. Element reads go through
  `(:: (vec-get (.data x) i) :float)` (carrier reinterpret per stdlib/vec).
- The `vec` type/ops are named `lavec` / `la-vec-*` to avoid colliding with the
  auto-loaded `stdlib/vec` prelude, which is now the backing store.
- Counted loops are `letrec` closures (`dotimes` is not a v0.21.0 form).
- The numeric kernels in `decomp`/`solve` (Cholesky, LU+pivot, Householder QR,
  fwd/back-sub, det, Q^T b) run in **inline C over the backing buffers** — this
  is both faster and sidesteps the codegen limitations noted below, and it was
  the cleanest place to fix the pre-existing paren bugs. `fmt` likewise formats
  in inline C over the buffer.
- `small` (the earlier `:copy` fixed-size module) is unchanged.

### Workarounds for turmeric codegen limitations found during the port

These are spice-side workarounds; each is a candidate turmeric report (the
list-API one is already on the turmeric agent's list):

1. **A hyphen in a `defstruct` type name did not survive C codegen.**
   `(defstruct la-vec ...)` type-checked but emitted `la-vec <var>` (invalid C).
   Worked around by hyphen-free type names (`lavec`, `cholfac`, `lufac`,
   `qrfac`); the *functions* keep hyphens fine.
   **RETIRED 2026-09-09:** re-tested on turmeric v0.46.0 -- a hyphenated
   `defstruct` type name now mangles to a valid C identifier
   (`la-vec` -> `tur_adt_la_hyvec`) and `make-struct` / `.field` round-trip
   correctly. The hyphen-free names already in the source are harmless and were
   left alone; new code need not avoid hyphens.
2. **A `^mut` float accumulator captured by a `letrec` closure does not
   propagate its mutations back out** (captured by value), and **threading a
   float accumulator through the recursive self-call mis-carriers to 0**. Both
   made pure-Turmeric float reductions return 0. ~~Worked around with a
   one-element heap `(Vec float)` accumulator cell.~~ **RETIRED 2026-06-21:**
   turmeric main #469 fixed the self-recursive float accumulator collapsing to
   the int carrier, so `la-vec-dot` / `la-vec-norm{,-1,-inf}` (vec.tur) and
   `mat-trace` / `mat-norm-fro` / `mat-norm-max` (mat.tur) now thread a plain
   `acc : float` through the `letrec` self-call. Verified: `tests/linalg.tur`
   green (36/36) against from-source `tur` @ `97dcd86`.
3. **No `-lm` for a cmake-dep-less spice**, so libm `sqrt`/`fabs` won't link.
   ~~Worked around with libm-free inline-C `la-sqrt` (Newton) / `la-fabs`.~~
   **RETIRED 2026-06-21:** turmeric main #471 links `-lm` unconditionally, so
   `la-sqrt`/`la-fabs` (vec.tur + mat.tur) now call libm `sqrt`/`fabs` directly.
4. **List API is half-present** (`cons` builtin survives; `car`/`cdr`/`null?`/
   `length` are gone; `(list ...)`/`tcons` are int-only so float lists can't be
   built). The cons-list `*-from-list` constructors were replaced with variadic
   `la-vec-of` / `mat-of` macros over `(Vec float)`.

`mat4-inv` in `small` is fully implemented (adjugate method: determinant by
complementary 2x2 minors, then the cofactor-transpose). Its only `panic` is the
singular-matrix guard. `tests/small.tur` covers it with four checks, including
the round-trips `ok 6 - mat4-inv inv(M)*M=I` and `ok 7 - mat4-inv M*inv(M)=I`.

---

## Historical record (the original blocker, pre-migration)

## Progress (2026-06-20, superseded by the Resolution above)

- **`linalg/small` is migrated and verified** — it is the self-contained,
  fixed-size module (`vec2/3/4`, `mat2/3/4`, the mat4 graphics ops). It now uses
  `:copy` struct *values* with `make-struct` + `(.field v)`, `^mut` locals for
  build-by-mutate, libm via `extern-c [x : float] : float`, and inline-C
  address-of (`&(m.m00)`) for the OpenGL `*-ptr` helpers. Checks clean and a
  cross-module smoke test (constructors, `vec3-cross`, `mat4-mul`,
  `mat4-mul-vec4`, `mat4-ptr`) compiles+runs. This is the **proven `:copy`
  template** for any other fixed-size struct work. Two notes: the constructor
  names moved `vecN` → `vecN-of` (the type name `vecN` is now the make-struct
  tag and can't double as a function), and `mat4-inv` was at that point **stubbed
  with a `panic`** -- its old body mixed `vec3-cross`/`vec4-dot` on the same
  values (only valid under int-pointer aliasing) and needed a type-correct
  reimpl + a numerical test. (Both landed; see the Resolution above.)
- **Remaining (5 modules):** `vec`/`mat` (core, need the `(Vec float)` + `^borrow`
  dynamic model), `solve`/`decomp`/`fmt` (same, plus the pre-existing paren
  bugs). These are the larger rewrite; `small` did not need `Vec`/borrows. **(All
  five are now done — see Resolution.)**

## Additional v0.21.0 idiom changes found during the `small` migration

Beyond `sizeof` / accessor-functions / `float64*` (below), the rewrite also hit:
`(float x)` coercion removed (type params `: float` instead, or `int->float`);
unary `(- x)` and reciprocal `(/ x)` removed (use `(- 0.0 x)` / `(/ 1.0 x)`);
struct field mutation needs a `^mut` binding; `defstruct Foo` reserves `Foo` as
the constructor tag (collides with a same-named `defn`); `(error ...)` is not a
turmeric form (use `(panic ...)`); a `^borrow :copy` struct is passed *by value*
in inline C (`v.x`, not `v->x`). These belong in the Report #2 migration note.

---

`linalg` does not build against turmeric v0.21.0. While paying down the Track C
type-hygiene work, an attempt was made to migrate it (rename the colliding
`vec-*` API to `la-vec-*`, swap `declare`→`extern-c`, migrate `defstruct`
syntax). That migration is **necessary but not sufficient**: two further
v0.21.0 changes are fundamentally incompatible with how linalg is written, and
fixing them is a rewrite, not a migration. The migration diff was therefore
**not committed** — linalg is left untouched at its prior (already-broken) state
so nothing regresses.

linalg stores every struct pointer as `:int` and hand-rolls allocation and
field access. v0.21.0 removed the two language features that model rests on.

## The five breaks (verified individually against main @ 48e99d9)

### Mechanical (a migration *can* fix these)

1. **`(declare …)` is no longer a valid form** → use `(extern-c …)`. linalg
   declares `malloc`/`free`/`printf`/`snprintf` this way in 5 files.
2. **Bare `defstruct` field syntax is rejected** — `(defstruct mat rows :int …)`
   must become `(defstruct mat [rows : int …])`.
3. **The public `vec-*` API hard-collides with the auto-loaded `stdlib/vec.tur`
   prelude** (`vec-new vec-len vec-get vec-set! vec-free`, and the
   `(defstruct vec …)` accessors `vec-len`/`vec-data`). Resolved by renaming the
   `linalg/vec` module API + type to `la-vec-*` / `la-vec` (mat-*, vec2/3/4 are
   unaffected).

### Architectural (a migration *cannot* fix these — full rewrite required)

4. **`sizeof` is no longer a Turmeric-level operator.** `(malloc (sizeof mat))`,
   `(sizeof :int)`, `(sizeof :float)` now error (`unknown function or operator
   'sizeof'` / `keyword in expression position requires -Xsymbols`). In v0.21.0
   `sizeof` exists only inside ```c blocks. linalg uses it pervasively to size
   its heap allocations.

5. **`defstruct` no longer generates `Struct-field` accessor *functions*; field
   access is `(.field obj)` on a typed struct value, and the type name is now a
   `make-struct` constructor.** Verified:
   ```turmeric
   (defstruct pt [x : int  y : int])
   (.x p)      ;; OK
   (pt-x p)    ;; error: unknown function or operator 'pt-x'
   ```
   linalg's entire surface is built on the accessor-function idiom over `:int`
   pointers — `(mat-rows m)`, `(set! (mat-rows m) v)`, `(vec-data v)`. None of
   these resolve anymore. In `small.tur` this also surfaces as
   `defstruct: 'vec2' is already defined`, because the new `vec2` constructor
   collides with the file's own `(defn vec2 …)`.

### Also: pre-existing, predates v0.21.0

`solve.tur`, `decomp.tur`, and `fmt.tur` carry **paren imbalances that the
reader rejects** (verified on the HEAD versions: `solve.tur:177 unterminated
list`, `decomp.tur:185 unterminated list`, `fmt.tur:188 unexpected ')'`). These
files have never parsed standalone; they are independent of v0.21.0 and must be
repaired as part of any linalg work regardless.

## Why this isn't a Track C "rename" ticket

Making linalg build requires rewriting every function to use typed struct values
+ `.field` access + `make-struct`, moving the raw `float64*` data arrays behind
inline-C (or a `:heap` typed pointer), and reworking the `:int`-pointer model
end-to-end. That is the U4 (sized-types) rearchitecture, not the U4
*prerequisite* rename. Nothing in the repo imports linalg, so it is not blocking
any other spice — but it cannot be made green by the Track C transformation set
alone.

## Options (for the owner to choose)

1. **Dedicated linalg rearchitecture task** — rewrite to the v0.21.0 typed-struct
   model (`.field` + `make-struct` + inline-C for the data arrays), folding in
   the mechanical migration above and the paren fixes. Largest, cleanest.
2. **turmeric-side compatibility decision** — if `sizeof` and/or accessor-function
   generation are intended to survive (other spices may rely on them), that is a
   compiler conversation to have in `turmeric-lang/turmeric`, not here.
3. **Defer** — leave linalg as-is (it is already red and unimported); revisit when
   U4 (sized linalg: `Vec n`/`Mat m n`) is actually scheduled, since that phase
   will rewrite these signatures anyway.

The spec-conformant mechanical migration (items 1–3 + the `mat_test.tur:136`
paren fix) was produced and verified clean per-file, but is held uncommitted
pending the rearchitecture decision so the branch carries no non-building code.
