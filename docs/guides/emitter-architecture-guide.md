---
title: C Emitter Architecture
category: Compiler Internals
description: How the C emitter turns an elaborated program into one translation unit -- the emit_value/emit_stmt contract, EmitCtx, the phase order of emit_program, ABI specializations, the side tables call sites consult, inline C, function definitions, and how to add an emit arm
---

# C Emitter Architecture (internals)

This guide is for contributors changing how `src/compiler/emit_*.c` turns an
elaborated `EX_PROGRAM` into C. It covers the direct-style emitter: the
value/statement contract, the `EmitCtx` state, the order in which
`emit_program` builds a translation unit, ABI specializations, the side tables
call sites consult, inline C, and function definitions. It ends with a worked
trace and a checklist for adding an `Expr` kind.

Out of scope, with the guide that covers each:

| Topic | Guide |
|---|---|
| The pipeline before emission | [compiler-internals](compiler-internals.md), [elaborator-internals-guide](elaborator-internals-guide.md) |
| The CPS backend and the DK runtime | [cps-dk-internals-guide](cps-dk-internals-guide.md) |
| Which representation a value travels in, and the bridges | [value-representations-guide](value-representations-guide.md), [type-erasure-guide](type-erasure-guide.md) |
| Monomorphization policy and the ABI cache | [monomorphization-abi-guide](monomorphization-abi-guide.md) |
| How names become C identifiers | [name-mangling-guide](name-mangling-guide.md) |
| Dictionaries and instance dispatch | [typeclass-internals-guide](typeclass-internals-guide.md) |
| Drops, frees and ownership in emitted code | [emitted-code-ownership-guide](emitted-code-ownership-guide.md) |
| The runtime preamble text and linkage | [runtime-preamble-and-linkage-guide](runtime-preamble-and-linkage-guide.md) |
| Region brackets and store hooks | [regions-internals-guide](regions-internals-guide.md) |
| `emit_header` / `emit_implementation` (multi-module builds) | [separate-compilation-internals-guide](separate-compilation-internals-guide.md) |
| The `#lang r7rs` split build | [r7rs-implementation-guide](r7rs-implementation-guide.md) |

> **Trust the code over the comments.** The emitter's comments are long and
> mostly accurate about *why*, but many are stale about *where* and *how
> much*. Examples: `emit_expr.c` describes `emit_value_dispatch` as "~10k lines
> over 124 cases" (it is ~12.9k lines over the 120 `ExprKind`s), the "Final
> assembly order" comment in `emit_program_inner` lists 9 bands where the code
> writes about 20, and a comment names `emit_join_coerce`, a function that does
> not exist. When this guide and the code disagree, the code wins; fix the
> guide.

---

## The short version

1. **One entry point per output.** `emit_program` writes a whole single-file
   TU. It trampolines onto a big stack (`tur_run_on_big_stack`) and calls
   `emit_program_inner`.
2. **Scan first, emit second.** Before any C is written, the ABI scan walks
   every item, decides which generic calls need a per-type clone, and records
   which call uses which clone.
3. **Many buffers, assembled at the end.** Typedefs, forward declarations,
   function bodies, `main`, static boxes and initializers are emitted into
   separate `Buf`s. The runtime preamble is *generated* after every body but
   *written* first.
4. **Expressions become strings plus statements.** `emit_value(ctx, body, e)`
   appends whatever statements `e` needs to `body` and returns a malloc'd C
   expression for its value. `emit_stmt` emits `e` for effect only.
5. **Every value call is hoisted.** A non-void call becomes
   `T __ps_N = (call);` followed by `if (tur_panicking) return <zero>;`. That
   check is how a panic unwinds on the compiled path.
6. **Ground truth over inference.** A call site does not re-derive the
   callee's C types. It looks them up in the signature table that the
   forward-declaration pass filled with what it actually printed.
7. **Functions try CPS first.** `emit_fn_def_body` asks
   `emit_cps_ir_try_fn`; only a function it declines is emitted in direct
   style.

---

## Source map

| File | Lines | Role |
|---|---|---|
| `src/compiler/emit.h` | 150 | Public API: `emit_program`, `emit_header`, `emit_implementation`, `emit_exports_manifest`, `emit_shared_runtime_header`, `emit_rt_split_source`, the split-build controls |
| `src/compiler/emit_internal.h` | ~1950 | `EmitCtx`, `EmitAbiSpecialization`, and the cross-file prototypes, grouped by defining file |
| `src/compiler/emit_module.c` | ~23.5k | `emit_program_inner` and the multi-module drivers; the ABI scan (`emit_abi_*`); the signature and local-variable side tables; forward declarations; ADT typedefs and constructors; shims, adapters and static fat boxes; the runtime preamble (`emit_runtime_preamble`); the split-build bookkeeping |
| `src/compiler/emit_expr.c` | ~22.5k | `emit_value` and `emit_value_dispatch` (the expression switch, ~12.9k lines); the call arm; the control-form merge temps; the panic-signal return |
| `src/compiler/emit_core.c` | ~8.2k | Helpers: `fresh_tmp`, `indent_buf`, naming (`name_for_binding`, `raw_name_for_binding`, `emit_call_name`), atoms, `emit_builtin`, `inline_c_substitute`, include hoisting, the carrier bridge, escape analyses, re-resolution of typeclass calls inside a spec |
| `src/compiler/emit_fns.c` | ~7k | `emit_fn_def` / `emit_fn_def_body`; tail calls (`tco_mark`, `emit_tail`, mutual tail-call groups); the stackless catch-unwind splitter (`gs_*`); the `tur:inline-c-fns` list |
| `src/compiler/emit_stmt.c` | ~1.7k | `emit_stmt` (the statement switch), `while`, `set!`, field and deref stores, instance dictionaries |
| `src/compiler/emit_effects.c` | ~1.6k | Expression arms for `reset`/`shift`/`handle`/`resume` and handler values when the direct emitter reaches them |
| `src/compiler/emit_split.c`, `.h` | ~1.2k | The text transform for `#lang r7rs` split builds (`emit_split_state`, `emit_split_rename`) |
| `src/compiler/emit_cps_ir.c`, `emit_dk_runtime.c` | -- | The CPS backend and DK runtime text; see [cps-dk-internals-guide](cps-dk-internals-guide.md) |
| `src/compiler/stack_guard.c`, `.h` | -- | `tur_run_on_big_stack`, `tur_stack_nearly_exhausted` |
| `src/compiler/types.c` | -- | `type_c_name`, the ADT-app monomorph registry (`type_codegen_emit_adt_apps`), `repr_of` |

---

## Entry points and the big stack

`emit.h` exposes four emission jobs. Each is a thin wrapper that runs its
`*_inner` function through `tur_run_on_big_stack`:

| Entry | Inner | Used for |
|---|---|---|
| `emit_program` | `emit_program_inner` | `main.c:compile_to_c`, which `cmd_emit_c`, `cmd_build_once`, `cmd_jit` and the REPL's JIT build call |
| `emit_header` | `emit_header_inner` | per-module `.h` in a multi-module build |
| `emit_implementation` | `emit_implementation_inner` | per-module `.c` in a multi-module build |
| `emit_exports_manifest` | `emit_exports_manifest_inner` | `exports.manifest` for `--shared` |

**Why the big stack.** The expression walk is plain structural recursion
(`emit_value -> emit_value_dispatch -> emit_builtin -> emit_value ...`), and
its depth follows the source. Macro expansion can nest far deeper than
hand-written code, and an ASan frame is many times its normal size. A fixed
depth limit was tried and failed in both directions
([emit-depth-guard-loses-race-with-asan-stack](../archive/emit-depth-guard-loses-race-with-asan-stack.md)).
So the job runs on a pthread (a Windows thread on `_WIN32`) with a
`TUR_STACK_MB` stack, default 256 MiB, reserved lazily. The call is
re-entrant-safe: already on a big stack, it calls the job directly. `tur`'s
own `main` also trampolines, so in practice emission is usually already on
the big stack.

`emit_value` keeps a backstop: past depth 8 it checks
`tur_stack_nearly_exhausted()`. If the stack is nearly gone it reports
TUR-E0712 once, returns the string `"0"` so the rest of the TU stays
well-formed, and `emit_program` fails on the diagnostic.

`emit_program_job` also brackets the run with `emission_enter` /
`emission_leave`, which clear the CPS backend's address-keyed caches
(`emit_cps_ir_forget`), and appends the `tur:inline-c-fns` comment after the
inner function returns.

**Process-global state.** Several tables live in file-scope statics rather
than in `EmitCtx`, so they must be reset per program. `emit_program_inner`
opens with `emit_sig_reset`, `emit_localvar_reset`, `static_init_reset`,
`gs_reset_group_registry`, `tcg_reset_group_registry`,
`type_codegen_reset_adt_apps`, `type_codegen_reset_fn_ptr_typedefs` and
`sym_codegen_reset`. A new file-scope table needs a reset there, or `tur lsp`
and `tur mcp`, which compile many programs in one process, will see the
previous program's entries.

---

## The value/statement contract

### `emit_value`

```c
char *emit_value(EmitCtx *ctx, Buf *body, const Expr *e);
```

- It **appends** any statements `e` needs to `body`, each indented to
  `ctx->indent`.
- It **returns** a malloc'd C expression that yields `e`'s value at the point
  just after those statements. The caller owns the string and frees it.
- The returned text is valid only *in that position*. It may name temps the
  statements declared, so it must be consumed in the same scope, after them.
- The `nil` literal, and builtins such as `println` that are emitted as
  statements, come back as the atom `((void)0)` (`atom_nil`). A call to a
  `: nil` function comes back as the bare call text, which the consumer must
  emit as a statement.

`emit_value` itself is a wrapper. It checks the G3 hole table and the stack
backstop, opens a region bracket around a `bt-scope` call, records the
pending-drop marks, dispatches to `emit_value_dispatch`, and then, for an
`EX_CALL` with a non-`nil`, non-`!` type, **hoists the call** (next section).

### `emit_stmt`

```c
void emit_stmt(EmitCtx *ctx, Buf *body, const Expr *e);
```

`emit_stmt` emits `e` for its effects and discards the value. Pure leaves
(literals, variables, pure tag tests) emit nothing. Wrapper nodes (`EX_ASCRIBE`,
`EX_CAST`, `EX_REINTERPRET`, `EX_POLY_WRAP`) delegate to their inner
expression: they used to be in the emit-nothing group, which silently deleted
a discarded wrapped call. Statement-only forms (`while`, `set!`, field
stores, `EX_INLINE_C` with its trailing `;`) have their own arms. Most other
arms call `emit_value` and drop the string. A call in statement position
becomes `(void)(__ps_N);` after its hoist, or `f(args);` for a `: nil`
callee, bracketed by `emit_pending_drops_mark` / `_drain`. Most kinds need
an arm in both switches (see the checklist below).

### What turns into statements

A construct must be emitted as statements first whenever C has no expression
form for it, or when something has to run between it and its consumer:

| Construct | Emitted as |
|---|---|
| A value-returning call | `T __ps_N = (call);` plus the panic check; the value is `__ps_N` |
| `if`/`match`/`let`/`do` in value position | a merge temp `T __tN;` declared first, a C block or `if` that assigns it, then the value `__tN` |
| `and`/`or` | `bool __tN = a; if (__tN) { __tN = b; }` (`emit_builtin`, `BS_AND_SC`/`BS_OR_SC`) |
| `println` and other `BS_PRINTLN_*` builtins | a `printf`/`puts` statement; the value is `((void)0)` |
| An inline-C block's `__TUR_VAL_N__` operands | one temp per operand, ahead of the spliced text |
| A by-value aggregate crossing to the carrier | a spill or heap box, ahead of the use (`emit_carrier_bridge`) |

Everything else (arithmetic, comparisons, variable reads, field reads,
literals) comes back as expression text, and the consumer embeds it.

### Temps and names

- `fresh_tmp(ctx)` returns a malloc'd `__tN`. The call hoist names its temp
  `__ps_N`. Both draw on `ctx->tmp_n`, which starts at 0 per program and is
  **never reset per function**, so every temp name is unique in the TU. The
  local-variable side table depends on that uniqueness.
- A local binding is spelled `<name>_<id>` (`k_10`); a parameter, or a local
  an inline-C block names, uses its raw mangled name (`name_for_binding` vs
  `raw_name_for_binding`). The spelling rules are in the
  [name-mangling guide](name-mangling-guide.md).

### Indentation

`ctx->indent` is the column of the next statement, and `indent_buf(body,
n)` writes `n` spaces. It starts at 4. `emit_fn_def_body` adds 4 for a
function body, so function bodies are indented 8, and the emitter-synthesized
`main` body (see below) is indented 4. Every construct that opens a C block
adds 4 around its contents and subtracts after. Inline-C text is spliced
verbatim after one `indent_buf`, so only its first line is reindented.
Indentation is cosmetic, but it is in every `expected.c` snapshot, so an
off-by-four is a snapshot diff.

### Worked example

```turmeric
(defn twice [x : int] : int
  (* 2 x))

(defn calc [c : bool n : int] : int
  (let [k (+ 1 (if c (twice n) 3))]
    (+ k (twice k))))
```

`tur emit-c` produces:

```c
static int64_t calc(bool c, int64_t n) {
        int64_t __t202;                       /* the let's merge temp */
        {
            int64_t __t203;                   /* the if's merge temp */
            if (c) {
                int64_t __ps_204 = (twice(n));        /* call hoist */
                if (tur_panicking) return ((int64_t)0);
                __t203 = __ps_204;
            } else {
                __t203 = INT64_C(3);
            }
            int64_t k_10 = (INT64_C(1)) + (__t203);
            (void)k_10;
            int64_t __ps_205 = (twice(k_10));
            if (tur_panicking) return ((int64_t)0);
            __t202 = (k_10) + (__ps_205);
        }
        return __t202;
}
```

(The comments are added.) `(+ 1 (if ...))` came back as the expression
`(INT64_C(1)) + (__t203)`, but only after the `if` had written its statements.

### The ordering hazard

The contract has one sharp edge. An argument that comes back as expression
text is evaluated where the *consumer* embeds it, which is after every later
argument's statements have run. If a later argument hoists a call that
mutates what the earlier text reads, the earlier read sees the new value:

```turmeric
(def ^mut counter 1)
(defn bump [] : int (set! counter (+ counter 100)) 5)
(defn main [] : int (println (+ counter (bump))) 0)
```

emits

```c
int64_t __ps_203 = (bump());
printf("%lld\n", (long long)((counter_4) + (__ps_203)));
```

and prints `106`, while `tur --interpret` prints `6`. The same happens with
`(sub counter (bump))` through a user function, which prints `96` against the
interpreter's `-4`. This is a defect in the current code, not a rule to
follow: a new arm that returns a variable read or a field read as text
inherits it. An arm that must preserve order has to spill its earlier
operands into temps when a later operand writes to `body`.

### Pending drops

Owned temporaries that an argument produces (an `any` box, a fresh sum-carrier
box, a by-value monomorph holding a boxed payload) are freed once the
consuming call has run. `emit_value` records `n_any_pending`,
`n_sum_pending` and `n_vsp_pending` before dispatch and drains back to those
marks after the call hoist. The drain hangs off the hoist because that is the
first statement boundary after the call. A `nil` or `!` call has no hoist, so
it leaves its entries for an enclosing call. A statement-position call uses
`emit_pending_drops_mark` / `emit_pending_drops_drain` instead. The ownership
reasoning is in [emitted-code-ownership-guide](emitted-code-ownership-guide.md).

---

## The call hoist, the panic check and `call_ret_note`

Every value call goes through the tail of `emit_value`:

1. **Name the temp's type.** In order of preference:
   1. the callee's return type from the signature table
      (`emit_sig_lookup_ret_ctype`), when the call text starts with an
      identifier;
   2. `ctx->call_ret_note`, which the call builder sets to the type it spelled
      into an indirect call's cast or thunk typedef;
   3. two anchored reads of emitter-written text: a `((RET (*)...` cast head
      or a `(T)(...)` cast wrap with a primitive or pointer `T`;
   4. otherwise `__auto_type`.

   Naming the type is not cosmetic. c2mir (the JIT's C front end) cannot parse
   `__auto_type`, and the local-variable side table only records a temp
   whose type is known. See S1 in the
   [JIT engine plan](../archive/jit-engine-plan.md).
2. **Write** `T __ps_N = (call);` and record `__ps_N`'s C type with
   `emit_localvar_record_ctype`.
3. **Mark** ownership facts about the temp: a raw Vec slot word
   (`emit_slot_word_mark`), or an owned Option/Result carrier box minted by an
   inline-C body or a fresh constructor (`emit_owned_carrier_mark`).
4. **Close** an open region bracket on the panic arm
   (`TUR_REGION_RETIRE_TO`).
5. **Emit the panic check**, `emit_panic_signal_return`:
   - `if (tur_panicking) return <zero of the function's C return type>;`
   - `return;` in a `void` function;
   - `break;` inside the stackless catch-unwind driver
     (`ctx->panic_signal_is_break`);
   - with defer frames open, the check first fires them, with the signal
     cleared, through the inlined `tur_frame_fire_lifo`;
   - with no known return type (`main`, top-level code), a comment
     `/* panic-return-signal: ret ctype unknown; no propagation here */`.
     An uncaught panic aborts inside `tur_panic`, so nothing needs to unwind
     there.
6. **Drain** the pending drops, push this temp as a pending drop if it is
   itself an owned argument, and close the region bracket on the normal path.

**A `: nil` call is not checked.** `emit_value` returns early for a `nil` or
`!` call, before the hoist, so no `if (tur_panicking)` follows it. The
statements after it run with the panic pending, until the next value call's
check returns. Under `catch-unwind` that is observable:

```turmeric
(defn boom [n : int] : nil (when (> n 0) (panic "boom")))
(defn work [n : int] : int (boom n) (println "after boom") n)
```

emits `boom(n); puts("after boom"); ...`, so `(catch-unwind (fn []
(work 1)))` prints `after boom` on the compiled path and not under
`tur --interpret`. Like the ordering hazard, this is a defect, not a
convention.

**`call_ret_note` protocol.** A builder sets the note as the *last* thing
before returning its call string (`note_call_ret`). `emit_value` copies and
clears it unconditionally after every dispatch, so a note from a void call can
never leak onto a later one. After the hoist, `emit_value` re-sets the note to
the temp's declared type, so a caller that cleared the note before emitting an
argument can read back what the argument was declared as.

**Tail calls skip the hoist.** When `emit_tail` asks for a call un-hoisted
(`ctx->tail_call_no_hoist == e`), `emit_value` returns the bare call text, so
the function can end in `return f(args);`, a real C tail call. It agrees only
when the callee's recorded return type equals the enclosing function's, no
pending drop or region bracket hangs off the call, and the call is not a raw
slot read. It reports the answer in `tail_call_no_hoist_taken`, and
`emit_tail` falls back to the hoisted spelling on a refusal. The nearest
non-tail caller performs the panic check
([proper-tail-calls plan](../archive/proper-tail-calls-plan.md)).

**Panic sites.** A direct call to a function whose inline-C body calls
`tur_panic(` is wrapped so the thread's current site is its source location
(`emit_site_wrap`; `tur_site_push` / `tur_site_pop`). Inside such a body the
emitter `#define`s `tur_panic` to `tur_panic_sited` for the length of the
body. That is why `(vec-get v 9)` reports the caller's line
([panic-location-names-the-runtime-not-the-call-site](../archive/panic-location-names-the-runtime-not-the-call-site.md)).

---

## Side tables: ground truth about what was printed

The emitter's types (`Type`) often say less than the C it printed: a generic
result c-names to the `int64_t` carrier while a clone returns a pointer, and
an earlier rule may have retyped a function's C return. So call sites consult
three tables keyed by **emitted C name**, filled from what was actually
written:

| Table | API | Filled by | Read by |
|---|---|---|---|
| Signatures | `emit_sig_record_param_ctype`, `emit_sig_record_ret_ctype`, `emit_sig_lookup_*` | the forward-declaration pass (`emit_fn_forward_decls`, `emit_abi_forward_decl`), extern-c forms, ADT constructors, thin fn-pointer globals | the call hoist; the int64<->pointer argument bridges; `tail_call_musttail_ok` |
| Local C types | `emit_localvar_record_ctype`, `emit_localvar_lookup_ctype` | the call hoist; let binders; merge temps | every carrier bridge deciding whether a value needs normalizing (`emit_value_is_recorded_as`) |
| Ownership marks | `emit_owned_carrier_*`, `emit_slot_word_*` | the call hoist | the carrier->concrete bridge (free after copy); the niche bridge |

Rules for these tables:

- **Record what you print.** A new site that prints a function signature must
  record it, or every call to that function falls back to `__auto_type` and
  breaks the JIT. Extern-c return types are recorded twice for this reason:
  once before any body is emitted, and again per item.
- **Never record a guess.** The `__auto_type` arm records nothing, on purpose.
- **Pointers are stable.** `emit_sig_lookup_ret_ctype` returns an interior
  pointer, and callers hold it across further emission. Re-recording retires
  the old string instead of freeing it (`emit_sig_retire`); the strings are
  freed at `emit_sig_reset`.

The representation choices these tables let the emitter verify are described
in the [value representations guide](value-representations-guide.md). In a
Debug build, the repr shadow (`repr_shadow_check`) compares each declared C
type with `repr_of`'s answer and reports disagreements.

---

## `EmitCtx`: a map of the struct

`EmitCtx` (`emit_internal.h`) is about 600 lines and 175 fields. It is
`memset` to zero at the start of `emit_program_inner`; a new field starts as
zero and is freed (if owned) in the long cleanup block at the end of the
function. Most fields are save/set/restore context for one construct. The
groups:

| Group | Fields (examples) | Purpose |
|---|---|---|
| Output buffers | `file`, `main_`, `thunk_typedefs`, `fatbox_init`, `fatbox_defs`, `pending_handler_fns` | Where helper text goes. `file` is redirected per function (see below); `thunk_typedefs` is the band ahead of the forward declarations, where on-demand typedefs, shims and runtimes land |
| Statement emission | `indent`, `tmp_n`, `return_emitted`, `lvalue_mode`, `dbg_last_line`/`dbg_last_file_id` | Indentation, temp numbering, `#line` dedup under `--debug` |
| Call-hoist notes | `call_ret_note`, `any_field_read_note`, `tail_call_no_hoist`, `tail_call_no_hoist_taken` | One-shot messages between a builder and `emit_value` |
| Pending drops | `any_pending`, `sum_pending`, `vsp_pending`, `any_scope_drops`, `sum_drop_admit` | Owned temporaries to free after their consuming call or at scope exit |
| Current function | `fn_params`, `inline_c_raw_locals`, `pbp_param_ptrs`, `current_fn_ret_ctype`, `own_cur_fn`, `no_unwind`, `mt_fn_cname`/`mt_body_buf` | Who is being emitted: raw-named params, pass-by-pointer params, the C return type the panic check zeroes |
| Closures and scopes | `closure`, `env_var_name`, `frame_var`, `in_scope_with_defers`, `defer_captures`, `dynvar_guard_*`, `letrec_knot`, `handle_captures`, `gen_*` | Lexical context: the env being read, the defer frame chain, dynamic-binding guards, the letrec knot, generator `_next` bodies |
| Specializations | `abi_specializations`, `specialized_call_exprs`/`_names`/`_outer`, `carrier_call_bindings`, `current_abi_specialization`, `fn_name_override`, `current_scan_fn`, `type_arena` | The ABI-spec table and call->clone map (next section) |
| Dictionary dispatch | `dict_dispatch_*`, `cur_dict_env_*` | Inside a dict-clone or dict-capturing closure, route a class-method call through the runtime dict |
| Dedup registries | `env_struct_names` (+ `_fn_typedefs`, `_cap_ctypes`), `thunk_typedef_names`, `fatshim_names`, `poly_fatshim_names`, `fatbox_keys`, `exbox_dict_names`, `emitted_gen_defs`, `dead_base_ctor_*` | "Emit this helper once per TU" sets. A helper is emitted the first time a site needs it and its spelling is recorded, so later sites agree with it |
| `any` / Saffron | `any_type_*`, `any_widen_ids`, `inst_row_*`, `saffron_dyn_emitted`, `any_bridge_emitted` | Per-TU `any` type ids and the runtime instance-row registry |
| Tail calls | `match_tail`, `tcg_fn_exprs`, `tcg_cur`, `dyn_tail_mode`, `tb_guard`, `tco_fn_e`, `tail_void`, `musttail_macro_emitted` | Self/mutual tail calls, bouncing dynamic tail calls |
| CPS cooperation | `in_cps_fn`, `program_root`, `sub_holes`/`sub_names`, `panic_signal_is_break` | State the CPS backend and the stackless catch-unwind splitter set while they delegate to `emit_value` |
| Separate compilation | `separate_compilation`, `fn_name_override_external` | Linkage of clones and exports in a multi-module build |

Two disciplines apply to the whole struct:

- **Save and restore around nested emission.** `emit_fn_def_body` saves and
  restores a dozen fields (`dict_dispatch_*`, `current_fn_ret_ctype`,
  `own_cur_fn`, `file`, `tcg_cur`, ...). A spec body or a lifted lambda may be
  emitted in the middle of another body. A field set without a restore leaks
  into whatever is emitted next.
- **Never hold a pointer into `abi_specializations` across interning.**
  `emit_abi_intern_spec` `realloc`s the array. It re-points
  `current_abi_specialization` itself; any other saved pointer must be saved
  as an index (`emit_abi_register_call` does this around its recursive scan).

---

## Phase order inside `emit_program_inner`

The function builds the TU in this order:

1. **Resets** of the side tables and registries listed above, and a first
   pass recording every `extern-c` return type.
2. **Context setup.** Allocate the buffers (`early_file`, `thunk_typedefs`,
   `fwd_decls`, `extern_decls`, `defer_thunks`, `file`, `body`,
   `def_init_body`, `cprelude*`, `fatbox_*`), zero and fill `ctx`, create the
   type arena. In a split build, write the stateful lazy runtimes
   (`ensure_saffron_dyn_runtime`, `ensure_r7rs_cast_helper`) into `rt_lazy`
   up front so every unit writes the same set.
3. **Flatten** the program (`flatten_program_items`, which expands
   `defmodule` bodies). A split library unit then drops items it does not own.
4. **ABI scan.** `emit_abi_scan_program` walks every item and interns
   specializations, then `emit_abi_carrier_relay_closure` closes the
   carrier-call set over relays. With `--emit-abi-trace`, one `abi-trace` line
   is printed per source call site (not per spec).
5. **ADT-app scan.** `scan_adt_apps_in_expr` registers concrete ADT
   applications for monomorphization.
6. **Find `main`.** Sets `user_has_main`. A near-miss name (`-main`, `Main`,
   ...) with no top-level statements gets TUR-W0624 later.
7. **Pass 0, into `early_file`:** every `defdata`/`defgadt` typedef, its drop
   glue and its base constructors; then the dynamic-variable runtime when the
   program has a `defdynamic`.
8. **Pass 1, into `fwd_decls`:** `emit_fn_forward_decls` (one prototype per
   emitted function, recording each into the signature table),
   `emit_abi_forward_decl` per spec, and `emit_global_def_forward_decls`.
   Module-level `defer`s are collected. The `^thread-local` block goes into
   `file`.
9. **Pass 2, the item loop.** In source order:
   - `EX_DEF`: a global declaration into `file`; its initializer into `body`
     (no user `main`) or `def_init_body` (user `main`, or a split library);
   - `EX_FN_DEF`: `emit_fn_def` into `file`, unless
     `emit_abi_fn_skip_generic` suppresses the generic base;
   - `EX_EXTERN_C`: a prototype into `extern_decls`, unless the preamble or
     `hamt.h` already declares it;
   - `EX_INLINE_C` at top level: verbatim into a `cprelude` buffer, deduped
     and wrapped in `tur:inline-c-begin/end`;
   - anything else: `emit_stmt` into `body`.
10. **Spec bodies.** Every interned specialization is emitted with
    `emit_fn_def` under `current_abi_specialization` and
    `fn_name_override = clone_name`. A linked inner-closure clone is hoisted
    ahead of its outer so its env struct lands at file scope first.
11. **Lens monomorphs** (VBM2b, simple then composed) and consumer clones
    (CM2), through the same spec-body path.
12. **Module-level defers**: `__module_defer_N` functions registered with
    `atexit` from the static-init band.
13. **Generate and write** the preamble, the hoisted `#include`s and the
    panic-site slot straight into `out`; then generate the late bands (defer
    thunks, ADT-app monomorphs, symbol records, fn-pointer typedefs,
    dead-base-constructor traps) and write every band in the order of the
    table below.
14. **Entry point.** With no user `main`, synthesize
    `int main(int argc, char **argv)`; otherwise wrap `def_init_body` in
    `__tur_module_def_init`, registered in the last static-init band. Then
    the trailers (bands 16-18).
15. **Cleanup** of every owned `EmitCtx` array, then
    `return diag_had_error() ? 1 : 0`. A codegen diagnostic fails the build,
    and the partial C is discarded.

### Generated late, written first

The preamble (step 13) is written to `out` before anything else, yet it is
generated after every body. Two things in or next to it depend on body
emission: the hoisted `#include` set, which `inline_c_substitute` collects as
it splices bodies, and the on-demand runtimes and typedefs that land in
`thunk_typedefs`. The hoisted includes deliberately sit *after* the end
marker `/* ==== tur: end of fixed runtime preamble ==== */`, so the text above
the marker is identical for every program and the JIT's split runtime can
match its hash
([jit-s2-split-disengages-on-hoisted-inline-c-include](../archive/jit-s2-split-disengages-on-hoisted-inline-c-include.md)).

### Output bands

What `out` contains, in order (single-file, non-split):

| # | Band | Contents |
|---|---|---|
| 1 | preamble | runtime text up to the end marker (see the [preamble guide](runtime-preamble-and-linkage-guide.md)) |
| 2 | hoisted includes, panic-site slot | `#include`s lifted from inline-C bodies; `tur_site_t`, `tur_site_push`/`pop`, `tur_panic_sited` |
| 3 | `early_file` | ADT typedefs, drop glue, base constructors, dynvar runtime |
| 4 | symbol records | `struct __tur_sym` and one record per keyword |
| 5 | fn-pointer typedefs | for concrete fn fields of parametric ADTs |
| 6 | ADT-app monomorphs | `tur_adt_Option__int` and friends, with constructors |
| 7 | `thunk_typedefs` | typed thunk typedefs, shims, adapters, static fat-box declarations, lazy runtimes, the `any` name table |
| 8 | `extern_decls` | `extern` prototypes for `extern-c` forms |
| 9 | `fwd_decls` | every function and clone prototype, global forward declarations, dead-base-constructor traps |
| 10 | `defer_thunks` | defer env structs and thunk prototypes |
| 11 | `cprelude` | top-level inline-C blocks (`tur:inline-c-begin/end`) |
| 12 | `pending_handler_fns` | handler functions not yet flushed |
| 13 | `file` | `^thread-local` block, globals, every function definition, spec bodies, module defers |
| 14 | defer thunk bodies | after `file`, since they may name its globals |
| 15 | `main` or `__tur_module_def_init` | the entry point |
| 16 | fat boxes | `TUR_FATBOX_DEF` lines and `__tur_fatbox_init` |
| 17 | instance rows, GC roots, CPS groups | Saffron dispatch registry; r7rs GC; fused CPS tail-call groups |
| 18 | `__tur_static_init` | the explicit init sequence and its constructor |
| 19 | `tur:inline-c-fns` | appended by `emit_program_job` |

Bands 5 and 6 are generated in the opposite order to how they are written,
and both orders matter. Emitting a monomorph calls `type_c_name`, which is
what registers the fn-pointer typedefs, so the monomorphs are generated
first; a monomorph can name such a typedef, so the typedefs are written
first.

**Static initialization.** Code that must run before `main` registers with
`static_init_register(fn, band)` rather than using
`__attribute__((constructor))`, which c2mir discards. The bands run in order:
`STATIC_INIT_KEYS` (pthread keys, fat-box fills), `STATIC_INIT_REGISTRY`,
`STATIC_INIT_ATEXIT` (module defers), `STATIC_INIT_DEFS` (user def
initializers, last).

**Two `main`s.** When a program has no `main` and every top-level form is
plainly classifiable, the elaborator folds the top-level statements into a
synthesized `(defn main [] : int (do ... 0))`
(`elab_toplevel.c`; [cps-toplevel-synthesized-main-bypasses-dk](../archive/cps-toplevel-synthesized-main-bypasses-dk.md)).
The emitter then sees a user `main` and emits it with `emit_fn_def`. Only
when the fold aborts (a top-level `do`, a bare atom, an ambiguous macro call)
does step 14 synthesize `main` itself; that version is indented 4 and ends in
`return 0;`. With a user `main`, a top-level *statement* in `body` is dropped
without a diagnostic, while `tur --interpret` runs it
([top-level-def-init-dropped](../archive/history/top-level-def-init-dropped.md)
records this as long-standing behavior).

### Where the CPS backend plugs in

| Hook | Site | What it does |
|---|---|---|
| `emit_cps_ir_forget` | `emission_enter`/`emission_leave` | clear address-keyed caches |
| `emit_cps_ir_try_fn` | `emit_fn_def_body` | emit a function in `S` as `f__cps` plus its entry wrapper. The first call (the first function of the item loop, after the ABI scan) runs coloring and classification, `ensure_S` |
| `emit_cps_ir_program_has_emittable` | `emit_runtime_preamble` | gate the DK runtime text; by then `ensure_S` has normally run already |
| `emit_cps_ir_emits_binding` | forward declarations | skip a `--cps-path` wrapper the backend already provides |
| `emit_cps_ir_generic_base_is_template` | `emit_abi_fn_skip_generic` | drop a performing generic's carrier base |
| `emit_cps_ir_colored_fn_needs_mono` | `emit_abi_register_call` | force a clone of a colored generic |
| `emit_cps_ir_flush_groups` | trailer | fused CPS mutual tail-call groups |

The backend calls back into `emit_value` for every `CT_LETRAW` it delegates,
setting `ctx->in_cps_fn` and the `g_cps_panic_fire` hooks so the panic check
inside a `__cps` body fires its pending defers.

---

## ABI specializations

Generic functions are emitted once against the `int64_t` **carrier** (the
"base"). A call whose type arguments change the function's C signature, or
the instances its body dispatches to, needs its own clone. The scan decides
which calls those are before anything is emitted. The policy (when to
monomorphize, the ABI cache, multi-module borrowing) is in the
[monomorphization guide](monomorphization-abi-guide.md); this section is the
mechanism.

### Inputs: `abi_bindings`

The elaborator attaches the call's named-tyvar substitution to the `EX_CALL`
node: `call_.abi_bindings` / `call_.n_abi_bindings`, an array of
`AbiTypeBinding { const char *name; Type type; }`, at most 16, arena-owned.
`emit_abi_register_call` reads it rather than re-deriving the substitution.
No bindings means nothing to specialize, with two recoveries:

- Inside a spec body, a call that lost its bindings (a return-only
  polymorphic call, a forwarding wrapper) has them synthesized from the
  enclosing spec's result family.
- A binding whose type collapsed to the carrier but whose *name* survived is
  rehydrated from the enclosing spec's binding of that name.

### The scan

`emit_abi_scan_program` calls `emit_abi_scan_expr` on every item.
`emit_abi_scan_expr` is a structural walk (it has a `default:` arm). Its
`EX_CALL` arm:

1. calls `emit_abi_register_call`;
2. if no spec was recorded and the call is not a generic relay, notes the
   callee as a **carrier call** (`emit_abi_note_carrier_call`), so its base is
   kept;
3. inside an active spec, runs `emit_abi_scan_fn_values` on the arguments, so
   a generic function passed *as a value* is cloned under the same bindings;
4. scans the arguments and callee.

Other arms register constructs at by-value merge points (`if`/`do`/`let`
whose type is a concrete application) so a binding-less `(none)` arm gets a
by-value clone.

### `emit_abi_register_call`

It is about 2,250 lines, mostly special cases with archived reports behind
them. The skeleton:

1. **Skip** the shapes that must stay on the carrier: a higher-kinded class
   method dispatched through a runtime dict (MB2.5), and a typeclass call
   that re-resolves inside the active spec to a concrete instance
   (`emit_reresolve_disp_type`; it is noted as a carrier call on the
   re-resolved method, or redirected to a by-value instance spec).
2. **Compose bindings** with the active spec (above).
3. **Instantiate** the callee's declared parameter and result types through
   the bindings (`emit_abi_instantiate_type`, scratch from `ctx->type_arena`).
4. **Decide.** `abi_changes` is set when an instantiated parameter or result
   c-names differently from the generic one (`type_c_name` strings differ),
   plus forced cases: a phantom-hidden aggregate, a ground `:heap` sum, a
   float element behind a closure parameter, a colored generic that needs a
   monomorph. `instance_changes` is set when the body dispatches on a type
   variable the bindings fix (`body_has_dispatch_on_app_tyvar`). Neither set:
   the call stays on the carrier.
5. **Intern** (`emit_abi_intern_spec`) and **record**
   (`emit_abi_record_specialized_call`).
6. **Link inner closures.** A generic that returns a closure, or a struct of
   closures, gets per-spec clones of those lambdas too
   (`inner_closure_spec_idx`, `extra_inner_closure_spec_idx`), with suffixed
   env structs so a float layout does not reuse the carrier one.
7. **Recurse.** If a new spec was created, scan the callee's body with that
   spec active. Nested calls then compose their bindings with it, so a whole
   chain of generics specializes from one call site.

### Interning and the clone name

`emit_abi_intern_spec` looks for an existing spec with the same binding,
argument types and result type (and the same bindings, when asked to match
them), and otherwise appends one. The name comes from `emit_abi_clone_name`:

```
<sanitized fn name>__spec__<result C type>_<arg0 C type>_<arg1 C type>...
```

Each C type is sanitized to identifier characters (`append_sanitized_c_token`;
a space or `*` becomes `_`). For `(defn pick [A] [b : bool x : A y : A] : A
...)` called as `(pick true 1.5 2.5)` and `(pick true "a" "b")`, the clones
are:

```c
static double pick__spec__double_bool_double_double(bool, double, double);
static const char * pick__spec__const_char___bool_const_char___const_char__(
        bool, const char *, const char *);
```

while `(pick false 3 4)` stays on the base `pick(bool, int64_t, int64_t)`.
Two genuinely different specs whose C types spell the same name (all handles
collapse to `int64_t`) get a `__h<n>` suffix, in intern order. Specs for
non-higher-kinded instance methods also set `typeclass_inst`, which routes
them through per-instantiation dictionaries.

### Recording and lookup

The call->clone map is three parallel arrays: `specialized_call_exprs`,
`specialized_call_names` and `specialized_call_outer`. The key is the pair
**(call `Expr*`, clone name of the active outer spec)**. One source body is
scanned once per outer spec, so the same call node maps to a different clone
per element type (M5 Finding 7). The outer is `NULL` at top level.

At emission, `emit_call_name` (`emit_core.c`) looks the call up under the
current `ctx->current_abi_specialization->clone_name`. It prefers an exact
match; a cross-spec fallback applies only inside a spec, and only when
`cross_spec_clone_fits_call` agrees. Typeclass re-resolution
(`emit_reresolve_method_call`) runs first when nothing is recorded under the
current outer.

### Which bases survive

A generic's carrier base is emitted by default. `emit_abi_fn_skip_generic`
drops it when nothing calls it through the carrier and one of these holds:

- it is *generic-unsafe*, i.e. a type variable is nested in a compound
  parameter or result type (`(Option A)`; a bare `[x : A]` is carrier-safe,
  so its base is always kept);
- it is a constrained generic whose base would bake a struct-receiver
  instance;
- it performs (`emit_cps_ir_generic_base_is_template`);
- it is an instance method of a dead instance (its dictionary is dropped in
  lockstep by `emit_stmt`'s `EX_INSTANCE_DEF` arm).

The forward-declaration pass applies the same predicate, so a dropped base
has no prototype either.

### Emission

Spec bodies are emitted after the item loop (step 10 above), each with
`current_abi_specialization` set. Inside one:

- `emit_resolve_type` and `emit_type_c_name` resolve type variables through
  the spec's bindings;
- `inline_c_substitute` expands `__TUR_TY_<A>__` and `__TUR_RET__` to the
  concrete C types;
- `atom_var` resolves a generic function value to its clone
  (`emit_fn_value_spec_clone`).

`--emit-abi-trace` prints, once per source call site (not per spec), which
path the scan chose: `concrete-clone`, `dictionary`, `polymorphic-wrapper`
or `carrier`:

```
abi-trace 9:12 pick concrete-clone pick__spec__double_bool_double_double
abi-trace 10:12 pick carrier
```

---

## Value-position control forms (joins)

`if`, `match`, `let`, `letrec` and `do` in value position declare a **merge
temp** before their branches and assign it in each. The temp's C type is the
join decision:

- `emit_if_value` first asks `fn_body_tail_byvalue_carrier_type`: if an arm
  produces a by-value aggregate (a by-value constructor clone), the temp is
  that aggregate and each carrier-producing arm is bridged to it.
- Otherwise `emit_control_result_temp_decl` picks the spelling:
  - a concrete heap pointer;
  - the `int64_t` carrier for a carrier producer;
  - `void *` for a fat fn;
  - else `emit_type_c_name(type)`.

  `control_result_temp_ctype` mirrors those branches for callers that need
  the spelling without declaring.
- Each arm's value is reconciled with the temp:
  - int64<->pointer straddles by `bridge_control_result_int_ptr`;
  - carrier<->aggregate by `emit_carrier_bridge`;
  - a branch that diverges (`return`, `panic`) is emitted as a statement.
- The temp's C type is recorded in the local-variable table. When that record
  was missing, an enclosing `let` unboxed the already-bridged value a second
  time.

There is no single "join coerce" function, and the comment in `emit_expr.c`
that names `emit_join_coerce` refers to code that no longer exists under that
name. A new value-position control form should reuse
`emit_control_result_temp_decl` and record its temp, rather than spell a temp
type of its own.

---

## Inline C

Inline C reaches the emitter in three positions:

| Position | Source shape | Emitted by |
|---|---|---|
| A whole function body | a `defn` whose body is one inline-C block | `emit_fn_def_body`'s `EX_INLINE_C` arm -> `emit_stmt` -> `inline_c_substitute` |
| An expression or statement in a body | an inline-C block among other forms; session ops the elaborator lowers to inline C | `emit_value_dispatch` / `emit_stmt` `EX_INLINE_C` -> `inline_c_substitute` |
| File scope | a top-level inline-C block | the item loop, verbatim into `cprelude`, deduped by whitespace-normalized chunk |

**Parameters are not substituted; they are named.** An inline-C function body
refers to its parameters by their C names, and the emitter makes those names
match: parameters are emitted with `raw_name_for_binding`, so `x` in the body
is the C parameter `x`. A `let` local an inline-C block mentions is
spelled raw too (`ctx->inline_c_raw_locals`, filled by
`emit_inline_c_raw_locals_collect`, which only admits a plain unshadowed C
identifier the block actually mentions). The body's text is spliced as-is and
must contain its own `return`.

**Placeholders** that `inline_c_substitute` does replace:

| Placeholder | Becomes |
|---|---|
| `__TUR_CAP_N__` | the C name of capture `N` (`name_for_binding`) |
| `__TUR_VAL_N__` | a temp holding the value of `val_exprs[N]`, emitted ahead of the text |
| `__TUR_TY_<A>__` | the C type bound to type variable `A` in the active spec, else `int64_t` |
| `__TUR_RET__` | the active spec's result C type, else the function's C return type, else `int64_t` |
| `__TUR_CNAME_<name>__` | the mangled C identifier of `<name>`, so inline C never hardcodes the mangling |

`__TUR_CAP_N__`/`__TUR_VAL_N__` are produced by the elaborator (session
types), not written by hand. `__TUR_TY_` also gates specialization: an
inline-C body is only cloned per type when it uses the template form
(`inline_c_has_ty_template`).

**Around a function body**, `emit_fn_def_body` adds two things:

- a `TUR_REGION_NOTE` for each parameter whose type can be a region node,
  because an opaque body may retain it
  ([region-escape-through-unhooked-stores](../archive/region-escape-through-unhooked-stores.md));
- `#define tur_panic(m) tur_panic_sited(m)` ... `#undef tur_panic` when the
  body calls `tur_panic(`.

It also records the function's C name with `emit_note_inline_c_fn`.

**Include hoisting.** Leading `#include` lines of a body are stripped
(`strip_hoistable_includes` -> `tur_hoist_top_includes_scan`) into a
TU-wide deduped set and emitted once after the preamble, each angle include
wrapped in `#if __has_include(...)`. A missing header is therefore not a
build error, only a `#pragma message` (unless the line carries
`tur:optional`). The elaborator scans file-scope blocks the same way.

---

## Machine-readable comments

The emitter writes a few comments that tools parse. Do not reword them.

| Marker | Written by | Read by |
|---|---|---|
| `/* ==== tur: end of fixed runtime preamble ==== */` | `emit_runtime_preamble` | the JIT's split-runtime swap (`main.c:jit_try_split_preamble`) and pruner (`jit_prune.c:jit_prune_full_source`); `tools/gen-runtime-split.py`; `tests/fconv_lint.py` (skips everything above it) |
| `/* tur:inline-c-begin */` ... `/* tur:inline-c-end */` | `inline_c_emit_block_deduped` around file-scope C | `tests/fconv_lint.py` |
| `/* tur:inline-c-fns: a b c */` (last line) | `emit_note_inline_c_fn` / `emit_write_inline_c_fns` | `tests/fconv_lint.py` |
| `/* tur:optional */` on a hoisted include | the author, carried by `TUR_HOIST_OPTIONAL_TAG` | `tur_emit_hoisted_include` |
| `/* panic-return-signal: ret ctype unknown; ... */` | `emit_panic_signal_return` | humans; it marks a call whose panic is not propagated |

`tests/fconv_lint.py` is the engine behind
`tests/check-emitted-float-conversions.py` (ctest
`tur_emitted_float_conversions`). It compiles the emitted C with clang, walks
the AST for float<->integer value conversions, and skips hand-written inline C
(the two inline-C markers) and the preamble. Any conversion it reports in
emitter-written code is a bug unless spelled `TUR_AS(T, x)`. It runs
`tur emit-c` with the repo root as the working directory, so pass absolute
paths or run it from the root: a relative path that does not resolve there
is treated as "not a program" and the run reports `0 program(s) checked`
with exit status 0.

`tests/run.sh` also ratchets the C compiler's warnings on every fixture build
(`-Wint-conversion`, `-Wincompatible-pointer-types`, `-Wfloat-conversion`,
`-Wfree-nonheap-object`), so an emitter change that mixes up a pointer and an
integer fails as a FAIL, not a warning
([emitted-c-pointer-integer-warnings-unwatched](../archive/emitted-c-pointer-integer-warnings-unwatched.md)).

---

## Emitting a function definition

`emit_fn_def(ctx, file, e)` sets `in_forwarding_wrapper` for `__poly_N`
wrappers and calls `emit_fn_def_body`, which runs these steps:

1. **Skip** a FnDef marked `skip_emission`.
2. **Try CPS.** `emit_cps_ir_try_fn(ctx, file, e)`. On success the function
   has been written as `f__cps` plus its entry wrapper; `^tailcall` is
   checked (`tc_verify_fn`) and the function returns.
3. **Mutual tail-call group.** If the function belongs to a T5 group, the
   first member writes the fused function's prototype, the last writes its
   definition, and this member's body becomes a call into it.
4. **Install context.** The dict-dispatch vectors of a dict-clone, the
   dict-env vectors of a dict-capturing closure, and the C return type
   (`current_fn_ret_ctype`). The return type has a long decision chain:
   instance methods, dict clones, boxed aggregate results, fn-typed returns.
   It must agree with `emit_fn_forward_decls` and `emit_abi_forward_decl`,
   which mirror it.
5. **Redirect `ctx->file`** to a temp buffer, so helpers lifted while
   emitting the body (handler functions, env structs) can be flushed to file
   scope *before* this definition.
6. **Closure env.** For a lifted lambda, emit its env struct unless
   `ctx->env_struct_names` says some site already did. The `EX_CLOSURE`
   construction site (`emit_closure_env_struct_and_glue`) and the CPS twin
   pre-pass register in the same list, and whichever runs first wins.
7. **Signature.** Linkage (`static` unless `main`, an exported function in a
   separate build, or a split-library definition), the C return type, and the
   parameters. A user `main` with no parameters is emitted as
   `int main(int argc, char **argv)` and builds `*args*`.
8. **Prologue.** `#line` under `--debug`; unbox wide by-value parameters;
   Windows binary stdio in `main`.
9. **Body**, by the first arm that applies:

   | Condition | Emission |
   |---|---|
   | stackless catch-unwind splitter claimed it | already written by `gs_*` |
   | T5 group member | `return <group>(idx, params...);` |
   | synthesized carrier wrapper (Prereq 6, M2b) | already written |
   | body diverges on every path | `emit_stmt`, then a dead typed `return` to silence `-Wreturn-type` |
   | inline-C body | region notes, `tur_panic` define, `emit_stmt` |
   | `nil` result, not tail-call eligible | `emit_stmt` |
   | tail-call eligible | `__tur_tailcall:;` label, then `emit_tail` |
   | body type `nil` but a value is expected | `emit_stmt`, then `return 0;` |
   | otherwise | `emit_fn_return_spelling(emit_fat_return_value(...))` |

10. **Restore** context, close the brace, flush pending handler functions,
    append the temp buffer to the real `file`, and, under `--cps-path`, write
    the legacy `f__cps` wrapper.

**Tail position** is the province of `emit_tail` (`emit_fns.c`): `tco_mark`
marks self tail calls, and `emit_tail` turns each into parameter assignment
plus `goto __tur_tailcall;`. A non-self tail call is emitted as
`return f(args);`, and `TUR_MUSTTAIL` is added when `tail_call_musttail_ok`
proves the signatures identical. The design and its refusals are in the
[proper-tail-calls plan](../archive/proper-tail-calls-plan.md).

**Closures and thunks.** A `fn` that captures is lifted by the elaborator to
a file-scope `EX_FN_DEF` with `fd->closure` set. The construction site
(`EX_CLOSURE`) allocates and fills the env. The lifted body reads captures
through `ctx->env_var_name`. Partial-application thunks
(`__papN`) and poly wrappers (`__poly_N`) are ordinary lifted functions with
`synth_kind` set. Fat closures, typed shims and static fat boxes are covered
by the [value representations guide](value-representations-guide.md).

---

## Trace: one tiny program

```turmeric
(defn pick [A] [b : bool x : A y : A] : A
  (if b x y))

(defn square [x : int] : int
  (* x x))

(defn main [] : int
  (println (square 7))
  (println (pick true 1.5 2.5))
  (println (pick false 3 4))
  0)
```

`tur emit-c` writes about 10,800 lines, ~5,700 of them preamble. The program's
own text, trimmed:

**Scan (step 4).** `(pick true 1.5 2.5)` instantiates `A := float`;
`double` differs from the base's `int64_t`, so the scan interns
`pick__spec__double_bool_double_double` and records it under outer `NULL`.
`(pick false 3 4)` instantiates `A := int`; the C types are unchanged, so it
is a carrier call. The stdlib's own calls intern further clones (here
`vec_new__spec__tur_adt_Vec__int__` and two others).

**Forward declarations (step 8).** `fwd_decls` gets `pick`, `square` and,
after every ordinary function, the spec prototype
`static double pick__spec__double_bool_double_double(bool, double, double);`.
Each is recorded in the signature table as it is printed.

**Item loop (step 9)**, into `file` in source order. `main` is a user `main`,
so it gets `(argc, argv)` and the `*args*` prologue:

```c
static int64_t pick(bool b, int64_t x, int64_t y) {
        int64_t __t202;
        if (b) {
            __t202 = x;
        } else {
            __t202 = y;
        }
        return __t202;
}

static int64_t square(int64_t x) {
        return (x) * (x);
}

int main(int argc, char **argv) {
        __tur_static_init();
        /* ... Windows stdio, *args* construction ... */
        int64_t __ps_203 = (square(INT64_C(7)));
        /* panic-return-signal: ret ctype unknown; no propagation here */
        printf("%lld\n", (long long)(__ps_203));
        double __ps_204 = (pick__spec__double_bool_double_double(true, 1.5, 2.5));
        /* panic-return-signal: ret ctype unknown; no propagation here */
        printf("%g\n", (double)(__ps_204));
        int64_t __ps_205 = (pick(false, INT64_C(3), INT64_C(4)));
        /* panic-return-signal: ret ctype unknown; no propagation here */
        printf("%lld\n", (long long)(__ps_205));
        int64_t __t206;
        __t206 = INT64_C(0);
        return (int)__t206;
}
```

Things to notice:

- Each call's temp has a named type (`int64_t`, `double`) taken from the
  signature table, not `__auto_type`.
- `main` has no known return C type, so its calls get the comment instead of
  `if (tur_panicking) return ...;`.
- The `0` tail goes through a merge temp; `(int)` narrows it for C's `main`.

**Spec bodies (step 10)**, after `main`:

```c
static double pick__spec__double_bool_double_double(bool b, double x, double y) {
        double __t208;
        if (b) {
            __t208 = x;
        } else {
            __t208 = y;
        }
        return __t208;
}
```

**Trailers (bands 14-19):** defer thunks, `__tur_module_def_init`, the
fat-box definitions, `__tur_static_init` with its constructor, and the
`tur:inline-c-fns` line listing ~190 stdlib inline-C functions. Each comes
from the auto-loaded stdlib, not from this program.

---

## Adding a new `Expr` kind or emit arm

`-Wall` includes `-Wswitch`, and `tur_core` builds with `-Werror` on every
platform but Windows. A `switch` over `ExprKind` **with no `default:`** must
therefore name every kind, and adding an enumerator breaks the build until
each such switch grows an arm. Today those are:

| Switch | File |
|---|---|
| `emit_value_dispatch` | `src/compiler/emit_expr.c` |
| `emit_stmt` | `src/compiler/emit_stmt.c` |
| `expr_print` | `src/compiler/expr.c` |
| `borrow_check_expr_recursive` | `src/passes/borrow_check.c` |
| `visit` (SRFI pruning) | `src/passes/srfi_prune.c` |

Every other walker has a `default:` and will **silently** treat the new kind
as a leaf or skip it. Those are the ones that bite. For the emitter:

1. **`expr.h`**: add the enumerator and its `as.` payload.
2. **`emit_value_dispatch`**: return a malloc'd expression; append statements
   to `body` at `ctx->indent`. If the value is a call that can panic, return
   the call text and let `emit_value` hoist it. Do not write your own panic
   check unless you emit the call as a statement.
3. **`emit_stmt`**: emit for effect, or delegate to the inner expression.
   Never drop a child that has effects.
4. **`emit_abi_scan_expr`** (`emit_module.c`, has a `default:`): descend into
   every child that can contain a call. A child the scan never visits never
   gets its clones, and the call emits the base generic name, which fails at
   link (`undefined reference to ...`) or in cc. The missing `EX_SET` and
   `EX_LETREC` arms were exactly this.
5. **`cps_visit_children`** (`src/passes/cps.c`, has a `default:`): list every
   evaluated operand. Coloring and the control-op predicates walk through it;
   a missing arm hides a `perform` from coloring.
6. **The CPS translator** (`src/passes/cps_ir.c`): `safe_to_delegate`
   returns false for an unknown kind, so a colored function containing it
   gets no `CT_LETRAW` for it and may be evicted. Add it to
   `safe_to_delegate` if it is control-free.
7. **The interpreter** (`src/turi/eval.c`): `tools/check_turi_parity.py`,
   run at the top of `tests/run.sh`, fails when a kind has no `case` there
   and no entry in `docs/artifacts/turi-carve-out.txt`.
8. **Analyses with a `default:`** that may need the kind: the escape walks in
   `emit_core.c` (`closure_binding_escapes`, `sum_box_binding_escapes`, ...),
   `expr_is_divergent` / `expr_contains_return_or_throw`, `collect_defined`,
   the free-variable collection, and the tail-call markers in `emit_fns.c`.
   Grep for an existing kind with a similar shape (`EX_RC_CLONE` for one
   child, `EX_BUILTIN` for a list) to find them:

   ```sh
   grep -n "case EX_RC_CLONE" src/compiler/*.c src/passes/*.c
   ```

9. **Region hooks**: if the arm stores a caller's word into memory that can
   outlive a bracket, call `emit_region_note_lvalue` after the store
   (CLAUDE.md, "Region Store Hooks").
10. **Snapshots and fixtures**: add a fixture that exercises the arm in value
    *and* statement position, inside a generic called at a non-`int` type,
    and (if it can appear near a `perform`) inside a colored function; check
    the last with `TUR_TRACE_EVICT=1`.

For a new **helper that is emitted once per TU** (a shim, a typedef, a lazy
runtime), follow the dedup-registry pattern: a name list in `EmitCtx`, an
`ensure_*` function that emits into `ctx->thunk_typedefs` on first use and
records its spelling, and a free in the cleanup block. Emitting into
`ctx->file` from inside a body puts the helper inside the function being
written.

---

## Fixture snapshots

About 160 fixtures carry an `expected.c` codegen snapshot. `tests/run.sh`
runs `tur [flags] emit-c <input>` (the `flags` file supplies the flags) and
fails the fixture with `codegen mismatch` and a unified diff when the output
differs. The snapshot includes the whole preamble, so a preamble change moves
every snapshot.

A codegen mismatch means the emitter's output moved. It does not by itself
mean a bug, and per CLAUDE.md a red fixture never blocks a commit. When your
change settles, regenerate the snapshots in the same PR as the change:

```sh
TUR=./build/tur
for dir in tests/fixtures/*/; do
  if [ -f "$dir/expected.c" ]; then
    input="$dir/input.tur"
    [ -f "$input" ] || input="$dir/$(basename $dir).tur"
    [ -f "$input" ] && "$TUR" emit-c "$input" > "$dir/expected.c" 2>/dev/null
  fi
done
```

This loop does not read a fixture's `flags` file. No snapshot fixture has one
today; if you add one, regenerate that fixture by hand with its flags.

To review a large regeneration, classify the diff:

```sh
git diff -- 'tests/fixtures/**/expected.c' | python3 tools/snapshot-diff-summary.py
```

It sorts each file into `preamble-only`, `mangling-only` or `body` and asks
you to read the `body` ones. `tools/audit-fixture-snapshots.py` lists
snapshots that assert nothing a `expected.stdout` does not already.

---

## Debugging the emitter

| Tool | Shows |
|---|---|
| `tur emit-c f.tur` | the TU. Search for `__spec__` (clones), `__ps_` (hoisted calls), `__auto_type` (an unnamed hoist type) |
| `tur --emit-abi-trace emit-c f.tur` | per source call site, the ABI path the scan chose; in a Release build also enables the repr shadow |
| `tur --dump-mono-specs ...` | the van Laarhoven lens mono-spec registry |
| `tur --debug ...` | `#line` directives mapping C back to `.tur` |
| `TUR_STACK_MB=<n>` | the emission stack size (default 256) |
| `TUR_DEBUG_STACK_GUARD=1` | the stack backstop's depth and headroom |
| `TUR_ABI_NO_ROUTE_ICE=1` | downgrade the "unrouted type variable" ICE (`emit_abi_assert_routed_concrete`) to a warning |
| `TUR_NO_OWN_DISCHARGE=1` | disable the consuming-callee discharge, to bisect an ownership change |
| `TUR_TRACE_EVICT=1`, `TUR_CPS_CENSUS=1` | why a function left (or never entered) the CPS backend |
| `python3 tests/check-emitted-float-conversions.py f.tur` | float<->int value conversions in emitted code |

[diagnosing-the-compiler-guide](diagnosing-the-compiler-guide.md) has the
general triage flow.

---

## Reading the archive

- [emit-value-dispatch-unbounded-recursion](../archive/emit-value-dispatch-unbounded-recursion.md)
  and [emit-depth-guard-loses-race-with-asan-stack](../archive/emit-depth-guard-loses-race-with-asan-stack.md):
  why emission runs on a sized stack.
- [jit-engine-plan](../archive/jit-engine-plan.md): S1 (named hoist types, the
  signature table), S1b (explicit static init), S2 (the fixed preamble and
  its hash).
- [history/end-to-end-monomorphization-plan](../archive/history/end-to-end-monomorphization-plan.md),
  [m4-typeclass-per-method-abi-plan](../archive/m4-typeclass-per-method-abi-plan.md)
  and [history/carrier-crossing-recovery-routing-plan](../archive/history/carrier-crossing-recovery-routing-plan.md):
  the specialization scan, by-value clones, and the recovery chokepoints.
- [repr-decision-function-plan](../archive/repr-decision-function-plan.md):
  `repr_of` and the repr shadow.
- [proper-tail-calls-plan](../archive/proper-tail-calls-plan.md): `emit_tail`,
  the un-hoisted tail call, groups and bouncing.
- [c-name-accessors-share-static-buffers](../archive/c-name-accessors-share-static-buffers.md):
  why name accessors return owned strings.
- [toplevel-def-initializers-run-before-toplevel-expressions](../archive/toplevel-def-initializers-run-before-toplevel-expressions.md)
  and [history/top-level-def-init-dropped](../archive/history/top-level-def-init-dropped.md):
  where def initializers run.
- [r7rs-programs-compile-slowly](../archive/r7rs-programs-compile-slowly.md):
  the split build (`emit_split.c`, `g_emit_split`).

---

## Maintenance

Update this guide in the same PR when you:

- add, remove or reorder an output band or a phase of `emit_program_inner`;
- add an `EmitCtx` field group, a file-scope side table, or a reset;
- change the call hoist, the panic-signal return, or the `call_ret_note`
  protocol;
- change how a clone is named, keyed or looked up;
- add or reword a machine-readable comment, or a tool that parses one;
- add an exhaustive `ExprKind` switch, or a walker a new kind must join.

The C excerpts are from `tur emit-c` and were trimmed; refresh them when the
shape they show changes. Cite functions, not line numbers.
