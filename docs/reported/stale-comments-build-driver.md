# Build-driver (`main.c` pass pipeline) comments that disagree with the code

**Severity:** low (comment accuracy). Two items are dead code (a write-only
field and a no-op pass), and one is a small deduplication. Filed 2026-10-17
from the "Trust the code over the comments" note in
[build-driver-internals-guide](../guides/build-driver-internals-guide.md).
Checked against `main` 9ac3cd43; line numbers are from that commit.

The guide names two examples, the "stub slot" and the `execv`, and both still
hold. Its pass table and section 4 point at three more. A read of the
pass-pipeline region of `main.c` turned up the rest.

## Changes

### 1. `PASS_EFFECT_ROW_INFER` is the effect-row checker, not a stub (`main.c:665`)

- **Says:** `PASS_EFFECT_ROW_INFER,  /* P19-2: stub slot -- no-op until inference lands */`.
  The `--` is a UTF-8 em dash in the source.
- **Code:** `run_core_pass` runs `effect_check_pass` there, plus
  `--dump-effects` (`:763-773`). `pass.h:36` already describes it correctly
  ("infer and validate effect rows per function").
- **Change:** `/* effect_check_pass: effect-row inference and checking */`, in
  ASCII.

### 2. The "how to add a pass" recipe is incomplete (`main.c:657-660`, `pass.h:4-7`, `pass.h:30-31`)

- **Says:** "add a PassKind constant to pass.h, add a case here, and add the
  case in run_core_passes() below". `pass.h` says the same, with "the
  run_core_passes() switch".
- **Code:** the switch is in `run_core_pass`, singular (`:688`).
  `run_core_passes` (`:817`) is only the loop. There is also a fourth place
  that must change, `pass_zone` (`:674`). Its own comment says that a missing
  arm is a deliberate `-Wswitch` error, so the recipe as written leads straight
  to that error.
- **Change:** list the four places in both files: the `PassKind` enumerator,
  the `core_passes[]` entry, the `pass_zone` arm (and a `ProfZone` if the pass
  needs its own), and the `run_core_pass` arm.

### 3. `tur run` does not `execv` (`main.c:721-723`)

- **Says:** "`tur run` calls this pass in-process then execv's the compiled
  binary, which discards any unflushed stdio buffer. Flush so the dump
  survives".
- **Code:** `cmd_run` runs the binary with `system()` (the `RUN_ENTRY` macro at
  `:6926`, and `:6999`). Both sites call `fflush(NULL)` first, for the reason
  recorded at `:6921` (tur-run-dump-output-interleaves-program-output: an
  unflushed dump would land *after* the program's output).
- **Change:** the `fflush(stdout)` is now belt-and-braces. Either delete it
  with its comment, since `cmd_run` owns the ordering, or keep it with a
  one-line comment: "harmless; cmd_run also flushes before system() so a dump
  precedes the program's output".

### 4. The VBM note in the elaborate arm says codegen is unchanged (`main.c:707-715`)

- **Says:** "Registry-only: codegen is unchanged (the per-spec by-value body
  emit is tracked separately)."
- **Code:** this is the claim
  [mono-specs-header-comment-stale](../archive/mono-specs-header-comment-stale.md)
  corrected in `mono_specs.h` on 2026-08-13. VBM2b and VBM3 landed 2026-07-04,
  and the by-value body emit is live for simple and composed lenses
  (`mono_specs.h:28-41`). This copy was missed in that sweep.
- **Change:** "Resolves abstract lens specs to concrete ones; the emitter
  redirects to the by-value `<lens>__mono_<hash>` bodies (see mono_specs.h)."

### 5. A sibling of item 4 in `globals.c` (`globals.c:250-256`)

- **Says:** "COMPOSED lenses fall back to the Path A carrier bridge
  (mono_specs.c lens_is_simple_for_pathb)".
- **Code:** composed lenses thread by value since CB1-CB5 (2026-07-05). Path A
  is a narrow backstop for the shapes CB2/CB3 cannot lower
  (`mono_specs.h:38-41`).
- **Change:** match the `mono_specs.h` wording, or replace the paragraph with
  "see mono_specs.h, CURRENT STATE".

### 6. The `PASS_EFFECT_LOWER` arm, `pass.h`, and `effect_lower.c` describe a lowering that does not happen

- **Says:**
  - `main.c:748`: "Phase 19: transform perform/handle into shift/reset."
  - `pass.h:35`: "perform/handle -> shift/reset (effect lowering)" (the `->` is a UTF-8
    arrow in the source).
  - `effect_lower.c:9-19`: "(perform ...) -> (shift k ...)", "(handle ...) ->
    (reset ...)".
- **Code:** `effect_lower` returns its input when the program contains no
  effect node. Otherwise `lower_expr` rebuilds the tree arm by arm and returns
  `EX_PERFORM`, `EX_HANDLE` and `EX_RESUME` unchanged (`effect_lower.c:374-381`).
  Nothing is lowered. The CPS/DK backend handles effects. The only real work in
  this arm is building the `EffectEnv` (builtin `Unsafe` and the capabilities),
  which later passes read.
- **Change (comment):** say so in all three places: "builds the EffectEnv;
  effect_lower is an identity walk -- the CPS/DK backend lowers effects".
- **Change (structure):** delete the `effect_lower` call and, if nothing else
  calls it, `effect_lower.c`. Keep the `EffectEnv` construction, and consider
  renaming the pass `PASS_EFFECT_ENV`. One caveat must be checked first: the
  rebuild is not quite an identity. `lower_discontinue` rebuilds
  `EX_DISCONTINUE` with `TYPE_NIL` and not with the node's own type
  (`effect_lower.c:113`). Removing the pass would keep the original type.
  Confirm nothing has come to depend on that type before deleting the pass. If
  something has, that dependency is a bug in its own right.

### 7. "(v1 stub)" on the kind checker (`main.c:736`)

- **Says:** "Phase HKT H0: kind inference and validation pass (v1 stub)."
- **Code:** `kind_check_pass` (`src/passes/kind_check.c:476`) is a real checker
  that returns an error count the pass acts on.
- **Change:** drop "(v1 stub)".

### 8. "(dump-only; not wired into codegen)" on `--dump-cps` (`main.c:781-782`)

- **Says:** "--dump-cps prints the ANF/CPS IR for every colored function
  (dump-only; not wired into codegen)."
- **Code:** `cps_ir_dump_program` prints `cps_ir_translate_fn`'s CT-IR, and the
  CPS backend emits C from that same IR (`emit_cps_ir.c:6859`, `:14237`).
- **Change:** "prints the CT-IR the CPS backend emits from, for every colored
  function."

### 9. Two dead `needs_hamt` fields

- `PassContext.needs_hamt` (`pass.h:70-71`, "Phase P3: HAMT lowering - track
  if HAMT functions are used") is never read or written anywhere.
- `Elab.needs_hamt` (`elab_internal.h:939`) is written (`elab_call.c:2728`,
  `:2772`, `elab_core.c:2637`) but never read. The live flag is the global
  `g_needs_hamt`.
- **Change:** delete both fields and the three writes.

### 10. `load_project_prelude_impl` and its call sites undersell what it loads (`main.c:1523-1536`, `:1693`, `:1802`)

- **Says:** the doc comment, which sits on the *forward declaration* at
  `:1534`, says "Load macro-only prelude files (macros.tur) ... Only
  macro-providing files are loaded here". Both call sites say "F1: inject
  macro-only prelude so project-mode files see when/cond/for/min/max."
- **Code:** the definition loads every file in `tur_stdlib_autoload_files()`,
  in order, and its own body comment says so ("The prelude IS the project-mode
  stdlib auto-load"). That comment in turn names `g_stdlib_autoload_files`,
  which does not exist; the function is `tur_stdlib_autoload_files`.
- **Change:** delete the forward declaration's comment and keep the body's,
  correcting the name. Change both call sites to "F1: prepend the stdlib
  auto-load list (the same files compile_to_c gets)." A doc comment on a
  forward declaration 40 lines away from the definition is how this drift
  happened. Keep a function's description at its definition.

### 11. `cmd_build_multi` still says "top-level" (`main.c:8475`)

- **Says:** "glob the directory's top-level .tur files".
- **Code:** it calls `collect_tur_files_deep`, which recurses.
- **Change:** "collect every `.tur` under the directory, recursively
  (collect_tur_files_deep)".

### 12. Three identical per-unit reset sequences (`main.c:1302-1306`, `:1649-1653`, `:1758-1762`)

- **Code:** `compile_to_c`, `compile_to_h` and `compile_to_implementation` each
  run the same five calls with the same trailing comments: `diag_reset`,
  `experiment_reset_warnings`, `pkg_tur_version_reassert`,
  `pkg_manifest_reassert`, `refine_discharge_reset`.
  `parse_check_read` (`:9526`) runs only `diag_reset`.
- **Change (structure):** extract `begin_compile_unit()` and call it from all
  three, so that a future per-unit state is reset in one place. Separately,
  check whether `parse_check_read` should call it too. If it skips the
  reasserts on purpose, say so at that call.

### 13. Dead `docs/` citations

| Where | Cited | Now at |
|---|---|---|
| `main.c:10196`, `src/turi/preload.c:260` | `docs/reported/turi-vec-new-filled-native-override-lost.md` | `docs/archive/` |
| `main.c:13227`, `globals.h:282` | `docs/reported/multi-variant-adts-always-heap-allocate.md` | `docs/archive/` |
| `main.c:13250` | `docs/reported/sr2-carrier-seam-rotted.md` | `docs/archive/` |
| `justrun.c:4` | `docs/tur-run-plan.md` | `docs/archive/history/` |

(`experiments.c:17` cites `docs/upcoming/fancy-rows-plan.md`, which does not
exist, but it sits inside an example descriptor for a made-up `fancy-rows`
experiment. That is fine as it is.)

## When this is fixed

Remove the two examples from the guide's "Trust the code over the comments"
paragraph. Also remove the "is stale" remarks from its pass table
(`PASS_EFFECT_LOWER`, `PASS_EFFECT_ROW_INFER`), from section 2 (the
`load_project_prelude_impl` note) and from section 4 (the "top-level" note).
