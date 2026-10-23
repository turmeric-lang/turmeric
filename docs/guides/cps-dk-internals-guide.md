---
title: CPS and the DK Continuation Machine
category: Compiler Internals
description: How the compiled path lowers effects and delimited control -- the coloring analysis, the CT-IR, the emitter's DK-threading ABI, the DK heap-continuation machine, who frees what, and the tools for debugging it
---

# CPS and the DK Continuation Machine (internals)

This guide is for contributors working on how the compiled path (`tur build`,
`tur run`) lowers `perform`/`handle`/`resume`, `reset`/`shift`/`shift0`,
`await`, `call/cc`/`escape`, and the cloneable and serializable continuation
forms. It describes the code as it stands; the design history is in the
archived plans listed under [Reading the archive](#reading-the-archive).

The interpreter (`turi`) has its own CEK machine and is not covered here; see
the [turi parity guide](turi-parity-guide.md).

> **Trust the code over the header comments.** Many file and function header
> comments in this area predate the July 2026 consolidation. They still say
> "dump-only", "not yet wired into codegen", "falls back to the direct
> emitter", or cite the deleted `emit_cps.c` as the reference. Where a comment
> and this guide disagree, read the code. If the code disagrees with this
> guide, fix the guide.

---

## The short version

1. **Coloring.** A whole-program analysis marks every function that can
   dynamically reach a control operator as *colored*
   (`src/passes/cps.c:cps_color_program`).
2. **CT-IR.** Each colored function's body is translated to a small
   A-normal-form IR with explicit continuations
   (`src/passes/cps_ir.c:cps_ir_translate_fn`).
3. **Classification.** The emitter decides which colored functions it can
   lower. The ones it can lower make up the set `S`
   (`src/compiler/emit_cps_ir.c:ensure_S`). A colored function outside `S` is
   emitted as ordinary direct-style C. A `perform` left in it fails the build,
   and a `reset`/`shift` left in it currently miscompiles silently (see
   [Stage 3](#stage-3-classification----which-colored-functions-get-the-cps-abi)).
4. **Emission.** A function in `S` becomes `f__cps(params..., DK *__kont)` plus
   a direct-style entry wrapper `f(params...)` that seeds a root continuation.
   The continuation is a **heap linked list of `DK` nodes**:
   - `reset` and `handle` push marker nodes onto it;
   - `shift` and `perform` slice it up to the nearest marker;
   - `resume` copies the slice or splices it back in.
5. **Runtime.** The DK machine is C text written into every program's preamble
   (`src/compiler/emit_dk_runtime.c`). Each entry wrapper installs a `setjmp`
   landing, the *driver*, so a tail-resuming handler runs in constant C stack.
6. **Memory.** Nodes and environment structs are registered on a per-thread
   *reap list*. It is drained when the outermost entry returns, with early
   hand-backs for nodes that provably ran exactly once.

There is exactly **one** lowering for effects and delimited control on the
compiled path. The old direct-style emitter (`emit_cps.c`) was deleted on
2026-07-12. The fiber effect runtime (`tur_effect_perform`,
`global_effect_handler_chain`, ...) was deleted on 2026-07-19. Fibers still
exist, but only as stacks for threads and async I/O (see
[Async, fibers and threads](#async-fibers-and-threads)).

---

## Source map

| File | Role |
|---|---|
| `src/passes/cps.c`, `cps.h` | Coloring (`cps_color_program`), the control-op predicates, the legacy `cps_transform` |
| `src/passes/cps_ir.c`, `cps_ir.h` | The CT-IR data model and translator (`cps_ir_translate_fn`), `--dump-cps` |
| `src/compiler/emit_cps_ir.c`, `.h` | Classification (`ensure_S`) and the CT-IR-to-C backend (`emit_cps_ir_try_fn`) |
| `src/compiler/emit_dk_runtime.c`, `.h` | String emitters for the runtime: the DK machine, the cloneable<->DK bridge, the serial marshaler, the call/cc escape runtime, the `setjmp` selection |
| `src/compiler/emit_module.c` | Prelude gating; `__dk_enter0` and `__dk_entry_leave`; the per-fiber state swap; `emit_rt_split_source` |
| `src/compiler/emit_effects.c` | Handler literals and handler tables (`tur_handler_entry_t`), `cont?`, the cloneable reset fallback, and the direct emitter's `reset`/`shift`/`handle` for evicted functions. `emit_effects_handle` past its `Unsafe`-marker short-circuit is leftover fiber-era code that no longer compiles ([report](https://github.com/turmeric-lang/turmeric/blob/main/docs/reported/evicted-handle-reaches-deleted-fiber-emitter.md)) |
| `src/compiler/emit_expr.c` | The poly-wrap `fn_cps` twin; the "this effect operation has no lowering here" error |
| `src/runtime/generated/tur_rt_split*.{c,h}` | A committed snapshot of the preamble runtime (see [The split runtime](#the-split-runtime)) |

These look relevant but are **not** on the emitted path:

| File | Status |
|---|---|
| `src/runtime/cps_prompt.c`, `.h` | The original reference DK machine (13-field `struct DK`). It is linked only into the `tur_cps_prompt_unit` test. The emitted machine has diverged from it substantially: about 40 fields, the driver, the reap list, handler groups, in-place resume. Read it for the core idea, not for current behavior. |
| `src/runtime/cps_rt.c`, `.h` | The CPS4 `TurKont` trampoline. Nothing outside `tests/cps_rt_unit.c` uses it, though it is still compiled into `tur_core`. |
| `tur_cont` in `src/runtime/runtime.h` | The pre-CPS fiber-snapshot continuation. `runtime.c` is not built by any target. The emitted preamble's `tur_cont` is an unrelated struct with the same name (a node of the catch-unwind segment trampoline). |

---

## Pipeline position

The core passes are listed in `src/driver/pipeline.c:core_passes[]` and run by
`run_core_passes`. The CPS-related ones are:

| Pass | What it actually does |
|---|---|
| `PASS_EFFECT_ENV` | Builds the `EffectEnv` (registers builtin `Unsafe` and the capabilities). The tree is untouched; effects are **not** rewritten to `shift`/`reset` anywhere before the DK backend. (It was `PASS_EFFECT_LOWER`, and called an identity `effect_lower` walk, until 2026-10-21.) |
| `PASS_EFFECT_ROW_INFER` | `effect_check_pass`, the effect-row checker. Its "stub slot" comment is stale. |
| `PASS_CPS` | Runs `--dump-cps-coloring` (stdout half) and `--dump-cps` on the pre-transform tree, then `cps_transform`, then `--dump-clone-plan` and the stderr half of `--dump-cps-coloring`. |

`cps_transform` is a near-identity tree copy plus **legacy** metadata
(`may_capture`, `is_cps`, cloneable `live_captures`). The only readers of that
metadata are the opt-in `--cps-path` wrappers and the dumps. The returned tree
does replace `ctx->prog`, and its `source_fn_def` retargeting is relied on, so
the pass cannot simply be deleted.

**The coloring and classification the backend uses run at emission time, not
in the pass list.** `ensure_S` runs lazily, on the first emitter query that
needs it. That is usually the first function body's `emit_cps_ir_try_fn`:
bodies are emitted before the preamble text is generated, so the preamble gate
`emit_cps_ir_program_has_emittable` comes later and hits the cache. `ensure_S`
runs `cps_color_program` on the tree being emitted and caches the result,
keyed on the program and `EmitCtx` addresses. `emit_cps_ir_forget` clears the
cache at the start and end of every top-level emission, because `tur lsp` and
`tur mcp` compile many programs in one process.

Effects therefore reach the CT-IR as `EX_PERFORM`/`EX_HANDLE`/`EX_RESUME`.
`cps_ir.c` builds `CT_PERFORM`/`CT_HANDLE`/`CT_RESUME` from them directly.

---

## Stage 1: coloring

`cps_color_program` (`src/passes/cps.c`) builds a call graph over top-level
`EX_FN_DEF`s and `defmodule` members. Lifted lambdas, partial-application
thunks and instance methods count, because the elaborator splices them in as
file-scope `EX_FN_DEF`s.

**Seeds.** `cps_directly_uses_control` (= `cps_control_walk`, which does not
descend into nested functions) seeds on:

- `reset`, `shift`, `shift0`;
- `cloneable-reset`/`-shift` and `serial-reset`/`-shift`;
- `perform`, `handle`, `resume`, `discontinue`;
- `call/cc` and `escape` (both are `EX_CALLCC`);
- `await`;
- `with-handler` and `compose-handlers`.

The same walk is exported as `cps_expr_uses_control`. The elaborator uses it
to hoist a control operand into a `let` before an `any` widen.

**Edges.** `cps_collect_calls` resolves a call's callee to a node, by binding
pointer first and then by C symbol. A lambda applied on the spot resolves to
its lifted node, and a typeclass method resolved to an instance becomes an edge
to that `__inst_...` function. A call it cannot resolve sets `has_indirect`.
That covers:

- a call through a fn-valued parameter;
- a call through a `let`-bound closure;
- a call through a capability field;
- `EX_DYN_CALL`;
- dictionary dispatch.

Exempt (never indirect): constructor calls (`call_.ctor`), inline-C bodies and
`extern-c` functions.

**Fixed point.**

1. `colored = seeds`.
2. Every `has_indirect` node is colored.
3. `cps_color_fixpoint` runs three steps:
   - **Backward propagation:** a caller of a colored function is colored.
   - **Force-coloring:** `cps_force_color_eff_fnval_args` colors a lambda
     passed to a `TY_FN` parameter, or stored in a constructor field, whose
     type carries a non-empty effect row.
   - **Backward propagation** again.
4. Two more fixpoints set `FnDef.may_await` (with `Binding.may_hold_awaiter` on
   parameters) and `FnDef.async_reachable`. The latter feeds TUR-W0044.

The results land in `FnDef.cps_colored`. The call is idempotent.

**Over-coloring (performance-audit item R-1).** The indirect rule has no
effect-row or purity test, so any function that calls through a fn value is
colored. So is every caller of such a function. Over the corpus this rule
alone accounts for most colored functions; see the
[WP2 census](https://github.com/turmeric-lang/turmeric/blob/main/docs/artifacts/perf-audit-2026-10/wp2-runtime-hot-paths.md#r-1-census----how-much-is-cps-colored-and-why).
Only the census instrument has landed; the narrowing has not. What a colored
function actually pays depends on Stage 3 (it may be evicted to direct
emission) and on how much of its body the translator delegates as `CT_LETRAW`.
The memory usage guide measures the cost of
[calls through function values](memory-usage-guide.md#calls-through-function-values-and-effects).

**Tools.**

- `TUR_CPS_CENSUS=1 tur emit-c f.tur` prints one stderr line per function:

  ```
  cps-census <colored|uncolored> <cause> <ind|-> <module|-> <file> <name>
  ```

  `cause` is `control` (a seed), `callee` (colored through a resolved callee
  or force-coloring) or `indirect` (colored only by the indirect rule). The
  census re-runs the fixpoint with `has_indirect` ignored to tell the last two
  apart. It does not change codegen.
- `--dump-cps-coloring` prints **two different dumps**:
  - stdout `cps-coloring: <name> COLORED|uncolored`, from `cps_colored`;
  - stderr `<name>: colored|uncolored` between `=== cps coloring ===` markers,
    from the **legacy** `may_capture`.

  They can disagree. Fixtures such as `cps-effect-coloring` match the stderr
  (legacy) half.

---

## Stage 2: the CT-IR

`src/passes/cps_ir.h` defines the IR:

- **Atoms (`CAtom`)** are trivial values: a source variable (`CA_VAR`), a
  CPS-minted temporary (`CA_CVAR`, named `__tN`), or a literal. Every atom
  carries its `TypeKind` and full `Type`.
- **Continuations (`CKont`)**:
  - `KK_RET`, the function's own `k`;
  - `KK_VAR`, a join point bound by `CT_LETCONT`;
  - `KK_PROMPT`, "the value delivered to the nearest prompt", used by reset,
    shift, handle and handler-case bodies;
  - `KK_LOOP`, translator-internal; its tails become `CT_CONTINUE`.
- **Terms (`CTerm`)**:

| Kind | Meaning |
|---|---|
| `CT_APPCONT` | `(k v)`: deliver an atom to a continuation |
| `CT_LETVAL` / `CT_LETPRIM` | Bind an atom / a builtin operator applied to atoms |
| `CT_LETCALL` | `x = f(atoms)`: a direct call to an uncolored function |
| `CT_TAILCALL` | `f(atoms, k)`: a call that threads the continuation. The callee is a colored function, a thread parameter or local (`via_registry`), or a rank-2 fn parameter's `fn_cps` slot (`via_fncps`) |
| `CT_LETCONT` | `letcont j(x) = jbody in body`: a join point. A non-tail colored call is always `LETCONT j(x)=rest in TAILCALL(..., j)` |
| `CT_IF`, `CT_MATCH` | Branching. `CT_MATCH` has an ADT tag-dispatch mode and a "direct" mode where the direct emitter dispatches and the arm bodies come back through `EmitCtx.match_tail` |
| `CT_LOOP` / `CT_CONTINUE` | A `while` with an interior control operator, and its back-edge |
| `CT_RESET`, `CT_SHIFT` | Delimited control. `CT_SHIFT` is the abortive base shift (and `shift0`) |
| `CT_HANDLE`, `CT_PERFORM`, `CT_RESUME` | Effects. `CT_HANDLE` carries `CHandleCase`s, a `shallow` flag, and a `dyn` table for first-class handler values |
| `CT_AWAIT` | `x = await(fut)` |
| `CT_CALLCC` | A local `setjmp` escape landing (does not thread the chain) |
| `CT_CLONEABLE` | A cloneable or serial reset whose context was decomposed into frames (`CloneFrame`, `CloneLet`) |
| `CT_LETRAW` | `x = <Expr>`: a control-free subexpression handed back to the direct emitter's `emit_value` |
| `CT_UNSUPPORTED` | A form outside the subset, with a reason string |

### Translation

`cps_ir_translate_fn` drives two mutually recursive workers:

- `cps_tail(e, kont)` delivers `e`'s value to `kont`.
- `cps_bind(e, x, rest)` binds it to `x` and continues with `rest`.

Non-atomic operands are named `__tN` through a pending list (`Pending`,
grown by `pend_push`). It is folded leftmost-outermost, so the IR's
evaluation order is the source's left-to-right order. It held 32 slots until
2026-10-17, past which `atomize` dropped bindings silently
([report](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/cps-ir-pending-list-overflow-drops-bindings.md)).

**`CT_LETRAW` is the most important design choice in the translator.** Any
subexpression that passes `safe_to_delegate`, a closed list of control-free
forms that make no colored call, is handed back to the direct emitter whole:
uncolored calls, struct operations, rc operations, closures, `match`, `while`,
`set!`, inline-C. The direct emitter already gets ownership right there (rc
control blocks, drop glue, scoped closure-env frees, monomorph names). An
owning value then stays a C local and never has to cross a DK slot. If the
whole body is delegatable (`whole_body_delegatable`), the function becomes one
`LETRAW` followed by `(k v)`. That is how most functions colored only through
the indirect rule end up.

Calls go down a decision ladder in `cps_tail`/`cps_bind` (`EX_CALL`):

1. A threadable fn-value call becomes a `CT_TAILCALL`, `via_registry` or
   `via_fncps` (see [Fn values and callbacks](#fn-values-and-callbacks)).
2. An effectful fn-value call that cannot thread becomes `CT_UNSUPPORTED
   "effectful fn-value call (E2 pending)"`.
3. A delegatable uncolored call becomes a `CT_LETRAW`.
4. Anything else is atomized:
   - a colored callee becomes a `CT_TAILCALL` (wrapped in a `CT_LETCONT` in
     bind position);
   - an uncolored callee becomes a `CT_LETCALL`.

`while` loops with an interior control operator (`build_loop`) lower to a
helper that takes the loop's carried `^mut` state as parameters. A variable
that is not set exactly once per iteration, unconditionally, becomes a
by-reference cell instead. Nested performing loops are still refused, and so
is a `return` inside such a loop when code follows it: the helper's own
continuation is the code after the loop, so the return would act as a `break`
([cps-loop-return-acts-as-break](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/cps-loop-return-acts-as-break.md)).
The user-facing limits are listed in the effects guide's
[Performs inside loops and conditionals](effects-system-guide.md#performs-inside-loops-and-conditionals).
`TUR_TRACE_CORE=1` prints `[LOOP-CARRIED]` and `[LOOP-REJECT]` lines for this
step, and `[CTX-REJECT]` lines for cloneable and serial contexts that cannot
be decomposed.

### `--dump-cps`

`tur --dump-cps check f.tur` prints the CT-IR of every colored top-level
function, stdlib instance methods included. For this program:

```turmeric
(defeffect Ask [] :int)

(defn use-ask [] : int
  (+ 1 (:: (perform (Ask)) :int)))

(defn run [] : int
  (handle (use-ask)
    (Ask [] k) (resume k 41)))

(defn main [] : int
  (println (run))
  0)
```

(`tests/fixtures/cps-backend-ascribe-only-control`), the user functions print
as:

```
cps-fn use-ask [] k:cont<int> entry
  let __t0 = perform Ask()
  let __t1 = (+ 1 __t0)
  (k __t1)
cps-end
cps-fn run [] k:cont<int> internal
  handle __t0 = {
    tailcall use-ask(<prompt>)  ; cps->cps
  }
  with Ask() k ->
    let __t1 = resume k 41
    (<prompt> __t1)
  in
  (k __t0)
cps-end
cps-fn main [] k:cont<int> entry
  letcont j2147483650(__t1) =
    let __t0 = (println __t1)
    (k 0)
  in
  tailcall run(j2147483650)  ; cps->cps
cps-end
```

Things to know when reading it:

- Join ids are `0x80000000 + n`, so `j2147483650` rather than `j2`.
- Every `CT_LETRAW` prints as `let __tN = <owning-op ?>  ; direct-emitted`,
  whatever it delegates.
- The dump runs before emission, so the thread-parameter sets are still empty.
  A call that the emitter would thread through the registry shows up here as
  `<unsupported: effectful fn-value call (E2 pending)>`. The dump is the
  translator's view, not exactly what gets emitted.

---

## Stage 3: classification -- which colored functions get the CPS ABI

`ensure_S` in `emit_cps_ir.c` decides the set `S` of functions emitted as
`f__cps`. It runs in four steps:

1. **Color the program and work out threadability.** Which fn-valued
   parameters and locals can carry the caller's chain is a fixpoint of up to
   16 rounds (see [Fn values and callbacks](#fn-values-and-callbacks)).
2. **Pick candidates.** Every colored function is a candidate unless it:
   - is exported (instance methods and may-await functions excepted);
   - is a non-"d2b" `main` (`fn_is_d2b_main`: zero parameters, and a body
     with a control operator);
   - fails `fn_sig_ok` (its signature has no CPS ABI);
   - is an unthreaded fn value through which an effect escapes.
3. **Check the body.** Each candidate is translated, and `term_core_ok` checks
   the structure. A residual `CT_UNSUPPORTED` disqualifies it.
4. **Run a taint fixpoint over effects.** Its rules:
   - drop a function that would need an unemittable heap join (Rule A);
   - drop a function whose outward serial receiver is not in `S` (Rule D);
   - drop a function that touches a tainted effect (Rule B);
   - taint an effect whose handler-to-performer path runs through a function
     outside `S` (Rule C).

   This keeps a performer and its handler on the same side. A function
   outside `S` that calls one in `S` goes through the callee's plain-name
   entry wrapper, which seeds a **fresh** root chain. A perform inside that
   callee could then never see a handler installed outside it.

A colored function outside `S` is **evicted**: `emit_fns.c:emit_fn_def_body`
falls through to the ordinary direct emitter. That is harmless for a function
that only calls through fn values. If the evicted body itself contains a
`perform` or a handler-value `with-handler`, `emit_value` raises a located
error:

```
this effect operation has no lowering here: the enclosing function left
the CPS backend's supported subset, and the direct emitter cannot lower
`perform`. Run with TUR_TRACE_EVICT=1 ...
```

`TUR_TRACE_EVICT=1` prints one line per evicted colored function:

```
[EVICT] <category> eff=<0|1> <name> <why>
```

along with `[E2-COLOR]` threadability lines. The categories are:

- **`SIG-*`**: permanent signature or ABI routing that no body change can fix.
  - `SIG-EXPORT`
  - `SIG-MAIN`
  - `SIG-REJECT` (the signature has no CPS ABI; common and harmless for
    control-free stdlib functions)
  - `SIG-INLINE-C`
  - `SIG-TAINT`
- **`BODY-*`**: a fixable body shape.
  - `BODY-UNSUPPORTED` (with the `CT_UNSUPPORTED` reason)
  - `BODY-STRUCT-CORE`
  - `BODY-STRUCT-JOIN`
  - `BODY-TAINT`

`eff=1` marks the evictions that matter. (The archived finish plan describes
a `BODY-*` build error. That check was later deleted as unreachable; the
`emit_value` error above is what fires instead.)

That error guards `perform` and `with-handler`. An evicted `handle` goes
through `emit_effects_handle`: one whose every clause is unreachable
(TUR-W0033, recorded as `HandleExpr.no_clause_reachable`) or the `(unsafe ...)`
marker emits its body in place, since nothing under it suspends. Any other
reports each `perform` under it with the error above, or, with none, a located
error at the handle. Its fiber-era body is gone
([evicted-handle-reaches-deleted-fiber-emitter](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/evicted-handle-reaches-deleted-fiber-emitter.md)).

**`reset`/`shift`** in an evicted function lowers onto a `setjmp` escape
landing (`emit_effects_reset`). A base `shift` stores its receiver's value on
the `tur_cur_shift_reset` stack and longjmps to the innermost reset, so the
abort is the same as on the backend. It used to evaluate the body and carry
on with the wrong value
([evicted-reset-shift-degrades-to-body-eval](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/evicted-reset-shift-degrades-to-body-eval.md)).
The value rides an int64 slot, so a reset whose result is not a scalar gets a
located error instead. `reset-shift-in-evicted-function` pins it.

---

## Stage 4: emitting C

### The ABI

A function in `S` is emitted as a body plus an entry wrapper:

```c
static int64_t add_hyask__cps(int64_t x, DK *__kont);   /* body */
__attribute__((unused)) static int64_t add_hyask(int64_t x);  /* entry */
```

- **Parameters keep their real C types** (`emit_param_ctype`): scalars,
  pointers, a `tur_poly_fn_t` for a rank-2 fn, a fn-pointer typedef, or a
  by-value aggregate when it is owning-free.
- **The return is always one `int64_t` word** (`slot_store`/`slot_load`):
  - Tier A, ints, pointers and cstrs: a plain cast.
  - Tier B, floats: a bit-reinterpret through a union, never a numeric
    conversion.
  - Tier C, owning-free by-value aggregates: a `malloc`'d box registered on
    the reap list.

  This word is the "slot" every DK frame receives its value in.
- **Delivering a value** to the continuation is `return dk_run(__kont, slot)`.
  The body does not hand its value back to its caller. It runs the rest of the
  chain and returns whatever that produces.

For `add-ask` from the [worked example](#worked-example-perform-and-handle)
below, the entry wrapper is:

```c
__attribute__((unused)) static int64_t add_hyask(int64_t x) {
    __dk_entry_depth++;
    size_t __dk_reap_mark = __dk_reap_n;
    DK *__root = dk_prompt(DK_ROOT_TAG, dk_done());
    int64_t __r;
    tur_jmp_buf __dkjb; tur_jmp_buf *__dksave = g_dk_driver; g_dk_driver = &__dkjb;
    DKPendPerform *__dkpp = g_dk_pend_perform;
    if (TUR_SETJMP(__dkjb) == 0) { __r = add_hyask__cps(x, __root); }
    else { g_dk_pend_perform = __dkpp; __r = __dk_drive_after(); }
    g_dk_driver = __dksave;
    int64_t __ret = (int64_t)(__r);
    __dk_entry_leave(__root, __dk_reap_mark);
    return __ret;
}
```

Step by step:

1. Seed a root chain, `prompt(ROOT) -> done`.
2. Install the trampoline landing (`g_dk_driver`). A tail resume `longjmp`s
   back here, and `__dk_drive_after` keeps running the chain it was handed.
3. Call the body.
4. On exit, `__dk_entry_leave` frees the root and, at depth 0, drains the reap
   list.

Zero-parameter functions share one out-of-line copy of this sequence,
`__dk_enter0` (emitted in `emit_module.c`). A colored `main` gets the same
sequence inline in `int main(int argc, char **argv)`.

**Crossing between the two worlds:**

- **Uncolored code calling `f`** calls the entry wrapper. Each such call starts
  a fresh chain, so effects never cross it.
- **A `__cps` body calling an uncolored (or evicted) function** makes an
  ordinary C call (`CT_LETCALL`, or a `CT_LETRAW`), checks `tur_panicking`, and
  continues.
- **`__cps` calling `__cps`** in tail position is `return g__cps(args,
  __kont)`. Once the C stack has grown more than `TUR_DK_BOUNCE_DEPTH` (64 KiB)
  below the landing, the call is instead packed into a resume frame and yielded
  to the driver (`__dk_tail_deep`/`__dk_bounce`). That keeps unoptimized
  builds, where the C compiler does not emit sibling calls, from overflowing
  the stack.

### Lifted helpers

Every continuation that has to live on the heap becomes a separate C function,
named after the enclosing function plus a suffix:

| Suffix | What it is | C signature |
|---|---|---|
| `_k<N>` | the continuation after a `reset` | `DKResumeFrame` |
| `_s<N>` | a `shift` body (the receiver applied to the value) | `(intptr_t env, DK *subk)` |
| `_hk<N>` | the continuation after a `handle` | `DKResumeFrame` |
| `_hc<N>_<i>` | handler case `i` | `(intptr_t env, intptr_t arg, DK *subk)` |
| `_pf<N>` | a straight-line continuation after a `perform` | `DKFrame` |
| `_rf<N>` | a perform continuation that itself uses control | `DKResumeFrame` |
| `_j<N>` | a heap-reified join point | `DKFrame` or `DKResumeFrame` |
| `_rk<N>` | the rest of a handler case after a non-tail `resume` | `DKResumeFrame` |
| `_aw<N>`, `_ak<N>` | an `await` continuation | `DKFrame` / `DKResumeFrame` |
| `_b<N>` | a bounced tail call | `DKResumeFrame` |
| `_loop<N>__cps` | a loop helper | `(params..., invariants..., DK *)` |
| `_unw` | the unwinder that runs a frame's pending `defer`s if the chain is abandoned | `(intptr_t env)` |
| `_sk*` | serial-continuation frames and registrations | -- |

A helper's captured variables travel in a per-helper env struct,
`typedef struct { T f0; ... } <helper>_env`. It is allocated where the frame is
created and registered on the reap list. `dk_env_sized` records its size, so
a parked copy can own a private copy.

Captures are restricted:

- Slot scalars and owning-free aggregates are always allowed.
- Owning values are allowed when the continuation is single-shot.
- In a multi-shot handler case, an owning capture is allowed if it is an rc
  (incremented on read-out) or a borrow-only carrier.
- A consuming carrier evicts the function.

### Joins

`CT_LETCONT` has three shapes:

- **Inline**: a C label, `param = v; goto L<j>;`. This is the default.
- **Heap join** (`emit_heap_join`): the join body is the continuation of a
  non-tail threaded call. It is lifted to `_j<N>` and pushed as a `join_once`
  frame in front of `__kont`, and the callee is called with that frame as its
  continuation. The frame is registered with `__dk_reap_node`, so only that one
  node is freed at the boundary; `dk_free` would walk into the caller's chain.
  If it runs exactly once without being copied, `__dk_join_release_node`
  hands it back immediately.
- **Escaping join**: something inside a lifted region delivers to `j`. It
  becomes a resume frame `__ejN`, which is made the current continuation while
  the body is emitted.

### Worked example: perform and handle

For this program:

```turmeric
(defeffect Ask [] : int)

(defn add-ask [x : int] : int
  (+ x (perform (Ask))))

(defn main [] : int
  (println (handle (add-ask 1)
             (Ask [] k) (resume k 41)))
  0)
```

`tur emit-c` produces (the stdlib's colored helpers omitted):

```c
/* perform: the rest of add-ask, (+ x <performed>), is a _pf frame */
typedef struct { int64_t f0; } add_hyask_pf0_env;
static intptr_t add_hyask_pf0(intptr_t env, intptr_t __t0__slot) {
    add_hyask_pf0_env *__cap = (add_hyask_pf0_env *)(intptr_t)env;
    int64_t x = __cap->f0;
    __dk_join_release_env((intptr_t)__cap);
    int64_t __t0 = (int64_t)(__t0__slot);
    return (intptr_t)((x) + (__t0));
}
static int64_t add_hyask__cps(int64_t x, DK *__kont) {
    add_hyask_pf0_env *__pfe0 = malloc(sizeof(add_hyask_pf0_env));
    __pfe0->f0 = x;
    __dk_reap_ptr((intptr_t)__pfe0);
    return dk_perform(2, "Ask", (intptr_t)(0),
        __dk_reap_node(dk_env_sized(dk_frame_join(add_hyask_pf0, (intptr_t)__pfe0, __kont),
                                    sizeof(add_hyask_pf0_env))));
}

/* handle: _hk0 is what runs after the handle; _hc0_0 is the Ask case */
static intptr_t main_hk0(intptr_t env, intptr_t __t1__slot, DK *__kont) {
    printf("%lld\n", (long long)(int64_t)__t1__slot);
    return dk_run(__kont, (intptr_t)(INT64_C(0)));
}
static intptr_t main_hc0_0(intptr_t env, intptr_t arg, DK *subk) {
    int64_t k_5 = (int64_t)(intptr_t)subk;
    ((DK *)(k_5))->consumed = 1;
    return dk_tail_resume((DK *)(k_5), (intptr_t)(INT64_C(41)));
}
static int64_t main__cps(DK *__kont) {
    DK *__h0 = __dk_reap_keep(dk_hgroup(dk_case_k_copied_only(
        dk_handler_tail_inplace(2, main_hc0_0, 0,
            dk_frame_resume_group_end(main_hk0, 0, __kont)))));
    return add_hyask__cps(INT64_C(1), __h0); /* cps->cps */
}
```

(This excerpt is lightly trimmed; regenerate it with `tur emit-c`.)

Read it as:

1. **The handle builds the chain inside-out.** First the handle's own
   continuation (`main_hk0`, marked `group_end`), then one `DKK_HANDLER` node
   per case. The case nodes are tagged with the effect's integer tag (2) and
   annotated with what emission proved about the case:
   - `tail_inplace`: it only tail-resumes, so it may be handed the original
     chain;
   - `k_copied_only`: every use of `k` copies it.

   `dk_hgroup` stamps the run of case nodes with one group id. The handled body
   is then called with that chain as its continuation.
2. **The perform pushes its own continuation** (`add_hyask_pf0`) and calls
   `dk_perform(tag, name, arg, chain)`.
3. **The case resumes in tail position** with `dk_tail_resume`. That
   `longjmp`s to the driver, which runs the original chain:
   - `add_hyask_pf0` (`1 + 41`);
   - the handler node (transparent);
   - `main_hk0` (prints `42`);
   - the root.

---

## The DK machine

The machine is emitted by `emit_cps_runtime_prelude` (`emit_dk_runtime.c`).
Read the `struct DK` comments in that text, or in its committed copy
`src/runtime/generated/tur_rt_split_decls.h`, alongside this section.

### Node kinds

| Kind | Meaning | When a value reaches it |
|---|---|---|
| `DKK_DONE` | end of chain | `dk_run` returns the value |
| `DKK_FRAME` | `v = fn(env, v)` | transforms the value, continues to `next` |
| `DKK_RESUME_FRAME` | `rfn(env, v, rest)` | hands over the downstream chain; `rfn` owns delivery, and `dk_run` returns its result |
| `DKK_PROMPT` | a `reset` marker with a `tag` | transparent |
| `DKK_HANDLER` | an effect case keyed by `tag`, carrying `handler`/`handler_env` | transparent |
| `DKK_SHIFT` / `DKK_SHIFT0` | a pending capture to the nearest prompt with `tag` | captures (see below) |

**Tags:**

- `0` (`DK_ROOT_TAG`): the root prompt seeded at every entry. `await` shifts
  to it.
- `1`: every `reset`/`shift`. There is one prompt tag for all base delimited
  control.
- `2` and up: one per effect, assigned by `emit_cps_ir.c:effect_tag` and
  memoized per interned effect symbol. The table holds `CPS_MAX_EFFECTS` (128)
  entries. Past that, `effect_tag` returns 2 for every further effect, which
  collides with the first one; nothing diagnoses it
  ([report](https://github.com/turmeric-lang/turmeric/blob/main/docs/reported/effect-tags-collide-past-128-effects.md)).

### Operations

- **`dk_run(k, v)`** runs a `while (k)` loop over the switch above.
  - It fires `__dk_panic_abandon` when a frame sets `tur_panicking`.
  - It performs the early hand-backs for `join_once`, `group_end`,
    `copy_tail`/`copy_last` and `splice_into` nodes as they run.
  - `dk_run_root` is the same function; its `root` parameter is unused.
- **`shift` / `shift0`.**
  1. Emitted as `dk_shift(1, body, env, k)` followed by `dk_run`.
  2. The machine finds the nearest `DKK_PROMPT` with the tag, `P`, and copies
     `[k->next, P)` into a fresh `sub`.
  3. For `shift` only, it appends a re-installed prompt.
  4. It runs `body(env, sub)`, frees `sub`, and delivers the body's value to
     `P->next`.

  The base `shift` is **abortive**. `(shift f v)` delivers `f(v)` to the
  `reset` and discards the context in between, and `CT_SHIFT`'s body is
  exactly "apply the receiver to the value". A body that wants to keep `sub`
  must copy it.
- **`dk_perform(tag, name, arg, k)`.**
  1. It walks from `k` to the first `DKK_HANDLER` with that tag, `H`. The
     search is by tag only; `hgroup` does not affect it. Reaching `DONE`
     prints `unhandled effect: Name (tag N)` and aborts. `name` is the
     effect's name, a literal at the perform site; it is used only there,
     since a tag is numbered per unit.
  2. It hands the case a continuation in one of four ways, cheapest first:
     1. **Tail, in place.** The case is `tail_inplace`, the handler is deep,
        and a driver is installed: the case gets the **original** `k`.
     2. **Tail, copy.** The case is `tail_resume`: `sub` is a copy of the whole
        rest of the chain. Resuming it runs past the handle, so nothing is left
        to deliver.
     3. **Resume-once, in place.** The case resumes at most once, non-tail and
        straight-line, and the chain past it is single-shot (`ss`): the case
        gets the original `k`, marked `resume_inplace`. If the case returns
        without resuming, `k..H` is abandoned (`__dk_abort_abandon`).
     4. **General.**
        - `sub` is a copy of `[k, H)`, plus a re-installed handler group for a
          deep handler, or copies of just the enclosing handler markers for a
          shallow one.
        - It is reap-kept before the case runs.
        - After the case returns: if `sub` was never copied, the perform was
          abortive and the unwinders of `k..H` fire.
        - The case's result is delivered to `H->next`, or returned as is when
          the case delivers through the chain itself (`case_delivers`).
- **Resume** lowers one of four ways (`emit_cps_ir.c:emit_resume`):
  1. **`dk_tail_resume(k, v)`** for a tail resume in a tail-resume case. It
     stores the chain and value in `g_dk_resume_chain`/`g_dk_resume_val` and
     `longjmp`s to the driver.
  2. **`dk_resume_into(k, v, rest, ...)`** for a case whose own resume
     continues into the chain. The rest of the case becomes an `_rk` frame
     spliced after the resumed part, instead of running nested on the C stack.
  3. **`x = dk_invoke(k, v)`** in the general case: run a fresh copy of `k` to
     completion and return its value. `dk_invoke` runs under
     `__dk_drive_bounded`, a scoped landing, so a tail resume inside the
     resumed computation cannot unwind past the case.
  4. **`__dk_resume_word`** for a `k` that left its case (stored, or passed to
     a helper) or a cloneable wrapper.

  `cont?` on a handler continuation reads `!k->consumed`, set at every resume
  site (`__dk_cont_live_word`); a word in the first page is no continuation,
  so it answers false and `__dk_resume_word` reports and aborts
  ([resume-of-non-continuation-int-crashes](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/resume-of-non-continuation-int-crashes.md)).
  On a `call/cc` continuation it asks the runtime's live set
  (`tur_escape_is_live`); a cloneable or serial continuation is always live
  ([cont-pred-on-escape-cont-fails-to-link](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/cont-pred-on-escape-cont-fails-to-link.md)).

### Why there is a driver

Without it, a handler that tail-resumes nests C frames on every perform:
`dk_perform -> case -> resume -> ... -> dk_perform`. That costs about 160 B
per perform, so an effectful loop of N iterations uses O(N) native stack.
E7 ("cps-tramp-resume", always on since 2026-07-19) replaces the nested call
with a `longjmp` back to the entry wrapper's landing:

1. The case's resume becomes `dk_tail_resume`, which `longjmp`s with the
   chain to run.
2. `__dk_drive_after` loops: re-arm the landing, run the chain, repeat on the
   next yield.
3. Each landing restores the pending-perform list it saved.

This is why every entry wrapper has a `TUR_SETJMP`. It is also why
`tur_jmp_buf` selection (`emit_tur_jmp_buf_prelude`) is emitted
unconditionally. Windows and the JIT need a non-`__builtin_setjmp` spelling.

### Field families

`struct DK` has about 40 fields. Most are protocol bits that emission or
`dk_perform` sets so that the runtime can skip a copy or free a node early.
They fall into groups:

| Group | Fields |
|---|---|
| Core | `kind`, `fn`, `env`, `tag`, `next`, `body`/`body_env` (shift; `body_env` is also reused as a scratch slot), `handler`/`handler_env`, `rfn` |
| Ownership and teardown | `borrow_next` (another chain owns `next`: `dk_free` stops here), `env_clone`/`env_drop`, `env_size`, `env_owned`, `unwind`, `ncopy`, `join_once`, `group_end`, `orphaned`/`orphan_env`, `inplace_head` |
| Handler semantics | `shallow`, `tail_resume`, `tail_inplace`, `case_delivers`, `resume_into`, `hgroup`, `consumed` |
| Single-shot shortcuts | `resume_once`, `resume_once_dyn`, `resume_inplace`, `resume_cut`, `splice_into`, `copy_tail`, `copy_last`, `k_last`, `k_copied_only`, `multishot`, `ss` |

Who sets them:

- **The emitter** sets the handler-semantics and single-shot fields from its
  case analyses: `case_tail_resumes_chain`, `case_resumes_k_only_in_tail`,
  `case_resumes_into`, `case_resumes_once`, `case_k_copied_only`,
  `case_is_multishot`, `case_reopens`. It does so through the `dk_handler_*`
  and `dk_case_*` constructors.
- **The runtime** computes `ss` (in `dk_new`, from `next`), `ncopy`,
  `resume_inplace` and `splice_into`.
- **`dk_copy_node`** carries the protocol fields into a copy. Per-instance
  state (`ncopy`, `unwind`, `borrow_next`, `resume_cut`, `inplace_head`)
  starts at zero.

---

## Memory: who frees what

- **The reap list.** Every DK node, env box and slot box the emitted code
  allocates is registered on a per-thread list (`__dk_reap_v`/`__dk_reap_kind`)
  through `__dk_reap_keep`, `__dk_reap_ptr`, `__dk_reap_node` or
  `__dk_reap_closure`. The entry kinds are: a plain `free`, a whole DK chain, a
  headered closure, or a single node.
- **Entry exit.** `__dk_entry_leave` frees the root. At entry depth 0 it then
  drains the list (`__dk_reap_run`). A nested entry only forgets back to its
  mark. An entry whose body parked on an `await` moves its registrations into
  the park record instead.
- **Early hand-backs.** Holding everything until the outermost entry returns
  would make every effectful loop grow, so the common single-shot shapes give
  their memory back as soon as they run:
  - `__dk_join_release_node`/`_env`
  - `__dk_group_release`
  - `__dk_copy_last_release`
  - the end of `__dk_drive_bounded`
  - `dk_perform`'s `k_copied_only` free
  - `dk_resume_into`'s `k_last` free
  - `__dk_await_release`

  Each fires only for the last reap entry, and only when that entry was never
  copied.
- **Captured continuations.**
  - `dk_perform`'s general `sub` is owned by the reap list; the case's `k`
    borrows it.
  - A `k` stored somewhere is copied on store.
  - A `^multishot`, resumable-payload or cloneable `k` is wrapped in a
    `tur_cloneable_cont` whose env is a DK chain copy.
- **Abandonment.** A frame created while `defer`s were pending carries an
  `unwind` hook (`__dk_with_unwind`). It fires exactly once, from:
  - `__dk_panic_abandon` (a panic unwinding the chain);
  - `__dk_abort_abandon` (an abortive handler case);
  - `__dk_escape_abandon` (an `escape` landing).
- **Pinning.** `#lang r7rs` re-entrant `call/cc` copies the C stack, which can
  hold DK pointers. Once a stack image exists, the prelude sets `tur_dk_pinned`
  and DK memory is never reclaimed again.
- **Regions.** DK frame-env fills carry no `TUR_REGION_NOTE`, and they are not
  on the hooked-store list in CLAUDE.md.
  - A region-scope callee is always reached through the `CT_TAILCALL`
    cps->direct arm. That arm brackets the call itself with
    `tur_region_push`/`pop_checked` and notes the result's escape.
  - The `CT_LETCALL` arm deliberately has no bracket.
    `tests/fixtures/region-scope-nontail-cps` pins that such a callee never
    lands there.

  A new path that lets a DK frame outlive a bracket needs a note, under the
  CLAUDE.md "Region Store Hooks" rule.

Known residue is tracked in open reports, for example:

- [effect-nontail-resume-copies-held-until-outer-entry](https://github.com/turmeric-lang/turmeric/blob/main/docs/reported/effect-nontail-resume-copies-held-until-outer-entry.md)
- [cloneable-cont-escaped-chain-leaks](https://github.com/turmeric-lang/turmeric/blob/main/docs/reported/cloneable-cont-escaped-chain-leaks.md)

Check [`docs/reported/`](https://github.com/turmeric-lang/turmeric/blob/main/docs/reported/README.md) for the current list.
Fixture programs run without LeakSanitizer under `tests/run.sh`. Use
`tests/run-leak-check.sh` (a `requires.leak-check` marker) to pin a DK leak
fix.

---

## Fn values and callbacks

A `perform` inside a callback must reach the handler of the function that
**called** the callback. That works only if the call through the fn value
passes the caller's chain along, i.e. if it is *threaded*. Calling the fn
value's plain C entry starts a fresh root, and the perform aborts with
"unhandled effect".

The translator and emitter have three ways to thread:

| Mechanism | Applies to | Emitted as |
|---|---|---|
| **`fn_cps` slot** (`via_fncps`) | a rank-2 (`tur_poly_fn_t`) parameter | `if (f.fn_cps) return f.fn_cps(f.env, w0, ..., k);`, else call `f.fn` and deliver. The slot is filled by the poly-wrap (`emit_expr.c:ensure_poly_wrap_cps_thunk`) when `cps_ir_fncps_sig_ok` holds. |
| **The E2 twin registry** (`via_registry`) | fn-valued parameters and locals every one of whose program-wide arguments is a registered threadable function (a fixpoint in `ensure_S`) | `((cast)__tur_cps_lookup_checked(key, "name"))(args..., k)`. A static initializer, `__tur_e2reg_<fn>`, maps each threadable function's entry address to its `__cps` twin (or an `__e2w` word-widening adapter). |
| **Local closure rewrite** | a `let`-bound capturing lambda called in the body | `(f a)` becomes a direct threaded call to the lifted lambda with its env (`cps_ir_let_local_closure`) |

**Signature limits.** The `fn_cps` and registry ABI passes each argument as
one `int64_t` word, `int64_t (*)(void *env, int64_t a0, ..., DK *)`:

- at most `CPS_FNCPS_MAX_ARGS` = 8 arguments;
- scalar kinds only, with floats bit-cast through a union
  (`cps_ir_fncps_word_of` / `cps_ir_fncps_value_of`);
- no aggregates.

A callback outside those limits is not threaded. If it is effectful, its
performer is evicted and the build reports the error from Stage 3, naming the
fn value through `emit_cps_ir_effect_escaping_fnval`. The value-representation
side of this (which fn-value representation crosses which boundary) is in the
[value representations guide](value-representations-guide.md#representations-fn-typed-values).

First-class handler values (`with-handler` on a non-literal) reach DK through
the handler table: each `tur_handler_entry_t` carries `dk_fn`/`dk_tag`,
stamped by `emit_effects.c`. `dk_hgroup_from_table` installs every entry with
a non-zero `dk_fn` as a multishot tail case.

---

## Features built on the chain

| Feature | Compiled lowering |
|---|---|
| `perform`/`handle`/`resume` | `DKK_HANDLER` nodes plus `dk_perform`, as above. `handle-shallow` sets `shallow`. |
| `reset`/`shift`/`shift0` | `dk_prompt(1, ...)`, plus `dk_shift`/`dk_shift0` and `dk_run` |
| `call/cc`/`escape` | `CT_CALLCC`: a local `tur_escape_cont` `setjmp` landing. A one-shot upward escape; it does not thread the chain and arms `__dk_escape_abandon`. Invoking `k` after the landing returned aborts with "continuation invoked after its call/cc prompt returned". |
| `call/cc*` | Elaborated (`elab_call_cc_star`) to a `cloneable-shift` inside a `cloneable-reset`, so it takes the cloneable row below |
| `cloneable-reset`/`-shift` | `CT_CLONEABLE`. Shape 1 is an identity `tur_cloneable_cont`. Shape 2 is a private local chain of `dk_frame`/`dk_frame_owning` frames under `dk_prompt(1)`. The cloneable<->DK bridge (`emit_cps_cloneable_bridge_prelude`) makes `dk_invoke`/`dk_copy_range`/`dk_free` the wrapper's resume/clone/drop. |
| `serial-reset`/`-shift` | `CT_CLONEABLE` in serial mode. Only a prefix of `DKK_FRAME`s is marshaled, by tag (arithmetic) or by registered name (`__sk_registry`); deserialization rebuilds `[frames..., prompt(1), done]`. See the [serializable continuations guide](serializable-continuations-guide.md). |
| `await` | `CT_AWAIT`. Fast path: `__tur_await_ready`, then run the frame in place. Slow path: `dk_shift(DK_ROOT_TAG, __tur_await_body, fut, frame)`, which parks a copy of the chain on the future (see below). Exceptions: an `await` inside a handler-case body is delegated to the direct `tur_await_future`, and a recursive await evicts. |
| `async` | Always a `CT_LETRAW` (the direct emitter spawns it). The body runs inline on the spawner's stack; there is no fiber. An `async` body that captures a session endpoint runs on a pthread instead |
| `gen`/`yield` | **Not on the chain.** A generated state machine (a struct plus a `_next` switch; see the [generators guide](generators-guide.md)) |
| `#lang r7rs` `call/cc` | **Not on the chain.** The prelude copies the C stack into an image (`TUR_REGION_NOTE_WORDS` over the copy). The first capture sets `tur_dk_pinned`, which turns DK reclamation off for the rest of the process |

---

## Async, fibers and threads

**Awaiting.** `__tur_await_body` is the shift body an `await` runs. It works
in three steps:

1. A ready (or rejected) future resumes at once with `dk_invoke`.
2. Otherwise it drains runnable scheduler fibers and checks again.
3. Otherwise it parks: a `TurAsyncPark` holding `dk_copy_range_owned(subk)` is
   hung on the future, and `tur_async_suspended` is set.

When the body then unwinds to its entry wrapper, `__dk_entry_leave` hands the
entry's root and reap segment to the park rather than freeing them. Fulfilling
the future (`__tur_async_resume`) re-enters under `__dk_drive_bounded`.

A park has to reach the `async` body's own entry. If a direct-style C frame
sits between the await and that entry, the runtime aborts ("await: a pending
future would park below a direct-style call"). TUR-W0044 is the compile-time
warning for the shapes that lead there. See
[await-parks-only-to-the-nearest-c-frame](https://github.com/turmeric-lang/turmeric/blob/main/docs/reported/await-parks-only-to-the-nearest-c-frame.md).

**Fibers** survive as **stacks** for `fiber-new`, task groups, the scheduler,
and async I/O and timers (`FiberBlock` in the emitted runtime;
`src/async/fiber.c` for the reactor's local fibers). They no longer run
effects. Each fiber carries its own copy of the DK per-thread state, which
`tur_fiber_block_resume` swaps on every switch:

- the driver landing;
- the reap list and entry depth;
- the pending-perform list;
- the escape and r7rs dynamic state.

The DK variables are `TUR_THREAD_LOCAL`:

- `g_dk_pend_perform`
- `__dk_reap_v`, `__dk_reap_kind`, `__dk_reap_n`, `__dk_reap_cap`
- `__dk_entry_depth`
- `g_dk_driver`
- `g_dk_resume_chain`, `g_dk_resume_val`

Under the JIT and the split runtime they are routed through
`src/runtime/tur_tls.c`.

These are process-wide:

- the atomic `g_dk_hgroup_ctr`;
- `tur_dk_pinned`;
- the E2 registry `__tur_cps_reg`, grown with `realloc` by
  `__tur_cps_register`. It was a 256-entry array until 2026-10-17, which
  dropped later registrations, so calls through those callbacks fell back to a
  fresh root and aborted with "unhandled effect"
  ([report](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/cps-callback-registry-overflows-past-256.md));
- `__sk_registry`;
- `g_dk_case_reopen_hnode`. It is a **plain static** that `dk_perform` sets
  immediately before each case call and a re-opening case reads at entry, so
  two threads performing at once could race on it. No failure has been
  observed or reported.

---

## The split runtime

The runtime text this guide describes is also committed, pre-generated:

| File | Contents |
|---|---|
| `src/runtime/generated/tur_rt_split.c` | the runtime as one translation unit |
| `src/runtime/generated/tur_rt_split_decls.h` | its declarations |
| `src/runtime/generated/tur_rt_split_embed.c` | the declarations as a string, plus an xxHash64 of the canonical text |

How they are produced and used:

- `tur emit-rt-split` prints the canonical text (`emit_rt_split_source`, every
  prelude gate forced on), and `tools/gen-runtime-split.py` turns it into the
  three files.
- `tur_rt_split.c` is the host runtime the JIT resolves against. It is also
  built into `libturt_preamble.a`, which the cc path links on Linux and
  Windows.
- The JIT re-hashes the canonical text at use time
  (`driver/preamble_split.c:jit_try_split_preamble`) and falls back to the full preamble on a
  mismatch. CI checks that the split path actually engages.

**A change to `emit_dk_runtime.c`, or to any preamble text, needs those files
regenerated and committed in the same PR.** Otherwise the split path silently
disengages. That makes the JIT slower, but the output stays correct.

---

## Debugging and testing

| Tool | Shows |
|---|---|
| `tur --dump-cps check f.tur` | the CT-IR per colored function ([above](#--dump-cps)) |
| `tur --dump-cps-coloring check f.tur` | coloring; stdout is current, stderr is legacy |
| `tur --dump-clone-plan check f.tur` | cloneable-shift capture plan (legacy metadata) |
| `tur --dump-cps-mono emit-c f.tur` | `[cps-mono]` lines: whether each colored generic's monomorphs are CPS-admissible (not listed in `--help`) |
| `tur --dump-effects check f.tur` | the inferred effect row per `defn`, for when coloring and the row disagree |
| `--emit-abi-trace` | ABI decisions at call boundaries, for a suspected cps->direct bridging bug |
| `--debug` or `TUR_CC_FLAGS=-O0` | turns off the sibling calls that `-O2` uses to hide C-stack growth; use it to reproduce a stack-growth bug |
| `TUR_RUNTIME=source\|lib\|split\|auto` | which copy of the runtime is linked (`tests/run-leak-check.sh` defaults to `source`) |
| `TUR_CPS_CENSUS=1` | why each function is colored (`control`/`callee`/`indirect`) |
| `TUR_TRACE_EVICT=1` | `[EVICT]` category per colored function left out of `S`, plus `[E2-COLOR]` threadability |
| `TUR_TRACE_CORE=1` | `[CORE-FAIL]` (structural check), `[LOOP-*]` (loop lowering), `[CTX-REJECT]` (cloneable/serial context) |
| `tur emit-c f.tur` | the emitted C. Search for `__cps(`, `dk_perform(`, `dk_handler` |

Tests:

- The ctest targets `tur_cps_prompt_unit` and `tur_cps_rt_unit`
  (`tests/cps_prompt_unit.c`, `tests/cps_rt_unit.c`) test the two standalone
  reference runtimes, **not** the emitted machine.
- The emitted machine is tested end to end by fixtures:
  - `cps-*` (about 190), including `cps-backend-*` (individual lowering
    shapes; their header comments usually quote the C they expect),
    `cps-tramp-resume-*` (tail-resume at depth, up to a million performs) and
    `cps-effectful-*` (threaded callbacks);
  - `effect*`, `continuation-*`, `async*`/`await*`, `cloneable*`, `serial*`.

  Some of these fixtures' header comments still describe the deleted backends.
- The `cps-oracle-*` fixtures (49) were written to assert direct == CPS when
  there were two backends. Today each is an `input.tur` plus `expected.stdout`
  that the compiled (`run.sh`), interpreted (`run-turi.sh`) and JIT
  (`run-jit.sh`) runs must all match.
- `--dump-cps-coloring` and `--dump-effects` output is pinned by
  `cps-coloring`, `cps-effect-coloring` and `effect-dump`, and in
  `tests/run-flags.sh`.
- About 75 of the `requires.leak-check` fixtures exercise a control operator.

**When you add a lowering shape:**

1. Add a fixture that exercises it.
2. Run it with `TUR_TRACE_EVICT=1` to confirm the function is actually in `S`.
   A fixture whose function was quietly evicted can still pass, because it no
   longer goes through the code you wrote.
3. If the shape allocates, add a `requires.leak-check` twin.

---

## Reading the archive

The design was built in stages, and each stage's plan is archived with a
status banner. In order:

1. [cps-transform-plan](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/cps-transform-plan.md):
   - CPS0-CPS11;
   - the coloring rule (CPS0.1);
   - the reference DK machine (CPS5);
   - the first codegen wiring.
2. [cps-ir-to-c-backend-plan](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/cps-ir-to-c-backend-plan.md): the
   CT-IR backend and the `__cps` ABI (C0/C1).
3. [cps-backend-unification-plan](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/cps-backend-unification-plan.md)
   and [cps-backend-direct-lowering-removal-plan](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/cps-backend-direct-lowering-removal-plan.md):
   folding `emit_cps.c` into the CT-IR backend and deleting it.
4. [cps-runtime-finish-plan](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/cps-runtime-finish-plan.md),
   [cps-dk-sole-effect-lowering-plan](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/cps-dk-sole-effect-lowering-plan.md)
   and [cps-dk-endgame-remaining-plan](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/cps-dk-endgame-remaining-plan.md):
   - E7 tail-resume trampolining;
   - deleting the fiber effect runtime.
5. [cps-backend-effectful-callbacks-plan](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/cps-backend-effectful-callbacks-plan.md):
   the E2 threading mechanisms.
6. [history/cps-backend-owning-env-teardown-e3-plan](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/cps-backend-owning-env-teardown-e3-plan.md)
   and [cps-backend-env-capture-owning-values-plan](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/cps-backend-env-capture-owning-values-plan.md):
   owning captures and env teardown.
7. [cps-backend-unification-u5-async-substrate-plan](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/cps-backend-unification-u5-async-substrate-plan.md):
   `await` on DK. Its architecture section predates the native `CT_AWAIT`
   lowering.

Individual `docs/archive/cps-*.md` bug reports explain specific DK flags, and
the field comments in `struct DK` often name the report that introduced them.

---

## Maintenance

Update this guide in the same PR when you:

- add or remove a CT-IR term kind, a DK node kind or a DK field family;
- change the `__cps` ABI or the entry wrapper;
- add an eviction category, a trace variable or a dump flag;
- move a feature onto or off the chain.

The source excerpts here are regenerated with `tur emit-c`; refresh them when
the shape they show changes. Cite functions, not line numbers.
