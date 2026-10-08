# Inline-C builders cannot return `(Result (Option T) E)`

**RESOLVED 2026-10-07** -- the composition the report tried now works, with no
new builders and no diagnostic needed. The builders' box is `{tag; word}`, and
in `tur_ok_int(tur_some_ptr(s))` the word is the inner builder's box; the fault
was only the readback, which reinterpreted the outer box as the monomorph's
by-value layout. `emit_readback_nested_sum_box` (`src/compiler/emit_core.c`)
now converts an inline-C box whose type's payload is a (non-heap) sum field by
field: it copies the outer tag, sends each nested payload word through the
carrier->concrete bridge as an owned box of its own -- which recurses for
deeper nesting, unwraps a niche Option, and frees that box -- and copies any
other payload exactly as the plain readback did.

It runs on the three readback paths an inline-C sum result takes: the owned
Option-shaped and Result readbacks (a `match` or `let` on the call), and a
fresh box that RM1 frees after the call it is passed straight into (the
`sum_pending` list -- `(show (rl 1))`). Anything else keeps the plain deref:
a box with no nested sum payload, a constructor with more than one field, and
every carrier whose box is a spilled by-value aggregate (whose layout is the
monomorph's). No fixture snapshot changed, byte for byte: the new arms
allocate their temps only once they know they apply.

Pinned by `tests/fixtures/inline-c-result-nested-sum` (`requires.leak-check`):
`(Result (Option cstr) E)` matched, let-bound and passed as an argument,
`(Option (Result int cstr))`, `(Result (Result int cstr) E)` and
`(Result (Option (Option int)) E)`, every arm. Clean under memcheck and
`tests/run-leak-check.sh`, and the JIT slice agrees. Documented in
`docs/guides/inline-c-results-guide.md` ("Nesting the builders").
`file-read-line`'s errno-0 workaround in `stdlib/io.tur` still works and was
left alone.

**Severity:** low-medium (silent wrong behaviour when attempted).

## Summary

`tur_ok_int` / `tur_ok_ptr` put a one-word carrier in the ok slot. A
monomorphised `(Result (Option cstr) E)` stores its `Option` payload BY VALUE
(`struct { tur_adt_Option__cstr _0; } Ok;`), so an inline-C body returning
`tur_ok_int(tur_some_ptr(s))` hands back a box whose inner tag is read from
the low bytes of a pointer. Neither arm of a nested `match` fires and nothing
is reported.

## Repro

```turmeric
(defopaque IoError :int)
(defn rl [n : int] : (Result (Option cstr) IoError)
  ```c
  if (n == 0) return tur_ok_int(tur_none());
  return tur_ok_int(tur_some_ptr(strdup("line")));
  ```)
(defn main [] : int
  (match (rl 1)
    (Ok o) (match o (Some s) (println s) (None) (println "eof"))
    (Err e) (println "err"))
  0)            ; prints nothing
```

## Workaround in tree

`file-read-line` (stdlib/io.tur) returns through a private inline-C helper
typed `(Result cstr IoError)`, encoding end-of-file as an errno-0 error, and
builds the `(Ok (Some s))` / `(Ok (None))` nesting in Turmeric.

## Fix directions

Either a builder that knows the by-value payload layout (e.g. a
`tur_ok_option_ptr(p)` family), or a diagnostic when an inline-C body's
declared return nests a sum type inside a sum type, pointing at the
workaround.
