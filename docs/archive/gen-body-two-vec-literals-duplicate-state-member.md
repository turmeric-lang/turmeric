# A gen body with two vector literals emits a state struct C rejects

**Resolved 2026-10-23.** Fix direction 1: a generator's state-struct field is now named by binding identity -- `gen_field_name` (`src/compiler/emit_core.c`) appends the binding id to the raw name, and the struct typedef, the `_create` fill, the frame-field ctype records and `capture_env_access`'s `__g->` path all spell it that way. Two `[...]` literals no longer collide on `__vw` / `__v`, and neither do two user lets of one name (`(let [x 7] ...) (let [x 8] ...)`, the same defect without the literal). Pinned by `tests/fixtures/gen-body-duplicate-local-names`.

**Severity: low-medium.** Found 2026-10-23 executing
[generator-type-plan](generator-type-plan.md). Reproduces on `main`
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
