# `TUR_REGIONS=0` fails to link when the runtime comes from source

**RESOLVED 2026-10-21** -- see [Resolution](#resolution-2026-10-21). Pinned by `tests/run-regions-seam.sh` check 3.

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

## Resolution (2026-10-21)

Fix direction 3, from the build side. The other two would put a region symbol
in a regions-off program, and `tests/run-regions-seam.sh`'s canary requires
that the emitted C name none; pasting `region.c` with regions off would also
have defined its symbols twice for a build that links `libturi.a`.

- `src/runtime/hamt.c` calls the note through `hamt_region_note`, which is
  `tur_region_note_escape` unless the file is compiled with
  `-DTUR_REGIONS=0`, and a no-op then.
- `tur build` / `tur compile` (`src/main.c`, `autolink_regions_off`) add
  `-DTUR_REGIONS=0` to the autolink flags when regions are off and the
  runtime sources are compiled bare -- after `apply_runtime_lib_mode`, so an
  archive link (which carries `region.o`) is untouched.

The repro reproduces in any build tree with `--runtime=source`; it now links
and prints `1`, and `hamt-basic`, `hamt-large` and `hamt-lisp-merge-with` run
correctly that way. `tests/run-regions-seam.sh` gained a third check that
builds `hamt-basic` with `TUR_REGIONS=0 --runtime=source` and compares its
output; the harness's other checks all link the archive.
