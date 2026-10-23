# The advanced type system rationale teaches syntax the compiler rejects

**Severity: low-medium (docs)** -- `docs/guides/advanced-type-system-rationale.md`
is the "Advanced Types" entry in the guide index. Its effect-row example uses
the retired `@ {Io}` syntax, and the union, intersection and existential
examples do not compile as written. A reader who copies any of them gets an
error. Some of its prose also describes features the compiler does not have.

**Status: OPEN.** Filed 2026-10-23, checked with a Release `tur` built from
turmeric `main` @ `43bee293`.

## Examples that do not compile

| Line | Written | What `tur check` says | Current form |
| --- | --- | --- | --- |
| 151, 160 | `(defn read-file [path : cstr] : cstr @ {Io} ...)` | `effect annotation '#{Io}' must precede the return type` | `(defn read-file [path : cstr] #fx{IO} : cstr ...)`. The effect is `IO`: `#fx{Io}` is TUR-E0026, "did you mean 'IO'?" |
| 155, 163 | `: int @ {Ask \| e}` | after moving it before the return type: TUR-E0026, `'\|' in effect row is neither an effect nor a row variable` | `#fx{Ask e} : int`. Row variables are written without `\|` |
| 194, 205 | `[x :(int \| cstr \| bool)] : unit` | `unsupported return type keyword 'unit'` | `: nil`, as the [union-intersection guide](../guides/union-intersection-types-guide.md) writes it |
| 196-198, 207-211 | match arms `(i :int)`, `(s :cstr)`, `(b :bool)` | `match on union type: expected '(varname : Type)'` | `(i : int)`, with the space. The sweet-exp side also puts each pattern and its body on separate lines |
| 201, 214 | `[^Serializable ^Printable x : a]` as an intersection | -- (not the intersection syntax) | `(A & B)`, e.g. `[x : (int & Serializable)]`, per the union-intersection guide |
| 365-366, 373-374 | `(defn box-it [x : a] :(exists [a] [(Show a)] a) (pack x ...))` | `pack: no instance 'Show' for type 'tyvar'` | `x` is an unconstrained type variable, so nothing proves `Show a` at the pack site. Pack a concrete type, as the [existential guide](../guides/existential-types-guide.md) does (`(pack 42 ...)`) |

The session-types example (lines 86-109) does compile.

## Prose that is out of date

- **Effect rows.** Line 170, "An effect row `{Io | e}`", has the same old
  spelling as the examples: it should be `#fx{IO e}`.
- **Sized types and `alloca`.** Lines 282-285 say the elaborator "emits
  `alloca` instead of `malloc`" when a size is static. Nothing in
  `src/compiler/` emits that for a sized type. The only `alloca(` in the
  compiler is a name buffer in `elab_forms.c`.
- **Sized types and FFI.** Lines 287-289 say an `extern-c` declaration "can
  annotate a C struct with its Turmeric-side sized type", and the elaborator
  then checks the layout at import time. No such check exists.
- **Effect rows and Rank-2.** Line 179 says effect polymorphism is
  "the `forall [e]` quantifier". An effect-polymorphic function is written
  with a lowercase row variable in `#fx{...}`. Check whether the Rank-2
  claim still holds before keeping it.

Still accurate, checked:

- The `-X` flags (`-Xlinear`, `-Xsessions`, `-Xeffect-types`) are accepted
  and warn (TUR-W0050).
- `--keep-contracts` and `set-contract-handler!` / `with-contract-handler`
  exist.
- `StaticInt` exists.

## Fix directions

1. Rewrite the examples in the table to the current forms, keeping the
   turmeric/sweet-exp pairs in step, and check each with `tur check`. The
   guide-pairs check only proves the two sides parse alike, not that either
   one typechecks.
2. Cut the `alloca` and `extern-c` layout paragraphs, or reword them as
   intent and link a plan.
3. Read the rest of the prose against the
   [effects guide](../guides/effects-system-guide.md) and the
   [sized-types guide](../guides/sized-types-guide.md).
