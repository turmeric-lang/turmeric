# The CPS backend still evicts some `handle`s over effectful fn fields

**Severity: medium.** A legal program is refused at build time with "this
effect operation has no lowering here". This is an expressiveness gap, not a
miscompile. It is what remains of the type fuzzer's `GEN_REJECT`s after
`handle-over-effectful-fn-field-in-arg-let-evicted` (archived) was fixed.

**Narrowed again 2026-10-03: item 4 (seed 4444 case 93) is fixed; only (3)
remains, now reduced to a five-line repro and investigated -- see "Item 4
fixed" and "Item 3, reduced" at the end.**  Seeds 3333, 4444, 5555 and
6666: 0 `GEN_REJECT`s and 0 bugs in 300 each.

**Narrowed 2026-10-03: item 5 (seed 3333 case 121) is fixed; (3) and (4)
remain.**  It was not the generic-over-rank-2-over-`handle` the shape
suggested.  Reduced, it needed only a `handle` in `main` plus a call to a PURE
generic whose body applies a lambda literal on the spot -- `(let [b x] ((fn []
b)))`.  The coloring pass (`cps_collect_calls`, cps.c) counted that applied
literal as an unresolved call, so the generic was colored; its signature then
kept it off the CPS backend, but `main` still translated the call as
cps->cps, with a join over its fat-closure result that is not
slot-representable (the `CT_LETCONT` `TUR_TRACE_CORE` named).  An applied
literal -- directly, or through the `__call_head_N` temp the elaborator hoists
it into (`closure_head_init`) -- has exactly one callee, its own lifted
lambda, so it is now an edge to that lambda's node: a pure lambda leaves the
generic uncolored, an effectful one still colors it.  Pinned by
`tests/fixtures/handle-beside-applied-lambda-generic`.  Seed 3333: 0
`GEN_REJECT`s in 300 (was 1); seed 4444: 1, item (4).

Found on the way, pre-existing and unchanged: a lambda literal that itself
PERFORMS, applied on the spot inside a function called under a `handle` --
`(defn app [] : cstr ((fn [] (perform (E "hi")))))` -- is "no lowering here"
compiled (it runs under `--interpret`), on the old coloring and the new.

**Narrowed 2026-10-01 (second pass): shapes (1) and (2) are fixed, plus two
neighbours the fixes exposed; (3) and (4) remain.**  Fuzz seeds 3333 and 4444
went from 5 `GEN_REJECT`s in 600 cases to 2 (one per seed): seed 3333 case 121
(below, "What is left") and seed 4444 case 93, shape (4).

## Fixed (2026-10-01)

- **(2) `unsupported form: EX_REINTERPRET`.**  A generic call whose result the
  elaborator re-typed to its instantiation is wrapped in a Tier A reinterpret
  (`int -> cstr`).  `safe_to_delegate` had no arm for it, so even a reinterpret
  over a pure call evicted the function; it is now exactly as delegatable as
  the value it converts.  A reinterpret whose operand carries control
  (`(g2 (handle ...))`) is decomposed: the operand into a binder of its own
  type, then the retype delegated over that binder (`cps_bind_reinterp`).  The
  previous attempt changed `cps_bind`/`cps_tail` only, which is why the trace
  did not move: the refusal came from `safe_to_delegate`.  Pinned by
  `tests/fixtures/handle-under-generic-call-reinterpret`.
- **(1) A rank-2 / fn-value (indirect) call with a `handle` operand.**  An
  indirect call was delegated only with LITERAL arguments.  When an argument
  carries control, the arguments are now bound first, left to right, and the
  call delegated over the binders (`delegate_call_atomized`); a constrained
  forall's dictionary argument (`EX_DICT`) rides inline.  In BIND position an
  indirect call whose arguments are merely non-literal is delegated whole --
  that was what evicted a `main` holding a `handle` for an unrelated
  `(.app s (g "x"))`.  TAIL position keeps the literal rule: delegating a tail
  indirect call turns it into a bind and a return, and the r7rs programs that
  recurse through closures ran out of stack (seen and reverted while landing
  this).  Pinned by `tests/fixtures/handle-in-rank2-call-operand`.
- **A rank-2 argument atomized ahead of a `handle`** -- `(ru ri (handle ...))`
  -- emitted INVALID C (pre-existing, on every compiler): the handle's
  continuation captured the `tur_poly_fn_t` binder and declared its env field
  `void *`.  The capture set now knows the fresh binders a poly-wrap
  `CT_LETRAW` binds (`cvar_is_polyfn`).  Same fixture.
- **A cps->direct call to a resolved clone** passed an atomized carrier word
  where the clone takes `const char *` (a `-Wint-conversion`, a hard error on
  clang / gcc 14); `atoms_csv_call_clone` bridges a variable whose recorded C
  type differs from the clone's parameter.

`TUR_TRACE_EVICT=1` now names a reinterpret's kinds and the form it wraps
(`EX_REINTERPRET int -> cstr of EX_CALL`), and `EX_CALL` / `EX_GET_FIELD` /
`EX_DICT` by name instead of `EX_#n`.

## What is left

3. **A signature-rejected generic HOF** (`l2gbapp11 SIG-REJECT`) on the path
   from the handler to the performer.
4. **A join continuation that would capture the field-load callee.**
   `(handle (.run (make-struct FE fe) (let [c false] (t (fn [] c)))) ...)`
   is `BODY-STRUCT-JOIN` (`needs_heap_join`).  Before the E2c capture fix
   (`CC_ATOM` / `COL_ATOM` of `tailcall.fn_atom`, 2026-10-01) the join read the
   callee atom uncaptured, which was invalid C (`use of undeclared identifier
   '__t2'`).  It is a clean refusal now, not admitted.  Seed 4444 case 93.
5. **FIXED 2026-10-03 (see the top).  Seed 3333 case 121**: `(l0gb8 (l0ru7 l0ri6 (handle ...)))` -- a generic
   call over a rank-2 call over a `handle`.  The reinterpret and the rank-2
   operand both lower now; what still fails is `main`'s structural core check
   (`BODY-STRUCT-CORE`; `TUR_TRACE_CORE=1` names a `CT_LETCONT` whose join
   parameter is not slot-representable -- the innermost reject, with nothing
   failing beneath it).  Not yet reduced further.

## Regression check

The fuzzer's `GEN_REJECT` count on a fixed seed: `python3
tests/type-fuzz-src.py --n 300 --seed 3333`, which is 0 since 2026-10-03,
as are seeds 4444 (was 1, item 4), 5555 and 6666.  Run it against a COPY of
the binary (`--tur`) if anything may rebuild `build/tur` meanwhile.

## Item 4 fixed (2026-10-03)

`(handle (.run (make-struct FE fe) (let [c false] (t (fn [] c)))) ...)`: the
argument is not atomic, so it is bound by a join lifted into a DK frame, and
that frame makes the field call -- so it captures the field-load callee
`(.run s)`, which the CPS IR atomizes into a fn-typed CVar.  `cap_add_cvar`
refuses a `TY_FN` CVar (it is not a slot type), `collect_caps` failed, and
`needs_heap_join` evicted `main` (`BODY-STRUCT-JOIN`).  The callee is the
same int64 direct-entry word a fn-value PARAM callee is, which already rides a
frame env as one (`cap_add_fn_scalar`); `cap_add_cvar_fn_scalar` does the
same for the field-load CVar, and the frame's `emit_e2a_fat_dispatch` reads
it from there.  Pinned by `tests/fixtures/handle-field-call-joined-arg`
(the fuzzer's case plus an altered resume value, an int field, and a handler
that keeps working after `resume`; every line equals `tur --interpret`).

Found on the way, and fixed: **two sequential `handle`s with a closure
literal in the second** were invalid C on `main` --
`'__tur_widen___fn_N' undeclared`.  The second body is lifted into the
first's continuation frame, emitted through the direct emitter, and the
closure's slot-0 widen wrapper went to `pending_handler_fns`, which the CPS
function emitter flushed AFTER its lifted helpers.  It flushes them first now
(they name only file-scope functions the forward declarations cover).
`tests/fixtures/handle-sequential-widened-closure`; two snapshots
(`defstruct-field-handler*`) move by the same reordering.

Also found (pre-existing, both back ends; since fixed): a struct TEMPORARY with
a fn field -- `(.run (make-struct S f) x)` -- leaks its 24-byte fn-field box;
a let-bound one is freed.  Filed as
[struct-temporary-fn-field-box-leaks](../archive/struct-temporary-fn-field-box-leaks.md).

## Item 3, reduced and investigated (2026-10-03)

No seed reproduces it any more, but the shape does, in five lines:

```turmeric
(defeffect E [x : int] :int)
(defn gapp [B] [f : (fn [B] B #fx{E}) v : B] : B (f v))
(defn eff [x : int] : int (perform (E x)))
(defn main [] : int
  (println (handle (gapp eff 1) (E [x] k) (resume k (+ x 1))))
  0)
```

"this effect operation has no lowering here" compiled; `--interpret` prints
2.  The NON-generic twin (`[f : (fn [int] int) v : int] : int`) works.  The
explicit `#fx{E}` row makes no difference.

What is in the way, measured:

1. **There is no clone to admit.**  A colored generic reaches the CPS backend
   as a mono-template that stands in for its ABI-specialized clones
   (`mono_template_all_admissible`).  `B := int` is the erased carrier
   instantiation, so no ABI spec is registered (`n_specs` has none for
   `gapp`), `any` is false, and the generic is not a template.  Its BASE
   sig-rejects (`fn_sig_ok`: a `TY_TYVAR` parameter and result), so it is
   `sig_perm`, its call path perm-taints `E`, and `eff` and `main` follow it
   to the fiber -- where `perform` has no lowering.
2. **Admitting the spec-less base is not enough.**  A prototype that let
   `fn_sig_ok` take a bare `TY_TYVAR` parameter/result as the int64 word when
   the generic has no ABI spec at all got past the signature, then failed the
   body check (`BODY-STRUCT-CORE`: `atom_ok` refuses every tyvar-typed atom,
   at the final `CT_APPCONT`), so `atom_ok` / `slot_store` would need the
   same carrier reading.
3. **And it exposed a soundness edge that must not ship.**  With an EMPTY-row
   parameter receiving the effectful function -- `(fn [B] B)`, which the
   default (non-strict) checker accepts -- the function's eviction stops
   being `sig_perm`, and a BODY eviction credits the call through `f` with
   `f`'s declared (empty) row.  Nothing tainted `E` any more, `main` stayed
   CPS and called `gapp` cps->direct, and the program aborted at run time with
   "unhandled effect" instead of being refused.  The `sig_perm` taint is what
   is keeping that case honest today.  The prototype was reverted.

Fix direction: either mint a spec (a clone) for a COLORED generic's carrier
instantiation too, so the existing mono-template path admits it; or admit the
spec-less base with tyvar-as-word reading in `fn_sig_ok`, `atom_ok` and the
slot store -- and in both cases make an effectful fn-value flowing into an
empty-row fn parameter taint like the `sig_perm` path does (or refuse it
under the lenient checker, as `--strict-effects` already would).

## Item 3, taken further (2026-10-09) -- three walls, in order

It is wider than a fuzz residue: stdlib's own generic HOFs hit it.
`(handle (option-map (some 1) (fn [x : int] : int (perform (E x)))) (E [x] k)
(resume k (+ x 1)))` is "no lowering here" compiled (`--interpret` gives 2).
The non-generic empty-row twin, `(defn app [f : (fn [int] int) v : int] ...)`,
compiles and is right with a named or a lambda callback.

A prototype (reverted) got through two of the three walls:

1. **No clone.** `emit_abi_register_call` (`emit_module.c`) mints a
   colored generic's clone only when its OWN body suspends
   (`emit_cps_ir_colored_fn_needs_mono`); `gapp`'s body only calls `f`.
   Minting one when the call passes an effectful fn value (a named fn or
   lambda whose inferred row is runtime-impure, or that may await) works:
   `gapp__spec__int64_t_int64_t_int64_t` is interned and
   `mono_template_all_admissible` accepts it.
2. **The callback is tiered `e1`.** `param_thread_class` returns `PT_E1` for
   any fn whose base `fn_sig_ok` fails, so `eff` was never threadable and
   perm-tainted `E` (with it `main`, the handler, fell off the backend --
   `SIG-MAIN` in the trace is that, not main's signature). Asking whether
   every monomorph's signature is admissible (`mono_sig_ok` over the specs)
   instead tiers it `now`.
3. **The call through `f` does not thread.** `fnval_withdraw_walk` then
   withdraws `eff`, because `cps_ir_param_call_threads` fails for `(f v)`
   in the generic body: the parameter's binding has NO effect row even when
   one is written (`(fn [B] B #fx{E})` -- `effect_row` is NULL on the param
   binding, so `call_is_effectful_fnvalue` says no), and it is not
   `is_poly_fn`, so the empty-row `fn_cps` path (`fncps_param_call_ok`)
   refuses too. The non-generic twin's parameter is fat-normalized; the
   tyvar-typed one is not, and the clone shares the base's Binding.

So the remaining work is in the elaborator: carry a written effect row onto a
tyvar fn parameter's binding, and fat-normalize a tyvar fn parameter as a
concrete one is (or decide it per clone). Walls 1 and 2's changes are small
and can be re-applied from the description above once wall 3 is down.
