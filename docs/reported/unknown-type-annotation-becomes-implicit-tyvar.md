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
