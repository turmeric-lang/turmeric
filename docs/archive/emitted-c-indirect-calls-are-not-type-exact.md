# Emitted C makes indirect calls through function pointers of the wrong type

**Severity: high (WASM), medium (native).** Undefined behaviour in C on every
target. On SysV/AAPCS most instances are ABI-benign: `bool` vs `int64_t`, or a
pointer vs `int64_t`, rides the same register. But WebAssembly's
`call_indirect` checks the exact signature, so each one is a trap on the web
build. And the same *mechanism* is what produced this repo's longest-running
silent-wrong-answer family, when the type that differs is a `double` or a
16-byte tagged `any`. Filed 2026-09-30 with the P0 representation-confusion
work.

**Status: RESOLVED 2026-10-02 (ninth sweep): zero traps across the corpus,
and the detector GATES.**  The `ptr<void>` decision below took the last four
to one, and that one is a different defect -- an erased producer read by a
typed caller, not a spelling -- filed as
[arrow-instance-closure-erased-to-words](arrow-instance-closure-erased-to-words.md)
and carried as the gate's single `known.fnsan` fixture -- until its direct
`.>>>` shape was fixed the same day, which emptied the list: the gate now
carries no `known.fnsan` at all.  The suite runs under
the trap flags in the `fnsan` CI job (`tests/run-fnsan.sh`, which proves the
detector armed with a canary first and requires each `known.fnsan` fixture to
STILL trap), and a fuzzer trap is the failing `BUG_fnptr_trap` by default now
(`TUR_FUZZ_FNSAN_STRICT=0` makes it report-only).

## How to see it

clang's `-fsanitize=function` compares every indirect call with the callee's
definition. Trap mode needs no UBSan runtime:

```sh
# An unsanitized libturi.a, so clang can link the fixtures that use it
# (the Debug one is built with gcc's ASan, which clang cannot link):
cmake -S . -B build-nosan -DCMAKE_BUILD_TYPE=Debug -DTUR_DEBUG_SANITIZE=OFF
cmake --build build-nosan -j --target libturi
CC=clang TUR_CC_FLAGS="-O2 -std=c99 -Wall -Wfloat-conversion \
  -Werror=implicit-function-declaration -fno-strict-aliasing \
  -fsanitize=function -fsanitize-trap=function -L$PWD/build-nosan/src" \
  timeout 720 bash tests/run.sh
```

A trap is SIGILL (exit 132). To find the site, build the fixture at **`-O0
-g`** with the same flags and run it under gdb. At `-O1` the trap is
attributed to whatever got inlined around the call: the first clustering of
this sweep blamed `dk_free` for what was an E2a call two frames away.

The detector is EXACT. `char *` vs `void *`, `long` vs `long long`, and
`bool` vs `long` results all trap. It cannot tell a benign mismatch from a
harmful one, so the corpus has to reach zero before it can gate.

## Where the corpus stands

| Sweep | Trapping fixtures | What changed |
| --- | --- | --- |
| first | 328 | -- |
| second | 165 | zero-parameter functions emitted `(void)`, not the unprototyped `()` the sanitizer hashes as a different type |
| third | 106 | session thread wrapper calls `void (*)(void *)`; stdlib `seq-call-bool-fn1` and the comparator calls (vec/map/set/mutmap/pair/result eq) cast slot 0 to the `int64_t` it returns for a narrow result (narrow-closure-result-read-through-int64-carrier) |
| fourth (2026-10-01) | 66 | a variadic's fat-box shim casts the rest slot to the definition's typed chain pointer (`ensure_variadic_rest_fatshim`); a fat closure packed into the `tur_poly_fn_t` carrier gets a word adapter when its thunk is not all-word (`ensure_fat_word_adapter`); a dictionary slot whose class-variable parameter is a non-word scalar holds a converting wrapper (`dict_slot_param_is_word_scalar`); a bare function boxed behind the generic word shim gets a bare-call word adapter keyed on its recorded signature.  **Two of these were silent wrong answers when the type was a `double`** -- see `docs/archive/dict-classvar-float-param-value-converted.md` |
| sixth (2026-10-01) | 57 -> 16 | **First sweep that counts every fixture**: `-L` points at an UNSANITIZED `libturi.a` (`cmake -S . -B build-nosan -DTUR_DEBUG_SANITIZE=OFF && cmake --build build-nosan --target libturi`), so the ~56 fixtures that failed to link under clang before are in -- 57 trapping at the start.  Fixed: a rank-2 `__poly_N` wrapper / capturing closure packed into a FORALL or erased carrier-base sink gets an adapter at the call site's convention (narrow "phase F" or word, per position; `ensure_named_call_adapter` / `ensure_call_adapter_ex`); a bare fn boxed for a `^fat` sink is spelled as the callee THE CALL SELECTS reads it -- a concrete parameter type, a spec clone's instantiated type, or words at the type-variable positions of a carrier base / inline-C body (`fn_to_fat_.sink_fn_type`, `ctx->fat_box_sink_type`) -- whenever the box's default choice would spell it differently; runtime callbacks (timer wheel `tur_scheduler_unpark_cb`, serial-registry ser/deser adapters, capability FileSystem vtable, image registry, `future-then`, a `nil` `(async ...)` body); a cloneable continuation's named receiver is called at its recorded result type; fixtures' own inline C calls each closure at its emitted type |
| seventh (2026-10-01) | 16 -> 9 | A `nil`-returning function passed where the slot is `(fn [...] A)`: no spec is made at `A := nil`, so the carrier base calls slot 0 as returning the word -- a bare fn's box gets a `void`-callee adapter (`ensure_call_adapter_ex` learned `void`), a capturing closure a `{shim, handle}` wrapper whose shim answers 0 (`EX_FN_TO_FAT.nil_result_word`, `ensure_nilres_fatshim`; on the stack when the sink provably keeps nothing, reaped with the DK entry inside a CPS body) -- `region-scope-void-body`, `nil-closure-into-erased-result`.  A thin call through a fn value spells a TY_FN parameter `int64_t`, as every definition does, not from an already-fat argument's `void *` (`annotated-fat-lambda-param`, `sf-let-bind-with-inner-call`).  A narrow-result closure packed into a CONCRETE (phase F) `tur_poly_fn_t` sink gets the narrow adapter at any arity, a by-value aggregate parameter passing through unchanged, and the hoisted `__borrowc` thunk is found through the carrier retype (`fn-value-carrier-fat-seams`, `hkt-cata-fmap-byvalue-carrier`).  A <= 16-byte by-value result boxed for a CONCRETE sink -- a `^fat` parameter or, new, a concrete `(fn ...)` struct field (`sink_fn_type`) -- takes the typed shim on every host, not only Win64 (`fat-dispatch-parametric-monomorph-return`, `fn-field-carrier-shim-read-typed`).  No snapshot moved |
| eighth (2026-10-02) | 9 -> 4 | The untyped-`^fat` inline-C cluster is gone, and it was hiding a **silent wrong answer**: `vec-eq?`, `map-eq?`, `result-eq?`, `mutmap-eq?` with a comparator typed `(fn [a : float b : float] ...)` compared xmm registers nobody set (`vec-eq?` and `map-eq?` answered true for 7.1 against 3.25; the interpreter said false).  The stdlib comparators take `(fn [A A] bool)` now (fix direction 4); a bare fn is boxed at words where the inline C reads them, a capturing closure or a forwarded typed `^fat` parameter is wrapped in a { adapter, handle } box (`EX_FN_TO_FAT.word_params`, `fat_closure_tyvar_sink_adapter`), a type variable a spec leaves unbound is a word, and a bare fn whose recorded signature differs from its type's (a niche parameter that arrives as the carrier box) gets an adapter instead of a typed shim that called it at the wrong type (`option-niche-vec-closure-cmp`, `niche-elem-comparator-conventions`, new `comparator-float-elements-word-adapter`).  A fat call through a fn value whose declared result resolves to a POINTER under the spec takes that result in the cast -- its parameters already followed the declared type -- and hands the word back (`stdlib-lens-record-field`, `lens-compose-wide-byvalue-get-put`).  And a typed `tur_poly_fn_t` carrier's phase-F call (F5: its thunk is natively typed) spells a parameter the declared `(fn [a] b)` resolves to a pointer as that pointer, bridging an argument that is only the carrier word -- `run-id`'s spec answers `int64_t` at a := Point (`van-laarhoven-lens-wide-compose`, the one snapshot that moved, by that one cast) |
| ninth (2026-10-02) | 4 -> 0 (+1 known) | **The `ptr<void>` decision: an untyped `ptr<void>` PARAMETER is the word in slot 0**, as a function-typed one (ER4) and a b4box aggregate already were (`type_is_word_closure_slot`, `thunk_param_slot_c_name`).  A closure thunk keeps `void *` in its definition and slot 0 holds a converting `__tur_slot0_<thunk>` entry; a bare defn's typed fatshim converts the word back (`thunk_param_def_c_name`); every hand-built fat-call cast asks the slot question; runtime C that calls slot 0 by hand follows (`httpd`'s handler dispatch, `image`'s deser; the reactor was already all-word).  A function-typed parameter now has a concrete C ABI in the typed-thunk predicate, so a captureless lambda whose parameter is a function takes the typed shim instead of the all-word adapter a typed caller misread (`fn-param-closure-typed-result-shim`, new -- found probing).  A fat value whose lambda the `tur_poly_fn_t` packing site cannot see gets the narrowing adapter keyed on its type (`narrow-fat-value-into-concrete-carrier`, new -- a type-fuzzer `FNPTR_TRAP`).  `fat-shim-void-ptr-arrow-compose`'s remaining trap is the Arrow instance (filed).  Pinned by `closure-slot-ptr-void-fn-word` (a closure erased to `ptr<void>` called back at both its own type and the `ptr<void>`-ified one) and `closure-slot-ptr-void-param-sinks` (`def`, defn, generic HOF, Vec, struct-field producers and sinks).  One snapshot moved |

Clusters at `-O0` as of the second sweep (165 fixtures; counts are
fixtures). The session, `seq-call-bool-fn1` and comparator rows are fixed in
the third sweep:

| Count | Site | Kind |
| --- | --- | --- |
| 38 | `tur_session_thread_wrapper`: `thunk(arg)` calls a `(fn [] nil)` fat slot 0 as `int64_t (*)(void *)` | runtime typedef, benign |
| 13 | poly-fn carrier `((int64_t (*)(void*, int64_t))g.fn)(g.env, x)` against a wrapper of another signature | M4 |
| 12 | `__tur_fatshim_tur_tagged_t_int64_t` casts a variadic callee's rest slot `int64_t` where the definition spells `tur_adt_Cons__any *` | pointer vs int |
| 11 | `__inst_Functor_fmap_Identity` | M4 |
| 9 | stdlib `seq-call-bool-fn1`: `TUR_APPLY1_T(bool, int64_t, f, x)` on a generic `int64_t` fatshim | bool vs int64 |
| 12 | stdlib comparators (`vec-eq?`, map/set/result eq): `bool(*)(void*, int64_t, int64_t)` | benign/M4 |
| ~70 | long tail: typed-field boxes on the generic shim for pointer signatures, E2a pointer args, reactor callbacks, serializer hooks | mixed |

How the fourth sweep was measured, since the trap gives no message without a
UBSan runtime: build each trapping fixture at `-O0 -g` with the trap flags,
run it under `gdb -batch`, and read frame 0 and its emitted C line. The 56
fixtures that failed to LINK under clang (libturi is built with gcc's ASan)
are environmental and not counted.

A fifth pass (56): an E2a registry entry declared with a pointer or narrow
parameter registers an adapter in the call site's word convention (`<fn>__e2w`).

Remaining after the eighth sweep (4 traps, measured with the full suite under
the command above at `-O2` -- 3482 passed, 4 failed -- and each confirmed at
`-O0 -g`).  One cluster, ONE slot whose producer and consumer decided the
spelling from different information:

| Cluster | Fixtures | Producer in the slot | Consumer's cast | What a fix has to decide |
| --- | --- | --- | --- | --- |
| `ptr<void>` used as a function type | `fat-captureless-closure-ptr-void`, `vec-captureless-fat-closure-readback`, `vec-typed-fat-closure-readback`, `fat-shim-void-ptr-arrow-compose` | the closure's own thunk, or the word shim, at its real signature: a function-typed parameter is `int64_t` (`void *__fn_25(void *, int64_t)`) | the sink's declared `(fn [ptr<void>] ptr<void>)`, so `void *(*)(void *, void *)` | the program erases a closure to `ptr<void>` and calls it at a `ptr<void>` signature.  Both are words, so the type-exact answer is ONE spelling for the two: either a `ptr<void>` thunk slot is the word (as a b4box slot is), or a function-typed one is `void *`.  Either is an ABI-wide change to every thunk typedef and closure definition, not a local fix |

**Re-measured 2026-10-02: still these 4, and a local adapter cannot reach
them.**  The eighth sweep's `{ adapter, handle }` wrap needs a call site that
knows the closure's real type.  Here three of the four
(`vec-captureless-fat-closure-readback`, `vec-typed-fat-closure-readback`,
`fat-shim-void-ptr-arrow-compose`) erase the closure into a `(Vec ptr<void>)`
or a `ptr<void>` value before the `(fn [ptr<void>] ptr<void>)` call, so no
site sees both spellings; the fourth (`fat-captureless-closure-ptr-void`)
traps twice -- the captureless closure is boxed behind the generic
`int64_t (*)(void *, int64_t)` word shim, and the capturing one's thunk takes
its fn-typed parameter as `int64_t`.  So the decision above stands as the
next step, not more adapters.

One clang-only wrong answer turned up and was a FIXTURE bug, not a compiler
one: `rc-of-byvalue-aggregate-payload`'s own `tag-of` read an 8-byte word where
the tag is a C `int`, taking in four bytes of struct padding that a struct copy
need not preserve (clang at `-O1`+ copies member-wise).

## Fixed on the way (not open)

The sweep and the widened fuzzers found these, and all are fixed with
fixtures:

- `saffron-dyn-witness-fn-arity-defaults-unary` (archived): the unary
  `g : fn` case printed a TYPE TAG.
- a bare `fn` struct field returned a float's BITS; storing a float-signature
  function there, or calling one with a float argument, is now rejected, as
  the `:fn` parameter carrier already was
  (`errors/fn-field-bare-float-{store,arg}-rejected`);
- an effectful TYPED fn field aborted ("no CPS entry registered"), and its E2a
  call value-converted a float argument into the wrong register
  (`fn-field-typed-float`).
- a lambda literal lifted ONCE with the carrier signature was called in a spec
  through a pointer typed from the arguments' resolved types:
  `((fn [y : A] : A y) x)` printed `0` at float32 (the float went in `xmm0`, the
  thunk read `rdi`), and a by-value struct argument segfaulted.  The call now
  follows the callee's EMITTED signature -- the fix direction below, applied to
  the call-head path -- and a closure literal's fat dispatch takes its return
  slot from the clone's recorded signature
  (`generic-spec-carrier-crossings-2`).

## Fix direction

One decision per call shape, keyed on the callee's EMITTED signature, not on
the erased type at the call. That is the same lesson as
[repr-decision-function-plan](repr-decision-function-plan.md). The
concrete moves, largest cluster first:

1. The session wrapper calls `void (*)(void *)`: `session-spawn` takes
   `(fn [] nil)`.
2. The boxing site picks a typed fatshim whenever the call site casts typed
   (`TUR_APPLY<N>_T`). Today `ensure_typed_fatshim` declines every all-word
   signature, and the call site still casts `bool` or a pointer type.
3. A variadic's rest slot has one C spelling, at the definition and in the
   shim.
4. The stdlib inline-C comparators and callbacks take a real function type
   (the `stdlib-int-stand-in-audit` S1 subset is the same list).  **Done for
   the comparators 2026-10-02** (eighth sweep); the callbacks remain.

When the sweep is at zero: add the flags above as a Linux CI leg, flip
`TUR_FUZZ_FNSAN_STRICT` on by default, and ratchet it like the
`-Wfloat-conversion` check in `tests/run.sh`.  **Done 2026-10-02**: the
`fnsan` job runs `tests/run-fnsan.sh`, and the fuzzers fail on a trap.

## The `ptr<void>` decision (2026-10-02)

The eighth sweep left one question: a program erases a closure to `ptr<void>`
and calls it back as `(fn [ptr<void>] ...)`, so its function-typed parameter
(`int64_t`, ER4) and the caller's `ptr<void>` one (`void *`) must be ONE
spelling.  Two candidates, and the choice was the word:

- **`ptr<void>` -> `int64_t` (taken).**  Touches only signatures that have a
  `ptr<void>` parameter -- httpd handlers, reactor and fiber callbacks, image
  deserializers, SF pipelines -- and makes them closer to the erased word
  convention every carrier consumer already uses.  The definitions keep
  `void *` (a slot-0 entry converts), so no body, forward declaration or CPS
  twin changed.
- **fn -> `void *` (rejected).**  Every higher-order closure has a function
  parameter, and every erased consumer calls slot 0 at words: it would have
  traded four traps for an adapter at every erased sink of every HOF.

Results were left alone: a `ptr<void>` result and a fat function result are
both `void *` already.  The rule is recorded in
[value-representations-guide](../guides/value-representations-guide.md#slot-0s-signature-which-parameters-are-the-word).
