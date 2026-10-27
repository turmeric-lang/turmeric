# Test-Suite Idioms Cleanup Plan

A pass over `tests/fixtures/` (952 directories, ~1278 `.tur` files) surfaced
recurring non-idiomatic patterns that exist not because the test author
preferred them, but because the language did not give them a better option.
Each is a **language-limitation tell**: fix the limitation and the workaround
goes away from every fixture that uses it.

This plan groups the findings by root cause, lists representative
file:line evidence, and proposes the language-side work needed to retire
each pattern. Phases are ordered by impact ÷ effort.

---

## 1. Fat-closure capture-forcing dummies

**Symptom.** A `(let [_ x] ...)` or a captured-but-unused outer binding
exists for the sole purpose of forcing the lambda to be emitted in the
fat-closure (env + thunk) calling convention rather than as a bare
function pointer.

**Evidence.**
- `tests/fixtures/backtrack-fresh/input.tur:52-54` --
  `;; Capture zero to force a fat-closure (required by fresh's calling convention).`
  `(let [zero 0] ... (fn [x] (let [_ zero] (mreturn ...)))`
- `tests/fixtures/free/pure/input.tur:9` -- `(let [dummy 0  ;; capture forces fat-closure emission`
- `tests/fixtures/reactor-timer/input.tur:32` -- `;; use-val: forces fat-closure generation by capturing a value from the outer scope.`
- `tests/fixtures/reactor-fd-writable/input.tur:37` -- `;; No-op: used to force fat-closure allocation when sentinel is captured.`
- Pattern repeats verbatim across `logic-query`, `logic-conjoined`,
  `logic-unify-fail`, `parsec-basic`, `parsec-json-subset`, and
  several `hkt-closures-*` fixtures.

**Root cause.** A non-capturing `fn` codegens as a plain C function
pointer; capturing functions codegen as a `{thunk, env...}` fat
struct. Anything that consumes a closure via fat-call (HKT methods,
`fresh`, reactor callbacks) crashes or misreads memory when handed a
bare function pointer. The user has to introduce a phantom capture to
flip the representation.

**Fix options.**
1. **Always-fat closures at fat-call sites.** When the callee
   expects a fat closure (known from the parameter type, e.g. an HKT
   method or any first-class function value), wrap a non-capturing
   `fn` in a trivial `{thunk, _}` shim at the call site. Removes the
   workaround entirely.
2. **`#[fat]` ascription on the lambda** (escape hatch for cases
   where representation cannot be inferred from context).
3. **Diagnostic.** If a bare function pointer reaches a fat-call site
   today, raise a typed error instead of letting the user discover it
   via segfault.

**Cleanup work after the fix.** A scripted sweep removes ~52
`(let [_ name] ...)` and ~7 `;; force ... closure` comments.

---

## 2. Sentinel integers (`0`, `-1`, `INT64_MIN`) standing in for nil/error

**Symptom.** `:int`-returning functions encode "absence" or "failure"
as a magic integer because there is no convenient `:Option`/`:Result`
return-type plumbing through inline-C boundaries.

**Evidence.**
- `tests/fixtures/backtrack-fresh/input.tur:4-5` -- `(defn mzero [] :int \`\`\`c return 0; \`\`\`)`
- `tests/fixtures/backtrack-fresh/input.tur:36` -- `int64_t unbound = INT64_MIN; /* sentinel: unbound logic variable */`
- `tests/fixtures/logic-query/input.tur:60` -- `(if (= ns -1) (mzero) (mreturn ns))`
- `tests/fixtures/hkt-free-stdlib/input.tur:41` -- `if (fr->tag == 0) { return fr->val; }` (Pure vs Suspend by raw tag)
- ~31 fixtures total carry sentinel/INT64_MIN/`return -1` patterns.

**Root cause.** Three sub-issues:
- `:Option`/`:Result` cannot ergonomically cross an inline-C
  boundary, so handwritten C helpers fall back to `:int` + sentinel.
- No `:void` discipline -- functions return `0` "because they have
  to return something."
- Tagged-union access from inline-C requires manual `tag == 0` checks
  on the runtime layout.

**Fix options.**
1. **C ABI for `:Option<int>` / `:Result<int,E>`.** Document the
   layout, expose `tur_some(x)` / `tur_none()` macros for inline-C
   blocks, so `mzero` becomes `(defn mzero [] :Option<int> ...)`.
2. **Allow `:void` returns from inline-C** without forcing a `return 0;`.
3. **Tag accessor macros** (e.g. `TUR_TAG(fr)`, `TUR_PAYLOAD(fr, Pure)`)
   so fixtures stop hardcoding `tag == 0`.

**Cleanup work.** Migrate the ~31 sentinel fixtures off magic
integers; deletes ~120 lines of inline-C boilerplate.

---

## 3. No `let*` / no sequential-binding sugar

**Symptom.** Deeply nested `(let [...] (let [...] (let [...] ...)))`
pyramids for what is morally a linear sequence of bindings.

**Evidence.**
- `tests/fixtures/hamt-lisp-show/input.tur:30-42` -- six consecutive
  single-binding `let` forms creating a 7-level indent pyramid.
- `tests/fixtures/hkt-free-stdlib/input.tur:88-118` -- nested lets
  for chained bind ops.
- 2019 total `let` forms in fixtures; zero uses of `let*`.

**Root cause.** Turmeric `let` is parallel-binding only. There is no
`let*` (sequential), and no `do`-block sugar that allows
`(let-binding name value)` interleaved with effects.

**Fix options.**
1. **Add `let*`** with sequential scoping (each binding sees previous
   bindings). Cheap macro layer over nested `let`; existing fixtures
   reformat trivially.
2. **Add `do!` / sweet-exp block form** that flattens
   `(let [a x] (let [b y] body))` into
   `do! { a := x ; b := y ; body }` style. Lower priority -- `let*`
   covers 95% of cases.

**Cleanup work.** Reformat the ~80 pyramid fixtures; reduces average
indent depth by 3-4 levels.

---

## 4. Mandatory `::` ascriptions for empty-container construction

**Symptom.** `(vec-new)`, `(set-new)`, `(map-new)` cannot be used
without an explicit `(:: ... (Vec int))` type annotation, even when
later usage pins the element type.

**Evidence.**
- `tests/fixtures/vec-eq-ascribed/input.tur:20-22` -- three `::` annotations on consecutive `(vec-new)` calls.
- `tests/fixtures/set-of-tvec-eq/input.tur:1-6` -- `(:: ... (Set (Vec int)))` chain.
- The same fixture's own header comment (lines 10-17) flags this as
  "F3 receiver-type-recovery, dictionary passing is the missing piece."

**Root cause.** Type inference is forward-only at construction sites
and cannot backsolve from `.eq?`/`.push!` usage to the empty
container's element type. ~70 fixtures carry redundant `::` purely as
inference scaffolding.

**Fix options.**
1. **Bidirectional inference at container literals.** When `vec-new`
   is later constrained by a typed operation, propagate the
   constraint backward. Tracked as F3 in the typeclass roadmap.
2. **Typed constructor sugar** -- `[]:int`, `#{}:int`,
   `#map{}:cstr->int` -- gives the user a one-token escape that is
   shorter than `(:: (vec-new) (Vec int))`. Compatible with #1; ships
   value before F3 lands.
   **Status: done** for vecs and sets -- shipped as `[]:T` and
   `#set{}:T` (the documented data-literal set spelling; `#set{...}`,
   not the older `#{...}` map form). Map typed-empty suffix is deferred
   (two type params). See the implementation-status table below.

---

## 5. Inline-C `#{Unsafe}` blocks for trivial struct ops

**Symptom.** Allocating a struct, reading a field, writing a field --
each costs a 5-line inline-C block.

**Evidence.**
- `tests/fixtures/sized-sz2-layout/input.tur:25-43` -- malloc +
  field-read + field-write, three separate `#{Unsafe}` defns.
- `tests/fixtures/hkt-closures/input.tur:17-36` -- `__opt_some`,
  `__opt_some?`, `__opt_unwrap` reimplemented in C per fixture.
- 21 fixtures use `#{Unsafe}` blocks; the duplication across them
  reveals missing primitives.

**Root cause.** No surface syntax for record construction or field
access on heap structs allocated outside `defstruct`. Authors fall
back to malloc + cast + `->field`.

**Fix options.**
1. **`defrecord` or extend `defstruct`** to cover heap-allocated
   variant types (Option, Free, Cell) with auto-generated
   accessors. The `__opt_*` helpers in `hkt-closures` should be
   stdlib primitives, not per-fixture inline-C.
2. **Inline field-access syntax** -- `(. ptr field)` and
   `(set-field! ptr field val)` -- generated to safe C, replacing
   the manual `((Cell*)(intptr_t)ptr)->field` pattern.

---

## 6. Manual fat-closure threading via `tur_poly_fn_t`-style C

**Symptom.** Fixtures hand-write the
`int64_t *fat = ...; ((int64_t(*)(void*, int64_t))(intptr_t)fat[0])(fat, arg)`
incantation to call a closure from inline-C.

**Evidence.**
- `tests/fixtures/stdlib-arrow/input.tur:12-16` -- `tur-call-closure1` helper.
- `tests/fixtures/parsec-basic/input.tur:56-60` -- same incantation inlined into `apply-parser`.
- `tests/fixtures/hkt-closures/input.tur:5-11` -- header comment
  explicitly cites this as a pre-TY5 workaround.

**Root cause.** No public C ABI macro for "apply this fat closure to
N args." Authors paste the cast every time.

**Fix.** Ship `TUR_APPLY1(f, a)`, `TUR_APPLY2(f, a, b)`, ... macros in
the runtime header. Mechanical sweep replaces the casts.

---

## 7. Manual typeclass dictionary threading in instances

**Symptom.** `(definstance Eq (Vec A) ...)` bodies hardcode `(= a b)`
on elements instead of recursively dispatching through the `Eq A`
instance. Authors note this in comments and write helper-based
workarounds.

**Evidence.**
- `tests/fixtures/vec-eq-ascribed/input.tur:14-17` -- "the
  Eq[Vec][(Eq A)] instance body hardcodes (= a b)... dictionary
  passing is the missing piece. See vec-of-vec-eq-manual for the
  manual recursive workaround."
- Affects 5+ fixtures (`vec-eq-*`, `set-of-tvec-eq`,
  `vec-of-vec-eq-manual`).

**Root cause.** Typeclass constraints declared on an instance head
(`(definstance Eq (Vec A) :where (Eq A) ...)`) are not propagated to
the body as an implicit dictionary parameter. The instance has no way
to call `.eq?` on its element type.

**Fix.** F3-2..F3-6 in the typeclass roadmap. Out of scope for this
plan to design; in scope to track as the blocking item for ~5
fixtures.

---

## Phasing

| Phase | Patterns covered | Why first |
|-------|------------------|-----------|
| **A.** Fat-closure shim + `TUR_APPLY*` macros (#1, #6) | Removes the most viral workaround; unblocks reactor/HKT/parsec fixtures | Self-contained codegen change; no type-system risk |
| **B.** `let*` (#3) | One macro, zero risk, immediate readability win | Trivial |
| **C.** Inline-C `:Option`/`:Result` ABI + `:void` returns (#2) | Removes 31 fixtures' worth of sentinel integers | Needs careful ABI design but unlocks the most semantic cleanup |
| **D.** Record syntax / field accessors (#5) | Retires 21 `#{Unsafe}` fixtures' worth of malloc boilerplate | Larger surface area; do after C lands |
| **E.** Typed-empty-container sugar (#4, partial) | Cheap user-visible win while waiting on F3 | Independent of F3 |
| **F.** Track F3 dictionary-passing for #7 | Owned by typeclass roadmap | Reference, not new work |

## Acceptance signal

For each phase, success is measured by **fixture diffs**: the
language change lands together with a sweep that deletes the
workaround from the fixtures listed above. A phase is "done" when
`grep` for its tell pattern (e.g. `;; force.*fat-closure`, `INT64_MIN`,
`let [_ `, nested-`let` pyramids of depth >=4) returns the expected
near-zero count.

## Implementation status

| Phase | Status | Notes |
|-------|--------|-------|
| **B.** `let*` (#3) | **Done** | Implemented natively as a desugaring special form (`elab_letstar`), so both the compiled (`tur`) and tree-walking (`turi`) paths get it via the shared `EX_LET` tree. An earlier macro attempt was abandoned: the compile-time macro evaluator force-evaluates `fn`/`if`/`let` forms found inside a binding-vector literal, so any closure-valued binding broke expansion. Fixed a latent `elab_let` NULL-`memcpy` crash for empty binding vectors (now reachable via `(let* [] ...)`). Swept `hamt-lisp-show`, `hkt-free-stdlib`; added the `let-star` fixture (turi-allowlisted). |
| **A #6.** `TUR_APPLY*` macros | **Done** | `TUR_APPLY1..4` + `TUR_CLOSURE_FN` ship in the runtime preamble. Swept 25 inline-C fixtures (parsec/logic/backtrack/hkt families) plus `stdlib-arrow`'s `tur-call-closure1`, dropping the now-dead `thunk`/`fn_ptr` intermediates. All 75 `expected.c` snapshots regenerated for the new preamble. |
| **C item 2.** `:void` inline-C returns | **Already satisfied** | A `:void` defn with an inline-C body needs no `return` today; verified. |
| **C #1.** inline-C `:Option`/`:Result` ABI | **Done** | Shipped `tur_some` / `TUR_NONE` / `tur_is_some` / `tur_opt_value` and `tur_ok` / `tur_err` / `tur_is_ok` / `tur_ok_value` / `tur_err_value` in the runtime preamble (next to `TUR_APPLY*`), over the canonical Option `{ bool is_some; int64_t value; }` (none == 0) and Result `{ bool is_ok; int64_t ok_val; int64_t err_val; }` heap layouts. An inline-C block now builds/inspects real `:Option<int>` / `:Result<int,int>` values instead of a magic sentinel (`-1` / `INT64_MIN` / `0`-as-absent), and the bytes are layout-identical to `stdlib/option.tur` / `stdlib/result.tur`, so values flow transparently across the inline-C boundary **in both directions** (stdlib `some?`/`ok?` reads a C-built value; a C consumer reads a stdlib-built one). Dogfooded: the canonical `some`/`none`/`some?`/`unwrap-or` and `ok`/`err`/`ok?`/`err?` bodies now call the helpers, retiring their hand-rolled `struct { ... }` cast boilerplate. New fixture `option-result-c-abi` exercises all helpers + bidirectional interop. All 79 `expected.c` snapshots regenerated for the new preamble. |
| **A #1.** Fat-closure auto-shim | **Done** | Shipped as the **sound** realization: a type-driven blanket shim is unsafe (the reactor *fat*-calls an `:int` callback while `hamt` *raw*-calls a `:ptr` callback -- the parameter type cannot tell a fat consumer from a raw-C callback), so the fat-call contract is made explicit via a new **`^fat` parameter marker** (on both `defn` and `extern-c`, threaded as `arg_fat[]` next to `arg_linear`/`arg_affine`). A bare non-capturing `fn` passed to a `^fat` parameter is wrapped in `EX_FN_TO_FAT`, which allocates `{ __tur_fatshim<arity>, orig_fn }`; the per-arity shim thunks live in the runtime preamble (int64_t carrier ABI, matching `TUR_APPLY` and the reactor casts) and forward args after reading the bare fn pointer from slot 1. A capturing closure passes through; nil/`0` is a null callback; **diagnostic half**: a non-fn argument to a `^fat` parameter is a typed error instead of a segfault (arity > 5 also rejected). `EX_FN_TO_FAT` carries `TY_PTR_VOID` like `EX_CLOSURE`, reusing the existing arg-emission casts, and is transparent in the turi interpreter. Swept the dummies: reactor (8 fixtures + `^fat` cb), `backtrack-fresh`, `backtrack-bind` (`mbind`), the seq combinators (`stdlib/seq/*` `^fat` params) + 8 `seq-*` fixtures, and 28 redundant `(let [_ x] ...)` wrappers in logic/parsec (those closures already captured a used value). Also fixed a latent codegen bug the cleanup exposed: a nil-typed user call in a non-nil function's tail position (`(defn main [] :int (some-void-call))`) was lowered with `emit_value` and discarded; it now lowers with `emit_stmt`. Fixtures `fat-shim` + `errors/fat-param-non-fn` added. **Return position** (follow-up, done): the `^fat` marker now also applies to a function's return type (`(defn f [] ^fat :ptr<void> ...)`), auto-shimming a returned non-capturing fn into a fat closure (`EX_FN_TO_FAT`) at every tail/return leaf -- symmetric with the parameter marker. The 2 genuine return-position dummies in `logic-fresh`/`logic-reify` were swept. See [fat-closure-return-position-plan.md](history/fat-closure-return-position-plan.md). **`stdlib/free.tur` sweep** (follow-up, done): marked the three Free combinators `free-bind`/`free-fmap`/`free-run` `^fat` (carrier param switched `:fn`->`:int`, matching the `seq/*` combinators) and retired the capture-forcing dummies from the `free-pure`/`free-lift-bind`/`free-interpreter` fixtures -- the last live cluster of the #1 tell-pattern. These three fixtures were previously dead (nested `tests/fixtures/free/<name>/`, invisible to `run.sh`'s one-level glob and absent from the turi allowlist) **and** broken (they predated `free-bind`/`free-run` becoming `#{Unsafe}`, so they no longer compiled); they were repaired (wrapped the unsafe combinator calls), re-homed to one-level `free-*` dirs so `run.sh` runs them compiled (leak-checked, matching the sibling `hkt-free-*` fixtures), and now pass on both the compiled and turi paths. A separate pre-existing codegen bug surfaced during the sweep -- a capturing closure constructed inside an `(unsafe ...)` block drops its capture -- documented as **KB-IDIOM-1** below (since **fixed** -- `collect_handle_captures` now folds a nested closure's captures into the handle's `body_env`; `free-lift-bind` was un-sidestepped to build its interpreter closure inside the `unsafe` block, and the positive fixture `unsafe-closure-capture` was added). |
| **C #3.** tag-accessor sweep | **Not started** | The remaining sentinel/`tag == 0` users are the bespoke hand-written inline-C engines (logic `Term` = `{ tag; data1; data2; }`, the Free monad = `{ tag; val; }`). Their layouts are fixture-specific and intertwined with multi-tag dispatch (`tag == 1/2/3`), so a canonical `TUR_TAG`/`TUR_PAYLOAD` macro does not cleanly retire them; several are also intentional inline-C interop tests (see Out of scope). Deferred. |
| **E item 2.** typed-empty-container sugar (#4) | **Done** | Shipped a reader-level desugar: a fused `:T` element-type suffix on a vec or set literal (`[]:int`, `#set{}:cstr`, `[]:(Vec int)`) lowers to `(:: <literal> (Vec T))` / `(:: <literal> (Set T))`, recovering an empty container's element type without the verbose `(:: (vec-new) (Vec int))` ascription. Implemented in `reader.c` (`maybe_container_type_suffix`), gated on `-Xdata-literals` for vecs (the `[...]` value literal is itself flag-gated) and adjacent to the closer (no whitespace), so binding vectors are unaffected and `::` is not mistaken for the suffix; codegen is byte-identical to the explicit ascription. A missing type after `:` is a typed read error. Swept all 8 `(:: (vec-new) (Vec T))` fixtures (`vec-eq-ascribed`, `vec-eq-ascribed-multi`, `set-of-tvec-eq`, `result-of-typed-eq`, `typed-slots/let-vec-new`, `map-of-tvec-eq`, `vec-of-tvec-eq`, `option-of-tvec-eq`) to `[]:T`; the `(:: (set-add ...) ...)` / `(:: (map-assoc ...) ...)` ascriptions on non-literal call results legitimately remain. Added fixtures `data-literal-typed-empty` and `errors/container-suffix-no-type`; guide updated. (Map typed-empty suffix is future work -- two type params; not in the #4 evidence.) |
| **D, E item 1 (bidirectional inference).** | **Not started** | `defrecord`/field-accessor syntax (D) and full backward inference at container literals (E item 1, F3-tracked) each remain a substantial reader/codegen/ABI change with broad snapshot churn. |
| **F.** F3 dictionary passing (#7) | Tracking only | Owned by the typeclass roadmap. |

## Known bugs surfaced during the sweeps

These are real codegen bugs uncovered while removing the workarounds above.
They are **not** idiom cleanup -- they are tracked here because the sweep is
what exposed them and because a fixture currently has to dodge them.

### KB-IDIOM-1. Capture dropped for a closure constructed inside `(unsafe ...)`

**Status: FIXED.** The handle-body free-variable analysis
(`collect_handle_captures` in `emit_core.c`) did not descend into a nested
closure's captures, so a variable referenced *only* through a closure built
inside the `unsafe` body was never threaded into `body_env`
(`has_captures` stayed false). The fix adds an `EX_CLOSURE` case (and an
`EX_FN_TO_FAT` descent) to the capture walk: a closure's pre-computed
`captures[]` -- already transitive, so an outer closure carries everything its
inner closures need -- are folded into the handle's capture set (minus any
binding defined within the handle body). With the var in `body_env`, the
closure's env-init resolves through `name_for_binding` to `__henv_N->field`
instead of the out-of-scope `<name>_NNN`. Positive fixture
`unsafe-closure-capture` (capturing closure with *two* captures built inside
`unsafe`); the `free-lift-bind` workaround was retired (its `interp` closure is
now built inside the `unsafe` block). The text below documents the original bug
for the record.

**Symptom.** A closure that captures an outer binding **and is constructed
inside an `(unsafe ...)` block** silently drops the capture. The generated C
references the captured variable in the wrong scope, so `cc` fails with
`'<name>_NNN' undeclared (first use in this function)` inside a
`__handle_body_N` function. (If a future change made the surrounding code
compile, the value would simply be wrong rather than a hard error.)

**Root cause.** `unsafe` lowers through the effect-handler machinery: its body
becomes a separate `__handle_body_N` C function. The closure-capture lowering
emits the env-init (`__t->field = <name>_NNN;`) in the handler-body function but
resolves `<name>_NNN` against the *enclosing* function's locals, which are not
in scope inside `__handle_body_N`. The capture binding is never threaded across
the handler-body boundary.

**Minimal repro** (`tur build` fails at the `cc` step):

```turmeric
(load "stdlib/free.tur")
(defn main [] : int
  (let [scale 3
        r     (unsafe (free-run (fn [inner] (* inner scale)) (free-lift 10)))]
    (println r))
  0)
```

```
__handle_body_3: error: 'scale_888' undeclared (first use in this function)
    __t5->scale = scale_888;
```

It is **not** specific to `^fat`, to `free`, or to inline-C -- any capturing
`fn` literal *built inside* an `unsafe` block reproduces it. Building the
closure **outside** the `unsafe` block and only calling the unsafe function
inside is the current workaround:

```turmeric
(let [scale  3
      interp (fn [inner] (* inner scale))   ; closure built in normal scope
      r      (unsafe (free-run interp (free-lift 10)))]  ; only the call is unsafe
  ...)
```

`tests/fixtures/free-lift-bind` uses exactly this shape to stay green.

**Fix direction (out of scope here).** When lifting an `unsafe` body into a
`__handle_body_N` function, the captured free variables of any closure
constructed inside it must be threaded into the handler-body frame (the same
way they are threaded for closures built in a normal function body). A
dedicated codegen fix + a positive fixture (capturing closure built inside
`unsafe`, asserting the correct value) should retire the `free-lift-bind`
workaround.

## Out of scope

- Performance work on the closure representation itself.
- Macro hygiene rework.
- Cleaning up fixtures whose workarounds are *intentional* tests of
  the workaround path (e.g. `vec-of-vec-eq-manual` -- the manual
  variant should stay alongside the post-F3 idiomatic one as a
  regression anchor).
