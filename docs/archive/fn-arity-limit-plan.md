# Plan: Address the 8-parameter limit on `defn` / `fn`

> **Status:** All phases complete (AR4 punted by design)
> **Last Updated:** 2026-05-27
> **Type:** Compiler / Language
> **Depends on:** [Haskell-style currying (CY0–CY4)](upcoming/currying-plan.md)
> -- this plan assumes CY1+ has landed (partial application at
> under-saturated call sites). The phase priorities and Phase 2's
> interaction rules are written against that assumption.

---

## Overview

`MAX_FN_ARITY = 8` in `src/compiler/types.h:333` is the hard cap on
positional parameters for `defn`, anonymous `fn`, captures-aware `fn`,
and `extern-c`. Hitting it during spice development is common enough
to be annoying: writing the `tur-frame` reshape / pivot / join / sort
helpers ran into it five distinct times, forcing the same workaround
(pack two related args into one, derive a value from a different
arg inside the body, or split a recursion into two functions).

The limit isn't load-bearing on any deep correctness story -- it sets
the fixed-array bound for arg metadata on the `Type` struct
(`arg_kinds`, `arg_linear`, `arg_unique`, `arg_unique_mut`,
`arg_affine`, `arg_relevant`) and for a handful of elaboration scratch
arrays in `elab_fns.c`. Bumping it is a one-#define change; the more
interesting questions are:

1. **What does the new limit buy us?** Raising 8 to 16 papers over
   the immediate pain but doesn't change the language.
2. **What does Clojure do?** -- variadic `& rest` args, multi-arity
   dispatch, and map destructuring. None of these are available in
   Turmeric today.
3. **What's the idiomatic Turmeric answer to "this function has lots
   of state"?** -- arguably `defstruct` (which already exists) used
   as an options record, but the language doesn't currently make
   that pattern feel first-class.

The plan covers all three: raise the hard cap as a stopgap, add
variadic rest parameters as the principal long-term fix, and
formalize a `defstruct`-options idiom in style guidance + a small
syntactic affordance.

### How currying changes this plan

The original draft treated the 8-arg cap as the primary pain point
and the three phases as a roughly equal-priority cluster. Currying
(see [currying-plan.md](upcoming/currying-plan.md)) shifts the
trajectory:

- **The cap is still strictly needed to bump** -- currying is a
  call-site transform; it can't reduce the *declared* arity of a
  function that genuinely takes 9+ independent inputs. Phase 1 stays
  exactly as cheap and exactly as needed.
- **Operational pressure on the cap drops.** Most of the 9-arg
  helpers in `tur-frame` were recursive accumulators threading a few
  "context" args plus 1-2 "changing" args. With currying landed, the
  idiomatic shape becomes `(fn [state...] (fn [i acc] (...recur...)))`
  -- closure-capture for context, fixed-arity recursion for the
  changing args. The 8 (or 16) cap feels less crowded.
- **Variadic `&` (Phase 2) drops in priority** from "needed for
  daily ergonomics" to "convenience + variadic-interface support
  (`println`, `format`)." It also gains an interaction story with
  currying -- functions with `&` opt out of partial-application
  under-saturation, since their declared arity is unbounded. See
  the [Variadic + currying interaction](#variadic--currying-interaction)
  subsection.
- **Options-struct ergonomics (Phase 3) get a priority bump.** With
  currying, options-struct + partial-application composes cleanly:
  `(def read-csv-fast (read-csv default-opts))` and then
  `(read-csv-fast "x.csv")`. This is the Haskell idiom for default
  values and it makes options-struct the *right* answer to
  many-named-args rather than just *an* answer.
- **Currying's Open Question #4** ("should `defn` auto-curry?") must
  be resolved before Phase 2 lands -- auto-currying changes what
  "declared arity" means, which ripples into the variadic rules. The
  Phase 2 implementation tasks call this out as a blocking
  prerequisite.

---

## Where the limit bites today

Sites in the codebase that ran into the 8-param ceiling during
turmeric-spices/tur-frame development (each had to be restructured
to fit):

| File | Function | Original arity | Workaround |
|------|----------|---------------|------------|
| `spices/frame/src/frame/select.tur` | `__with-col-replace` | 9 | dropped `ncols` (unused in body) |
| `spices/frame/src/frame/select.tur` | `__with-col-append` | 9 | dropped `ncols` |
| `spices/frame/src/frame/group.tur` | `__g-build-agg-cols` | 9 | dropped `f` and `n-aggs` |
| `spices/frame/src/frame/join.tur` | `__j-assemble` | 9 | derived `lsch`/`rsch` from `l`/`r` inside body |
| `spices/frame/src/frame/reshape.tur` (pre-removal) | `__pv-build-tables` | 9 | inlined `n-keys` derivation |

In each case the function was a recursive accumulator threading a few
pieces of "context" plus 1-2 changing args. The workaround was always
"compute one arg from another inside the body" -- a code-smell
ergonomics tax, not a correctness gap.

A second pattern (less common but more painful) is multi-builder
functions in builders / codegen. `__ds-fill` in `frame/print.tur`
already takes 8 args and would naturally want 9 (carrying all of
`b-var b-count b-mean b-std b-min b-q25 b-med b-q75 b-max`); it was
split into three passes (`__ds-fill`, `__ds-fill-2`, `__ds-fill-max`)
purely to stay under the cap.

---

## What Clojure does

Clojure's three relevant mechanisms, in rough order of "how often
they're the right answer":

### 1. Variadic rest parameters (`& rest`)

```clojure
(defn foo [a b & more]
  ;; `more` is a seq of the remaining args; may be nil if none supplied
  (apply + a b more))

(foo 1 2)         ;; more = nil
(foo 1 2 3 4 5)   ;; more = (3 4 5)
```

At the bytecode level Clojure emits a Java method with `n` positional
params + one `ISeq` param. Calls with `> n` args bundle the tail into
a `PersistentList` at the call site (the compiler generates the
boxing); calls with `<= n` args pass `null` for the rest slot.

Practical limit on positional params: JVM allows 255; Clojure imposes
none on top of that. In practice 4-5 positional + variadic covers
99% of cases.

### 2. Multi-arity dispatch

```clojure
(defn foo
  ([] (foo 0 0))
  ([a] (foo a 0))
  ([a b] (+ a b)))
```

One name, several arities. The compiler emits separate JVM methods;
the call site dispatches on argc.

Useful for default-argument patterns (`(foo)` falls through to
`(foo 0 0)`) and for arity-specific fast paths (variadic + fixed-arity
specializations).

### 3. Map destructuring (`{:keys [...]}`)

```clojure
(defn foo [{:keys [delim quote has-header infer-rows null-str]
            :or {delim \, quote \" has-header true infer-rows 100}}]
  ...)

(foo {:delim \;})
```

The "many args" problem becomes a "single map" problem; named args
+ defaults + destructuring make the call site readable even when
there are 20+ logical inputs.

This is the **idiomatic Clojure answer to "I want named optional
args."** The `& rest` form is for "I take an unknown number of
positional values."

### What Clojure deliberately doesn't have

- **Default values in `defn` arglists.** You write a multi-arity
  defn instead (or use map destructuring `:or`).
- **Keyword arguments at the call site** (no `(foo :delim \;)`).
  You pass a map literal.
- **Hard limit on parameter count** other than the JVM's 255.

---

## Options

### Option A: Just raise the hard cap

Bump `MAX_FN_ARITY` from 8 to (say) 32 or 64.

**Memory cost:** Each `Type` instance carrying function-arg metadata
grows linearly:
- `arg_kinds[N]` -- N bytes (enum is 1 byte)
- `arg_linear[N]` -- N bytes
- `arg_unique[N]`, `arg_unique_mut[N]`, `arg_affine[N]`, `arg_relevant[N]` -- 4 * N bytes
- Total per `Type`: ~6N bytes on top of the existing struct.
- 8 -> 32: per-Type adds ~144 bytes. The compiler holds maybe 10k
  Type nodes at peak for a non-trivial project; bloat ~1.5 MB. Fine.

**Code changes:** One `#define`, plus any explicit `[MAX_FN_ARITY]`
stack arrays in `elab_fns.c` that get bigger.

**Pros:** Trivial. Unblocks every documented pain site immediately.

**Cons:** Doesn't add expressiveness. A 17-arg defn is still
illegible; the language just lets you write it.

### Option B: Variadic rest parameters

```turmeric
(defn foo [a : int b : int & rest : int] : int
  ;; rest is a cons-list of int values
  ...)
```

The body sees `rest` as a freshly-allocated cons list (same shape as
the cons cells the spice ecosystem already uses). At the call site
the compiler emits inline cons-building code for the tail args.

Type signature: the type of `rest` is some `list<T>` where `T` is
the declared rest type (`:int`, `:cstr`, etc.). A homogeneous rest
list is simpler than Clojure's heterogeneous `ISeq` and matches
Turmeric's typed-cons-cell idiom.

**Pros:**
- Matches Clojure's most-used mechanism.
- Solves the "many similar args" pattern (e.g., `(println a b c d e f g h i j)`)
  cleanly.
- Cons-list machinery already exists; no new runtime type needed.
- Cost is one cons-cell allocation per call -- acceptable.

**Cons:**
- Doesn't help the "many *dissimilar* args" pattern that's the actual
  pain point in our case (the `frame/select` and `frame/group`
  helpers have mixed-type, mixed-purpose args).
- Heterogeneous rest (different types per slot) would need a tagged
  union type Turmeric doesn't have.

### Option C: Multi-arity defn

```turmeric
(defn foo
  ([a :int] :int             (foo a 0))
  ([a :int b :int] :int      (+ a b))
  ([a :int b :int & rest :int] :int (...)))
```

Dispatch on argc at the call site (compiler-resolved when the call
arity is statically known, which is the common case).

**Pros:**
- Clean default-value pattern.
- No runtime cost when arity is known statically.

**Cons:**
- Doesn't help arity > 8 unless paired with Option B.
- Adds syntactic complexity to `defn` parsing.

### Option D: `defstruct`-options idiom + struct destructuring

`defstruct` exists already (see `stdlib/concurrent.tur`,
`stdlib/io.tur`, etc.). Lean into it for "many-args" cases:

```turmeric
(defstruct CsvOpts
  [delim       : int
   quote       : int
   has-header  : int
   infer-rows  : int
   null-str    : cstr])

(defn read-csv-string [src : cstr opts : CsvOpts] : int
  (let [d (CsvOpts.delim opts)
        q (CsvOpts.quote opts)
        ...]))
```

Optionally add struct destructuring in arg lists (Clojure-style):

```turmeric
(defn read-csv-string [src : cstr
                       {:keys [delim quote has-header] :as opts} :CsvOpts] : int
  ...)
```

Or call-site keyword args that elaborate into a struct literal:

```turmeric
(read-csv-string "x.csv" :delim 44 :quote 34 :has-header 1)
```

**Pros:**
- Best for "many *named* args" -- which is most of our pain.
- Pairs cleanly with the existing `defstruct`.
- Documentable as a style guideline.

**Cons:**
- Real syntactic work for the call-site keyword form.
- Doesn't help "many *positional* args" -- but those are rarer and
  usually a code smell.

### Option E: Status quo + style guidance

Document that >8 params is a smell. The actual fix is to refactor:
- Pack context into a struct passed once.
- Pre-compute derived values inside the body instead of as args.
- Split a recursion accumulator into a wrapper + helper.

**Pros:**
- Zero code changes.
- Forces better code structure.

**Cons:**
- Doesn't help when the function genuinely needs many independent
  inputs.
- The frame-spice helpers were already structured this way; the cap
  still bit them.

---

## Recommendation

Assuming currying (CY1+) has already landed, the three phases retain
their same shape but the priorities reorder:

**Phase 1 (Stopgap, strictly required):** Raise `MAX_FN_ARITY` from
8 to **16**. One #define, one rebuild. Currying does not reduce the
*declared* arity of a function, so this stopgap is necessary
regardless. Documented as "the cap that matches typical compiler
scratch-array bounds; if you need more, you're probably writing
something that wants Phase 3."

**Phase 3 (Options-struct ergonomics, now promoted ahead of Phase 2):**
Document `defstruct` + partial-application as the idiomatic answer
to "many named args with defaults." With currying landed,
`(def read-csv-fast (read-csv default-opts))` is the natural
default-args idiom and pairs cleanly with `defstruct`-options. Style
guide note + optional call-site keyword sugar if adoption justifies
it.

**Phase 2 (Variadic rest, lower priority post-currying):** Implement
`&` rest params with a typed cons-list rest. Now framed as "support
genuine variadic interfaces (`println`, `format`)" rather than as a
9-arg escape hatch. Includes a worked-out interaction with currying
(see the [next subsection](#variadic--currying-interaction)) and
depends on resolving currying's Open Question #4 (auto-currying)
first.

**Skipped (for now):** Multi-arity `defn` (Option C) and struct
destructuring in arg lists. Both are nice-to-haves; punt until a
concrete use case asks for them. Multi-arity has the same
auto-currying interaction concern as Phase 2.

### Variadic + currying interaction

Once currying is in the language, "call site has fewer args than
arity" already means something (partial application). The variadic
`&` semantics must be defined relative to that. Proposed rule:

| Function shape | Call has N args | Behaviour |
|---|---|---|
| `(defn f [a b c] ...)` (no `&`), arity 3 | N < 3 | partial application (currying) |
| `(defn f [a b c] ...)` (no `&`), arity 3 | N = 3 | normal call |
| `(defn f [a b c] ...)` (no `&`), arity 3 | N > 3 | over-application (currying chain) |
| `(defn f [a b & rest] ...)`, required = 2 | N < 2 | partial application up to the `&` boundary |
| `(defn f [a b & rest] ...)`, required = 2 | N >= 2 | variadic dispatch; rest collects |

In other words: the `&` boundary is the "currying limit" for
variadic functions. You can under-saturate up to the required args
(`(f a)` returns a closure waiting for at least `b`), but you can't
under-saturate into the rest. This matches the natural reading and
avoids the question "is partial application taking a variadic arg
to produce a smaller variadic?"

The remaining decision is whether `defn` auto-currying (currying
plan's Open Q #4) should apply to variadic functions. Proposed
answer: **no**. Auto-curried `defn` produces a curried-by-default
entry point only for functions whose arity is statically known and
fixed. Variadic functions are called by passing all positional args;
they don't get an auto-curried entry point. Users who want curried
behaviour wrap them manually.

---

## Implementation phases

Order: Phase 1 -> Phase 3 -> Phase 2 (the recommendation section
above explains the reorder; tl;dr currying makes Phase 3 the bigger
ergonomic win and Phase 2 the lower-priority "real variadic support"
work).

### Phase 1 -- raise the cap (1 hour, no currying dependency)

- [x] **AR0** -- Change `MAX_FN_ARITY` from `8` to `16` in
  `src/compiler/types.h:333`. Rebuild. Run the full test suite plus
  `turmeric-spices/spices/frame/tests/frame/`. Confirm no fixture
  regressions.
- [x] **AR1** -- Audit the error-message sites
  (`elab_fns.c:553`, `:1324`, `:1537`, `:1691`, `elab_types.c:759`)
  to make sure they still print `MAX_FN_ARITY` correctly (they all
  use `%d` already, but verify).
- [x] **AR2** -- Revisited the five named pain sites in `tur-frame`
  plus the `__ds-fill` triple-split. Findings:
  - `__with-col-replace`, `__with-col-append`, `__g-build-agg-cols`:
    the args dropped under the 8-cap (`ncols`, `f`, `n-aggs`) were
    genuinely unused inside the recursion (terminator uses `i < 0`).
    Re-adding them is pure ceremony with no readability win. *No change.*
  - `__j-assemble`: deriving `lsch`/`rsch` from `l`/`r` is a clean
    two-line `let`; not worth churning. *No change.*
  - `__pv-build-tables`: function no longer exists (file refactored).
  - `__ds-fill` in `print.tur`: **the real win.** Three near-identical
    recursive passes (`__ds-fill`, `__ds-fill-2`, `__ds-fill-max`)
    each calling `__ds-stats` per numeric column -- redundant work
    purely to stay under 8 args. Unified into a single 13-arg
    `__ds-fill`; one pass, one `__ds-stats` call per column.
    Frame `reshape_test` (incl. the `describe` case) still passes.

Stopping point if scope creeps: this phase alone resolves every
documented pain case in the repo, regardless of whether currying or
Phase 2/3 ever lands.

### Phase 3 -- options-struct ergonomics (~3 days, depends on CY1+)

- [x] **AR3** -- Style guide section in `CLAUDE.md` and
  `docs/style-guide.md` documenting:
  - >5 params is a smell; reach for `defstruct`.
  - "Options struct" idiom: pass a single struct holding the named
    args, with a `default-foo-opts` constructor for defaults.
  - **Currying-friendly default pattern**:
    `(def read-csv-fast (read-csv default-opts))` -- partial
    application locks in the defaults, the resulting function takes
    only the variable args.
  - Quick decision guide for choosing between options struct and `& rest`.
- [~] **AR4** -- *Punted by design.* Call-site keyword sugar
  `(read-csv :delim 44 :has-header 1)` elaborating into
  `(read-csv (CsvOpts :delim 44 :has-header 1 :quote 34 ...))` was
  always gated on adoption. No usage has yet justified the parser
  work; the AR3 style-guide pattern (`(def read-csv-fast (read-csv
  default-opts))`) covers the common case via currying. Revisit if a
  concrete options-heavy API calls for it.

### Phase 2 -- variadic rest params (~1 week, depends on CY1+ and CY-Q4)

**Prerequisite:** Currying-plan Open Question #4 ("should `defn`
auto-curry?") must be resolved before this phase starts. The
variadic-vs-curry interaction rules in the
[Variadic + currying interaction](#variadic--currying-interaction)
subsection assume the answer is "no auto-currying for variadic
functions"; if the answer turns out to be "yes auto-curry
everything," some of those rules need a rewrite.

- [x] **AR5** -- Parser: accept `&` in `defn` / `fn` arg lists.
  Grammar: `(defn name [param :type ... & rest :type] :ret body)`.
  Reject more than one `&`. Type annotation on rest is required.
- [x] **AR6** -- Type system: extend the function type with
  `bool is_variadic` + `TypeKind rest_kind` in `Type.as.fn`.
  Variadic functions match call sites with `>= n_required` args.
- [x] **AR7** -- Elaborator: variadic functions opt out of
  partial-application synthesis when the call has `>= n_required`
  args; under-saturated calls still go to partial-application.
- [x] **AR8** -- Call-site codegen: when the callee is variadic and
  the call has `n_required + k` args, build a right-folded cons list
  (`EX_CONS_LIST` expression kind) from the surplus args and pass
  it as the final argument. Rest args are type-checked against
  the declared rest element kind. `g_has_variadics` gates the
  `__tur_cons_of` helper in the C preamble (only emitted when needed).
- [x] **AR9** -- Inline-C interop: emit a clear compile error when a
  variadic body contains inline-C directly.
- [x] **AR10** -- Tests:
  - `tests/fixtures/variadic-defn-basic/` -- zero rest args (nil) and
    multiple rest args; verifies cons-list shape.
  - `tests/fixtures/errors/variadic-typed-rest/` -- compile error on
    type-mismatched rest arg.
  - `tests/fixtures/errors/variadic-inline-c-error/` -- compile error
    on inline-C in a variadic body.
  - `tests/fixtures/variadic-passthrough/` -- variadic that forwards
    the rest list to a helper for inspection.
  - `tests/fixtures/variadic-tail-recursion/` -- tail-recursive walk
    of the rest list.
  - `tests/fixtures/variadic-arity-16/` -- 15 fixed params + `& rest`
    (total 16 = MAX_FN_ARITY); confirms raised cap and variadic work
    together.
  - *Deferred*: variadic + `(apply ...)` (apply primitive not yet
    implemented); variadic + currying under-saturation boundary test.

---

## CLI / language surface changes (after Phase 2)

```turmeric
;; --- variadic defn ---

(defn println-all [first : cstr & rest : cstr] : void
  (println first)
  (for [s rest]
    (println s)))

(println-all "hello")                ;; rest = nil
(println-all "a" "b" "c")            ;; rest = (cons "b" (cons "c" 0))

;; --- variadic with arity > 8 ---

(defn long-print [a : cstr b : cstr c : cstr d : cstr e : cstr f : cstr
                  g : cstr h : cstr & rest : cstr] : void
  ...)

(long-print "1" "2" "3" "4" "5" "6" "7" "8" "9" "10")
```

Error messages:

```
error: defn 'foo': too many fixed parameters (max 16); use & to take a rest list
error: defn 'foo': variadic body contains inline-C; inline-C blocks need fixed arity
error: defn 'foo': multiple `&` in parameter list
error: defn 'foo': `&` must be followed by exactly one rest param
```

---

## Tests

Phase 1 only needs the existing test suite to keep passing after the
#define bump. Phase 2 adds:

- `tests/fixtures/variadic-defn-basic/` -- minimal variadic with 1
  fixed + 1 rest; calls with 0, 1, 5 rest args; assert each cons-list
  shape.
- `tests/fixtures/variadic-passthrough/` -- variadic that forwards to
  another variadic via cons-list manipulation (no `apply` yet).
- `tests/fixtures/variadic-typed-rest/` -- compile error on
  type-mismatched rest arg.
- `tests/fixtures/variadic-inline-c-error/` -- compile error on
  inline-C in a variadic body.
- `tests/fixtures/variadic-tail-recursion/` -- tail-recursive walk
  of the rest list (the natural recursion pattern).
- `tests/fixtures/variadic-arity-16/` -- 16 fixed params + `&` rest;
  confirms the raised cap and variadic work together.

---

## Risks and open questions

1. **Cons-list garbage.** Every variadic call allocates a cons cell
   per surplus arg. For hot-path code (sort comparators, hash-join
   per-row callbacks) this is unacceptable overhead. v1 mitigation:
   document that variadics are for "occasional convenience calls,"
   not inner loops. v2: stack-allocate the cons list when escape
   analysis proves it doesn't outlive the call.

2. **Inline-C compatibility.** Inline-C blocks declare fixed C
   signatures; there's no clean way to bridge variadic Turmeric to
   variadic C. Most spice authors will want non-variadic defns for
   anything that calls into inline-C. Phase 2 documents this and
   emits a clear error if you mix them.

3. **Refactor pressure on existing APIs.** `agg`, `arrange`, `join`,
   etc. currently take parallel cons lists (a workaround for the
   lack of "list of pairs" support). With variadic + `defstruct`
   options the natural shape becomes
   `(agg g :out-names ... :in-names ... :tags ...)`. We should NOT
   churn the v0.1.0 APIs until the language features land and we
   have a real ergonomics improvement to point at.

4. **Backward compat.** Bumping `MAX_FN_ARITY` is a purely additive
   change -- existing code keeps working. Adding `&` to the parser
   needs to handle the case where `&` already means something else
   in user code (currently nothing in `stdlib/`, but worth a
   grep). If `&` is already used, fall back to `&rest` or
   `:rest`.

5. **`apply` primitive.** Most variadic ergonomics in Lisp/Clojure
   come from `apply`. Without it, variadic-to-variadic forwarding
   requires manual cons manipulation. Phase 2 should ship a
   simultaneous `(apply fn arg1 arg2 ... rest-list)` primitive so
   the variadic story feels complete.

6. **Compiler memory bloat at high caps.** Going to 32 or 64 is
   fine; going to 256 starts to show in `Type` struct size. The
   recommendation caps at 16 to keep the Type struct under a cache
   line of arg metadata. If a real case needs > 16, the answer is
   "use a struct or a rest param" -- not "raise the cap further."

7. **Multi-arity dispatch (Option C) interaction.** If we add
   multi-arity later, it needs to interact predictably with
   variadic. Clojure's rule: at most one arity can be variadic, and
   that arity must be the largest. Adopt the same rule; document
   in the multi-arity plan if it ever lands.

8. **Currying's auto-curry decision (CY Open Q #4) blocks Phase 2.**
   If `defn` ends up auto-currying, the variadic interaction rules
   in this plan need a rewrite -- "declared arity" becomes the
   curried entry point's arity, not the worker's. Worst case, Phase
   2's grammar needs an explicit `^no-curry` annotation on variadic
   functions. Resolve before AR5.

9. **Currying's effect-row + rank-2 propagation (CY4) is in flight
   when this plan starts.** Phase 2's variadic rest type needs to
   participate in effect-row inference (a variadic `IO`-effecting
   function's rest list still carries `IO`). Co-design with CY4.

---

## Future work

- **Multi-arity `defn`** (Option C). With currying landed, this is
  partly redundant for the default-value case (`(def foo-with-default
  (foo default-val))` covers most of it). Keep on the future-work
  list for arity-specific fast paths; should land after Phase 2 so
  the variadic + currying + multi-arity three-way interaction is
  worked out.

- **Struct destructuring in arg lists.**
  `(defn read-csv [src :cstr {:keys [delim quote] :as opts}] ...)`.
  Pairs with Phase 3 to make options-struct usage feel native.

- **Call-site keyword args.**
  `(foo :delim 44 :quote 34)` elaborating into a struct literal.

- **`apply` primitive.** Required for variadic forwarding; should
  ship alongside Phase 2.

- **Tail-call-friendly variadic recursion.** The natural recursion
  shape walks the rest list; make sure the existing TCO machinery
  handles it without growing the cons-list head pointer in a
  register-pressure-bad way.

- **Stack-allocate cons cells for short-lived rest lists.** Escape
  analysis-driven optimization once variadic call counts are
  measurable.
