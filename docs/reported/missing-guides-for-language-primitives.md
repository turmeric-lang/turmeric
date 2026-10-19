# Several groups of language primitives have no user-facing guide

**Severity: low-medium (documentation).** Nothing is broken, but a user who
meets one of the forms below has no guide that explains it. Most of them appear
only in `docs/guides/syntax-guide.md`'s "Reserved names" table (names, no
semantics), in an internals guide written for compiler work, or in one example
written for another purpose.

Found 2026-10-19 while documenting the compiler builtins for
`api-docs-miss-language-primitives` (task 1: the API reference). That report is
about the per-form *reference*; this one is about the *guides* -- the prose
that says when to reach for a form and what to watch for.

## How this was measured

Every name in `reserved_special_forms_` (`src/compiler/elab_call.c:358`) and
every public name in the builtins table (`src/compiler/builtins.c:11`) -- 163
names -- was searched across the 174 guides in `docs/guides/` as code: a call
`(name ...` or a backticked `` `name` ``. syntax-guide's Reserved-names table
was discounted (it lists names only), and each remaining hit was read, since
counts mislead: `weak` matches "weak symbol" and `->` matches every type arrow.

```sh
grep -rnE '\((ptr-add|transmute|panic-with|\+\.) ' docs/guides/*.md   # nothing outside syntax-guide
```

## The gaps

### 1. Unsafe code -- no guide

`unsafe`, `ptr-deref` `ptr-write` `ptr-add` `ptr-sub` `ptr-null?` `ptr-of`,
`unsafe-cast` `reinterpret` `transmute`, `array-get-unchecked`
`array-set-unchecked`, `raw-malloc` `raw-free` `raw-realloc` `raw-memcpy`
`raw-memset`, `c-call`.

What exists: effect-rows-internals-guide explains how the `Unsafe` effect is
checked; ffi-guide uses `ptr-deref` and `raw-malloc` in two examples;
sandboxing-guide lists them under `TURI_CAP_UNSAFE`; polymorphism-guide's
"unsafe escape hatch" shows inline C only. `ptr-add`, `ptr-sub`, `ptr-of`,
`unsafe-cast`, `reinterpret`, `transmute`, `raw-free`, `raw-realloc` and
`raw-memcpy` have no mention outside the reserved-names table.

What a guide has to say, from `src/compiler/elab_unsafe.c` and
`emit_builtin` (`src/compiler/emit_core.c`): each form requires an enclosing
`(unsafe ...)`; `ptr-deref`, `ptr-write` and the unchecked array ops read and
write 8-byte words (`*(int64_t *)p`, `((int64_t *)p)[i]`); `ptr-add` /
`ptr-sub` offsets are in bytes; `unsafe-cast` and `reinterpret` ignore their
target type and yield `:int` (`(int64_t)x`) -- "for v1", per the source --
while `transmute` checks that the sizes match and yields the target type; a
`raw-malloc` result is move-typed (ffi-guide:323).

Where: a new `docs/guides/unsafe-guide.md`, linked from ffi-guide and
c-integration-guide.

### 2. Operators -- no guide

`+ - * / mod`, `= not= < > <= >=`, `and or not`, the float-only `+. -. *. /.`,
and `bit-and bit-or bit-xor bit-shl bit-shr`.

What exists: numeric-tower-guide's "Operators come from the `Num` typeclass"
covers `Rational` and `Complex` only; sized-primitives-guide uses the `bit-*`
ops in passing. `+.`, `-.`, `*.` and `/.` are mentioned by no guide at all.

What a guide has to say: which types each operator accepts (int, float,
float32, the sized integers; `=`/`not=` also on bool and `Sym`); `/` aborts on
an integer division by zero but follows IEEE 754 for floats
(`builtin_div_is_ieee`); `and`/`or` short-circuit; `bit-shr` is a logical
shift, not arithmetic (`emit_builtin`'s `>>` case).

Where: an "Operators" section in numeric-tower-guide, or a short
operators-guide it links to.

### 3. The built-in reference, rc and weak forms

`ref` `deref` `drop!` `ref?` `lref/new`, `rc/of` `rc/clone` `rc/drop`
`rc/strong-count` `rc->ptr` `rc/from-ref` `ref/from-rc`, `weak` `upgrade`
`weak?`.

What exists: ownership-guide teaches `rc<T>` and `weak<T>` through
`stdlib/weak.tur`; c-integration-guide ("Reference counting", "Weak pointers")
covers the control block as C sees it; emitted-code-ownership-guide shows
`rc/drop` and `drop!` in generated code. The builtin `weak`, `upgrade`, `weak?`,
`rc->ptr`, `rc/from-ref` and `ref?` have no user-facing mention, and nothing
says when to use the builtins rather than `stdlib/weak.tur`.

Where: a section in ownership-guide (`lref/new` may belong in structs-guide,
which already shows it).

### 4. Panic payload forms

`panic-with`, `panic-payload-type`, `panic-payload-value`,
`panic-payload-file`, `panic-payload-line`, `panic-payload-downcast`.

What exists: error-handling-guide's "Inspecting the panic" documents
`stdlib/panic.tur`'s `Panic` wrapper (`result-panic`, `panic-message`,
`panic-file`, ...). The compiler forms themselves are mentioned by no guide.

Where: a short section in error-handling-guide saying what each form is and
which layer a program should use.

### 5. Effect-handler variants

`handle-shallow`, `try-with`, `discontinue`, `compose-handlers`.

What exists: effects-system-guide mentions `handle-shallow` in one table row
("A deep handler re-installs itself on resume; `handle-shallow` does not") and
`compose-handlers` twice; `try-with` appears only in async-await-guide;
`discontinue` only once in delimited-control-operators-guide. The real
explanations are in cps-dk-internals-guide and effect-rows-internals-guide.

Where: a section in effects-system-guide (shallow vs. deep handlers, aborting a
continuation, composing handlers).

### 6. Smaller holes

- **Threading macros** `->` / `->>`: used in cli-args-guide's examples
  (`(-> (args/spec-new) ...)`) and explained by no guide. macros-guide or
  syntax-guide.
- **`let*`**: binding-forms-guide covers `let`, `letrec` and named `let`, not
  `let*`.
- **`defrec`**: internals guides only (`elab_types.c` notes its body is ignored
  in v1). hkt-guide, beside `defkind`.

## Fix directions

Write the guide or section named under each gap, add new guides to
`docs/guides/README.md`, and pair each `turmeric` example with its `sweet-exp`
sibling as the other guides do (`tur run check-guides`). Once
`api-docs-miss-language-primitives` task 1 lands per-form reference pages
(`stdlib/forms/`), link each guide section to them rather than restating
signatures.
