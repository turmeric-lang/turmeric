# JIT-scoped dynamic FFI via c2mir (no new dependency)

> **Status:** GRADUATED 2026-08-21 in 0.38.0 -- `call-ptr` and `callback-ptr`
> are ordinary `unsafe` forms and no longer need `--enable=jit-ffi` (a
> lingering enable is a TUR-W0063 no-op).  The `-DTUR_JIT=ON` build gate on the
> interpreter path is unchanged, and `unsafe` is still required.  The row
> graduated at its own `expires_at`, which is the review it asked for: the gate
> existed only so the signature vocabulary could move, F4 settled that by
> encoding layout inline in the sig string rather than in a registry, and the
> follow-on batch below then measured the surface rather than merely holding
> it still.  The one substantive item still open -- the aarch64 HFA gap -- is
> a refusal with a diagnostic, not silence.
>
> F1-F5 implemented (F1-F3 2026-08-17; F4 + F5 2026-08-18), and
> the follow-on batch of 2026-08-21 closed four of the five items F5 left
> open -- x86-64 verification (which found a nested-aggregate miscall),
> inbound callback aggregates, extern-c aggregate slots, and scalar width
> fidelity (which turned out to be a live wrong-answer bug in the return
> direction, not a cosmetic inconsistency).  The aarch64 HFA gap in MIR is
> the one substantive item still open.  See "Open after F5" below.
> Proposed 2026-08-17.
> **Track:** post-v1. Requires `-DTUR_JIT=ON` builds; non-JIT builds keep
> today's behavior unchanged.
> **Type:** interpreter/JIT (`src/turi/`, `src/jit_engine.c`) + one small
> compiled-path codegen form.
> **Supersedes-in-spirit:** "Phase 4: retire the generated table" of
> [docs/archive/interpreter-arbitrary-arity-ffi-plan.md](../archive/interpreter-arbitrary-arity-ffi-plan.md),
> which contemplated libffi for the same retirement.
> **See also:** [ffi-spices-integration-plan.md](ffi-spices-integration-plan.md)
> (the package-layer follow-ons: REPL JIT-hook link flags, variadic
> exports, where F4/F5 pay off for spices),
> [docs/guides/ffi-guide.md](../guides/ffi-guide.md) (user-facing guide).
>
> **Implementation notes (F1-F3):**
> - The provider hook lives in `src/turi/jit_ffi.h` + `jit_ffi_hook.c`
>   (tur_core, MIR-free); the c2mir-backed provider in `src/turi/jit_ffi.c`
>   rides in `tur_jit_obj` and is installed from `main()` under
>   `TUR_HAVE_JIT` -- the same split as `TurSpiceJitHook`.  Thunks compile
>   through the existing `tur_jit_compile_image` path (one resident image
>   per unique signature, negative-cached on compile failure).
> - `call-ptr` is NOT a new expr kind: it is `EX_CALL` with a non-NULL
>   `ptr_sig` (`CallPtrSig`, expr.h), so every recursive walker that already
>   visits `fn_expr` + `args` traverses it unchanged.  Both the eval_expr
>   dispatch and the work-stack driver's descending `EX_CALL` case intercept
>   on `ptr_sig`.
> - Landed alongside, because the fixture forced it: the dl builtins' codegen
>   never emitted `<dlfcn.h>` (so compiled `(unsafe (dlopen ...))` had never
>   actually compiled), and the interpreter had no `BS_DLOPEN`/`BS_DLSYM`/
>   `BS_DLCLOSE` eval arms at all.  Both fixed (`g_needs_dlfcn` preamble
>   gate + `-ldl` autolink marker; real dl calls in `eval_builtin`,
>   capability-gated as before).
> - The signature vocabulary gained `'F'` (exact float32) over the shim
>   mapping's widened `'f'`; `sv`/`out_s` thunk parameters are reserved for
>   F4's struct extension.
> - Tests: `tests/fixtures/jit-ffi-call-ptr` (portable inline-C
>   address-of, compiled + jit paths), and `tests/run-flags.sh`'s
>   `jit-ffi-*` cases (real `strtol` via extern-c under `--interpret`, the
>   dlopen -> dlsym -> call-ptr loop, the non-JIT clean diagnostic, and the
>   experiment gate); the JIT-only cases probe the binary and PASS-skip on
>   an engine-less build (`TUR=./build-jit/tur bash tests/run-flags.sh`
>   exercises them).

> **Implementation notes (F4-F5, 2026-08-18):**
>
> - **The struct registry section 4 warned about does not exist, by design.**
>   Section 2.1 sketched naming a layout "by hash ... resolved against a small
>   registry", and section 4 flagged that registry as the piece most likely to
>   grow.  Encoding the layout INLINE in the sig instead (`{ff}`, exact-width
>   member codes, nesting allowed) makes a sig string a complete
>   self-describing layout: the thunk cache stays keyed on it alone, and
>   nothing has to be kept in sync between the elaborator and turi.
> - **The compiled path needed almost nothing.**  A `defstruct` already emits
>   as the exact by-value C struct with the declared field types, so a
>   signature slot naming a record just works; only the argument cast had to
>   go (C has no cast to a struct type).  The interpreter builds bytes itself,
>   from the same layout engine that renders the thunk's declaration.
> - **MIR mis-passes floating-point aggregates on aarch64**, which is the
>   real boundary on F4.  c2mir's aarch64 ABI file hands back one undifferentiated
>   `MIR_T_BLK` for every aggregate where the x86-64 sibling runs the full SysV
>   eightbyte classification, and both MIR backends then route every block
>   through `x0..x7`.  AAPCS64 puts an HFA in `v0..v7`.  A c2mir thunk calling
>   a natively compiled callee therefore writes the members where the callee
>   does not look -- and the answer is whatever was left in the SIMD
>   registers, so some call sites are accidentally right.  The interpreter
>   REFUSES an HFA on aarch64 rather than miscall it; compiled code is
>   unaffected.  Matrix + root cause at file:line:
>   [docs/archive/mir-aarch64-fp-aggregate-abi.md](mir-aarch64-fp-aggregate-abi.md).
> - **F5 reuses F3's node** -- a `CallPtrSig` with `is_callback`, the closure
>   sitting where the target address sat -- so F3's "no walker had to learn a
>   new kind" property still holds, and both forms share one `[T1 T2 -> R]`
>   parser.
> - **Two F4/F5 boundaries below were retired on 2026-08-21** -- callbacks
>   now take and return aggregates, and the scalar vocabulary is
>   exact-width.  The notes are kept as written because they explain why
>   each was drawn where it was; "Open after F5" records what changed.
> - **A callback must lower to a plain C function** (top-level defn, or an
>   inline lambda with no captures).  A C callback slot has no room for a
>   captured environment, and the `:fn` runtime representation is not uniform
>   enough to recover one from -- one of its three shapes already SIGSEGVs on
>   the ordinary argument path
>   ([docs/reported/let-bound-noncapturing-lambda-segfaults-as-fn-arg.md](../reported/let-bound-noncapturing-lambda-segfaults-as-fn-arg.md)).
>   Enforced in elaboration so `tur run` and `--interpret` agree.
> - **No `callback-free!`** (section 2.3 floated it as optional).  It would be
>   a no-op on the compiled path and a use-after-free primitive on the other;
>   a C library holding a function pointer cannot tell us it is finished, so
>   there is no safe moment to reclaim one.  Callbacks are process-lifetime,
>   matching turi's closure policy, and the provider caches on (sig, ctx) so a
>   `callback-ptr` in a loop reuses its image.
> - Tests: `tests/fixtures/jit-ffi-call-ptr-struct`,
>   `tests/fixtures/jit-ffi-callback-ptr` (qsort(3) with a Turmeric
>   comparator, the acceptance case named in section 3), and five new
>   `jit-ffi-*` cases in `tests/run-flags.sh` covering the interpreter's
>   struct marshaller, the aarch64 HFA refusal, the interpreter callback, the
>   closure rejection, and the non-JIT diagnostic.

## 0. Summary

Give Turmeric a real dynamic FFI -- call an arbitrary function pointer
with a signature known only at runtime, including struct-by-value, plus
runtime-created callbacks -- by synthesizing tiny C thunks at runtime
with **c2mir**, the C front end of the MIR JIT we already vendor
(`cmake/mir.cmake` builds `c2mir/c2mir.c` into `tur_mir` today). Scope
the feature to JIT builds, pair it with a pure-codegen `call-ptr` form
for the AOT path, and the `dlopen`/`dlsym` story becomes complete
end-to-end **without adding libffi, dyncall, or any third-party FFI
library to the tree**.

## 1. Rationale (read this before reopening the libffi question)

### 1.1 The scope limitation is smaller than it looks

Walk through who actually needs runtime call construction:

- **The compiled path does not.** AOT code can call an arbitrary
  `dlsym`'d pointer with pure codegen -- an
  `(unsafe (call-ptr p [sig] args))`-style form just emits
  `((ret (*)(...))p)(args)` into the generated C. The current hole --
  `dlopen`/`dlsym`/`dlclose` builtins exist
  (`src/compiler/builtins.c:192-194`, codegen at
  `src/compiler/emit_core.c:3556-3573`) but **there is no way to invoke
  the resulting pointer** -- is a codegen feature, not an FFI-library
  problem.
- **The interpreter is the only consumer that genuinely needs
  signatures materialized at runtime.** It is why all three layers of
  the current machinery exist: the generated fixed-shape trampoline
  table (`tools/gen_ffi_dispatch.py`, default `--max-arity 6`, `'i'`/
  `'f'`/`'v'` type classes, `'?'` = unrepresentable), the shape-string
  thunk (`src/runtime/ffi_dispatch_thunk.c`), and the per-export
  `__ffi` shims emitted for spices
  (`emit_ffi_export_shims`, `src/compiler/emit_module.c:13245`).
- And the interpreter is exactly where the JIT already lives:
  `TurSpiceJitHook` (`src/turi/spice_loader.h:117-124`, installed
  `src/main.c:9928`) already replaces the dlopen/dlsym spice path when
  the JIT is present, and `jit_import_resolver` already resolves
  externals via `dlsym(RTLD_DEFAULT, ...)` (`src/jit_engine.c:354`).

So "FFI, but only under the JIT" is not a compromise that strands the
main use case -- it scopes the feature to precisely the one place that
needs it.

### 1.2 Why not libffi / dyncall / ffcall / C-Invoke

Survey of the field (verified 2026-08-17):

| | upstream CMake | license | status | arm64 mac / x64 linux / win | callbacks |
|---|---|---|---|---|---|
| dyncall | **yes** (ships CMakeLists, FetchContent-clean) | ISC | 1.4 (2022-12), slow but alive; Mercurial upstream at dyncall.org | all three | dyncallback |
| libffi | no -- autotools; vcpkg (`unofficial-libffi`), conan, or 3rd-party CMake ports (am11 `feature/cmake-build-configs`) | MIT | 3.8.0 (2026-08), very active | all three incl. win-arm64 | `ffi_closure`, battle-tested |
| GNU ffcall | no -- autotools | **GPLv3+** | 2.5 (2024-09) | unix-strong; native win64 weak | yes |
| C/Invoke | no | BSD | **dead** (1.0, 2007) | no arm64, no win64 | on dead platforms only |

- **ffcall** is disqualified outright: GPLv3+ (not LGPL) linked into
  the runtime would impose GPL obligations on every Turmeric program.
- **C/Invoke** is dead and predates arm64 entirely.
- **dyncall** is the best pure-CMake citizen (tiny, ISC, upstream
  CMakeLists) and would be the pick *if* we wanted a third-party lib.
- **libffi** is the industry default (CPython, Ruby, GLib) with the
  deepest ABI hardening, but upstream is autotools; CMake consumption
  means vcpkg/conan or owning a community port, and `ffi.h`/
  `fficonfig.h` are per-target autoconf-generated, which is the real
  pain of hand-vendoring it.
- **The project already evaluated libffi and deferred it once**:
  `docs/archive/interpreter-arbitrary-arity-ffi-plan.md:176-192`
  ("Alternative A: libffi") chose the per-export shim path instead,
  retaining libffi as the preferred option only if the generated table
  is ever retired. This plan is the alternative answer to that same
  Phase 4: retire the table (for JIT builds) with a dependency we
  already carry.

### 1.3 Policy fit is the clincher

The top-level CMakeLists states the standing policy twice
(`CMakeLists.txt:64-77`, `:89-98`): **a default configure must not
touch the network and must not grow a dependency.** Any third-party FFI
lib therefore goes behind an opt-in flag anyway -- at which point it is
competing with c2mir, which is *already behind that flag*
(`TUR_JIT`), already pinned to our own patched fork
(`cmake/mir.cmake:80-84`), and already built (`mir.c`, `mir-gen.c`,
`c2mir/c2mir.c` at `mir.cmake:108-112`). A JIT-scoped FFI adds zero
configure-time surface and zero new licenses.

### 1.4 What c2mir thunks buy over the current machinery

- **Arbitrary arity and shape.** No `--max-arity` regeneration; no
  `-1` from `tur_ffi_thunk_call` on an unlisted shape; the 381-entry
  generated table becomes a non-JIT fallback only.
- **Struct-by-value.** The `'?'` "unrepresentable" class
  (`src/runtime/ffi_dispatch.h:9-13`) becomes representable: c2mir
  implements the target C ABI, and our pinned MIR fork already carries
  ABI patches (make_one_ret aliasing, aarch64 `__uint128_t` alignment,
  ... -- documented at `cmake/mir.cmake:18-77`), so this codepath is
  one we effectively co-own.
- **The interpreter's extern-c table stops lying.** Today turi's
  extern-c support is a hardcoded 7-entry table (`exit`, `free`(noop),
  `strlen`, `getenv`, `printf`, `printf_s`, `puts` --
  `register_extern_c_known`, `src/turi/eval.c:378-396`); **everything
  else silently returns nil** (`native_nil_stub`, `eval.c:310-314`).
  With thunks, any `extern-c` declaration resolves via
  `dlsym(RTLD_DEFAULT)` and gets called for real. That is a
  correctness upgrade for `--interpret`, not just a convenience.
- **Callbacks.** libffi attaches per-instance data to a generated
  function pointer via `ffi_closure`; with c2mir we get the same
  effect by generating a fresh tiny C function per callback instance
  that embeds the context pointer as a literal constant in its source
  (see 2.3). Heavier per instance (a compile, not a template stamp),
  but interpreter callback volume is low.

Compile cost is milliseconds per unique signature, amortized by a
cache -- negligible against tree-walking dispatch overhead.

### 1.5 The honest costs

1. **Feature-behind-a-build-flag.** `TUR_JIT` is OFF by default, so
   dynamic FFI exists only in JIT builds. Fine for the REPL/dev-tooling
   story (which is where you want it); the fallback ladder (2.4) keeps
   non-JIT builds at today's behavior rather than breaking them.
2. **Platform coverage is MIR's, not libffi's.** x86-64 Linux and
   arm64 macOS -- the actual targets -- are solid. Windows and exotic
   architectures are much thinner than libffi's 40-arch matrix; WASM
   is out entirely. If dynamic FFI is ever needed there, that is the
   point where libffi (or dyncall) re-enters the conversation -- the
   `jit_ffi_call` interface below is deliberately shaped so either
   could be slotted in behind it later.
3. **More ABI surface on the MIR fork.** Struct-by-value will exercise
   c2mir corners the JIT's current usage may not; the six patches
   already carried suggest we will find a couple more. Real
   maintenance, but bounded, and upstreamable.

## 2. Design

### 2.1 Signature descriptors

Reuse and extend the existing shape-string vocabulary
(`'i'`/`'f'`/`'v'` from `ffi_dispatch.h`, `ffi_shim_class_for_kind` at
`emit_module.c:13200-13217`) rather than inventing a second one:

- Keep `'i'` (int64 carrier: `:int`, `:cstr`, `:bool`, `:ptr<T>`,
  sized ints), `'f'` (double), `'v'` (void return).
- Add `'F'` = float32 (currently widened; needed for exact ABI),
  and a bracketed struct form `"{...}"` naming a C layout by hash for
  by-value aggregates, resolved against a small registry of struct
  layouts the elaborator already knows.

A signature is `ret ':' args`, e.g. `"i:ifi"`, `"v:{Vec3}i"`.

### 2.2 Call thunks

New TU `src/turi/jit_ffi.c` (JIT builds only; `#ifdef TUR_JIT`):

```c
/* Returns a cached or freshly compiled thunk for this signature. */
typedef void (*TurJitFfiThunk)(void *fn, const int64_t *iv,
                               const double *fv, void *sv,
                               int64_t *out_i, double *out_f, void *out_s);
TurJitFfiThunk tur_jit_ffi_thunk(const char *sig);
```

Implementation: render the signature to a ~10-line C source string
(exact cast-and-call, same shape as the generated
`tur_ffi_call_*` bodies in `src/runtime/ffi_dispatch.c:28-30` but for
the precise signature), feed it through `c2mir_compile` + `MIR_gen`
on the engine's context (`src/jit_engine.c` already owns one), cache
the resulting pointer in a hash table keyed by the signature string.
Thread-safety: single-threaded interpreter today; take the JIT
engine's existing lock discipline if that changes.

Marshalling on top reuses `ffi_thunk.c`'s existing layer
(`marshal_arg_i` / `marshal_arg_f`, `src/turi/ffi_thunk.c:42-61`)
unchanged, plus a new struct marshaller.

### 2.3 Callback thunks (reverse direction)

```c
void *tur_jit_ffi_callback(const char *sig, TuriEnv *env, TuriValue fn);
```

Generates per-instance C of the form:

```c
int64_t __tur_cb_17(int64_t a0, double a1) {
    return tur_jit_ffi_dispatch((void *)0x<CTX_ADDR_LITERAL>, a0, a1);
}
```

where the context (a heap cell pinning `env` + the Turmeric closure) is
embedded as an address literal, and `tur_jit_ffi_dispatch` is a fixed
exported runtime symbol (visible because `tur` links with
`ENABLE_EXPORTS`, `src/CMakeLists.txt:413`). Lifetime: callbacks are
process-lifetime by default (matching turi's existing closure policy);
an explicit `callback-free!` can reclaim the MIR module later if
needed.

### 2.4 Fallback ladder (who answers a dynamic call)

Order at the `ffi_native_shim` level (`src/turi/ffi_thunk.c:98-215`):

1. **JIT thunk** (`tur_jit_ffi_thunk`) -- JIT builds; any signature.
2. **Per-export `__ffi` shim** (`e->ffi_shim`, `ffi_thunk.c:175-180`)
   -- spice exports, all builds; already arity-unlimited.
3. **Generated table** (`tur_ffi_thunk_call`, `ffi_thunk.c:181-201`)
   -- non-JIT builds, shapes within `--max-arity`; error message keeps
   pointing at rebuild/regenerate as today.

The generated table is thereby demoted, not deleted -- non-JIT builds
lose nothing, and deleting it later becomes a separate, optional
cleanup once JIT builds are the default REPL configuration.

### 2.5 Language surface

- **Interpreter, extern-c:** on `EX_EXTERN_C` registration
  (`src/turi/eval.c:8454-8465`), JIT builds resolve the symbol
  (`dlsym(RTLD_DEFAULT)`, falling back to `jit_load_autolink`'s
  dlopened libs, `src/jit_engine.c:387-414`) and bind a thunk-backed
  native instead of `native_nil_stub`. The 7-entry known table stays
  as an override for the semantics-bearing entries (`free` must remain
  a no-op in turi).
- **Both paths, new form:** `(unsafe (call-ptr p [T1 T2 -> R] args...))`
  -- requires an `unsafe` block exactly like `c-call`
  (`elab_c_call`, `src/compiler/elab_unsafe.c:659`, is the template).
  AOT codegen emits the direct cast-and-call; turi routes it through
  `tur_jit_ffi_thunk` (non-JIT interpreter builds report a clean
  "requires a JIT-enabled build" diagnostic, never nil).
- **Interpreter capability gate:** thunk calls sit behind the existing
  `TURI_CAP_FFI` bit (`src/turi/env.h:138-145`), same as
  dlopen/dlsym.
- **Experiment gating:** ~~per the strict rule, this ships as an
  `EXPERIMENTS[]` row (`--enable=jit-ffi`, full descriptor, plan_path
  pointing here, `experiment_warn_if_used` in the `call-ptr`
  elaboration).~~ **Discharged 2026-08-21:** the row shipped as described
  in 0.34.0 and graduated in 0.38.0 at its own `expires_at`. The name is in
  `GRADUATED[]`, so a downstream `build.tur` that still says
  `:experiments [:jit-ffi]` keeps compiling as a TUR-W0063 no-op.

## 3. Phases

- **F1 -- call thunks.** `jit_ffi.c`, signature cache, wire into the
  fallback ladder ahead of `ffi_shim`. Fixture: a spice export with
  arity > 16 and a mixed int/float signature callable from the REPL
  without an `__ffi` shim (delete the shim from the fixture's manifest
  build to prove the path).
- **F2 -- extern-c for real in turi.** Thunk-backed registration on
  `EX_EXTERN_C`; keep the known-table overrides. Fixtures under
  `requires.interp-only` asserting a real libc call (e.g. `strtol`)
  returns a real value under `--interpret` in a JIT build.
- **F3 -- `call-ptr`.** Elaboration + AOT codegen + turi routing +
  experiment row. Fixture: dlopen a test .so, dlsym, call-ptr, both
  paths.
- **F4 -- struct-by-value.** DONE 2026-08-18. Extend signatures with
  `"{...}"`, struct marshaller, retire `'?'` errors for registered
  layouts. Expect MIR-fork ABI patches here; budget for it.
- **F5 -- callbacks.** DONE 2026-08-18. `tur_jit_ffi_callback` +
  `callback-free!`. Fixture: qsort(3) with a Turmeric comparator under
  `--interpret`.

F1/F2 are independently shippable and carry most of the value; F4/F5
can trail indefinitely.

### Open after F5

Resolved 2026-08-21 (the F4/F5 follow-on batch):

- **x86-64 verification: DONE, and it found a real bug.** Measured on an
  x86-64 host against cc-compiled callees for every SysV class -- packed
  float pair (one SSE eightbyte), two-SSE, INTEGER+SSE both orders,
  single-GP, MEMORY-class (>16 bytes), and aggregate returns of each. The
  *flat* path was already correct. The *nested* path was not, on every
  architecture: a nested by-value record field's `CtorField.kind` is the
  int64 carrier, so the interpreter's sig renderer classed it `'q'` and
  built a thunk for a struct shape the callee does not have (`{{ww}w}`
  passed as `{qw}` -- silent wrong answers, the aarch64 failure mode
  without the aarch64). Fixed: the sig renderer now recurses into nested
  by-value record fields, mirroring codegen's `adt_field_is_inline_byval`;
  the marshaller flattens/rebuilds nested records leaf-by-leaf. A TY_APP
  monomorph field (also inlined by codegen, but needing per-application
  substitution) is refused cleanly. Regression: run-flags
  `jit-ffi-call-ptr-struct-nested-interp` (hermetic native .so).
- **Aggregates inbound to a callback: DONE.** `tur_ffi_cb_dispatch` grew
  the sv/out_s channels; the generated callback takes the record by value
  and hands its address through (aggregate return: dispatch packs into the
  callback's zero-initialized return slot). Elaboration now admits record
  slots (kind-checked against the function; identity where the fn type
  carries it), and the compiled per-site adapter passes aggregates through
  uncast (C has no struct casts). The provider refuses an HFA on aarch64
  inbound exactly as outbound -- the native caller writes v0..v7, the
  c2mir callee would read x0..x7. Regression: run-flags
  `jit-ffi-callback-struct-{interp,compiled}`.
- **extern-c with aggregate parameters (and returns): DONE.** It needed
  more than the plan guessed: `ExternC` carried only TypeKinds, so
  elaboration now keeps the full record type for a `[v : SomeRecord]`
  annotation (validated: single-constructor, non-`:heap`, non-parametric),
  sets `result_full_type` so call sites type an aggregate return correctly,
  and the prototype emitter spells the record's C type. In turi, an
  aggregate-signature extern-c registers a thunk-backed native that packs
  record args / rebuilds a record return through the F4 layout engine.
  Regression: run-flags `jit-ffi-extern-c-struct-{interp,compiled,rejects-heap}`.
- **Scalar width fidelity: DONE, and it was a live wrong-answer bug, not
  the cosmetic inconsistency this list described.** Collapsing every
  integer width onto `'i'` was indeed benign for *arguments* -- but the
  *return* direction was not: a C callee returning `int` leaves the upper
  half of the return register unspecified, so a thunk declared to return
  `long long` read whatever was there. `neg_int(1234)` came back as
  `4294966062` instead of `-1234`, for any negative or high-bit result.
  The vocabulary now carries exact-width codes (`b`/`h`/`w`, capitalized
  for unsigned) so the thunk declares the callee's true C type and the
  cast extends by signedness. Argument compatibility is unchanged (any
  int-class value into any int-class slot); only the emitted C declaration
  got precise. Regression: run-flags `jit-ffi-narrow-return`.
- **A leak in `tur_ffi_register_extern_thunk` (found by this work).** The
  per-registration payload was `calloc`'d and never freed, so a procedural
  macro -- which runs turi *inside* `tur build` -- leaked one per extern-c
  declaration and LeakSanitizer failed the compile. Eight fixtures were
  red under a JIT build; the default non-JIT suite never saw it because no
  provider is installed there, so the thunk path is unreachable. Both
  extern-c registrations now use `turi_env_register_native_ex` with a
  finalizer, which is exactly what that API is for.

Still open, not blockers on anything:

- **The aarch64 HFA gap in MIR** (the F4 boundary). Fixing it is vendored
  backend work in `rjungemann/mir`: an HFA classifier in
  `c2mir/aarch64/caarch64-ABI-code.c`, plus a SIMD-register path for that
  blk class in both `mir-gen-aarch64.c` (call lowering) and `mir-aarch64.c`
  (the hand-encoded interface thunks, where `gen_ld_pat` currently only
  encodes GP loads). Landing it means pushing to the fork and bumping the
  pin in `cmake/mir.cmake` -- mind the CACHE-VARIABLE trap at lines 67-76.
  Until then the interpreter refuses the case, in both directions.
- **TY_APP monomorph fields in interpreter aggregates.** Codegen inlines a
  concrete parametric monomorph field (`(Pair2 int cstr)`) by value, but
  rendering its layout interpreter-side needs per-application type
  substitution; refused cleanly today rather than mis-described.

## 4. Risks / open questions

- **c2mir compile errors at runtime** must surface as diagnostics, not
  aborts: capture c2mir's error stream per compile and wrap in a
  `result`-shaped native error.
- **`dlsym(RTLD_DEFAULT)` symbol visibility** differs macOS vs Linux
  for symbols in the main executable; `ENABLE_EXPORTS` covers our own
  runtime, but extern-c against a lib the process never linked needs
  `jit_load_autolink` or an explicit dlopen first. Document the
  resolution order.
- ~~**Signature registry for structs** needs a single source of truth
  shared between elaborator and turi; the monomorph layout tables are
  the likely donor. Scope carefully in F4 -- this is the phase most
  likely to grow.~~ **Dissolved in F4:** the layout is encoded inline in
  the sig string rather than referenced by hash, so a sig is
  self-describing and there is no registry to share. See the F4-F5
  implementation notes at the top.
- **Sanitizers:** `tur_mir` is deliberately built unsanitized
  (`cmake/mir.cmake:118-126`); calls crossing into thunked code are
  invisible to ASan. Same status quo as the JIT today; note it in the
  guide.
- **If Windows/WASM ever need this**, swap the thunk provider behind
  `tur_jit_ffi_thunk` for dyncall (best CMake citizen, ISC) or libffi
  (deepest ABI coverage, MIT, autotools upstream) -- the interface is
  the insulation layer; nothing above it changes.
