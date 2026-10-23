# A gen body with two vector literals emits a state struct C rejects

**Severity: low-medium.** Found 2026-10-23 executing
[generator-type-plan](../archive/generator-type-plan.md). Reproduces on `main`
before that work.

## Repro

```turmeric
(defn main [] : int
  (let [gv (gen [] (yield [1 2 3]) (yield [4 5]))]
    (gen-next gv))
  0)
```

`tur run` fails in cc:

```
error: duplicate member '__vw'
error: duplicate member '__v'
typedef struct { int32_t __state; void *(*__next_fn)(void *); int64_t __vw;
  tur_adt_Vec__int * __v; int64_t __vw; tur_adt_Vec__int * __v; } __gen_main_0_t;
```

One vector literal in the body compiles; `--interpret` runs both.

## Root cause

Each `[...]` literal lowers to a `let` of hygienic temporaries named `__vw` /
`__v`. `elab_gen` promotes every `let` binding in the body to a state-struct
field (`collect_let_bindings_walk`, `src/compiler/elab_forms.c`), and the
emitter names the field from the binding's source name, so two literals give
two distinct bindings with one C name.

## Fix directions

Name the promoted field by binding identity (a per-binding suffix, as other
lifted locals are), or de-duplicate the field names in the gen struct emitter.
