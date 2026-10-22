---
title: Regions (RM3) Internals
category: Compiler Internals
description: How region brackets work on the compiled path -- arena generations, rewind vs retire, the static walk and the runtime escape note, the store-hook rule and every place it is applied, the CPS and call/cc interactions, and the tools that tell a rewind from a retire
---

# Regions (RM3) internals

This guide is for contributors who touch anything that allocates a `:heap`
node, stores a word into memory, erases a type to `:int`, or brackets a call
with `with-region` / `bt-scope`. It is also the explanatory home for the
CLAUDE.md rule **"Region Store Hooks"**: CLAUDE.md keeps the rule, and this
guide says why it exists, what it covers today, and how to extend it.

The user-level picture (what a region is for, when to reach for one) is in
the [GC guide](gc-guide.md#what-is-not-gc-managed) and the
[memory usage guide](memory-usage-guide.md#4-bracket-allocation-heavy-phases-in-with-region).
The design history is the archived
[regions plan](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/regions-plan.md).

> **Trust the code over the comments.** There is no `--enable=regions` flag
> any more: regions are on unless `TUR_REGIONS=0` was set when `tur` ran, and
> the comments that said otherwise were corrected on 2026-10-21. Since then
> the embedded runtime sources (`region.h`, `arena.{h,c}`, `region.c`,
> `r7gc.c`) are pasted into emitted programs with their comments stripped, so
> a comment edit there no longer touches the `expected.c` snapshots or the
> split hash. What is still open in this area is listed in
> [stale-comments-regions](https://github.com/turmeric-lang/turmeric/blob/main/docs/reported/stale-comments-regions.md).

---

## The short version

1. **A generation is an `Arena`.** `tur_region_push` opens one on a
   per-thread stack. While it is the innermost open generation, every `:heap`
   ADT node the program constructs is bump-allocated in it
   (`tur_region_alloc_or_malloc`). Outside any generation the same call is
   `malloc`.
2. **A bracket is a call to `with-region` or `bt-scope`.** The emitter
   recognises the callee by name (`emit_binding_is_region_scope`) and wraps
   the call in a push and a pop. The stdlib bodies do nothing region-related.
3. **The pop either rewinds or retires.** A rewind resets the arena in one
   O(slabs) step and pools it for the next push. A retire keeps the arena
   mapped until process exit -- exactly what a build without regions does.
4. **A rewind needs two locks to agree.** The static walk
   (`region_type_reaches_node`) proves the bracket's result *type* cannot
   reach a node. The runtime escape note (`tur_region_note_escape`) flags a
   generation whenever one of its addresses is seen leaving it. Either lock
   alone can force a retire. Neither alone can permit a rewind.
5. **The result is not the only way out.** A node can leave a bracket by
   being stored, captured, boxed, or erased to `:int`. Every such site must
   call the note. That is the store-hook rule.
6. **Refusing is always safe.** A missed proof costs a saving, never
   correctness. A missed *note* is a silent use-after-rewind.

---

## Source map

| File | Role |
|---|---|
| `src/runtime/region.h`, `region.c` | The generation stack, the three pops, the escape note, ownership, shutdown, `TUR_REGION_STATS` |
| `src/runtime/arena.c`, `arena.h` | The bump arena under each generation; `arena_reset` poisons reclaimed bytes in Debug builds |
| `src/runtime/region_rt_embed.h`, `cmake/embed_region_runtime.cmake` | `region.h`, `arena.{h,c}` and `region.c` as byte arrays, pasted into an emitted program by `emit_module.c:emit_region_runtime_bodies` |
| `stdlib/region.tur` | `with-region`: a bare forwarder `(body)` |
| `stdlib/trail.tur` | `bt-scope`: a trail mark plus a deferred undo, then `(body)` |
| `src/compiler/emit_expr.c` | The static walk (`region_type_reaches_node`, `region_adt_reaches_node`, `emit_region_scope_reclaims`), recognition (`emit_binding_is_region_scope`), the direct-path bracket in `emit_value`, `region_ascription_erases_node`, `emit_region_word_can_be_node`, and most emitter note sites |
| `src/compiler/emit_core.c` | `emit_region_note_lvalue` (the emitter's one note spelling), `emit_region_shutdown_atexit`, the null-test exception in `emit_builtin` |
| `src/compiler/emit_cps_ir.c` | The CPS-path bracket in `emit_term_inner`'s CT_TAILCALL cps->direct arm; `tailcall_routes_cps_to_cps` |
| `src/compiler/emit_fns.c` | The inline-C typed-parameter entry note in `emit_fn_def_body` |
| `src/compiler/elab_call.c` | The implicit node -> `:int` erasure at a call argument (`call_arg_is_region_node_word`); the Saffron `with-region` refusal |
| `src/compiler/emit_module.c` | The `TUR_REGION_*` macros (both arms), the routed base ctor in `emit_adt_typedef_and_ctors`, the catch-boundary retire in `emit_runtime_preamble` |
| `src/compiler/types.c` | `regions_enabled`, `region_free_fn`, the routed monomorph ctor in `emit_registered_adt_app_rec` |
| `src/runtime/hamt.c` | The host-runtime notes in `tur_hamt_set` and `tur_hamt_transient_set` |
| `src/driver/cli.c` | `drv_apply_env_overrides` reads `TUR_REGIONS` into `g_opt_regions` (default `true`, `src/runtime/globals.c`) |

---

## The model

### Generations

`region.c` keeps one `Arena` per generation, with a 64 KiB default slab
(`TUR_REGION_SLAB`). Slabs are allocated lazily, so a bracket that allocates
nothing costs no slab. Per thread (`__thread`) it keeps:

| Array | Holds |
|---|---|
| `g_live` | Open generations, innermost last. The 1-based depth returned by `tur_region_push` indexes it. |
| `g_escaped` | One sticky flag per live generation. Set by a note; never cleared until the pop. |
| `g_retired` (+ `g_ridx`) | Popped without reclaim. Kept mapped; indexed by 64 KiB address block so `tur_region_owns` stays O(1) (performance-audit R-3). |
| `g_pool` | Rewound arenas, reset and ready for the next push, so a bracket in a loop pays for its slabs once. |

Ownership is process-wide: `g_reg` registers every live or retired arena in
any thread, under a spinlock touched once per push, retire and rewind. The
query skips the lock while every registered arena is this thread's, which is
every single-threaded program.

### Brackets: `with-region` and `bt-scope`

Both stdlib functions take a `[A] [^fat body : (fn [] A)] : A` thunk:

```turmeric
(defn with-region [A] [^fat body : (fn [] A)] : A (body))
(defn bt-scope [A] [^fat body : (fn [] A)] #fx{Bt} : A
  (let [m (bt-mark)] (defer (bt-undo-to! m)) (body)))
```

Neither body touches a region. The bracket is a **codegen decision at the
call site**. `emit_binding_is_region_scope` matches the callee binding's name
(`"bt-scope"` or `"with-region"`) and returns false when `regions_enabled()`
is false. A user definition cannot hijack the name: redefining an auto-loaded
stdlib name is already an error.

**turi** has no emitter, so `with-region` is an identity call there.
**Saffron** rejects `with-region` with TUR-E0312
(`elab_call.c`, `saffron_reject_static_only`): its values are `any`, which
the walk always refuses. `bt-scope` is allowed; its trail half still works.

### Rewind, retire, pool

| Call | Effect | Emitted by |
|---|---|---|
| `tur_region_pop(d)` | Retire generation `d` and anything above it. | A bracket whose static walk refused |
| `tur_region_pop_checked(d)` | Retire anything above `d` (skipped pops), then rewind `d` if its `g_escaped` flag is clear, else retire it. Returns whether it rewound. | A bracket whose static walk passed |
| `tur_region_pop_reclaim(d)` | Rewind unconditionally. | Nothing emitted; `pop_checked` calls it |

A **rewind** is `arena_reset` + `reg_remove` + push onto `g_pool`. When
`arena.c` is compiled without `NDEBUG` -- the copy pasted into a program is,
since `tur build` passes no `-DNDEBUG` -- `arena_reset` fills the used bytes
with `0xDE`. Under ASan it also re-poisons them, so a straggler read traps
at the dereference. That poison is the backstop every region fixture leans
on.

A **retire** moves the arena to `g_retired` and indexes its slabs. The memory
stays valid and owned until `tur_region_shutdown`. A depth deeper than the
stack is a spent handle (no-op); a shallower one retires the generations
whose pops were skipped.

`emit_core.c:emit_region_shutdown_atexit` registers `tur_region_shutdown` as
the first statement of `__tur_static_init`, behind its idempotence guard.
`atexit` handlers run LIFO, so shutdown runs after every module `defer`,
which may still read a value in a retired generation
(`region-shutdown-order`).

### Where region memory comes from

`tur_region_alloc_or_malloc` is spelled at exactly these emitted sites, each
through `types.c:region_alloc_fn()`, which answers `malloc` when
`regions_enabled()` is false:

| Site | Allocates |
|---|---|
| `emit_module.c:emit_adt_typedef_and_ctors` | The base ctor of a `:heap` ADT |
| `types.c:emit_registered_adt_app_rec` | The monomorph ctor of a parametric `:heap` ADT (`ctor_Cons_Cons__int`) |
| `emit_module.c:emit_program_inner` | The third, early-file copy of the heap ctor emitter |
| `emit_expr.c:emit_value_dispatch` | The SR4 per-link box for a recursive ctor field |
| `emit_module.c:ensure_saffron_dyn_runtime` | The dynamic-call rest-list cells |
| `emit_module.c:emit_flush_dead_base_ctor_traps` | The base ctor of a phantom-only parametric `:heap` ADT (ungated until 2026-10-17: [dead-base-ctor-ignores-regions-off](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/dead-base-ctor-ignores-regions-off.md)) |

The ctor emitters carry comments saying they mirror each other: a change to
one belongs in all three. The free side mirrors the allocation side.
`types.c:region_free_fn` returns `"tur_region_free"` when regions are on, and
every node drop path spells that instead of `free`. `tur_region_free` is a
no-op on region memory, live or retired, because a pointer into a slab must
never reach libc `free()` (glibc aborts).

### When does a generation actually rewind?

Only when **both** locks pass for that bracket on that run. In practice that
means the code inside the bracket kept its nodes **typed** from construction
to consumption:

| Style inside the bracket | Typical verdict |
|---|---|
| Typed spine (`nxt : Link`, `(TCons :int :TL)`, `tcons-of`), consumed in Turmeric, scalar or scalar-record result | Rewind |
| Node erased with `(:: node :int)`, or passed to an inline-C `:int` parameter, or to `tcons` (an `:int` tail) | Retire (erasure note) |
| Node stored into anything that outlives the bracket | Retire (store note) |
| Result type that can hold a node | Retire (static walk) |

The erased style is common in older fixtures and benchmarks, and it always
retires. Measured with `TUR_REGION_STATS=1` on this tree:

| Program | pushes | rewinds | retires |
|---|---|---|---|
| `tests/fixtures/region-with-region` | 103 | 103 | 0 |
| `tests/fixtures/region-scope-value-survives` | 103 | 103 | 0 |
| `tests/fixtures/region-scope-void-body` | 3 | 3 | 0 |
| `tests/fixtures/region-scope-escape-refused` | 4 | 1 | 3 |
| `tests/fixtures/region-escape-via-inline-c` | 2 | 1 | 1 |
| The typed worked example below | 2 | 1 | 1 |

The first three used to build their spine through `(:: (Link ..) :int)` and
sum it in inline C, so every bracket retired and they could not catch a
rewind that corrupts memory, though their comments said the pop rewinds
([region-rewind-fixtures-never-rewind](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/region-rewind-fixtures-never-rewind.md),
fixed 2026-10-21; `region-scope-escape-refused`'s rewinding control had the
same flaw). Their spines are typed now, and each pins its `region-stats` line.
When you write a region fixture, pin that line too, not just stdout: a
`run.env` holding `TUR_REGION_STATS=1` plus the line in `expected.stderr` (and
a `stderr.compiled-only` marker, since the interpreter has no regions), as
those four do; or a `hook.sh`, as the fixtures below do, when one program is
not enough.

---

## Lock 1: the static walk

`emit_expr.c:region_type_reaches_node` answers one question about the
**instantiated** result type of the bracket call: can a value of this type
transitively reach a region-allocated node? It refuses (answers yes)
whenever it cannot prove no.

| Result type | Verdict | Reason |
|---|---|---|
| `int`/`bool`/`float` family, `nil`, `never` | pass | No pointer |
| `cstr` | pass | No path routes a char buffer through a generation |
| Non-`:heap` ADT whose fields are scalar keywords | pass | `region_field_form_is_scalar` reads the declared form |
| Non-`:heap` ADT whose field has a recorded `full_type` that passes | pass | Walked recursively (an enum, a by-value sum of scalars) |
| `(Vec T)`, `(Map K V)`, `(Set T)`, `(MutableMap K V)` | pass iff the element types pass | `region_def_is_malloc_collection`: storage is stdlib `malloc` |
| Other parametric monomorph (`(Pair int int)`, `(Option int)`) | pass iff its substituted field types pass | `substitute_adt_app_type_owned` |
| Any `:heap` ADT | refuse | It is the node |
| Self- or mutually-recursive ADT | refuse | NULL `full_type` with a non-scalar form, or the def is already on the walk path |
| Pointer, ref, `rc`, closure, `any`, union, type variable, existential, `TY_STRUCT` | refuse | The `default:` arm |

Three details matter when you widen it:

- **The `seen` array is a path, not a visited set.** A def met again while
  still on the path is a cycle and refuses. A def is popped when fully
  explored, so the same harmless ADT in two sibling fields stays provable.
  The comment explains why the old visited-set rule would have proved a
  mutually recursive spine safe.
- **Budget.** Depth 24, 32 path slots. Running out refuses.
- **The type comes from the call, not the callee.** `bt-scope` declares
  `: A`, which would always refuse. Both bracket sites walk the call
  expression's type.

`emit_region_scope_reclaims` is the same walk for callers outside
`emit_expr.c`. The CPS backend uses it, so the two paths cannot drift.
Every widening so far has landed with a hook fixture that pins the
retire/rewind split and reads the values back (`region-scope-adt-result`,
`-shapes`, `-parametric`, `-vec-scalar`). Keep doing that.

## Lock 2: the runtime escape note

```c
void tur_region_note_escape(const void *p);            /* one word */
void tur_region_note_escape_words(const void *p, size_t n); /* every aligned word */
```

`tur_region_note_escape` walks the live generations innermost-first. If one
of them owns `p` (`arena_owns`), its `g_escaped` flag is set and the walk
stops. It returns immediately when no generation is open, so a note outside
any bracket costs one compare. The flag is sticky until the pop.

It flags the generation that **owns** the word, not the innermost one. A
store inside a nested bracket of a node the outer generation allocated must
block the outer rewind. The fuzzer's "nested" shape checks exactly this.

The words form reads every aligned word of `n` bytes as a possible address.
A scalar that happens to compare as an address in a live generation causes
a false positive. That only refuses a rewind.

| Lock | Proves | Cannot prove |
|---|---|---|
| Static walk | The result's *type* cannot reach a node, transitively | Anything about values that left by another route, or a node hidden in an `:int` |
| Escape note | A specific word that left is not in this generation | What that word transitively reaches |

Do not remove either on the strength of the other. The region header and
`region-scope-escape-refused` both make this point: deleting the result note
turns that fixture into an ASan use-after-poison.

## Closing the bracket

Both paths emit the same three-way decision after the call:

```c
int __tur_rgn_N = tur_region_push();
<T> r = <call>;
if (tur_panicking) TUR_REGION_RETIRE_TO(__tur_rgn_N - 1);
/* walk passed, result is a word (pointer, int64_t, intptr_t): */
tur_region_note_escape((const void *)(intptr_t)r); (void)tur_region_pop_checked(__tur_rgn_N);
/* walk passed, anything else (a by-value aggregate): */
tur_region_note_escape_words(&r, sizeof r); (void)tur_region_pop_checked(__tur_rgn_N);
/* walk refused: */
tur_region_pop(__tur_rgn_N);
```

- **Words vs aggregate.** The direct path decides by the hoisted temp's C
  type (`ret_ct`); the CPS path by the callee's recorded return C type. The
  words form catches an erased node in an `:int` field of an admitted record
  (`region-escape-via-erasure`).
- **Panic arm.** A panic out of the thunk returns before the pop. The panic
  arm retires everything from this bracket up (`TUR_REGION_RETIRE_TO`, which
  expands to `tur_region_pop(d + 1)` when the depth is deeper). Every catch
  boundary in the preamble (`tur_catch_unwind`, `tur_catch_panic_of` and
  their `_box`/`_box_via` variants) records `TUR_REGION_DEPTH()` on entry
  and retires down to it on the panic arm
  (`region-catch-retires-stranded-generation`). Retire, never rewind: the
  panic payload may point in.
- **Void results.** `emit_value` skips a call typed `nil`/`never`. A
  `bt-scope` or `with-region` call still gets a bracket, because the call is
  emitted against the erased `int64_t` carrier (`region-scope-void-body` pins
  this; its spine is typed, so its 3 brackets all rewind).
- **Allocation in argument position** lands in the generation too: the push
  precedes the argument emission on the direct path. The comment in
  `emit_value` argues why this is sound for a thunk argument.

---

## The store-hook rule

### Why it exists

The static walk only sees the result. On 2026-09-06, the day after
graduation, three ordinary programs rewound memory that something still
pointed into (region-lock-hardening in the
[regions plan](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/regions-plan.md)):

1. a node pushed into an outer `Vec` from inside `with-region`;
2. a node hidden behind an erased `:int` field of an admitted record result;
3. a `(Vec int)` result whose elements were erased nodes.

Each was a segfault, or a wrong answer, on the default build. The fix
generalised the runtime note from "the result word" to "every word that
leaves a generation".

### The rule

> Any primitive that writes a caller's word into memory that can outlive a
> `with-region` / `bt-scope` bracket must note that word, before the store.

"Outlive a bracket" means malloc'd memory, a global, a shared cell, a
foreign structure, or a stack image. A plain local does not count; it dies
with its frame (`emit_set_stmt` skips non-global, non-shared `set!`
targets).

### The hooked set, as the code has it

Verified against the tree on 2026-10-16.

**Stdlib inline C** (`TUR_REGION_NOTE(word)` before the store):

| Module | Functions |
|---|---|
| `vec.tur` | `vec-push!`, `vec-set-o!` (behind `vec-set!`) |
| `mutmap.tur` | `mutmap-set!` (key and value) |
| `trail.tur` | `bt-set!`, `g-set!` |
| `atomic.tur` | `atomic-store!`, `atomic-swap!`, `atomic-cas!` |
| `gen.tur`, `grid.tur`, `rcvec.tur` | `gen-arr-push!`, `grid-set!`, `rcvec-push!` |
| `ref.tur` | `ref-new` |
| `chan.tur` | `chan-send`, `async-chan-send`, `async-chan-try-send` |
| `future.tur` | `promise-fulfill`, `promise-fail`, `future-of`, `future-error-of` |
| `fiber.tur` | `fiber-resume`, `fiber-yield` |
| `r7rs/prelude.tur` | `r7rs-dyn-set__` (words form), `r7rs-cont-capture__` (the call/cc stack image) |
| `r7rs/thread.tur` | `r7rs-thread-spawn-c__` (the thunk's words) |

**Host runtime C** (calls `tur_region_note_escape` directly):
`hamt.c:tur_hamt_set` (every persistent map/set setter funnels through it)
and `hamt.c:tur_hamt_transient_set`.

**Emitter stores** (`emit_region_note_lvalue` after the store):

| Store | Function |
|---|---|
| `set!` on a global or a by-ref shared cell; the `rc` release-old path | `emit_stmt.c:emit_set_stmt` |
| `set-deref!` | `emit_stmt.c:emit_set_deref_stmt` |
| `set-field!` | `emit_stmt.c:emit_set_field_stmt` |
| Closure env fill, per captured field | `emit_expr.c:emit_value_dispatch` |
| `rc/of` payload | `emit_expr.c:emit_value_dispatch` |
| Fields of a malloc'd (non-`:heap`) ctor box | `emit_module.c:emit_adt_typedef_and_ctors`, `types.c:emit_registered_adt_app_rec`, `emit_module.c:emit_program_inner` |

**Emitter boxes** (`TUR_REGION_NOTE_WORDS` over a fresh malloc'd box that no
store hook will see): `emit_agg_box` (container element and field boxes);
the by-value ADT box in `dyn_widen_to_any` and the `any`-widen arm of
`emit_value_dispatch`; the open-class result box in `emit_value`; the three
escaping carrier-bridge boxes for call arguments in `emit_value_dispatch`;
the closure-env carrier box; and the CPS backend's `__aggbox_N`
(`emit_cps_ir.c:atoms_csv_call_typed_offs`).

**Erasures**:

- `(:: x T)` where `region_ascription_erases_node` is true: the source type
  can hold a node (a non-opaque ADT, an application, a struct), the target
  is `int`, `ptr<void>` or `any`, and the walk refuses the source. Noted in
  the `EX_ASCRIBE` arm of `emit_value_dispatch`.
- The same erasure written as a call argument. The call emitter strips an
  argument's ascriptions, so it remembers the erasure and notes the emitted
  value (`stdlib-region-store-hooks-unswept`).
- The implicit typed-node -> `:int` argument, which the elaborator turns
  into an explicit ascription (see below).
- A node handed to an inline-C callee's type-variable parameter, noted at
  the call (`region-escape-via-wrapper`).

**Inline-C bodies**: every typed parameter that can be a node, noted at body
entry (`emit_fns.c:emit_fn_def_body`; `region-escape-via-inline-c`).

### Where CLAUDE.md's list and the code differ

The CLAUDE.md list is right about every site it names. It does not name:
`ref-new`; the three `chan.tur` senders; the four `future.tur`
producers; `fiber-resume`/`fiber-yield`; `r7rs-dyn-set__` and
`r7rs-thread-spawn-c__`; and the `any`-widen and carrier-bridge boxes.
Treat this guide's table as the inventory and CLAUDE.md as the rule.

### How to add a hook

**In stdlib inline C.** One line before the store. Use the words form for a
by-value aggregate:

```c
TUR_REGION_NOTE(val);                    /* a word */
TUR_REGION_NOTE_WORDS(&agg, sizeof agg); /* every word of an aggregate */
vec->data[vec->len++] = (int64_t)val;
```

The macros are always defined in the emitted preamble
(`emit_module.c:emit_closure_fat_runtime`). Under `TUR_REGIONS=0` they
expand to `((void)0)`, so the same stdlib text compiles on both arms. The
names are upper-case on purpose: `tests/run-regions-seam.sh` greps the off
arm for `tur_region_` and must find nothing.

**In the emitter.** Call `emit_region_note_lvalue(body, indent, ctype, lv)`
after the store. It returns early when regions are off and when `ctype` is a
known scalar spelling (`double`, `int32_t`, ...). Otherwise it always emits
the words form, `TUR_REGION_NOTE_WORDS(&(lv), sizeof(lv))`, because the
claimed C type is not always the emitted one and `(intptr_t)` of an
aggregate is a hard cc error. So `lv` must be an addressable lvalue. If you
only have an expression, hoist it into a named temp first; do not reach for
`__auto_type`, which c2mir cannot parse (the `EX_ASCRIBE` arm explains the
JIT breakage that caused).

**In host runtime C** (`src/runtime/*.c`, never pasted into a program): call
`tur_region_note_escape(word)` directly. The macro does not exist there.
Read the caveat under [Tools](#turn-regions-off-tur_regions0) first: a
direct call is an unconditional symbol reference.

**Then the fixture.** Add a case to `tests/fixtures/region-escape-via-store`
(or `-store-future` if it needs pthreads, which the JIT cannot compile). Pass
the node **typed and directly**. A case that builds it through
`(:: (Link ..) :int)` is noted at the erasure before it reaches your store,
so it passes with your hook deleted. `region-escape-via-store-future`
records the measurement: rewinds=2 without the hooks, retires=2 with them.
If the stored word can be read back unchanged after a rewind (the arena is
not always reused first), pin the `region-stats` line through a `hook.sh` as
`region-escape-via-wrapper` does.

### Typed parameters, erased words, and the implicit erasure

An inline-C body is opaque, so the emitter cannot see what it retains. The
rule is applied at the callee, where every parameter is a plain C
identifier. `emit_fn_def_body` notes a parameter when
`emit_region_word_can_be_node` says its word can *be* region memory:

| Parameter type | Noted? |
|---|---|
| `:heap` ADT (a node) | yes |
| By-value aggregate the static walk refuses (it may hold a node) | yes |
| Scalar, `cstr`, `ptr<void>`, type variable, function | no |
| `defopaque` newtype (C-made handle) | no |
| `Vec`/`Map`/`Set`/`MutableMap` handle (stdlib `malloc`) | no |

The filter is narrower than the static walk on purpose. The walk refuses a
`ptr<void>` out of ignorance; noting one would add a runtime call to every
hot accessor and flag nothing.

A word that arrives **erased** carries no type for this filter to read. That
is why the stdlib hooks note by hand: their `val` is a carrier word, not a
`Link`. Three mechanisms make sure an erased node was noted somewhere
upstream:

1. **The implicit erasure** (`elab_call.c`, the
   `stdlib-region-store-hooks-unswept` block). When an argument meets an
   `:int` parameter, the elaborator wraps it in an `EX_ASCRIBE` to `:int`,
   which the emitter then notes:
   - for an inline-C callee, any ADT, application or struct argument;
   - for a Turmeric-bodied callee, only a `:heap` node word
     (`call_arg_is_region_node_word`), and never for a constructor. A
     constructor's box is either routed into the same generation or notes
     its own fields, so noting there retired every typed tree.

   The Turmeric-bodied case closed `sized-buf-set!`, a typed wrapper over an
   inline-C store. Its known cost is `tcons`, whose tail is `t : int`: a
   list built with it inside a bracket retires. `tcons-of` keeps the rewind.
2. **The type-variable call-site note** (`emit_value_dispatch`, security
   audit WP5 M-6). The entry note skips a def-less type variable, so a node
   passed to `[A] ... x : A` of an inline-C callee is noted at the call,
   where its concrete type is known.
3. **The primitive's own note**, for a word that reached it through an
   erased generic path neither of the above saw.

Two exceptions, both deliberate:

- **The null test.** `(= (:: l :int) 0)` and `(not= ...)` against a literal
  `0` do not note (`emit_builtin` sets `region_erasure_compare_only` for the
  `EX_ASCRIBE` arm). The word is compared and dropped. Before this, every
  bracket that walked a stdlib list with `tnil?` retired
  (`region-list-null-test-rewinds`).
- **An un-nameable temp.** When the erased value is not a bare identifier
  and `emit_binding_repr_c_name` cannot name its C type, the `EX_ASCRIBE`
  arm skips the hoist and the note. The code relies on every later store to
  note the word. It is the one place the erasure note can be silently
  absent.

---

## Interaction with the CPS backend

Read the [CPS/DK internals guide](cps-dk-internals-guide.md) first. Three
facts govern regions there:

1. **Every region bracket's caller is colored.** `with-region` and
   `bt-scope` call a fat thunk, an indirect call, so they are colored, and so
   is every caller (`TUR_CPS_CENSUS=1` shows `colored indirect`). A bracket
   inside an ordinary `defn` is therefore often emitted by the CPS backend;
   `main` and functions evicted from `S` go through `emit_value`. Both paths
   occur in the worked example below.
2. **The bracket lives on the CT_TAILCALL cps->direct arm.** A cps->cps tail
   call never returns to this frame, so there would be nowhere to put the
   pop. `tailcall_routes_cps_to_cps` returns false for a region scope, and
   the arm's `force_direct` does the same
   ([region-bracket-lost-when-bt-scope-specializes](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/region-bracket-lost-when-bt-scope-specializes.md)).
   The arm uses `emit_region_scope_reclaims` on the call expression's type
   and emits the same three-way close as `emit_value`, before
   `dk_run(__kont, ...)` delivers the value.
3. **CT_LETCALL carries no bracket, by proof.** The IR builder emits
   CT_LETCALL only for an uncolored callee, and both region scopes are
   colored. `region-scope-nontail-cps` asserts zero letcalls to
   `with-region` in non-tail positions.

Notes inside a CPS-lowered function come along for free: an uncolored call
in one is a CT_LETRAW delegated to `emit_value`, which emits the stripped
erasure and type-variable notes as usual (checked with a `keep-gen`-style
store in a colored named function).

**A thunk that can perform is not bracketed.** The bracket lives on the
cps->direct arm, where the thunk runs in direct style under the bracket call
and its plain entry seeds a fresh DK root. A `perform` inside it would not
reach a handler outside the bracket. It used to abort with
`tur: unhandled effect (tag N)`
([region-bracket-blocks-effects](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/region-bracket-blocks-effects.md)).
So `region_scope_call_bracketed` (`emit_cps_ir.c`) keeps the bracket only for
a thunk that cannot reach an effect op (`fnvalue_arg_reaches_effect_op`). A
thunk that can is routed like any other call, threaded cps->cps, exactly as
under `TUR_REGIONS=0`. The cost is the rewind for that call: nothing it
allocates is reclaimed at the bracket. That is the lock's failure mode
anyway, a saving lost rather than a stale read. The test is syntactic, so a
thunk whose `perform` is handled inside it loses its bracket too.
`region-bracket-effect-reaches-outer-handler` pins all of these shapes.

## Interaction with call/cc stack images

`#lang r7rs` `call/cc` copies the C stack into a malloc'd image
(`r7rs-cont-capture__` in `stdlib/r7rs/prelude.tur`). That copy is a store
of every word on the stack into memory that outlives any bracket around it.
It therefore ends with `TUR_REGION_NOTE_WORDS(c->img, c->n)`: any stack word
pointing into a live generation flags that generation. A Scheme file cannot
open a region itself; the case is a Turmeric caller that calls a Scheme
library inside a bracket. `region-escape-via-callcc` pins it: before T8
the re-entered continuation printed garbage where `TUR_REGIONS=0` printed 7.

The DK machine's heap continuations (the compiled path's `shift` /
`handle` captures) carry no region note of their own. Today that is covered
by the point above: a thunk that can capture is never bracketed, so no
continuation is captured inside a bracket. If brackets ever go around such
a thunk, the frames a captured continuation holds become a new escape route,
and they need the note.

---

## Tools

### `TUR_REGION_STATS=1` (read by the compiled program)

At exit, `tur_region_shutdown` prints one stderr line:

```
region-stats: pushes=N rewinds=N retires=N
```

This is the only way to tell a rewind from a retire. Stdout cannot: a
rewound generation's poisoned bytes are often never read. Use it on any
region fixture you write. Counts are process-wide atomics.

### Turn regions off: `TUR_REGIONS=0`

`TUR_REGIONS` is read by **`tur`**, at compile time (`drv_apply_env_overrides`), not by
the program. `=0` clears `g_opt_regions`, which removes the routed ctor
allocation (back to `malloc`), the bracket around `with-region` /
`bt-scope`, the pasted region runtime and the `atexit`. Emitter notes are
either skipped or spelled as `TUR_REGION_*` macros that now expand to
nothing. It is the bisection hatch: a wrong answer that disappears under it
is a lock or hook bug; one that stays is something else.

One caveat, measured on this tree, where only the `tur` target is built (no
`libturt_runtime.a` or `libturi.a`): `hamt.c` is autolinked from source into
every program that loads the stdlib, and it calls `tur_region_note_escape`
unconditionally. With regions off nothing pastes `region.c`, so
`TUR_REGIONS=0 tur build` fails to link (`undefined reference to
tur_region_note_escape`) unless a runtime archive supplies the symbol.

`tests/run-regions-seam.sh` builds its population with
`-Werror=implicit-function-declaration`, so a region call left undeclared
under `TUR_REGIONS=0` fails there on any compiler; gcc 13 only warns.

### The poison backstop

- Debug arena poison: `arena_reset` memsets reclaimed bytes to `0xDE` in
  any non-`NDEBUG` build, and under ASan re-poisons them.
  `TUR_DEBUG_ARENA_POISON=0` turns the ASan half off;
  `TUR_DEBUG_ARENA_GUARD=1` mprotects freed slabs.
- `tests/check-region-poison.sh` (ctest `tur_region_poison`) builds a canary
  that reads reclaimed region memory and fails if ASan stays quiet.

### Harnesses

| Harness | ctest | Checks |
|---|---|---|
| `tests/region_unit.c` | `tur_region_unit` | Ownership of live and retired memory, no reclaim on plain pop, depth mismatch, nesting, NULL with no region |
| `tests/run-regions-seam.sh` | `tur_regions_seam` | Under `TUR_REGIONS=0`: the canary emits no `tur_region_` symbol; a population of spine and bracket fixtures prints its committed stdout |
| `tests/regions-fuzz-src.py` (via `run-regions-fuzz-src.sh`) | `tur_regions_fuzz_src` | Generated brackets with every escape route. Values read after the pop must match a prediction on both arms (default arm under ASan), and `region-stats` must match the generator's rewind/retire model. Its header lists two sabotages it is proven to catch. |

The ctest fuzz run is smoke size (`REGIONS_FUZZ_N=12`). For a session, run
`python3 tests/regions-fuzz-src.py --n 200 --seed <fresh>`.

### Fixtures

| Fixture | Pins |
|---|---|
| `region-escape-via-store` | Typed nodes stored by each hooked primitive; every value read after the pop |
| `region-escape-via-store-future` | The `future.tur` stores, split out because pthreads costs JIT coverage |
| `region-escape-via-erasure` | A node reaching the result only through an `:int` word (record field, `(Vec int)`) |
| `region-escape-via-erased-argument` | The implicit erasure, and an explicit one written as a call argument; stats pinned by `hook.sh` |
| `region-escape-via-inline-c` | The entry note retires a bracket that hands a node to inline C, and a scalars-only body still rewinds |
| `region-escape-via-wrapper` | A Turmeric wrapper over an inline-C store, and a type-variable parameter; stats pinned |
| `region-escape-via-callcc` | The r7rs `call/cc` stack image |
| `region-escape-via-main-loop` | The closure ABI a spice frame callback relies on, after the bracket's memory is reused |
| `region-scope-escape-refused` | Each lock refusing a case the other cannot see |
| `region-scope-adt-result`, `-shapes`, `-parametric`, `-vec-scalar` | Static-walk widenings: the retire/rewind split in emitted C, values read on both arms |
| `region-scope-nontail-cps` | Brackets in non-tail CPS positions take the tailcall arm |
| `region-list-null-test-rewinds` | The null-test exception (5 rewinds) against an arithmetic control (5 retires) |
| `region-catch-retires-stranded-generation` | Panic through a bracket; catch boundaries retire |
| `region-shutdown-order` | Shutdown registered before module defers; flag-off emits no region symbol |
| `region-retired-index-owns` | `tur_region_owns` through the retired-address index after 2000 retires |
| `errors/extern-c-refuses-region-node` | A `:heap` node cannot cross an `extern-c` boundary (it has no body to note in) |
| `errors/saffron-region-bracket-refused` | TUR-E0312 for `with-region` in Saffron |

---

## Worked example

A typed spine, one bracket with a scalar result and one returning the spine:

```turmeric
(defdata TL :heap (TNil) (TCons :int :TL))
(defn tbuild [n : int acc : TL] : TL
  (if (<= n 0) acc (tbuild (- n 1) (TCons n acc))))
(defn tsum [t : TL] : int
  (match t
    (TNil) 0
    (TCons v r) (+ v (tsum r))))
(defn round [n : int] : int
  (with-region (fn [] : int (tsum (tbuild n (TNil))))))
(defn keep [n : int] : TL
  (with-region (fn [] (tbuild n (TNil)))))
(defn main [] : int
  (println (round 10))
  (println (tsum (keep 4)))
  0)
```

```
$ tur build typed.tur -o typed && TUR_REGION_STATS=1 ./typed
55
10
region-stats: pushes=2 rewinds=1 retires=1      (stderr)
```

The constructor routes into the innermost generation
(`tur emit-c typed.tur`, trimmed):

```c
static tur_adt_TL * ctor_TL_TCons(int64_t _0, int64_t _1) {
    tur_adt_TL *__r = (tur_adt_TL *)tur_region_alloc_or_malloc(sizeof(tur_adt_TL));
    __r->tag = 1;
    __r->as.TCons._0 = _0;
    __r->as.TCons._1 = _1;
    return __r;
}
```

`round` is colored, so its bracket is on the CPS tailcall arm. The result is
`int`, the walk passes, and the pop is checked (trimmed):

```c
static int64_t tur_u_round__cps(int64_t n, DK *__kont) {
    ...
    __t210->n = n;
    TUR_REGION_NOTE_WORDS(&(__t210->n), sizeof(__t210->n));   /* closure env fill */
    ...
    int __tur_rgn_214 = tur_region_push();
    int64_t __t213 = with_hyregion((int64_t)(intptr_t)_un_unborrowc_un21_22); /* cps->direct */
    if (tur_panicking) TUR_REGION_RETIRE_TO(__tur_rgn_214 - 1);
    ...
    tur_region_note_escape((const void *)(intptr_t)__t213); (void)tur_region_pop_checked(__tur_rgn_214);
    return dk_run(__kont, (intptr_t)(__t213));
}
```

The closure env is a plain `malloc`, never region memory, and the captured
`n` is a scalar: nothing is flagged, and the pop rewinds.

`keep` returns `TL`, a `:heap` ADT. The walk refuses at compile time, so the
emitter does not even ask the runtime:

```c
    int __tur_rgn_219 = tur_region_push();
    int64_t __t218 = with_hyregion((int64_t)(intptr_t)_un_unborrowc_un29_30); /* cps->direct */
    if (tur_panicking) TUR_REGION_RETIRE_TO(__tur_rgn_219 - 1);
    ...
    tur_region_pop(__tur_rgn_219);
```

The four `TCons` cells stay valid in a retired generation until exit, which
is why `(tsum (keep 4))` can read them.

Now a store. The bracket's result is `int`, so the walk passes, but the body
pushes a node into a vector created outside:

```turmeric
(defdata Link :heap (Link [v : int nxt : int]))
(defn stash [out : (Vec Link) n : int] : int
  (with-region (fn [] : int
    (do (vec-push! out (Link n 0)) n))))
```

The thunk body (trimmed) shows two notes on the same word: the call-site
type-variable note for `vec-push!`'s `val : A`, and then the one inside
`vec-push!` itself:

```c
static int64_t __fn_8(void * __env_p_11) {
    ...
    tur_adt_Link * __ps_70 = (ctor_Link_Link(__env___env_10->n, INT64_C(0)));
    ...
    TUR_REGION_NOTE_WORDS(&(__ps_70), sizeof(__ps_70));
    vec_hypush_ex((int64_t)(intptr_t)(__env___env_10->out), (int64_t)(intptr_t)(__ps_70));
    ...
}
/* stdlib/vec.tur, vec-push! */
  TUR_REGION_NOTE(val);
  vec->data[vec->len++] = (int64_t)val;
```

`stash` is emitted direct (by `emit_value`), and its close is
`tur_region_note_escape(...); (void)tur_region_pop_checked(...)`. The flag
set by the note makes that pop retire
(`region-stats: pushes=1 rewinds=0 retires=1`), and the node read back from
`out` after the bracket is intact.

---

## Open reports and history

Open:

- [byvalue-recursive-shared-copies-leak](https://github.com/turmeric-lang/turmeric/blob/main/docs/reported/byvalue-recursive-shared-copies-leak.md)
  and [carrier-sum-option-boxes-have-no-owner](https://github.com/turmeric-lang/turmeric/blob/main/docs/reported/carrier-sum-option-boxes-have-no-owner.md)
  -- leak reports that name a region as the remedy for part of their residue.

Archived, for the *why*:

- [region-rewind-fixtures-never-rewind](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/region-rewind-fixtures-never-rewind.md)
  -- three rewind fixtures retired every bracket (an erased spine); typed
  now, with the rewind count pinned through `run.env`.
- [regions-plan](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/regions-plan.md) -- the design, the increments,
  region-lock-hardening, and the graduation checklist.
- [region-escape-through-unhooked-stores](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/region-escape-through-unhooked-stores.md)
  -- the inline-C entry note, the extern-c boundary, the panic jam.
- [stdlib-region-store-hooks-unswept](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/stdlib-region-store-hooks-unswept.md)
  -- the implicit erasure; why a case that erases on the way in tests
  nothing.
- [region-walk-refuses-every-adt-result](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/region-walk-refuses-every-adt-result.md)
  -- the form-based scalar field check and the path-not-visited-set rule.
- [region-bracket-lost-when-bt-scope-specializes](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/region-bracket-lost-when-bt-scope-specializes.md)
  -- why the CPS arm forces cps->direct.
- [stdlib-list-null-check-retires-regions](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/stdlib-list-null-check-retires-regions.md)
  -- the null-test exception.
- [region-rewind-reuses-only-head-slab](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/region-rewind-reuses-only-head-slab.md)
  and [region-retired-generations-make-shutdown-quadratic](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/region-retired-generations-make-shutdown-quadratic.md)
  -- the spare-slab list, the retired index and the one-pass shutdown.

[frozen-region-aliasing-via-coercing-cast](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/frozen-region-aliasing-via-coercing-cast.md)
is about a `frozen` borrow region, an unrelated feature that shares the
word.

Related guides: [gc-guide](gc-guide.md),
[memory-usage-guide](memory-usage-guide.md),
[backtrackable-state-guide](backtrackable-state-guide.md#bt-scope-is-also-a-region-and-with-region-is-only-a-region),
[cps-dk-internals-guide](cps-dk-internals-guide.md),
[value-representations-guide](value-representations-guide.md),
[emitted-code-ownership-guide](emitted-code-ownership-guide.md),
[runtime-preamble-and-linkage-guide](runtime-preamble-and-linkage-guide.md),
[emitter-architecture-guide](emitter-architecture-guide.md),
[elaborator-internals-guide](elaborator-internals-guide.md),
[r7rs-implementation-guide](r7rs-implementation-guide.md).

---

## Bugs found while writing this guide

Each was reproduced against the compiler this guide describes and filed in
[`docs/reported/`](https://github.com/turmeric-lang/turmeric/blob/main/docs/reported/README.md). When one is fixed and archived,
update the passage above that describes it and drop it from this list.

- [region-bracket-blocks-effects](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/region-bracket-blocks-effects.md) -- (resolved 2026-10-18) A `perform` inside `with-region` / `bt-scope` could not reach an outer handler
- [regions-off-source-runtime-link-failure](https://github.com/turmeric-lang/turmeric/blob/main/docs/reported/regions-off-source-runtime-link-failure.md) -- `TUR_REGIONS=0` fails to link when the runtime comes from source

## Maintenance

Update this guide in the same change when you:

- add or remove a **store hook** anywhere (stdlib, emitter, host runtime):
  fix the hooked-set tables, and the CLAUDE.md list if it names the site;
- add an **allocation site** (spell it `region_alloc_fn()`), or a free path
  that must use `tur_region_free` (`region_free_fn()`);
- **widen or narrow the static walk**, or change `emit_region_word_can_be_node`
  or `region_ascription_erases_node`;
- change where a bracket is emitted (a new CPS arm, a new region-scope name);
- change what `TUR_REGIONS=0` removes, or the `region-stats` format (the
  fuzzer and several `hook.sh` fixtures parse it);
- resolve a linked report (move its link to `docs/archive/`).

Re-measure the tables marked "measured" with `TUR_REGION_STATS=1` rather than
copying numbers forward.
