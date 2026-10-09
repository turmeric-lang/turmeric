# The CPS backend still evicts some `handle`s over effectful fn fields

**Narrowed 2026-10-09: the float instantiation of item 3 is fixed** (see "The
float instantiation, fixed" at the end).  **What is left:** an UN-annotated fn
parameter of a generic (stdlib's `option-map` shape).

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

## Item 3, taken further (2026-10-09): a written row now works

**First, the repro was wrong, silently.** `(fn [B] B #fx{E})` puts the row
AFTER the result; the type parser read the result and dropped everything
after it without a word, so the parameter was effect-free -- which is why
"the explicit `#fx{E}` row makes no difference" above. The row goes before
the result, `(fn [B] #fx{E} B)`, and a form after the result is now an error
naming that (`tests/fixtures/errors/fn-type-row-after-result`).

**With the row in place, three things stood in the way, all fixed:**

1. **No clone.** `emit_abi_register_call` (`emit_module.c`) minted a colored
   generic's clone only when its OWN body suspends. It now also mints one
   when the generic has a fn parameter with a non-empty effect row, or the
   call passes a fn value whose inferred row is runtime-impure or that may
   await (`emit_abi_fn_has_effectful_fn_param`,
   `emit_abi_call_passes_effectful_fn`); `mono_template_all_admissible`
   accepts the clone.
2. **The callback was tiered `e1`.** `param_thread_class` returned `PT_E1`
   for any function whose base `fn_sig_ok` fails; a generic whose every
   clone's signature is admissible (`mono_sigs_all_ok`) now tiers by its
   body like a concrete function, so the callback threads.
3. **A generic nothing calls tainted its effects.** An unused
   `(defn g [B] [f : (fn [B] #fx{F} B) ...])` is SIG-REJECT with `F` in its
   set, so every `perform` of `F` in the program was refused. A generic with
   no clone, no carrier call and no address taken (`generic_unreached`) now
   counts as a mono-template: it stands in for nothing and taints nothing.

Also fixed on the way, in the effect pass: a call through a ROW-VARIABLE
parameter, `(fn [int] #fx{e} int)`, charged nothing to the caller unless the
callee also declared `#fx{e}` itself, so a handler around
`(gp eff 20)` drew a false TUR-W0033 (generic or not). The argument's row is
charged at the call now, as it already was for an un-annotated parameter.

Pinned by `tests/fixtures/generic-hof-effectful-callback` (leak-checked): a
named, a lambda, a capturing lambda and a pure callback through
`(fn [B] #fx{E} B)`, a row-variable parameter, and an unused generic over
another effect. `tests/type-fuzz-src.py --n 300 --seed 3333`: 282 ok, 18
SEAM_REJECT, 0 bugs -- the same as before the change.

### Still open

- **An UN-annotated fn parameter of a generic** -- stdlib's `option-map`,
  `(handle (option-map (some 1) (fn [x : int] : int (perform (E x)))) ...)`.
  A concrete function's un-annotated `(fn [int] int)` parameter is
  fat-normalized (the `tur_poly_fn_t` carrier, whose `fn_cps` slot threads
  an effectful callback); a type variable in the signature keeps it thin
  (`repr-trace ... thin-fn tyvar-sig`), and the clone shares the base's
  Binding, so the call through it threads by neither route.
- ~~**A float instantiation**~~ -- fixed 2026-10-09, below.

## The float instantiation, fixed (2026-10-09)

`(gapp2 (fn [x : float] : float (perform (F x))) 7.1)` through
`(fn [B] #fx{F} B)` had three gaps. Only the first refused; the other two
were miscompiles behind it, found by admitting the call. The program
segfaulted then.

- **The result retype.** The erased generic's int64 result comes back to a
  float through a NON-Tier-A reinterpret (`int -> float`). The bind and tail
  paths decomposed only a Tier A reinterpret over a control-bearing operand.
  Now any reinterpret does (`cps_bind_reinterp`, `src/passes/cps_ir.c`): the
  operand is bound to a binder of its own type, and the direct emitter spells
  the retype of that binder.
- **The argument into the clone.** The elaborator erases a generic call's
  arguments for the base, so `main` held 7.1 as its carrier bits. The cps->cps
  call to the monomorph clone `gapp2__spec__double_...(int64_t f, double x,
  ...)` passed those bits to `double x`, which C converts numerically.
  `atoms_csv_call_typed_offs` (`src/compiler/emit_cps_ir.c`) now reads an
  int64 atom given to a clone's `double` / `float` parameter back by its bits,
  the reverse of the existing float-into-carrier rule.
- **The fn-value call inside the clone.** The E2a dispatch cast the callee's
  parameter from the signature's KIND, which is the type variable, so it
  spelled `int64_t`, and C converted the double numerically.
  `e2a_param_ctype` now resolves the parameter's full type through the active
  monomorph (`g_cps_mono_resolver`).

Found on the way and fixed: the `cstr` instantiation of the same generic did
not build under clang. Its E2a dispatch passed the `const char *` argument
into the `int64_t` slot bare (`-Wint-conversion`; gcc only warned).
`atoms_csv_call_cps` now reads the argument's kind through the active
monomorph too. It was reachable before this change: a `cstr` result retype is
Tier A.

Pinned by `tests/fixtures/generic-hof-effectful-callback-scalar`. It covers:
- `float`, `float32`, `bool` and `cstr` instantiations;
- a named function and lambdas;
- a non-tail call;
- a two-argument float callback.

Every line equals `tur --interpret`. The fixture is leak-checked, passes the
JIT harness, runs clean under `-fsanitize=function`, and has no finding from
`check-emitted-float-conversions.py`.
