# Constrained-generic monomorph passes a pass-by-ptr aggregate by value

**Severity:** high -- any constrained generic over a pass-by-ptr aggregate fails
to compile. Not a miscompile: `cc` rejects it, so nothing ships silently.

**Status: RESOLVED 2026-10-03 (archived).**  Three emitter faults, all
fixed; see "Fixed" at the end.

Found while fixing the sibling defect in the dict wrapper
(`dictwrap-passbyptr-param-convention`, fixed); this one is in the
**monomorph** path and is untouched by that change.

## Repro

```turmeric
(defstruct Reg [a : int b : int c : int])   ;; 3 words -> pass_by_ptr

(defclass Join [t]
  (join [x : t y : t] : t))

(definstance Join [Reg]
  (join [x y] y))

;; The constrained generic is the trigger. Calling `join` directly is fine.
(defn merge-two [T] [(Join T)] [p : T q : T] : T
  (join p q))

(defn main [] : int
  (let [r (merge-two (make-struct Reg 1 2 3)
                     (make-struct Reg 10 20 30))]
    (println (.a r)))
  0)
```

```
error: passing 'tur_adt_Reg' to parameter of incompatible type 'const tur_adt_Reg *';
       take the address with &
```

## Mechanism

The instance impl is emitted by-pointer, which is correct:

```c
static tur_adt_Reg __inst_Join_join_Reg(const tur_adt_Reg * x, const tur_adt_Reg * y);
```

The monomorphized caller takes its own parameters **by value** and then passes
one argument with `&` and the other without:

```c
static tur_adt_Reg merge_two__spec__tur_adt_Reg_tur_adt_Reg_tur_adt_Reg(
        tur_adt_Reg p, tur_adt_Reg q) {
        tur_adt_Reg __t177 = p;
        tur_adt_Reg __ps_178 = (__inst_Join_join_Reg(&__t177, q));   /* `q` needs & */
        ...
}
```

The first argument gets a spill temporary and its address; the second does not.
So the address-taking is driven by something per-argument (the spill) rather
than by the callee's parameter convention.

A second emission in the same file looks wrong for the same reason -- the
specialized instance body derefs a parameter it declared by value:

```c
static int64_t __inst_Join_join_Reg__spec__int64_t_tur_adt_Reg_tur_adt_Reg(
        tur_adt_Reg x, tur_adt_Reg y) {
    ... *__tur_ret_p = *(y); ...     /* `y` is not a pointer here */
}
```

Both suggest the monomorph path decides the parameter convention
independently of `type_struct_pass_by_ptr`, where the impl, the dict slot
typedef and (now) the dict wrapper all agree on it.

## Fix direction

`src/compiler/emit_stmt.c` already has one predicate for this, used by the slot
typedef and by `dict_slot_param_is_carrier`:

```c
!mi->closure && !body_is_inline_c && type_struct_pass_by_ptr(pt)
```

The monomorph/specialization path should consult the same predicate when it
emits a `__spec__` signature and when it builds the argument list for a call
into an instance impl, so all four sites agree. The dict-wrapper fix in this
same change is the one-branch version of exactly that.

## Not the same as

`tests/fixtures/dictwrap-passbyptr-param-convention` pins the **dict wrapper**
convention and passes. It deliberately calls `join` directly rather than
through a constrained generic, precisely so it does not also trip this defect;
the comment in that fixture says so.

## Fixed (2026-10-03)

Three separate faults, each needed for the repro and its neighbours:

1. **Only the receiver was spilled.**  `emit_reresolved_receiver_is_by_ptr`
   (emit_core.c) answered for argument 0 only -- "the receiver / dispatch
   tyvar".  It is now `emit_reresolved_param_is_by_ptr(ctx, call, i)`, asked
   for every argument of a dictionary-dispatched call re-resolved in a spec:
   an argument whose resolved type crosses the pass-by-pointer threshold is
   spilled and its address passed, unless it is already a `const T *`
   parameter or an earlier branch already took its address.  The receiver
   keeps its original contract (a type-variable receiver only); an `int`
   beside the structs is untouched because its own type is not an aggregate.
   (The instance FnDef's recorded `param_types` are NOT a reliable second
   witness -- a 5-field struct receiver read as not-pbp there -- so the
   argument's resolved type is the test.)
2. **A spec clone's by-value params were registered as pbp.**  `emit_fn_def`
   pushed every pass-by-pointer-typed parameter onto `pbp_param_ptrs`, but an
   ABI-spec clone spells its parameters from the spec, BY VALUE (the
   `use_abi_spec` arm of the signature loop), so every use took the pointer
   path: `*(y)` on a `tur_adt_Reg y`.  A spec clone registers none.
3. **A `let`-bound method result took the representative's type.**
   `(let [pq (merge-two p q)] (join pq r))` declared its merge temp
   `tur_adt_Too` inside the `Two` clone: `call_ordinary_defn_byval_aggregate`
   read the declared result of the call's `fn_binding`, which for a
   dictionary-dispatched call is the elaborator's representative instance
   method.  It now asks the method the active spec re-resolves to
   (`emit_reresolve_method_fndef`).  Pre-existing and independent of the
   pass-by-pointer change (the unmodified build failed the same way, plus the
   faults above).

Pinned by `tests/fixtures/constrained-generic-passbyptr-args` (two- and
three-struct-argument methods, an `int` beside them, a generic relaying to a
generic through a `let`, a wide sum type; every line equals `tur
--interpret`; runs natively under `tur jit`).  `run.sh` 3554/0.
