# `effect-cont<T>` names a resume's result only when T is a scalar

**Severity: low.** A correct program is refused at compile time, never
miscompiled. Noted 2026-10-12 while fixing
[cont-angle-bracket-annotation-becomes-tyvar](../archive/cont-angle-bracket-annotation-becomes-tyvar.md)
and filed 2026-10-13.

## Repro

```turmeric
(defstruct P [x : int y : int])
(defeffect Ask [] int)
(defn helper [k : effect-cont<P> v : int] : P (resume k v))
(defn step [] : int
  (let [p (handle (P (perform (Ask)) 2) (Ask [] k) (helper k 5))] (+ (.x p) (.y p))))
(defn main [] : nil (println (step)))
```

| | result |
| --- | --- |
| `tur --interpret` | `7` |
| `tur run` | TUR-E0709: `helper` declares return type `P` but its body returns `int` |

With `effect-cont<int>` and a handle of int, the same helper is typed
correctly. Inside the case body itself an aggregate is fine too, because a
case's `k` records its handle's full result type (`Binding.cont_result_type`).

## Root cause

A continuation type stores its result as a bare `TypeKind`
(`Type.as.cont.returns`, `src/compiler/types.h`), and so does `arg`. A kind
is a whole type only for a scalar. `elab_make_resume`
(`src/compiler/elab_effects.c`) therefore takes `returns` only for int,
float, bool, cstr and nil widths, and otherwise falls back to the resume
value's type, here `int`.

## Fix directions

1. Give the cont type a full result type: a `const Type *returns_full`
   beside `returns`, filled where `cont<T>` / `(cont T)` resolve T
   (`cont_type_from_keyword_name`, `fn_type_from_form_impl`), and read by
   `elab_make_resume`. `type_eq` would compare it when both sides have one.
2. Then a struct or ADT result crosses the slot boxed, as a case body's
   aggregate resume already does: `eff_slot_load` / `slot_load` key on the
   resume's type.
