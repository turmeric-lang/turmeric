# Separate compilation: CPS entry-wrapper spells a by-pointer aggregate param by value, conflicting with the forward decl

> **RESOLVED (2026-07-22).** Fixed in `src/compiler/emit_cps_ir.c`: the
> direct->cps entry wrapper now spells a pass-by-ptr aggregate param as the
> direct emitter's `const T *` (via `cps_entry_param_by_ptr` +
> `cps_entry_param_byptr_ctype`, mirroring `emit_fn_forward_decls`) and
> dereferences it (`*p`) when threading the arg into the by-value `__cps` call.
> `emit_params`' by-value per-param logic was extracted into `emit_param_ctype`
> so the wrapper reuses it for the params it does not rewrite. `chol`/`lu`/`qr`
> in `linalg` now emit a wrapper signature that agrees with their forward decl;
> `linalg__decomp.c` compiles with zero `conflicting types`. Full suite: 2264
> passed, 2 failed (both pre-existing: `re-string`, `vec-push-...`).
>
> **NOTE -- `linalg` is still not a clean `--shared` build, but for an
> unrelated, pre-existing, build-mode-INDEPENDENT reason:** several linalg
> inline-C helpers declared `: cstr` (`__vec-fmt`, `__mat-fmt`, and others in
> `small.tur`/`sized.tur`) `return (int64_t)(intptr_t)buf;` -- returning the
> int64 carrier from a function whose C signature is `const char *`. Apple
> clang 21 promotes `-Wint-conversion` to a hard error, so this fails in
> whole-program `tur build` too (reproduced with a 6-line single-file program).
> It is a spice-source portability issue (the inline-C should `return buf;`),
> not the CPS ABI bug this report covers, and is tracked separately.

**Severity:** medium (blocks `tur build --shared` of a spice with a
CPS-lowered `defn` that takes a pass-by-pointer aggregate param). Pre-existing;
surfaced while auditing `../turmeric-spices` after the base-ADT-typedef header
fix unmasked it.

## Symptom

```
linalg__decomp.c:4371:41: error: conflicting types for 'linalg__decomp__chol'
  4371 | __attribute__((unused)) tur_adt_cholfac linalg__decomp__chol(tur_adt_mat a) { ...
  note: previous declaration is here
   808 | tur_adt_cholfac linalg__decomp__chol(const tur_adt_mat *);
```

The forward declaration passes the by-value aggregate `mat` **by const pointer**
(`const tur_adt_mat *`), but the CPS "direct->cps entry wrapper" spells the same
parameter **by value** (`tur_adt_mat a`). Reproduced by `spices/linalg`
(`chol`, `lu`, `qr` -- all take a `mat` and are CPS-lowered).

## Minimal repro

A `defn` that (a) takes a large/record aggregate param the direct emitter passes
by pointer (`type_struct_pass_by_ptr`), and (b) gets CPS-lowered (contains a
control op, or is dragged onto the DK). `linalg`'s:

```turmeric
(defn chol [^borrow a : mat] : cholfac
  (when (not (= (mat-square? a) 1)) (panic "chol: matrix must be square"))
  (let [n      (mat-rows a)
        l      (mat-new-zeroed n n)
        status (__chol-kernel (.data l) (.data a) n)]
    (when (< status 0) (do (mat-free l) (panic "...")))
    (make-struct cholfac l)))
```

## Root cause

`emit_params` (`src/compiler/emit_cps_ir.c:7592`) spells a by-value aggregate
param with `binder_ctype_full` -- i.e. **by value** (`tur_adt_mat`). The
direct->cps entry wrapper at `src/compiler/emit_cps_ir.c:8036` uses
`emit_params` for its signature, so it presents `tur_adt_mat a`. But the direct
emitter's forward decl (`emit_fn_forward_decls`, `src/compiler/emit_module.c`)
passes the same aggregate **by const pointer** because `type_struct_pass_by_ptr`
is true for it. Two declarations of the same external symbol with different
parameter types => `conflicting types`.

The admission gate `fn_byval_agg_param_ok`
(`src/compiler/emit_cps_ir.c:2585`) already documents and guards this exact
hazard -- it restricts by-value-aggregate CPS params to aggregates the direct
emitter *also* passes by value (`!type_struct_pass_by_ptr`). The bug is that
`chol` reaches CPS lowering **anyway** (via a different admission/forcing path,
not `fn_byval_agg_param_ok`), so a `type_struct_pass_by_ptr` param slips through
and the entry wrapper's `emit_params` spelling diverges from the forward decl.

## Fix directions

- Make the entry-wrapper signature mirror the direct forward decl's param
  spelling: for a `type_struct_pass_by_ptr` aggregate, declare `const T *` and
  dereference when threading the value into the `__cps` call (the arg-list build
  at `emit_cps_ir.c:8041`). This keeps the boundary symbol's ABI identical to
  what direct callers / the FFI export shim expect.
- Or extend the CPS-admission gate so a `defn` with any `type_struct_pass_by_ptr`
  aggregate param is not force-lowered to CPS (evict to the direct emitter), the
  same way `fn_byval_agg_param_ok` already excludes it for the clean-admission
  case.

## Related

- Same file/area as the entry-wrapper linkage fix from this audit
  (`static` vs non-static for exported CPS defns, `emit_cps_ir.c:8036`).
- [[project_monomorphization_north_star]].
