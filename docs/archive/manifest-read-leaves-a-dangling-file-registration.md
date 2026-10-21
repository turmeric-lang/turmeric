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

## Resolved 2026-10-21

Fix direction 1. `diag_unregister_file(file, prev)` (`src/compiler/diag.c`)
forgets `file` if its id still names it and puts back what the id named
before. `pkg_manifest_read_status` records `diag_source_file(0)` before
registering its stack-local `SourceFile` and calls the unregister on each of
its three returns past the registration, so the registry never names the
dead frame. A diagnostic at `SPAN_UNKNOWN` emitted after a manifest read
prints no path (or the caller's own file 0), instead of reading the frame.

The other stack-local registrations at id 0 (`pkg_lockfile_read` and the
`:experiments` read in `pkg.c`, `global.c`, `fmt.c`, the driver's single-file
paths) have the same shape and were not changed here; no caller is known to
trip them, for the reason above.
