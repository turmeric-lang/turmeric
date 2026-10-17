# Emitter comments that disagree with the code

**Severity:** low (comment accuracy). No miscompile. Two items are cheap structural
cleanups (a header re-sort, and dropping tombstones for deleted code); the rest
are comment rewrites. Filed 2026-10-17 from the "Trust the code over the
comments" note in
[emitter-architecture-guide](../guides/emitter-architecture-guide.md). Every
claim below was checked against `main` 9ac3cd43. Line numbers are from that
commit; the function names are the stable part.

The guide's note names three examples. All three still hold. Running a sweep
over every comment in `src/compiler/emit_*.c` / `emit_*.h` for names that occur
nowhere in code, and for `docs/` paths that do not exist, found the rest.

None of these comments sits inside an emitted C string, so fixing them changes
no `expected.c` snapshot and no S2 split hash.

## Changes

### 1. `emit_value_dispatch` size, written into a comment (`emit_expr.c:6964-6967`)

- **Says:** "emit_value_dispatch is ~10k lines over 124 cases ... and there are
  ~218 emit_value call sites". This is in the comment above
  `EMIT_EXPR_DEPTH_STACK_FLOOR`.
- **Code:** `emit_value_dispatch` runs from `emit_expr.c:9691` to `:22655`, which
  is 12,965 lines. It has 120 `case EX_*:` labels, one for each of the 120
  `ExprKind` enumerators in `expr.h`. There are 224 `emit_value(` occurrences
  in emitter code (comments excluded).
- **Change:** say it without numbers. Something like: "emit_value_dispatch is
  the largest function in the tree, with one arm per `ExprKind`, and each arm
  interleaves statement emission with child-value emission. emit_value is
  called from a couple of hundred sites." A measured count in a comment goes
  stale as soon as someone adds an arm. The guide's source map already carries
  the size and re-measures it.

### 2. The "Final assembly order" list (`emit_module.c:21476-21497`, `emit_program_inner`)

- **Says:** nine numbered bands: `early_file`, `concrete_fn_ptr_typedefs`,
  `concrete_adt_apps`, `extern_decls`, `fwd_decls`, `defer_thunks`,
  `pending_handler_fns`, `file`, `main()`. Its continuation line under item 6
  is also mis-indented (` *` at column 1).
- **Code:** `out` is written at `:21443-21668`, in this order:
  1. the preamble, the hoisted includes and the panic-site slot (written
     *above* the comment, at `/* Final assembly. */`, `:21435`);
  2. `early_file`;
  3. `sym_records`;
  4. `concrete_fn_ptr_typedefs`;
  5. `concrete_adt_apps`;
  6. `rt_lazy` (split builds only);
  7. `thunk_typedefs`, after `emit_any_type_name_table` appends to it;
  8. `extern_decls`;
  9. `fwd_decls`;
  10. `defer_thunks`;
  11. `cprelude`, then `cprelude_own`, then `cprelude_user`;
  12. `pending_handler_fns`;
  13. `file`;
  14. `defer_thunk_defs`;
  15. `main` or `__tur_module_def_init`;
  16. `fatbox_defs`, then `__tur_fatbox_init`;
  17. `emit_instance_row_table`;
  18. `emit_r7rs_gc_tls_roots`;
  19. `emit_cps_ir_flush_groups`;
  20. `static_init_emit`;
  21. `emit_ffi_export_shims` (J2 only).

  `emit_program_job` then appends the `tur:inline-c-fns` comment.
- **Change (comment):** move the list up to `/* Final assembly. */` at
  `:21435`, where the first write happens, and make it the real sequence. The
  "macos-int-conversion" paragraph after it refers to "(2) and (3)". Have it
  name the bands (`concrete_fn_ptr_typedefs` / `concrete_adt_apps`) instead, so
  renumbering cannot break it.
- **Change (structure, optional):** the assembly is about 20
  `if (x.len) { buf_write(out, ...); buf_putc(out, '\n'); }` lines that are
  interleaved with frees, and the order exists only as statement order. Pull it
  into a function such as `emit_program_assemble(out, &bands)`, taking a struct
  whose fields are the band `Buf`s declared in output order. A reader could
  then see the order in the struct declaration, and a band added to the struct
  would also be in the right place in the output. `emit_program_inner` is about
  2,000 lines (`:19787` to `:21753`), so splitting off the tail is also
  good for its own sake.
- **Guide follow-up:** the guide's "Output bands" table has the same gaps. It
  leaves out `rt_lazy`, `cprelude_own`, `cprelude_user` and the FFI shims, and
  it folds lazy runtimes into `thunk_typedefs`. Re-check the table against the
  list above.

### 3. `emit_join_coerce` (`emit_expr.c:30`)

- **Says:** "The same two-step question the `any` temp reader asks
  (emit_join_coerce's temp_is_tagged arm)".
- **Code:** no such function exists. The `temp_is_tagged` arm is in
  `bridge_control_result_int_ptr` (`emit_expr.c:2757`). `emit_cps_ir.c:9471`
  already names it correctly.
- **Change (comment):** replace the name with `bridge_control_result_int_ptr`.
- **Change (structure):** the "two-step question" is now written out three
  times. Each copy strips `EX_ASCRIBE`, asks for the recorded local spelling,
  and falls back to `emit_binding_repr_c_name`. The copies are
  `emit_arg_is_any_carrier_word` (`:33-51`), the `temp_is_tagged` arm
  (`:2757-2768`) and, with parameters added, `emit_ident_emitted_ctype`
  (`:2710`) used in the arm below it. Make one helper, say
  `emit_value_spelled_ctype(ctx, v, expr)`, built on `emit_ident_emitted_ctype`,
  and call it from all three. The comment can then point at that function and
  not at a sibling arm. Note: the `temp_is_tagged` arm currently skips the
  parameter lookup that `emit_ident_emitted_ctype` adds. Before unifying,
  confirm that a `tur_tagged_t` temp is never assigned straight from a
  parameter, or accept the wider lookup on purpose.

### 4. A comment separated from its function (`emit_expr.c:11-13`)

- **Says:** "ACB: true when kind represents a concrete aggregate type (struct,
  ADT, or type-application) ...". It sits directly above the S1 comment for
  `note_call_ret`.
- **Code:** it describes `type_kind_is_aggregate`, which is 35 lines further
  down (`:53`).
- **Change:** move it to just above `type_kind_is_aggregate`.

### 5. `emit_site_push_text` (`emit_core.c:7`)

- **Says:** `#include "diag.h" /* diag_file_path (emit_site_push_text) */`.
- **Code:** `diag_file_path` is used by `emit_line_directive` (`:549`) and
  `emit_site_ref_text` (`:2095`). `emit_site_push_text` does not exist; the
  site helpers are `emit_site_set_text` and `emit_site_ref_text`.
- **Change:** `/* diag_file_path (emit_line_directive, emit_site_ref_text) */`.

### 6. The `emit_effects.c` file header (`emit_effects.c:1-12`)

- **Says:** the effect runtime prelude fragments ("fiber struct fields,
  global_effect_handler_chain, tur_effect_perform definition") "remain in
  emit_module.c". It also lists `EX_PERFORM` under Region C.
- **Code:** `emit_module.c:17911` records that `tur_effect_perform` and the
  fiber dispatch were "DELETED 2026-07-19 with the fiber effect runtime".
  `global_effect_handler_chain` occurs nowhere in `src/`. `EX_PERFORM` is
  handled inline in `emit_value_dispatch` (`emit_expr.c:18110`), which emits
  nothing, because the fiber emitter for it was deleted. The file also has
  arms the header does not list: `emit_effects_handler_lit` and
  `emit_effects_compose_handlers`.
- **Change:** rewrite the header to list what is actually here: the defect,
  handle, handler-literal, compose, resume and discontinue arms; the
  reset/shift family; and the continuation predicate. Drop the paragraph about
  prelude fragments, and add a single "the CPS/DK backend is the only effect
  lowering" line. The section sign in that paragraph is also non-ASCII.

### 7. A leftover TODO naming a nonexistent helper (`emit_effects.c:758-760`, `emit_effects_shift`)

- **Says:** "Use emit_call_helper to handle both functions and closures / For
  now, just emit as a regular call".
- **Code:** `emit_call_helper` does not exist. The code right after the
  comment handles the closure case itself (`EX_CLOSURE` -> thunk name).
- **Change:** delete the two lines and leave `/* Emit the call:
  k_fn(body_val) */`.

### 8. More names that no longer exist

| Where | Names | Change |
|---|---|---|
| `emit_cps_ir.c:12106` | "Mirrors emit_cloneable_ctx" | The direct emitter's cloneable context lowering was deleted (cps-backend-direct-lowering-removal D6). Drop the "Mirrors" sentence, or point it at `cloneable_ctx_free_vars` if that is the twin intended |
| `emit_module.c:17212` | `emit_async_owns_env` | It is `async_spawn_owns_env` (`emit_expr.c:9677`) |
| `emit_expr.c:1978` | `emit_resolve` | It is `emit_resolve_type` (`emit_core.c:235`) |
| `emit_module.c:425`, `types.c:922` | `type_extract_struct_app`, described as live ("its struct-app branch defers to `type_extract_struct_app`") | The function was deleted when structs were retired. `type_has_concrete_codegen_layout`'s `TY_APP` arm now returns `false` unconditionally (`types.c:865`). Say exactly that. Five more comments name it (`emit_core.c:6531`, `:6850`, `emit_expr.c:15186`, `emit_module.c:4192`, `:5886`); each already says the path is dead, so see item 9 |

### 9. Tombstones for deleted code, and one that contradicts itself

Several comments exist only to record that a function was deleted:

- `emit_carrier_return_override`: `emit_core.c:530`, `emit_fns.c:4497`,
  `emit_module.c:11192`, `emit_stmt.c:1168`;
- `type_extract_struct_app` (see the table above);
- `emit_abi_collect_type_bindings`: `emit_module.c:3982`;
- the cloneable reset context: `emit_module.c:16111`, `emit_effects.c:620`,
  `:833`;
- `tur_effect_cont_valid`: `emit_module.c:16526`;
- `emit_effects_perform` / `emit_effects_with_handler`: `emit_expr.c:18115`.

One of them is now wrong rather than merely historical. `emit_stmt.c:1158-1166`
still lists "1. emit_carrier_return_override: body returns a struct by value"
as the first step of the return-type priority, and three lines later says that
branch was removed.

- **Change:** delete the bare tombstones. The history is in git and in the
  archived plans. Where a deletion explains why the code is shaped as it is
  now, keep one present-tense sentence ("a method body is never `TY_STRUCT`, so
  the return type comes from the binding"). Renumber the `emit_stmt.c` priority
  list to the three steps that remain. Each deletion is an opportunity for a
  comment to go stale, and these comments are pure maintenance load.

### 10. `emit_internal.h` is not grouped by defining file

- **Says:** the guide's source map describes `emit_internal.h` as "grouped by
  defining file", and the header's section banners say the same
  (`/* ------------ emit_core.c: helpers, naming, atoms, builtins ------------ */`,
  `:990`).
- **Code:** that one section holds 239 prototypes, and about 133 of them are
  defined somewhere else: 82 in `emit_module.c` (for example `emit_sig_reset`,
  `emit_vl_mono_name`), 40 in `emit_expr.c` (`emit_binding_is_region_scope`,
  `emit_panic_call`), 7 in `emit_fns.c` (`emit_pbp_push`), 3 in `emit_stmt.c`
  and 1 in `emit_cps_ir.c` (`effect_tag`). The `emit_expr.c` section holds
  three prototypes from `emit_effects.c` (`emit_handle_is_*`), and the
  `emit_fns.c` section holds two from `emit_module.c` (the dead-base-ctor
  traps).
- **Change (structure):** re-sort the prototypes into real per-file sections.
  This is mechanical and can be scripted: for each prototype, find the file
  whose column-0 definition names it. The other option is to drop the per-file
  banners and group by topic. Either way, the guide row should describe
  whatever the header ends up doing.

### 11. Dead `docs/` citations (12)

Each of these moved from `docs/reported/` (or `docs/`) into `docs/archive/`:

| Where | Cited | Now at |
|---|---|---|
| `emit_core.c:3074` | `docs/reported/separator-fold-collides-emitted-c-names.md` | `docs/archive/` |
| `emit_cps_ir.c:203` | `docs/reported/any-struct-box-leak-per-widen.md` | `docs/archive/` |
| `emit_expr.c:1042` | `docs/reported/byvalue-product-tail-var-double-unboxed-nonparametric.md` | `docs/archive/` |
| `emit_expr.c:13102` | `docs/reported/closure-in-defdata-field.md` | `docs/archive/` |
| `emit_expr.c:21894` | `docs/reported/sr2-carrier-seam-rotted.md` | `docs/archive/` |
| `emit_fns.c:123` | `docs/control-flow-completeness-plan.md` | `docs/archive/history/` |
| `emit_fns.c:429`, `:4445` | `docs/reported/emit-tail-return-path-lacks-carrier-bridges.md` | `docs/archive/` |
| `emit_module.c:14797`, `:22165`, `:22205` | `docs/reported/multi-variant-adts-always-heap-allocate.md` | `docs/archive/` |
| `emit_module.c:15031` | `docs/first-class-handlers-semantics.md` | `docs/archive/history/` |

The same sweep over all of `src/` finds 29 dead paths, even though the
2026-08-13 sweep (`docs/archive/mono-specs-header-comment-stale.md`) repointed
255 of them.
A docs-lint check would stop this recurring, and it is a few lines of shell:

```sh
grep -rhoE 'docs/[A-Za-z0-9/._-]+\.md' src | sort -u |
  while read -r p; do [ -f "$p" ] || echo "dead citation: $p"; done
```

Adding it to `tests/check-docs-lint.sh` would also cover the other five
stale-comments reports. As the archived report warns, a citation can wrap
across a `*` continuation line, so the check undercounts. That is fine for a
ratchet.

## When this is fixed

Trim the examples out of the guide's "Trust the code over the comments"
paragraph, or replace them with whatever is still true. Item 2's guide
follow-up applies at the same time.
