---
title: Who Frees What in Emitted Code
category: Compiler Internals
description: How the direct-style C the compiler emits releases memory -- the ownership model and move tracking, drop glue, the by-value recursive spine drops (scope exit, lend, own-discharge), scope-exit frees and the shapes they decline, closure environments, carrier and element boxes, the rc<T> surface, and the workflow for confirming and pinning a leak
---

# Who Frees What in Emitted Code (internals)

This guide is for contributors who change how the compiler allocates or
releases memory in **direct-style** emitted C, and for anyone reading a
LeakSanitizer report against a `tur build` binary. It describes the code as it
stands.

Out of scope, with where to go instead:

- CPS-lowered functions and the DK machine (the reap list, early hand-backs,
  continuation copies): [cps-dk-internals-guide.md](cps-dk-internals-guide.md#memory-who-frees-what).
- Regions (`with-region`, the escape note, rewind vs retire):
  [regions-internals-guide.md](regions-internals-guide.md) and
  [gc-guide.md](gc-guide.md).
- The rc runtime and the cycle collector themselves: [gc-guide.md](gc-guide.md).
- Which ownership strategy a library should pick:
  [ownership-guide.md](ownership-guide.md).
- What each feature costs in bytes: [memory-usage-guide.md](memory-usage-guide.md).
- The interpreter, which reclaims by pool and sweep, not by drop:
  [turi-interpreter-internals-guide.md](turi-interpreter-internals-guide.md).

> **Trust the code over the header comments.** Comments in this area name the
> report that introduced each rule, and many still describe the state just
> before a later fix (an archived report calls the spine drop
> `drop_recspine_<T>`; the emitted name is `drop_localowned_<T>`). Where a
> comment and this guide disagree, read the code. If the code disagrees with
> this guide, fix the guide.

---

## The short version

1. **There is no general "drop at scope exit" pass.** Each heap shape has its
   own owner rule, decided either by the elaborator (which injects a `defer`
   into the `let` body) or by the emitter (which writes a trailing free after
   the `let` body). Anything no rule claims leaks.
2. **Every rule's polarity is "prove it, or leak".** A wrong "owned" is a
   use-after-free or double free; a missed one is a leak. So every predicate
   only ever greenlights a free, and refusal is the default.
3. **Drop glue** (`drop_glue_tur_adt_<T>`) exists only for types that
   `needs_drop_glue`. Having it makes the type move-only, and that move
   discipline is what makes a single owner -- and so a free -- sound.
4. **By-value recursive spines** are freed at three points: the owning local's
   scope exit, right after a lend of a fresh temporary, and inside a consuming
   callee (*own-discharge*, shallow or deep). A whole-program provenance
   analysis decides which values own their spine.
5. **Trailing frees are trailing.** A `return`, `throw` or panic that leaves
   the scope early skips them -- and, today, skips the elaborator's injected
   drops too (see [Early exits](#early-exits)). The tail path is handled
   separately ([The tail path](#the-tail-path)).
6. **Fixture programs are not leak-checked** by `tests/run.sh`. Opt a fixture
   into `tests/run-leak-check.sh` with a `requires.leak-check` marker.

---

## Source map

| File | Role |
|---|---|
| `src/compiler/elab_structs.c` | Sets `AdtDef.needs_drop_glue` and `CtorField.drop_inner_def` per field (`:copy` / `:heap` exclusions) |
| `src/compiler/elab_forms.c` | `elab_let`'s auto-drop passes: injected `defer`s for `ref`/`rc`/Drop-opaque locals and rc/ref fields; the `drops_fn_fields` and `drops_local_owned` flags. Predicates `elab_byval_drop_adt`, `elab_byval_localowned_adt`, `localowned_binding_is_confined`, `elab_own_byval_copy` |
| `src/compiler/elab_core.c` | Move tracking: `binding_mark_moved`, `binding_mark_lent`, `is_binding_consumed`, `is_field_consumed` |
| `src/compiler/elab_call.c` | Lends at call sites (`binding_mark_lent`); `hoist_borrowed_closure_args` (the `__borrowc_N` let) |
| `src/compiler/elab_fns.c` | Infers the per-function ownership facts: `nonretain_param_mask`, `nonretain_ptr_param_mask`, `nonretain_sum_param_mask`, `resalias_param_mask`, `returns_fresh_closure`, `returns_fresh_any` |
| `src/compiler/emit_core.c` | Ownership provenance (`own_compute`, `emit_own_binding_owned`, `emit_own_expr_owned`) and own-discharge (`emit_own_match_discharges`, `emit_own_arm_field_discharge`, `emit_own_arm_discharges`); closure-env drop predicates (`closure_env_drop_is_shallow`, `closure_env_drop_frees_box_only`) |
| `src/compiler/emit_module.c` | Writes the glue: `emit_adt_byval_drop_glue` (`drop_glue_`, `drop_localowned_`, `drop_fnfields_`, `walk_glue_`), `emit_adt_glue_field_drops`; the preamble's `tur_closure_drop` / `TUR_CLOSURE_DROP` and `__tur_any_drop` |
| `src/compiler/emit_expr.c` | `emit_let_value` and every `let_binding_*_freeable` predicate; closure env allocation and env drop glue (`emit_closure_env_struct_and_glue`); `emit_carrier_sum_free`, `emit_boxed_struct_payload_free`, the pending-drain queues; the `tur-wide-byval?` / `tur-vec-elem-wide?` folds |
| `src/compiler/emit_fns.c` | The env drop glue twin; the tail path: `tco_let_refusal`, `tco_drop_glue_refusal`, `let_binding_push_scope_frees` callers |
| `src/compiler/types.c` | `type_is_boxed_container_elem`, `region_free_fn` |
| `src/passes/rc_elision.c` | Pairs a last-use `rc/clone` with a matching `rc/drop` and elides both |
| `src/runtime/rc.c`, `src/runtime/region.c` | `rc_strong_decrement`, `rc_free_queue_drain`; `tur_region_free` |
| `tests/run-leak-check.sh` | The opt-in leak gate (`requires.leak-check`, `known-leak`) |

---

## The ownership model

The emitter has no ownership type. It works from four facts the elaborator
leaves on each `Binding` and function, plus one analysis of its own.

### Owned, lent, borrowed

| A value is ... | When | Who frees |
|---|---|---|
| **owned** by a local | it was constructed, returned by a call, or moved in; no use moved it on | the local's scope exit |
| **moved** | a use handed it on for real: a retaining call, a `let` rebinding, a `return`, a constructor argument | the new owner |
| **lent** | every poisoning use passed it to a callee proven not to retain it | still the local's scope exit |
| **borrowed** (a `^borrow` parameter, a global, a container element, a field read) | the value is a view of something another owner holds | never this code |

Function results are owned by the caller. That is a convention the elaborator
enforces where it can: `elab_own_byval_copy` clones the rc fields of a
by-value product copied out of a shared view (a `^borrow`, a global, a
container element, or a match binder of one) where it becomes an owner, and
rejects one holding a borrowed `ref` field with TUR-E0108.

### How moves are tracked

`Binding` carries the state (`src/compiler/expr.h`):

- `is_moved` -- the static poison the move checker reads (TUR-E0201 on a later
  use). Set by both of the next two.
- `moved_owning` -- set by `binding_mark_moved`. **Sticky**: it is never rolled
  back with the per-branch move-state snapshots, so a lend in one arm and a
  real move in the other reads as moved.
- `lent_to_nonretaining` -- set by `binding_mark_lent` when the callee's mask
  says it keeps nothing. The binding is poisoned exactly as by a move, but the
  scope-exit drop still fires.
- `is_binding_consumed(body, b)` -- not a flag but a walk: an explicit
  `(rc/drop b)`, `(drop! b)` or `(ref/from-rc b)` **anywhere** in the body.
  It suppresses the auto-drop on every path, including paths that do not
  consume (see [Declined shapes](#declined-shapes)).
- `binding_moved_during_init[k]` -- a local moved by a later sibling
  initializer of the same `let`.

The drop decisions read these; the type checker reads only `is_moved`.

### Parameters: `^borrow` and the inferred masks

A `^borrow` parameter (`Binding.is_borrow`) is never owned by the callee:
the callee frees nothing it is handed. For an unannotated parameter the
elaborator infers, per function, whether the body can keep it. The answer is a
bitmask on the function's binding:

| Mask | Parameter kind | Lets the caller ... |
|---|---|---|
| `nonretain_param_mask` | fn-typed / `^fat` | free a fresh closure env it passed (the `__borrowc` hoist) |
| `nonretain_ptr_param_mask` | `cstr`, `ptr<void>`, by-value recursive ADT | free a caught-Result payload, or keep a lent spine |
| `nonretain_sum_param_mask` | stdlib `Option` / `Result` | free a fresh sum argument's payload box after the call |
| `resalias_param_mask` | `^borrow` recursive ADT kept only in the result | track the result as an alias of the source |

Bits past 31 are left unset (no free). A body with inline C gets no bits in
the sum mask: a C body can stash the pointer.

### Ownership provenance (by-value recursive ADTs)

For the spine-owning types (below), the elaborator's flags are not enough: a
by-value copy out of a `^borrow`, a global or `vec-get` shares its boxes with
an owner that will free them. So `emit_core.c:own_compute` runs a
whole-program **greatest fixed point** before either spine consumer frees
anything. A value is owned when it is:

- a constructor call whose recursive-type arguments are owned;
- a let-local or match binder proven owned (binders inherit from an owned
  scrutinee), or a parameter every caller passes owned (`OwnFn.owned`);
- the result of a direct call to a function whose every returned value is
  owned (`OwnFn.fresh`);
- an `if` / `match` / `do` / `let` whose value positions are all owned.

Everything starts assumed. A function is knocked out of `owned` when its
address is taken (`EX_VAR` of the function), and never gets it when it is
external: an instance method, a C export, a `#[used]` defn, a lifted lambda,
`main`, or an exported function under separate compilation. If the fixed point
does not settle in 64 rounds, nothing is claimed. The walk enumerates operands
through `cps_visit_children`, so a new expression kind is not silently
skipped.

The design history, including the use-after-free that made the analysis
necessary, is in
[byvalue-recursive-adt-boxes-are-never-freed](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/byvalue-recursive-adt-boxes-are-never-freed.md).

---

## Two families of release

Every scope-exit release is one of two kinds, and they behave differently on
an early exit.

**Elaborator-injected `defer`s.** `elab_let` (`elab_forms.c`) appends
`EX_DEFER` items, flagged `is_drop_glue`, to the end of the `let` body's
`do`:

| Local | Injected |
|---|---|
| `ref<T>` (and a `weak/upgrade` box) | `(defer (drop! b))` |
| `rc<T>`, constrained existential | `(defer (rc/drop b))` |
| move-only Drop opaque (`Handler`) | `(defer (<Drop.drop> b))` |
| single-constructor by-value product with `rc` / `ref` fields | `(defer (rc/drop (.f o)))` / `(defer (drop! (.f o)))` per field |

Each is skipped when the local is moved, moved during init, or consumed.

**Emitter trailing frees.** `emit_expr.c:emit_let_value` collects, per
binding, the frees that need the binding's *emitted* C type to decide, and
writes them after the body, in this order:

| Predicate | Emitted |
|---|---|
| `let_binding_env_freeable` | `TUR_CLOSURE_DROP(f);` |
| `let_binding_mut_cell_freeable` | `tur_region_free((void *)(intptr_t)(cell));` |
| `let_binding_box_freeable` | `tur_result_box_free(...)` (a caught Result box) |
| `let_binding_sum_box_freeable` | `emit_carrier_sum_free` (an erased Option/Result cell) |
| `let_binding_vsp_box_freeable` | `emit_boxed_struct_payload_free` (a by-value Option/Result's payload box) |
| `let_binding_sum_closure_freeable` | `sum_closure_payload_walk` (a closure in an Option/Result) |
| `let_binding_any_freeable` | `__tur_any_drop(x);` |
| `Binding.drops_fn_fields` | `drop_fnfields_tur_adt_<T>((void *)&o);` |
| `Binding.drops_local_owned` + provenance | `drop_localowned_tur_adt_<T>((void *)&xs);` (reverse binding order) |

The last two flags are set by the elaborator but emitted here, not as
`defer`s, so that a CPS-colored function stays admissible: a `defer` reading a
fat-fn field is a continuation-capture shape the CPS backend refuses.
CPS-lowered functions never run `emit_let_value`, so these frees do not
happen there.

---

## Drop glue

### What makes a type need it

`AdtDef.needs_drop_glue` is set in `elab_structs.c` when some constructor
field is:

| Field | Condition |
|---|---|
| `rc<T>`, `ref<T>`, `weak<T>` | always |
| boxed fn (`(fn [...] R)`, arity 0..4) | always (the field owns a fat handle) |
| a nested by-value product that itself needs drop glue | non-parametric, non-`:heap` inner (`drop_inner_def = inner`) |
| a direct self-reference (`tl : Lst`) | type not `:copy`, not `:heap` (`drop_inner_def = def`) |
| `any` | type not `:copy`, not `:heap` |

`needs_drop_glue` makes the type **move-only**. That is the soundness line
for `:copy`: under `:copy`, `(let [t (Cons 3 (Nil)) a (Cons 1 t) b (Cons 2
t)] ...)` compiles and two boxes carry the same tail pointer, so a per-spine
free would free it twice. `:copy` recursive types therefore get no glue and
leak per value; the answer for them is a region bracket.

### What gets emitted

`emit_module.c:emit_adt_byval_drop_glue` writes, for each such type:

| Function | Releases | Frees the node? |
|---|---|---|
| `drop_glue_tur_adt_<T>(void *)` | owning fields of the live variant | yes, `region_free_fn()` |
| `drop_localowned_tur_adt_<T>(void *)` | the same fields (recursive / `any` types only) | no -- the node is a stack local |
| `drop_fnfields_tur_adt_<T>(void *)` | boxed fn fields only (single-variant types) | no |
| `walk_glue_tur_adt_<T>(...)` | reports `rc` children to the cycle collector | -- |

Per field, `emit_adt_glue_field_drops` releases in reverse field order:

| Field | Release |
|---|---|
| `rc` | `rc_strong_decrement(f); rc_free_queue_drain();` |
| `weak` | `rc_weak_decrement(f);` |
| `ref` | `free(f);` |
| `drop_inner_def` set | `drop_glue_tur_adt_<Inner>(f);` (recursion for a spine) |
| `any` | `__tur_any_drop(f);` |
| boxed fn | `free(f);` -- see the warning below |

A multi-constructor type switches on `s->tag` and releases only the live
variant's fields. For `(defdata Lst [] (Cons [hd : int tl : Lst]) (Nil))`
the glue is (from `tur emit-c`, trimmed):

```c
static void drop_glue_tur_adt_Lst(void *ptr) {
    if (!ptr) return;
    tur_adt_Lst *s = (tur_adt_Lst *)ptr;
    switch (s->tag) {
    case 0:
    if (s->as.Cons._1) drop_glue_tur_adt_Lst((void *)(intptr_t)s->as.Cons._1);
        break;
    case 1:
        break;
    }
    tur_region_free(ptr);
}
```

> **The boxed-fn arm.** A capturing closure is allocated with an 8-byte header
> and its handle points *past* it (see
> [Closure environments](#closure-environments)), so a fn field is released
> with `TUR_CLOSURE_DROP`, never a bare `free` -- in `drop_fnfields_<T>` and in
> `emit_adt_glue_field_drops` alike. The latter emitted a bare `free` until
> 2026-10-17, so `rc/of` of such a struct, or a recursive ADT with a fn field
> going out of scope, aborted with `free(): invalid pointer`
> ([adt-fn-field-drop-glue-bare-free](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/adt-fn-field-drop-glue-bare-free.md);
> pinned by `closure-field-drop-glue-rc-struct` and
> `closure-field-drop-glue-defdata`).

### Regions vs drop glue

A node is allocated with `tur_region_alloc_or_malloc` and released with
`region_free_fn()`, which is `tur_region_free` by default and `free` under
`TUR_REGIONS=0`. `tur_region_free` refuses region memory (a process-wide
registry says who owns a word), so drop glue is safe to run on a node built
inside a bracket. A `:heap` node has no glue at all: outside a region it lives
until exit; inside one it goes at the rewind. See
[regions-internals-guide.md](regions-internals-guide.md).

---

## By-value recursive spines

A by-value recursive ADT (or one with an `any` field) is a stack value whose
recursive field holds a pointer to a heap copy of the next link: a by-value
product cannot ride the int64 carrier and cannot contain itself inline. The
types in scope are exactly `emit_own_adt_of` / `elab_byval_localowned_adt`:
by-value, non-generic, `needs_drop_glue`, with a self-recursive or `any`
field. Their spine is freed at one of three points.

### 1. Scope exit of the owning local

`elab_let` sets `Binding.drops_local_owned` when the local:

- was not moved for real (`is_moved && !(lent_to_nonretaining && !moved_owning)`),
- was not moved during init and is not consumed, and
- is **confined**: `localowned_binding_is_confined` walks every match binder
  and field read rooted at the local as an alias, and refuses if any alias
  reaches a store, a closure, a `let`, a retaining callee, or the scope's
  value (unless that value is a non-pointer scalar).

`emit_let_value` then also asks `emit_own_binding_owned`. Both must agree.

### 2. A lent temporary

`(llen (build 3))` hands a fresh spine straight to a non-retaining parameter.
No local names it, so `emit_core.c:emit_call_arg_temp_is_lent_fresh_spine`
spills it to a temp and queues it on the `vsp_pending` channel, drained after
the call as `drop_localowned_tur_adt_<T>((void *)&__tN);  /* lent temporary:
nothing else owns it */`. It requires a static, non-tail call, a non-pointer
scalar result, and an owned argument.

### 3. Own-discharge in a consuming callee

A callee that returns part of its argument cannot be non-retaining, so the
caller moves the spine into it for real. The callee then frees what it did
not pass on. `emit_own_match_discharges` admits a `match` when:

- the scrutinee is a parameter, and `emit_own_param_owned` says every caller
  passes it owned;
- the match is on the body's straight-line spine (`own_on_spine`: evaluated
  once per call, as the body's value);
- the parameter is used exactly once (`own_uses == 1`);
- the match is **not** being emitted on the tail path (`emit_match_in_tail`);
- `TUR_NO_OWN_DISCHARGE` is unset in the compiler's environment.

Then, after each arm computes its value, `emit_own_arm_discharges` decides per
recursive field (`emit_own_arm_field_discharge`):

| Mode | When | Emitted | Why it is safe |
|---|---|---|---|
| **SHALLOW** | the binder is the arm's value, or is moved exactly once into a constructor or a consuming callee (`own_moved_once`) | `tur_region_free(__scrut->...);` | the contents were copied on; only the box that held them is garbage |
| **DEEP** | the binder is unused, or every use is confined (the strict alias walk) | `drop_glue_tur_adt_<T>(__scrut->...);` | the whole sub-spine is garbage |
| none | a guarded arm, a wildcard / var / literal pattern, a move into a **tail** call, anything the walk cannot place | nothing | a leak |

The parameter's own storage is the caller's (a by-value ADT argument is
passed as `const T *`) and is never touched. For `(defn tail [xs : Lst] : Lst
(match xs (Cons h t) t (Nil) (Nil)))` the Cons arm emits (trimmed):

```c
case 0: {
    const tur_adt_Lst *t_11 = (const tur_adt_Lst *)(intptr_t)(__scrut->as.Cons._1);
    __t202 = *(t_11);
    tur_region_free((void *)(intptr_t)__scrut->as.Cons._1);  /* moved out: free its box */
    break;
}
```

A move into a tail call is declined because a free after the call costs the
C compiler the jump a recursive walker relies on.

What still leaks -- a consuming callee reached through a borrow, and spines in
a container -- is
[byvalue-recursive-shared-copies-leak](https://github.com/turmeric-lang/turmeric/blob/main/docs/reported/byvalue-recursive-shared-copies-leak.md).

---

## Scope-exit frees in detail

### Early exits

`emit_let_value` checks `expr_contains_return_or_throw(body)` first. When the
body can leave early, it collects **no** trailing frees except `any` drops,
which it also pushes on the `any_scope_drops` channel so every `return` inside
the body fires them. Every other trailing free is a leak on that path, never a
use-after-free. The emitted `if (tur_panicking) return ...;` after each call
skips them as well.

The injected `defer`s are covered on a `return`. `emit_do_value` registers a
defer with `tur_frame_push_defer` *when control reaches it*, and the
auto-drops (`is_drop_glue`) sit after the body's value, so the frame chain an
early `return` fires does not hold them yet. Each do-emitter (`emit_do_value`,
`emit_stmt`'s `EX_DO` arm, `emit_tail_drop_hoist_do`) therefore notes its
trailing drop-glue run against its frame (`emit_frame_note_pending_drops`)
while it emits the items ahead of it, and clears the note once it pushes them.
The `return` emitter computes its value first, fires the frame chain, then
emits every noted drop along that chain inline, innermost first
(`emit_frame_run_pending_drops`) -- except a binding the value moves out, so
`(return x)` on an owned `rc` still hands it over. The note is keyed by frame,
so a lambda body, which starts with no frame, never sees its enclosing
function's. Pinned by `ref-auto-drop-early-return` (leak-checked) and
`rc-auto-drop-early-return` (`gc-live-blocks` around the calls).

### The tail path

A `let` on the tail path of a function with a backedge is emitted by
`emit_tail`'s inline arm, which has no trailing code. `tco_let_refusal`
(`emit_fns.c`) admits a binding that `let_binding_may_need_scope_free` only
when every use of it is a plain scalar read (`tco_drop_use_ok`); its release
is then pushed onto the `any` drop channel (`let_binding_push_scope_frees`)
and fires at the backedge or `return`, after the arguments are in temps.
Otherwise the `let` leaves the tail grammar (`TC_LET_DROP`).

A body with injected drop-glue defers keeps a tail position only for a
*backedge*, and only when `tco_drop_glue_refusal` finds the captured locals
dead at the call (`TC_DROP_LIVE`) and no `return` sharing the block
(`TC_DROP_EXIT`). A non-self call under an open frame is not a C tail call.

### Declined shapes

These are deliberate leaks, not oversights. Each is a place where the free
could be wrong.

| Shape | Declined by | Leak |
|---|---|---|
| early `return` / `throw` / panic out of the `let` body | `emit_let_value`, defer timing | every free of the scope that is neither an `any` drop nor, on a `return`, an injected drop-glue defer |
| `(rc/drop b)` in one branch only | `is_binding_consumed` | `b` on the other branch |
| `(set! h h)`, or `(rc/drop h)` then `(set! h v)` | `elab_set_rc_release` | see [ownership-guide.md](ownership-guide.md#reassigning-an-rct-binding-with-set) |
| a closure returning a pointer or struct, or a sum with inline C in its body | `let_binding_env_freeable` | the env |
| a closure that escapes (returned, stored, captured, passed to a retaining param) | `closure_binding_escapes` | the env |
| a spine copied out of a shared view | provenance | the copy's boxes stay the source's |
| a `match` arm with a guard, or emitted on the tail path | own-discharge | the moved-from box |
| a `:copy` recursive ADT | no glue by design | every link (use `with-region`) |
| a `:heap` node outside a region | no glue by design | every node |
| a `weak<T>` local | no auto-drop | its weak count (`weak/drop` it) |

---

## Closure environments

### Layout

Every capturing closure is a heap block with an 8-byte drop-glue header
(`emit_expr.c`, the `EX_CLOSURE` arm):

```c
void *__t204 = malloc(sizeof(void *) + sizeof(struct __env_12));
*(void (**)(void *))__t204 = drop_glue___env_12;
struct __env_12 *__t203 = (struct __env_12 *)((char *)__t204 + sizeof(void *));
__t203->__fn = (tur_thunk_int64_t_int64_t_t)__fn_10;
__t203->k = k;
```

The handle is the pointer past the header, so dispatch (`TUR_CLOSURE_FN`,
`TUR_APPLYn`) reads `__fn` at offset 0. **Never `free` a closure handle.**
Release it with `TUR_CLOSURE_DROP(h)`, i.e. `tur_closure_drop`, which reads
`env[-1]` and calls the glue (or frees the base when the slot is NULL).

`drop_glue_<env>` is emitted twice in lockstep -- `emit_fns.c` (the
authoritative pre-pass) and `emit_expr.c:emit_closure_env_struct_and_glue` --
and releases, in reverse capture order:

- an `rc` capture: `rc_strong_decrement` + drain (the env fill retained it);
- an owned `^fat` capture: `TUR_CLOSURE_DROP` (the capture was moved in),
  except a letrec self-capture;
- a capture whose type has a `Drop` instance: that instance's `drop`;

then frees the base. Other owning captures (a raw nested closure, a `ref`) are
**not** walked: recursing blind would double-free a capture another owner also
drops.

### When an env is freed

| Owner | Rule |
|---|---|
| a `let`-bound closure | `let_binding_env_freeable`: a closure literal (or a `returns_fresh_closure` call, or a partial-application `__pap` env) whose result is a scalar -- or a sum / product / `cstr` when its body has no inline C -- and that does not escape the body or a sibling init |
| an argument to a non-retaining fn parameter | `hoist_borrowed_closure_args` (`elab_call.c`) binds it to a `__borrowc_N` let, so the rule above applies |
| a confined `letrec` group | `letrec_members_confined`: each member only calls the others |
| an `async` body bound to a local and spawned once | the spawn (`spawn_owns_env`), when the body settles |
| a boxed fn field of a by-value struct local | `drop_fnfields_<T>` at scope exit (`drops_fn_fields`) |
| a closure inside an Option/Result local | `sum_closure_payload_walk`, in the simplest shapes ([report](https://github.com/turmeric-lang/turmeric/blob/main/docs/reported/sum-closure-payload-never-dropped.md)) |
| a closure widened to `any` by a `returns_fresh_any_closure` producer | `__tur_any_closure_drop` |

The `{shim, orig}` fat box a bare named `defn` is wrapped in is allocated once
at file scope; per-call shim boxes are the subject of
[fn-param-at-type-variable-slot-boxed-per-call](https://github.com/turmeric-lang/turmeric/blob/main/docs/reported/fn-param-at-type-variable-slot-boxed-per-call.md).
Which fn representation crosses which boundary is in
[value-representations-guide.md](value-representations-guide.md#representations-fn-typed-values).
The design history is
[closure-drop-glue-plan](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/closure-drop-glue-plan.md).

---

## Boxes

| Box | Allocated by | Owner | Freed by |
|---|---|---|---|
| **erased Option/Result cell** (`int64_t` carrier; None is NULL) | a sum constructor at the carrier ABI, `tur_box_*` | the local or temp it was minted for, when the producer is fresh (`emit_init_owns_fresh_sum`) and every use is an accessor | `emit_carrier_sum_free` at scope exit, or the `sum_pending` drain after a consuming call |
| **inline-C Option/Result box** | `tur_some_ptr` / `tur_ok_ptr` / `tur_box_*` in a body whose **declared** result is `(Option T)` / `(Result T E)` | the caller | the readback bridge (`emit_core.c`, `emit_owned_carrier_is`), or the pending drain |
| **value-struct payload box** in a by-value Option/Result monomorph | the spec constructor | the sum value | `emit_boxed_struct_payload_free` (scope exit, or at the top of the answering `match` arm) |
| **`any` payload box** | the widen, when the registry row says `boxed` | the scope that holds the `any` | `__tur_any_drop`, which also calls the row's `drop` (a `drop_localowned_<T>`) |
| **container element box** (`type_is_boxed_container_elem`) | the push / insert bridge | the container | `vec-free` / `map-free`, told by the `tur-vec-elem-wide?` / `tur-wide-byval?` folds |
| **`^mut` cell** (`TurMutCell`) | the cell constructor | the `let` | scope exit, once every capturing closure is dead |
| **caught Result box** | `catch-unwind` | the `let` | `tur_result_box_free` (not the ok payload, which `ok-val` may have copied out) |
| **CPS slot box** | the DK lowering | the reap list | see the CPS guide |

Three rules carry most of the weight:

1. **A declared Option/Result result from inline C is a transfer.** The body
   must return a fresh box. A borrowed box needs a borrow-shaped signature:
   `vec-get [A] (v : (Vec A)) : A` returns an element box the vector owns, and
   `(:: (vec-get v 0) (Option int))` is still a borrow. Keying on the call's
   resolved type instead of the declaration is a double free. See
   [inline-c-results-guide.md](inline-c-results-guide.md#who-owns-the-box).
2. **Boxing and freeing ask one predicate.** The element push bridge, the
   read-back, and the ownership folds all call
   `type_is_boxed_container_elem` (itself `repr_of`'s answer), so they cannot
   drift. When they did, `(Vec (Option int))` leaked a box per push.
3. **`__tur_any_drop` reads the minting TU's registry row**, not a local
   switch, so it frees only boxes a widen allocated -- across modules too.

---

## `rc<T>` and `weak<T>`, from the emitter's side

The mechanics are in [gc-guide.md](gc-guide.md); this is what the emitter
writes and what to watch for.

- `rc/of` allocates the payload and a control block (`rc_cb_alloc`, or
  `rc_cb_alloc_struct` with the type's `drop_glue_` and `walk_glue_` for a
  struct with owning fields).
- `rc/drop` is `rc_strong_decrement(cb); rc_free_queue_drain();`.
  `rc/clone` is `rc_strong_increment(cb);`. `src/passes/rc_elision.c` marks a
  last-use clone and its matching drop `elide`; the emitter then writes a
  comment instead of either call.
- A local `rc<T>` gets the injected `(defer (rc/drop b))`; a `set!` on it
  releases the overwritten value (`elab_set_rc_release`), gated on exactly the
  auto-drop predicate.
- A `let` initialized from a bare rc **field read** is wrapped in
  `EX_RC_CLONE`, because the source still owns that field.
- A `weak<T>` in a struct field is released by the owner's glue; in a local it
  is not.

**LeakSanitizer cannot see an rc leak.** Every control block is reachable
from the collector's registry, so a leaked rc block reads as live. Count
instead: `(gc-live-blocks)` before and after the shape.

---

## Workflow: confirm and pin a leak

### 1. Reproduce it outside the suite

`tests/run.sh` builds fixture programs without sanitizers and runs them with
`detect_leaks=0` ([test-suite-portability-guide.md](test-suite-portability-guide.md#7a-leak-checking----what-is-covered-and-what-is-not)).
Build one program the way `tests/run-leak-check.sh` does:

```sh
TUR_RUNTIME=source \
TUR_CC_FLAGS="-O1 -g -std=c99 -Wall -fno-strict-aliasing -fsanitize=address -L./build/src" \
  ./build/tur build repro.tur -o /tmp/repro
ASAN_OPTIONS="detect_leaks=1:exitcode=23" \
LSAN_OPTIONS="use_stacks=0:use_registers=0" /tmp/repro
```

Every flag is there for a reason the harness records:

- `TUR_CC_FLAGS` **replaces** the default flags, so the `-L` must be restated.
- `-O1`, not `-O2`: `-O2` inlines the allocation site out of the trace.
- `TUR_RUNTIME=source` compiles the runtime into the program. The prebuilt
  archive is not instrumented, so a use-after-free inside `rc_strong_decrement`
  passed the gate until this was added.
- `use_stacks=0:use_registers=0`: a stale pointer in a dead frame keeps a
  leaked block "reachable". The default roots hid 127 bytes across one sweep.
- `exitcode=23` separates a leak from the program's own failure exit.

### 2. Read the report against the emitted C

The frames name C functions in `/tmp/tur-build/<mangled-path>.c`, at line
numbers in that file. `tur emit-c repro.tur > repro.c` produces the same text,
so read the allocation site there. The allocator in the top frame says what
leaked:

| Top frame / allocation | What it is |
|---|---|
| `tur_region_alloc_or_malloc` beside `*__tN = (...)` | a recursive-field box or a `:heap` node |
| `malloc(sizeof(void *) + sizeof(struct __env_N))` | a closure env |
| `tur_box_*`, `tur_some_ptr`, a `some__spec__...` | an Option/Result box |
| `rc_cb_alloc*` | an rc control block -- but see the rc note above |
| `malloc` in a spec constructor beside a monomorph | a value-struct payload box |

"Direct leak" is the first unowned allocation; "Indirect leak" is what hangs
off it. Fix the direct one first.

### 3. A/B the mechanism

- `TUR_NO_OWN_DISCHARGE=1 ./build/tur build ...` turns own-discharge off (read
  at compile time, in `emit_own_match_discharges`). On the `tail` example
  above it turns a clean run into a 24-byte, one-allocation leak.
- `TUR_REGIONS=0` restores plain `malloc`/`free` for nodes. In a build tree
  with no `libturi.a` (the runtime is then compiled from source), it currently
  fails to link: `hamt.c` calls `tur_region_note_escape`, and
  `emit_region_runtime_bodies` embeds `region.c` only when regions are on.
  Build all targets first, or A/B with the archive.

### 4. Pin it

Add a fixture with a `requires.leak-check` marker; `tests/run-leak-check.sh`
then requires both a clean LSan run and `expected.stdout`. If the leak is real
and filed but not fixed, add a `known-leak` file whose first line names the
open report: the gate prints `KNOWN` instead of `FAIL`. When the fix lands,
the gate **fails** while the marker remains ("marked known-leak but ran
clean"), so delete it in the same change. Four fixtures carry one today.

A clean run means "nothing was orphaned", not "nothing was retained". For
retention -- rc blocks, DK reap entries -- count it.

---

## Open leak reports, by mechanism

Check [`docs/reported/`](https://github.com/turmeric-lang/turmeric/blob/main/docs/reported/README.md) for the current list.

**By-value recursive spines**

- [byvalue-recursive-shared-copies-leak](https://github.com/turmeric-lang/turmeric/blob/main/docs/reported/byvalue-recursive-shared-copies-leak.md) --
  a spine reaching a consuming callee through a borrow, or held in a container.

**Option/Result boxes**

- [carrier-sum-option-boxes-have-no-owner](https://github.com/turmeric-lang/turmeric/blob/main/docs/reported/carrier-sum-option-boxes-have-no-owner.md) --
  carrier cells behind consumers the compiler cannot see through (dictionary
  dispatch, HKT-constrained generics).
- [sum-closure-payload-never-dropped](https://github.com/turmeric-lang/turmeric/blob/main/docs/reported/sum-closure-payload-never-dropped.md) --
  a closure in an Option/Result local beyond the simplest shapes.

**Closure envs and fn boxes**

- [async-capturing-body-env-never-freed](https://github.com/turmeric-lang/turmeric/blob/main/docs/reported/async-capturing-body-env-never-freed.md) --
  a capturing `async` body's env in the shapes the spawn cannot own.
- [fn-param-at-type-variable-slot-boxed-per-call](https://github.com/turmeric-lang/turmeric/blob/main/docs/reported/fn-param-at-type-variable-slot-boxed-per-call.md) --
  a fn parameter boxed into a type-variable slot per call.

**Structs on the carrier ABI**

- [cps-byvalue-struct-with-closure-field](https://github.com/turmeric-lang/turmeric/blob/main/docs/reported/cps-byvalue-struct-with-closure-field.md) --
  a `defstruct` with a `defopaque` field beside a closure field stays on the
  carrier ABI, and boxes per construction, until the CPS backend can take such
  a struct by value. (Without a closure field it is by value and leaks nothing:
  `opaque-field-struct-box-never-freed`, archived.)

**Continuations (DK)**

- [effect-nontail-resume-copies-held-until-outer-entry](https://github.com/turmeric-lang/turmeric/blob/main/docs/reported/effect-nontail-resume-copies-held-until-outer-entry.md) --
  retention, not a leak: copies held until the outermost entry returns.
- [cloneable-cont-escaped-chain-leaks](https://github.com/turmeric-lang/turmeric/blob/main/docs/reported/cloneable-cont-escaped-chain-leaks.md) --
  an escaped cloneable continuation's wrapper and chain.

**Dynamic dialects**

- [saffron-static-ownership-residue](https://github.com/turmeric-lang/turmeric/blob/main/docs/reported/saffron-static-ownership-residue.md) --
  `any` boxes no static owner reaches, with the Saffron collector off.

---

## Reading the archive

Each rule above was added by one report. The ones that explain *why* a rule
has the shape it has:

- [byvalue-recursive-adt-boxes-are-never-freed](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/byvalue-recursive-adt-boxes-are-never-freed.md) --
  spine drops, the `:copy` line, lends, own-discharge, provenance.
- [closure-drop-glue-plan](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/closure-drop-glue-plan.md) -- the env
  header, `TUR_CLOSURE_DROP`, non-retaining fn params.
- [any-struct-box-leak-per-widen](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/any-struct-box-leak-per-widen.md)
  and [any-widen-stored-in-an-adt-field-has-no-owner](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/any-widen-stored-in-an-adt-field-has-no-owner.md) --
  `any` drops and the deep `__tur_any_drop`.
- [reclamation-plan](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/reclamation-plan.md) -- RM1, the
  accessor-whitelist sum-cell drop.
- [inline-c-option-carrier-box-leaks](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/inline-c-option-carrier-box-leaks.md) --
  the inline-C transfer contract.
- [value-struct-payload-sum-monomorph-box-has-no-owner](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/value-struct-payload-sum-monomorph-box-has-no-owner.md)
  and [sum-payload-box-leaks-at-match](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/sum-payload-box-leaks-at-match.md) --
  payload boxes.
- [mut-cell-is-never-freed](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/mut-cell-is-never-freed.md),
  [rc-of-sum-type-drops-no-glue](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/rc-of-sum-type-drops-no-glue.md),
  [history/drop-glue-shallow-nested-owning-aggregate](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/drop-glue-shallow-nested-owning-aggregate.md).

---

## Bugs found while writing this guide

Each was reproduced against the compiler this guide describes and filed in
[`docs/reported/`](https://github.com/turmeric-lang/turmeric/blob/main/docs/reported/README.md). When one is fixed and archived,
update the passage above that describes it and drop it from this list.

- [regions-off-source-runtime-link-failure](https://github.com/turmeric-lang/turmeric/blob/main/docs/reported/regions-off-source-runtime-link-failure.md) -- `TUR_REGIONS=0` fails to link when the runtime comes from source

## Maintenance

Update this guide in the same change when you:

- add a heap allocation site in emitted code -- it needs an owner rule here
  and a `requires.leak-check` fixture;
- add a `let_binding_*_freeable` predicate, an injected auto-drop, or a field
  kind to `emit_adt_glue_field_drops`;
- change what sets `needs_drop_glue`, `drop_inner_def`, or one of the
  ownership masks;
- change the closure env layout or `tur_closure_drop`;
- close or file a leak report listed above.

A new store into memory that outlives a region bracket also needs a
`TUR_REGION_NOTE` (CLAUDE.md, "Region Store Hooks"). The emitted-C excerpts
here come from `tur emit-c`; refresh them when the shape they show changes.
Cite functions, not line numbers.
