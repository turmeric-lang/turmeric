---
title: Typeclass Dictionary Internals
category: Compiler Internals
description: How the compiler resolves a typeclass method call to an instance (receiver- and return-directed), what the call records, how constrained generics reach the right instance (monomorphization, dict clones, captured dictionaries), superclasses, default methods, HKT instances on the by-value path, instances across modules, how `definstance` lowers to a C dictionary struct + singleton, the closure-handle convention, and the failure shapes with their diagnostics.
tags:
  - typeclasses
  - codegen
  - type-checking
---

# Typeclass Dictionary Internals

This guide is for contributors working on how `defclass`, `definstance` and a
class-method call are elaborated and emitted on the compiled path (`tur
build`, `tur run`). It covers the two halves of the machinery:

- **Resolution and relay** -- how a call picks its instance, what it records,
  and how a constrained generic reaches the instance its caller chose.
- **Lowering** -- how an instance becomes a C dictionary struct, a singleton
  and a set of `__inst_*` functions, and the C-type rules that keep the three
  views of one method in agreement.

The user-level surface is in the [typeclass guide](typeclass-guide.md); this
guide does not repeat it. For the type-level machinery of higher-kinded classes
see the [HKT guide](hkt-guide.md); for the spec-symbol model see the
[monomorphization ABI guide](monomorphization-abi-guide.md).

> **Trust the code over the header comments.** The resolver has been patched
> one report at a time for a long while, and many comments describe the state
> before the next patch. Examples found while writing this guide: the comment
> above the receiver search in `elab_method_call` says it calls
> `typeclass_env_lookup_instance_by_key` (it does not -- only the interpreter
> does); `typeclass.h` says superclasses are gated behind
> `--enable=class-superclasses` (they graduated on 2026-09-25); and
> `emit_stmt.c` says "a program with only STATIC dispatch emits no dict at
> all" (it does -- see [Dead-instance elimination](#dead-instance-elimination)).
> Where a comment and this guide disagree, read the code. If the code
> disagrees with this guide, fix the guide.

---

## The short version

1. **Registration.** `defclass` registers a `TypeClass` and `definstance` a
   `TypeClassInstance` in one global `TypeClassEnv` per compilation. Both
   lists are prepended to, so iteration order is **newest first**. Instances
   have no export list; they are global.
2. **Resolution happens in the elaborator.** A bare `(m x ...)` is routed to
   `elab_try_return_dispatch` when `m` is a return-directed method, otherwise
   rewritten to the dotted `(.m x ...)` and handed to `elab_method_call`, which
   selects an instance from the receiver's static type.
3. **A resolved call is a direct call.** The `EX_CALL` gets
   `fn_binding = <the instance method's binding>` -- emitted as a call to
   `__inst_<Class>_<method>_<T>` -- plus a `dict_arg` (an `EX_DICT` naming the
   instance and method) that is an annotation for later passes, not the
   callee.
4. **Constrained generics are monomorphized by default.** Inside
   `(defn f [A] [(C A)] ...)` a call on an `A` receiver binds to a
   *representative* instance so the carrier base clone is valid C, and each
   `f__spec__...` specialization re-resolves it at emit time
   (`emit_reresolve_method_call`).
5. **Dictionaries are passed only where monomorphization cannot reach**: a
   constrained generic used as a rank-2 value, and a direct call to a
   constrained *higher-kinded* generic. Those calls go through a **dict
   clone** (`f__dict_N`) that takes one `int64_t` dictionary per constraint
   and loads each method out of slot `k` of that dictionary.
6. **Lowering.** Every live instance emits `__inst_*` method functions, a
   `dict_<Class>_<T>` struct and a `dict_<Class>_<T>_singleton`. A slot whose
   C signature cannot match the uniform dict-call shape is filled with a
   `__dictwrap_*` shim.

---

## Source map

| File | Role |
|---|---|
| `src/compiler/typeclass.h`, `typeclass.c` | Data model (`TypeClass`, `TypeClassMethod`, `TypeClassInstance`, `TypeConstraint`); the registry; `typeclass_env_lookup_instance` (constraint discharge, kind-erased); `typeclass_env_lookup_instance_exact` (structural); superclass entailment and closure |
| `src/compiler/elab_typeclasses.c` | `elab_defclass`, `elab_definstance` (instance registration, method elaboration, default splicing, duplicate and orphan checks), `elab_method_call` (receiver-directed resolution), `elab_try_return_dispatch` (return-directed resolution), `make_dict_expr`, `elab_typeclass_superclasses_finish` (post-unit superclass pass) |
| `src/compiler/elab_call.c` | Bare-name routing to the two resolvers; `make_dict_clone`, Route B and ambient-dict forwarding; `prefer_method_dispatch` |
| `src/compiler/elab_fns.c` | Per-defn constraint publication (`cur_fn_constraints`, the ambient HKT dict binding); `call_dispatch_is_static` |
| `src/compiler/emit_stmt.c` | `EX_INSTANCE_DEF`: dict struct, `__dictwrap_*` shims, singleton; the `dict_slot_*` predicates |
| `src/compiler/emit_core.c` | Emit-side re-resolution (`emit_dispatch_tyvar`, `emit_reresolve_disp_type`, `emit_reresolve_method_call`); dict-param and env-dict dispatch (`emit_call_name`); `emit_dict_name`; `emit_inst_fn_return_carrier` |
| `src/compiler/emit_expr.c` | `EX_DICT` (singleton field or singleton address); the dict-slot direct-call path |
| `src/compiler/emit_module.c` | Liveness (`emit_instance_is_live`, the `EX_DICT` arm of `emit_abi_scan_expr`); `emit_abi_register_call` (spec interning from `abi_bindings`); `emit_inst_method_wants_external` (cross-module linkage) |
| `src/compiler/elab_module.c` | `elab_noinst_retry`: park a defn in an imported module until an importer registers an instance |
| `src/turi/eval.c` | The interpreter's own dictionary binding (`frame_bind_one_dict`); not covered here |

---

## Data model

`TypeClassEnv` holds two singly-linked lists, `typeclasses` and `instances`,
each prepended by `typeclass_env_register_typeclass` /
`typeclass_env_register_instance`. Every search that "takes the first match"
therefore takes the **most recently registered** one. The resolver relies on
this in a few places and guards against it in others (see
[Ambiguity](#ambiguity)).

The fields that matter for resolution:

| Struct | Field | Meaning |
|---|---|---|
| `TypeClass` | `type_params`, `type_param_kinds` | `[a]` or `[^f]`; a non-`KIND_STAR` kind marks an HKT class |
| | `methods[]` | In declaration order. This order is also the **dict slot order** |
| | `super_forms`, `supers` | The `[(Super a)]` preamble, unresolved until the post-unit pass |
| | `from_stdlib` | Set when registered while an auto-loaded stdlib module was elaborated |
| `TypeClassMethod` | `param_types`, `return_type` | The class signature; class variables appear as `TY_TYVAR` |
| | `param_explicit_type` | Whether the user annotated a parameter; an unannotated one is the class variable |
| | `default_method_form` | The unelaborated method form when the class wrote a default body |
| `TypeClassInstance` | `type_args`, `type_arg_syms` | The head; `type_arg_syms` keeps the source name for mangling |
| | `method_impls[]` | One `FnDef` per class method, in class order |
| | `type_param_constraints` | `[(Show A)]` on a parametric instance; `param_idx` says which head argument |
| | `partial_hole_pos` | The `_` slot of a `(Result _ B)` head, or `0xFF` |
| | `origin_file_id` | Used for the orphan check, user-vs-stdlib tie-breaks and linkage |
| `TypeConstraint` | `typeclass`, `tyvar` | One `(C A)` on a defn |
| | `return_resolved` | The tyvar appears only in the result |

There are two instance matchers in `typeclass.c`, and they answer different
questions:

- `typeclass_env_lookup_instance` compares **kinds** (with ADT identity for
  `TY_ADT` heads), then checks the candidate's own `type_param_constraints`.
  A `TY_TYVAR` in a required position is accepted tentatively. This is what
  discharges constraints and what the return-directed resolver uses.
- `typeclass_env_lookup_instance_exact` compares the whole type with
  `type_eq`. Associated-type projection and fundep coherence use it.

The receiver-directed resolver (`elab_method_call`) uses **neither**. It has
its own loop, described next, because it has to tell apart cases the
kind-erased matcher collapses.

---

## How a method call picks its instance

### Routing a bare call

`elab_call.c` decides which resolver sees a call whose head is not bound to a
user defn (`!fn_binding`):

1. If the name is a **return-only** method of some class --
   `elab_try_return_dispatch` sets `handled` -- the return-directed resolver
   owns it.
2. Otherwise, if the name is a class method and the call has an argument, it
   is rewritten to the dotted form `(.m x ...)` and handed to
   `elab_method_call` (the "GHE1" path).

A user defn of the same name normally wins (the `!fn_binding` gate). The
exception is `prefer_method_dispatch`: when the name is also a method of a
**user** class and an instance of that class matches the first argument's
static type (`elab_user_method_instance_matches`), the method wins. A stdlib
class method never beats a user defn, which keeps "override a stdlib method
with a defn" working. See
[TUR-W0039](#tur-w0039-method-name-shadowing-a-free-defn).

### What "return-directed" means

A method is return-directed when some class type parameter appears in its
return type and in **no** parameter type:

```turmeric
(defclass Default [a] (default-of [] : a))       ;; return-directed
(defclass Applicative [^f]
  [(Functor f)]
  (pure [x : a] : (f a))                          ;; return-directed
  (ap [ff : (f (fn [a] b)) fa : (f a)] : (f b)))  ;; receiver-directed
```

The test is `method_is_return_dispatch`. Its twin,
`elab_symbol_is_return_dispatch_method`, is also used by
`elab_forms.c:if_form_is_return_dispatch`, so a concrete sibling `if` arm can
supply the expected type. A method whose result is one of the class's
**associated** types (`(empty [] : Inner)`) also counts; its dispatch variable
is the class's first parameter.

### Receiver-directed resolution (`elab_method_call`)

The receiver (`items[1]`) is elaborated first; its static type drives the
search. The function tries, in this order:

1. **Type witness.** `(.m @T x ...)` -- the reader turns `@T` into
   `(deref T)` -- pins the instance whose head names `T`. An `any` receiver
   is unboxed to `T` with the checked unbox.
2. **Existential `open`.** A receiver bound by `(open e [a v] ...)` over a
   constraint-carrying existential dispatches through the packed witness
   table (`EX_EXISTS_DISPATCH`), not through instance search. See the
   [existential types guide](existential-types-guide.md).
3. **Abstract type variable.** When the receiver's type is a `TY_TYVAR` (or
   an unascribed carrier element, `obj_is_unascribed_carrier_elem`):
   - If the enclosing defn does not constrain that variable to the method's
     class (or a subclass of it -- `typeclass_entails`), the call is
     **TUR-E0015** "does not constrain 'A' to it". Inside a `definstance`
     body the check is skipped.
   - Otherwise a **representative** instance is chosen so the polymorphic
     base clone is valid C, in tiers: a `TY_INT` head; else a scalar that
     rides the `int64_t` carrier (`cstr`, `bool`, sized ints, `ptr<void>`,
     `Sym`); else a non-pointer `defopaque` newtype. Floats and aggregates
     are never representatives. The representative is a placeholder; see
     [Constrained generics](#constrained-generics-reaching-the-callers-instance).
4. **`any` receiver with a genuine `C [any]` instance** takes that instance.
5. **The general search** over every instance with a method of that name:
   - A primitive receiver (`KIND_STAR` here) matches an instance whose head
     has the **same `TypeKind`**.
   - A non-primitive receiver matches a non-primitive head, narrowed by:
     the same ADT definition (bare or applied, after walking a `TY_APP` to
     its head); concrete element types equal when both sides are concrete
     (`(Option cstr)` is not `(Option int)`); a function head only for a
     function receiver; an `any` head only for an `any` receiver; a bare
     type-variable head demoted to a fallback when the receiver is a
     concrete nominal type.
   - A head match must also satisfy the instance's own
     `type_param_constraints` for the receiver's element types
     (`typeclass_instance_constraints_satisfied`). A failure records the
     instance as `unsat_inst` and the search continues.
   - A name match whose head does not fit is a **fallback**; the first one
     seen is remembered.
6. **Diagnostics and fallback resolution** -- see the next sections.

Not every arm resolves to an instance. In a `#lang saffron` file an `any`
receiver can produce a runtime-dispatched `EX_DYN_METHOD` or `EX_DYN_FIELD`
instead; that path is outside this guide.

### Ambiguity

There are two distinct ambiguity checks, and both report **TUR-E0020**:

- **Two user classes, one method name.** When a cheap pre-scan of the class
  list (`ambig_watch`) finds the name declared by two non-stdlib classes, the
  search keeps going after its first exact match. If an instance of the
  *other* class also matches, the call is rejected. Without this, the newest
  `definstance` silently won.
- **Several fallbacks, no exact match.** This is the "receiver type is erased
  (int64_t)" message: the receiver is `:int` or otherwise unknown and several
  instances match by name.

Several instances of the **same** class that all match is not ambiguity; the
first (newest) one wins, without a diagnostic.

### Fallback resolution -- there is no type defaulting

Turmeric has no Haskell-style defaulting: no rule turns an unconstrained
numeric literal into some preferred instance. The resolver does make a few
choices on the author's behalf, and they are all here:

| Situation | Choice | Where |
|---|---|---|
| Abstract tyvar receiver | A carrier-shaped representative, re-resolved per specialization | `elab_method_call`, tiers above |
| Several fallbacks, exactly one from a non-stdlib file | The user instance (T1: user instance shadows stdlib) | `user_fallback_*` in `elab_method_call` |
| Several fallbacks, abstract tyvar receiver | The first candidate, as representative | same |
| Dotted call to a return-directed method with an expected type | Re-route to `elab_try_return_dispatch` | same (the `for`-comprehension `.pure` case) |
| Nullary return-directed method, no expected type, exactly one instance and it is the function arrow | That instance | `elab_try_return_dispatch` |

Every other shape is an error. In particular a **concrete** receiver with no
exact match is TUR-E0015 "no instance of typeclass 'C' for type 'T'" however
many fallbacks there are -- one fallback used to be bound silently, which
crashed at runtime when the layouts differed. The whitelist of "concrete"
receivers is in the `concrete_distinct_receiver` test. `:int` is excluded
from it, because a `:int` receiver cannot be told apart from an erased value.

The exclusion is narrowed for a lone fallback: an `:int` receiver whose only
name-matching instance has a head no carrier word can hold -- `cstr`, a float
kind, `bool`, `nil` -- is TUR-E0015 too (`int_recv_foreign_head`). With only
`Shout [cstr]` in scope, `(shout 5)` used to pass `tur check` and segfault
([int-receiver-binds-sole-foreign-instance](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/int-receiver-binds-sole-foreign-instance.md)).
A lone instance with a type-variable or applied head still binds, and so does a
lone struct instance, which `cc` rejects rather than running. The ambiguity
guard below fires for two or more fallbacks.

### Return-directed resolution (`elab_try_return_dispatch`)

1. **Candidates.** Every class with a return-only method of this name is a
   candidate (at most 8). With several, the first whose declared result
   unifies with the expected type (`rt_unify_return`) is chosen. For an
   associated-type result, the first that has an instance binding that type
   to the expected one is chosen.
2. **A class with a function-arrow instance declines** when the call has
   arguments (`typeclass_has_arrow_instance`): the function argument selects
   the instance through `elab_method_call` instead.
3. **No expected type** is an error ("cannot infer type for return-directed
   method"), except for the unique function-arrow instance case above.
4. **Binding the dispatch variable.** `rt_unify_return` walks the declared
   result against the expected type -- bare (`a` vs `cstr`) or structured
   (`(Result a E)` vs `(Result User E)`) -- and binds the class variable.
5. **Abstract result.** If the expected type is itself a type variable --
   `(:: (deserialize b) A)` inside a constrained generic -- there is no
   instance to pick yet. A representative is chosen the same way as for a
   receiver (an `int` head, else a carrier-shaped opaque newtype when the
   tyvar reaches a parameter). For a higher-kinded class it is the ambient
   constraint's representative (`cur_hkt_dict_binding->ambient_repr`). The
   call is tagged with a `dict_arg` and keeps the tyvar as its type, so emit
   can re-resolve it.
6. Otherwise `typeclass_env_lookup_instance` finds the instance, or the call
   is "no instance 'C T'".

The expected type comes from the ordinary channel: an ascription
`(:: (default-of) cstr)`, a typed `let` binding, a defn's declared return for
its tail, or a method parameter's declared type.

### What a resolved call records

Both resolvers build an `EX_CALL`. The fields downstream passes read:

| Field | Value | Read by |
|---|---|---|
| `fn_binding` | The chosen instance method's binding (`__inst_<Class>_<method>_<T>`) | The emitter: a resolved call is a **direct call** |
| `fn_expr` | `NULL`. Set to the `EX_DICT` only in the defensive no-binding case | `emit_expr.c` dict-slot direct call |
| `dict_arg` | `EX_DICT` from `make_dict_expr`: the instance, `dict_name`, and the mangled `method_name` | Emit re-resolution, dict-clone dispatch, `call_dispatch_is_static`, sum-box drop stamping |
| `abi_bindings` | Single-parameter kind-`*` class: class var -> receiver type, plus the instance head's own tyvars (`A` of `[(Option A)]`) and ground method tyvars. HKT class: class var -> constructor **head**, plus element tyvars, only when the instance body is by-value-expressible. A `(->)`-headed instance: the method's element tyvars, when fully ground | `emit_abi_register_call`, which interns a per-instantiation spec |
| `type` | The declared result with class and element variables substituted; a tyvar for an abstract return dispatch | Everything downstream |

`call_dispatch_is_static` (`elab_fns.c`) is the predicate for "the
`fn_binding` is the method that will run": no `dict_arg`, or a receiver (or
result) whose head is concrete. Inside a constrained generic the binding is
only a representative, and passes that need certainty -- the sum-box
drop-after stamp, the borrowed-closure hoist -- ask this first.

`make_dict_expr` spells `dict_name` with a smaller suffix table than the
emitter's `emit_dict_name`. That is harmless today because the emitter
re-derives the name from the instance (`emit_dict_name`) rather than trusting
the node's string.

### Instance order does not matter

A defn that calls a method before any instance exists is elaborated
speculatively and retried after the rest of its unit, so `definstance` may
appear below its use. Inside an **imported** module the importer's instances
have not been registered when the module is elaborated; such a defn is parked
and retried at each statement boundary of the importer (`elab_noinst_retry`,
`elab_module.c`, counted by `e->noinst_failures`). A program that declares no
instance of the class anywhere still gets TUR-E0015.

---

## Constrained generics: reaching the caller's instance

There are three mechanisms, chosen per call site. The first is the default.

### 1. Monomorphization (default)

The constrained defn is emitted once as a **carrier base** whose method calls
use the representative instance, and once per distinct argument C signature as
an ABI specialization `f__spec__<sig>`. While a specialization's body is
emitted, every call carrying a `dict_arg` whose dispatch type variable
(`emit_dispatch_tyvar`: the ascribed receiver, the bare receiver, or the
call's own result type -- or a `TY_APP` spine headed by a tyvar for an HKT
method) resolves through the active spec is renamed to the concrete instance's
method (`emit_reresolve_method_call`). The authoritative name comes from the
matching instance's own `FnDef` (`emit_concrete_inst_method_fndef`). A
reconstructed `__inst_<Class>_<method>_<component>` is the fallback.

```turmeric
(defclass Describe [a]
  (describe [x : a] : cstr))

(definstance Describe [int]
  (describe [x] "an int"))

(definstance Describe [cstr]
  (describe [x] "a string"))

(defn label [A] [(Describe A)] [x : A] : cstr
  (describe x))

(defn main [] : int
  (println (label 42))
  (println (label "hi"))
  0)
```
```sweet-exp
defclass Describe [a]
  (describe [x : a] : cstr)

definstance Describe [int]
  (describe [x] "an int")

definstance Describe [cstr]
  (describe [x] "a string")

defn label [A] [(Describe A)] [x : A] : cstr
  describe(x)

defn main [] : int
  println $ label 42
  println $ label "hi"
  0
```

`tur emit-c` (trimmed):

```c
static const char * label(int64_t x) {                       /* carrier base */
        TUR_MUSTTAIL_SELF(label);
        TUR_MUSTTAIL return __inst_Describe_describe_int(x);  /* representative */
}
static const char * label__spec__const_char___const_char__(const char * x) {
        return __inst_Describe_describe_cstr(x);              /* re-resolved */
}
...
        const char * __ps_202 = (label(INT64_C(42)));
        const char * __ps_203 = (label__spec__const_char___const_char__("hi"));
```

No dictionary is passed. The `int` call uses the carrier base because a spec
at `int` would have the same C signature. That is why the representative must
be the `int` (carrier-shaped) instance: the base is not just a placeholder,
it is the code that runs for carrier-shaped arguments. Two specs with the same
C signature but different types (two `defopaque` newtypes over `int`) are told
apart by the `__h<n>` suffix ("Gap H"). A generic whose class variable reaches
**no** parameter (`(defn zero-of [^Mo A] [] : A (mz))`) gets only one spec for
every instantiation. For that reason the return-directed resolver's
opaque-newtype representative tier requires the tyvar to reach a parameter
(`cur_fn_constraint_param_mask`); without one, the call stays a hard error.

The re-resolved callee may take a struct receiver or argument by pointer while
the spec holds it by value. `emit_reresolved_param_is_by_ptr` makes the call
site pass `&tmp` in that case.

### 2. Dict clones (runtime dictionary passing)

`make_dict_clone` (`elab_call.c`) copies a constrained defn's `FnDef`,
prepends one `int64_t` parameter per constraint (in constraint order) and
marks it with `dict_clone_params` / `dict_clone_classes`. The clone is named
`<f>__dict_<N>`. While its body is emitted, a call whose `dict_arg` class
matches constraint `k` loads its target from that dictionary
(`emit_call_dict_param_dispatch_index`, `emit_call_name`):

```c
((ret (*)(params...))((void **)(intptr_t)__dict_k)[slot])(args...)
```

`slot` is the method's index in the class. The cast reads the dict struct as
an array of function pointers, so the struct's field order and the class's
method order must agree (they do: `EX_INSTANCE_DEF` emits fields in class
order).

A clone is made in three situations:

| Trigger | Site |
|---|---|
| A constrained generic passed as a value to a rank-2 `(forall [a] [(C a)] ...)` parameter | the rank-2 pass site in `elab_call.c` |
| **Route B**: a direct call to a constrained **higher-kinded** generic whose constraints all ground to concrete instances | `elab_call.c`, after the ambient forwarding block |
| **Ambient forwarding** (Gap B): a constrained HKT generic calling another one at its own abstract functor | the `cur_hkt_dict_binding` block; the leading argument is the caller's own dict |

Route B is limited to higher-kinded constraints on purpose. Kind-`*`
constrained defns keep monomorphization, which their by-value element
specialization depends on.

A rank-2 example (`label` from above becomes `say`; trimmed):

```turmeric
(defn say [A] [(Describe A)] [x : A] : cstr
  (describe x))

(defn use-both [f (forall [a] [(Describe a)] (-> a cstr))] : int
  (println (f 42))
  (println (f true))
  0)

(defn main [] : int
  (use-both say))
```

```c
static int64_t say_un_undict_un11(int64_t __dict_12, int64_t x) {
        const char * __ps_70 = (((const char * (*)(int64_t))
                                 ((void **)(intptr_t)__dict_12)[0])(x));
        if (tur_panicking) return ((int64_t)0);
        return (int64_t)(intptr_t)__ps_70;
}
static int64_t __poly_15(void * __poly_env_16, int64_t __poly_x0_18,
                         int64_t __poly_x1_20) {
        return say_un_undict_un11(__poly_x0_18, __poly_x1_20);
}
/* in use-both: the caller supplies the dictionary for each instantiation */
    ((int64_t(*)(void*, int64_t, int64_t))f.fn)(f.env,
        (int64_t)(intptr_t)((int64_t)(intptr_t)(&dict_Describe_int_singleton)),
        (int64_t)(INT64_C(42)))
/* in main */
    use_hyboth((tur_poly_fn_t){ NULL, (int64_t(*)(void*,int64_t))__poly_15 })
```

A dict clone always returns the `int64_t` carrier (`emit_fns.c` forces it). A
by-value aggregate result is boxed and bridged back at the call site
(`dict_clone_byvalue_result`).

A Route B example, with a superclass in play (see
[Superclasses](#superclasses)):

```turmeric
(defn incr [^m] [^Monad m c : (m int)] : (m int)
  (bind c (fn [x : int] : (m int) (pure (+ x 1)))))

(defn main [] : int
  (match (incr (some 41))
    (Some v) (println v)
    (None)   (println "none"))
  0)
```

```c
/* main: one dictionary per constraint, superclasses included */
incr__dict_15__spec__..._tur_adt_Option__int(
    (int64_t)(intptr_t)(&dict_Monad_Option_singleton),
    (int64_t)(intptr_t)(&dict_Applicative_Option_singleton),
    (int64_t)(intptr_t)(&dict_Functor_Option_singleton), __ps_220);

/* the lifted continuation captured the Applicative dict and calls `pure`
 * (slot 0 of Applicative) through it */
struct __env_25__spec__tur_adt_Option__int { int64_t __fn; int64_t __dict_18; };
    ... ((int64_t (*)(int64_t))((void **)(intptr_t)
         __env___env_25__spec__tur_adt_Option__int->__dict_18)[0])((x) + (INT64_C(1)))
```

### 3. Dictionaries captured by nested lambdas

A clone's body is shared, but a lambda inside it is lifted to its own
function and has no dict parameter. `dict_clone_lower_nested_mappers`
converts each nested lambda that dispatches a constraint method into a
closure that **captures** one dict per class it uses (`dict_env_classes`).
While its body is emitted the method loads from `env->dict`
(`emit_call_dict_env_dispatch_index`). The dict parameter bindings are
memoised on the original `FnDef` (`memo_dict_params`), so every clone of one
defn uses the same binding identities and a shared lambda can capture them
consistently. `dict_clone_forward_generic_calls` forwards the dicts into a
nested call to another constrained generic.

One shape is still refused: a dispatch the walk cannot reach because the
lambda is applied in place inside another lambda and lifted to a by-name call
with no environment (see [Troubleshooting](#troubleshooting)).

### The interpreter

`turi` does not monomorphize. It binds a dictionary per constraint at each
call (`frame_bind_one_dict`, `src/turi/eval.c`) and dispatches through it, so
the representative never runs there. See the
[turi parity guide](turi-parity-guide.md).

---

## Superclasses

The preamble `(defclass Monoid [a] [(Semigroup a)] ...)` graduated on
2026-09-25 and is unconditional. The stdlib's HKT tower uses it:
`Applicative [^f] [(Functor f)]` and `Monad [^m] [(Applicative m)]`. Four
pieces make it work:

1. **Parsing** keeps each element as an unresolved form (`super_forms`) with
   the class-parameter indices it names (`super_arg_idx`), because the
   superclass may be declared below the subclass. Shape errors are
   **TUR-E0390**.
2. **Entailment.** `typeclass_entails(env, sub, sup)` walks the superclass
   closure (by name, so a class re-registered through two import paths still
   counts). The constrained-tyvar gate and the return-directed
   representative search both ask it, so `[^Monoid A]` licenses `combine`.
3. **Constraint expansion.** `typeclass_constraints_with_supers` appends
   each single-parameter constraint's superclass closure to the defn's
   constraint list (`elab_fns.c`, before the body is elaborated). Implied
   entries go **after** the declared ones. Dictionary passing and the
   interpreter read this list, which is why `incr` above takes three dicts
   for one `^Monad m`. Multi-parameter superclass shapes are not expanded;
   they rely on the dispatch-site walk.
4. **The instance obligation.** `elab_typeclass_superclasses_finish` runs
   once the unit is elaborated (called from `elab_toplevel.c`). It resolves
   every preamble (**TUR-E0391** for an unknown class), rejects cycles
   (**TUR-E0392**), and checks that every instance of a class with
   superclasses has the superclass instances at the same type arguments,
   through `typeclass_env_lookup_instance` (**TUR-E0393**). That last check
   is what makes entailment sound: without it a `[^Monoid A]` body could call
   `combine` at a type with no `Semigroup` instance, and the resolver would
   fall back to the silent representative bind.

Superclasses add no codegen of their own: there is no superclass pointer in a
dict struct. A subclass constraint simply contributes extra dictionaries or
extra re-resolutions.

---

## Default methods

A class method form with body forms after its return type carries a default:

```turmeric
(defclass Ord2 [a]
  (lt? [x : a y : a] : bool)
  (lte? [x : a y : a] : bool (or (lt? x y) (= x y))))

(definstance Ord2 [int] (lt? [x y] (< x y)))
```
```sweet-exp
defclass Ord2 [a]
  (lt? [x : a y : a] : bool)
  (lte? [x : a y : a] : bool (or (lt? x y) (= x y)))

definstance Ord2 [int] (lt? [x y] (< x y))
```

- `elab_defclass` keeps the method **form**, unelaborated, as
  `default_method_form`. A default cannot be elaborated at the class in
  general: no instance exists yet, so a sibling call like `(lt? x y)` has
  nothing to resolve against. (Elaborating it eagerly used to break every
  class that merely declared a default.)
- `elab_definstance` splices that form in for any method the instance omits
  (`impl_is_default_arr`). It then goes through the ordinary instance-method
  path with the receiver at the instance's type, so `lte?` becomes
  `__inst_Ord2_lte_qu_int` and its sibling call resolves statically:

  ```c
  static bool __inst_Ord2_lte_qu_int(int64_t x, int64_t y) {
          bool __ps_71 = (__inst_Ord2_lt_qu_int(x, y));
          ...
  ```

- A default body that mentions **no** sibling method is *also* elaborated
  once at the class as `__default_<Class>_<method>`
  (`SYNTH_DEFAULT_METHOD`). That copy exists so class-level rules -- an
  effect beyond the method's declared row is TUR-E0009 even when every
  instance overrides the method -- are checked once. The test is syntactic
  (`default_body_mentions_sibling`).
- An instance that omits a method with no default is an error: "missing
  method 'm' for 'C', and the class declares no default for it".

---

## HKT instances on the by-value path

A higher-kinded class (`[^f]`) has methods whose types mention `(f a)`. The
stdlib's `Option` and `Result` are by-value ADTs, so an `(Option float)` is a
16-byte struct, not a pointer. Each instance method therefore has two
possible lowerings:

- The **carrier base** `__inst_<Class>_<method>_<Head>`, whose `(f a)`
  parameters and result are the `int64_t` carrier (a pointer to the
  aggregate). This is what the dict slot points at.
- A **by-value spec** `__inst_<Class>_<method>_<Head>__spec__<sig>` that
  takes and returns the concrete aggregate.

The receiver-directed resolver attaches HKT `abi_bindings` (class var ->
constructor head, plus the element variables recovered by unifying the
method's declared parameter types against the arguments) only when **both**
of these hold:

- The instance body can be monomorphized by value: it is not inline C, and it
  constructs its result (`m7_body_constructs_byvalue`) or, for a bare-element
  result like Comonad `extract`, reads one out
  (`m7_body_returns_byvalue_element`).
- The result type grounds (`m7_byvalue_grounded`), or it is bound
  symbolically to the enclosing generic's own type variable
  (`m7_symbolic_elem`).

The two conditions gate both the by-value result type and the bindings, and
they must agree. A carrier-bodied method with a by-value result type would
make the consumer read a struct the producer never wrote.

`(fmap (some 7.1) (fn [x : float] : float (* x 2.0)))` (trimmed):

```c
static tur_adt_Option__float
__inst_Functor_fmap_Option__spec__tur_adt_Option__float_tur_adt_Option__float_int64_t(
        tur_adt_Option__float container, tur_poly_fn_t g) {
        ...
            case 1: {
                double v_1001146 = (double)__scrut->as.Some._0;
                double __ps_207 = (((double (*)(void*, double))g.fn)(g.env, v_1001146));
                ...
                tur_adt_Option__float __ps_208 = (some__spec__tur_adt_Option__float_double(__ps_207));
```

Nothing here touches `dict_Functor_Option`. It is not emitted at all in that
program, because no `EX_DICT` references it and the carrier base is never
called.

Return-directed HKT methods (`pure`, `empty`) bind the class variable to the
**constructor head** of the expected type (`Option`, not `(Option int)`) and
recover the element from the arguments, mirroring the receiver path:
`(let [p : (Option int) (pure 9)] ...)` calls
`__inst_Applicative_pure_Option__spec__tur_adt_Option__int_int64_t(9)`.

Partially applied heads -- `(definstance Functor [(Result _ B)] ...)` --
record the hole's position in `partial_hole_pos`. Their dict and carrier base
are suffixed with the head and the type name of the fixed argument, e.g.
`__inst_Functor_fmap_Result_tyvar` / `dict_Functor_Result_tyvar`. In a plain
probe at `(Result int cstr)`, `fmap` and `bind` currently run on that carrier
base, with the by-value receiver spilled and its address passed (the answer
is correct). When an HKT method runs through a dictionary (a dict clone), the
slot always has the uniform carrier shape -- `int64_t (*fmap)(int64_t,
tur_poly_fn_t)` for `dict_Functor_Option` -- and the clone boxes a by-value
argument before the call (the `malloc` + `TUR_REGION_NOTE_WORDS` pair in
`incr` above).

---

## Instances across modules

Classes and instances live in the one global `TypeClassEnv`, so an instance
is visible everywhere once its module has been elaborated. **There is no
syntax for exporting an instance, and none is needed.** The same holds for
classes and their methods: naming a class in `(export ...)` is an error
("exported symbol 'C' is not defined in this module"), and an importer calls
a method without referring to it -- `xmi/use` in the fixture below calls
`dbl` with only the type and two defns in its `:refer` list.

Three rules apply at module boundaries:

- **Orphans.** `elab_definstance` rejects an instance (**TUR-E0013**) when
  the class was defined in another file and none of the head's types belongs
  to this file. A primitive type is credited to a designated stdlib home file
  (`builtin_kind_home_basename`, `builtin_type_home_basename`): `int` and the
  other scalars to `typeclass.tur`, `rc`/`weak` to `rc.tur`, `str` to
  `str.tur`, `Sym` to `sym.tur`. A class defined in the entry file
  (`origin_file_id == 0`) is never orphan-checked.
- **Linkage.** Under separate compilation an instance method of a user
  module is emitted with external linkage and declared in the module header,
  so another TU can call `__inst_*` directly. Four sites -- definition,
  forward declaration, CPS entry, header -- consult
  `emit_inst_method_wants_external` (`emit_module.c`). It says no for stdlib
  instances, because a `(load "stdlib/...")`-ed file is spliced into every
  module that loads it and would otherwise define the symbol in each TU. The
  known residual: a **non**-stdlib file `load`-ed into two modules of one
  spice still duplicates its instance methods.
- **Instances registered after the import.** See
  [Instance order does not matter](#instance-order-does-not-matter).

The mechanics of headers, per-module TUs and the spec cache are in the
[separate compilation internals guide](separate-compilation-internals-guide.md).
The fixture that pins the linkage rule is
`tests/fixtures/spices/cross-module-instance`, run by
`tests/spice-c-sources-tests.sh`. A single-file fixture has no boundary to
cross.

---

## Dictionary lowering

Each **live** `definstance C [T]` emits, at file scope (`emit_stmt.c`,
`EX_INSTANCE_DEF`):

1. The method impls, one `__inst_<Class>_<method>_<suffix>` per class method
   (the binding name computed in `elab_definstance`; the method name and
   suffix go through `tur_mangle_ident`, so `lt?` becomes `lt_qu`).
2. A **dictionary struct** `dict_<Class>_<suffix>` with one function-pointer
   field per method, **in class-method order**.
3. Any `__dictwrap_<Class>_<method>_<suffix>` shims (below).
4. A **global singleton** `dict_<Class>_<suffix>_singleton` whose fields
   point at the impls, or at their shims.

The `<suffix>` is the primitive name (`int`, `cstr`, `float32`, ...), the
source symbol for an ADT or an unresolved head, or `<ctor>_<arg>` for an
applied head.

### Slot signatures and `__dictwrap_*` shims

A dict slot is only ever read on the **dictionary-passing** path, where the
caller does not know the instance. So the slot is typed for that caller, not
for the impl:

- A parameter the class declares as its **bare class variable** (`[x : a]`,
  or unannotated `[x]`) is the `int64_t` word in the slot whenever the impl
  takes it as `bool`, a narrower integer, a float or a pointer
  (`dict_slot_param_is_word_scalar`).
- A **by-value aggregate** parameter is the `int64_t` carrier (a pointer to
  the aggregate) unless the impl takes it `const T *`
  (`dict_slot_param_is_carrier`).
- A class-variable **result** (bare `a`, or a kind-`*` applied `(Option a)`)
  is returned as the word: bits for a float, the pointer for a pointer, a
  `malloc`'d box for a by-value aggregate (`dict_slot_result_is_word_scalar`).
- A function-typed parameter (`param_is_fn`, `[f :fn]`) is `tur_poly_fn_t`;
  a pass-by-pointer struct is `const T *`, as in the impl.

When any of these applies, the slot is filled with a shim that converts and
calls the impl. From the `Describe` example above:

```c
typedef struct dict_Describe_cstr {
    const char * (*describe)(int64_t);
} dict_Describe_cstr;

static const char * __dictwrap_Describe_describe_cstr(int64_t __a0) {
    return __inst_Describe_describe_cstr((const char *)(intptr_t)__a0);
}
static dict_Describe_cstr dict_Describe_cstr_singleton = {
    .describe = __dictwrap_Describe_describe_cstr,
};
```

`dict_Describe_int` has the same slot type and points straight at
`__inst_Describe_describe_int`, which already takes `int64_t`. All instances
of one class thus present the same slot signature. That is what lets a dict
clone call any of them through one cast. (Before these shims, a `double`
argument went through an integer register and the impl read a float register:
a silent wrong answer.)

### Dead-instance elimination

`EX_INSTANCE_DEF` emits nothing for an instance that `emit_instance_is_live`
reports dead. The scan marks an instance live when one of its methods is
called directly, or when an `EX_DICT` names it (the `EX_DICT` arm of
`emit_abi_scan_expr` calls `emit_abi_note_instance_dict_ref`). The impls are
skipped in lockstep (`emit_abi_fn_skip_generic`), so a singleton never names a
skipped body. A live instance always gets its dict, even when nothing reads
the singleton. In a build containing a `#lang saffron` file, an instance whose
type is widened to `any` somewhere (`emit_abi_instance_tag_is_widened`) is
also noted live, because runtime dispatch reads the dict through the
registry (`emit_abi_scan_program`).

### Method-field C-type resolution

The slot's return type is chosen in this order (`emit_stmt.c`), which matches
the impl signature in `emit_fns.c`:

1. `binding->type.as.fn.result_full_type` -- the method's fn type carries a
   full return Type. **This is the load-bearing path for closure returns**
   (below).
2. `result_kind` via `emit_type_from_kind` -- primitives and bare references.
3. `body->type` when there is no fn-typed binding.

That Type is then spelled by `dict_slot_ret_c_name` (which returns `int64_t`
when the impl's result rides the carrier ABI). For a `TY_FN` result,
`emit_inst_fn_return_carrier` overrides it. When
`dict_slot_result_is_word_scalar` holds, the slot says `int64_t`. (The older
first step, `emit_carrier_return_override`, is gone: a method body is never
`TY_STRUCT` any more.)

### How `EX_DICT` is emitted

`EX_DICT` (`emit_expr.c`) has three spellings:

| Node | C |
|---|---|
| With a `method_name`, used as a call's `fn_expr` | `dict_X_singleton.method` -- called directly, with no cast, because the field is already typed (the defensive path; resolved calls use `fn_binding`) |
| Without a `method_name` (a dictionary value: Route B, rank-2 pass sites) | `(int64_t)(intptr_t)(&dict_X_singleton)` |
| `is_ambient` inside a dict clone | the clone's own dict parameter |

Runtime dispatch through a dictionary *parameter* is not an `EX_DICT` at
all: it is the `((void **)dict)[slot]` cast that `emit_call_name` writes for
a call whose `dict_arg` class matches the clone's constraint.

**The dict-field type, the impl signature (or its shim) and the dispatch-site
cast must agree.** They are three views of one function pointer.

---

## The closure-handle convention

A first-class **closure value is carried as an `int64_t`** at runtime: a handle
to the heap fat-closure box `{ thunk, env... }` (or, for a captureless lambda, a
bare function pointer). This is the same handle `TUR_APPLY0/1/2/...` consume by
reading the thunk from slot 0. `type_c_name` (`types.c`) lowers function types
accordingly:

- A `cfnptr` `TY_FN` (a typed C function pointer) -> its
  `tur_fnptr_*` typedef.
- A **boxed** `TY_FN` (a stored first-class closure) -> `void *`.
- Every other `TY_FN` -> **`int64_t`** (the handle). In particular a closure
  whose result is *itself a function* (a curried return, `(fn [int] (fn [int]
  int))`) or whose result is unknown must not recurse into the result kind:
  that bottoms out at a zeroed `TY_FN` shell and emits an unknown-void
  carrier, silently dropping the handle.

### Why a closure-returning method needs `result_full_type`

When a method declares a function return type, e.g.

```turmeric
(defclass HasArr [a]
  (arr-of [self n : int] : (fn [int] int)))
```
```sweet-exp
defclass HasArr [a]
  (arr-of [self n : int] : (fn [int] int))
```

the instance-method elaborator (`elab_definstance`,
`src/compiler/elab_typeclasses.c`) builds the method's fn type with
`type_fn(param_kinds, n, return_type.kind)`. That stores only the return *kind*
(`TY_FN`) and **drops the full return Type**. Without the full type, codegen
falls back to `emit_type_from_kind(TY_FN)` -- a zeroed shell whose result kind is
`TY_UNKNOWN` -- and the dict field and impl signature lower to an unknown-void
carrier. The emitted impl then `return`s a closure handle from a
`void`-returning function: a silent miscompile.

The fix mirrors the regular `defn` path ("Issue 1b" in `elab_fns.c`): when the
declared return is a `TY_FN`, `elab_definstance` attaches it as
`result_full_type`. `emit_inst_fn_return_carrier` (`emit_core.c`) then picks
the carrier, and the dict field (`emit_stmt.c`) and the impl signature
(`emit_fns.c`) both consult it:

| Method body | Dict field / impl return | Call-result binding |
|---|---|---|
| Captureless lambda (`body_yields_thin_fn`) | the fn-ptr typedef, e.g. `tur_fnptr_int64_t_int64_t_t` | `tur_fnptr_int64_t_int64_t_t v = __inst_...(...)` |
| Capturing lambda (a fat box) | `int64_t` | `int64_t g = (int64_t)(intptr_t)(__ps_N)` |

For a **curried** return (a closure returning a closure) the
`result_full_type` is a `TY_FN` whose result kind is again `TY_FN`;
`type_c_name`'s rule above carries it as `int64_t` rather than recursing to
unknown-void.

### Calling a closure returned from a method

The returned handle obeys the usual captureless-vs-fat distinction:

- A returned closure that **captures** free variables is a fat box -- apply it
  through `TUR_APPLY1(f, x)` (the standard protocol).
- A returned closure that **captures nothing** is a bare function pointer --
  apply it through a thin cast `((int64_t(*)(int64_t))(intptr_t)f)(x)`.

The `tests/fixtures/instance-closure-return-*` fixtures cover both, plus int and
struct captures, float and bool results, curried returns, and two methods
composed at the call site.

---

## Re-running a `definstance`

What happens when a `definstance` with the same class and mangled head suffix
is elaborated twice depends on where the second one comes from
(`elab_definstance`, the "idempotent re-instance guard"):

| Second definition | Result |
|---|---|
| In a stdlib file (the same stdlib file loaded twice, or an explicit `(load "stdlib/...")` over an auto-loaded stub) | Silent no-op; the first definition wins |
| In a REPL / playground turn, replacing a non-stdlib instance from an earlier turn (`elab_prior_turn_instance`) | The old instance is unlinked and the new one registered, so re-running a program or `(reload)` works |
| Anywhere else -- a user file, including one that duplicates a stdlib instance | **TUR-E0025** "instance C [T] is already defined" |

The third row is deliberate (decided 2026-09-11): replacing a stdlib instance
would change what code runs inside the stdlib itself. The way to give a type
different behaviour is a `defopaque` newtype with its own instance.

A repeated `defclass` follows a simpler rule (`elab_defclass`): an identical
signature (`typeclass_signatures_match`) is a silent no-op -- the stdlib
pre-declares some classes -- and a different one is "typeclass 'C' is already
defined".

## TUR-W0039: method name shadowing a free `defn`

A typeclass method name shares the value namespace with ordinary `defn`
bindings. When a free `defn` has the same name as a method of a
**user-defined** class, the compiler emits **TUR-W0039** at the `defn`
(`elab_toplevel.c`). The two coexist:

- A bare `(name x ...)` dispatches to the method when an instance of the user
  class matches `x`'s static type (`prefer_method_dispatch`), and calls the
  free `defn` otherwise.
- The dotted `(.name x ...)` always dispatches the method.

```turmeric
(defclass Describe [a] (describe [x : a] : cstr))
(definstance Describe [int] (describe [x] "method"))
(defn describe [x : cstr] : cstr "free defn")   ;; TUR-W0039 here

(describe 1)     ;; => "method"
(describe "s")   ;; => "free defn"
```

The warning does not fire for a stdlib class (`from_stdlib`), or for a
`defn` that itself lives in a stdlib file: overriding a stdlib method with a
same-named defn is an intentional pattern, and there the defn always wins.
Rename one of them when the shadowing is unintentional. See
[arrows-guide.md](arrows-guide.md) for the worked example that originally
surfaced the diagnostic.

## Associated type members

A class may declare **associated type members** alongside its value methods.
An associated type is a type-level projection from the instance type to another
type -- the Haskell `type Storage c = ...` / Rust `type Item;` pattern:

```turmeric
(defclass Container [t]
  (type Elem : Type)               ;; associated type member
  (make-empty [self : t] : int))   ;; ordinary value method

(definstance Container [(Vec int)]
  (type Elem = int)                ;; per-instance binding
  (make-empty [self] 0))

(definstance Container [(Vec cstr)]
  (type Elem = cstr)
  (make-empty [self] 0))
```
```sweet-exp
defclass Container [t]
  (type Elem : Type)              ;; associated type member
  (make-empty [self : t] : int)   ;; ordinary value method

definstance Container [(Vec int)]
  (type Elem = int)                ;; per-instance binding
  (make-empty [self] 0)

definstance Container [(Vec cstr)]
  (type Elem = cstr)
  (make-empty [self] 0)
```

In any type-annotation position, the projection `(Elem (Vec int))` resolves to
whatever the matching instance bound -- here `int`; `(Elem (Vec cstr))`
resolves to `cstr`:

```turmeric
(defn take [x : (Elem (Vec int))] : int (+ x 1))   ;; x : int
```
```sweet-exp
defn take [x : (Elem (Vec int))] : int {x + 1}   ;; x : int
```

Rules and representation:

- **Parsing.** `(type Name : Type)` in a `defclass` body is an associated type
  member; `(type Name = <type>)` in a `definstance` body binds it. The
  discriminator is a `type` head whose second element is a bare symbol (a
  method named `type` carries a `[params]` vector instead, so it is never
  misclassified). Members are stored on `TypeClass.assoc_type_names`; bindings
  on `TypeClassInstance.assoc_types` (`src/compiler/typeclass.h`).
- **Every member must be bound.** A `definstance` that omits a member, or binds
  a name the class never declared, is a hard error reported on the instance.
- **Resolution is precise.** `(Name T...)` matches the instance whose type
  arguments are structurally equal to the projection's arguments (an exact
  N-ary match, `typeclass_env_lookup_instance_exact`). This is *not* the
  kind-erased dispatch `typeclass_env_lookup_instance` (which cannot tell
  `Vec int` from `Vec cstr`); associated-type projection has its own
  `typeclass_env_resolve_assoc_type` / `typeclass_env_resolve_assoc_type_n`.
- **Erased.** Associated types live only in the elaborator's type-annotation
  parsers (`type_expr_from_form` and the `defn` signature parser
  `fn_type_from_form`, which must carry its own copy of the hook because it
  builds type applications without delegating). There is no dictionary field
  and no codegen -- zero runtime cost.
- **As a method result.** A method whose result is an associated type
  (`(empty [] : Inner)`) is return-directed on the class's first parameter;
  see [Return-directed resolution](#return-directed-resolution-elab_try_return_dispatch).
  An ascription that spells the projection, `(:: (empty) (Inner W))`, names
  the instance. A bare type selects the one instance binding the member to it
  and refuses when several do.
- **Scope.** Multi-parameter classes are supported: a projection names every
  class parameter (`(Acc (Vec int) int)`) and resolves by the exact N-ary
  match above. One output type per member; no type-level arithmetic.
  Functional dependencies are a separate mechanism, declared with the
  `| (a -> b)` clause on the class head (see
  [typeclass-guide.md](typeclass-guide.md#functional-dependencies)).

---

## Troubleshooting

Every message below is real `tur check` output, trimmed to its first line.

| Shape | Diagnostic | Fix |
|---|---|---|
| No instance for a concrete receiver: `(describe "hi")` with only `Describe [int]` | `error [TUR-E0015]: no instance of typeclass 'Describe' for type 'cstr' (method '.describe'). Add (definstance Describe [cstr] ...) ...` | Add the instance, or call on a type that has one |
| No instance of the class anywhere in the program | `error [TUR-E0015]: 'm' is a method of typeclass 'C', but this program declares no 'C' instance at all ...` | Add one; it may appear anywhere in the file |
| A parametric instance whose own constraint fails for the element: `Show [Vec] [(Show A)]` at `(Vec bool)` with no `Show [bool]` | `error [TUR-E0015]: no instance of typeclass 'C' applies to type 'T' ... a parametric instance's own constraints must also hold for the element type.` | Add the element instance |
| A generic calls a method on its own tyvar without the constraint: `(defn say [A] [x : A] : cstr (describe x))` | `error [TUR-E0015]: 'describe' is a method of typeclass 'Describe', but 'say' does not constrain 'A' to it ...` | `(defn say [A] [(Describe A)] ...)` |
| Two user classes declare `encode`, both with an `int` instance | `error [TUR-E0020]: ambiguous method dispatch: 'encode' is declared by typeclass 'Fmt' and by typeclass 'Enc', and both have an instance for type 'int' ...` | Rename one method |
| An `:int` (erased) receiver with several aggregate instances: `(area 5)` with `Shape [Sq]` and `Shape [Ci]` | `error [TUR-E0020]: ambiguous method dispatch: '.area' matches 2 instances (Shape[Ci], Shape[Sq]) -- receiver type is erased (int64_t). ...` | Annotate the receiver, or pin it: `(area @Sq x)` |
| An `:int` receiver with exactly one instance, of another type: `(shout 5)` with only `Shout [cstr]` | none -- `check` is clean; the build casts `5` to the instance's type and crashes or fails in `cc` (see [the known gap](#fallback-resolution----there-is-no-type-defaulting)) | Pass a value of the instance's type, or add `Shout [int]` |
| An `any` receiver with no `C [any]` instance (typed dialect) | `error [TUR-E0020]: cannot dispatch '.m' on an 'any' receiver ...` plus a help line | Narrow with `is?`, unbox with `cast`, or pin with `@T` |
| A return-directed method with no expected type: `(println (default-of))` | `error: cannot infer type for return-directed method 'default-of'; add a type ascription, e.g. (:: (default-of ...) T)` | `(:: (default-of) cstr)` or a typed `let` |
| An ascription whose shape does not fit the declared result | `error: ascribed type does not match the result shape of 'm'` | Ascribe the whole result type, e.g. `(Result User cstr)` |
| `@T` names a type with no instance | `error: no instance of typeclass method '.m' for type 'T' -- check that (definstance ... [T] ...) is in scope` | Fix the witness |
| Same instance defined twice (or a user copy of a stdlib instance) | `error [TUR-E0025]: instance Describe [int] is already defined; a class has one instance per type -- ... wrap it in a newtype (defopaque) ...` | Delete one, or use a newtype |
| Instance omits a method with no default | `error: definstance: missing method 'two' for 'Two', and the class declares no default for it` | Implement it, or give the class a default |
| Instance names a method the class does not have | `error: method implementation name 'm' doesn't match any method of 'C'` | Fix the name |
| Subclass instance without its superclass instance | `error [TUR-E0393]: (definstance Mono [int]) requires a Semi [int] instance ('Semi' is a superclass of 'Mono'); none is in scope. ...` | Add the superclass instance |
| Instance for a foreign class and a foreign type | `error [TUR-E0013]: orphan instance: typeclass 'C' is defined in a different module and none of the type arguments belong to this module ...` | Move the instance, or wrap the type in a local newtype |
| A constrained generic passed rank-2 whose constraint method is dispatched inside a lambda the dict-clone walk cannot reach | `error [TUR-E0313]: forall-dict-pass: 'say' dispatches a typeclass method on its constrained type variable from inside a directly-applied nested lambda that cannot be lowered ...` | Call the method directly in the body. Until 2026-10-17 the text cited "(TUR-E0311)" (the unknown-`:engine` code) and the diagnostic carried no code ([dict-clone-nested-lambda-cites-wrong-code](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/dict-clone-nested-lambda-cites-wrong-code.md)) |
| `check` is clean but `cc` reports `call to undeclared function '__inst_...'` or a dict-slot type mismatch | (none from `tur`) | A resolver/emitter disagreement -- a bug. Reduce it and compare `tur emit-c` against the shapes in this guide |

When a resolved call does something unexpected, read the emitted C first:

```sh
tur emit-c prog.tur > /tmp/p.c
grep -n '__inst_<Class>_\|dict_<Class>_\|__spec__\|__dict_' /tmp/p.c
```

The callee name says which instance was chosen. A `__spec__` caller with a
representative callee (`..._int`) means re-resolution did not fire. A
`__dict_N` clone means the call went through dictionary passing. `tur
--timings` reports resolution time as the `typeclass` phase.

---

## See also

- [Typeclasses in Turmeric](typeclass-guide.md) -- the user-level surface.
- [HKT guide](hkt-guide.md), [Monomorphization ABI guide](monomorphization-abi-guide.md),
  [Value representations](value-representations-guide.md) -- the carrier,
  the by-value aggregates and the spec symbols this guide refers to.
- [Elaborator internals](elaborator-internals-guide.md),
  [Emitter architecture](emitter-architecture-guide.md),
  [Separate compilation internals](separate-compilation-internals-guide.md),
  [Compiler internals](compiler-internals.md).
- [Polymorphism guide](polymorphism-guide.md) -- where typeclasses sit among
  the other polymorphism mechanisms.
- The arrow surface in `stdlib/arrow.tur` and `stdlib/kleisli.tur` is a
  production-grade exercise of these rules: `Category` (the superclass of
  `Arrow` and `ArrowZero`), `Arrow`, `ArrowChoice`, `ArrowZero`, and a second
  `Category` instance (`Kleisli`) all share method names and exercise both
  the closure-handle convention and the method-vs-defn namespace rules. See
  [arrows-guide.md](arrows-guide.md).

### Reading the archive

The *why* behind most branches in the resolver is in `docs/archive/` and
`docs/archive/history/`. The reports most worth reading:

- `history/m5-constrained-poly-wrong-instance-on-tyvar-receiver.md`,
  `history/return-dispatch-tyvar-silent-misdispatch.md` -- why an abstract
  receiver or result binds a representative and re-resolves at emit.
- `history/forall-dict-pass-multi-constraint-hkt-plan.md`,
  `history/forall-dict-pass-nested-lambda-method.md` -- dict clones and the
  captured-dictionary lowering.
- `typeclass-superclasses-plan.md` -- the superclass design (SC1-SC8).
- `definstance-not-dispatchable-across-modules.md` -- cross-module linkage.
- `for-comprehension-pure-ambiguous-against-stdlib.md` -- the dotted
  return-directed fallback.
- `history/typeclass-methods-share-value-namespace-with-defns.md` -- the
  method/defn coexistence rule behind TUR-W0039.
- `history/load-not-idempotent-typeclass.md` -- the re-instance guard.
- `history/closure-returning-instance-method-codegen-plan.md` -- the
  closure-return carrier rule.
- `history/nested-closure-transitive-capture.md` -- an *orthogonal*
  capture-set defect (a grandchild closure's free var not threaded through the
  middle closure), independent of the carrier type.
- `history/intra-instance-method-dispatch-unsupported.md` -- calling a
  sibling method via `(.other self ...)` inside an instance body (now
  supported: an instance is registered before its bodies are elaborated).

## Bugs found while writing this guide

Each was reproduced against the compiler this guide describes and filed in
[`docs/reported/`](https://github.com/turmeric-lang/turmeric/blob/main/docs/reported/README.md). When one is fixed and archived,
update the passage above that describes it and drop it from this list.

None open.

