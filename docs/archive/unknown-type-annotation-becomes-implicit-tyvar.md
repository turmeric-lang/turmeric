# A misspelled type annotation silently becomes a type variable

**Severity: low (policy decision).** Nothing is miscompiled, but a typo in a
type annotation is accepted, and nothing about the parameter is checked.
This is how `cont<int>` went unnoticed until
[cont-angle-bracket-annotation-becomes-tyvar](../archive/cont-angle-bracket-annotation-becomes-tyvar.md)
fixed that one spelling. Filed 2026-10-13.

## Repro

```turmeric
(defn h3 [k : zork] : int 1)     ; lowercase, used once
(defn h4 [k : Zork] : int 1)     ; capitalized too
(defn main [] : nil (println (+ (h3 4) (h4 5))))   ; prints 2, no diagnostic
```

## Root cause

This is by design. An unknown name in an annotation is an implicit type
variable: "an unknown keyword is an implicit type variable", Phase HRT/G2,
at `src/compiler/elab_fns.c` in the defn parameter ladder, and the bare-symbol
fallback in `type_expr_from_form` (`src/compiler/elab_types.c`).

The KB-026 pass (`elab_fns.c`, "gate GS4 implicit type variables") was meant
to catch the typo case: a name that occurs in only one type position.
However, it demotes that name to another named type variable rather than
reporting it. Its own comment says the lone `[n : nope]` "should stay an
unresolved type so the ... hint can fire".

`e->strict_unknown_types` exists and turns the fallback into an error, but
only inside a `#row{...}` element.

## Fix directions

1. **Warn on a single-occurrence unknown name in a signature**, the case
   KB-026 already identifies, and suggest the nearest known type. That keeps
   genuine implicit type variables, which relate two positions or are
   declared, working unchanged.
2. Or require implicit type variables to be declared (`[A]`) and make the
   fallback an error under a flag first. That is a bigger change to the GS4
   surface.

Either way, run the corpus first. Single-occurrence unknown names may
already exist in fixtures or spices as deliberate erased parameters.

## 2026-10-17: `option<T>` is the case most people will hit

The spelling the project's own prose uses for these types is not a type in an
annotation. Measured on v0.65.5 with `tur check`, each of these parameter
annotations accepts a `cstr` argument with no diagnostic:

| Annotation | Result |
| --- | --- |
| `[x : option<int>]` | accepted -- implicit type variable |
| `[x : Option<int>]` | accepted -- implicit type variable |
| `[x : vec<int>]`, `[x : Vec<int>]` | accepted -- implicit type variable |
| `[x : (Option int)]` | `TUR-E0001: expected (Option int), got cstr` |

As a typed `def`, `(def ^mut s : option<Ctx> (None))` is accepted, but the
global's type is then a type variable. Every later `(set! s (Some c))` fails
with `value type (Option Ctx) does not match binding type tyvar`, and
`(match s ...)` fails with `scrutinee must be an ADT type, got tyvar`.
`defdynamic` reports the same annotation as `TUR-E0603` (substructural), which
sends the reader looking in the wrong place.

No `.tur` source in either repository writes the angle-bracket spelling today.
It appears only in prose: CLAUDE.md uses `option<T>` / `result<T,E>` seven
times, and several guides use `Option<int>`. Prose is where new code copies its
spellings from. So direction 1 above has a concrete first target: when the
unknown head of an angle-bracket annotation is `option`, `Option`, `result`,
`Result`, `vec` or `Vec`, suggest the s-expression spelling `(Option T)`.

## Resolved 2026-10-21: fix direction 1, as a warning

The KB-026 pass (`src/compiler/elab_fns.c`), which already identifies a
parameter annotation naming no known type that appears nowhere else in the
signature, now emits **TUR-W0045** there before demoting the name to an
unresolved type variable. The message names the parameter and the spelling;
when the name is an angle-bracket form whose head is `option` / `Option`,
`result`, `vec` or `list`, it suggests the s-expression spelling `(Option T)`
outright. `tur explain TUR-W0045` has the long form. Nothing else changes: a
declared type parameter, a name that relates two positions, an ADT's own
parameter, and anything inside the stdlib preload stay quiet, and the program
still compiles as before. Pinned by
`tests/fixtures/unknown-type-annotation-warns` (a typo, `option<int>`, and
the three quiet shapes). Direction 2 (an error under a flag) was not taken.
