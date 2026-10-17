# Dead generic-base constructor traps call the region allocator with regions off

**RESOLVED 2026-10-17** -- see [Resolution](#resolution-2026-10-17). Pinned by `heap-phantom-generic-dead-base-ctor` (run by `tests/run-regions-seam.sh`).

**Severity:** low. With `TUR_REGIONS=0`, a `:heap` generic struct emits a call
to a function that is not declared. gcc 14+ and clang 16+ reject that outright;
older gcc warns and truncates the returned pointer to `int`.

## Repro

```turmeric
(defstruct PBox :heap [V] [a : int b : int])

(defn pbox-new [V] [a : int b : int] : (PBox V)
  (PBox a b))

(defn main [] : int
  (let [p (pbox-new 1 2)]
    (println (.a p)))
  0)
```

```
$ TUR_REGIONS=0 tur emit-c pbox.tur | grep tur_region_alloc_or_malloc
    tur_adt_PBox * __r = (tur_adt_PBox *)tur_region_alloc_or_malloc(sizeof(struct tur_adt_PBox));
$ TUR_REGIONS=0 tur run pbox.tur
warning: implicit declaration of function 'tur_region_alloc_or_malloc'
```

## Root cause

`src/compiler/emit_module.c:emit_flush_dead_base_ctor_traps` hard-codes
`tur_region_alloc_or_malloc`. Every other allocation site chooses between it
and `malloc` with `regions_enabled()`.

## Fix directions

- Use `regions_enabled() ? "tur_region_alloc_or_malloc" : "malloc"`, as the
  other sites do. Better still, route all of them through one helper.

## Resolution (2026-10-17)

`types.c:region_alloc_fn()` -- the twin `region_free_fn` already referred to --
answers `malloc` when regions are off, and every node allocation site spells
it, `emit_flush_dead_base_ctor_traps` included.

The seam harness alone would not have caught this on gcc 13, which only warns
on the implicit declaration. It now builds its population with
`-Werror=implicit-function-declaration`, so a region call left undeclared
fails on any compiler.
