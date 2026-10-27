# Recursive Types and Free Monad -- Implementation Plan

**Status:** Not started. RF0--RF4 planned.

**Prerequisites:** Phase 15 (typeclasses), HKT (higher-kinded types, `^f` / `^^f` kind syntax), Phase 2 (closures).

**Related:** [hkt-guide.md](guides/hkt-guide.md), [../guides/advanced-type-system-rationale.md](../guides/advanced-type-system-rationale.md), [archive/higher-kinded-types-plan.md](archive/higher-kinded-types-plan.md)

**Last updated:** 2026-05-15

---

## Summary

Turmeric's type system currently has no mechanism for a type definition to refer to itself. This rules out a class of useful abstractions -- most visibly the Free monad, but also fixed-point combinators at the type level, rose trees, and expression ASTs where a node may contain an arbitrary number of child nodes of the same type.

This plan tracks the work required to:

1. Allow type definitions to be self-referential (recursive types).
2. Verify, lower, and emit recursive types correctly.
3. Build `Free` and `Fix` as stdlib types that exercise the new machinery.
4. Document the feature and integrate it with the existing HKT / typeclass infrastructure.

---

## Motivation

### The Problem with Non-Recursive Types

Every Turmeric struct or variant today must be defined in terms of already-known, non-recursive types. This means the following are all impossible to express directly:

```turmeric
;; A tree where each node may have many children -- currently rejected
(defstruct Tree [value : int children : int])  ; "children" would need to be (list Tree)

;; A type-level fixed-point combinator -- currently rejected
(defstruct Fix [unfix : int])  ; "unfix" would need to be (^f (Fix ^f))

;; The Free monad -- currently rejected
;; Free f a = Pure a | Free (f (Free f a))
```

The last two cases additionally require HKT (higher-kinded type) arguments, but even the first is ruled out by the non-recursive type restriction.

### Motivating Use Cases

| Use case | Why it needs recursive types |
|---|---|
| Free monad | `Free f a` wraps `f (Free f a)` -- directly recursive in `f` |
| Rose tree / AST nodes | Each node holds a list of child nodes of the same type |
| Type-level fixed point (`Fix`) | `Fix f` unfolds to `f (Fix f)` |
| Mutual recursion (e.g., `Expr`/`Stmt`) | Two types each reference the other |
| Tidal-style pattern trees | Pattern alternatives may contain sub-patterns |

### The Free Monad as a Design Target

The Free monad is the motivating "proof of concept" for this plan. Once recursive types and the right HKT plumbing exist, `Free` can be implemented as a pure stdlib type with no compiler magic:

```turmeric
;;; Free -- the Free monad over a functor f.
;;;
;;; Free f a = Pure a | Suspend (f (Free f a))
;;;
;;; Any Functor f yields a Monad via Free f for free.
(defdata Free [^f a]
  (pure-free  [value :int])        ; wraps the pure value a
  (suspend    [step  :int]))       ; wraps one layer of f (Free f a)
```

`bind` for `Free f a` recurses on the structure:

```turmeric
(defn free-bind [ma : int fn : int] : int
  (match-free ma
    (pure-free v)   (fn v)
    (suspend   s)   (suspend (fmap s (fn [inner] (free-bind inner fn))))))
```

This pattern is the foundation of effect interpreters, trampolined computation, and extensible effects.

---

## Design

### What "Recursive Type" Means Here

A recursive type is any `defstruct` or `defdata` whose fields (or variant payloads) mention the type being defined, possibly under a type constructor:

```turmeric
;; Direct recursion
(defstruct ListNode [head : int tail : int])   ; tail :: option ListNode

;; Recursion under a functor argument (HKT)
(defdata Free [^f a]
  (pure-free [value :int])
  (suspend   [step  :int]))                  ; step :: f (Free f a)

;; Mutual recursion
(defstruct Expr [node : int])                 ; node :: ExprNode
(defdata ExprNode
  (lit  [n :int])
  (add  [l :int r :int])                     ; l, r :: Expr
  (call [fn :int args :int]))                ; args :: list Expr
```

In all cases the C representation is a heap pointer (`int64_t`) -- the indirection is what makes the layout finite.

### Representation in C

Recursive types are **always heap-allocated** and referenced by opaque `int64_t` pointer. No layout change is needed for the C backend -- the backend already represents all struct/variant values as tagged heap pointers. What is new is:

1. The elaborator must accept self-referential `defstruct` / `defdata` definitions without rejecting them as ill-formed.
2. The kind checker must allow a type variable to appear in a position that creates a cycle, provided the cycle passes through at least one pointer indirection (automatic for heap-allocated types).
3. Codegen must emit forward declarations for mutually recursive types.

### Scope for This Plan

| Feature | Included | Notes |
|---|---|---|
| Direct self-recursion in `defstruct` / `defdata` | Yes | Phase RF0 |
| Recursion under a type-constructor argument (`^f`) | Yes | Phase RF1 |
| Mutual recursion between two types | Yes | Phase RF1 |
| `Fix` stdlib type | Yes | Phase RF2 |
| `Free` monad stdlib type | Yes | Phase RF3 |
| Infinite-size types (no pointer indirection) | No | Statically rejected |
| Coinductive / lazy types | No | Deferred |

---

## Approach Evaluation

### Approach A: Allow Recursive Definitions, Pointer-Mediated Only (Recommended)

**Design:** The elaborator lifts the "no forward references" restriction for `defstruct` / `defdata`. It detects cycles and requires that every cycle pass through a heap-pointer field (which all aggregate fields already are). Self-referential fields are typed as `int64_t` (opaque pointer) at the C level, exactly as today.

**Pros:**
- Minimal compiler change: the C representation is already pointer-based.
- No new IR nodes required.
- Cycles through HKT arguments work automatically once cycle detection is in place.
- Forward declarations in C output handle mutual recursion cleanly.

**Cons:**
- Elaborator must grow a cycle-detection pass over type definitions.
- Kind checker must understand partially-applied type constructors in recursive position.

**Complexity:** Medium. The elaborator and kind checker are the only affected components.

### Approach B: Explicit Indirection Marker (`^box`)

**Design:** Require programmers to mark recursive fields with a `^box` annotation that signals "this field is heap-allocated and may refer back to the enclosing type".

**Pros:**
- Zero implicit cost -- every allocation is explicit.
- Easier to reason about memory layout.

**Cons:**
- Verbose for the common case (every recursive field needs annotation).
- Unnecessary: all aggregate types are already heap-allocated.
- Changes user-visible syntax.

**Verdict:** Rejected. The annotation would be noise because there is no case in Turmeric where a recursive field could be stack-allocated.

### Approach C: Iso-Recursive Types with Explicit `fold` / `unfold`

**Design:** Introduce `fold` / `unfold` operations (as in System F-mu) that wrap and unwrap one layer of recursion. Users write `(fold x)` to produce a recursive type and `(unfold r)` to consume it.

**Pros:**
- Theoretically clean.
- Explicit control over unrolling.

**Cons:**
- Significant syntactic overhead.
- No benefit over equi-recursive types for a C-targeting language.
- Unfamiliar to most Turmeric users.

**Verdict:** Rejected for v1. May be revisited if GADTs require it.

### Decision: Approach A

Approach A is the right fit for Turmeric. The runtime representation is already pointer-based, so no new overhead is introduced. The only changes are in the elaborator and kind checker.

---

## Phases

### Phase RF0 -- Recursive Struct and Data Definitions

**Goal:** Allow a `defstruct` or `defdata` definition to mention its own name in field / variant types.

**Tasks:**

- [ ] Extend the type elaborator (`src/elab.c`) to accept forward references to the type currently being defined.
- [ ] Add a cycle-detection pass that runs after all type definitions in a file are collected. Reject cycles that would produce an infinite-size layout (i.e., a cycle with no pointer indirection). In practice this means: only struct/data fields, which are already `int64_t` pointers, so all cycles are valid.
- [ ] Update error messages to distinguish "unknown type" from "illegal infinite-size type" (the latter being a statically sized struct field pointing to itself with no indirection, which cannot occur today but should have a clear diagnostic).
- [ ] Emit C forward declarations (`typedef struct ...;`) for all types before their bodies, so that mutually recursive `struct` definitions compile.
- [ ] Write fixture tests:
  - `tests/fixtures/recursive-types/self-referential-struct.tur` -- a `ListNode` that holds an optional tail of type `ListNode`.
  - `tests/fixtures/recursive-types/mutual-recursion.tur` -- `Expr` / `ExprNode` mutual recursion.
  - `tests/fixtures/recursive-types/simple-tree.tur` -- a binary tree `Tree` with left/right children of type `Tree`.

**Exit criterion:** The three fixture tests pass. Self-referential and mutually recursive type definitions are accepted by the elaborator, and the generated C compiles cleanly.

---

### Phase RF1 -- Recursion Under a Type Constructor (HKT Position)

**Goal:** Allow recursion where the recursive occurrence is wrapped under a higher-kinded type argument, as in `f (Fix f)` or `f (Free f a)`.

**Tasks:**

- [ ] Extend the kind checker to handle HKT type variables (`^f`) in recursive position. The kind of `f (Fix f)` must unify with `* -> *` applied to `Fix f`, producing kind `*` -- confirm this unifies correctly.
- [ ] Verify that the elaborator does not reject `(defdata Free [^f a] (pure-free [value :int]) (suspend [step :int]))` where `step` conceptually has type `f (Free f a)` (stored as opaque `int64_t`).
- [ ] Extend cycle detection to follow HKT application nodes when checking for infinite-size cycles.
- [ ] Write fixture tests:
  - `tests/fixtures/recursive-types/fix-type.tur` -- define and use `Fix`.
  - `tests/fixtures/recursive-types/hkt-recursive.tur` -- a simple type parameterised by `^f` that recurses through `f`.

**Exit criterion:** The new fixtures pass. HKT-recursive type definitions are accepted and generate correct C.

---

### Phase RF2 -- `Fix` Type in Stdlib

**Goal:** Ship `stdlib/fix.tur` with the type-level fixed-point combinator `Fix` and its core operations.

**API surface:**

```turmeric
;;; Fix -- the fixed-point type of a functor.
;;;
;;; Fix f is the least fixed point of the functor f, i.e.
;;; Fix f ≅ f (Fix f).
(defdata Fix [^f]
  (roll [unfix :int]))  ; unfix :: f (Fix f)

;;; roll -- wrap one layer of f into a Fix value.
;;;
;;; Parameters:
;;;   layer -- a value of type (f (Fix f))
;;;
;;; Returns:
;;;   A Fix value wrapping layer.
;;;
;;; Example:
;;;   (roll (__opt_some (roll (__opt_none))))
;;;
;;; Since: Phase RF2
(defn roll [layer : int] : int ...)

;;; unroll -- unwrap one layer of Fix.
;;;
;;; Parameters:
;;;   fix -- a Fix value
;;;
;;; Returns:
;;;   The inner f (Fix f) value.
;;;
;;; Example:
;;;   (unroll (roll (__opt_some (roll (__opt_none)))))
;;;
;;; Since: Phase RF2
(defn unroll [fix : int] : int ...)

;;; cata -- catamorphism: fold a Fix value using an algebra.
;;;
;;; Parameters:
;;;   alg -- a function from (f a) to a (the F-algebra)
;;;   fix -- a Fix value to fold
;;;
;;; Returns:
;;;   The result of recursively applying alg to each layer.
;;;
;;; Example:
;;;   (cata (fn [node] (match-expr node ...)) expr-tree)
;;;
;;; Since: Phase RF2
(defn cata [alg : int fix : int] : int ...)
```

**Tasks:**

- [ ] Implement `roll` / `unroll` using inline C allocation (similar to `__opt_some` / `__opt_none`).
- [ ] Implement `cata` -- the catamorphism. Requires `fmap` from `Functor ^f` to recurse through the layer.
- [ ] Add `definstance Functor [Fix]` using `cata` and the inner functor's `fmap`.
- [ ] Write fixture tests in `tests/fixtures/fix/`.
- [ ] Add `;;;` docstrings to all public functions.

**Exit criterion:** `roll`, `unroll`, and `cata` work in fixture tests. The catamorphism correctly folds a `Fix`-encoded expression tree.

---

### Phase RF3 -- `Free` Monad in Stdlib

**Goal:** Ship `stdlib/free.tur` with the Free monad and its `Functor`, `Applicative`, and `Monad` instances.

**API surface:**

```turmeric
;;; Free -- the Free monad over a functor f.
;;;
;;; Free f a = PureFree a | Suspend (f (Free f a))
;;;
;;; Any Functor ^f yields a Monad over Free ^f for free,
;;; without requiring ^f itself to be a monad.
(defdata Free [^f a]
  (pure-free [value :int])
  (suspend   [step  :int]))

;;; free-pure -- lift a pure value into Free.
;;;
;;; Parameters:
;;;   x -- the value to lift
;;;
;;; Returns:
;;;   A Free value in the pure leaf case.
;;;
;;; Example:
;;;   (free-pure 42)  ; => (pure-free 42)
;;;
;;; Since: Phase RF3
(defn free-pure [x : int] : int ...)

;;; free-lift -- lift one layer of f into Free.
;;;
;;; Parameters:
;;;   fx -- a value of type (f a)
;;;
;;; Returns:
;;;   A Free value that suspends on fx and returns the inner value.
;;;
;;; Example:
;;;   (free-lift (__opt_some 5))
;;;
;;; Since: Phase RF3
(defn free-lift [fx : int] : int ...)

;;; free-bind -- monadic bind for Free.
;;;
;;; Parameters:
;;;   ma -- a Free value
;;;   fn -- a function from a to (Free f b)
;;;
;;; Returns:
;;;   A new Free value representing the sequenced computation.
;;;
;;; Example:
;;;   (free-bind (free-pure 3) (fn [x] (free-pure (* x 2))))  ; => (pure-free 6)
;;;
;;; Since: Phase RF3
(defn free-bind [ma : int fn : int] : int ...)

;;; free-fmap -- functor map for Free.
;;;
;;; Parameters:
;;;   free -- a Free value
;;;   fn   -- a function from a to b
;;;
;;; Returns:
;;;   A new Free value with fn applied to all pure leaves.
;;;
;;; Since: Phase RF3
(defn free-fmap [free : int fn : int] : int ...)

;;; free-run -- interpret a Free computation with a natural transformation.
;;;
;;; Parameters:
;;;   interp -- a function from (f a) to (m a) for some Monad m
;;;   free   -- a Free computation to run
;;;
;;; Returns:
;;;   The result of running free under interp.
;;;
;;; Example:
;;;   (free-run option-interp my-program)
;;;
;;; Since: Phase RF3
(defn free-run [interp : int free : int] : int ...)

;; Typeclass instances
(definstance Functor [Free]
  (fmap [free fn] (free-fmap free fn)))

(definstance Monad [Free]
  (bind [ma fn] (free-bind ma fn)))
```

**Tasks:**

- [ ] Implement `free-pure` and `free-lift`.
- [ ] Implement `free-bind` (recursive descent; terminates because `PureFree` is the base case).
- [ ] Implement `free-fmap` using `free-bind` and `free-pure`.
- [ ] Implement `free-run` -- the natural transformation interpreter.
- [ ] Register `Functor` and `Monad` instances.
- [ ] Write fixture tests in `tests/fixtures/free/`:
  - `pure.tur` -- `free-pure` and `free-bind` with pure values.
  - `lift-bind.tur` -- lift an `option` layer, bind over it.
  - `interpreter.tur` -- define a small DSL as a `Free` program and run it with a concrete interpreter.
  - `monad-laws.tur` -- left identity, right identity, associativity.
- [ ] Add `;;;` docstrings to all public functions.

**Exit criterion:** All four fixture files pass. The monad laws test confirms the three laws hold for the `option` functor.

---

### Phase RF4 -- Integration, Docs, and Polish

**Goal:** Ensure recursive types and the new stdlib modules are integrated with the rest of the toolchain.

**Tasks:**

- [ ] Update `docs/guides/hkt-guide.md`:
  - Remove or update the "Recursive types / Free monad: Not yet supported" limitation.
  - Add a new section "Recursive Types and Fix / Free" with examples and links to `stdlib/fix.tur` and `stdlib/free.tur`.
- [ ] Run `just docs` to regenerate `docs/api/` and `stdlib/docstrings.tur`.
- [ ] Add `stdlib/fix.tur` and `stdlib/free.tur` to the stdlib layout table in `CLAUDE.md`.
- [ ] Add `fix` and `free` entries to the standard HKT typeclass table in `docs/guides/hkt-guide.md`.
- [ ] Verify WASM build (`just wasm`) compiles cleanly after the new stdlib files are added.
- [ ] Add benchmark fixtures to `tests/benchmarks/` measuring `free-bind` overhead vs direct recursion.
- [ ] Review and tighten error messages from the cycle-detection pass (RF0) against the new test surface.

**Exit criterion:** `just test` passes. `just wasm` succeeds. The HKT guide no longer lists "Recursive types / Free monad" as unsupported.

---

## Compiler Touchpoints

| File | Change | Phase |
|---|---|---|
| `src/elab.c` | Accept forward references within a definition block; add cycle-detection pass | RF0 |
| `src/elab.c` | Extend kind checker to handle HKT variables in recursive position | RF1 |
| `src/emit.c` | Emit forward `typedef struct` declarations before type bodies | RF0 |
| `src/typeclass.h` | No change expected; recursive types use existing HKT machinery | -- |
| `src/expr.h` | No change expected | -- |
| `stdlib/fix.tur` | New file | RF2 |
| `stdlib/free.tur` | New file | RF3 |
| `stdlib/typeclass.tur` | Add `Fix` and `Free` instance stubs | RF2, RF3 |

---

## Testing Strategy

### Unit Fixtures

Each phase introduces dedicated fixtures under `tests/fixtures/recursive-types/` (RF0--RF1) and `tests/fixtures/fix/` / `tests/fixtures/free/` (RF2--RF3).

### Negative / Diagnostic Tests

| Scenario | Expected diagnostic |
|---|---|
| Truly infinite-size cycle (impossible in Turmeric but should error clearly) | "type `T` would have infinite size; all recursive fields must be heap-allocated" |
| Using a recursive type without HKT support (RF0 only) | Accepted -- direct recursion works without HKT |
| `Free` used without a `Functor` instance for `f` | Type error from existing typeclass machinery |

### Monad Law Tests

`tests/fixtures/free/monad-laws.tur` checks all three monad laws against a concrete functor (`option`):

1. Left identity: `(free-bind (free-pure x) f) == (f x)`
2. Right identity: `(free-bind m free-pure) == m`
3. Associativity: `(free-bind (free-bind m f) g) == (free-bind m (fn [x] (free-bind (f x) g)))`

Equality is checked structurally using a helper that compares `Free` values recursively.

---

## Open Questions

1. **Termination of `free-bind` on divergent trees**: `free-bind` is structurally recursive on `Free`, which terminates if the `Free` value is finite. Infinite `Free` trees (coinductive) are not supported in this plan. Should the runtime detect non-termination, or is it the caller's responsibility?
   - Proposed for v1: caller's responsibility. Document that `free-bind` assumes a finite tree.

2. **`free-run` and effect integration**: Should `free-run` produce a `result` in case the interpreter can fail, or always return `int`? Wrapping the return in `result` would make error handling cleaner.
   - Proposed for v1: return `int` (opaque). Callers wrap in `result` if needed.

3. **Mutual recursion across files**: RF0 covers mutual recursion within a single file. Cross-file mutual recursion (type `A` in `a.tur` references type `B` in `b.tur` which references `A`) requires a two-pass declaration strategy. Is this in scope?
   - Proposed: defer to a later phase. Document the limitation.

4. **`Ana` (anamorphism) and `Hylo` (hylomorphism)**: Should `stdlib/fix.tur` also ship `ana` (unfold) and `hylo` (fold-unfold fusion)?
   - Proposed: include `ana` in RF2, defer `hylo` to RF4 or later.

---

## Relationship to Other Plans

| Plan | Relationship |
|---|---|
| [hkt-guide.md](guides/hkt-guide.md) | This plan resolves limitation 4 in the Known Limitations section |
| [../guides/advanced-type-system-rationale.md](../guides/advanced-type-system-rationale.md) | GADTs (planned for v2) may interact with recursive types; this plan is prerequisite |
| [lazy-sequences-plan.tur](lazy-sequences-plan.md) | `Free` can model lazy sequences via the `Identity` functor; no direct dependency |
| [generator-functions-plan.md](generator-functions-plan.md) | Generators could be encoded as `Free`; integration is a stretch goal, not in scope here |

---

*Last updated: 2026-05-15*
