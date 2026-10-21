# `defdynamic` of `(Option T)` type-checks, then fails in cc: its root is emitted before its type

**Severity: low-medium (loud failure, but in the C compiler, with nothing
pointing at the cause).** A dynvar whose type is a generic ADT instantiation
passes the type checker. The emitted C then defines the dynvar's by-value root
about 240 lines before that instantiation's `struct`, so cc stops with
`unknown type name 'tur_adt_Option__int'`. A plain `defdata` is rejected at the
declaration instead, with `TUR-E0603` (move-only). So the two kinds of ADT get
different answers to the same question. Filed 2026-10-17 while revisiting
[dynvar-adoption-spices-plan](../upcoming/spices/dynvar-adoption-spices-plan.md),
whose tourist target wants a "no request in flight" root for a context var.

## Repro

Measured on the v0.65.5 release binary:

```turmeric
(defdynamic *v* :(Option int) (:: (None) :(Option int)))
(defn main [] : int
  (binding [*v* *v*]
    (println 1))
  0)
```

```
error: unknown type name 'tur_adt_Option__int'
```

`(Option cstr)` and `(Option Ctx)`, with `(defopaque Ctx :int)`, fail the same
way, naming `tur_adt_Option__cstr` and `tur_adt_Option__Ctx`. By contrast:

```turmeric
(defdata Shape (Circle int) (Square int))
(defdynamic *v* :Shape (Circle 1))
;; error [TUR-E0603]: defdynamic: '*v*' -- dynamic vars may not hold
;; substructural (linear or move-only) types
```

The root needs the `::` ascription. `(None)` on its own is typed as the bare
`Option` and is refused with `TUR-E0602`, which is correct, but you have to find
the spelling yourself.

## Root cause

In `tur emit-c` of the repro, `static tur_adt_Option__int _dynvar_root_v;` and
the DV3 snapshot struct's `tur_adt_Option__int val_v;` both come from the
dynvar block that `src/compiler/emit_module.c:20262-20289` writes into
`early_file`, and they land at line 6148. The monomorphized
`struct tur_adt_Option__int { int tag; union ... }` is emitted at line 6392,
behind its `TUR_TY_`/`TUR_FWD_` guards. A forward typedef would not be enough:
a static object of the type, like a `sizeof` in the snapshot code, needs the
complete type.

## Fix directions

First decide whether an `(Option T)` belongs in a dynvar at all.

1. **If it does:** emit the dynvar block after the monomorphized ADT
   definitions, or emit each dynvar's root and snapshot slot after the
   definition of the type it holds. Then add a fixture that binds, reads twice,
   and rebinds an `(Option T)` dynvar, so the read path is shown not to free a
   box that something else still owns.
2. **If it does not:** extend the `TUR-E0603` check to generic ADT
   instantiations, so `(Option T)` is refused at the declaration the way
   `Shape` is, and name the alternative in the diagnostic.

Either way, the dynamic-vars guide should say which ADTs a dynvar can carry.

## 2026-10-17: a payload-free `defdata` is refused too

Measured on v0.65.5: `TUR-E0603` also refuses an enum with no payloads at all.

```turmeric
(defdata LogLevel (Debug) (Info) (Warn) (Error))
(defdynamic *lv* :LogLevel (Info))
;; error [TUR-E0603]: defdynamic: '*lv*' -- dynamic vars may not hold
;; substructural (linear or move-only) types
```

No constructor carries anything that could be duplicated or freed twice, so
"move-only" is stricter than the rule needs. It is what keeps
[dynvar-adoption-plan](../upcoming/dynvar-adoption-plan.md) ADO-1 from typing
`*log-level*` as an enum; the plan uses a `defopaque` newtype instead. When
this is decided, admitting payload-free ADTs is a separate, smaller question
than admitting `(Option T)`.

## Resolution (2026-10-18)

The first fix direction: an `(Option T)` belongs in a dynvar. There were three
gaps, and the repro hit only the first.

1. **Order.** The dynvar block in `src/compiler/emit_module.c` now flushes the
   monomorphized ADT definitions (`type_codegen_emit_adt_apps`) before it
   declares a root of a `TY_APP` type. The root and the DV3 snapshot slot
   then see the complete struct.
2. **Specialization.** The emitter's ABI scan (`emit_abi_scan_expr`) had no
   case for `EX_DEFDYNAMIC`, `EX_DYNVAR_BINDING` or `EX_DYNVAR_SET`. A generic
   call there, such as `(some 7)` in a `binding` override, was never
   registered. It was emitted as the carrier base, and its int64 word was
   stored into the by-value slot, which cc rejects. The three nodes are now
   scanned like a let binding of the var's type.
3. **Expected type.** The root, each `binding` override and a `set!` value are
   elaborated with the var's declared type on the expected-type channel, as a
   let annotation is. A bare `(None)` or `(none)` root no longer needs the
   `::` ascription.

Pinned by:
- `defdynamic-option-root`: `(Option int)`, `(Option cstr)` and `(Option
  Ctx)` vars, each bound, read twice and rebound. It also has bare `(None)` /
  `(none)` roots, `set!` and a nested `(none)` override. It is leak-checked,
  and passes under `tur run`, `tur --interpret` and `tur jit`.
- `defdynamic-option-convey`: an `(Option Ctx)` var through
  `spawn-conveying`.

The dynamic-vars guide now says which ADTs a dynvar can carry. The other
question here, whether a payload-free `defdata` should be admitted past
`TUR-E0603`, is still open. It moved to
[defdynamic-refuses-payload-free-defdata](defdynamic-refuses-payload-free-defdata.md) (since fixed).
