# Plan: spell type-declaration flags as `^attr`, not `:keyword`

> **Status:** DESIGN SETTLED, 2026-10-27; nothing implemented. The three open
> questions are decided (see "Decisions"): attributes go before the name,
> `^move` stays, and `^heap` moves with the rest. P0 is next. A spelling change
> only: every form here means exactly what it means today, and the checker's
> rules for linear, affine, copy and heap values do not move.

## The problem

The same word has two spellings depending on where it sits:

```turmeric
(defopaque Chan : ptr<void> :linear)        ; every Chan is linear
(defn close [^linear c : Chan] : unit ...)  ; this binding is linear
(defn open [] : ^linear Chan ...)           ; this result is linear
(exists :linear [a] [(Show a)] a)           ; this existential is linear
```

There are two conventions for flags on a definition, and the type declarations
are the only forms that use keywords:

| Form | Flags today | Where |
| --- | --- | --- |
| `defn` | `^deprecated ["msg"]`, `^reflect`, `^construct`, `^byval`, before the name | `elab_defn_name_index`, `src/compiler/elab_toplevel.c:2767` |
| `def` | `^mut`, `^persistent`, `^private`, before the name | `src/compiler/elab_forms.c`, `elab_fns.c:15681` |
| `defeffect` | `^private` before the name; `^extends Parent`, `^capability` after the signature | `src/compiler/elab_effects.c:1455`, `:1654` |
| `defdynamic` | `^private` before the name | `src/compiler/elab_dynvars.c:71` |
| `match` | `^non-exhaustive` | `src/compiler/elab_structs.c:3576` |
| parameters, `let`, return types | `^linear`, `^affine`, `^relevant`, `^unique`, `^mut`, `^borrow`, `^fat` | `elab_fns.c:9995`, `elab_types.c:1915` |
| **`defopaque`** | **`:linear`, `:affine`, `:sealed`, `:non-null`, after the base type** | `src/compiler/elab_structs.c:1010` |
| **`defstruct`** | **`:copy`, `:move`, `:linear`, `:heap`, `:no-auto-ctor`, after the name** | `src/compiler/elab_structs.c:690` |
| **`defdata` / `defgadt`** | **`:copy`, `:move`, `:linear`, `:affine`, `:heap`, `:no-auto-ctor`, after the name** | `src/compiler/elab_structs.c:1789` |
| **`exists`** (type) | **`:linear`** | `src/compiler/elab_types.c:1429` |

Three costs follow from that:

1. **A reader has to learn two spellings for one idea.** `^linear` and `:linear`
   both mean "used exactly once". The difference is only how far the claim
   reaches (one binding, or every value of the type), and the form already says
   that.
2. **A flag and a type look identical.** In `(defopaque Chan :ptr<void> :linear)`
   the first keyword is a type and the second is a flag. Nothing lexical tells
   them apart, so every editor pack has to guess
   ([editor-packs-do-not-colour-annotations-and-generic-types](../reported/editor-packs-do-not-colour-annotations-and-generic-types.md),
   "An open design question"). A `^linear` is already one token with one
   meaning, and Try Turmeric colours it as a modifier.
3. **The type position is split.** `: ^linear Chan` is legal as a return
   type, while the type form `exists` takes `:linear`.

## The rule this plan adopts

- **`^name` is an attribute of the thing being declared or bound.** Most take
  no argument. A few take one (`^deprecated "msg"`, `^extends IO`), and that
  stays as it is.
- **`:name` is data or a named argument**: map keys, `:refer [...]`, manifest
  keys, and the type annotation `: T`.

Under that rule, every flag on a type declaration is a `^` attribute.

## Target syntax

The attributes go **before the name**, as on `defn`, `def`, `defdynamic` and
`defeffect`'s `^private`:

```turmeric
(defopaque ^linear Chan : ptr<void>)
(defopaque ^affine ^sealed Future : ptr<void>)
(defopaque ^non-null Handle : ptr<void>)
(defstruct ^copy Point [x : int y : int])
(defstruct ^linear Socket [fd : int])
(defstruct ^heap Node [next : ptr<Node> val : int])
(defdata ^copy Shape (Circle int) (Square int))
(defdata ^linear Token [A] (Token A))
(exists ^linear [a] [(Show a)] a)
```

In sweet-exp the same: `defopaque ^linear Chan : ptr<void>`.

The full set is `^copy`, `^move`, `^linear`, `^affine`, `^heap`, `^sealed`,
`^non-null` and `^no-auto-ctor`. On a declaration they mean exactly what the
keyword means today. The mutual exclusions carry over: `^linear` with
`^affine`, and `^copy` with `^move`.

### Why before the name, not in the keyword's old slot

- **A type-parameter vector already reads `^` as a kind.** `[^f a]`, `[^^f]`
  and `[^&cols]` declare kinds (`elab_structs.c:1821`), so any slot next to the
  vector is a trap. Before the name never touches the vector.
- **It matches the definitions that already take `^`.** `(defn ^deprecated f ...)`
  and `(def ^mut x ...)` put the attribute first. That is the convention a
  reader has already met.
- **It reads the way the type position does.** `(defopaque ^linear Chan ...)`
  and `: ^linear Chan` say the same thing in the same order.

The alternative is to keep the slot and change only the sigil, as in
`(defopaque Chan : ptr<void> ^linear)`. That is the cheaper migration, since no
tool that finds a declaration's name has to change. It was rejected
(2026-10-27) because
`defstruct` and `defdata` put their flags next to the type-parameter vector,
so the kind collision above would be one typo away, and because it keeps the
type declarations unlike every other definition. `defeffect`'s trailing
`^extends` / `^capability` are attributes of the effect's signature and stay
where they are.

## Phases

### P0 -- one shared parser, accepting both spellings

- Add `sym_caret_copy`, `sym_caret_move`, `sym_caret_heap`, `sym_caret_sealed`,
  `sym_caret_non_null` and `sym_caret_no_auto_ctor` to the interned table
  (`elab_core.c:2493`). `^linear` and `^affine` already exist.
- Write one helper, say `elab_type_decl_attrs(e, form, &idx, &out)`. It reads
  the leading `^` attributes into a flags struct and reports an unknown `^name`
  or a contradictory pair, naming the declaration form. `elab_defstruct`,
  `elab_defdata` / `defgadt` and `elab_defopaque` all call it. The trailing
  keyword loops stay for now and fill the same struct, so both spellings reach
  identical code.
- Add one helper that returns the name index of a type declaration, as
  `elab_defn_name_index` does for `defn`. Every pre-pass that reads the name
  calls it. The comment at `elab_toplevel.c:2759` records the bug this
  prevents: a module pre-declare scan that skipped none of the `^` attributes.
  Known callers:
  - the forward-type pre-pass, `elab_toplevel.c:3396` (reads `items[1]`);
  - `elab_defstruct` lowering to `defdata`, `elab_structs.c:895`. It can forward
    the parsed flags rather than rebuilding keyword forms;
  - `scheme_lower.c:5247` (`TYPE_HEADS`);
  - the hoisting of nested type definitions, `elab_call.c:5041`.
- `exists` accepts `^linear` beside `:linear` (`elab_types.c:1429`).
- **Fix one gap on the way.** `defstruct`'s keyword loop has no `:affine`,
  though `defdata` and `defopaque` do. From reading the loop, an `:affine`
  there ends the loop and is then parsed as part of the fields. With one
  shared parser, `^affine` works on all three. Pin it with a fixture.

**No `--enable` gate.** This adds a second spelling that elaborates to the
same thing. It has no semantics in flux and no cost to impose, which is what
the experiment rule in CLAUDE.md is for. If review disagrees, an
`EXPERIMENTS[]` row is cheap to add.

**Fixtures** (new):
- `caret-type-attrs` runs every attribute on every form. Its output matches
  the keyword spelling of the same program.
- `errors/caret-type-attrs-linear-and-affine` and
  `errors/caret-type-attrs-unknown` pin the diagnostics.
- `caret-type-attrs-defstruct-affine` pins the gap fixed above.
- `caret-exists-linear` pins the type form.

### P1 -- tools that read declarations

Each of these finds a declaration by its head and then its name, or reads the
flags. Each has to skip or understand the leading `^` attributes:

- `tools/gendocs.py`: `def_re` (line ~593), plus the `struct_ann` detection for
  `defstruct` (~1107) and `defopaque` (~1113). The rendered declaration shows
  the attributes as written.
- `src/lsp/lsp_docs.c`, `src/cli/lsp_lite.c:259` and `src/turi/docstrings.c:242`,
  for the signature strings.
- `src/compiler/fmt.c`. It must keep the attributes on the head line, and must
  not reflow them under the name.
- `web/main.js` `CARET_MODIFIERS` and the matching list in
  `tools/genguides.py`. **These lists are required, not cosmetic.** Any `^name`
  not on them is coloured as a kind, so `^copy` would read as a type variable
  until it is added.
- The editor-packs report gains a line: the new attributes join each pack's
  modifier set.

### P2 -- migrate the tree

A rewrite tool in the style of `tools/spaced-types-rewrite.py`, which moves the
trailing keywords to leading `^` attributes. It works on forms rather than
lines, since many declarations span lines. Rough counts of single-line forms
today, across `.tur`, `.sweet` and `.md`:

| Keyword | Sites |
| --- | --- |
| `:copy` | ~600 |
| `:heap` | ~260 |
| `:linear` | ~70 |
| `:move` | ~65 |
| `:affine` | ~17 |
| `:non-null` | ~15 |
| `:sealed` | ~12 |
| `:no-auto-ctor` | 3 |
| `exists :linear` | 6 |

Run it over `stdlib/`, `tests/fixtures/`, `docs/guides/` and `examples/`, then
regenerate the codegen snapshots in the same PR. The spelling does not reach
emitted C, so there should be no snapshot churn, and a moved snapshot means
the rewrite changed something it should not have.

Update these by hand:
- the `;;@form` lines and docstrings in `stdlib/forms/datatypes.tur` (`defstruct`,
  `defdata`, `defopaque`), then `tur run docs`;
- `substructural-types-guide`, `opaques-guide`, `structs-guide`,
  `sum-types-guide`, `gadts-guide` and `type-annotations-guide`. The
  substructural guide gains a short section that says one spelling now covers
  bindings and types;
- `turmeric-spices`, separately, once P3 ships a release that warns.

### P3 -- warn on the keyword spelling

A new `TUR-W` warning on a trailing flag keyword on `defopaque`, `defstruct`,
`defdata`, `defgadt` or `exists`. It names the `^` spelling, for example
"write `(defopaque ^linear Chan ...)`". `tur fmt`, which formats in place,
learns the same rewrite as the P2 tool, so a user's own code migrates with one
command. The warning lives for at least one minor release.

### P4 -- drop the keyword spelling

Remove the keyword loops. A trailing `:linear` is then an error that names
the `^` form, because the declaration's arity no longer fits.

## Out of scope

- **Any change to what the attributes mean.** That includes how an `^affine`
  binding of a `^linear` type is checked, whether `^relevant` / `^unique`
  should exist on type declarations, and whether a `^linear` type with a
  `^copy` binding is an error. Those questions predate this plan, and folding
  them in would turn a mechanical rename into a semantics change. Note them as
  follow-ups if P0 surfaces them.
- **Keywords that carry a value**: `:refer [...]`, manifest keys such as
  `:spices` and `:build-dir`, map literals, `:as`. Those are named arguments.
- **`defeffect`'s trailing `^extends` / `^capability`**, which are already
  `^`.
- **Inline-C and `extern-c` C types such as `^int`.** They are already `^`, and
  their meaning (a C type) is separate.

## Decisions (2026-10-27)

1. **The attributes go before the name.** The trailing slot was rejected for
   the reasons in "Why before the name". So the name-finding helper and the
   tooling work in P1 are in scope.
2. **`^move` stays.** It is the default, so writing it is optional, but a
   declaration can still state its kind explicitly, as the guides' tables do.
   It is a full member of the set, not a deprecated alias: the P3 warning
   rewrites `:move` to `^move` and does not drop it.
3. **`^heap` moves with the rest.** It is about representation, not about how
   often a value is used, but it is a flag with no value on the declaration, so
   the rule puts it on `^`. The change covers every flag on a type
   declaration, not only the substructural ones.
