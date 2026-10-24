# Several readers still leave diag file 0 pointing at their own dead stack frame

**Severity:** low (latent). No caller is known to trip any of these today:
every path checked emits nothing between the read and the next `diag_reset()`.
Filed 2026-10-24 so that the residue of the resolved
[manifest-read-leaves-a-dangling-file-registration](../archive/manifest-read-leaves-a-dangling-file-registration.md)
has an open row. Until now it was listed only in that archived report's last
paragraph, and that list is partly wrong (below).

## The shape

The reader builds a stack-local `SourceFile` with `file_id = 0` and passes it
to `diag_register_file`, which stores the pointer in `files_[0]`
(`src/compiler/diag.c:48`). It then returns without unregistering. Until the
next `diag_reset()`, any diagnostic at file 0 (including `SPAN_UNKNOWN`) prints
its path and snippet from the dead frame. That read is the ASan
stack-buffer-overflow the archived report shows.

## Where it still happens (checked 2026-10-24 on `main` acd35812)

| Site | Unregisters? | Callers, and why they do not trip it |
| --- | --- | --- |
| `pkg_lock_read`, `src/compiler/pkg.c:2035` | no | `cmd_run.c:493`, `project.c:1026` / `:1065`, `pkg.c:7333`: setup steps whose follow-ups (`pkg_verify_locked_spices`, `pkg_fetch_all`, `pkg_cmake_build`) report through `fprintf`, not diag, before the compile resets the table |
| `fmt_format_buffer`, `src/compiler/fmt.c:2214` | no | `tur fmt`, `script_deps.c:1052`, and the LSP's `textDocument/formatting` (`lsp.c:2172`). Every later LSP analysis calls `diag_reset()` first (`lsp.c:354`, `:480`, `:1618`) |
| `cmd_format` / `parse_check_read`, `src/driver/cmd_format.c:174` / `:334` | no | command-level; the process ends |
| `cmd_explain`, `src/driver/cmd_misc.c:985` | no | command-level |
| `compile_to_c` / `_h` / `_implementation`, `src/driver/pipeline.c:584` / `:914` / `:1019` | no | the frame is live for the whole compile; afterwards, each next compile resets first |

**Not this shape, despite the archived note:** `global.c`'s two readers
(`tur_state_read_clean` `:261`, `tur_script_index_read_clean` `:693`) are only
called through `tur_state_read` / `tur_script_index_read` (`:216`, `:732`).
Those wrappers save the table, reset it, and restore it with
`diag_files_replace`, so nothing dangles. `pkg_script_header_read`'s id-0
registration (`pkg.c:1557`) already unregisters (`:1630`).

## Fix directions

- One helper for "read a side file on a clean slate". The save / reset / read /
  reset / restore sequence is written out three times already (`global.c:216`,
  `:732`, `turi/env.c` `env_diag_save`/`_restore`). Making it a single diag
  function and calling it from `pkg_lock_read` and `fmt_format_buffer` fixes the
  two sites reachable from a long-lived process (`tur run`, the LSP).
- Or `diag_unregister_file(&file, prev)` on each return, as
  `pkg_manifest_read_status` does.
- Leave the command-level ones (`cmd_explain`, `cmd_format`) as they are.
