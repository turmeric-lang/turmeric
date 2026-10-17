# Diagnostics comments, and one user-visible explanation, that disagree with the code

**Severity:** low, except item 2, which is low-medium. Item 2 is **user-visible**:
`tur explain TUR-D0001` describes a promotable warning, but the code is always
a hard error. Filed 2026-10-17 from the "Trust the code over the comments" note
in [diagnostics-internals-guide](../guides/diagnostics-internals-guide.md).
Checked against `main` 9ac3cd43; line numbers are from that commit.

The guide names two topics: the deprecation band's severity, and which
prefixes `tur explain` accepts. Both have several stale comments.

## Changes

### 1. The deprecation band's severity rule (`diag.h:301-303`)

- **Says:** "Deprecation band (TUR-D####): ... Emitted as DIAG_WARNING;
  promoted to DIAG_ERROR under --Werror=deprecated."
- **Code:** each call site picks its own level, and the four codes all differ
  from that rule:

  | Code | Emitted | `--Werror=deprecated` |
  |---|---|---|
  | `TUR-D0001` | `DIAG_ERROR` always (`elab_types.c:735`, `reject_fn_type_colon`) | n/a |
  | `TUR-D0002`, `D0003` | `DIAG_WARNING` always (`elab_effects.c:74`, `warn_legacy_fx_row`) | **not** promoted |
  | `TUR-D0004` | warning (`elab_structs.c:3624`) | promoted |

  The uncoded "'x' is deprecated" use-site warning (`elab_module.c:2182`) is
  also promoted.
- **Change (comment):** state the per-site reality, the way the guide does.
- **Change (structure, optional; behaviour decision):** add one helper,
  `diag_deprecation_level()` (returns `g_werror_deprecated ? DIAG_ERROR :
  DIAG_WARNING`), and call it from `elab_structs.c`, `elab_module.c` and
  `warn_legacy_fx_row`. The band comment would then become true for every
  code except D0001, which stays a hard error and should say so in its own
  enum comment. This makes `--Werror=deprecated` promote D0002/D0003, which
  changes behaviour, so it is the maintainer's call. If D0002/D0003 should
  stay unpromotable, say why at `warn_legacy_fx_row`.

### 2. `tur explain TUR-D0001` describes the code as a promotable warning (`diag.c:2664-2684`)

- **Says (user-visible text):** "leading colon inside a (fn ...) type is
  deprecated ... is being phased out ... Example triggering this warning ...
  Promoted to an error under --Werror=deprecated."
- **Code:** since fn-type-bare-identifier-plan Phase 4 this is a hard error
  with no flag involved (`elab_types.c:660-664` explains that the code kept
  its `D` prefix so tooling keeps working). The diagnostic itself says "no
  longer accepted".
- **Change:** reword the explanation as an error. For example: "TUR-D0001:
  leading colon inside a (fn ...) type is no longer accepted ... (it was a
  deprecation warning in v0.18/v0.19; the code keeps its D prefix)". Drop the
  `--Werror=deprecated` sentence. Also update the enum comment at
  `diag.h:304-307` ("Phase 3 ... Drop the colon") to say it has been an error
  since Phase 4.
- **Test:** no test pins this text today. `tests/run-flags.sh` has `tur
  explain` checks only for `E0012` and `E0013`. A one-line check that the D0001
  explanation does not say "warning" would keep this from drifting back.

### 3. Comments that say `tur explain` takes only `TUR-E` codes

`diag_looks_like_code` accepts `E`, `W` and `D` (`diag.c:455-467`, and its own
comment is right). These comments say otherwise:

| Where | Says | Change |
|---|---|---|
| `main.c:12636-12637` (`looks_like_diag_code_`) | "the form "TUR-E" followed by one or more decimal digits" | see the structural note below |
| `main.c:13097-13098` (`cmd_explain`) | "If the argument looks like a diagnostic code (TUR-E####)" | "(TUR-E/W/D####)" |
| `diag.h:957-958` (`diag_code_from_string`) | "Parse a TUR-E#### string" | "Parse a TUR-<prefix>#### string (any prefix in the table, including the retired TUR-I0379)" |

- **Change (structure):** `looks_like_diag_code_` (`main.c:12638`) is a
  one-line forwarder to `diag_looks_like_code`, and its comment is the stale
  one. Delete it and call `diag_looks_like_code` directly in `cmd_explain`.
  The orphan comment above it, "Phase 8: Handle --explain flag - compile code
  snippet and show detailed error" (`:12634`), describes `cmd_explain` 450
  lines further down. Delete it too, or move it to `cmd_explain`.

### 4. `TUR-I0379` cannot be explained (`diag.c:455-467`)

- **Code:** `diag_code_from_string` knows `TUR-I0379` (`diag.c:579`), but
  `diag_looks_like_code` rejects the `I` prefix. So `tur explain TUR-I0379`
  elaborates the string as source and prints "unbound symbol 'TUR-I0379'".
  The guide shows this.
- **Change (code, one character):** accept `'I'` in `diag_looks_like_code`
  and its comment. Alternatively, derive the accepted prefixes from the table
  so they cannot disagree. The open
  [diag-code-tables-drift-without-a-check](diag-code-tables-drift-without-a-check.md)
  covers the tables more generally.

### 5. `diag_explain`'s comment documents a parameter that does not exist (`diag.h:950-955`)

- **Says:** "`code_str` must be a string like "TUR-E0012" (as returned by
  diag_code_to_string); `code` is the corresponding DiagCode enum value."
- **Code:** `bool diag_explain(DiagCode code, FILE *out);` has no `code_str`.
- **Change:** drop the `code_str` sentence and document `out` instead.

### 6. What `--Werror=deprecated` covers (`globals.c:219-221`, `main.c:12577-12578`, `:13354`)

- **Says:** it "promotes deprecation warnings emitted by elab_lookup_sym to
  errors" (`globals.c`), and that it "promotes ^deprecated use-site warnings
  to errors" (both `main.c` sites).
- **Code:** it also promotes `TUR-D0004` (`elab_structs.c:3624`), and it does
  not touch D0002/D0003 (item 1).
- **Change:** one accurate line in `globals.c`, next to the variable, listing
  what reads it. Point the two `main.c` comments at it.

### 7. Dead `docs/` citation (`diag.h:207`)

- **Says:** "E07xx band reserved in Phase CF0; see
  docs/control-flow-completeness-plan.md."
- **Change:** `docs/archive/history/control-flow-completeness-plan.md`.

## When this is fixed

Remove the example topics from the guide's "Trust the code over the comments"
paragraph. Remove "whatever the `diag.h` comment says" from its "Prefixes and
bands" section. If item 4 lands, also remove the `tur explain TUR-I0379`
example from the `tur explain` section.
