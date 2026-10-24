# Macro System Direction -- procedural macros on turi, not phases

> **Archived 2026-09-09.** Direction adopted; Stages 0-3 landed with every
> follow-up, and Stage 4's RM5 and R3 reflection landed. The one remaining
> Stage 4 item, a JIT fast path for hot procedural macros, is shelved pending
> demand and is not a phase in progress. The CT-language freeze in the
> POLICY section stands and is the reason to keep this record.

Status: direction adopted; Stages 0-3 landed with every follow-up; of Stage 4, RM5 and R3 reflection are landed and the JIT fast path stays shelved pending demand.

## The question

The macro-time language is a small, closed compile-time (CT) evaluator
inside elaboration (`src/compiler/elab_macros.c`): quasiquote plus
`if`/`do`/`let`/`letrec`/`fn`/`map` and ~18 builtins, no arithmetic, no
calling runtime functions.  To expand macro power, do we (a) keep growing
this bespoke macro-time language, (b) adopt Racket-style phases, or (c)
something else?

## The answer: (c), a hybrid

**Freeze the CT language where it stands; keep template `defmacro` as the
permanent fast path; add an explicitly opted-in procedural macro form
whose body is ordinary Turmeric evaluated at expansion time by turi.**

Why not (a): every capability added to the CT evaluator (arithmetic,
strings, type inspection) is a second, drifting implementation of the
language, with its own bug trail -- see the ~15 archived macro-gap reports
in `docs/archive/history/` -- while a near-complete interpreter of the
real language (`src/turi/eval.c`) ships in the same binary, uncalled by
the compiler.

Why not (b): Racket's phase tower exists to make separate compilation,
per-phase module instantiation, and hygiene sound.  Turmeric elaborates
whole-program from source in one pass, macros are define-before-use, and
hygiene is deliberately manual -- the problems phases solve do not arise
here.  The one idea worth borrowing is small: an explicit annotation for
"this import is needed at expansion time" (Stage 3).

Why (c) fits unusually well: Clojure has no phases because the compiler
runs inside the runtime -- each top-level form is compiled and loaded
sequentially, so macros are just fns called with forms as data.  Turmeric
cannot do that literally (AOT to C), but turi is the next-best thing:
full-language, in-process (`TURI_EVAL_SOURCES` is linked into `tur`), and
sharing the compiler's exact reader and `elaborate_program_session`.
Clojure's "everything above is loaded" invariant maps exactly onto the
existing define-before-use rule, and everything downstream of
`expand -> Form*` (provenance notes, depth guards, definstance
re-spanning -- `elab_call.c`) is reused unchanged.  The one genuinely new
piece is a syntax value crossing the boundary.

## CT language freeze -- POLICY

The compile-time evaluator is **frozen**: no new builtins in
`CT_BUILTIN_TABLE`, no new `CtValue` cases (explicitly: no `Type` case,
no integers), no arithmetic.  The depth-cap diagnostic's "the compile-time
evaluator has no arithmetic" note stays true.  New expressiveness demand
routes to Stage 2's procedural macros, where the answer is "the whole
language" instead of one more bespoke builtin.  (Type-level reflection
remains governed by R3 of
[row-types-followups-plan.md](../upcoming/row-types-followups-plan.md): bounded,
total accessors if ever, never a general evaluator.)

## Stage 0 -- quick wins (LANDED)

- `CT_BUILTIN_TABLE` X-macro in `elab_macros.c`: one table drives both
  `ct_eval_builtin` dispatch and the template-routing gate
  (`template_needs_ct_eval`), killing the historical "builtin added to
  the evaluator but not the gate, silently never fires" bug class.
- Unified `gensym`: one implementation (`gensym_fresh`) for all three
  call paths, with freshness checked against the symbol table
  (`symtab_contains`) so user-written `tmp_0` cannot be captured; a
  `(gensym)` reaching ordinary runtime code is now a plain error instead
  of a half-baked TYPE_INT binding.
- `tur expand <file>`: prints every macro expansion to stdout with
  `;; name @ file:line:col` headers; diagnostics stay on stderr.  The
  intended golden-file mechanism for expansion tests.
- Multi-form `defmacro` bodies: setup forms + final template, wrapped in
  a synthetic CT `do`, always routed through the CT evaluator.

## Stage 1 -- syntax value + macro-time env plumbing (LANDED)

- `TY_SYNTAX` compile-time type kind (`types.h`, `TYPE_SYNTAX`, surface
  name `Syntax`; comment-void codegen placeholder -- never a compiled
  runtime value) and `TURI_SYNTAX` interpreter tag wrapping `Form*`
  (`src/turi/value.h`), extended through every TuriTag switch (repr,
  promotion, collmark, ffi tag names) and given structural `=` semantics
  via the hoisted `form_equal` (forms.c, shared with the CT evaluator).
- ~30 syntax natives (`wk_register_syntax_natives`,
  `src/turi/interpreter_natives.c`), registered typed (`TUR_NRT_SYNTAX`
  -> `TYPE_SYNTAX`) so REPL calls elaborate cleanly: `read-string`,
  accessors (`syntax-first/rest/nth/tag/len`), tag predicates,
  converters (`syntax->int/float/str`, `int/float/bool/str/sym->syntax`,
  `syntax-sym-name`, `syntax->string` -- the crossing that ends "no
  arithmetic at macro time" in Stage 2), span-inheriting constructors
  (`syntax-list/vec/cons`), `syntax-gensym` (symtab-checked freshness),
  `syntax=?`, `syntax-error`.
- Macro-time env: `elab_macro_env_get`/`_dispose` (`elab.h`,
  `src/turi/macro_env.c` -- implemented turi-side so the include
  direction stays turi -> compiler), hung off `Elab.macro_env`, lazily
  created, `TURI_CAP_NONE` + default sandbox fuel, torn down at all
  three elaborator teardown sites.
- **Stage gate passed** (ctest `tur_macro_env_nested`,
  `tests/turi/macro-env-nested.c`): env creation brackets
  `turi_env_new`'s unconditional `g_interpret_mode = true`; each nested
  eval is bracketed by `turi_eval_with_sink` ("Gap 7": mode + diag sink
  + diag file registry).  Deliberate decision recorded: macro-time code
  runs with interpreter semantics (`#?(:turi ...)` arms selected) --
  the macro-time language IS the interpreted language.
- Deferred from Stage 1: a REPL `expand-1` (needs prompt access to the
  session's macro registry -- natural alongside Stage 2's `defmacro*`);
  Stage-2 items: routing macro bodies into this env, quasiquote
  producing Syntax, and diag-sink policy for diagnostics raised *by*
  macro-time code mid-expansion.

## Stage 2 -- `defmacro*` procedural macros, same-module (LANDED)

- `defmacro*` dispatched next to `defmacro` (`elab_call.c`); `MacroDef`
  carries `kind = MACRO_TEMPLATE | MACRO_PROCEDURAL` + the macro-env
  binding name (`<name>__mx`).  At definition (`elab_defmacro_star`,
  elab_macros.c) the body is synthesized into
  `(defn <name>__mx [p : Syntax ...] : Syntax body...)`, printed, and
  evaluated into the macro env (`elab_macro_env_define_proc`) -- so type
  errors in the body surface at the DEFINITION.  Params implicitly
  Syntax; `& rest` is declared as one fixed Syntax param.
- Call path (`elab_macro_env_call_proc`, src/turi/macro_env.c, branched
  from `elab_expand_macro`): marshal raw arg forms to TURI_SYNTAX
  (variadic tail packed into one list form), fresh fuel per expansion,
  interp-mode bracketed `turi_call`, then a mandatory **cross-symtab
  import walk** -- macro-side forms intern symbols into the macro env's
  own table, and the elaborator dispatches special forms by pointer
  identity, so the expansion is deep-copied into the compile
  arena/symtab, with span-less constructed forms attributed to the call
  site.  Everything downstream (provenance note, definstance re-span,
  toplevel hand-off, refine crossing) is the unchanged template path.
- Reentrancy fixes the smoke tests forced (audit addenda): nested
  elaboration re-stamps the global builtin table's `name_sym` pointers
  (`builtins_init` runs at every elaborate entry) -- stamped back to the
  compile's symtab after each definition eval; the nested eval's
  `diag_reset` + file-id-0 registration are bracketed (whole-registry
  save + `diag_force_had_error`, also applied to the `read-string`
  native); `TY_SYNTAX` was falling into `typekind_default_copy_kind`'s
  CK_MOVE default, making second uses of a param a use-after-move --
  now CK_COPY; `syntax-error` types as Syntax so error branches unify.
- Kinds mix freely in both directions (procedural expansions calling
  template macros and vice versa); `tur expand` traces both.
- Fixtures: `macro-procedural`, `macro-procedural-variadic` (a counting
  macro -- the CT evaluator's designed-out case),
  `errors/macro-procedural-non-syntax`,
  `errors/macro-procedural-syntax-error`.
- Stdlib preload (follow-up, LANDED): the macro env now runs the REPL's
  exact preload sequence (macros -> native stubs -> collections ->
  typeclasses -> pin -> re-register natives) plus the string files
  (cstr.tur, str-build.tur), with capabilities denied only AFTER the
  preload (`(load ...)` is import-gated) and the whole creation
  self-bracketed (diag registry + builtins_init re-stamp) since it fires
  lazily mid-compile.  ~0.4s added to the first defmacro*-using compile.
  Enablers fixed along the way: elab_fn's return-keyword ladder gained
  elab_defn's typekind_from_symbol fallback (a fn LITERAL returning
  `: Syntax` -- or `: int32`, `: Sym` -- typed as a tyvar/int carrier
  before), the letrec pre-bind return peek learned Syntax, and the
  str-concat interpreter native allocates from the env value pool
  instead of malloc (a real leak under the leak-checked compile path
  once macros run it at expansion time).
- Proof-of-migration (LANDED): `tests/fixtures/macro-procedural-derive/`
  ports derive-show-cstr as a defmacro* -- a letrec'd, typed, recursive
  field walker building the same emitted `Show` instance; the
  template-derived and procedurally-derived structs render identically.
- Quasiquote producing Syntax (follow-up, LANDED): a quasiquote in a
  defmacro* body lowers -- purely syntactically, in elab_defmacro_star
  before the body reaches the macro env (`sxqq_walk`/`sxqq_lower`,
  elab_macros.c) -- into syntax-constructor calls.  `~expr` splices a
  Syntax-valued expression verbatim; `~@expr` splices a list-shaped
  Syntax via the new `syntax-append` native; new natives `kw->syntax`,
  `nil->syntax`, `syntax-type-ann`, `syntax-quote` round out the
  template vocabulary.  Nested quasiquote and `~@`-into-vector are
  clear diagnostics.  The `tur expand` trace now suppresses the macro
  env's internal preload expansions.
- REPL `expand-1` (follow-up, LANDED): `:expand <form>` at the prompt
  expands the form's head macro exactly once and prints the result --
  template and defmacro* macros alike, including ones defined at earlier
  prompts (the accumulated session source re-elaborates through a
  throwaway ElabSession whose macro registry survives for the manual
  expansion; cmd_expand, src/turi/repl.c).

Every Stage 0-3 item and every follow-up is now landed; the only
remaining roadmap work is the demand-driven Stage 4 convergence items.

## Stage 3 -- cross-module macro-time deps (LANDED)

- `(import m :for-macros)` -- a defmodule import-spec option
  (`ImportSpec.for_macros`, parsed in elab_module.c's parse_import_spec)
  that evaluates module m into the macro-time env so defmacro* bodies can
  call its functions at expansion time.  Macro-time ONLY: no runtime load,
  no scope injection, and combining with :as/:refer is a diagnostic (a
  module needed in both phases is imported twice).  Explicit, never
  implicit: which code runs at compile time is always visible in the
  source.
- Mechanism (`elab_macro_env_import`, src/turi/macro_env.c): the module
  path resolves through `elab_module_resolve_path` (a probe-only mirror of
  elab_load_module's search order -- importing file's dir -> stdlib ->
  -I dirs, kept in lockstep), then the macro env evaluates
  `(load "<path>")` with TURI_CAP_IMPORT granted for exactly that eval and
  only that grant taken back (a --macro-caps=io grant survives).  Repeat
  imports dedup via a sentinel binding in the env's own table (the load
  splice does NOT dedup across eval turns -- measured).  The shared
  mid-compile bracket (diag registry incl. id 0 + had-error flag +
  expansion-trace mute + builtins_init re-stamp) is factored into
  macro_env_bracket_enter/exit, used by creation, definition, and import.
- Module functions bind under their BARE names in the macro env
  (qualified m/name does not resolve there); documented in the guide.
- A defmacro* DEFINED in another module needs no :for-macros: export +
  :refer works through the existing whole-MacroDef clone (the PROCEDURAL
  kind and closure binding ride along; one compile-wide registry, one
  per-compile env).  Pinned by tests/fixtures/macro-cross-module-procedural/.
- `--macro-caps=io` (global flag, main parser + worker replay): re-grants
  exactly TURI_CAP_IO at macro-env creation for the rare effectful macro;
  any other name is a hard flag error -- ffi/unsafe/inline-C/async are
  never offered.  Default deny pinned by
  tests/fixtures/errors/macro-io-denied/; grant + import-still-denied
  pinned in the tur_macro_env_nested ctest.
- Fixtures: macro-for-macros-import (multi-file, incl. repeat-import
  dedup), macro-cross-module-procedural,
  errors/macro-for-macros-combined, errors/macro-for-macros-missing,
  errors/macro-io-denied.

## Stage 4 -- convergence (demand order)

- Reader-macro RM5, function expanders (LANDED, by subsumption): a
  template reader macro expanding into a defmacro* call gives read-time
  syntax a full-language procedural expander with no second expander
  mechanism -- `#csum[1 2 3 4 5]` folds to the literal 15 at compile time
  (tests/fixtures/reader-macros-procedural/).  The archived plan's
  "expander : Form -> Form once compile-time eval is available" is
  thereby closed without new code.
- Bounded type reflection per R3 (LANDED): `syntax-struct-fields`, a
  macro-env-only native (src/turi/macro_env.c) returning a
  single-constructor record's field names as a Syntax list of symbols --
  a bounded, TOTAL, flat projection of the compile's ADT registry, per
  R3's ruling (../upcoming/row-types-followups-plan.md): no Type value ever
  crosses into an evaluator.  The registry is reached through a
  reflection window (a saved/restored current-Elab pointer) that
  call_proc opens around each expansion, since Elab is a stack local of
  elaborate_program_session; the native refuses to run outside one.
  Unknown names, opaque newtypes, and multi-constructor/positional data
  types are expansion-time diagnostics.  Payoff pinned by
  tests/fixtures/macro-reflect-derive/: `(derive-show3 P3)` derives Show
  with NO field list.
- JIT fast path (`tur_jit_compile_image` as a transparent cache for hot
  procedural macros): still shelved -- a pure optimization with no
  demonstrated demand, blocked on a syntax-object calling convention
  regardless.

## What NOT to do

1. No Racket phase tower.
2. No hygiene overhaul bundled in -- unhygienic-with-gensym is documented
   design; changing it is an independent, breaking, post-v1 decision.
3. No heuristic routing between the two evaluators.
4. No JIT-first procedural macros.
5. No mass migration of the template macros.
6. No further CT-evaluator growth -- freeze means freeze.
7. No implicit macro-time imports.
