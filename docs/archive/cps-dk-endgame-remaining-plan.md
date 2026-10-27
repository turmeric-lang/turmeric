---
title: "v2 -- CPS/DK endgame: the remaining fiber-live surface (measured)"
status: COMPLETE (2026-07-19) -- every bucket B1-B8 closed; flag graduated; fiber effect runtime deleted (Stage G)
severity: existential (this was the concrete work-list for deleting the fiber effect runtime)
measured: 2026-07-18
resolved: 2026-07-19
---

> **COMPLETE (2026-07-19).** Every fixable bucket (B1-B7) and the mislabeled
> "permanent" bucket (B8) is DONE. The `cps-tramp-resume` experiment has
> GRADUATED to always-on (`src/runtime/experiments.c` GRADUATED[], commit
> 786946a1), and the fiber effect runtime C has been physically DELETED from
> `emit_module.c` (Stage G, commits 7b40fc4e/d46f74bef -- see updates 3a.2-3a.4
> below). A full-corpus sweep returns zero `tur_effect_perform`,
> `tur_effect_cont_resume`, `global_effect_handler_chain`, `EffectHandlerFrame`,
> and `tur_handler_dispatch` call sites. The CPS/DK backend is the sole effect
> lowering. The only residuals are pre-existing owning-value teardown leaks
> tracked separately in `docs/reported/` -- not fiber-liveness blockers. This
> work-list is fully discharged and ready to archive.

# CPS/DK endgame -- the remaining fiber-live surface

Tactical companion to `cps-dk-sole-effect-lowering-plan.md` (the strategic
master plan). That document says *why* the fiber effect runtime must be deleted
and states the kill criteria. This document is the concrete, **measured**
work-list: exactly which fixtures still ride the fiber under
`--enable=cps-tramp-resume`, why (root cause pinned to a repro), and the fix
direction for each -- so the flag can graduate to always-on and the fiber effect
machinery can be deleted.

## 0. Measure this correctly -- do NOT trust the `eff=1` column

The `TUR_TRACE_EVICT=1` `[EVICT] ... eff=N` trace is the WRONG signal for the
deletion goal, in two directions:

- **It over-counts.** `eff` is `(se->eff_lo || se->eff_hi)` -- the *entire*
  effect row, which includes compile-time-only markers (`#fx{Unsafe}`, `IO`).
  So `unsafe-*`, `variadic-*`, `sized-*`, `lint-*`, `free-*` fixtures light up
  `eff=1` while never touching the fiber effect runtime at all.
- **It under-counts.** The trace only fires for a `cps_colored` *named* `defn`
  that falls back to the direct emitter. A top-level handle folded into a
  synthesized main, a handle inside `(defmodule ...)`, and a first-class
  `with-handler` value emit fiber `tur_effect_perform` with **no eviction trace
  line at all**.

**The correct measure** is the emitted C itself: count `tur_effect_perform("`
call sites (string-literal effect name = a real call; the bare
`tur_effect_perform(const char*...)` definition in the preamble is not a call
site) plus `__handle_body_N(void)` fiber handle thunks.

```sh
TUR=./build/tur
for dir in tests/fixtures/*/; do
  input="$dir/input.tur"; [ -f "$input" ] || continue
  n=$("$TUR" --enable=cps-tramp-resume emit-c "$input" 2>/dev/null \
        | grep -cE 'tur_effect_perform\("')
  [ "$n" -gt 0 ] && echo "$(basename "$dir") $n"
done
```

Under this measure, on 2026-07-18: the 22 opted-in `cps-tramp-resume-*` fixtures
are **100% fiber-clean**, and exactly **24 non-opted-in fixtures** stay
fiber-bound when the flag is forced on. Those 24 are the entire remaining
surface. The goal is to drive them to zero (minus the documented permanent
carve-outs), then flip the flag on by default and delete the fiber effect
runtime.

## 1. The remaining surface -- 24 fixtures, 8 buckets

Fixable: **B1-B7 (20 fixtures)**. Permanent-candidate carve-outs: **B8 (4)**.

Recommended execution order is by leverage: **B3 (7) -> B1+B2 (7) -> B4/B5/B6
-> B7 -> decide B8**.

> **PROGRESS (2026-07-18):** **B1 (3), B2 (4), and B3 part 1 (4) are DONE** --
> fiber-live surface **24 -> 13**. B1: classify each `defmacro` by template head so
> a statement-expanding macro folds into the synthesized main (elab_toplevel.c).
> B2: three passes descend into `EX_DEFMODULE` bodies -- coloring (cps.c,
> flag-gated), classification (ensure_S iterates the flattened program), and
> colored-callee resolution (`callee_colored` + `fd_for_binding`). B3 part 1:
> seed `EX_WITH_HANDLER`/`EX_COMPOSE_HANDLERS` in `cps_directly_uses_control`
> (flag-gated) + `expr_has_handle`, and merge a `compose-handlers` of literals
> into one multi-effect DK handler group -- landing fh-handler-value,
> with-handler-value, fh-multishot-value, fh-compose-handlers.
>
> **B3 part 2 -- DONE (2026-07-18):** all 3 DYNAMIC handler-value cases DK-lower
> -- `fh-multi-effect-type` (handler PARAMETER), `defstruct-field-handler` and
> `-multi` (handler in a struct field). Whole B3 bucket (7) is now DK; **fiber-live
> 24 -> 10**. Steps 1-4 landed: handler-table entry `dk_tag`/`dk_fn` +
> `dk_hgroup_from_table`; DK-ABI case fn at the handler literal site
> (emit_effects_handler_lit); a `dyn` CT_HANDLE installing from the runtime table
> (build_with_handler + emit_handle); `slot_ty` admits TY_HANDLER (handler param
> sig-ok); and taint completeness (EX_WITH_HANDLER marks the handled_row as
> discharged; a bare EX_HANDLER_LIT value no longer marks handled). Verified:
> output == fiber, ASan-clean, suite green (benign preamble snapshot regen only).
> Remaining fixable: **B4-B7 (9)**.

#### B3 part 2 -- STEP 1 LANDED + concrete plan for the rest

The fiber path lowers a handler value to a runtime `tur_handler_table_t*` (entry
= `{const char *eff_name; int64_t (*fn)(args,n,k,env); void *env; uint8_t
cont_kind}`, emit_module.c preamble) and runs the body in a fiber dispatching
against the table. DK-lowering it installs a DK handler group from the table.

**STEP 1 -- LANDED (commit "B3 part 2 step 1", behavior-neutral):**
- `tur_handler_entry_t` gains `int dk_tag; intptr_t (*dk_fn)(intptr_t,intptr_t,
  struct DK*);` (fiber path calloc-zeroes them).
- `dk_hgroup_from_table(const tur_handler_table_t *t, DK *base)` (emit_dk_runtime.c):
  folds the table into `dk_hgroup(dk_handler(e.dk_tag, e.dk_fn, e.env, ...))`
  over `base`, h1-outer. Unused until Steps 2-3 wire it.

Refined approach found while scoping (more tractable than a full CT-IR rewrite):
keep the handler literal on the DIRECT emitter but have it ALSO emit a DK-ABI
case fn; only the dynamic with-handler INSTALL needs a CPS-side term.

**STEP 2 -- DK case fn at the literal site (emit_effects.c, direct path).**
`emit_effects_handler_lit` already emits the fiber case body via the direct
emitter (`__effect_handler_N`); the case body is IDENTICAL for the DK variant
except `resume`/`k`. Add an `EmitCtx` DK-case mode: emit a second fn
`__dk_hcase_N(intptr_t env, intptr_t arg, DK *subk)` that reuses the body
emission, and in `emit_effects_resume` (the `k_type.kind==TY_INT` branch) emit,
under the mode, `((DK*)k)->consumed = 1; return dk_tail_resume((DK*)k, <v>);`
with `k` bound to `subk`. Target shape (verified from with-handler-value):
```c
static intptr_t __dk_hcase_N(intptr_t env, intptr_t arg, DK *subk) {
    /* env unpack (captures) + arg unpack (0/1 effect param) */
    /* side-effect stmts (e.g. puts(s)) */
    ((DK*)subk)->consumed = 1;
    return dk_tail_resume((DK*)subk, (intptr_t)(<v>));
}
```
Populate `entries[i].dk_tag = effect_tag(eff)` (compile-time constant) and
`.dk_fn = __dk_hcase_N`, `.env` = same heap env as the fiber entry. Gate on
`g_opt_cps_tramp_resume`. Compose is free (concat copies the new fields). The 3
target case bodies are all single-shot tail resumes (no `^multishot`) -- so this
restricted emit suffices; a `^multishot` dynamic handler stays evicting until a
follow-up. NEUTRAL until Step 3 (the fns are unused).

**STEP 3 -- dynamic with-handler install (CPS side).** `build_with_handler`
(cps_ir.c) currently evicts the dynamic case (`CT_UNSUPPORTED`). Add a CT term
`CT_WITH_HANDLER_DYN { CAtom table; CVar x; CTerm *delim; CTerm *body }` (or
reuse CT_HANDLE with a `dyn_table` atom + empty cases). Translate
`(with-handler <dynamic hv> body)` -> eval hv (emit_value -> table var), then
emit exactly like CT_HANDLE (emit_cps_ir.c ~5960) but with the handler chain
replaced by:
```c
DK *__hN = __dk_reap_keep(dk_hgroup_from_table(<table>,
             dk_frame(<hk>, (intptr_t)__kont, dk_copy_enclosing_handlers(__kont))));
```
where `<hk>` is the with-handler continuation (LH_RESET_CONT over the body).
Run the body as the delim under `__hN`; deliver the result. Wire the new term
through term_core_ok / first_unsupported / collect_effects / the CTerm walkers.

**STEP 4 -- semantics + verify.** Env ownership/free timing (the fiber path frees
the table at with-handler exit; on the DK path the table + case envs must be
reaped once -- reuse `__dk_reap_keep`/boundary reap), and the body's OWN
unhandled effects propagating outward (the base frame already carries
`dk_copy_enclosing_handlers`). Verify per Sec 2: fiber-vs-DK output equivalence
+ ASan-clean on all 3 fixtures, then the suite.

---

### B1 -- macro-expanded top-level handle (3 fixtures) -- FIXABLE

**Fixtures:** `effect-with-write`, `effect-with-fail`, `effect-with-getenv`.

**Measured root cause.** A *literal* top-level handle already DK-lowers:

```turmeric
(defeffect Ask [] : int)
(handle (do (let [x (perform (Ask))] (println x)))
  (Ask [] k) (resume k 42))         ; => 0 perform sites under the flag
```

A **macro-expanded** one does not:

```turmeric
(defeffect Write [s : cstr] : nil)
(defmacro with-write [body]
  (handle body (Write [s] k) (do (println s) (resume k nil))))
(with-write (do (perform (Write "hello")) (perform (Write "world"))))
                                    ; => 2 perform sites (fiber) under the flag
```

The synthesized-main fold (`elab_toplevel.c`, `fold_stmt_is_risky` /
the fold gate) is intentionally macro-conservative: it aborts the fold when a
top-level form has a macro head, so a macro that expands to a `handle` never
reaches the d2b-main DK path.

**Fix direction.** Run the fold *after* top-level macro expansion (fold on the
expanded form), or teach the gate that a macro whose expansion is a single
`handle`/`reset`/`do`-of-effectful-statements is fold-safe. The DK lowering of
the resulting handle already works (the literal case proves it) -- this is
purely about letting the expanded form reach the fold. Guard: flag-off must stay
byte-identical; only the flag-on synthesized main changes.

---

### B2 -- handle inside `(defmodule ...)` (4 fixtures) -- FIXABLE

**Fixtures:** `module-effect-private`, `module-cross-module-effect`,
`effect-export-explicit`, `effect-row-cross-private`.

**Measured root cause.** The blocker is the `defmodule` wrapper itself, isolated
by bisection:

| Variant | perform sites (flag on) |
|---|---|
| `(defn run [] (handle ...))` called from `main`, no module | **0** (DK) |
| `^export run`, no module | **0** (DK) |
| same code inside `(defmodule mymod (export run) ...)` | **1** (fiber) |
| inside `(defmodule ...)` with NO export | **1** (fiber) |
| inside `(defmodule ...)` with `^private` effect | **1** (fiber) |

So it is neither the export nor `^private` -- wrapping an otherwise-DK-lowerable
`handle` in `(defmodule ...)` reintroduces the fiber lowering. The
module-qualified function path (`mymod__run`) is not being routed through the
DK/d2b machinery the bare top-level path uses.

**Fix direction.** Trace where module member elaboration diverges from
top-level `defn` elaboration (module name-mangling / the module-member emit
path in `elab_toplevel.c` + `emit_cps_ir.c`). The colored classifier and the
d2b/DK lowering must see module members identically to top-level defns. Likely a
missing `cps_colored`/d2b classification on the module-qualified binding, or the
module emit path bypassing `emit_cps_ir`.

---

### B3 -- first-class handler VALUES (7 fixtures) -- FIXABLE, highest leverage

**Fixtures:** `fh-handler-value`, `with-handler-value`, `fh-multishot-value`,
`fh-multi-effect-type`, `fh-compose-handlers`, `defstruct-field-handler`,
`defstruct-field-handler-multi`.

**Measured root cause.** These use the first-class handler form -- a runtime
handler object built with `(handler (E [x] k) ...)` and installed with
`with-handler` -- rather than the `handle` special form:

```turmeric
(with-handler (handler (Ask [] k) (resume k 42))
  (do (println (perform (Ask))) 0))
```

plus its combinators: `compose-handlers` (two handler values merged) and
handler values stored in a `defstruct`/ADT field (`(handler Ask int int)` typed
field, read back with `.h` and installed). The DK backend lowers the static
`handle` special form but not the runtime-handler-value path -- that path still
lowers to `__handle_body_N` / `tur_effect_perform`.

**Fix direction.** This is the single biggest win (7 fixtures) and the deepest
of the fixable buckets. `with-handler` applied to a handler *value* needs a DK
lowering analogous to `handle`: install the handler object's cases as a DK
handler frame (`dk_hgroup` / the `DKK_HANDLER` path) at runtime. The handler
value already lowers to `TY_HANDLER` on the int64 carrier; the work is emitting
a DK handler-install from a runtime handler object instead of from static
handle-case syntax. `compose-handlers` = install both case-sets in one group;
struct-field handlers = the same install where the handler object comes from a
field read. Do `fh-handler-value`/`with-handler-value` first (simplest), then
multishot/multi-effect, then compose/struct-field.

---

### B4 -- cross-function effect propagation (2 fixtures) -- DONE (both DK-lower)

**Status (2026-07-18): BOTH fixtures land, suite 2203/0.**
- `cps-backend-effect-under-match`: perform=0, output 8. (commits 187bb6a, 37fb373)
- `handle-effectful-fn-param-same-fn`: perform=0, output 5/5, ASan-clean. (commit ad31ec5)
  Threaded the fn-value param called in a handle body to the handle's OWN prompt
  (ptc_walk EX_HANDLE case) + registered address-taken named effectful fns for
  the `__tur_cps_lookup` registry (not just lifted lambdas).

**Fixtures:** `cps-backend-effect-under-match`, `handle-effectful-fn-param-same-fn`.

- `cps-backend-effect-under-match`: a `perform` inside a `match` arm of a callee
  (`pick`) reached transitively through another callee (`route`) from the
  handler's `run`. The colored `__cps` threading is not propagating through the
  match-arm-in-a-transitively-called-function shape.
- `handle-effectful-fn-param-same-fn`: an effectful fn-**value** parameter --
  `(defn run-with [f : (fn [] #fx{E} int)] (handle (f) (E [] k) (resume k 5)))`
  called with both a lambda and a named effectful fn. This is the long-standing
  "E2 effectful fn-value" root; see the (still-open) reports
  `docs/reported/cps-effectful-fnvalue-call-under-handle-installing-hof.md` and
  `docs/reported/effectful-fnvalue-param-miscompile.md`.

**Fix direction.** Extend the E2 effectful-fn-value threading (a `DK*`-carrying
fn-value calling convention) so a colored fn-value parameter called under a
handle threads the DK; and ensure colored `__cps` propagation reaches a
`perform` under `match` in a transitively-called callee.

**Progress (2026-07-18): BOTH fixtures DONE -- suite 2203/0. Fixture 1 below;
fixture 2 (handle-body effectful fn-value threading) landed in ad31ec5 (see the
status block at the top of this B4 section for its two gated changes).**

- **LANDED -- layer 1 (match-arm coloring):** `cps_directly_uses_control` now
  recurses into `EX_MATCH` (flag-gated), so a `perform` in a match arm colors its
  fn. `pick` is colored. (commit 187bb6a)
- **LANDED -- layer 2 (opaque-ADT carrier param):** `fn_carrier_param_ok` admits
  an int64-carrier ADT param (`type_c_name` == "int64_t", i.e. a boxed sum type,
  NOT a by-value product). Both emitters already spell it `int64_t`, so the ABI
  was consistent -- only the sig gate rejected it. `pick` cleared SIG-REJECT.
  (commit 187bb6a)
- **LANDED -- layer 3 (`EX_MATCH` CT-IR lowering):** a `CT_MATCH` term (scrutinee
  atom + ctor arms, each arm's field bindings + a CPS-translated body). Scope
  restricted (match_dk_ok) to a BOXED tagged-sum ADT scrutinee, ctor arms +
  trailing wildcard, no guards/literals/by-value-products; anything outside falls
  through to the existing wholesale CT_LETRAW delegation (so int/enum matches are
  byte-identical to before). `emit_match` mirrors the direct emitter's
  `->tag`/`->as.Ctor._N` dispatch; `adt_int64_carrier` admits the boxed-sum
  scrutinee atom through the slot gate. Wired through every CTerm walker
  (term_core_ok / first_unsupported / needs_heap_join / collect_caps_rec /
  has_capture_rec / jbody_has_* / emit_binder_decls). (commit 37fb373)
- **layer 4 (downstream) -- LANDED as a consequence:** with the match lowered,
  `route` (constructs `(Full 7)`) and `run` (was SIG-TAINT) both DK-lower;
  `emit-c --enable=cps-tramp-resume` on the fixture emits 0 `tur_effect_perform`.
- **Known flag-on rough edge:** `emit_match` does not drop the scrutinee node, so
  the flag-on path leaks it (16 bytes) atop the universal DK-node baseline leak;
  flag-off is leak-clean. See `docs/reported/cps-match-scrutinee-not-dropped.md`.

Original 4-layer analysis (for reference):

- `cps-backend-effect-under-match` has FOUR layers, all required together:
  1. `cps_directly_uses_control` (cps.c) has no `EX_MATCH` recursion, so a
     `perform` in a match arm never colors its fn (`pick` reads UNCOLORED ->
     fiber-performs `Choose` -> taints it). Adding an `EX_MATCH` seed IS correct
     but must be **flag-gated** -- coloring `pick` flag-off broke the shipping
     path (it then evicts and the fixture stopped printing `8`).
  2. Even colored, `pick [b : Box]` is **SIG-REJECT** on its ADT param. `Box` is
     a boxed SUM type: kind is `TY_ADT`, def non-NULL, but `def->is_heap` is
     **false**, so `carrier_handle_ok` (heap-adt/heap-struct only) does NOT match
     it -- yet the direct emitter passes it as an `int64_t` carrier. A
     `fn_carrier_param_ok` mirroring `fn_carrier_ret_ok` therefore does NOT admit
     `Box`; admitting it needs a broader "boxed-ADT int64-carrier param"
     predicate AND an emit_params ABI that spells it to match the caller
     (binder_ctype_full currently spells the ADT pointer, not int64).
  3. `route` is `BODY-STRUCT-OR-TAINT` -- it constructs `(Full 7)` (a heap-ADT
     construction) in a colored fn, itself an admission gap.
  4. `run` is `SIG-TAINT` only as a consequence of 1-3; it should clear once the
     chain admits.
- `handle-effectful-fn-param-same-fn` is the E2 effectful-fn-value root: a
  `(fn [] #fx{E} int)` PARAM called under a handle (`(handle (f) ...)`) must
  thread the DK so `f`'s `perform` reaches the caller's handler. No small
  bridge; it is the DK-threaded fn-value calling convention.

Recommended order when resumed: land the gated `EX_MATCH` control seed first
(sound, isolated), then the boxed-ADT carrier-param ABI (2) + heap-ADT
construction admission (3) together, then E2 for the second fixture. Given the
ADT-ABI depth, B4 is a proper multi-slice effort, not a quick win like B1-B3.

---

### B5 -- async x effect interaction (2 fixtures) -- DONE (both DK-lower)

**Status (2026-07-18): BOTH fixtures DK-lower to perform=0, output 15, ASan-clean,
suite 2203/0.**
- `effects-async` -- `(async (fn [] (handle (perform E) (E ...))))`, an async
  closure that HANDLES its own effect. (commit 8501075) The E2 taint-completeness
  rule was perm-tainting it because `expr_collect_effects` folds performed+handled
  into one set; the new `fn_net_escaping_acc` gates the rule on the NET escaping
  effect (performed but not discharged by an enclosing handle in the same body),
  so a self-handling fn-value is admitted and its interior handle installs its own
  DK prompt.
- `async-with-handler` -- `(async (with-handler ...))`, the SAME program without
  the explicit `(fn [] ...)` wrapper. `elab_async` now NORMALIZES a bare (non-fn)
  async argument into `(async (fn [] EXPR))` -- semantically `(async EXPR)` is
  already a thunk, and making it an explicit lambda routes the body through the
  lifted-fn pipeline (a colored fn whose interior handle CPS-lowers) instead of
  the inline fiber thunk.  A fn-VALUED arg (explicit lambda / named fn) is left
  untouched.  The bare-expr thunk path already forbids capturing outer locals, so
  the synthesized lambda is capture-free (no new Send obligation).  Blast radius
  was a single fixture -- every other bare async site passes a fn value.

Related (and correctly by-design) is `cps-async-recursive-await-eviction`
(archived) -- recursive `await` deliberately evicts.

---

### B6 -- effectful typeclass instance method (1 fixture) -- DONE

**Fixture:** `typeclass-effect-row-caller`. DK-lowers to perform=0, output 42,
ASan-clean, suite 2203/0. (commit dbc94be)

**Root cause.** An instance method's `c_export_name` is pinned to its INTERNAL
mangled dict-slot name (`__inst_IOShow_io_hyshow_int`), so the CPS backend
SIG-EXPORT-evicted it like a user ABI export -- it stayed fiber, tainting Write,
so `main`'s handler co-classified to fiber (SIG-TAINT).

**Fix (all gated on `g_opt_cps_tramp_resume`, flag-off byte-identical):**
- New `Binding.is_instance_method`, set in `elab_typeclasses` where the dict-slot
  name is pinned -- marks the export as internal, not a user `^:export-as`.
- The SIG-EXPORT rule (candidate gate + eviction categorization) no longer
  perm-routes an instance-method export under the flag, so the CPS backend owns
  it: it emits the `__cps` variant AND keeps the exported direct entry the dict
  slot references (`.io_hyshow = __inst_IOShow_io_hyshow_int`).
- `fn_ret_type`: an instance method's declared return is left `TY_UNKNOWN` by elab
  (a placeholder), failing the sig gate; defer to the inferred body type (`TY_NIL`).

The `(.io-show 42)` call statically resolves to the instance method, so once it
CPS-emits, `main`'s handle body threads it as a normal cps->cps call
(`__inst_IOShow_io_hyshow_int__cps(42, __kont)`) -- no dynamic-dict work needed.

---

### B7 -- escaping / multishot continuation via `set!` (1 fixture) -- DONE

**Fixture:** `effect-capture-k`. **DK-lowers to perform=0, output 0/10, ASan-clean,
suite 2203/0 (commit 5098a0a).** The escaping continuation (`(set! m k)` in a
handler case, `resume`d after the handle exits) is lowered via a by-reference heap
cell: the mutable's C name binds a shared `int64_t *` cell captured by reference
into the lifted handler case + continuation, and the stored continuation is
deep-copied (`dk_copy_range`) at the store so it survives `dk_perform`'s free
(reaped at the entry boundary).  The `fold_stmt_is_risky` escaping-mutable
exclusion that kept the fixture on the fiber path is lifted.  Full design +
emit-level diagnosis in
[cps-b7-escaping-continuation-plan.md](cps-b7-escaping-continuation-plan.md).

The DK backend already emits a structurally-close handler-case + continuation-frame
pair; three concrete defects keep it broken (`k_hystore undeclared`): (1) the
capture walkers never record a `^mut` that is WRITTEN in a lifted case body, so the
case's `env` is `0`; (2) even captured, a by-VALUE case capture cannot carry the
`set!` write back to the continuation frame -- the mutable must be a SHARED cell
captured BY REFERENCE; (3) the stored `subk` is not cloned, so it dangles once
`dk_perform` frees the captured chain (needs copy-on-store via `dk_copy`).  The fix
is a new by-reference (heap-cell) capture flavor threaded through the capture
machinery + copy-on-store -- a multi-slice feature, distinct from the bounded
single-mechanism fixes B4-B6.  See the plan doc for the full design.

**Measured root cause.** The genuinely hard one -- the handler captures its
continuation into an outer mutable and resumes it **after** the handle exits:

```turmeric
(let [^mut k-store 0]
  (let [first-result (handle (compute)
                       (Ask [] k) (do (set! k-store k) 0))]
    (println first-result)
    (let [second-result (resume k-store 5)]   ; resume AFTER handle returned
      (println second-result))))
```

The continuation escapes its handler dynamic extent and is invoked later. This
needs a heap-cell (by-reference) mutable capture plus copy-on-store so the stored
continuation survives `dk_perform`'s free of the captured chain
(`tur_cloneable_cont_alloc` is the existing reified-continuation substrate).

**Fix direction.** By-reference (heap-cell) mutable capture for a stored
continuation + copy-on-store into the `^mut` so the DK chain outlives the
handler. See `docs/upcoming/cps-backend-multishot-continuations-owning-capture-plan.md`
and `docs/upcoming/cps-dk-multishot-user-effects-plan.md`.

---

### B8 -- fiber-substrate-entangled (4 fixtures) -- DONE (was mislabeled PERMANENT)

**All four DK-lower to perform=0 (2026-07-18).** session-effects/session-mp-effects
via the inline-C-expr delegation + session/role void* carrier param + the nil
inline-C side-effect fix + capturing spawn-closure delegation.  Full 1792-fixture
sweep: zero fiber-live `tur_effect_perform`.  See cps-b8-session-effects-plan.md.

**Fixtures:** `fiber-effect`, `p19-8-fiber-effect-chain`, `session-effects`,
`session-mp-effects`.

**Status (2026-07-18): NOT permanent.** `fiber-effect`/`p19-8` already emit
perform=0. `session-effects`/`session-mp-effects` were mislabeled permanent on the
false premise that inline-C can never CPS-lower; slice 1 (delegate control-free
inline-C expressions, commit 61242b1) disproves it -- `main` moves off
SIG-INLINE-C. Remaining: a session `void*` carrier param + a capturing spawn
closure (ordinary B4/B6-class admission). Full plan:
[cps-b8-session-effects-plan.md](cps-b8-session-effects-plan.md). The old WONT-FIX
analysis (`docs/archive/cps-session-effects-permanently-fiber-bound.md`) is
superseded.

**Measured root cause.**

- `fiber-effect`, `p19-8-fiber-effect-chain`: **deliberately** exercise the
  cooperative-fiber coroutine primitive via inline-C (`tur_fiber_block_new/
  resume/yield/free`), with an effect `handle` running *on* a fiber. The
  coroutine-fiber feature is a legitimate separate language feature; the effect
  handle inside a fiber-run function cannot DK-lower across the opaque inline-C
  fiber boundary.
- `session-effects`, `session-mp-effects`: `spawn` is inline-C
  `pthread_create(..., tur_session_thread_wrapper, ...)`; the effectful
  `exchange`/`role-*` perform `SessionLog` across a pthread + inline-C session
  runtime. `SIG-INLINE-C` main -- the CPS backend cannot thread a DK through an
  opaque inline-C body.

**Decision needed.** These are the master plan's honest carve-outs. Two options:
(a) accept them as permanent `SIG-INLINE-C` carve-outs and delete the fiber
*effect* runtime anyway *iff* the coroutine-fiber primitive (`tur_fiber_block_*`,
distinct from `tur_effect_perform`) is retained as its own feature; or (b)
separate the cooperative-fiber substrate from the effect runtime so the effect
machinery can be deleted while `tur_fiber_block_*` stays. **The key question for
the master plan's kill criteria: is `tur_effect_perform` implementable purely on
`tur_fiber_block_*`, or are they independent?** If independent, B8 does not block
deleting the effect runtime -- these fixtures keep using the coroutine fibers and
never touch effect perform once their interior handles (where possible) DK-lower.

---

## 2. Verification protocol (every slice)

1. **Flag-off byte-identical.** For every fixture, `emit-c` with no flag must be
   byte-identical before/after the change (the experiment is gated; the shipping
   path must not move). Snapshot-diff the corpus.
2. **Flag-on fiber-clean.** The target fixture's `emit-c --enable=cps-tramp-resume`
   drops to **0** `tur_effect_perform("` call sites.
3. **Output equivalence.** The fixture's program output is identical on the fiber
   path and the DK path (direct-vs-DK equivalence), e.g. `effect-with-write`
   still prints `hello`/`world`.
4. **ASan clean.** The emitted program is ASan/UBSan-clean (no UAF / double-free /
   leak) -- the borrowed-`__kont` class of bug (see
   `docs/archive/cps-effect-nested-value-position-borrowed-kont-uaf.md`) is the
   cautionary precedent.
5. **Suite.** `bash tests/run.sh` (12-min timeout) -- read the result; regen
   `expected.c` snapshots in the same change if codegen moved.

## 3. Definition of done (flag graduation)

The `cps-tramp-resume` experiment graduates (flag deleted, feature always-on) and
the fiber **effect** runtime is deleted when:

- B1-B7 are closed: the fiber-live sweep (Sec 0) returns only the B8 carve-outs.
- The B8 decision (Sec 1) is made and, if any remain, they are documented
  permanent `SIG-INLINE-C` carve-outs that provably do not require the fiber
  *effect* machinery (only the coroutine-fiber primitive, retained separately).
- `tur_effect_perform`, `EffectHandlerFrame`, `global_effect_handler_chain`,
  `tur_handler_dispatch`, and the `emit_effects.c` direct perform/handle/resume
  emitters are removed; the suite stays green flag-off (now the only path).

## 3a. Flag-on suite measurement (2026-07-19) -- graduation is NOT yet reachable

The fiber-live sweep (Sec 0) is at zero: B1-B8 are all closed, every
`tur_effect_perform("` call site under `--enable=cps-tramp-resume` is gone. By
the Sec 3 criteria as originally written, that reads as "ready to graduate."

**It is not.** Sec 3 only tested the *effect* surface. Graduating the flag and
deleting the fiber runtime forces **every** fixture onto the DK path -- including
pure, non-effect generic code that currently never CPS-lowers at all (it stays
direct because the classifier only colors effectful fns). Forcing the default on
(`g_opt_cps_tramp_resume = true`) and running the full suite measures that:

```
summary: 2047 passed, 156 failed   (flag forced on; vs 2203/0 flag-off)
```

The 156 break down as:

- **138 codegen mismatch** -- snapshot churn (all snapshots are flag-off).
  Reconcilable by regen; not correctness. NOT a blocker on its own.
- **14 build failed** -- REAL codegen bugs. The `__cps` ABI param-type inference
  is wrong when a *generic / dictionary-passing* value function is CPS-lowered:
  `option_hyeq_qu__cps` is declared `(int64_t o1, int64_t o2, ...)` but its
  caller passes `tur_adt_Option__int`; `map_hyeq_hyloop__cps` takes `int64_t
  keyeq` but is handed a `void*`; `uncons-hyfmap` cps->direct emits an unmangled
  `tcons`. Fixtures: `option-basic`, `eqmap-struct-content`,
  `eqmap-struct-float-fields`, `gde-generic-dict-eq-map`, `map-typed-consumer`,
  `map-multiword-struct-key`, `set-cstr-content`, `set-typed-consumer`,
  `set-multiword-struct-element`, `show-collections`, `show-collections-content-hamt`,
  `poly-fat-float-closure-eqmap`, `cps-backend-generic-cross-fn`,
  `m5-lambda-aft-tyvar-prior-accepts-concrete`. These are NOT effect fixtures --
  they are the polymorphic eq/map/set/show + fat-closure surface. This is a
  general CPS-classifier/`__cps`-signature correctness gap, orthogonal to effect
  lowering.
- **2 stdout mismatch -- genuine DK-trampoline RUNTIME crashes**, and these are
  the sharpest blocker because they are the fiber-effect fixtures themselves:
  - `fiber-effect` -> **segfault** (exit 139) when its own body CPS-lowers.
  - `p19-8-fiber-effect-chain` -> **`*** longjmp causes uninitialized stack
    frame ***: terminated`** (exit 134) -- glibc fortification catching an E7
    tail-resume `longjmp` into a frame that already returned.
  Deleting the fiber runtime would force exactly these onto the DK path
  permanently, and they crash. This must be fixed before graduation is even
  correctness-safe, independent of the leak and snapshot work.
- **1 diagnostic mismatch** -- `errors/result-question-outside-fn`; the
  experiment lifecycle warning (TUR-W0060/W0061) very likely perturbs
  `expected.stderr`.

Plus the leak axis: the 104-byte per-perform-cont DK-node leak
(`docs/archive/cps-perform-cont-frame-leak-on-tail-resume.md`) fails a
leak-checked flag-on suite.

### 3a.1 Update (2026-07-19) -- all real blockers fixed; only snapshot regen remains

Every REAL failure class above is now closed; a re-measure forcing the flag on
gives:

```
summary: 2063 passed, 140 failed   (flag forced on; was 2047/156)
```

and **all 140 remaining failures are `(codegen mismatch)` -- ZERO build failures,
ZERO crashes, ZERO diagnostic/behavior mismatches.** What landed:

- **14 build failures -> fixed** by one root cause: a generic template must
  sig-REJECT a tyvar-carrying carrier param/return (`type_has_unresolved_tyvar` in
  `fn_carrier_param_ok`/`fn_carrier_ret_ok`) so it stays a mono_template and callers
  thread the concrete monomorph clone instead of a generic `<fn>__cps(int64_t,...)`.
- **2 DK-trampoline crashes -> fixed**: `tur_fiber_block_resume` saves/restores
  `g_dk_driver` + the DK meta-stack depth across its `swapcontext`, so a DK handle
  that yields mid-flight inside a coroutine fiber cannot leak its stack-bound driver
  to the resumer (fiber-effect 10/99, p19-8 20/30/99).
- **1 diagnostic mismatch (`errors/result-question-outside-fn`) -> fixed**, plus a
  sibling found on re-measure (`errors/effect-unhandled`): the top-level-statement
  -> synthesized-main fold was wrapping a fn-body-only form (`?`, `return`) or a
  top-level unhandled `perform` inside main, suppressing its compile-time
  diagnostic. The fold now aborts for those, keeping the top-level rejection.
- **104-byte tail-resume leak -> fixed**: both straight-line LH_PERFORM_CONT frames
  reap via `__dk_reap_node` at the entry boundary instead of a tail-resume-skipped
  `dk_free_node`. ASan-clean on all five reported fixtures.

**Conclusion.** The effect-lowering goal (fiber-live == 0) is DONE, and the flag-on
path is now a **behaviorally complete** drop-in: the entire suite builds, runs, and
diagnoses identically flag-on. The only remaining work before flipping the default
and deleting the fiber effect runtime is MECHANICAL / policy: (1) regenerate the
~140 flag-off snapshots in the graduation commit; (2) a proper `EXPERIMENTS[]`
graduation (delete the row, make the feature always-on) per CLAUDE.md rather than a
raw default flip -- which also settles any experiment-lifecycle-warning interaction;
(3) one remaining leak is a SEPARATE, pre-existing owning-value teardown gap on the
CPS path (a 16-byte `ctor_Full` ADT value in `cps-backend-effect-under-match`, NOT
the perform-cont frame) -- tracked in `docs/reported/`, the Phase-3 E3/E4 owning-
value drop. Sec 3 should be read WITH this addendum: fiber-live == 0 was necessary
but not sufficient; the sufficient bar (whole-suite flag-on correctness) is now met
except for snapshot regen + the separate owning-value leak.

### 3a.2 Update (2026-07-19) -- graduated; physical runtime deletion has a measured 3-fixture blocker

The `cps-tramp-resume` experiment is **graduated**: `g_opt_cps_tramp_resume`
defaults true, the `EXPERIMENTS[]` row is retired to `GRADUATED[]`, and the 24
`--enable=cps-tramp-resume` fixture flags files are removed. The DK path is the
default+sole *perform/handle* lowering: a full-corpus sweep finds **zero
`tur_effect_perform("` call sites and zero `__handle_body_N` fiber handle thunks**.

**But the fiber effect runtime C cannot yet be physically deleted (Stage G steps
2-3).** An invariant probe -- ICE at the `emit_value` dispatch for every effect
form -- failed on **53 fixtures**, disproving that the whole fiber effect path is
dead. Narrowing to actual fiber-runtime *calls* in emitted user code, the residue
is precise:

- `tur_effect_perform` : **0** fixtures (dead -- deletable).
- `__handle_body_N` (fiber handle) : **0** fixtures (dead -- deletable).
- **`tur_effect_cont_resume` : 3 fixtures** -- `defstruct-field-handler`,
  `defstruct-field-handler-multi`, `fh-multi-effect-type`.

The 3 residual fixtures store a `(handler E V R)` VALUE in a `defstruct` field,
read it back, and apply it via `(with-handler (.h row) body)`. The `perform`
inside `body` DK-lowers (`dk_perform`), but the dynamically-obtained handler's
`(resume k ..)` and the `with-handler` application still route through the fiber
`tur_effect_cont_resume` / `TurEffectCaptureCtx` path -- a DK-perform + fiber-resume
SPLIT. So `tur_effect_perform == 0` was necessary but **not sufficient**: the
resume-side (effect-capture-continuation) surface is a distinct axis the metric
never measured.

**The remaining deletion work is therefore one bounded slice:** DK-lower the
`with-handler`-of-a-dynamically-obtained-handler-value resume (the struct-field /
first-class handler value applied via `with-handler`), so `tur_effect_cont_resume`
drops to 0 across those 3 fixtures. Once it does, the fiber effect runtime C
(`tur_effect_perform`, `EffectHandlerFrame`/`Case`, `global_effect_handler_chain`,
`tur_handler_dispatch` + msdyn, `tur_effect_cont_*`, and the two `FiberBlock`
effect fields) plus the `emit_effects.c` direct perform/handle/resume emitters are
all unreferenced and can be deleted (churning the 140 `expected.c` snapshots, a
manageable regen -- not a >500 coordinated window). `tur_handler_table_t` STAYS
(the DK handler-value path in emit_cps_ir.c / emit_dk_runtime.c uses it).

### 3a.3 Update (2026-07-19) -- the bounded slice LANDED; fiber effect runtime fully dead

Done. The fix was smaller than a re-lowering: `emit_effects_handler_lit` was
emitting BOTH a fiber case fn (`__effect_handler_N`, whose direct-emitted `resume`
lowers to `tur_effect_cont_resume`) AND a DK case fn (`__dk_hcase_N`) into every
handler VALUE, storing both in the table (`entries[].fn` + `entries[].dk_fn`).
`dk_hgroup_from_table` installs via `dk_fn` and never reads `.fn`, so the fiber
case fn was pure dead weight. Now it is buffered and appended only when the DK
case is NOT emittable (the genuine non-DK fallback), with `entries[].fn = NULL`
otherwise. Buffering (not reordering) keeps `tmp_n` identical, so only the dead
`__effect_handler_N` blocks disappear (2 handler-value snapshots regenerated).

**Corpus sweep now: ZERO `tur_effect_perform` call sites AND ZERO
`tur_effect_cont_resume` call sites** (and zero `__handle_body_N` fiber handle
thunks). The fiber effect runtime C is **fully unreferenced** -- only its own
(unconditionally-emitted) definitions remain. Suite 2203/0.

**What's left is purely the physical removal of the now-dead runtime C** from
`emit_module.c` (Stage G step 3): the `tur_effect_perform` / `tur_handler_dispatch`
+ `__tur_msdyn_*` / `tur_effect_cont_resume`+`_valid` / `EffectHandlerFrame`+`Case`
/ `TurEffectCaptureCtx` / `global_effect_handler_chain` blocks (emit_module.c
~7283-7337, ~7473-7485, ~7538-7548, ~8525-8660) plus the two `FiberBlock` effect
fields (`effect_handler_chain`, `eff_ctx`) and their init in `tur_fiber_block_new`.
This is a mechanical dead-code deletion (no behaviour change) but a delicate one:
the blocks are INTERLEAVED with the surviving `FiberBlock`/concurrency runtime and
the msdyn path references `tur_cloneable_cont_alloc` (which STAYS, DK-shared), and
it churns all 140 `expected.c` snapshots. Best done as its own focused pass with a
suite + ASan verify. `emit_effects.c`'s `emit_effects_handler_lit`/`with_handler`/
`compose_handlers` STAY -- they now emit DK handler tables/cases, not fiber code.

### 3a.4 Update (2026-07-19) -- Stage G DONE: fiber effect runtime C deleted

Landed. The blocks above are gone from `emit_module.c`: `TurContK`,
`TurEffectCaptureCtx`, `EffectHandlerCase`/`Frame`, `global_effect_handler_chain`,
`tur_effect_perform`, `__tur_msdyn_*` + `tur_handler_dispatch`, `tur_effect_cont_*`,
and the two `FiberBlock` effect fields + their init in `tur_fiber_block_new`.
`FiberBlock`/scheduler/reactor (concurrency), `tur_handler_table_t`/`entry_t` (DK
handler-value path), `tur_cloneable_cont_*` (DK `__Shift` bridge), and
`migration_safe` STAY.

**Done criterion met (Sec 7):** a full-corpus sweep of emitted C returns
`grep -c 'tur_effect_perform|global_effect_handler_chain|EffectHandlerFrame|
tur_handler_dispatch|tur_effect_cont_resume|TurEffectCaptureCtx' == 0` for EVERY
fixture. Suite 2203/0; effect fixtures run correctly and are ASan-clean apart from
a pre-existing owning handler-VALUE table leak (`tur_handler_table_new` in a
`defstruct`-field handler is never freed -- the owning-value teardown gap, tracked
in `docs/reported/cps-owning-adt-value-not-dropped-under-match.md`, NOT introduced
by this deletion). The obsolete `fiber-cross-resume` fixture (inline-C that
manufactured a `TurContK` to test the deleted cross-fiber guard) was removed.

**Remaining (optional tidy, not blocking):** `emit_effects.c` still carries the
now-unreachable fiber emit branches (`emit_effects_perform`, `emit_effects_handle`,
and the fiber arms of `emit_effects_resume`/`with_handler`) as dead code -- they
compile (string literals) and are never reached (corpus-verified), so they are a
hygiene cleanup, not a correctness item. The CPS/DK backend is now the **sole
effect lowering**; the fiber effect runtime is gone.

## 4. Status of the reports this plan supersedes

Resolved and archived 2026-07-18 (verified by the Sec 0 measure): the
effect-subtype cluster (`cps-e2-pure-lambda-into-effectful-fnvalue-param`), the
while-loop interior-handle residue
(`cps-while-native-conservative-subset-fiber-residue`,
`cps-while-loop-with-interior-handle-no-native-lowering`), the effect
pass-through soundness bug (`handle-body-passthrough-effect-unhandled`, now
prints `12`), the async recursive-await by-design note
(`cps-async-recursive-await-eviction`), and the permanently-fiber-bound session
note (`cps-session-effects-permanently-fiber-bound`, now B8 here).

Still open in `docs/reported/`: `cps-toplevel-synthesized-main-bypasses-dk`
(B1 + B2), `cps-effectful-fnvalue-call-under-handle-installing-hof` +
`effectful-fnvalue-param-miscompile` (B4), and three non-fiber-liveness residuals
that are correctness/hygiene, not blockers for this deletion:
`cps-reopen-perform-onode-leak` (O(N) DK-node leak, flag-on only),
`cps-drop-elided-under-delimited-control` (heap leak), and
`cps-named-receiver-uniform-fnptr-cast` (low-severity fn-ptr-cast UB). The
`uncons-hyfmap-cps-direct-emits-unmangled-tcons` warning is a live but separate
codegen defect.
