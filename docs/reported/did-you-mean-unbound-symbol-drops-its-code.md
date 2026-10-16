# An unbound-symbol error with a suggestion loses its TUR-E0003 code

**Severity:** low. The same error has a code or not depending on whether a
near-miss name exists, so tools that filter by code miss it.

## Repro

```turmeric
(defn main [] : int
  (let [count 1]
    (println cuont))
  0)
```

```
$ tur check dym.tur
dym.tur:3:14: error: unbound symbol 'cuont'
dym.tur:3:14: help: Did you mean 'count'?
```

With no near-miss, the same error prints `error [TUR-E0003]: unbound symbol`.
The JSON and LSP `code` fields are empty in the suggestion case.

## Root cause

`src/compiler/diag.c:diag_emit_with_suggestion` (and `_with_notes`) take no
`DiagCode`, so callers that attach a suggestion cannot pass one.

## Fix directions

- Add coded variants (or a code parameter) and pass TUR-E0003 from the
  unbound-symbol site.
