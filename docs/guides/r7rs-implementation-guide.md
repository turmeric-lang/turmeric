---
title: How #lang r7rs Is Implemented
category: Compiler Internals
description: How a #lang r7rs file becomes a Turmeric program -- dialect selection, the Scheme-to-Turmeric lowering and its rename table, the value representation, libraries and the split build, call/cc and the dynamic environment, the conservative collector, SRFIs, and the harnesses that cover them
---

# How `#lang r7rs` Is Implemented (internals)

This guide is for contributors changing how R7RS Scheme is compiled or run.
It describes the code as it stands. What the dialect offers a user, and where
it differs from the standard, is in the [R7RS guide](r7rs-guide.md); the
design history is in the archived plans listed under
[Reading the archive](#reading-the-archive).

The core idea fits in one sentence: **Scheme is Saffron's dynamic substrate
under a Scheme reader**. A `#lang r7rs` file reads into the same `Form`
model every dialect uses, a Form-to-Form pass rewrites the Scheme special
forms into Turmeric ones, and from elaboration on it is an ordinary program
whose unannotated values are `any`. Almost everything Scheme-specific lives in
three places: the lowering (`src/compiler/scheme_lower.c`), the prelude
(`stdlib/r7rs/prelude.tur`) and the collector (`src/runtime/r7gc.c`).

> **Trust the code over the header comments.** Several comments here
> predate later stages (checked 2026-10-16): the prelude's numeric header
> denies exact rationals and complex numbers (both exist); its
> representation header says strings are only `cstr` (`R7rsString` came in
> T3); and the collector comment above `r7rs_gc_active` calls it
> single-threaded (it runs threads in parallel). Where a comment and this
> guide disagree, read the code.

---

## The short version

1. **Selection.** `#lang r7rs`, `#lang r7rs/sweet` or a `.scm` extension
   picks `LANG_R7RS`. Its trait row turns on the dynamic `any` substrate,
   Scheme truthiness (only `#f` is false) and the Scheme runtime, and names
   `stdlib/r7rs/prelude.tur` as the auto-loaded prelude.
2. **Reading.** The Scheme reader is a *variant* of the Turmeric reader
   (`READER_R7RS`): Scheme lexemes, no Scheme semantics.
3. **Load expansion.** An `(import ...)` of an on-demand `(scheme ...)`
   library or an SRFI splices that library's file in, as a `(load ...)`
   would.
4. **Lowering.** `scheme_lower_program` rewrites every form whose span is in
   a `LANG_R7RS` file: core forms to Turmeric forms, `syntax-rules`
   expanded, Scheme names renamed to the prelude's `r7rs-<name>` spellings,
   `set!` variables boxed, each top-level statement wrapped in a prompt.
5. **Elaboration and emission** are the ordinary ones. A compiled program is
   usually built as two or three C units (library, imported libraries,
   program) so the prelude is compiled once and cached.
6. **Runtime.** Every allocation of the program goes through a conservative
   mark-sweep collector, `call/cc` copies the C stack, and `main` runs on a
   1 GiB thread.

---

## Source map

| File | Role |
|---|---|
| `src/compiler/lang_dialects.c` | `LANG_TRAITS[]` (the `r7rs` trait row), `LANG_BASES[]`, `lang_dialect_apply` |
| `src/compiler/reader.c` | `lang_base_from_name`, `reader_type_from_extension` (`.scm`), the Scheme lexical layer (`scheme_enabled`, `try_read_scheme_number`) |
| `src/compiler/scheme_lower.c`, `.h` | The lowering: `RENAMES`, `SCHEME_LIBS`, `ONDEMAND`, `SRFI_LIBS`, `syntax-rules`, libraries, assignment conversion, the escape-only `call/cc` test |
| `src/compiler/elab_toplevel.c` | `load_expand_forms` (the library splice) and the call to `scheme_lower_program` in `elaborate_program_session` |
| `src/compiler/elab_module.c` | The same lowering for an imported module (`elab_load_module_impl`) |
| `src/compiler/stdlib_autoload.c` | Appends `g_lang_prelude` last to the autoload list |
| `src/compiler/emit_split.c`, `.h` | Library / imported-library / program units: `emit_split_state`, `emit_split_rename`, `emit_split_pieces` |
| `src/main.c` | `prelude_split_applies`, `prelude_split_object` (object cache), `SplitLibCache` (emission cache), the call to `srfi_prune_program` |
| `src/passes/srfi_prune.c`, `.h` | Drops SRFI definitions nothing reaches |
| `src/compiler/emit_module.c` | `r7rs_gc_active`, `emit_r7rs_gc_prologue`, `emit_r7rs_gc_tls_roots`, `emit_deep_stack_runtime` |
| `src/compiler/emit_dk_runtime.c` | `tur_r7rs_dyn` (the per-thread dynamic environment), `tur_dk_pinned` |
| `src/runtime/r7gc.c` | The collector, pasted into the program's unit |
| `src/turi/interpreter_natives.c` | The interpreter's twins of the prelude's inline C (`r7rs-cont-capture__`, `r7rs-toplevel__`, the dynamic-environment slots, ...) |
| `src/turi/r7rs_embed.c`, `.h` | The evaluator behind `(scheme eval)` |
| `src/compiler/r7rs_bignum.inc`, `r7rs_numsyntax.inc`, `src/turi/r7rs_unicode.inc` | Shared C for numbers and Unicode tables |
| `tools/gen-r7rs-inc.py`, `tools/gen-r7rs-unicode.py` | Generate the stdlib copies of that C |

| Stdlib file | Role |
|---|---|
| `stdlib/r7rs/prelude.tur` | Every resident procedure, written in Turmeric shapes but read as `#lang r7rs` |
| `stdlib/r7rs/{bignum,numsyntax,unicode}.tur` | Loaded by the prelude; their file-scope C is generated |
| `stdlib/r7rs/{time,process-context,file,read,eval}.tur` | On-demand libraries, spliced in only when imported |
| `stdlib/r7rs/thread.tur` | The C primitives SRFI 18 calls (`SRFI_HELPERS`) |
| `stdlib/srfi/<N>.scm` | One `define-library` per importable SRFI |
| `stdlib/sicp/extras.scm` | `(sicp extras)`, carried by the SRFI machinery |

---

## Pipeline

### Dialect selection

`lang_base_from_name` (`src/compiler/reader.c`) resolves the `#lang` token
through `lang_base_lookup` over `LANG_BASES[]`. Two rows are Scheme:
`LANG_R7RS` with `READER_R7RS` (`#lang r7rs`) and with `READER_R7RS_SWEET`
(`#lang r7rs/sweet`, SRFI 110 over Scheme lexemes). The `r7rs` trait row is
not `reader_axis_free`, so `r7rs/neoteric` and the like are not bases. A
`.scm` file is `#lang r7rs` without the line
(`reader_type_from_extension`, `lang_dialect_from_extension`).

`lang_dialect_apply` sets two globals from the trait row:

- `g_opt_dynamic_any` (from `dynamic`): unannotated means `any`, and the
  dynamic call/operator/field surface comes with it. Saffron sets it too.
- `g_opt_r7rs` (from `scheme_runtime`): tells the emitter this is a Scheme
  program -- which collector opt-out applies, and the deep stack. Its name
  survives from the `EXPERIMENTS[]` row the dialect had until it graduated
  in 0.57.0 (`GRADUATED[]` in `src/runtime/experiments.c`). The split build
  keys off the prelude instead (`g_lang_prelude`).

`scheme_truthiness` is read per file off the span
(`lang_dialects.c:lang_span_is_scheme`, used by the elaborator's
`elab_saffron_truthy`), so a Turmeric file in the same program keeps
Turmeric's rule.

### Reading

The Scheme layer of the reader (`r->scheme_enabled`) handles each R7RS lexeme
Turmeric spells differently: `#t`/`#f`, `#\x` with R7RS names, `#(...)`
(read as `F_VEC`), `#u8(...)`, `,`/`,@`, dotted pairs, `|sym|`, numeric
prefixes and the whole number syntax, and `#!fold-case`. A number token goes
through `r7rs_ns_parse` (`r7rs_numsyntax.inc`), the same parser
`string->number` and `read` use, so `1/2` and `3+4i` are one token.

In **user** Scheme source (not the prelude or `stdlib/r7rs/`), a leading `:`
is an identifier, `[...]` is a list, `true`/`false`/`nil` are identifiers,
and Turmeric `#` literals, inline C and `@` are refused
([r7rs-turmeric-syntax-leaks](../archive/r7rs-turmeric-syntax-leaks.md)).

### Load expansion

Before lowering, `load_expand_forms` (`elab_toplevel.c`) asks
`scheme_import_library_files` which files a top-level `import` needs and
splices each as a `(load ...)`, once per compile: an on-demand `(scheme ...)`
library (`SCHEME_LIBS`), an SRFI's `.scm` file, and any helper file the SRFI
needs (`SRFI_HELPERS`). The prelude itself arrives through the autoload list:
`stdlib_autoload.c` appends `g_lang_prelude` last.

### The lowering

`elaborate_program_session` calls `scheme_lower_program` after load
expansion and before the `main` fold, guarded by `scheme_lower_needed`;
`elab_load_module_impl` does the same for an imported module. The decision
is per form, off the span's file: a form from a non-Scheme file passes
through by pointer, and a Turmeric-shaped form inside a Scheme file
(`(defn ...)`, `(let [x 1] ...)`) passes through with its subforms lowered.
Nothing under `quote` is touched.

The core rewrites (the authoritative list is `scheme_lower.h`):

| Scheme | Turmeric |
|---|---|
| `(define (f a . r) b...)` | `(defn f [a & r : any] b'...)` |
| `(define x e)` | `(def x e')`, `^mut x : any` when `set!` anywhere |
| `(lambda (a b) b...)` | `(fn [a b] b')` |
| `let`, named `let`, `let*`, `letrec`, `do` | `let` / `letrec` / a `letrec` loop |
| `begin` | `do` (spliced at top level) |
| `cond`, `case`, `and`, `or`, `when`, `unless` | value-returning `if` chains |
| `case-lambda` | a variadic `fn` dispatching on the count |
| `define-values`, `let-values` | temporaries over the `R7rsValues` carrier |
| internal `define`s | `letrec` / `let`, R7RS `letrec*` order |
| `quote` / `quasiquote` | runtime builders (`r7rs-list`, `r7rs-cons`, `r7rs-vector`, `r7rs-append2__`) |
| `guard`, `parameterize`, `delay`, `delay-force` | calls into the prelude (`lower_guard`, ...) |
| `define-record-type` | a `defstruct R7rsRec_<name> :heap`, lifted to top level |

Things the lowering does beyond the table:

- **Operators.** `+ - * / = < > <= >=` are not rows of the rename table. In
  call position `lower_operator` folds them onto binary prelude helpers
  (`r7rs-add2__`, ...), handles `(+)`, `(- x)`, `(/ x)`, and binds a
  comparison chain's arguments once. In value position the operator names
  the variadic `r7rs-+`, so `(apply + xs)` works. The `OPS` table is in
  `sl_init`.
- **`syntax-rules`.** The expander lives here (`sr_expand`, `sr_match`,
  `sr_inst`, the `hyg_*` renamers), not on the `defmacro*` substrate: a
  `defmacro*` runs during elaboration, after this pass, and its output would
  be Scheme forms nothing lowers any more. Hygiene is renaming: binders a
  template introduces get fresh names, and a template's free identifiers
  resolve in the macro's defining scope (`LFrame`).
- **Assignment conversion** (`ac_walk`). Every `set!` variable becomes an
  `R7rsBox`; reads become `r7rs-unbox__` and `set!` becomes
  `r7rs-box-set!__`. A compiled closure copies what it captures, and a
  re-entered continuation restores C locals, so a variable has to be a heap
  location to behave like a Scheme location.
- **Per-form prompt.** Each top-level statement of a program is lowered as
  `(r7rs-toplevel__ (lambda () stmt))` (`lower_toplevel_stmt`), so a
  continuation captured in it extends only to the end of the form. Library
  bodies and REPL lines are lowered bare.
- **Name respelling.** Local binders are renamed apart (`x__v12`); a program
  global that collides with an auto-loaded stdlib name, a Turmeric type name
  or a Turmeric special form becomes `<name>--user`; a `^name` identifier
  becomes `__scheme_caret_name`; an SRFI's definitions become
  `srfi<N>--<name>`. `scheme_source_name` undoes these for error messages.
  A redefined standard name keeps its standard meaning in forms evaluated
  before the redefinition (`redefinitions_to_set`, the `--std--` prefix).
- **Module wrapping.** A program that imports anything is wrapped in a
  `defmodule` named after its file (Turmeric's `import` is only legal
  there), with its statements as the body of a synthesized `main`. A file
  name starting with a digit gets an `r7rs-program-` prefix.

Diagnostics for malformed forms go through `err`; the offending form is
dropped and elaboration continues, so `diag_had_error` reports it.

### The rename table

The prelude cannot define `car`: the typed stdlib's `list.tur` already owns
that name, and redefining an auto-loaded name at top level is an error. So
every prelude procedure is spelled `r7rs-<name>`, and `RENAMES[]` in
`scheme_lower.c` maps the Scheme spelling to it at every unquoted occurrence
(`rn` -> `rn_global`). The same table answers `scheme_public_name`, which
error messages use to name the procedure the way the program wrote it.

Three tables, not one:

| Table | Holds | Consulted |
|---|---|---|
| `RENAMES` | resident procedures (`(scheme base)`, `char`, `cxr`, `complex`, `inexact`, `lazy`, `write`, `case-lambda`) | always |
| `ONDEMAND` | procedures of on-demand libraries (`time`, `process-context`, `file`, `read`, `eval`, `repl`, `load`, `r5rs`) | only after the unit imports that library |
| `OPS` (in `sl_init`) | the nine numeric operators | always, via `lower_operator` |

**The rule.** A procedure added to the prelude is added to `RENAMES` in the
same change, or Scheme code cannot reach it (an on-demand library's goes in
`ONDEMAND`). The rule is stated twice: in the comment above `RENAMES` in
`scheme_lower.c` and in the NAMES paragraph of `stdlib/r7rs/prelude.tur`'s
header. **No script enforces it.** Neither `check-r7rs-srfi-sync.sh` nor
any other check compares the table with the prelude; a missed row shows up
only as an unbound name when a Scheme program calls it. Today the table and
the prelude agree: every public (non-`__`) `r7rs-` definition without a row
is an operator, a name the lowering emits itself (`r7rs-values-ref`), or an
internal that cannot take `__` (`r7rs-cont-save-state`: an inline-C
`__TUR_CNAME_` splice ends at the first `__`).

Internal helpers end in `__` by convention; the prelude's own spans are
exempt from renaming and from the operator rewrite (`prelude_span`), which is
how it can call Turmeric's raw `+`.

---

## Values

### `any` is a tagged word pair

An `any` in emitted C is

```c
typedef struct { int64_t tag; int64_t val; } tur_tagged_t;
```

`tag` is a 64-bit type id (a hash of the type) and `val` the payload: the
integer, the bits of a double, or a heap pointer. `TUR_TAG(id, v)` builds
one. A per-unit table of `__tur_any_ti` rows (id, name, boxed, drop)
registers each type a unit widens; in a split build each unit registers its
own (the one data symbol `check-r7rs-prelude-split.sh` allows both units to
define). `is?` tests the tag, and a typed parameter receiving an `any`
checks it once at the call ("crossing the seam"). The general machinery is
Saffron's; see [value representations](value-representations-guide.md) for
the boundary rules.

The prelude's predicates are functions on purpose. An inline `(is? x T)`
narrows `x` to `T`, and a narrowed `:heap` struct is a unique value, so a
second use would be TUR-E0201; a call leaves `x` an `any`.

### Scheme types

| Scheme | Representation (`stdlib/r7rs/prelude.tur`) |
|---|---|
| pair | `R7rsPair :heap [a d]` (not `car`/`cdr`, which would meet the stdlib's `car` in `.field` lookup) |
| `()` | `R7rsNull`, one value `r7rs-null-value__`; an unset dynamic slot (all zero) also reads as `()` |
| boolean | Turmeric `bool` |
| char | `R7rsChar`, a `defopaque` over the code point, so `char?` and `integer?` are disjoint |
| string | a `cstr` (literals, and strings from `symbol->string`, `number->string`, `read`, Turmeric) or an `R7rsString :heap [cps : (Vec int)]` (strings a procedure newly allocates); `string-set!` on a `cstr` is a named error |
| symbol | `Sym` (`stdlib/sym.tur`); `string->symbol` interns through `stdlib/sym-dynamic.tur`. A quoted symbol is a keyword record (`__tur_sym_...`) |
| vector | `(Vec any)` |
| bytevector | `R7rsBytevector :heap [bytes : (Vec int)]` |
| eof | `R7rsEof`, one value |
| multiple values | `R7rsValues [items]` over a Scheme list; `(values x)` is `x` |
| error object | `R7rsError :heap [message irritants kind]` (kind: 0 raise, 1 file, 2 read) |
| record | `R7rsRec_<name> :heap` |
| parameter, promise | `R7rsParam`, `R7rsPromise` over `R7rsPBox` |
| port | `R7rsPort :heap`, over an opaque `R7rsIo` C buffer |
| boxed `set!` variable | `R7rsBox :heap [v]` |

Wherever an `any` is unboxed to a `cstr` (a Scheme call passing a string to
a Turmeric `cstr` parameter, or Turmeric code casting a Scheme result), the
elaborator calls the prelude's `r7rs-str__`
(`elab_toplevel.c:r7rs_string_unbox`): a `cstr` passes through and an
`R7rsString` becomes a fresh UTF-8 copy.

### Numbers

| Kind | Representation |
|---|---|
| exact integer in int64 | `int` |
| exact integer outside int64 | `R7rsBig :heap [digits : cstr]`; `r7rs-big-norm__` is the only constructor, so a bignum never holds a value an `int` could |
| exact rational | `R7rsRatio :heap [num den]` |
| inexact real | `float` (double) |
| complex | `R7rsComplex :heap [re im]` |

Exactness is the representation. `r7rs-add2__` and friends take the int64
path for two ints and check for overflow, falling into the bignum core
(`r7rs-big2__`); an exotic operand goes to `r7rs-big-arith__`; a mixed
int/float pair takes the promoting dynamic operator. The bignum and number
syntax C is in `stdlib/r7rs/bignum.tur` and `numsyntax.tur`, generated from
the `.inc` files (see [Generated C](#generated-c-and-its-sync-checks)).

A quick check of the tower and the representations:

```scheme
(write (/ 7 2))                       ; 7/2
(write (sqrt -4))                     ; +2i
(write (* 99999999999 99999999999))   ; 9999999999800000000001
(write (+ 1 7.1))                     ; 8.1
```

---

## Libraries

### Import sets

`lower_import_set` folds an import set's `only`/`except`/`prefix`/`rename`
nest into one `SchemeImportSpec`, then dispatches on the library name
(`library_module`):

| Name | Meaning |
|---|---|
| `(scheme <x>)` | a `SCHEME_LIBS` row. Resident: a scoping statement only. On-demand: its file was spliced by load expansion, and its `ONDEMAND` names start resolving |
| `(srfi N)`, `(sicp extras)` | `srfi_import`; never a module (see [SRFIs](#srfis)) |
| `(turmeric a/b)` | the Turmeric module `a/b`; `stdlib/` is dropped, and an auto-loaded stdlib file is a no-op that grants its names |
| anything else | a user library: `(my utils)` is module `my/utils` |

For a `(scheme ...)` library, whose names are global anyway, `only` and
`except` change what a name *means* rather than what is visible:
`(except (scheme base) assoc)` lets the program define its own `assoc`. A
user library or Turmeric module gets `:refer` from `only`, `:as` from
`prefix`, and a full import from `except`.

User Scheme source sees the auto-loaded Turmeric stdlib only through
`(turmeric stdlib/<file>)` grants (`grant_stdlib_import`,
`scheme_std_visible`).

### `define-library`

A file with a `define-library` lowers to exactly one
`(defmodule my/utils (export ...) imports... body...)` and may hold nothing
else at top level. Restrictions that remain (one library per file, named by
its path) are recorded in
[r7rs-library-file-shape-and-export-rename](../reported/r7rs-library-file-shape-and-export-rename.md).
`(export (rename a b))` is supported through the clash table or an `any`
alias.

Exported `syntax-rules` macros are not module exports: they are expanded by
this pass, before any module loads. An importer reads them from the
library's source (`lib_syntax_of`, resolving the path through the
`SchemeLibResolveFn` callback `elab_scheme_library_path`), and the library
exports the definitions its macros reference under hidden spellings
(`lib_rewrite_exports`).

Importing a Turmeric module or a user library sets
`g_scheme_lower_opened_seam`, which turns off the interpreter's
continuation-image sweep for the rest of the process (natives there can hold
values the sweep cannot see).

### The split build

A compiled Scheme program reaches most of the prelude before it does
anything (the uncaught-error printer alone pulls in `write` and the numeric
tower), so the prelude is compiled once and linked. The emitter writes the
same elaborated program as up to three C units (`emit_split.h`):

| Unit | Mode | Defines | Cached by |
|---|---|---|---|
| library | `EMIT_SPLIT_LIB` | the runtime preamble and every auto-loaded stdlib item (prelude included); all runtime state | hash of its text: `<tur-build>/prelude/<hash>.o` |
| imported-library | `EMIT_SPLIT_IMP` | the stdlib libraries the program imports (SRFIs pruned per program, `(scheme eval)`, `(scheme read)`) | hash of its text |
| program | `EMIT_SPLIT_CLIENT` | the program | -- |

A unit defines its own items, declares those of the units before it, and
never sees later ones. `emit_split_state` rewrites verbatim text (the
preamble, hoisted inline C, file-scope C blocks): in the library unit
file-scope variables lose `static`; in a later unit they become `extern`,
external functions become prototypes and constructors are demoted, so there
is one instance of every piece of state. Every exported name gets the prefix
`tur_sl_` (`EMIT_SPLIT_PREFIX`) so it cannot bind to the same-named symbols
in `libturi.a`. `emit_split_export_static_fns` (via `emit_split_once_fns`)
makes the collector, the DK runtime and the deep-stack helper defined once,
in the library unit. `emit_split_pieces` compiles a cold library unit as
several pieces in parallel.

When it applies: `prelude_split_applies` (`src/main.c`) -- a `#lang r7rs`
entry, a 64-bit host, not `--debug`, not writing a manifest, on Linux and
macOS by default (`TUR_PRELUDE_SPLIT=1` opts in elsewhere; Windows is off
until [r7rs-prelude-split-wrong-symbols-on-windows](../reported/r7rs-prelude-split-wrong-symbols-on-windows.md)
closes). `TUR_PRELUDE_SPLIT=0` and `TUR_IMPORT_SPLIT=0` turn the two splits
off.

Two caches sit on top:

- **The object cache** (`prelude_split_object`): a missing object is
  compiled to a private name and renamed into place, so parallel builds
  never read a half-written one.
- **The emission cache** (`SplitLibCache`): emitting the library unit's text
  was still a large share of `tur`'s work, so the first build writes the
  renamed text (`.libtext`) and the state later units need (`.lib`). The key
  covers the `tur` binary, the command line, `TUR_*` variables, switches and
  every stdlib file the unit draws from. A program that reads a module of
  its own, or has an `extern-c`, does not use it. A wrong entry is caught by
  `emit_split_carry_load`, `split_check_sigs`, or a failed compile/link, and
  each falls back to emitting. `TUR_SPLIT_LIB_CACHE=0|verify` controls it.

If the units fail to compile or link, `cmd_build` walks a ladder, each step
"slower, never wrong": a cached library unit is re-emitted, three units
become two, two become one. The steps are announced only under
`TUR_SHOW_CC`; otherwise the only trace is the failed cc's own output and a
`tur: cc invocation failed` line before the program runs normally. So a
split that always fails costs extra builds on every run without failing
anything. In a tree where only `--target tur` was built (no runtime
archives) every Scheme build takes the ladder to the bottom. Link details
belong to the [build driver internals](build-driver-internals-guide.md) and
[runtime preamble and linkage](runtime-preamble-and-linkage-guide.md)
guides.

---

## Control

### Escape-only `call/ec`

`r7rs-call/ec__` wraps Turmeric's own one-shot `call/cc` (the DK machine's
escape; see [CPS and the DK machine](cps-dk-internals-guide.md)). The
receiver gets a variadic procedure that unwinds the wind stack to the
capture point (`r7rs-rewind-to__`) and returns its arguments. `guard` and
the eval bridge use it. Invoking it after the call/cc returned is the named
error "continuation invoked after its call/cc prompt returned".

### Re-entrant `call/cc`

The public `r7rs-call/cc` (T5) copies the C stack:

1. `r7rs-cont-capture__` allocates an `r7k_cont`, sets `tur_dk_pinned = 1`,
   saves the per-thread runtime state through `r7rs-cont-save-state` (the
   tail-call trampoline's root and armed callee, the DK driver, pending
   perform and entry depth, the live escapes), and `setjmp`s.
2. `r7k_measure` sizes the image from just below the capture to the base:
   the current top-level form's mark (`r7k_form_base`, set by
   `r7rs-toplevel__`) when there is one, else the thread's stack base
   (`r7k_stack_base`; on glibc it stops below the thread's TCB and static
   TLS so a re-entry does not write thread-locals back).
3. The image is `malloc`ed (so it is on the collected heap and scanned) and
   copied, and `TUR_REGION_NOTE_WORDS(c->img, c->n)` notes every word in
   it, per the region store-hook rule: a word pointing into a live region
   generation marks it escaped, so no bracket rewinds memory a re-entry
   would bring back.
4. Invoking the continuation checks it is on the capturing thread
   (`r7rs-cont-check-here__`) and, for an eval capture, that it is still
   resumable; travels the wind stack (`r7rs-travel-to__`: `after` thunks
   out, `before` thunks in); stores the delivered value in dynamic slot 3;
   then `r7rs-cont-restore__` grows the stack past the image
   (`r7k_restore`'s `alloca`), copies it back and `longjmp`s. The capture
   returns again with the handle's low bit set, and `r7rs-call/cc` returns
   the delivered value.

Where no stack base is found, `r7rs-cont-capture__` returns `NULL` and
`call/cc` is the escape. On Windows the jump uses no-unwind
`__builtin_setjmp`/`longjmp` (or the host's `tur_sjlj_*` under c2mir), since
win64 `longjmp` unwinds through frames the copy has just replaced. Under
ASan, stack-use-after-return detection is turned off in the program
(`__asan_default_options`).

`tur_dk_pinned` is never reset. Once any copying capture has happened,
`dk_free` and the reap-list hand-backs become no-ops for the rest of the
process: an image can hold any live DK node, and re-entering it after its
CPS entry returned must find the node intact. On the default build the
collector reclaims that memory instead.

### Choosing between them

At each `(call/cc (lambda (k) body...))` in user source, `callcc_escape_only`
proves `k` cannot outlive the call, and if so the call becomes
`(r7rs-call/ec-proc__ ...)`. `k` qualifies when every occurrence is the
operator of a call, and every closure it occurs in is either a `lambda`
argument of a procedure that calls and drops its procedure arguments
(`for-each`, `map`, their vector and string twins, `call/cc`) or a named
`let` whose name also only heads calls. Anything else -- a stored lambda,
`delay`, `case-lambda`, an internal `define`, a rebinding of `k` -- keeps the
copying `call/cc`. `dynamic-wind` is deliberately not in the list.

`r7rs-call/cc`'s no-stack-base fallback and the escape-only lowering both
reach `r7rs-call/ec__` through the procedure *value* `r7rs-call/ec-proc__`:
a direct call would make the caller a CPS function, and a CPS entry's heap
frames are freed when it returns, which is exactly what a re-entry must not
find. (`lower_guard` calls `r7rs-call/ec__` directly.)

### The dynamic environment

The wind stack, handler stack, parameter bindings and the delivered value
are slots 0-3 of `tur_r7rs_dyn`, a `TUR_THREAD_LOCAL` struct of four
`tur_tagged_t` declared by `emit_dk_runtime.c` (routed through
`src/runtime/tur_tls.c` under the JIT). The prelude reads and writes it with
`r7rs-dyn-ref__` / `r7rs-dyn-set__`; the setter notes the word
(`TUR_REGION_NOTE_WORDS`), since the slot outlives any region. A fiber
switch swaps it. It used to be process-global, and a `raise` on one thread
reached another thread's handler
([archived report](../archive/r7rs-dynamic-environment-shared-across-threads.md)).

On top of the slots:

- `dynamic-wind` pushes an `R7rsWind` frame while the thunk runs;
- `with-exception-handler` installs its handler with `dynamic-wind`, so an
  escape restores the handler stack;
- `raise` and `raise-continuable` walk the handler stack, and `guard`
  escapes through `call/ec`;
- `parameterize` binds a parameter's cell under `dynamic-wind`;
- a promise is a shared `R7rsPBox`.

### Stack depth and tail calls

A compiled Scheme `main` runs on a 1 GiB thread (64 MiB on 32-bit;
`emit_deep_stack_runtime`, emitted only when `g_opt_r7rs`).
`TUR_MAIN_STACK_MB` resizes it and `TUR_NO_DEEP_STACK` turns the move off.
Dynamic tail calls are proper through the tail-call trampoline
(`tur_tb_armed_for`, `tur_tb_root`), which is why the continuation capture
saves and restores that state
([archived report](../archive/r7rs-tail-call-through-static-call-not-proper.md)).

---

## The collector (`r7gc.c`)

A Scheme value is a `:heap` box, and Turmeric's memory model never frees
those, so without a collector a Scheme program only grows. `r7gc.c` is a
conservative, non-moving mark-sweep collector in the Boehm style. The
`r7rs-gc` experiment graduated on 2026-09-25 and the collector is now on by
default; the file's first line still calls it an experiment.

**Plugging in.** `emit_r7rs_gc_prologue` pastes the source into the unit
(once, in the library unit of a split build), and `emit_r7rs_gc_macros`
redirects `malloc`, `calloc`, `realloc`, `free`, `strdup`, `strndup`,
`tur_region_alloc_or_malloc` and `tur_region_free` to `tur_gc_*` with
object-like macros (so `free` passed as a pointer is the collector's too).
The macros are withdrawn around runtime sources pasted verbatim. The runtime
archive (`libturt_runtime.a`: HAMT, `rc<T>`, strings, symbols) allocates
through the hook in `src/runtime/rt_alloc.h`, which a constructor points at
the collector, so a Scheme value held only in a Turmeric map stays alive.

**When.** `r7rs_gc_active`: on for a Scheme program unless `--shared`,
`TUR_R7RS_GC=0` or `--no-r7rs-gc`. The same collector serves Saffron
(`TUR_SAFFRON_GC=0` / `--no-saffron-gc`). It is compiled in only on
Linux/glibc and macOS, and not under `tur jit` (globals live in MIR memory
the root scan cannot see); elsewhere every entry point is libc.

**Roots.** Every registered thread's stack and spilled registers (its own
for the collector, from the park point for a parked thread, from the
stop-signal handler's frame otherwise); each thread's `TUR_THREAD_LOCAL`
variables, registered at thread start through `tur_rt_tls_roots` (written by
`emit_r7rs_gc_tls_roots`), plus values stored with `pthread_setspecific`;
the executable's data and bss (the Mach-O writable segments on macOS), less
libturi's renamed sections on Linux; the used bytes of every region
generation; and each thread's allocation cache.

**Threads.** Allocation is a per-thread cache of size-class slots refilled
under `G->heap`. A collection takes `G->world`, then `G->heap`, then stops
every other thread with a signal (`SIGPWR`/`SIGXCPU` on Linux) or leaves a
parked one where it is. Blocking calls are wrapped as release points. A
thread the unit did not start that calls in aborts with a message
(`tur_gc_check_thread`).

**Interaction with RC and static drops.** Static drops still run: a `free`
reaches `tur_gc_free`, which returns the slot to its free list. A pointer
the collector does not own goes to libc, an interior pointer or a double
free is ignored, and a free from an unknown thread is left to the collector.
So an object freed early by ownership is reclaimed immediately, and
everything ownership cannot prove (aliased `any` boxes, cycles, pinned DK
nodes, continuation images) is reclaimed by tracing.

**Knobs.** `TUR_GC_STATS=1`, `TUR_GC_TORTURE=N` (collect every Nth
allocation; turns a missing root into a crash), `TUR_GC_TORTURE_SCALE=R`,
`TUR_GC_THRESHOLD=B` (default 8 MiB floor). See the
[GC guide](gc-guide.md) for the memory model it complements and the
[regions internals guide](regions-internals-guide.md) for the region
registry it scans.

---

## The interpreter and `eval`

`tur --interpret` runs the same lowered forms: the interpreter elaborates
through `elaborate_program_session` too (`src/turi/eval.c`), and REPL and
`eval` turns hand the lowering their earlier turns through
`scheme_lower_set_session_prior`. Each prelude function whose body is
inline C has a native twin in `src/turi/interpreter_natives.c`, registered
under the same name (`native_r7rs_cont_capture`, `native_r7rs_toplevel`,
the dynamic-environment slots, ...). The interpreter's `call/cc` uses the
same stack-copying technique over its own frames, and its memory policy
(pool sweeps, process-lifetime closures) is its own. See the
[turi interpreter internals](turi-interpreter-internals-guide.md) and
[turi parity](turi-parity-guide.md) guides.

`(import (scheme eval))` splices `stdlib/r7rs/eval.tur`, whose inline C
carries a `__tur_autolink__: -lturi` marker, so only a program that imports
it links the interpreter (`check-r7rs-eval-link.sh` pins both halves). The
evaluator (`src/turi/r7rs_embed.c`) is one embedded `TuriEnv` per process.
Datums cross as `write` text, procedures as ids in either direction, and a
result is a kind byte (`D`, `P`, `H`, `U`, `R`, `E`). A program continuation
captured inside a call from evaluated code reaches the evaluator's frame
state through four hooks the prelude's capture calls
(`r7rs-cont-hooks-install__`, `r7k_hook_*`).

---

## SRFIs

### The table

`SRFI_LIBS[]` in `scheme_lower.c` is the single source for SRFI behaviour.
Each row is a number, a kind, a title, a file and a reason:

| Kind | Meaning |
|---|---|
| `SRFI_BUILTIN` | R7RS already is the SRFI: the import binds names to the `(scheme ...)` bindings and emits nothing; the file has an export list and no body |
| `SRFI_ALIAS` | a few new names for built-in procedures (38, 45, 66) |
| `SRFI_LIBRARY` | a real implementation, spliced in when imported |
| `SRFI_NOLIB` | syntax that is always on (0, 62, 105) |
| `SRFI_NOTPLANNED` | refused with `why` (40) |
| `SRFI_NOTYET` | refused until the plan stage in `why` |

The table drives the import (`srfi_import`), cond-expand's `srfi-N` and
`(library (srfi N))` (`feature_holds`), the load expander's splice
(`lib_files_of_set`), and the list `(features)` must return (the prelude
writes it out; the sync check compares). `(sicp extras)` rides
the same machinery as row `SICP_EXTRAS_NUM` (100000), with definitions
spelled `sicpx--<name>` and no `srfi-N` feature.

### How an SRFI is compiled

An importable SRFI is one file, `stdlib/srfi/<N>.scm`, holding one
`(define-library (srfi N) ...)` in plain R7RS. It is not a module:

- the load expander splices it once per compile; `srfi_source_forms` turns
  the library into its body's definitions, each spelled `srfi<N>--<name>`,
  emitted at top level outside any program module, so every module of the
  compile sees them;
- every importer knows the export list and macros (`SrfiLib`, read from the
  spliced form or from the file) and binds each kept name: a macro to its
  hidden macro, a definition to its `srfi<N>--` spelling, a re-exported
  R7RS name to R7RS's own binding;
- an SRFI whose primitives are C has them in a prelude-shaped helper under
  `stdlib/r7rs/` (`SRFI_HELPERS`; today SRFI 18's `thread.tur`), spliced
  ahead of it.

`srfi61_cond` and `srfi17_set` switch on SRFI 61's `=>` cond clause and
SRFI 17's `(set! (f x) v)` in a unit that imports those SRFIs' `cond` and
`set!`.

### Pruning

`srfi_prune_program` (`src/passes/srfi_prune.c`) runs in `main.c` after the
transform passes and before emission, on the whole program. It removes each
top-level definition from a `stdlib/srfi/` file that nothing outside those
files reaches, transitively. Without it, a procedure used as a value
anywhere gets a static fat box filled at startup, which keeps it and its
callees alive, and SRFI 1 cost every importing program about 1.6 s of build.
It is conservative: only `defn`s and allocation-only `def`s are removed,
C-linkage and (when a manifest is written) exported definitions stay, and an
expression kind the walk does not model makes it give up and change
nothing.

`TUR_NO_SRFI_PRUNE=1` turns it off. Because pruning runs before the split,
an SRFI's imported-library unit is per program rather than shared.

### Adding an SRFI

1. Write `stdlib/srfi/<N>.scm` as a `define-library (srfi N)`. Note the
   licence in its header and in `stdlib/srfi/COPYING`.
2. Add or change its `SRFI_LIBS` row (kind and `file`).
3. Update the SRFI table in [r7rs-guide.md](r7rs-guide.md#srfis), including
   the "Here" column.
4. Update the prelude's `r7rs-features` list if the SRFI is newly
   supported.
5. If it has C primitives, add a helper file and an `SRFI_HELPERS` row.
6. If a suite exists, add `tests/r7rs/srfi/<N>/tests.scm` and a `floor`.

`tests/check-r7rs-srfi-sync.sh` checks steps 1-4 against each other: every
importable row names an existing file holding `(define-library (srfi N)`, a
built-in row's file has no body and the others have one, every `.scm` file
has a row, the guide's table matches, and `(features)` is `R7RS_FEATURES`
followed by every supported `srfi-N`. It runs in CI's "Docs lint" job
through `tests/check-docs-lint.sh`.

---

## Generated C and its sync checks

Some C exists twice, once for each back end, and must not drift:

| Source | Copy | Generator | Check |
|---|---|---|---|
| `src/compiler/r7rs_bignum.inc`, `r7rs_numsyntax.inc` (reader and interpreter `#include` them) | file-scope C of `stdlib/r7rs/bignum.tur`, `numsyntax.tur` | `tools/gen-r7rs-inc.py` | `tests/check-r7rs-inc-sync.sh` |
| Unicode tables | `stdlib/r7rs/unicode.tur` and `src/turi/r7rs_unicode.inc` | `tools/gen-r7rs-unicode.py` | `tests/check-r7rs-unicode-sync.sh` (the two copies against each other) |

Change the `.inc` source and regenerate in the same commit. The interpreter
natives that mirror prelude inline C (the `r7k_*` helpers, the dynamic
slots) are hand-kept twins with no check. A change to preamble text also
needs [the split runtime](cps-dk-internals-guide.md#the-split-runtime)
regenerated.

---

## Tests

Do not assume a fixture run covers all of this: `tests/run.sh` builds
fixture programs without sanitizers and only compares output. See the
[running tests guide](running-tests-guide.md) for invocation.

| Harness (ctest target) | Checks |
|---|---|
| `run.sh`, `run-turi.sh`, `run-jit.sh` | the `r7rs-*` fixtures (~140 dirs) and `errors/r7rs-*` (~27) compiled, interpreted, and under `tur jit` (collector off) |
| `run-r7rs-conformance.sh` (`tur_r7rs_conformance`) | chibi-scheme's R7RS suite on both back ends; fails only below the floor (1223) or when a settled difference starts passing |
| `run-r7rs-srfi-suites.sh` (`tur_r7rs_srfi_suites_*`) | each SRFI's own suite, with a per-SRFI floor |
| `run-r7rs-gc.sh` (`tur_r7rs_gc`) | every r7rs fixture under `TUR_GC_TORTURE`; HAMT-held values; libturi not a root |
| `run-r7rs-sanitize.sh` (`tur_r7rs_sanitize`) | every r7rs fixture under ASan + UBSan (no recover) |
| `run-r7rs-import.sh` (`tur_r7rs_import`) | the Turmeric <-> Scheme seam both ways, both back ends |
| `run-init-r7rs.sh` (`tur_init_r7rs`) | `tur init --r7rs` scaffolds build, run and test |
| `tests/turi/repl-lang-r7rs.sh` (`tur_repl_lang_r7rs`) | `tur repl --lang r7rs` |
| `check-r7rs-prelude-split.sh` | no data symbol defined in two units; programs share the library object; outputs |
| `check-r7rs-split-lib-cache.sh` | warm builds read the emission cache and match a cold one; damaged entries recover |
| `check-r7rs-srfi-prune.sh` | an unused SRFI import costs nothing; only reached procedures are emitted |
| `check-r7rs-srfi-sync.sh`, `-inc-sync`, `-unicode-sync` | the tables and generated copies above |
| `check-r7rs-eval-link.sh` | `(scheme eval)` links libturi only when imported |
| `check-r7rs-tail-calls-small-stack.sh` | proper tail calls with the deep stack off, in 8 MiB |
| `check-r7rs-main-stack-size.sh` | `TUR_MAIN_STACK_MB` and the overflow message |
| `lsp_r7rs_diagnostics` | LSP diagnostics on Scheme files |

---

## Open reports

From `docs/reported/` on 2026-10-16:

- [r7rs-library-file-shape-and-export-rename](../reported/r7rs-library-file-shape-and-export-rename.md)
  -- one library per file, named by its path; held by decision.
- [r7rs-prelude-split-wrong-symbols-on-windows](../reported/r7rs-prelude-split-wrong-symbols-on-windows.md)
  -- fix landed; the split stays off on Windows until a Windows run confirms.
- [turi-r7rs-strings-and-ports-kept-until-teardown](../reported/turi-r7rs-strings-and-ports-kept-until-teardown.md)
  -- under `--interpret`, a dropped string's code points and a string port's
  buffer are kept until teardown.
- [jit-r7rs-threads-lifecycle-exited-1-once](../reported/jit-r7rs-threads-lifecycle-exited-1-once.md)
  -- one unreproduced JIT failure of `r7rs-threads-lifecycle`.

The heap-frames design for deep non-tail recursion is on hold:
[r7rs-heap-frames-plan](../upcoming/hold/r7rs-heap-frames-plan.md).

---

## Reading the archive

| Topic | Archived plan or report |
|---|---|
| everything (start here): R0-R10, T0-T8, decisions D1-D11 | [r7rs-lang-plan](../archive/r7rs-lang-plan.md) |
| the collector, then its threads | [r7rs-gc-plan](../archive/r7rs-gc-plan.md), [r7rs-gc-threads-plan](../archive/r7rs-gc-threads-plan.md) |
| SRFIs, SRFI 18/216 and `(sicp extras)` | [r7rs-srfi-plan](../archive/r7rs-srfi-plan.md), [r7rs-srfi-18-216-sicp-plan](../archive/r7rs-srfi-18-216-sicp-plan.md) |
| the split build and its caches | [r7rs-programs-compile-slowly](../archive/r7rs-programs-compile-slowly.md), [r7rs-imported-libraries-recompiled-every-build](../archive/r7rs-imported-libraries-recompiled-every-build.md), [r7rs-split-units-each-compile-the-runtime](../archive/r7rs-split-units-each-compile-the-runtime.md) |
| escape-only `call/cc`, per-form prompts, the Windows jump | [r7rs-callcc-memory-never-freed](../archive/r7rs-callcc-memory-never-freed.md), [r7rs-toplevel-reentry-reruns-forms](../archive/r7rs-toplevel-reentry-reruns-forms.md), [r7rs-reentrant-callcc-not-on-windows](../archive/r7rs-reentrant-callcc-not-on-windows.md) |

Each other `docs/archive/r7rs-*.md` report explains one rule; the code
comments usually name the report that introduced it.

---

## Maintenance

Update this guide in the same PR when you:

- add a `SCHEME_LIBS`, `ONDEMAND` or `SRFI_LIBS` kind, or a new name table;
- change a Scheme type's representation in the prelude;
- change what `prelude_split_applies`, the split modes or the caches do;
- change the continuation capture, the dynamic-environment slots or the
  collector's roots;
- add an r7rs harness or check script.

A good candidate for automation is the rename rule: a check that every
public `r7rs-` definition in the prelude has a `RENAMES` or `ONDEMAND` row
(or is on an allow-list of internals) would turn the stated rule into an
enforced one. Cite functions, not line numbers.
