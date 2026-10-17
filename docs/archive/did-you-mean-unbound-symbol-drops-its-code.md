# An unbound-symbol error with a suggestion loses its TUR-E0003 code

**RESOLVED 2026-10-17** -- see [Resolution](#resolution-2026-10-17). Pinned by `errors/unbound-symbol-doc-url` (now also asserts `error [TUR-E0003]: unbound symbol`).

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

## Resolution (2026-10-17)

`diag_emit_with_suggestion_code` (`src/compiler/diag.c`) carries a `DiagCode`
to every sink -- the header, JSON and the LSP -- and `diag_emit_with_suggestion`
is a wrapper passing `DIAG_CODE_NONE`. Both unbound-symbol sites in
`elab_toplevel.c` (the did-you-mean and the r7rs leading-colon hint) pass
TUR-E0003. The two "unknown function or operator" suggestion sites in
`elab_call.c` stay uncoded, as their suggestion-less sibling is.
