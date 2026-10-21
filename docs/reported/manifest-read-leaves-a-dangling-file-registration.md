# `pkg_manifest_read` leaves file 0 pointing into its own stack frame

**Severity:** low (latent). No known caller trips it today; one did for a
moment while fixing
[interpret-ignores-a-malformed-manifest](../archive/interpret-ignores-a-malformed-manifest.md).

## What happens

`pkg_manifest_read` (`src/compiler/pkg.c`, the `SourceFile file = {0};` before
`read_all`) builds a stack-local `SourceFile` with `file_id = 0` and passes it
to `diag_register_file`, which stores the POINTER in the global `files_[0]`.
Nothing unregisters it, and `file.src` and `file.path` point at buffers the
function also releases. After it returns, `files_[0]` names a dead frame until
the next `diag_reset()`.

Every compile entry point calls `diag_reset()` before emitting anything, which
is why this has not shown. A diagnostic at `SPAN_UNKNOWN` (file 0) emitted
between a manifest read and a reset prints `f->path` out of that frame:

```
ERROR: AddressSanitizer: stack-buffer-overflow ... READ of size 95
    #4 diag_emitv src/compiler/diag.c  (the `path` fprintf)
    #6 pkg_manifest_reassert src/compiler/pkg.c
```

That is what `pkg_manifest_reassert()` did when first called straight after
the `:experiments` read in `cmd_eval_h`; the fix there resets first, as
`compile_to_c` does.

## Fix directions

1. Unregister file 0 on every return out of `pkg_manifest_read` (a
   `diag_unregister_file`, or restore the previous `files_[0]`). Several
   returns, so a single exit label is the tidy form.
2. Or register a manifest-lifetime copy (heap `SourceFile`, duplicated path
   and source), so a later diagnostic that legitimately points into the
   manifest can still render its snippet.
