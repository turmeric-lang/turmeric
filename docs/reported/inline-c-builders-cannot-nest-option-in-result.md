# Inline-C builders cannot return `(Result (Option T) E)`

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
