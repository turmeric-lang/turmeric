# `TUR_REGIONS=0` fails to link when the runtime comes from source

**Severity:** low. The regions-off build arm cannot link a trivial program
unless a `libturi.a` is available.

## Repro

In a build tree where `libturi.a` was not built (e.g. after
`cmake --build build --target tur`):

```turmeric
(defn main [] : int
  (println 1)
  0)
```

```
$ TUR_REGIONS=0 tur build triv.tur -o triv
hamt.c:(.text+0x16a2): undefined reference to `tur_region_note_escape'
```

The same build with regions on links fine.

## Root cause

The embedded HAMT runtime (`hamt.c`) calls `tur_region_note_escape`
unconditionally: host-runtime C notes escapes directly, per CLAUDE.md's
region-store-hook rule. But `src/compiler/emit_module.c:emit_region_runtime_bodies`
embeds the region runtime (`region.c`) only when regions are on. With regions
off and no archive to supply the symbol, nothing defines it.

## Fix directions

- Emit a no-op `tur_region_note_escape` (and `_words`) when regions are off;
  or
- always embed the region runtime's note functions; or
- guard the call in `hamt.c` the way the `TUR_REGION_NOTE` macros already
  compile to `((void)0)` under `TUR_REGIONS=0`.
