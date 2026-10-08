# Saffron -- a dynamically typed `#lang` over the Turmeric runtime

Status: **complete** -- every stage, S0 through S9, landed (S0-S5
2026-09-07, S6 2026-09-08, S7-S9 2026-09-09), and `#lang saffron` GRADUATED
at 0.46.0 (2026-09-10). It is an ordinary base dialect on the same footing as
`#lang turmeric`: there is no `EXPERIMENTS[]` row, no enable and no lifecycle
warning (the graduation note in `src/runtime/experiments.c` records why
`g_opt_dynamic_any` outlived the row). About ninety fixtures with `saffron`
in the name pin the dialect on both back ends, plus twelve under `errors/`.
[saffron-guide.md](../guides/saffron-guide.md) and
[introducing-saffron.md](../guides/introducing-saffron.md) are the
user-facing halves. Archived 2026-09-28.

What kept this file in `docs/upcoming/` until then was the dynamic-dispatch
residue S9 left -- see [S9's remaining limits](#s9----what-is-still-open). The
plan's own rule was to archive once those landed or moved to reports of their
own; on 2026-09-28 they moved. (S9's last design item, the constrained
instance at `A = any` -- D8 Q1 -- turned out to be a silent wrong answer
rather than a limit, and was built 2026-09-26. The same day, binary-fn methods
and the class-declared result type landed too, including a result that
mentions the class variable applied --
[saffron-applied-class-var-result-takes-one-instances-type](../archive/saffron-applied-class-var-result-takes-one-instances-type.md).)
Re-measured 2026-09-28 against `./build/tur`, both remaining limits still
stand -- compiled-only clean panics at the witness's checked cast, while
`--interpret` answers -- and each is now a report:

- [saffron-dyn-witness-fn-arity-defaults-unary](../reported/saffron-dyn-witness-fn-arity-defaults-unary.md)
  (low): the fn-arity default. A class parameter spelled `g : fn` (no arity)
  is cast to `(fn [any] any)`, so a two-argument lambda panics. A FULLY
  unannotated `g` never reaches the witness: a class method records it as
  `int` in both dialects, and the instance body's `(g ..)` is already "'g' is
  not a function or continuation".
- [saffron-dyn-parametric-extra-read-as-class-var](saffron-dyn-parametric-extra-read-as-class-var.md)
  (low-medium): an `int` extra on a parametric head is read as the class
  variable and cast to `(Head any..)`. Not only an unannotated one, as S9's
  list says -- a spelled `n : int` panics the same way.

Deferred by decision, not residue: open questions 2 (a `.saf` extension) and
4 (whether `tur repl` defaults to Saffron -- "revisit after usage") are
product decisions. The interpreted `Eq [cstr]` noted under S9 (`(eq? "x"
"y")` interprets to `1`, in any dialect) is the interpreter's TI7 inline-C
carve-out ([turi-parity-post-v1-plan](../archive/turi-parity-post-v1-plan.md)),
not Saffron's.

Other open reports a Saffron program reaches (none was a stage blocker):

- [saffron-static-ownership-residue](../reported/saffron-static-ownership-residue.md)
  (low, by design): what the static drops cannot own, which only a build with
  the collector OFF leaks.  **2026-09-28:** a compiled single-unit Saffron
  program allocates from the r7rs-gc collector, which closed
  [any-widen-stored-in-an-adt-field-has-no-owner](../archive/any-widen-stored-in-an-adt-field-has-no-owner.md),
  [dynamic-returned-closure-env-is-never-freed](../archive/dynamic-returned-closure-env-is-never-freed.md) and
  [cps-capturing-closure-env-leaks-through-dyn-call](../archive/cps-capturing-closure-env-leaks-through-dyn-call.md);
  [byvalue-recursive-adt-boxes-are-never-freed](../archive/byvalue-recursive-adt-boxes-are-never-freed.md)
  closed the same day on its own.  The two closure-env reports also gained
  static drops that day, so a returned lambda a `let` owns and a lambda
  passed to a non-retaining parameter are freed with the collector off too.
- [jit-x86-64-struct-valued-statement-expression-miscompiles](jit-x86-64-struct-valued-statement-expression-miscompiles.md)
  (medium, JIT engine on x86-64 only): resolved 2026-09-29 -- the c2mir
  fix merged as rjungemann/mir#5 and the MIR pin moved to it.
- [static-instance-spec-calls-any-lambda-as-concrete-result](static-instance-spec-calls-any-lambda-as-concrete-result.md)
  (high): a `.foldl` that resolves STATICALLY on a Saffron value calls the
  `any`-returning lambda as if it returned `double`. Still reproduces
  2026-09-28 (compiled prints `2.25` / `4.6e-310` for `1.5` / `9.75`).

The rest of this header is the running log of the build-out. It is kept as
written, with dated corrections where a claim has since changed.

**S0 is complete as of 2026-09-07** -- all twelve prerequisites are fixed and
archived: **P1** (cross-TU `any` ids), **P2** (by-value rank-2 receiver,
guarded), **P2b** (`@TypeName` implies the unbox), **P2c** (parametric
narrowing), **P2d** (monomorph under a widen), **P3** (unannotated return
inference), **P4** (`type-of` on a boxed closure), **P5** (the guide's `any`
examples), **P6** (`: any` drives coercion at `let`/`def`/`if`/letrec), **P7**
(native ADT constructors lose the ADT name), **P8** (float truncated through the
dict carrier), and **P9** (an `any`-boxed fn matched every fn type).

Four more `any`-surface defects were found *by* that burn-down and also fixed:
a union widened to `any` emitted uncompilable C; a partial application widened
as a `ptr<void>`; a collection handle inside an `any` reported as `int` under
`--interpret`, with `is?` wrong in both directions; and a capturing closure
could not be recovered from an `any` at all. The `any` surface now agrees
across both back ends for structs, ADTs, applied constructors, collections,
opaques, unions, and every function shape -- which is the substrate S2-S4 stand
on, so this is more than S0 asked for.

Three residuals were left open, none of them S0 blockers (all three are fixed
and archived since):
[a shim-box leak when a fn is widened through a local binding](../archive/any-fn-widen-through-local-binding-leaks.md)
(low-medium), [a segfault in the interpreter's `any` reflection for an
inline-C-produced opaque](../archive/interp-inline-c-opaque-segv-in-any-reflection.md)
(high, but inside the existing TI7 inline-C carve-out -- worth closing before
S3/S4 lean on the interpreter hard), and
[a spurious `-Wfree-nonheap-object` in emitted code](../archive/any-drop-inlining-warns-free-nonheap.md)
(cosmetic).

**S1-S5 landed 2026-09-07** -- the `#lang` language axis with its gate and
listing, the `any` default for unannotated parameters, the dynamic operator
layer (arithmetic, comparison, print, truthiness), dynamic call plus dynamic
field access, and the compiled back end for all three. Saffron runs higher-order
code over a heterogeneous list -- `map`, `filter` and `fold` over
`(1 "hi" 7.1 true)` -- and now runs it **compiled**, printing the same eight
lines as the interpreter. Every S3/S4 fixture lost its `requires.interp-only`
marker and is asserted on both back ends.

S5's predicted risk -- **ownership** -- was real, was filed, and is now
[fixed](../archive/saffron-any-return-defeats-the-frame-box-rule.md). Widening
a by-value payload into an `any` parameter no longer allocates at all: the
frame-box rule fires again, so the widen emits a caller-frame copy. It needed
two changes, and the first was one the report itself had missed -- the dynamic
nodes were invisible to `expr_subtree_has_inline_c`, which switched the whole
non-retain inference off for every Saffron body before its result gate was even
consulted. **RC-managed `any` boxes, which this plan proposed as the mitigation,
were not needed** and should not be revived on this evidence.

One leak remains under the gate, and identifying it correctly took measuring the
emitted C rather than reading the source: `saffron-higher-order`'s container is
`(Cons [hd : any tl : any])`, so its 21 allocations are all the **`any` widen
into a field** and none of them the recursive-carrier box it was first attributed
to. Filed as
[any-widen-stored-in-an-adt-field-has-no-owner](../archive/any-widen-stored-in-an-adt-field-has-no-owner.md),
and it is a **prerequisite for S6**: a container of `any` is exactly what that
stage is about, so the element box needs an owner before it lands.
(**Corrected 2026-09-26:** it did not block S6, which landed 2026-09-08
around it. The report is still open, narrowed to one shape -- a recursive
function handing a match binder to an opaque call -- and
`saffron-higher-order` measures 680 bytes in 17 allocations, down from 21.)
(**Closed 2026-09-28** by the collector: a compiled Saffron program allocates
from the r7rs-gc heap, so the boxes no static owner reaches are reclaimed;
the no-collector residue is `saffron-static-ownership-residue`.)

The sibling finding that measurement separated out --
[a self-recursive by-value ADT mallocs one box per link](../archive/byvalue-recursive-adt-boxes-are-never-freed.md),
which reproduces in plain Turmeric with no `any` anywhere -- is now partially
fixed: a non-escaping local's spine is freed at scope exit. Its two residues (a
local handed to a callee, and `:copy` types, where `with-region` already
reclaims the spine) are recorded there.
**S6, containers and the Saffron prelude, is DONE (2026-09-08).** S7 (the
boundary) and S8 (tooling) followed on 2026-09-09, S9 (runtime typeclass
dispatch) was built the same day, and the dialect graduated 2026-09-10.

Worth stating plainly, because it changes how the rest of this plan should be
read: **six of those eight reports had a diagnosis that was wrong on
inspection**, always in the same direction -- control flow read and a cause
inferred, rather than measured. P2d and P3 were ordering bugs, not missing
machinery. P8's recommended direction was the larger one. P6 had four causes,
not the two filed. The "today" claims below were written the same way, so treat
them as leads to verify rather than findings, and instrument before acting on
any of them.

---

## 0. The ask, and the honest size of it

> Saffron is Turmeric without type annotations. Essentially a
> non-static-typed version of Turmeric.

This is several epics, and the request came in knowing that. The value is
real: the runtime underneath is far more capable than what a dynamic Lisp in
this weight class normally gets -- delimited continuations (`shift`/`reset`,
serial and cloneable), algebraic effects with deep and shallow handlers,
generators, STM, channels, a HAMT, a tree-walking interpreter with a debugger
and a DAP server, an incremental REPL, and a macro system with reader macros
and four surface syntaxes. Fennel gets Lua's runtime. Saffron would get this
one.

The load-bearing static machinery -- refinements, session types, linearity,
regions, monomorphization -- does not survive the move, and this plan says so
explicitly rather than pretending it can be retrofitted. See
[Section 6](#6-carve-outs).

---

## 1. Thesis

**Saffron is not a second language. It is Turmeric elaborated with `any` as the
default type instead of `int`, plus the dynamic operator layer that turns `any`
from a storage type into an operational one.**

Same reader. Same macro expander. Same `Expr` tree. Same two back ends. Same
runtime. Same stdlib where the stdlib is polymorphic enough to be reachable.
The delta is concentrated in two places:

1. **Elaboration defaults.** Four sites in `elab_fns.c` currently write
   `TY_INT` for an unannotated parameter or return. Under Saffron they write
   `TY_ANY`.
2. **The dynamic surface.** `any` today can be *stored*, *reflected on*
   (`type-of`, `is?`, `cast`) and *narrowed* in an `if` guard. It cannot be
   added, printed, called, matched, compared, indexed, or field-accessed. That
   list is the actual work.

Everything else -- the `#lang` plumbing, the prelude, the tooling -- is
mechanical by comparison.

Keeping this framing is what bounds the maintenance burden. The moment Saffron
becomes a fork of the elaborator, it stops being affordable.

---

## 2. What already ships (measured)

This section exists because the starting position is much better than it
looks, and the plan's staging depends on knowing exactly how much better.

### 2.1 `any` is a real type with a real runtime representation

`TY_ANY` is the top type (`types.c:5381` -- `if (super_.kind == TY_ANY) return
true;`). Its C representation is `tur_tagged_t { int64_t tag; int64_t val; }`
(`types.c:519`). A value widens into it through `elab_coerce_to_any`
(`elab_call.c:418`), which wraps it in `EX_UNION_INJECT` carrying a runtime
tag. Immediates ride the `val` word; floats ride as their IEEE-754 bit
pattern; by-value aggregates are heap-boxed and the box is freed by
`__tur_any_drop` (`emit_module.c:818`).

Struct and ADT payloads do **not** tag as a bare `TY_STRUCT` -- `emit_any_type_id`
(`emit_module.c:752`) interns the monomorph's `type_name` and hands out
`TUR_ANY_ID_BASE + index`, so `Point` and `Other` are distinguishable at
runtime and `type-of` reports the source-level name.

### 2.2 The reflection surface works

Probed and passing today:

```turmeric
(defn box-str [] : any "hi")
(defn describe [x : any] : cstr
  (if (is? x int) "int"
    (if (is? x cstr) "cstr" "other")))
(defn bump [x : any] : int
  (if (is? x int) (+ x 1) 0))     ;; if-guard narrowing: x is int inside
```

`type-of`, `cast` (checked, panics on mismatch), `is?`, and `if`-guard
narrowing all work on both back ends.

### 2.3 The interpreter is already a dynamic runtime

This is the single most important fact in the plan.

`TuriValue` (`turi/value.h:53`) is a tagged union -- exactly the representation
a dynamic language wants. `+` dispatches on that tag at runtime
(`eval.c:3567`: `bool is_float = (args[0].tag == TURI_FLOAT);`). `println`
dispatches on it too (`eval.c:3759`). And `EX_UNION_INJECT` -- the widen-to-any
node -- is a **no-op** in the interpreter:

```c
/* eval.c:10750 */
case EX_UNION_INJECT:
    return eval_expr(env, frame, e->as.union_inject_.value);
```

because `any` does not need a box when every value is already tagged.

So the interpreter does not need a dynamic runtime built for it. It *is* one.
The only thing standing between `#lang saffron` and a working interpreted
Saffron is the **shared elaborator**, which the parity guide is explicit about:
"The shared front-end (parser, macro expander, elaborator, substructural
checker) runs identically on both paths, so type errors ... are the same."

This is what makes an interpreter-first staging honest rather than a dodge.

### 2.4 Local inference already exists

`let` bindings infer. `(let [x 7.1 y (* x 2.0)] (println y))` prints `14.2`
today -- no annotation anywhere. So Saffron does not need an inference engine
built from nothing; it needs the existing one to stop being cut off at the
`defn` boundary.

There is even a precedent for inferring an unannotated *parameter*:
`elab_fns.c:9569` types an unannotated `fn` param from the expected function
type pushed by the call site, instead of defaulting to `int`. Saffron widens
that seam; it does not open a new one.

### 2.5 The control-flow runtime is type-agnostic

Delimited continuations, effect handlers (deep and shallow), generators, STM,
channels, dynamic variables, panics: none of them are typed machinery in any
way Saffron disturbs. The parity guide marks every one of them `OK`/`OK`. They
should come across for free, and the plan budgets nothing for them beyond
fixtures that prove it.

---

## 3. What does not ship (measured)

Every row below was reproduced against `v0.44.2`. Transcript in the appendix.
**Every row is closed** (2026-09-26 re-check): the table is kept as the
starting position the stages were measured against.

| # | Saffron needs | Today | Diagnostic |
|---|---|---|---|
| ~~G1~~ | unannotated param is dynamic | **FIXED by S2 (2026-09-07)** -- was: defaults to `int`; `(twice 7.1)` on `(defn twice [x] (* x 2))` is rejected | -- |
| ~~G2~~ | unannotated return is dynamic | **FIXED 2026-09-07** -- was inconsistent (`int`/`cstr` bodies OK, `7.1` a hard error claiming the fn "declares return type 'nil'"). Now inferred for every body type. Still `int`-shaped rather than dynamic, which is S2's job, but no longer wrong | -- |
| ~~G3~~ | `(+ x 1)` where `x : any` | **FIXED by S3/S5 (2026-09-07)** -- was: rejected, TUR-E0006 `operator lookup failed for '+', first arg type any` | -- |
| ~~G4~~ | `(println x)` where `x : any` | **FIXED by S3/S5 (2026-09-07)** -- was: rejected; the union guide's own headline example did not compile | -- |
| ~~G5~~ | `(f x)` where `f : any` | **FIXED by S4/S5 (2026-09-07)** -- was: rejected, `'f' is not a function or continuation` | -- |
| ~~G6~~ | `(match x)` where `x : any` | **FIXED by S4/S5 (2026-09-07)** -- a checked narrow to the ADT the arms name. Was: rejected at elaboration, `match: scrutinee must be an ADT type, got any` | -- |
| ~~G7~~ | heterogeneous containers | **FIXED by S6 (2026-09-08)** -- `[...]`, `#map{}`, `#set{}` and `(list ...)` widen each element to `any`. Was: `(vec-of 1 "two")` rejected with TUR-E0001 | -- |
| ~~G8~~ | `type-of` on a boxed closure | **FIXED 2026-09-07** -- both back ends answer `"fn"`. But the tag is the bare `TY_FN` kind, so it identifies functions as a CLASS and not by signature; see P9 | -- |
| ~~G9~~ | `=`/`<`/`>` on `any` | **FIXED by S3/S5 (2026-09-07)** -- was: rejected, TUR-E0006 | -- |
| ~~G10~~ | `any` truthiness in `if` | **FIXED by S3/S5 (2026-09-07)** -- D4's rule: `false` and `nil` are falsy, everything else truthy | -- |
| ~~G11~~ | field access on `any` | **FIXED by S4/S5 (2026-09-07)** -- was: no dynamic path | -- |

G3, G4, G5, G6, G9 were one problem wearing five hats: **the builtin operator
table was keyed on concrete argument kinds and had no `any` row.** That was the
core engineering work of this plan, and it was the same work on both back ends
-- except that on the interpreter the *implementation* already existed
(Section 2.3) and only the elaborator needed to stop rejecting the call. S3-S5
did it: the elaborator routes such a call to the `EX_DYN_*` nodes D4 lists.

G2 and G8 are defects independent of Saffron and were filed separately. **G2 is
fixed** ([inferred-return-defaults-inconsistently](../archive/inferred-return-defaults-inconsistently.md)
-- an ordering bug, not the missing inference the report guessed); G8 is
[type-of-on-boxed-closure-diverges](../archive/type-of-on-boxed-closure-diverges.md),
and G4's documentation half is
[any-type-guide-examples-do-not-compile](../archive/any-type-guide-examples-do-not-compile.md),
both fixed and archived.

Two further defects were found by the follow-up research into S0 and D8, and
both are prerequisites rather than side notes -- `any-type-ids-are-per-tu.md`
(the box tag means different things in different translation units) and
`forall-dict-byvalue-receiver-emits-uncompilable-c.md`. Neither is in the gap
table above because neither is a *missing* capability; they are existing
machinery that is wrong. See S0's P1-P5 table.

---

## 4. Design decisions

### D1 -- `#lang` gets a language axis, orthogonal to the reader axis

**Verdict: add `LangDialect` beside `ReaderType`. Do not grow `ReaderType`.**

`#lang <base>[/<dialect>] <layer>*` today resolves the whole first token to a
`ReaderType` (`reader.c:4532`, `lang_base_from_name`). That enum means "which
reader": `turmeric`, `turmeric/curly-infix`, `turmeric/neoteric`,
`turmeric/sweet`.

Saffron is not a reader change. Saffron files want *all four* readers --
`#lang saffron`, `#lang saffron/sweet`, and so on. Folding those into
`ReaderType` would double the enum, and every `switch (reader_type)` in the
tree would then have to remember that half its cases mean the same reader.

So:

```c
typedef enum LangDialect {
    LANG_TURMERIC = 0,   /* the default; every existing file */
    LANG_SAFFRON,
} LangDialect;
```

`lang_base_from_name` splits into a `(LangDialect, ReaderType)` pair.
`detect_lang_layered` gains a `LangDialect *out_lang`. `SourceFile`
(`diag.h:437`) grows a `lang` field beside `reader_type`. `detect_lang` keeps
its signature and discards the new out-param, exactly as it already discards
the layer set -- so no existing caller changes.

Layers stay orthogonal to both axes: `#lang saffron/sweet stringed` is legal
and means what it reads as. (**Superseded:** the layer axis was later
decommissioned. `#lang` now takes exactly one base dialect -- `saffron`,
`saffron/sweet`, and so on -- and a trailing token is TUR-E0330; per-file
syntax goes in a `#use-reader-macros` file and per-file gates in an
`EXPERIMENTS[]` row. The dialect table is `lang_dialects.c`.)

Extensions follow later, not first: `.saf` and `.saf.sweet` in
`reader_type_from_extension`'s sibling, once the semantics are real. Stage 1
is `#lang saffron` inside an ordinary `.tur` file, which keeps every tool
working while the semantics are in flux.

### D2 -- annotations stay legal in Saffron

**Verdict: Saffron is "annotations optional", never "annotations forbidden".**

This is the decision the rest of the design hangs off. It buys three things
that are otherwise very expensive:

- **FFI survives.** `extern-c` and inline-C need concrete C types. A Saffron
  file can annotate exactly the boundary defn and leave the rest dynamic.
- **Migration is bidirectional.** Adding an annotation to a Saffron file is a
  refinement, not a rewrite; deleting one from a Turmeric file is a
  loosening. The two dialects are the same language at different points on
  one dial.
- **Performance has an escape hatch.** A hot Saffron function can be
  annotated and get the static path's codegen with no dialect change.

The corollary is that CLAUDE.md's "No Lazy `:int` Stand-Ins" rule applies to
Saffron code that *does* annotate, unchanged. An unannotated Saffron parameter
is `any`, which is honest; an annotated one that says `:int` for a request
handle is the same defect it always was.

### D3 -- the default type is `any`, and inference still runs

**Verdict: unannotated means `any` as the *declared* type, but local inference
is still allowed to prove something narrower.**

The naive reading of "no static types" is "make everything `any` and box
everything". That would be correct and unbearably slow -- every integer add
would go through a tagged dispatch.

Instead: the *signature* is `any` (that is what makes the call site dynamic and
the language usable), but inside a body, a binding whose value is provably one
concrete type stays that type. `(let [x 7.1] (* x 2.0))` should remain a
`double` multiply in a Saffron file, exactly as it is today. The machinery for
this already exists -- it is what makes an unannotated `defn` return infer at
all (G2), and fixing G2 confirmed the inference block is already in `elab_defn`
rather than something Saffron would have to add.

This gives Saffron a clean optimisation ladder that does not need to be built
up front:

1. **S3 baseline** -- every `any` operation goes through dynamic dispatch.
2. **Later** -- local type propagation un-boxes within a body.
3. **Later still** -- a whole-program pass that observes a Saffron defn is
   only ever called at one argument type and specialises it.

Rung 1 is the plan. Rungs 2 and 3 are explicitly out of scope and named here
only so the design does not foreclose them.

### D4 -- the dynamic surface is eight operations

Saffron needs `any` to support exactly these, and this list should not grow
without a decision:

| Operation | Compiled lowering (S5, as built) | Interpreter |
|---|---|---|
| arithmetic (`+ - * / mod`) | `__tur_dyn_arith(op, tur_tagged_t, tur_tagged_t)`, left-folded for the variadic forms | `builtin_lookup` on the runtime tag, with numeric promotion |
| bit operators (`bit-and/or/xor/shl/shr`) | same helper, int-only like `mod` | same |
| comparison (`= not= < > <= >=`) | `__tur_dyn_cmp`, returning a BOXED bool (the node's type is `any`) | same |
| print (`println`) | `__tur_dyn_println`, each arm spelling the value the way the static emitter spells it | same |
| call (`(f x ...)`, `f : any`) | `TUR_APPLYn_T` through the fat protocol; the arity/signature check is one tag compare against the fn box id | closure values already first-class |
| truthiness (`if`, `when`, `and`, `or`) | `__tur_dyn_truthy`; `and`/`or` lower to STATEMENTS so an operand needing statements of its own stays lazy | tag test |
| `not` | `__tur_dyn_not` -- bool-only, NOT truthiness, matching what `builtin_lookup` does | `builtin_lookup` |
| field access (`(.f x)`) | if/else chain over the box id, one arm per program type with that field | `TuriStruct` field lookup |
| type dispatch (`match` on `any`) | checked NARROW to the ADT the arms name, then the ordinary match | tag switch |
| index (`(vec-get v i)` etc. on `any`) | **S6** -- outside the compiled set, reported by name | delegate |

**The float rule is load-bearing here.** `tur_tagged_t.val` is `int64_t` and a
float rides as its IEEE-754 bit pattern. Every dynamic arithmetic path must
branch on the tag and *reinterpret*, not convert. Per CLAUDE.md, every probe
in this area leads with `7.1`, never `7.0` and never `7` -- an integer literal
cannot show the truncation this would produce if the reinterpret is wrong.
`tests/fixtures/saffron-dyn-arith-float` exists to pin exactly that.

**Truthiness needs a decision, and this plan makes it:** in Saffron, `false`
and `nil` are falsy; **`0`, `""`, and the empty container are truthy.** That is
the Lisp/Clojure convention rather than the C/Lua one, it matches the fact that
Turmeric's `if` already wants a `bool` (so there is no legacy int-truthiness to
preserve), and it avoids the classic `(if (vec-len v) ...)` bug.

### D5 -- the Saffron/Turmeric boundary is a contract boundary

**Verdict: gradual typing with runtime checks at the seam, not erasure.**

A Turmeric module importing a Saffron module sees `any`-typed exports and must
narrow them (`cast`, `is?`, `match`) like any other `any`. That is already how
`any` works; nothing new is needed.

A Saffron module importing a Turmeric module is the interesting direction. The
callee has a real signature. Three options were considered:

- **(a) Reject** unless the Saffron caller annotates. Safe, and unusable.
- **(b) Erase** -- pass the `any` payload through unchecked. Fast, and it turns
  a type error into a memory-safety bug, because the payload word is
  reinterpreted at whatever the callee declared.
- **(c) Check at the seam** -- insert an implicit `cast` at each argument whose
  static type is `any` and whose parameter type is concrete. A mismatch panics
  with the existing `cast: any holds cstr, not int` message.

**(c).** It is the only option that keeps the typed half of the program's
guarantees actually meaning something, the check is one tag compare, and the
machinery (`EX_ANY_CAST`, `__tur_any_cast_check`) already ships. This is the
standard sound-gradual-typing answer and the reason `cast` was built checked
rather than unchecked in the first place.

The cost is honest and should be documented rather than hidden: a Saffron
program that calls the typed stdlib in a hot loop pays one tag compare per
argument. `--saffron-unchecked-boundary` is deliberately **not** proposed; if
the cost ever matters, the answer is D3's rung 2, not a soundness switch.

### D6 -- refinements become runtime contracts

**Verdict: a refinement in a Saffron file lowers to a runtime contract check,
not an error and not a silent drop.**

**DONE 2026-09-08.** The INTERPRETER was already correct; the compiled path
emitted uncompilable C, because a predicate over an `any` is a dyn-op returning
`any` while `tur-contract-check` takes a `bool`. That is the same question D4
answers for an `if` condition, so the two now share one helper
(`elab_saffron_truthy`), applied at all three contract sites -- parameter
refinements, `:pre`, and the return refinement. Pinned by
`tests/fixtures/saffron-refinement-contract` and `-violated` as a PAIR: a
passing-only fixture would also pass against a compiler that dropped the
predicate, which is exactly the outcome this decision forbids.

`#refine{x : int | (> x 0)}` needs a static base type and an SMT discharge.
Neither exists over `any`. But the language already ships the dynamic answer:
`stdlib/contract.tur`, with `assert!`/`require!`/`ensure!` and a contract
handler.

So a refinement on a Saffron binding keeps its predicate and checks it at
runtime. This is a genuinely good outcome rather than a consolation prize --
it is exactly the design Racket landed on, the predicate stays in the source
where a reader can see it, and a Saffron file that later gets annotated can
have its contracts discharged statically without changing a character.

The one thing that must not happen is a refinement being accepted and
**ignored**. That would make a Saffron file look like it carries a guarantee
it does not have.

### D7 -- static-only features are rejected, with a message that says why

**Verdict: hard error naming the feature and the reason, never a silent
downgrade.** The verdict stands. The LIST was wrong, and measurement cut it
from five families to one.

#### DONE 2026-09-08 -- and the list is one entry, not five

The draft below listed five families. Each was then measured, by running the
same violating program in a Saffron file and in a Turmeric file and comparing
the diagnostics. **Four of the five check identically in Saffron.**

| Feature | Drafted as | Measured in a Saffron file |
| --- | --- | --- |
| `with-region` | reject | **genuinely downgraded.** Emits `tur_region_pop` where Turmeric emits `tur_region_pop_checked` -- retire, not rewind. Safe; reclaims nothing. **Rejected, TUR-E0312.** |
| `defgadt` | reject | **works.** Skolem escape fires identically. |
| `Session[P]` | reject | **works.** TUR-E0211 fires, naming the same protocol state. |
| `^linear` / `^unique` / `lref<T>` | reject | **works.** TUR-E0101 / TUR-E0100 fire identically. |
| `&T` / `&mut T` | reject | **works.** The aliasing conflict fires identically. |

The premise that produced the long list -- "an `any` cannot carry these
guarantees" -- conflated two different kinds of proof:

- Proofs that read an **inferred type**. `with-region` is the only one. Its
  rewind is licensed by a static walk over the *bracket body's result type*,
  and that is exactly the thing Saffron makes `any`.
- Proofs that read an **annotation**, or walk **uses and scopes**. Everything
  else. A GADT's indices come from its constructors' return-type annotations;
  a session's protocol is in the channel's annotation; linearity is a use
  count; a borrow is a scope walk. D2 keeps annotations legal in Saffron, so
  `any` never enters any of these.

The draft's safety argument was also wrong in the direction that matters. A
Saffron region bracket is **not** a use-after-rewind waiting to happen:
`region_type_reaches_node` takes its `default: return true` arm for `TY_ANY`
("can reach a node"), so the static walk refuses and the emitter conservatively
*retires* the generation. The failure is not unsoundness -- it is that the
bracket costs a push and a pop and saves nothing, with nothing saying so.
**That silence is what TUR-E0312 refuses.**

`bt-scope` opens a region too and is deliberately **not** rejected. Its feature
is the trail level -- mark and undo -- which works in Saffron unchanged; only
the region half degrades. Taking a working feature away to report a lost
optimisation is the wrong trade.

Pinned by `tests/fixtures/errors/saffron-region-bracket-refused` (the
rejection) and, more importantly, by
`tests/fixtures/saffron-static-guarantees-still-hold` plus four `errors/`
fixtures (`saffron-gadt-skolem-escape`, `saffron-session-protocol-incomplete`,
`saffron-linear-used-twice`, `saffron-borrow-conflict`) -- the guard against a
later reading of the original list quietly removing four working features.

Regions being off is still true, and still per **file**, not per program: a
Turmeric module in the same project keeps them in full. The two dialects link
together; they just do not each get the other's guarantees.

#### The original draft, for the record

In a Saffron file, these are `TUR-E03xx` errors:

- session types (`Session[P]`, `make-session`) -- the protocol *is* the type
- GADTs (`defgadt`) -- index refinement is the whole feature
- linear / affine / unique (`^linear`, `^unique`, `lref<T>`)
- explicit borrows (`&T`, `&mut T`) and lifetime annotations
- `with-region` / region brackets

The last three deserve a note. Regions rewind a generation only when the
compiler can *prove* nothing outside it points in, and that proof is a static
walk over the bracket's result type (see the Region Store Hooks rule in
CLAUDE.md). Over `any`, the result type says nothing. A region bracket in a
Saffron file would be a use-after-rewind waiting to happen, which is precisely
the failure mode `docs/archive/region-escape-through-unhooked-stores.md`
documents. **Saffron runs on the GC/RC arm; regions are off, not degraded.**

Note this is a *file*-level restriction, not a program-level one. A Turmeric
module in the same project keeps regions, linearity, and sessions. The two
dialects link together; they just do not each get the other's guarantees.

### D8 -- typeclasses need a static receiver, at first

**Verdict: S1-S7 require a statically known receiver type at each typeclass
method call. Runtime instance dispatch stays unscheduled -- but it is a
smaller, better-founded piece of work than the first draft of this plan
assumed, and the shape below is what it would be.**

(**Superseded 2026-09-09:** runtime instance dispatch was scheduled as S9 and
built. A method call on an un-narrowed `any` in a Saffron file dispatches on
the box tag; typed Turmeric keeps the diagnostic. See [S9](#s9----runtime-typeclass-dispatch-d8----built-2026-09-09)
for what shipped and what is still open. The measurements below are what it
was built on.)

The first draft asserted this was tractable "because `emit_any_type_id`'s name
table is already a runtime type registry". That was a guess. It was then
measured, and the picture is more favourable in one direction and more
constrained in another.

#### What was measured

**(a) The static path monomorphizes; there is nothing to reuse.** A
constrained `defn` produces one specialization per instantiating type:

```c
static double describe__spec__double_tur_adt_Circle(tur_adt_Circle x);
static double describe__spec__double_tur_adt_Square(tur_adt_Square x);
```

Under Saffron the argument is `any` and pins no type, so there is nothing to
specialize on. Runtime dispatch is not an optimisation of the static path --
it is a different path.

**(b) Per-instance dict types are not the obstacle they look like.** Each
instance gets its own C struct with concrete-typed method pointers, and there
is no `dict_Shape` supertype:

```c
typedef struct dict_Shape_Circle { double (*area)(tur_adt_Circle); } dict_Shape_Circle;
typedef struct dict_Shape_Square { double (*area)(tur_adt_Square); } dict_Shape_Square;
```

But the existing dispatch site already punches straight through that, casting
the dictionary to `void **` and indexing by slot:

```c
/* the mode-B dict-clone body, from tests/fixtures/forall-dict-show */
static int64_t poly_hyshow_un_undict_un1444(int64_t __dict_1445, int64_t x) {
    const char *__ps_40 =
        (((const char * (*)(int64_t))((void **)(intptr_t)__dict_1445)[0])(x));
```

So **a dictionary is already an `int64_t` at runtime and a method is already
reached as `((void **)dict)[slot]`.** The plumbing D8 needs largely exists and
ships; `forall-dict-pass` graduated 2026-07-06.

**(c) What is actually missing is instance *selection*.** In mode B the
**caller** picks the singleton, from a type it knows statically:

```c
((int64_t(*)(void*, int64_t, int64_t))f.fn)(f.env,
    (int64_t)(intptr_t)(&dict_Show_int_singleton), (int64_t)(INT64_C(7)));
```

Saffron has no such caller. The selection has to come from the value's box tag:
`dict = registry[class][tag]`. That registry is exactly what S0's P1 fix
builds -- so **P1 is a hard prerequisite for D8**, and conversely D8 is
cheaper than it looks *given* P1.

**(d) The carrier pun does not survive by-value receivers.** The cast above is
honest only because `int` and `bool` both ride the int64 carrier. With a
by-value struct receiver the same machinery emits uncompilable C:

```
error: incompatible type for argument 1 of
  '(double (*)(tur_adt_Square))*(void **)__dict_1449'
  note: expected 'tur_adt_Square' but argument is of type 'int64_t'
```

That is a defect in its own right -- `forall-dict-pass` guards its other
unsupported shape with TUR-E0311, and this one has no guard -- and it is filed
as
[forall-dict-byvalue-receiver-emits-uncompilable-c](../archive/forall-dict-byvalue-receiver-emits-uncompilable-c.md).
For D8 it is the load-bearing constraint: dictionary slots must hold
**per-instance carrier wrappers**, not raw instance functions:

```c
static double __dictwrap_Shape_area_Circle(int64_t c) {
    return __inst_Shape_area_Circle(*(tur_adt_Circle *)(intptr_t)c);
}
```

Then every slot has one uniform carrier signature and the pun becomes honest.
The box/no-box decision is the same `emit_type_is_byvalue_adt` predicate `any`
widening already uses, so the two paths should share it.

**(e) The interpreter splits from the compiled path here too.** `TypeClassEnv`
(`typeclass.c:166`) is a compile-time linked list searched by `TypeKind` plus
`AdtDef` pointer, living in the compiler arena. Under `--interpret` that env is
*live at eval time*, and values already carry their type name
(`turi_any_named_type`). So interpreted runtime dispatch is a lookup against
machinery that is already in memory; compiled runtime dispatch needs the table
emitted. **This is the same asymmetry as Section 2.3**, which is a good sign
the staging model generalises rather than being special-pleading for S3/S4.

#### How big is the limitation, actually? (measured)

Smaller than "no typeclasses in Saffron" sounds, because **the operators a
dynamic program leans on are not typeclass methods.** `+ - * / = < > <=`
and `println` are rows in the builtin operator table
(`builtins.c:28` -- `{ "=", NULL, 2, 2, {.kind=TY_INT}, {.kind=TY_BOOL},
BS_BIN_INFIX, "==" }`), not `Eq`'s `eq?`. They are D4's problem, and D4 solves
them for `any` without any instance resolution at all. Ordinary Saffron code --
arithmetic, comparison, printing, string work, control flow -- never reaches
D8's limit.

What does reach it: user-defined classes, the HKT stack (`fmap`, `bind`,
`pure`), `Eq`'s `eq?`, and `Hash`/`MapKey`, which `Map` requires of its keys.

**The escape hatches work for a MONOMORPHIC receiver:**

| Route | Status | Shape |
|---|---|---|
| `is?`-guard narrowing | **works today** | `(if (is? x Circle) (area x) ...)` -- verified: 19.6349 / 50.41 |
| explicit `cast` | **works today** | `(area (cast x Circle))` |
| annotate the binding | works trivially | `[x : Circle]` |
| `@TypeName` witness | **works (fixed 2026-09-07)** | `(tag-of @bool x)` -- pins the instance and unboxes, checked |

The first is the important one, and it is not ceremony: it reads as a
type-case, which is how dynamic languages dispatch anyway (Clojure's
`condp instance?`, Racket's predicate `cond`). A Saffron program that wants
per-type behaviour writes the type-case it would have written regardless.

The fourth row used to read "broken on `any`", which mattered more than a
missing convenience: `@TypeName` is what the compiler's own ambiguity
diagnostic tells you to reach for, so following the hint produced a `cc`
error. It now pins the instance **and** unboxes the receiver, checked -- one
token, and a wrong witness panics rather than reinterpreting the payload. The
same change made a method call on an un-narrowed `any` a diagnostic naming all
three routes, where with a single instance in scope there had been no
diagnostic at all. See
[typeclass-dispatch-on-any-receiver-emits-uncompilable-c](../archive/typeclass-dispatch-on-any-receiver-emits-uncompilable-c.md);
its single-instance half had a worse cause than filed, recorded there.

**For a PARAMETRIC or HKT receiver, none of them work.** This was measured
after the fact and it inverts the paragraph above for the entire `Functor` /
`Applicative` / `Monad` stack, plus `Option` and `Result`:

| Attempt on an `any` holding `(Some 7.1)` | Result |
|---|---|
| `(is? x Option)` | **silently `false`** |
| `(cast x Option)` | **panics**: `cast: any holds Option, not Option` |
| `(is? x (Option float))` | `error: 'is?' expects a type name as second argument` |
| `(cast x (Option float))` | same |

with `type-of x` answering `"Option"` the whole time, so nothing in the
language surface indicates what is wrong. The cause is that
`emit_any_type_id` interns by `type_name`, which renders a `TY_APP` **per
instantiation** -- deliberately, so `(Box int)` and `(Box float)` stay
distinct -- while a bare `Option` target resolves to the head type, a
different key. Two ids get interned and both are `shown` as `"Option"`:

```c
case 1000: return "Option";      /* the widen: (Option float) */
case 1001: return "Option";      /* the is? target: bare Option */
```

**FIXED 2026-09-07** --
[any-narrowing-broken-for-parametric-receivers](../archive/any-narrowing-broken-for-parametric-receivers.md).
`is?` and `cast` now share one target resolver: an applied target
`(Option float)` interns the same `TY_APP` the widen site did, and a bare
constructor is a hard error naming the arity instead of a silent `false`. So
the type-case idiom reaches the HKT stack, on both back ends:

```turmeric
(defn dyn-double [x : any] : float
  (if (is? x (Option float))
    (unwrap-or (fmap (cast x (Option float))
                     (fn [v : float] : float (* v 2.0)))
               0.0)
    0.0))                                    ;; => 14.2, compiled and interpreted
```

`tests/fixtures/any-narrow-parametric-roundtrip`. With that, the parametric
case rejoins the monomorphic one and the "how big is the limitation" answer
above holds uniformly -- which was the point of fixing it before scheduling
anything else here.

Two limits were recorded rather than silently absorbed, and **the first is now
closed (2026-09-08)**. The `if`-guard narrowing did **not** extend to an
applied target, so the `cast` above was load-bearing rather than decorative;
"widening it is its own change" was right, and that change is
`tests/fixtures/if-guard-narrows-applied-target` -- `if_guard_narrowing`
matched `(is? x T)` only for an `F_SYM` target and threaded a `Symbol` into the
rewrite, so passing the type FORM through was the whole fix. Note the
consequence: the explicit `cast` in the branch is now REDUNDANT and therefore an
error, exactly as it already was for a bare target, so
`any-narrow-parametric-roundtrip` was updated to drop it. That is a behaviour
change for code written against the workaround, and it is the workaround going
away rather than a capability being lost.

The second limit stands: the interpreter cannot discriminate two instantiations of one
constructor (a `TuriValue` carries the ADT, not the type argument), so it
head-matches; `any-narrow-parametric-discriminates` and
`any-cast-wrong-instantiation` are compiled-only for exactly that reason.

A second, independent gap sits next to it: a *generic defn* called in an
`: any` return position is never monomorphised, so
`(defn f [] : any (some 7.1))` emits a call to an undeclared `some` and fails
at `cc` (writing `(Some 7.1)` directly works). Filed as
[generic-fn-in-any-return-position-emits-uncompilable-c](../archive/generic-fn-in-any-return-position-emits-uncompilable-c.md).

`@TypeName` (`elab_typeclasses.c:5563`) is the dedicated syntax for exactly
this situation -- and it is what the compiler's own ambiguity diagnostic
recommends -- but it pins the instance without unboxing the receiver, so
following the hint produces a `cc` error. Filed as
[typeclass-dispatch-on-any-receiver-emits-uncompilable-c](../archive/typeclass-dispatch-on-any-receiver-emits-uncompilable-c.md).
**Fixing it is the cheapest large improvement to D8's ergonomics available**:
the witness already names the target instance, which is precisely what `cast`
needs, so it can lower to dispatch-plus-checked-unbox and become a one-token
answer to the erased-receiver case.

**Where the hatches genuinely run out:** a heterogeneous collection crossed
with an open (user-extensible) class. `(map show xs)` over a vec holding three
user types cannot be narrowed at one site -- the dispatch has to happen
per element, so the user ends up hand-writing the type-case that a vtable
would be. That is the real cost, and S6 makes heterogeneous collections the
default, so it is not a corner case.

**Where they do not run out, and this bounds the damage:** a *closed* instance
set can have its type-case written once, in the prelude, instead of by every
caller. `MapKey` has five stdlib instances (`int`, `bool`, `cstr`, `float32`,
`float`) plus `String` and `Sym` -- a finite set the prelude controls. So
S6's `assoc`/`get` can dispatch dynamic keys with one `is?` chain and Saffron
gets working maps without D8. The same trick covers `Hash` and `Eq`.

So the limitation has two halves, and only the first is narrow:

- **Monomorphic receivers: narrow.** Closed classes are a prelude
  implementation detail; open classes over heterogeneous data are where a user
  feels it, and the type-case they write is idiomatic anyway.
- ~~**Parametric / HKT receivers: total, until the narrowing bug is fixed.**~~
  **RESOLVED. This paragraph was stale from 2026-09-07 and is kept struck
  through because it was the stated blocker.** The narrowing bug was fixed that
  day ([archived](../archive/any-narrowing-broken-for-parametric-receivers.md))
  and re-verified 2026-09-08: `(is? x (Option float))` / `(cast x (Option
  float))` round-trip on both back ends. `fmap`/`bind`/`pure`, `Option`,
  `Result` and the monadic pipelines are reachable from `any` via the type-case.

The parametric case has **rejoined the monomorphic one**, which is what that
paragraph said would happen. The remaining asymmetry between the two -- the
`if`-guard narrowed a BARE target but not an APPLIED one, so the parametric
shape needed an explicit `cast` the guard had already proved -- was closed
2026-09-08 (`tests/fixtures/if-guard-narrows-applied-target`). Both shapes now
narrow, and in both a redundant `cast` on the narrowed binding is an error, as
it always was for the bare shape.

So "how much does D8 cost us" can now be estimated against the design rather
than against a bug -- which was the point of fixing it first.

#### The verdict, restated -- CLEARED FOR WORK 2026-09-08

**Every prerequisite D8 named is now met.** The verdict used to read "D8 stays
unscheduled ... it needs P1 and P2 fixed first". P1 (the runtime type registry)
and P2 (the by-value receiver, now guarded) landed 2026-09-07, along with P2b,
P2c and P2d; the last asymmetry between the monomorphic and parametric
type-case closed 2026-09-08. Nothing technical blocks D8 any more, and it is
**scheduled as S9** below.

It is scheduled AFTER S8, on the plan's own reasoning rather than for
convenience. The open questions the measurement did not answer -- superclass
chains, default methods, HKT receivers, and what happens when two instances
match a tag -- were called out as "worth deciding on evidence from real Saffron
programs, not up front." S8 is what makes real Saffron programs writable: the
guide, the editor support, the REPL. Building D8's dispatch before anyone can
comfortably write the programs that would settle its design questions is the
order this plan set out to avoid.

Until S9 lands, Saffron still ships with `(show x)` on an un-narrowed `any` as
an error naming the three narrowing routes -- which, since the type-case now
reaches the HKT stack, is a workable answer rather than a wall.

The shape, restated as buildable work:

1. One class-level dict type with carrier-shaped slots.
2. ~~Per-instance wrappers from (d)~~ -- **DONE 2026-09-09.** The piece the plan
   called "the only one with no home yet". Each instance now gets a carrier
   wrapper in its dict slot, and the slot is declared carrier-shaped to match,
   so the mode-B `(void **)dict[slot]` pun is honest for a by-value receiver.
   That also answers the guard's second objection -- no single REPRESENTATIVE
   instance could stand for two layouts -- because every slot is now
   `(carrier) -> ret`. `forall-dict-byvalue-receiver` went from an `errors/`
   fixture to a passing one that both back ends run, and the interpreter's
   answer (19.6349 / 50.41 / 0) is what the compiled path now prints.
3. Registration into P1's registry at static-init.
4. A `registry[class][tag]` lookup at the call site.
5. A clean "no instance for T" panic.

**Pieces 3-5 are DONE 2026-09-09.** `(.show x)` on an un-narrowed `any` runs
the right instance on both back ends; a type with no instance panics naming the
class and the runtime type. See "S9 -- built" below.

#### Piece 3 was ATTEMPTED 2026-09-09 and reverted -- two constraints found

The registry itself was built and **works**: a `{class, tag, dict}` row table
mirroring P1's chunked `{id, name, boxed}` one, accumulated per TU and
published at static-init with a linear find. On the rank-2 fixture it emitted
exactly what it should:

```c
static const __tur_inst_row __tur_inst_rows[] = {
    { "Eq", 3LL, &dict_Eq_int_singleton },
    { "Shape", 5271915457487731915LL, &dict_Shape_Circle_singleton },
    { "Shape", 7170320896677541932LL, &dict_Shape_Square_singleton },
};
```

It was reverted because COMPILING it -- not reading it -- turned up two
constraints that change the shape of the remaining work:

1. **Dicts are only emitted for rank-2 / mode-B use.** A program with purely
   static dispatch emits NO dict at all, so hanging registration off the
   singleton site registers nothing for exactly the programs D8 is for. (This
   is the same measurement that made piece 2 safe -- the slot has no typed
   caller -- read the other way round.) Runtime dispatch needs a dict per
   instance of any class with an `any`-receiver call site, so **something must
   force dict emission**, and deciding what forces it is now the first question
   of piece 3 rather than an afterthought.

2. **The row table must be emitted AFTER the singletons.** Published in the
   preamble beside the type rows -- the obvious place, since the two are
   siblings -- it references `&dict_Shape_Circle_singleton` before that symbol
   exists: `error: 'dict_Eq_int_singleton' undeclared here`. The type rows have
   no such problem because they hold only string literals and integers. Either
   the table moves late or the singletons get forward declarations.

Neither is visible from reading the code, and the second hid behind the first
in the compiler's output as a spurious-looking
`missing initializer for field 'dict'` -- gcc had dropped the undeclared symbol
and then reported the row as short.

So piece 3 is a known quantity now rather than a sketch, and the remaining work
is: force dict emission for dispatched classes, emit the rows late, add the
call-site node, and the no-instance panic.

#### What forces dict emission: measured 2026-09-09

Constraint 1 above asks what should force a dict. The blunt answer -- **every
instance of every class, no analysis** -- was measured rather than argued, and
it is viable, with the cost falling in a place that changes where the gate
belongs.

**It compiles and runs.** `emit_instance_is_live` (`emit_module.c:6564`) patched
to return true unconditionally, rebuilt: a hello-world goes from 1 dict
singleton to **47**, and from 100 to 128 `__inst_*` bodies, and still builds and
runs clean. The earlier failure (`__inst_Functor_fmap_Option undeclared`) came
from bypassing the *dict* half of that gate while leaving the *body* half on.
The gate governs both in lockstep -- `emit_stmt.c:675` and
`emit_abi_fn_skip_generic` -- and moving both together is sound. This is worth
recording because "force dict emission" reads like a dict-site change and is
not one.

**The cost is nothing today and 4.5x once the rows exist.** Three binaries,
identical flags:

| | bytes |
|---|---|
| baseline (gate on) | 39,512 |
| all-live, dicts unreferenced | 39,512 |
| all-live + a table referencing every dict | **179,992** |

The linker strips unreferenced statics, so forcing liveness alone is free. But
the `{class, tag, dict}` table is exactly the thing that references every dict,
which anchors all 47 and their method bodies. Hello-world becomes 180 KB -- on
**every** program, Turmeric and Saffron alike. Emitted C grows 330 KB -> 352 KB
either way (compile time, always paid).

So the v0 shape is not "no gate", it is **one coarse gate instead of a per-class
pre-pass**, and the trigger already exists. `elab_typeclasses.c:6637` is the site
that today REJECTS a method call on an un-narrowed `any`, and it holds
`best_inst->typeclass`. Turning that rejection into a dispatch site inherently
records "class C is dynamically dispatched" -- no separate walk over the
program. Emit rows for every instance of those classes only, and a program that
never dispatches on `any` emits no table and pays nothing. A later pre-pass then
refines instance selection *within* a dispatched class, which is a smaller and
purely-optimizing change.

**A fourth constraint surfaced in the sweep**: `dict_Clone_T_singleton` -- an
instance whose receiver is a TYPE VARIABLE, so it has no ground tag to key a row
on. It is in the forced set today. Skip such instances, or give them a wildcard
row; either way it needs deciding before rows can be emitted.

#### Potential pre-passes: researched 2026-09-09

The measurement above ends on "v0 wants one coarse gate, a pre-pass refines it
later". Researching what that pre-pass could be turned up three things that
change the answer, two of them because the machinery already exists.

**The pre-pass already exists -- it is `emit_abi_scan_expr`.** The pre-emission
ABI scan already walks every body, already has an `EX_UNION_INJECT` case
(`emit_module.c:6043`, added by P2d), and already carries the exact primitive
piece 3 needs: `emit_abi_note_instance_dict_ref` (`emit_module.c:3103`) marks an
instance live by noting a carrier call on each of its method bindings -- which is
precisely what keeps the dict and its bodies in lockstep. It is used today to
keep an existential pack's witness dicts alive. So "force dict emission" is not a
new pass and not a change at the dict site; it is new *notes* inside a walk that
already runs.

**The cheapest useful key is the WIDENED TAG, not the dispatched class.** The
widen site already computes the row key: `emit_any_type_id(ctx,
e->as.union_inject_.value->type)`, guarded by `e->type.kind == TY_ANY` to
separate it from a real union's member index (`emit_expr.c:6581-6587`).
Collecting that set in the scan is about three lines. What it buys, measured:

| | count |
|---|---|
| dicts forced by "every instance of every class" | 47 |
| ... grouped: Eq 22, MapKey 6, Hash 6, Functor 3, Monad 2, Clone 2, Applicative 2, Alternative 2, MonadError 1, Bifunctor 1 | 10 classes |
| distinct types ever widened into `any`, richest Saffron fixture in the tree (`docs-saffron-guide-examples`) | **8** (4 named + 4 primitive) |
| ... other Saffron fixtures | 3-7 |

The class axis is the weak one and the numbers say why: **`Eq` is both the
most-instantiated class and the likeliest dynamic-dispatch target**, so gating on
"classes with an `any` dispatch site" can still admit 22 of the 47 for exactly
the program you care about. The tag axis cuts the same set to at most 8 and
realistically 3-4, because a value can only be in an `any` box by having been
widened somewhere.

The tag axis also **disposes of the fourth constraint for free**: an instance
with a type-variable receiver (`Clone T`) matches no widened tag, so it is simply
never registered. It stops being a special case.

**Per-TU soundness -- this is what rules the cross-product out.** P1 made the
registry a set of per-TU chunks merged at static init, with the id a hash so
"the same type carries the same id in every TU and the rows simply agree where
they overlap" (`emit_module.c:962`). Instance rows can follow that pattern
exactly. But `tur build <dir>` folds a project into ONE TU while `--shared` and
`emit-c --output-dir` do not (A.5), so a pre-pass is only usable if each TU can
decide alone:

- **Tag axis alone is TU-local-sound.** The TU that widens `Point` registers
  every instance whose receiver is `Point`, for every class. A different TU that
  dispatches on that box finds the row in the merged registry. Neither TU needs
  to know what the other does.
- **Class axis alone is TU-local-sound**, for the mirror reason (the dispatching
  TU has all the instances via autoload), but buys much less.
- **The cross-product (tags x dispatched classes) is NOT.** TU A widens `Point`
  and dispatches nothing; TU B dispatches `.eq` and widens nothing; neither
  registers `Eq Point` and the program panics at a site that should work. It is
  sound only whole-program, which `--shared` is not.

So the tag axis is the pre-pass to build, and it is buildable *now* rather than
later -- it is cheaper than the class gate it would replace, not an optimisation
layered on top. The one hole is an `any` box a TU never widened because the
RUNTIME built it (native ADT construction, the interpreter); that needs
confirming before the tag set can be treated as complete.

**The endgame may be no registry at all, and that machinery also exists.**
`IT4 typeclass intersection dispatch on union types`
(`elab_typeclasses.c:6045`) already handles a receiver whose type is a UNION: for
each member it resolves the instance at elaboration and builds an `EX_MATCH`, so
`(.show x)` on `(int | bool)` emits an inline tag switch calling
`__inst_Show_show_int` / `__inst_Show_show_bool` **directly** -- no dict, no
registry, no indirect call, each arm inlinable (verified by reading the emitted C
for `tests/fixtures/union-types-typeclass-dispatch`). Its own comment says it
bypasses dictionary dispatch deliberately.

Two caveats, both measured. A union's tag is a MEMBER INDEX and an `any`'s is the
global type id -- two tag spaces -- and the bridge already exists as a
statically-generated per-union remap switch on the `union -> any` widen (`case 0:
TUR_TAG(3LL, ...)` for int, `case 1: TUR_TAG(2LL, ...)` for bool). And the
receiver must be a union *at elaboration*, so refining `any -> union` is a
dataflow question (which widens reach this site), which is a real new pass and a
whole-program one.

That makes it a v1 item, not v0. But it reframes what "add a pre-pass later"
means: the later pre-pass is not a smaller registry, it is **eliminating the
registry at every site whose incoming type set is closed**, falling back to the
registry only where it genuinely is not.

The four design questions D8 lists may partly answer themselves once the key is
the concrete box tag -- two instances cannot match one tag, and defaults are
already resolved into per-instance slots -- but that is reasoning, not
measurement, and superclass chains genuinely need a decision.

### D9 -- the gate

(**Done and retired.** The row below shipped at 0.45.0 and Saffron GRADUATED
at 0.46.0 on 2026-09-10, so the row is gone: `#lang saffron` needs no enable,
prints no lifecycle warning, and a manifest's `:experiments` cannot turn it
off. `g_opt_saffron` survives as `g_opt_dynamic_any`, a "this build has a
dynamic TU" fact the emitter reads, not a gate -- see the graduation note in
`src/runtime/experiments.c`. `lang_layers.c`, cited below, is now
`lang_dialects.c`. The section is kept as the record of how it was gated.)

Per CLAUDE.md's Experimental Compiler Features rule, Saffron is exactly the
shape that must ship behind `--enable=`:

```c
/* src/runtime/experiments.c -- the first live row since option-niche. */
{ "saffron",
  "dynamically typed dialect (#lang saffron)",
  "docs/archive/saffron-lang-plan.md",
  "0.45.0",                   /* introduced */
  "0.52.0",                   /* expires_at -- advisory, never blocks a cut */
  XF_LIFECYCLE_PROTOTYPE,
  &g_opt_saffron },
```

`#lang saffron` is the file-scoped enable, at CLI precedence -- the same
relationship a semantic `#lang` layer has to its experiment. It therefore
inherits that rule's sharp edge, and should: a project manifest that scopes
`:experiments` and leaves `saffron` out makes `#lang saffron` a **hard error**,
not a silent ignore. `lang_layers_apply_semantic` already implements exactly
this policy (`lang_layers.c:160`); the dialect axis calls the same code.

`expires_at` is a deadline, not an earliest date. Graduating early is routine
and an expiring row never blocks a release cut.

---

## 5. Stages

Each stage is independently landable and leaves the tree green-ish. Sizes are
rough multiples of a day of focused work, not commitments.

### S0 -- prerequisites (was "small"; the id fix is medium)

Five filed defects, all wrong regardless of Saffron. The first is the one that
matters: **it was an open question in the first draft of this plan and has
since been measured. The answer is that the ids are NOT stable, and Saffron
cannot be built until they are.**

| # | Report | Blocks |
|---|---|---|
| ~~P1~~ | ~~any-type-ids-are-per-tu~~ | **DONE 2026-09-07.** The id is a hash of the identity key, not the per-TU intern index, and the name table became a registry each TU publishes `{id, name, boxed}` rows into. Multi-TU builds now agree; the drop reads the minting TU's boxed flag. This also builds the runtime type registry D8 would key instance lookup off, as anticipated. [Archived](../archive/any-type-ids-are-per-tu.md) |
| ~~P2~~ | ~~forall-dict-byvalue-receiver-emits-uncompilable-c~~ | **DONE 2026-09-07** (guarded). The by-value shape is now a diagnostic, not uncompilable C. D8's carrier-wrapper work is smaller than the report implied -- the caller already boxes into the carrier, so only a per-instance deref wrapper in the dict slot is missing -- but it is a dictionary-ABI change and stays with D8. [Archived](../archive/forall-dict-byvalue-receiver-emits-uncompilable-c.md) |
| ~~P2b~~ | ~~typeclass-dispatch-on-any-receiver-emits-uncompilable-c~~ | **DONE 2026-09-07.** `@TypeName` on an `any` receiver now implies the checked unbox, so the witness is a complete one-token answer to an erased receiver; and a method call on an un-narrowed `any` is a diagnostic naming all three routes instead of uncompilable C. [Archived](../archive/typeclass-dispatch-on-any-receiver-emits-uncompilable-c.md) |
| ~~P2c~~ | ~~any-narrowing-broken-for-parametric-receivers~~ | **DONE 2026-09-07.** Was: `is?` on an `any`-held `Option` silently false, `cast` panicking `holds Option, not Option`. `is?`/`cast` now share one target resolver, take an applied `(Option float)`, and reject a bare constructor with a diagnostic. The type-case idiom reaches the HKT stack on both paths. [Archived](../archive/any-narrowing-broken-for-parametric-receivers.md) |
| ~~P2d~~ | ~~generic-fn-in-any-return-position-emits-uncompilable-c~~ | **DONE 2026-09-07.** Not a missing monomorph request -- elaboration was already correct, and `emit_abi_scan_expr` simply had no case for `EX_UNION_INJECT`, so a call under a widen was never scanned. Four cases added. Wider than filed: argument position and user generics too. [Archived](../archive/generic-fn-in-any-return-position-emits-uncompilable-c.md) |
| ~~P3~~ | ~~inferred-return-defaults-inconsistently~~ | **DONE 2026-09-07.** Was an ordering bug, not a missing inference: the conflict check ran before the block that adopts the body's type, so it compared against the un-inferred `TY_NIL`. Unannotated returns are now inferred for every body type, float included. [Archived](../archive/inferred-return-defaults-inconsistently.md) |
| ~~P4~~ | ~~type-of-on-boxed-closure-diverges~~ | **DONE 2026-09-07.** A `TY_FN` tag now answers "fn" on both back ends. [Archived](../archive/type-of-on-boxed-closure-diverges.md) |
| ~~P9~~ | ~~any-fn-tag-does-not-discriminate-signatures~~ | **DONE 2026-09-07** via fix direction 2, as anticipated. A fn payload interns a per-SIGNATURE id keyed on the rendered fn type, and `type_name`'s TY_FN case became a shared `tur_fn_type_key` so the interpreter reconstructs the identical key from a closure's FnDef -- real parity, not a documented divergence. Swept up a second defect: the interpreter's cast switch had no TY_FN arm, so `(cast 7 (-> int int))` handed back an int typed as a function. And the anticipation held -- this WAS the prerequisite for D4's dynamic-call row, which is now also done (see below). [Archived](../archive/any-fn-tag-does-not-discriminate-signatures.md) |
| ~~P5~~ | ~~any-type-guide-examples-do-not-compile~~ | **DONE 2026-09-07.** Scope was wider than "docs only" implied: compiling every example in the guide rather than the two filed turned up four more of the same species -- `deftype` used to name a union and an intersection, a fabricated `str` helper, a `defclass` with no `definstance`, and a stale `type-of` -> `"adt"` claim the guide's own text contradicts. Pinned by `docs-any-guide-examples`, which both suites run, so they cannot rot again silently. [Archived](../archive/any-type-guide-examples-do-not-compile.md) |
| ~~P6~~ | ~~any-coercion-not-driven-by-expected-type~~ | **DONE 2026-09-07.** An `: any` annotation now widens at all four positions that take one -- `let`, `def`, the `if` join, and letrec/named-`let` accumulators -- each of which had a distinct cause. These are the positions S2/S6 lean on hardest. [Archived](../archive/any-coercion-not-driven-by-expected-type.md) |
| ~~P7~~ | ~~interp-native-ctor-loses-adt-name~~ | **DONE 2026-09-07**, and the severity was understated: `is?` compares exactly the names `type-of` reports, so this was a false NEGATIVE for every natively-built option and result under `--interpret`, not a cosmetic string -- a type-case took the wrong arm silently, on the path Saffron ships on first. Scope turned out bounded: every native ADT construction goes through `turi_make_struct`, 11 call sites. [Archived](../archive/interp-native-ctor-loses-adt-name.md) |
| ~~P8~~ | ~~forall-dict-float-result-truncated~~ | **DONE 2026-09-07.** A float result through a mode-B dict clone was silently truncated (2.5 -> 2); both ends of the carrier crossing now bit-reinterpret instead of converting. Not Saffron-specific, but it was on the runtime-dictionary machinery D8 would build on. [Archived](../archive/forall-dict-float-result-truncated.md) |

#### P1 -- the id question, answered

`emit_any_type_id` interns into the per-TU `EmitCtx` and returns
`TUR_ANY_ID_BASE + first-seen index`, so **the same type gets a different id in
each translation unit.** Measured on `tur build --shared`, where `amod.c` tags
`Beta` as 1000 while `main.c` reads 1000 as `"Gamma"` and tests `is? v Beta`
as `== 1001`. Four wrong behaviours follow, all reproduced:

1. `type-of` returns another type's name.
2. `is?` is a false negative on the correct type.
3. A valid `cast` panics -- `cast: any holds ByVal, not HeapThing` on a value
   that genuinely is a `HeapThing`, with both names wrong.
4. `__tur_any_drop` consults the wrong `boxed` flag, so a TU calls `free()` on
   a handle another TU owns (or leaks, in the mirror case).

The first draft's probe passed only because **`tur build <dir>` folds the whole
project into one TU** -- confirmed by wrapping `CC`: one `.c`, both modules
inside, one consistent table. `--shared` and `emit-c --output-dir` (the CMake
path) both split, and both diverge.

This reclassified S0 from bookkeeping to a real prerequisite, because Saffron
makes `any` the type of nearly every cross-module value.

**FIXED 2026-09-07** --
[any-type-ids-are-per-tu](../archive/any-type-ids-are-per-tu.md). The id is now
FNV-1a over `type_name`, forced clear of the `TypeKind` range, so it needs no
coordination between TUs; `__tur_any_name_ext` became a registry each TU
publishes `{id, name, boxed}` rows into at static-init, replacing the single
function pointer every TU overwrote. Mode 4 falls out for free: the `boxed`
flag rides the same row as the name, so a drop reads the flag the *minting* TU
published rather than its own table's. The original repro went from
`Gamma / Gamma / 0` to `Gamma / Beta / 1`.

Pinned by `tests/run-any-type-id-multi-module.sh` (ctest
`tur_any_type_id_multi_module`), which drives separate compilation because
`tur build <dir>` inlines everything into one TU and hides the bug -- and which
was verified to fail without the fix, with all four behaviours visible. One
cost recorded: `__tur_any_find` is a linear walk, and `__tur_any_drop` calls it
at every scope exit owning an `any`; if that ever measures, the answer is an
index built at startup, not a return to per-TU numbering.

The registry was not throwaway scaffolding, as anticipated: it is the runtime
type registry D8 would key instance lookup off, so this is the first half of
D8's foundation whether or not D8 is ever scheduled.

**Exit: MET 2026-09-07.** ~~P1~~ done, with the multi-TU pin the report asked
for (extended since to pin a fn payload's id across TUs as well); P2-P9 all
fixed and archived. S1 is unblocked.

Four further `any` defects were found by this burn-down and closed with it --
[union-to-any-widen](../archive/union-to-any-widen-emits-uncompilable-c.md),
[partial-application-as-ptr](../archive/partial-application-widened-to-any-is-a-ptr.md),
[collection-handles-as-int](../archive/interp-collection-handles-report-as-int.md),
and [capturing-closure-round-trip](../archive/any-cannot-recover-a-capturing-closure.md).
Together they mean **D4's dynamic-call row already works**: an `any` holding any
function shape -- lambda, capturing closure, partial application, named `defn` --
can be tested with `is?`, recovered with `cast`, and called, identically on both
back ends. S4 inherits that rather than building it.

### S1 -- the `#lang` axis, no semantics (small) -- DONE 2026-09-07

D1's plumbing only. `LangDialect`, `SourceFile.lang`, `detect_lang_layered`
out-param, `lang_base_from_name` returning a pair, `reader_type_name`'s
sibling `lang_dialect_name`. The `EXPERIMENTS[]` row from D9, with
`g_opt_saffron` read by nothing yet.

`#lang saffron` compiles a file **exactly as `#lang turmeric` does** and warns
once (TUR-W0060, the experiment lifecycle warning) that the dialect carries no
semantics yet.

**Exit: MET 2026-09-07.** `#lang saffron` and `#lang saffron/sweet` parse and
run identically to their `turmeric` spellings on both back ends; `tur
lang-layers` lists both axes (and `--json` emits them under `dialects` /
`layers`); `tur experiments` carries the row.

Landed as described, with three notes worth keeping:

- **`detect_lang_layered` did not need an out-param.** It has a dozen callers,
  most of which have no use for the language axis, so the dialect-aware entry
  is `detect_lang_dialect` and `detect_lang_layered` became a wrapper passing
  NULL -- the same relationship `detect_lang` already had to it. Only the paths
  that *elaborate* a file thread the dialect onto `SourceFile.lang`.
- **The gate has one home, not one per detection site.** Every path that
  elaborates a file -- compile, `--interpret`, an imported module, the REPL --
  funnels through `read_all_with_registry_from`, which already applies the
  semantic-LAYER gate. `lang_dialect_apply` sits beside it there, so no
  detection site can forget it. The detection sites' only job is to set the
  field.
- **`--interpret` needed a second wiring point.** The file-eval entry
  pre-detects the `#lang` line before the eval blob, to load the prelude under
  the same reader; the language axis needs seeding there for the same reason,
  and setting it only on the blob left the interpreter silently un-gated. A
  probe printing the reader-entry state found that in one step.

The planned `errors/saffron-without-enable` fixture is **not** in the tree, and
the reason is worth recording: the manifest-scoped hard error works (verified by
hand against a project whose `build.tur` carries `:experiments []` -- it reports
"whose experiment 'saffron' is disabled by the project manifest"), but it needs
a project directory with a manifest, which the single-file fixture harness
cannot express. The existing semantic-LAYER policy has the same gap: nothing in
`tests/` covers it either. `errors/saffron-unknown-dialect` covers the reachable
negative instead. Closing the manifest gap properly wants a dedicated runner in
the shape of `tests/run-any-type-id-multi-module.sh`, and would cover both
policies at once.

### S2 -- default to `any`, interpreter first (medium) -- DONE 2026-09-07

Flip the four `TY_INT` defaults in `elab_fns.c` (defn params ~6726, fn params
~9566, defn return ~7183, fn return ~9705) to `TY_ANY` **when the enclosing
`SourceFile.lang` is `LANG_SAFFRON`**. Keep D3's local inference.

At this point almost every interesting Saffron program still fails, because
the operators reject `any`. That is fine and expected: S2's job is to prove
the default flips cleanly and that a program which only *moves* dynamic values
around (binds them, passes them, returns them, stores them) works end to end
on both back ends.

**Exit: MET 2026-09-07**, and on BOTH back ends simultaneously rather than
"interpreter first" -- the flip turned out to be back-end-agnostic, because it
happens in the shared elaborator.

**It was two sites, not four.** The plan named defn params, fn params, defn
return and fn return. Only the two PARAM defaults need flipping: with
parameters defaulting to `any`, an unannotated return infers `any` from the body
on its own (which is what P3 fixed), so `(defn id [x] x)` gets `any -> any` with
no return-site change. Three neighbouring `TY_INT` defaults are deliberately
left alone -- a `& rest` parameter in `defn` and `fn` (that int is a cons-list
HANDLE, not a value type) and an `extern-c` parameter (which declares a C
signature, where `any` would describe an ABI that does not exist).

**The dialect needs no threading through elaboration.** Every Form carries the
`file_id` of the file it was read from, and the dialect lives on the
`SourceFile`, so `lang_span_is_saffron(span)` is a registry lookup. That makes
the answer per-FILE for free, which is D5's contract boundary arriving early: a
Saffron program that loads a Turmeric module gets each file's own defaults, and
the fixture asserts exactly that.

**`--interpret <file>` needed a third wiring point**, and finding it took a
probe rather than a reading. The file-eval entry does not fold the user file
into the eval blob -- it splices a `(load ...)`, so the file is registered
separately by the load path in `elab_toplevel.c`, which set `reader_type` and
layers but not the language. Before that, the compiler saw Saffron defaults for
a `#lang saffron` program and the interpreter saw Turmeric ones. The probe that
found it printed the SourceFile pointer and path at the param site; reading the
control flow had said the blob's `<eval>` entry was the one that mattered.

**What S2 does not yet reach**, all as the stage predicted (the operators reject
`any`, which is S3's job) and all identical on both back ends:

- `(if x ...)` on a dynamic value -- `if condition must be bool, got any`.
- `(println x)` on one -- no `any` overload.
- Arithmetic, comparison, and every other builtin.

One thing it reached that the stage did not predict: `(vec-of (id 1))` ICEs the
compiler with a `(Vec any)` representation disagreement. **Not caused by S2** --
it reproduces in plain Turmeric with explicit annotations -- but S2 makes it
trivially reachable. Filed as
vec-of-any-repr-decision-ice and **fixed 2026-09-07**
([archived](../archive/vec-of-any-repr-decision-ice.md)): two different
questions -- does `(Vec any)` NAME a monomorph, and how is an `any` ELEMENT
stored -- had been answered as one. Its residue,
[vec-any-monomorph-is-half-plumbed](../archive/vec-any-monomorph-is-half-plumbed.md),
was S6's own subject rather than a blocker on it, and is now fixed too.

### S3 -- the dynamic operator layer, interpreter (medium) -- DONE 2026-09-07

The elaborator learns to route a builtin call with an `any` argument to a
dynamic node (`EX_DYN_OP`) instead of failing operator lookup. The
interpreter's arm for that node is thin, because `eval.c:3567` and `:3759`
already do the work.

Covers G3, G4, G9, and D4's truthiness rule.

**Exit: MET 2026-09-07.**

**The interpreter's arm was NOT thin, and the reason is the float rule.** The
claim above -- that `eval_builtin` already does the work -- is half right:
`eval_builtin` IS tag-driven, so the interpreter never needed a static type for
these operators, and the elaborator's refusal was the only thing in the way.
But it decides int-vs-float from argument ZERO's tag alone and then reads every
argument through that union member. That is correct for static Turmeric, where
the elaborator has already made the operands agree, and silently wrong the
moment they can differ. `(* x 2)` with `x = 7.1` printed **6.91692e-323** --
the integer 14's bit pattern read as a double. The dynamic path promotes
instead (all-numeric and any-float means all-float), in the dyn-op arm rather
than in `eval_builtin`, which static callers rely on as-is.

CLAUDE.md's lead-with-7.1 rule is what caught it on the first probe rather than
the second; an integral literal would have printed `42` and looked finished.

**`(* 2 x)` is not symmetric with `(* x 2)`.** The static lookup SUCCEEDS for
the first -- the table is keyed on argument zero, and that one is an int -- and
then rejects `x` as "arg 2: expected int, got any". So the route cannot be
"lookup failed"; it is "any operand is dynamic", which is the honest rule
anyway.

**`and` / `or` needed lazy handling, ahead of the eager argument loop.** They
are short-circuit builtins with bool-only rows (BS_AND_SC / BS_OR_SC), so a
dynamic `and` over ints found no overload and panicked. D4 puts them in the
truthiness group, and they must stay lazy -- `(and (some? x) (unwrap x))` is a
guard, and evaluating the second operand anyway turns it into a crash. They
return a bool, matching Turmeric's; returning the deciding VALUE (Lisp style) is
left as a deliberate D4 choice rather than something to settle by accident.

**The node is EX_DYN_OP, as planned**, and being a distinct kind rather than a
flag on EX_BUILTIN paid for itself twice: `-Werror=switch` enumerated every site
that needed an arm (the debug printer, borrow-check, statement position, the
value emitter), and the turi parity ratchet required the interpreter arm. The
compiled back end reports the S5 gap as a diagnostic naming `--interpret`,
because guessing would emit an int add over tag words -- a miscompile, not a
missing feature.

**Fixtures.** `saffron-dyn-arith-float` (both operand orders, mixed promotion,
comparison; every literal has a fractional part on purpose),
`saffron-dyn-truthy` (D4's rule, including the `0` and `""` rows that would flip
if the decision were ever changed by accident, plus a short-circuit row where
`and` must not evaluate a `(panic ...)`), and
`errors/saffron-dyn-op-compiled-unsupported` for the compiled refusal.

### S4 -- dynamic call and dynamic field access, interpreter (medium) -- DONE 2026-09-07

`EX_DYN_CALL` (G5) and dynamic `(.f x)` (G11), plus `match` on an `any`
scrutinee (G6). Closures widen into `any` with a real tag, which also closes
G8 properly rather than papering it.

This is the stage that makes Saffron a *language* rather than a calculator --
higher-order functions are the whole point of the surface syntax.

**Exit: MET 2026-09-07** -- `tests/fixtures/saffron-higher-order` runs `lmap`,
`lfilter` and `lfold` over `(1 "hi" 7.1 true)`.

**G6 needed nothing.** `match` on an `any` scrutinee already worked, measured
before any code was written. What did NOT work was the arm UNIFIER: it had no
`any` case, so a fold whose recursive arm was still being inferred and whose
base arm was the `any` accumulator was rejected as "expected int (from earlier
arm), got any". The `if` join already widened this way; `match` has its own
unifier and did not. Fixed there, and NOT gated on the dialect -- `any` is
Turmeric's top type too, and an arm of type `any` beside one of type `int` has
exactly one sound join in either language.

**S2's note about returns was incomplete, and this is the correction.** S2
concluded that only the two parameter defaults needed flipping because an
unannotated return infers from the body. True for a non-recursive body; a
SELF-CALL is the case inference cannot cover, because the type is needed before
the body is analysed. `lfilter`'s two `if` branches came out `then=Lst
else=int`, the `int` being its own recursive call reading the pass-1 forward
declaration (the int64 carrier). An unannotated Saffron return now forwards as
`any`, which being the top type joins with whatever the body turns out to
produce, so the post-body construction still narrows it.

**D5's seam landed here rather than in S7**, because the exit criterion needs
it: a Saffron caller reaches stdlib, and stdlib has real signatures. The
implementation is smaller than S7 implies -- `elab_any_unbox_to` is the node
`(cast x T)` already lowers to, so the seam is that node inserted at an
argument, and D5's chosen behaviour (a runtime check, not erasure) comes with
it. Verified both ways: the right type passes through, the wrong type panics
with the ordinary cast message. What S7 still owed was the OTHER direction,
since landed.

**Containers are S6's, and the fixture says so.** The exit list is a `defdata`
defined in the fixture, not a stdlib cons list, because a stdlib cons list
carries int64 handles -- `head` hands back an int and the heterogeneity is gone
before `type-of` sees it. `defdata` needed one line to accept an `:any` field
(it was a name table with no `any` row); whether the wider container story
works is S6's question: the ICE that used to block it is fixed, and so are both
halves of its residue -- a `(Vec any)` now reads back each element with its own
tag, and a `vec-of` at `any` clears the emitted-C ratchet
([vec-any-monomorph-is-half-plumbed](../archive/vec-any-monomorph-is-half-plumbed.md)).

**Three nodes, and `-Werror=switch` plus the turi parity ratchet enumerated
every site each one needed.** EX_DYN_CALL, EX_DYN_FIELD (and S3's EX_DYN_OP)
each forced arms in the debug printer, borrow-check, statement position, the
value emitter and eval.c. The statement-position arms are not uniform and the
difference matters: a dynamic operator and a dynamic field read are pure and
are discarded, a dynamic CALL runs a user function and must not be.

### S5 -- the compiled path (large) -- DONE 2026-09-07

Everything S3 and S4 did for the interpreter, done again in C:
`__tur_dyn_arith`, `__tur_dyn_cmp`, `__tur_dyn_println`, `__tur_dyn_truthy`,
`__tur_dyn_not`, the dynamic call, the dynamic field read, and `match` on an
`any` scrutinee.

**Exit criterion MET.** All four S3/S4 fixtures lost `requires.interp-only`,
gained `requires.leak-check`, and print identically on both back ends;
`saffron-higher-order` -- map/filter/fold over `(1 "hi" 7.1 true)` -- gives
`4 / int / cstr / float / bool / 2 / 2 / 8.1` compiled, `7.1 + 1 = 8.1`
included, which is the float rule surviving the whole dynamic path. `run.sh`
2861 passed / 0 failed, `run-turi.sh` 1953 / 0, `run-leak-check.sh` 87 / 0 with
one known-open (the recursive-ADT box, below), turi parity 117/118 with the
recorded carve-out.

**The ownership prediction was right; it was then fixed, and the mitigation this
section proposed turned out to be the wrong one.**
[The report](../archive/saffron-any-return-defeats-the-frame-box-rule.md) has
the full account, including a correction to its own first diagnosis. Two
blockers in series:

- `expr_subtree_has_inline_c` had no arm for the three dynamic nodes, so every
  Saffron body hit its conservative `default` -- "may hide inline-C" -- and the
  non-retain inference skipped the function entirely, before any of the reasoning
  about result types applied. Found with one probe on the inference's entry after
  reading the control flow gave a confident wrong answer.
- The result gate, which this section correctly identified: the frame-box rule
  is gated on the callee's RESULT being a non-pointer scalar, and a Saffron
  function returns `any`. The fix is not a wider whitelist but the escape walk
  itself, run unconfined for an `any` result -- it draws the distinction a kind
  test cannot, accepting `(defn f [x] (+ x 1))` and `(defn get-x [p] (.x p))`
  while still refusing `(defn dyn [x] x)`.

The box is not freed; it is never allocated -- the widen emits a caller-frame
copy. **RC-managed `any` boxes were not needed**, and nothing measured here
argues for them.

The sibling `saffron-higher-order` leak was a different thing and remains open
as [byvalue-recursive-adt-boxes-are-never-freed](../archive/byvalue-recursive-adt-boxes-are-never-freed.md):
a plain Turmeric recursive ADT leaks one box per cons cell too, with no `any`
anywhere (3 cells / 3 allocations, 5 / 5). Saffron makes that shape easy to
reach; it does not create it.

Five things differed from what this section expected:

- **The dynamic operator set had to be enumerated, and that is a real
  asymmetry.** The interpreter resolves a dynamic operator through
  `builtin_lookup` against the live builtin table, so it inherits every builtin
  for free. A compiled program has no such table, so this half lists its
  operators: arithmetic, the bit operators, comparison, `not`, truthiness and
  `println` -- every builtin over primitive scalars. `cons` and the container
  operations are outside it and get a diagnostic naming the operator, which
  `errors/saffron-dyn-op-compiled-unsupported` now pins (it used to pin the
  whole-stage gap). D4's row 8 (index on `any`) is genuinely S6's.

- **The field read is a chain, not a table, and it is CLOSED.** The interpreter
  scans `ctor->fields[i].name` at run time; the compiled path does that scan at
  emit time over every type in the program with a field of that name, leaving
  one integer compare per candidate. So the interpreter's version is open (any
  struct with the right field works) and this one is closed over the program.
  That is inherent to compiling rather than a shortcut, and for one program the
  two sets coincide.

- **`match` on `any` did not need the box-id switch this section imagined.** It
  needed a NARROW: the arms name an ADT, so the scrutinee unboxes to it through
  the same checked `cast` D5 uses at argument position, and the ordinary match
  machinery runs. One ADT only -- arms from two ADTs would need the switch, and
  guessing which ADT to narrow to would be worse than the existing diagnostic.

- **An `:any` ADT field had to become a real two-word member.** `defdata`
  lowered it to the int64 carrier, which stores the payload and drops the tag,
  so `type-of` on a field read answered whatever the carrier collided with. That
  is three lines (`adt_field_c_type`, `adt_field_scalar_c_type`, and the two
  match-binder sites, which must read the slot rather than scalar-cast it) and
  it is the thing that made a heterogeneous container work at all.

- **An unannotated Saffron return had to BE `any`, not "whatever inference
  produced".** S4 forwarded `any` for the self-call and left the final type to
  inference; the two then disagreed, and `lmap`'s self-call spoke a signature the
  definition did not have. Inference still runs inside the body -- what is pinned
  is the signature, the one place a caller has to agree. This is the completion
  of D3's "the default type is `any`", not a departure from it.

Two smaller things landed with it, both because a fixture asserting both back
ends forced the question. A NIL payload could not be widened at all (`nil` emits
as `((void)0)`, so the carrier cast was a hard cc error) -- nothing in Turmeric
widens a nil, and `(dyn nil)` is ordinary in Saffron. And the interpreter's
runtime type errors named a primitive "a value of a different type" where the
compiled ones named it "cstr", because the helper behind them answers NULL for
anything that is not a struct; a display-name helper beside it makes the two
agree word for word, which is what `saffron-seam-panics` asserts.

### S6 -- containers and the Saffron prelude (medium) -- DONE 2026-09-08

**Landed 2026-09-07: a `(Vec any)` element round-trips with its own tag, on the
compiled path.** That was the stage's blocking question -- until it, a
`(Vec any)` was write-only: `(vec-get v 0)` was typed `int` and `type-of` on it
was a compile error. Two changes at the two ends of the same value, and neither
was where the residue report guessed:

- `call_result_type` collapses a bare-tyvar result to the int64 carrier and
  records a reinterpret. Right for a scalar, impossible for an `any`: it is the
  TWO-word `tur_tagged_t`, so there is no single carrier word to bitcast back
  from, and the collapse kept the payload and dropped the tag. `any` and `union`
  join the composites that comment already exempts, for the reason it already
  gives -- "a reinterpret cannot carry a composite anyway".
- The reader end. A `(Vec any)` element is stored BOXED, so a generic `: A`
  accessor hands back the slot word; `TUR_GETTAG` on an `int64_t` is a hard cc
  error. The `any` readers bridge a carrier-form operand back to the aggregate,
  keyed on the value's recorded emitted spelling so every other shape is
  untouched.

Two probes were wrong before that: the element-read recovery in `emit_expr.c`
looked like the site, and instrumenting it showed the WORKING `(Vec Pt)` case
takes the identical path. Those edits were reverted rather than shipped as dead
code with a confident comment.

**The interpreter diverged for exactly one commit, and now agrees.** Its Vec
buffer is raw int64 cells and it recorded ONE tag per vector --
`native_vec_push` called it "the homogeneous element tag" -- so every element of
a heterogeneous vector reported the LAST one's type: a wrong answer with no
diagnostic. Right for as long as the type system enforced homogeneity, and
`(Vec any)` is the first element type for which it is not. The side table now
carries a byte per element ([archived](../archive/vec-any-interp-keeps-one-element-tag.md)),
and `vec-any-element-roundtrip` runs on both back ends. It carried
`requires.compiled` for one commit, to record the divergence rather than hide it
behind a skip nobody reads.

**Map and Set are now settled, and the answer changes S6's order.** Measured on
both back ends
([map-of-any-is-broken-on-both-back-ends](../archive/map-of-any-is-broken-on-both-back-ends.md)):

| container | `--interpret` | compiled |
|---|---|---|
| `(Vec any)` | correct | correct |
| `(Map int any)` | ~~`int` for every value~~ **fixed** | ~~cc error~~ **fixed** |
| `(Set any)` | clean diagnostic | clean diagnostic |

**Both Map halves are now fixed (2026-09-08).** Map was NOT Vec's problem again
with a different table -- it had no tag side table at all, and the Vec analogue
does not transfer (that table keys on an element INDEX a persistent trie does
not expose). It was the same problem one step earlier: the TAG was dropped at
the STORE.

And the fix was much smaller than "it touches every map operation that reads a
value" predicted, because **the HAMT already had the mechanism and the compiled
path already used it**. `Hamt` carries a `val_owned` flag selected by bit 1 of
the same `owned` word the `_eq_o` operations already thread (bit 0 = key,
bit 1 = value); the runtime then retains the box on structural copy, releases it
when the entry dies, and stamps `val_owned` on the result. So the persistence
problem that ruled out a side table is solved by the trie itself, and a reader
asks the map rather than a table. Five sites, all in `collections_native.c`.

The boxing is CONDITIONAL -- an int-valued map is byte-identical to before, and
a map boxes from its first non-int value -- which confined the change to the
maps that were broken. The one shape that needed care is a map that has already
stored raw int carriers and is then handed a string: boxing only the new value
leaves it HALF-boxed, so the first non-int store rebuilds the map with every
value boxed, once per lineage, into a new map that leaves the persistent
original raw and readable. `#map{:a 1 :b "two"}` is exactly that shape;
`tests/fixtures/map-any-value-upgrade` pins it.

The compiled half was small for a reason worth carrying forward: the STORE side
was already right, because `repr_of` answers `REPR_BOXED_AGG` for an `any` at a
container-element position and the HAMT assoc boxes accordingly. Only the read
was missing its deref. So the container work has one shared decision (`repr_of`
at `CONTAINER_ELEM`) and per-container read plumbing, which is the shape to
expect for `#set{...}` and cons lists too.

Asking early was the right call twice over: the interpreter half was a SILENT
wrong answer, and it would otherwise have been found by a user writing
`#map{:a 1 :b "two"}` -- with the widen already landed, so the literal would
have looked like the culprit.

Still to do, and the ORDER is now measured rather than assumed:

1. ~~**The `vec-of` clone-selection bug**~~ -- **DONE 2026-09-08.** It did BLOCK
   the headline feature rather than sit beside it (`[1 "two" 7.1]` lowers to
   `(vec-of ...)`, and a single `vec-of` at `any` tripped run.sh's emitted-C
   ratchet), and the cause was not where four rounds of reading the call sites
   put it. Not the cross-spec fallback in `emit_call_name`, which took a sibling
   clone only because there was nothing else to take: the `any` clone of
   `vec-empty-like__` never interned a `vec-new` monomorph at all, because
   `emit_abi_register_call`'s family-element rehydration admits a scalar layout
   or a concrete ADT app and `any` is neither. One disjunct.
   [Archived](../archive/vec-any-monomorph-is-half-plumbed.md);
   `tests/fixtures/vec-of-any-heterogeneous` pins a heterogeneous `vec-of`
   round-tripping `int`/`cstr`/`float`, leak-clean.
2. ~~**G7 itself**~~ -- **the VECTOR half is DONE 2026-09-08.** `[...]` in a
   Saffron file widens each element to `any` at the LITERAL (one helper in
   `elab_toplevel.c`'s `F_VEC` case), before `vec-of` sees it -- which keeps
   that macro's `tur-vec-homog__` homogeneity check intact and satisfies it at
   `any`, rather than teaching the macro a dialect-dependent second mode. The
   widen is unconditional per file, so `[1 2 3]` is a `(Vec any)` too:
   `tests/fixtures/saffron-vector-literal-homogeneous` pushes a string into one
   and reads it back. Both fixtures leak-clean, both back ends agree.
3. ~~The interpreter's Map value tag~~ -- **DONE 2026-09-08.** It moved AHEAD of
   `#map{...}` on measurement, not on preference: `#map{:a 1 :b "two"}` fails
   identically to the vector literal did (`tur-map-homog__` on the VALUE side),
   so the same widen was the obvious next step -- but a `(Map K any)` under
   `--interpret` reported `int` for every value with no diagnostic, so doing the
   widen first would have made every Saffron map literal a silent wrong answer
   on one back end. That is precisely the trap the Vec case set, and the reason
   the report was filed to check Map and Set BEFORE the literal work rather than
   after. Sequencing it this way is what let the `#map{}` widen land next
   against two agreeing back ends. [Archived](../archive/map-of-any-is-broken-on-both-back-ends.md).
4. ~~The `#map{...}` / `#set{...}` twins~~ -- **DONE 2026-09-08**, and `#set{}`
   at first needed NO change -- a decision REVERSED 2026-09-09, recorded here
   because the earlier text explained why the widen would break it.
   - `#map{...}` takes the same widen as `[...]`, on the VALUES only. The KEYS
     are already normalized to one key type by the lowering above the widen.
     (**Changed 2026-09-26: the keys widen too**, raw, so a Saffron map literal
     is `(Map any any)` like `(map-new)`. A `(Map Sym any)` literal behind an
     `any` could satisfy no seam, which can only ground an open key to `any`
     -- see
     [saffron-open-generic-result-not-grounded](../archive/saffron-open-generic-result-not-grounded.md).)
     `tests/fixtures/saffron-map-literal`. (A heterogeneous key needs
     `Hash[any]`/`MapKey[any]`, which now exist -- see the set row -- but the
     key normalization still runs first.  Keying a map by `any` works since
     2026-09-10: the instances gained a Sym arm and an `any` key agrees with
     the typed key on hash, carrier and comparator -- saffron-dynamic-surface-
     pass H3/H4.)
   - `#set{...}` -- **widens like `[...]` since 2026-09-09.** It had worked
     heterogeneously WITHOUT a widen only because `set-of` had no homogeneity
     check: each element resolved its own `hash`/`mk-box`/`mk-cmp` and the set
     claimed `(Set int)` while holding a cstr (`set-of-element-type-is-not-
     checked`, now archived). Resolving that gave `set-of` the `vec-of`-style
     typed insert (`set-add-elem__`), which made the widen NECESSARY, and
     `Hash[any]`/`MapKey[any]` (each `any` hashes and keys by its payload; the
     HAMT keeps a comparator per entry, which is what makes mixing them sound),
     which made it POSSIBLE. `tests/fixtures/saffron-set-literal` now pins the
     widened literal with byte-identical behaviour; `set-of-any-elements` and
     `errors/set-of-heterogeneous` pin the plain-Turmeric halves.
5. ~~Cons lists~~ -- **DONE 2026-09-08.** `(list 1 "two" 7.1)` hit the third of
   the three homogeneity checks (`tur-list-homog__`) and takes the same widen.
   It is a CALL rather than a reader literal, so the hook is in `elab_form`'s
   F_LIST case rather than beside the data literals.

   One difference from Vec and Map, recorded because it changes which idiom to
   reach for: `Cons` is `(defstruct Cons :heap [A] (head A) (tail :int))`, so
   the TAIL is an erased carrier. `.tail` hands back an `:int` and the element
   type is gone one step in; `(:: (.tail l) (Cons any))` recovers it and the
   next `.head` reads its own tag. So the widen makes a Saffron cons list
   heterogeneous, but WALKING one still costs an ascription per step -- which is
   why a `defdata` with `any` in both slots (what `saffron-higher-order` uses)
   stays the better idiom for a list you actually walk.
   `tests/fixtures/saffron-cons-list`.
6. ~~The prelude~~ -- **DONE 2026-09-08.** `stdlib/saffron/prelude.tur`, with
   `vec-map` / `vec-filter` / `vec-fold` over `(Vec any)`, auto-loaded for a
   `#lang saffron` ENTRY file on BOTH back ends.

   Three things it settled that were not obvious in advance:

   - **Scope is the entry file, and the flag is self-resetting.** A Saffron file
     imported by a Turmeric program does not drag the prelude in, and a Turmeric
     file checked in the same process right after a Saffron one does not see it
     either -- the flag is set on every dialect detection, true or false, rather
     than only when true. Same hazard `g_trail_autoloaded` records.
   - **The prelude is `#lang saffron` itself.** Its own bodies use D4
     truthiness, a hard "if condition must be bool, got any" under Turmeric.
     Rather than special-case the filename in the two loaders, the compiled
     autoload path learned to run `#lang` detection on a stdlib file -- which
     the interpreter's `(load ...)` already did -- so the file says what it is
     and neither call site has to remember.
   - **Writing it found the fourth and fifth `any`-carrier straddles**, in one
     function: a `let` binding declared `any`, and an `any` slot of a
     fat-closure dispatch. It also found why the earlier consumer bridges kept
     declining on the values they exist for -- a hoisted CALL temp was never in
     the carrier-representation side table. See
     [any-carrier-straddle-is-bridged-per-consumer](../archive/any-carrier-straddle-is-bridged-per-consumer.md),
     which now records the measured blocker on the general fix.

   Held to R5's line: these are adaptors, present only because a `(Vec any)`
   cannot be handed to a `[A B] [v (Vec A) f (fn [A] B)]` signature without
   naming A and B. The dynamic `assoc`/`get` and the truthiness helpers the
   stage text also lists are NOT here: `map-get`/`map-assoc` already work on a
   `(Map K any)` from Saffron, and D4 truthiness is a compiler rule rather than
   a function, so neither needs an adaptor. Adding one for its own sake is the
   wedge R5 warns about.

### S6 -- containers and the Saffron prelude (medium) -- scope, all landed

G7. `(vec any)` becomes the default container element in Saffron, so
`[1 "two" 7.1]` is a vector of three boxes. Same for `#map{...}` values, cons
lists -- and, since 2026-09-09, `#set{...}` too. The set had first measured OUT
of this list, but for a bad reason: it only built heterogeneously because
`set-of` skipped the homogeneity check its siblings have, lying about its
element type. With `set-of` checked like `vec-of` and `Hash[any]`/`MapKey[any]`
in the stdlib, the literal widens to an honest `(Set any)` and behaves exactly
as before (`tests/fixtures/saffron-set-literal`, item 4 above).

**Exit: MET 2026-09-08.** Every literal, the unannotated `main`, and the
prelude. Fixtures: `saffron-vector-literal`,
`saffron-vector-literal-homogeneous`, `saffron-map-literal`,
`saffron-set-literal`, `saffron-cons-list`, `saffron-unannotated-main`,
`saffron-dyn-ops-on-vec-elements`, `saffron-prelude`.

**Both blockers on this stage are fixed.** The repr-decision ICE (2026-09-07):
a `(Vec any)` builds, stores and frees correctly, with its elements boxed one
word wide and released with the vector. Then the two halves of
[vec-any-monomorph-is-half-plumbed](../archive/vec-any-monomorph-is-half-plumbed.md)
(2026-09-07 and 2026-09-08): `vec-get` on a `(Vec any)` reports each element's
own type rather than `int`, and a `vec-of` producing one clears run.sh's
emitted-C ratchet. So the answer to this stage's central question -- how does an
element type reach a read through a container -- exists for `Vec`, and the same
question governs `#map{...}`, `#set{...}` and cons lists; answer it once from
the `Vec` shape rather than per container.

A `stdlib/saffron/prelude.tur` autoloads for `LANG_SAFFRON` files: the dynamic
`map`/`filter`/`fold`/`reduce`/`assoc`/`get`, the truthiness helpers, and thin
`any`-taking wrappers over the typed stdlib entry points a dynamic program
reaches for constantly. This is where Saffron stops feeling like Turmeric with
the types removed and starts feeling like its own thing.

### S7 -- the boundary (medium) -- DONE 2026-09-09

D5's implicit checked `cast` at each Saffron -> Turmeric argument whose
parameter is concrete and whose argument is `any`. Fixtures for both
directions, including the panic path.

**Both directions now have coverage, and the reverse one was BROKEN, not
merely unpinned.** D5 said a Turmeric module importing a Saffron one "sees
`any`-typed exports and must narrow them ... nothing new is needed" -- which
assumed the import worked. It did not: the import path (`elab_module.c`)
hardcoded `READER_TURMERIC` and never ran `#lang` detection, so
`(import dyn)` on a `#lang saffron` module was
`unexpected character '#' (0x23)`. A Saffron module could not be imported at
all. The `(load ...)` path already detected it -- the same split that let
`tur fmt` reject every `#lang` file while every other entry point accepted
them, found the same way and fixed the same way.
`tests/run-saffron-import.sh` (ctest `tur_saffron_import`) pins it on both back
ends; `tests/fixtures/saffron-boundary-check` and `saffron-boundary-panic` pin
the forward direction and its panic path.

**The HIGH-severity defect that blocked this stage is FIXED (2026-09-08)** --
[archived](../archive/saffron-unannotated-param-container-cast-panics.md).
`(defn pick [v] (vec-get v 0))` panicked compiled and worked interpreted,
because the seam's guard exempts a bare `TY_TYVAR` expected type but not a
`TY_APP` whose ARGUMENT is a tyvar. Two changes were needed and neither works
alone: ground the target's open type arguments to `any`, AND re-collect the
call's tyvar bindings after the seam substitutes the argument -- without the
second the result collapses to the int64 carrier and the return widens the box
POINTER, which is a WRONG ANSWER rather than a panic. That is why the first
attempt at this was reverted. The fixtures assert VALUES on both back ends, not
the absence of a panic.

**`:strict` is retired (2026-09-09).** It appeared twice in this plan and was
never defined. The analysis narrowed it to either a vestige of a draft in which
the boundary check was optional -- superseded by D5's verdict -- or an unwritten
Saffron-as-linter opt-in; the author confirmed the first. Nothing was built for
it and nothing is owed. S7 is complete.

**The HAND-WRITTEN spelling is settled ahead of it (2026-09-08), and it is the
same operation.** `::` now refuses an `any` operand and names `cast`
([archived](../archive/any-narrowing-ascription-does-not-compile.md)); before
that it was four different `cc` errors compiled and four wrong answers
interpreted, plus a fifth behaviour for a union target that type-checked and
silently did not narrow. So a Saffron author who writes the narrowing by hand
gets pointed at exactly the node D5's boundary inserts, rather than at a
spelling that looks like it should work. Ascribing an `any` TO `any` stays legal
-- it is identity, and the S6 data-literal widen emits it.

### S8 -- tooling (medium, parallelisable) -- DONE 2026-09-09

`tur fmt` / `tur format` (the formatter reads `reader_type`; it needs to
preserve a `#lang saffron` line and not add annotations), `tur lsp` and
`lsp-lite` (a Saffron file must not report `any` everywhere as an error),
`tur repl --lang saffron`, `tur init --saffron`, `tools/gendocs.py`
(docstrings without types), the vim/vscode syntax packs, and a
`docs/guides/saffron-guide.md`.

`tur fmt` is DONE (2026-09-08): it preserves a `#lang` header verbatim,
formats the body, and normalises to one newline.

**`tur lsp` / `lsp-lite` needed no work -- MEASURED 2026-09-08, not assumed.**
The stated concern was that "a Saffron file must not report `any` everywhere as
an error". It does not: driving a live server, a valid Saffron buffer publishes
ZERO diagnostics and a broken one reports `unknown function or operator`,
identically to the Turmeric buffers beside them. The LSP writes the buffer to a
temp `.tur` file and runs a full compile, and `#lang` detection is
content-based, so a Saffron buffer is analysed as Saffron for free.

That is true by INHERITANCE rather than by design -- the LSP never mentions the
dialect -- and two other entry points in this codebase have already been caught
hardcoding `READER_TURMERIC` and breaking on `#lang` (`tur fmt`, and the module
import path in S7), each found only when someone tripped over it. So it is now
pinned: `tests/lsp/saffron-diagnostics.py` (ctest `lsp_saffron_diagnostics`).

The test asserts a BROKEN buffer reports before it believes a clean one, and
that control is load-bearing: diagnostics publish asynchronously, so a session
that exits before analysis runs reports nothing, which is indistinguishable
from a clean file. The first version of this probe did exactly that and would
have concluded the Saffron path was fine on no evidence. Verified to fail
against a stub server that publishes nothing.

**`docs/guides/saffron-guide.md` is DONE (2026-09-08)**, with every example
compiled and run by both suites (`tests/fixtures/docs-saffron-guide-examples`),
and its three error claims pinned separately
(`saffron-guide-refinement-violated`, `errors/saffron-guide-unnarrowed-method`,
`errors/saffron-region-bracket-refused`). The `any` guide's examples rotted once
because nothing compiled them; this one is pinned from the start.

Writing it was not a formality. Drafting the examples found a **build breaker**
-- the container seam emitted C that GCC >= 14 rejects, so an unannotated
container parameter did not build on a modern toolchain
(`saffron-container-param-cast-shape`) -- and corrected three claims that would
otherwise have shipped wrong: `type-of` on a literal (the `any` default is on
PARAMETERS, not expressions), `+` on strings (numeric only, it panics), and a
refinement violation being STATIC when the value is statically known rather
than the runtime panic the first draft described. Examples a reader would write
exercise shapes the 2897-fixture corpus does not.

**`tur repl --lang saffron` is DONE (2026-09-08)**, and it turned out to be a
fix as well as a feature. Typing `#lang saffron` at the prompt -- the guide's
first line -- was ACCEPTED AND SILENTLY IGNORED: the handler read the line with
`detect_lang_layered`, which reports only the READER axis, so `saffron` came
back as plain `turmeric`, the "already set" early-out fired, and the language
half was dropped. The session kept Turmeric's `int` parameter default while
telling the user nothing. Both routes now go through one env switch that
carries the dialect. Pinned by `tests/turi/repl-lang-saffron.sh` (ctest
`tur_repl_lang_saffron`), whose probe is a float add through an unannotated
defn -- `(add 1 2)` is 3 under either dialect and would pin nothing.

That work also surfaced `cli-usage-error-paths-exit-zero`: an unknown flag is
reported and then exits 0, codebase-wide, so `tur build --typo || exit 1`
succeeds. Filed rather than fixed here -- it is every subcommand, not this one.
(Fixed 2026-09-09: every error path exits 2 via `usage_error`; archived.)

**`tur init --saffron` is DONE (2026-09-08)**, and it found the FOURTH entry
point hardcoding `READER_TURMERIC`. `load_project_prelude` (main.c) built its
SourceFile with no `#lang` detection, so a scaffolded Saffron LIBRARY died on
the autoloaded prelude's own first line -- `unexpected character '#'` -- while
the BINARY scaffold built fine, because that path goes through a different
loader. Same defect as `tur fmt`, the module import path (S7) and the
single-file autoload (S6); same one-block fix. Pinned by
`tests/run-init-saffron.sh` (ctest `tur_init_saffron`), which asserts the
scaffold BUILDS AND RUNS rather than that it contains a `#lang` line -- the
text check would have passed against the broken library path. Verified to fail
on exactly that case with the detection disabled.

**`tools/gendocs.py` needed NO WORK -- measured 2026-09-08.** The concern was
"docstrings without types"; a `#lang saffron` module's untyped parameters
already come back as `('a', None)`, which is correct, and the `#lang` line does
not disturb the parser. Verified by loading `parse_tur_file` directly (gendocs
cannot run without the `markdown` package, which is unavailable here).

Checking it did turn up an unrelated defect in that parser, filed as
`gendocs-misparses-the-spaced-annotation-form`: the SPACED `[a : int]` form
yields a phantom parameter named `int` and a return type of `':'`, affecting
1064 of 1999 stdlib definitions. Latent -- `docs/api/` is not checked in -- but
it would corrupt the reference on the next `tur run docs`. (Fixed 2026-09-09:
both spellings parse alike, 0 of 2002 affected; archived.)

**The syntax packs are DONE (2026-09-08), and the item was mis-scoped.** It
reads as though packs exist and need a Saffron update. They did not exist:
`editors/vscode-turmeric` is a DEBUGGER extension that declares the `turmeric`
language but contributes no grammar (`grammars: 0`), and there was no vim pack
at all -- no `.tmLanguage`, no `.vim`, no `syntax/` anywhere in the tree. So
there was no syntax highlighting for Turmeric in any editor, let alone Saffron.

Both now exist: a TextMate grammar wired into the existing extension, and a
standard `syntax/` + `ftdetect/` + `ftplugin/` vim pack. Each covers the
`#lang` line with its base highlighted distinctly, `;;;` docstrings, the
definition forms, BOTH annotation spellings, parameter attributes, effect rows,
reader dispatch, and inline C as embedded.

Pinned by `tests/run-editor-syntax.sh` (ctest `tur_editor_syntax`), and the two
halves have deliberately unequal assurance, which the script states: the vim
pack is LOADED IN A REAL VIM and queried with
`synIDattr(synIDtrans(synID(l, c, 1)))`, while the vscode half is JSON validity
plus regex checks, since no TextMate engine is available here.

Loading it for real earned its keep twice. The `\b` terminator on the
special-form rule silently matched NOTHING for any form ending in `?` or `!`
(`is?`, `set!`) -- there is no word boundary after a non-word character -- and
rule ORDER made the fused `y :float` highlight as a keyword literal while the
spaced `y : float` highlighted as a type, because Vim gives a later-defined
item priority. Both were found by measurement, and the harness was verified to
fail on the second when reintroduced.

**S8 is complete.** The next stage is S9.

### S9 -- runtime typeclass dispatch (D8) -- BUILT 2026-09-09

D8's prerequisites are all met (see its verdict above): P1 built the runtime
type registry instance lookup keys off, P2 guarded the by-value receiver, and
the monomorphic/parametric type-case asymmetry is closed. The five pieces are
listed there; four have a home already.

**Scheduled after S8 deliberately.** D8's remaining questions -- superclass
chains, default methods, HKT receivers, two instances matching one tag -- are
the ones the measurement could not answer, and the plan's own position is that
they want evidence from real Saffron programs. S8 is what makes those programs
writable. Building the dispatch first would mean deciding those four questions
up front, which is what this stage was deferred to avoid.

Exit: a `(show x)` on an un-narrowed `any` dispatches on the box tag, on both
back ends, with a clean "no instance for T" panic; and the four open questions
answered from programs rather than from first principles.

#### S9 -- built 2026-09-09 (pieces 3-5)

The exit condition's first half is **met**: `saffron-dyn-typeclass-dispatch`
runs the int, float, cstr and bool instances of one class through one
`(.kind-of x)` on both back ends, and a type with no instance panics naming
both. Typed Turmeric keeps the diagnostic -- the dialects reach the same
resolution state and choose differently, which is the design, and
`errors/saffron-guide-unnarrowed-method` moved to the Turmeric arm to pin it.

What the build changed relative to the sketch:

- **The pre-pass is the ABI scan**, not a new pass. `emit_abi_note_instance_dict_ref`
  already marked an instance live with dict and bodies in lockstep; forcing
  emission is new NOTES in a walk that already runs, keyed on the widened tag
  collected at `EX_UNION_INJECT`.
- **The row points at a per-instance SHIM TABLE, not the dict.** The dict's
  slots are not uniformly shaped, so one cast at a dynamic site would pass a
  double in the wrong register class and read a bool out of a 64-bit word --
  P8's truncation one layer up. Per-instance shims convert instead, the float
  arm bit-reinterpreting.
- **A scan gap fell out**, and it was a real bug of its own:
  `emit_abi_scan_expr` had no arms for `EX_DYN_OP`/`EX_DYN_CALL`/`EX_DYN_FIELD`,
  so nothing under `(println ...)` was ever walked -- every widen and every
  monomorph request beneath one went unrecorded. Exactly what P2d fixed for
  `EX_UNION_INJECT`, one node family later.

**v0 limits, each a clean panic rather than a wrong answer**: a method taking
more than the receiver (`eq [x : a y : a]`) or returning the class's own type
variable (`clone : a -> a`); an instance for a type CONSTRUCTOR
(`Functor Option`), since a widened value's tag is minted from `(Option float)`;
and a type-variable receiver (`Clone T`), which has no ground tag.

(**Status 2026-09-26.** The first two are lifted: saffron-dynamic-surface-pass
M9 gave extra-argument and class-variable-result methods a witness on a
primitive or non-parametric receiver, and 2026-09-26 extended it to a
parametric head at its all-`any` instantiation (`.eq?` on an `any` holding a
Vec -- see Q1). The type-constructor case is Q3, built. The type-variable
receiver was never a limit in practice: the tag axis only registers instances
whose tag was widened somewhere, so it is simply never registered. The
"clean panic rather than a wrong answer" claim was NOT true of the constrained
instance, which answered wrongly -- see Q1.)

#### The four design questions -- answered 2026-09-09

Each was measured against a real program, and three of the four turned out
not to be dynamic-dispatch questions at all.

**Q1 -- superclass chains.** This language has no `defclass` superclass
declaration; the shape the question meant is the CONSTRAINED instance,
`(definstance Show [Vec] [(Show A)] ...)`. Such an instance is
parametric-headed, so it is the same gap as Q3, plus one more thing on top: the
instance body dispatches `(show (:: x A))` on each element through the
constraint's dictionary, and under dynamic dispatch with `A = any` that
dictionary would itself have to dispatch dynamically -- a synthesised
`dict_Show_any` whose slot does the registry lookup on the element's tag. That
is the genuinely open design item, and it sits behind Q3. The ground case
(`.show` on an `any` holding a float) dispatches correctly today; its
`println` panic was Saffron's dynamic print not knowing `String`, not the
dispatch.

**Q1 BUILT 2026-09-26 -- and it was a wrong answer, not a limit.** Measured
before building anything, with a user `Kind [Vec] [(Kind A)]` that answers
with its first element's kind and the stdlib's own `Eq [Vec]`:

| route | compiled | interpreted |
|---|---|---|
| dynamic -- `(.kind-of x)`, `x` an `any` holding `[7.25 1]` | `int` (the representative) | `int` (the whole vector sent to `Kind [int]`) |
| dynamic -- `.eq?` on two such vectors | panic: no dynamic slot | `false` for two equal vectors |
| static -- `(kind-of v)` / `(.eq? a b)`, `v : (Vec any)` | TUR-E0020 "matches 21 instances ... receiver type is erased" | same |

Four defects, fixed in turn:

- **The `C [any]` dictionary is minted.** When a constrained instance meets
  `A = any` in a dynamic file -- at the static constraint check, or at a
  dynamic dispatch site for its class -- the elaborator synthesises
  `(definstance C [any] (m [__x ...] (.m __x ...)) ...)` in that span
  (`saffron_mint_dyn_any_instance`). It is stamped `dyn_any_minted`, so an
  `any` receiver never resolves to it statically (its own body would call
  itself); the body's `.m` is an `EX_DYN_METHOD`. A method with extras casts
  the witness's `any` back to the class's declared result. Kind-* classes
  only, and never when a hand-written `C [any]` exists (`Hash`, `MapKey`).
  An `[any]` head also stopped matching every non-primitive receiver as a
  wildcard, so a `(Vec any)` reaches `C [Vec]` rather than the newest
  `[any]` instance.
- **The compiled row calls the `(Vec any)` spec, not the carrier base.** A
  direct shim called the base impl, whose element call is the elaborator's
  representative. A constrained instance on a parametric head now gets a
  witness at `(Head any..)` -- returning the declared result for a
  one-parameter method, so the slot keeps the direct shims' signature -- and
  the element call in that spec re-resolves to `C [any]`. The argument bridge
  reads the element box through its carrier word (`__tur_any_of_carrier`)
  only when the argument's emitted spelling IS a word: a `(Result any any)`
  match binder and a `vec-get` temp have the same static type and different
  C types. The M9 witness now also covers a parametric head, casting a
  class-variable extra to `(Head any..)` as a typed call would.
- **The interpreter named a collection box by its handle.**
  `turi_any_display_type` unwrapped every box and answered `int` for a Vec
  (H5 had special-cased only Sym). The box's name is the answer for every box.
- **The interpreter's dynamic entry bound no dictionaries.** A constrained
  instance entered through `EX_DYN_METHOD` now gets a parent frame pinning
  each constraint variable to `any`, which binds the minted dictionary, and
  boxed extras are unwrapped as the receiver already was.

Pinned by `saffron-dyn-constrained-instance` and `saffron-eq-vec-any` on both
back ends. A typed file mints nothing -- it has no registry to dispatch
through -- and its `(Vec any)` case now says what is wrong: TUR-E0015, "no
instance of typeclass 'Kind' applies to type", instead of the ambiguity
(`errors/typeclass-constraint-unsatisfied-vec-any`).

**Q2 -- default methods: WORK, 2026-09-09.** They could not be measured at
first because default bodies did not work under STATIC dispatch either (the
typeclass guide's own example failed three ways). Fixed -- the default is kept
as a form on the class and spliced in per instance
([archived](../archive/typeclass-default-methods-do-not-work.md)) -- and the
dynamic case then held by construction: the shim table iterates
`method_impls[i]`, and a default-filled slot is a shim like any other. Pinned
by `saffron-dyn-default-method` on both back ends.

**Q3 -- HKT receivers: keyed on the head, BUILT 2026-09-09.** `(.fmap o (fn
[x] (+ x 1)))` on an `any` holding `(some 41)` answers 42 on both back ends,
pinned by `saffron-dyn-hkt-dispatch` (with a 7.35 row and a `(none)` row).
The design held, with one correction and two additions found by building it:

- The row is keyed on the all-`any` instantiation, `(Option any)`, and points
  at a WITNESS defn the elaborator synthesises in the dispatch site's Saffron
  span -- `(defn __dynwit_Functor_fmap_Option [__r : (Option any) __a1] : any
  (.fmap __r (cast __a1 (fn [any] any))))` -- so the static path mints the
  by-value spec for that instantiation, the D5 seams do the typing, and the
  `: any` return re-tags the result. The emitter writes a two-line C shim from
  the receiver word to the witness. Never the carrier base, which reads an
  `(Option any)`'s 16-byte element as an int64.
- **Correction:** the plan said "Saffron builds every value at the all-`any`
  instantiation". It did not. `(some 41)` is a stdlib FUNCTION with a bare
  type-variable parameter, not a raw ctor, and built an `(Option int)`; `(none)`
  has no parameter at all and left `A` open. Both are now `(Option any)`, by
  the one rule stated three ways: a bare-tyvar parameter widens to `any` when
  no compound parameter of the callee can pin its variable (`unwrap-or`'s `d :
  A` keeps its type because `o : (Option A)` pins `A` -- the guide's own
  type-case example broke before that refinement); a still-open RESULT variable
  binds to `any` before the result is instantiated, so the binding reaches the
  emitter as an abi_binding and the `(Option any)` spec is minted; and both,
  like the ctor widen, defer to an enclosing ascription that pins the
  instantiation on purpose.
- **Arity is a stated limit.** Neither the class (`[container g]`, unannotated)
  nor the impl (an inferred `g` records as the erased fn carrier) knows `g`'s
  arity, so the witness casts an erased-fn extra to `(fn [any] any)` -- what a
  Saffron lambda is -- and a closure of another arity panics at the checked
  cast rather than being called wrongly. Binary-fn methods (Foldable's) are
  out until the class records arity. (**Corrected 2026-09-26:** the class
  DOES record it when it spells the parameter -- Foldable's `fn : (fn [b a]
  b)` -- and the witness now casts to that arity. Only an unannotated class
  parameter is still taken as unary. See S9's open list below.)
- **Exposed on the way:** the witness's first draft passed its `any` parameter
  straight into `fmap`, and the rank-2 path wrapped that LOCAL in a file-scope
  thunk that calls it by name (`'f' undeclared`). Plain Turmeric, no Saffron;
  filed as
  [local-fn-value-into-rank2-slot-gets-a-by-name-wrapper](../archive/local-fn-value-into-rank2-slot-gets-a-by-name-wrapper.md).
  The cast form sidesteps it.

The poly-fn `any`-parameter blocker named in the previous version of this
paragraph is fixed and archived.

**Q4 -- two instances matching one tag.** Cannot happen, and not because the
tag is per instantiation: a second `definstance` for the same `(class, type)`
is dropped at definition by the idempotent re-instance guard, first definition
wins, silently. Static resolution, the interpreter's walk and S9's registry all
agree because there is only ever one instance to find. What that exposed is
that a USER instance for a stdlib-covered type is a silent no-op, against the
file's own stated intent that user instances shadow stdlib ones. Filed as
[duplicate-instance-silently-drops-a-user-definstance](../archive/duplicate-instance-silently-drops-a-user-definstance.md);
the decision (replace or error) is a language one and S9 inherits it. **The
warning landed 2026-09-09** -- a user-file definstance the guard drops is now
told so -- which removes the silence without pre-empting that decision.
(**Decided 2026-09-11: reject.** An exact duplicate from a non-stdlib file is
TUR-E0025, the conventional overlapping-instance rule, so there is still only
ever one instance to find.)

One robustness note from Q1: `EX_DYN_METHOD` takes its result type from the
instance the static resolver happened to select rather than from the class
declaration (`TypeClassMethod.return_type`). It was correct in every measured
case -- for `Show` the instance's `void *` IS `String` -- but the class is the
right source, since it is what makes one slot callable through one signature.
(**Done 2026-09-26** for a concrete declared result; an applied class
variable is still open -- see below.)

#### S9 -- what is still open

Measured 2026-09-26, after Q1 landed; updated the same day as items closed.
What remains is two stated limits on UNANNOTATED class parameters -- the
fn-arity default (unary) below and the parametric-head extra last in the
list -- each a clean panic at a checked cast, never a silent wrong answer:

- ~~**Binary-fn methods (Foldable's).**~~ **Fixed 2026-09-26.** The impl
  never records a function argument's arity, but the class does when it
  spells the parameter (Foldable's `fn : (fn [b a] b)`), so
  `saffron_mint_dyn_witness` casts the erased-fn extra to `(fn [any any]
  any)` from the declaration. An unannotated class parameter (`[container
  g]`) is still taken as unary. Two compiled defects sat behind the cast: the
  witness's spec at `b := any` re-widened `init` -- already the box -- as the
  payload of another box (a widen of an operand that resolves to `any` is now
  the identity, `EX_UNION_INJECT`), and loading `stdlib/rc.tur` minted a
  `Hash [any]` whose dynamic row cast the receiver word to `tur_tagged_t` (a
  box is never tagged `any`, so `emit_instance_dispatch_recv_type` declines
  it). Pinned by `saffron-dyn-binary-fn-method`, a user `Foldable [Two]`.
- **The result type's source.** ~~The robustness note above still holds.~~
  **Done 2026-09-26 for a concrete declared result**: `EX_DYN_METHOD` reads
  it from the class (a class method must declare one). Building that exposed
  a plain-Turmeric defect: an instance body was never widened to a declared
  `: any` result, so `(size [x] 3)` under `(size [x : a] : any)` returned a
  raw word from a `tur_tagged_t` function (cc error), now widened as a
  defn's is (`typeclass-any-result-instance-widens`,
  `saffron-dyn-any-result-method`). ~~**Still open:** a result that mentions
  the class variable APPLIED (`: (Option a)`) takes the picked instance's
  type.~~ **Fixed 2026-09-26**
  ([saffron-applied-class-var-result-takes-one-instances-type](../archive/saffron-applied-class-var-result-takes-one-instances-type.md)):
  pass 1 of `definstance` now substitutes the class variable through the
  application (`Wrap [Pt]` returns `(Option Pt)`, not an open `(Option a)`
  that lowered to the carrier), the body sees that result as its expected
  type, and the dispatch goes through a per-instance `any` witness. On the
  way: a Saffron `[..]` / `#set{..}` / `#map{..}` literal now builds the
  instantiation a declared return or ascription pins, instead of a `(Vec
  any)` the expectation typed `(Vec float)` (a silent wrong answer in a plain
  defn). The class must spell the receiver -- `(wrap-self [x : a] : (Option
  a))`. With `[x]` unannotated the method is return-directed by the typed
  rule and `x` stays `int`, so dispatching it on an `any` is now TUR-E0020
  with that hint (it silently mis-tagged before; typed code refuses the
  static form too). A body building the wrong instantiation is a type error
  at the body. The interpreter's dynamic arm reads the class
  declaration for one thing -- turning an inline-C `bool` result that came
  back as an int into a bool -- and boxes a collection result under the
  impl's declared type when the node is `any`.
- ~~**An empty literal is not `(Vec any)`.**~~ **Fixed 2026-09-26.** `[]`
  expands to a `(vec-new)` carrying the stdlib's span, so the open-result rule
  (which gated on the call's span) left it an open `(Vec A)` whose box matched
  no registry row. The rule now also consults the top-level form's dialect.
  Fixed with
  [saffron-open-generic-result-not-grounded](../archive/saffron-open-generic-result-not-grounded.md),
  whose other three defects were in the same family.
- **An unannotated extra on a parametric head is read as the class
  variable.** `Eq`'s `(eq? [x y])` records `int` in both the class and the
  impl, which is what a typed `(.eq? v w)` hands a `(Vec any)` to; so the
  parametric witness casts such an extra to `(Head any..)`. A genuinely
  int-typed extra on a parametric head is indistinguishable and panics at
  that cast.

Not S9's, but met on the way: interpreted `Eq [cstr]` is inline C, which the
interpreter does not run (TI7), so `(eq? "x" "y")` interprets to `1` in any
dialect.

---

## 6. Carve-outs

Permanent. These are not "later" -- they are the honest answer to "what does
Saffron give up".

**Four rows here were wrong until 2026-09-08 and are now corrected.** The first
draft assumed that any feature carrying a static guarantee must be given up in
a dialect whose default type is `any`. Measured -- the same violating program in
a Saffron file and a Turmeric file, diagnostics compared -- only ONE is. The
distinction is whether a proof reads an INFERRED type (regions do) or an
ANNOTATION / a walk over uses and scopes (everything else does), and D2 keeps
annotations legal. See D7 for the measurement table, and
`tests/fixtures/saffron-static-guarantees-still-hold` plus four `errors/`
fixtures for the guard against this being re-decided from the old prose.

| Feature | Status in Saffron | Why |
|---|---|---|
| Refinement types | runtime contracts (D6) | no static base type to discharge over |
| Session types | **kept** -- TUR-E0211 fires identically | the protocol is in the ANNOTATION, which D2 keeps legal |
| GADTs | **kept** -- skolem escape fires identically | the indices are in the constructors' return-type annotations |
| Linear / affine / unique | **kept** -- TUR-E0101/E0100 fire identically | a use-count walk over the body; `any` never enters it |
| Explicit borrows, lifetimes | **kept** -- the aliasing conflict fires identically | a scope walk over uses; `any` never enters it |
| `with-region` / regions | rejected, TUR-E0312 | its proof reads the bracket body's INFERRED RESULT TYPE -- the one thing Saffron makes `any` (D7) |
| Monomorphization, by-value HKT | off; everything boxes | requires ground types at each site |
| Typeclass dispatch on `any` | **dispatches on the box tag** since S9 (2026-09-09); a few shapes still panic cleanly, see [S9](#s9----what-is-still-open) | typed Turmeric keeps the diagnostic by design |
| `extern-c`, inline-C | requires annotations (D2) | C needs concrete types |
| Effect rows, `--strict-effects` | inferred as today | effect rows are not value types; expected to work unchanged |

---

## 7. Risks

**R1 -- the elaborator forks anyway.** Every stage that adds
`if (lang == LANG_SAFFRON)` to a hot path in `elab_fns.c` / `elab_call.c` is a
step toward two elaborators. Mitigation: make the dialect flip *data* (a
default `TypeKind`, a table row) rather than *control flow*, wherever the shape
allows.

**R2 -- boxing costs, discovered late.** A dynamic program in a system with no
generational GC and no NaN-boxing allocates hard. S5 pins the leak story from
the first fixture, but throughput is unmeasured. Mitigation: a Saffron row in
`benchmarks/` from S3 onward, not from S8.

**R3 -- fixture count.** The suite is ~1442 fixtures and ~4-5 minutes. Saffron
needs each behaviour proven on both back ends, so its fixtures roughly double
per feature. Keep them small and prefer one fixture per behaviour over one per
combination.

**R4 -- `expires_at` misread as a gate.** CLAUDE.md is explicit that an
expiring `EXPERIMENTS[]` row never blocks a release cut, and that believing
otherwise has already stranded two releases. Saffron will be the only live row
for a while, which makes it the most likely candidate to be misread that way.

**R5 -- scope creep into a second stdlib.** S6's prelude is the thin end of a
wedge that ends with a parallel dynamic stdlib. The prelude should be
adaptors, not reimplementations, and any file added to it needs a reason
beyond "the typed one needs annotations".

---

## 8. Open questions

1. ~~**Cross-TU `any` id stability.**~~ **ANSWERED, and the answer was no.**
   The ids diverge per translation unit and four behaviours are wrong on
   `tur build --shared` today. Moved from an assumption to a hard S0
   prerequisite (P1); see
   [any-type-ids-are-per-tu](../archive/any-type-ids-are-per-tu.md). Kept
   here rather than deleted because the first draft's probe said "correct" and
   the reason it did -- `tur build <dir>` is single-TU -- is the kind of thing
   that would otherwise get re-derived from scratch.
2. **`.saf` extension** -- worth it, but not before the semantics settle;
   sequencing it early means every tool learns a file type whose meaning is
   still moving.
3. ~~**Does Saffron get its own `main` convention?**~~ **ANSWERED 2026-09-08,
   and the answer is `:int`, not the proposed `:nil`.** An unannotated Saffron
   `main` defaulted to `: any` like every other function, and the emitted C then
   had `return TUR_TAG(...)` inside a function declared `int` -- a cc error with
   no Turmeric diagnostic in front of it. Every Saffron fixture wrote `: int` on
   `main` to step around it, which is exactly the annotation the dialect exists
   to remove. `:int` keeps `main`'s meaning identical in both dialects: an
   explicit exit code still works, the process-exit convention does not fork per
   `#lang`, and a body yielding something else is an ordinary return-type error
   pointing at the body rather than at cc. Only the zero-arity `main`.
   `tests/fixtures/saffron-unannotated-main` is the first fixture with no type
   annotation anywhere.
4. **REPL default dialect.** `tur repl` is the surface where a dynamic dialect
   is most valuable and where changing the default is most disruptive. Propose
   `--lang saffron` opt-in at S8; revisit after there is usage. (**The opt-in
   landed at S8** -- `tur repl --lang saffron`, and `#lang saffron` typed at
   the prompt. The default is still Turmeric; the "revisit after usage" half
   is what remains open.)
5. ~~**Should the interpreter ever be Saffron-only?**~~ **MOOT.** If the compiled path (S5)
   proves disproportionately expensive, an honest fallback is "Saffron runs on
   the interpreter and the JIT, not on `cc`". That is a real product, given the
   REPL and DAP already exist. Recorded so the option is on the table rather
   than discovered under pressure. (S5 landed 2026-09-07 and every Saffron
   fixture is asserted on both back ends, so the fallback was never needed.)

---

## Appendix A -- probe transcript

All against `./build/tur` at `v0.44.2` (`2da89e84`), Debug build.

### A.1 -- the `int` default (G1, G2)

```
$ cat p1.tur
(defn add1 [x] (+ x 1))
(defn main [] : int (println (add1 41)) 0)
$ tur run p1.tur
42

$ cat p2.tur
(defn scale [x] (* x 2.0))
(defn main [] : int (println (scale 7.1)) 0)
$ tur run p2.tur
error [TUR-E0042]: mixed-width numeric arithmetic: '*' arg 2 is float, expected int

$ cat p17.tur
(defn twice [x] (* x 2))
(defn main [] : int (println (twice 21)) (println (twice 7.1)) 0)
$ tur run p17.tur
error [TUR-E0001]: function 'twice' arg 1: expected int, got float

$ echo '(defn greet [] "hello")' ...        =>  prints "hello"   (return inferred cstr)
$ echo '(defn n [] 42)' ...                 =>  prints 42        (return inferred int)
$ echo '(defn pi [] 3.14)' ...              =>  error [TUR-E0707]: function 'pi' declares
                                                return type 'nil' but its body returns float
```

The third line is G2: three unannotated returns, three different behaviours,
and an error message naming a declaration the programmer never wrote.

### A.2 -- `any` is storage-only (G3-G6, G8)

```
$ cat p3.tur
(defn bump [x : any] : any (+ x 1))
$ tur run p3.tur
error [TUR-E0006]: operator lookup failed for '+': got 2 arg(s), first arg type any
  note: available overload: + arity 2..* arg=int result=int
  note: available overload: + arity 2..* arg=float result=float

$ cat p7.tur                       # the union guide's headline example, corrected to : nil
(defn debug-print [x : any] : nil (println x))
$ tur run p7.tur
error [TUR-E0006]: operator lookup failed for 'println': got 1 arg(s), first arg type any

$ cat p8.tur
(defn get-any [] : any (make-struct Point 3 4))
... (match a (n : int) ... (p : Point) ...)
$ tur run p8.tur
error: match: scrutinee must be an ADT type, got any

$ cat p9.tur
(defn apply2 [f x] (f x))
$ tur run p9.tur
error: 'f' is not a function or continuation

$ cat p18.tur
(defn mk [] : any (fn [x : int] : int (+ x 1)))
(defn main [] : int (println (type-of (mk))) 0)
$ tur run p18.tur
unknown                            # the interpreter answers "fn" (eval.c:10810)
```

What *does* work (p5.tur, unmodified):

```
$ tur run p5.tur
int                                # (describe (box-int))  -- is? narrowing
cstr                               # (describe (box-str))
42                                 # (bump (box-int))      -- narrowed arithmetic
cstr                               # (type-of (box-str))
hi                                 # (cast (box-str) cstr)
```

And local inference, unannotated, with a real fractional part:

```
$ cat p10.tur
(defn main [] : int (let [x 7.1 y (* x 2.0)] (println y)) 0)
$ tur run p10.tur
14.2
```

### A.3 -- cross-TU `any` ids (ANSWERED: they diverge)

Two-module project, `amod` widening a `Beta` to `any`, `main-mod` interning
`Gamma` *first* so its local id ordering differs from `amod`'s. The same
program, two build modes:

```
$ tur build . && ./build/bin/anyids            # single-TU
Gamma
Beta
1                                              # correct

$ tur build --shared .                         # multi-TU (link obj/*.c, run)
Gamma
Gamma                                          # WRONG: a Beta reports as Gamma
0                                              # WRONG: (is? v Beta) is false
```

The emitted C, side by side:

```c
/* obj/amod.c */  case 1000: return "Beta";     TUR_TAG(1000, ...)   /* Beta */
/* obj/main.c */  case 1000: return "Gamma";
                  case 1001: return "Beta";     TUR_GETTAG(v) == 1001  /* is? */
```

A third mode surfaces when `main` casts the value it received -- the cast is
valid and panics anyway, naming the wrong type in both positions:

```
panic at tur_runtime.h:1817: cast: any holds ByVal, not HeapThing
```

and a fourth in `__tur_any_drop`, whose per-TU `boxed` flag makes one TU
`free()` a handle another TU owns.

**Why the first draft's probe said "correct":** `tur build <dir>` folds the
whole project into ONE TU. Confirmed by wrapping `CC` and capturing its
inputs -- one `.c` (7445 lines, both modules inside), one consistent table.
`--shared` passes three, and `emit-c --output-dir` (the CMake-consumer path)
splits the same way.

Full write-up:
[any-type-ids-are-per-tu](../archive/any-type-ids-are-per-tu.md).

### A.5 -- runtime typeclass dictionaries (D8)

A constrained `defn` monomorphizes -- no dictionary survives:

```c
static double describe__spec__double_tur_adt_Circle(tur_adt_Circle x);
static double describe__spec__double_tur_adt_Square(tur_adt_Square x);
```

The mode-B rank-2 path *does* carry a dictionary at runtime, as an `int64_t`
reached by slot index (`tests/fixtures/forall-dict-show`):

```c
static int64_t poly_hyshow_un_undict_un1444(int64_t __dict_1445, int64_t x) {
    const char *__ps_40 =
        (((const char * (*)(int64_t))((void **)(intptr_t)__dict_1445)[0])(x));
```

with the caller choosing the singleton from a statically known type:

```c
((int64_t(*)(void*, int64_t, int64_t))f.fn)(f.env,
    (int64_t)(intptr_t)(&dict_Show_int_singleton), (int64_t)(INT64_C(7)));
```

The same shape with a by-value struct receiver does not compile:

```
$ tur run tc3.tur
error: incompatible type for argument 1 of
  '(double (*)(tur_adt_Square))*(void **)__dict_1449'
  note: expected 'tur_adt_Square' but argument is of type 'int64_t'
tur: cc invocation failed (status 256)
```

Filed as
[forall-dict-byvalue-receiver-emits-uncompilable-c](../archive/forall-dict-byvalue-receiver-emits-uncompilable-c.md).

### A.4 -- `#lang` today

```
$ printf '#lang saffron\n...' > p12.tur && tur run p12.tur
tur: error: #lang unknown is not yet implemented

$ tur lang-layers
NAME         KIND      SINCE   SUMMARY
stringed     reader    v1      #s"..." owned-String literal (string/from-cstr)
```

---

## See also

- [docs/archive/lang-layers-plan.md](../archive/lang-layers-plan.md) -- the
  `#lang` base/layer model D1 extends
- [docs/guides/union-intersection-types-guide.md](../guides/union-intersection-types-guide.md)
  -- `any`, `cast`, `type-of`, `is?`, `if`-guard narrowing
- [docs/guides/turi-parity-guide.md](../guides/turi-parity-guide.md) -- what
  the interpreter can already run
- [docs/guides/experimental-flags-guide.md](../guides/experimental-flags-guide.md)
  -- the `--enable=` lifecycle D9 uses
- [docs/guides/contract-types-guide.md](../guides/contract-types-guide.md) --
  the runtime contracts D6 lowers refinements to
