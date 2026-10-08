---
title: `tur compile` and `tur link`
category: CLI Tools
description: Splitting the monolithic `tur build` into a cacheable object compile plus a cheap link -- `tur compile`, `tur link`, `--split-build`, and how `--runtime=` decides the way the runtime gets linked
---

# `tur compile` and `tur link` -- the compile/link split

A monolithic `tur build` is a single `cc` call that compiles **and** links in
one step. That call is uncacheable (ccache marks a multi-input compile+link
invocation "Uncacheable"). The `tur compile` + `tur link` pair splits it into a
cacheable object compile followed by a cheap link -- the same mental model you
already have from `cc -c` + `cc`.

```
tur compile foo.tur -o foo.o   # frontend lowering + `cc -c` (cacheable)  + foo.link
tur link    foo.o   -o foo     # `cc` link, reading foo.link for link flags
```

`tur build foo.tur -o foo` is exactly those two composed.

## `tur compile <file.tur> -o <out.o>`

Lowers one Turmeric source to an object file. This is the `emit-c` lowering
fused with `cc -c`, so you never have to drop to a raw `cc` for the middle step.
It writes three artifacts next to `<out.o>`:

| File | What it is |
| --- | --- |
| `<out>.o` | the compiled object (the **cacheable** unit) |
| `<out>.c` | the generated C the object was compiled from |
| `<out>.link` | a sidecar recording the resolved link flags |

Flags: `-I <dir>` (module resolution, repeatable), `-o <out.o>`,
`-B` / `--build-dir <dir>` (routes the object under `<dir>/obj/`). Enclosing
`build.tur` spice includes and `:reader-macros` are auto-discovered exactly as
`tur build` does.

`emit-c` is untouched -- it stays the "show me the C" tool and still backs the
snapshot fixtures. `tur compile` is the additive fused step, not a rename.

### The `.link` sidecar

```
# tur link sidecar v1
asan: 0
autolink:/abs/turmeric/src/runtime/hamt.c -I/abs/turmeric/src/runtime
cmake:
auxsrc:
```

The sidecar carries the **fully resolved** link flags (`-lturi` SDK anchoring,
ASan autodetect, tree-relative path anchoring, and the `-lturi`-supersedes-bare-
`.c` filter have all already run). It is the single source of truth for the link
step, so the compile and link halves cannot disagree about link flags.

## `tur link <obj/src>... -o <out>`

Links precompiled objects (and/or `.c` sources) into an executable. For each
input object it reads the sibling `.link` sidecar and unions the flags (with
whole-value dedup, so several objects of one program that each carry the same
resolved flags do not re-link a runtime source twice).

Flags: `-o <out>`, `--shared` (produce a shared library), `--link-flags "..."`
(extra linker flags, e.g. `"-L<dir> -lfoo"`).

```
tur compile app.tur -o build/obj/app.o
tur link    build/obj/app.o -o build/bin/app
```

## `tur build --split-build`

`tur build` grows two rollout flags:

- `--split-build` -- build a single file as `compile` + `link` (the object
  compile is a cacheable `cc -c`). Native builds only.
- `--no-split-build` -- force the monolithic single-`cc` build. **This is the
  default** while the split is proven out, so `tur build` output stays
  byte-identical to before.

Under `--split-build`, a single-file build runs the real `cmd_compile` then
`cmd_link` code paths (with the object + sidecar landing under the build temp
dir and cleaned up afterward), so `tur build`, `tur compile`, and `tur link`
share one implementation and cannot drift.

## `tur build --runtime=` -- how the runtime gets linked

A program that uses runtime facilities (maps, arc, reactor, ...) needs the
`src/runtime/*.c` sources linked in. Four modes:

- `--runtime=auto` (the default) -- link the lean, non-sanitized
  `libturt_runtime.a` archive when it is locatable, else transparently fall
  back to recompiling the bare runtime sources. It never links the (possibly
  ASan) full `libturi.a` on its own, so a default build is behaviorally
  identical to the source path.
- `--runtime=lib` -- force the archive link (lean archive preferred, else
  `libturi.a`), turning "recompile the runtime per build" into "link a static
  archive built once." Static linking dead-strips to only the referenced TUs,
  so the binary is unchanged.
- `--runtime=source` -- force recompiling the bare runtime sources
  (`cc` recompiles `hamt.c` etc. on every build).
- `--runtime=split` -- what `auto` does, but it insists on the preamble split
  described [below](#the-runtime-preamble----compiled-once-not-per-program)
  and says so when the split declines.

```
tur build --runtime=lib foo.tur -o foo     # force the archive link
tur build --runtime=source foo.tur -o foo  # force autolink+recompile sources
```

The mode also applies to `tur compile` (the `.link` sidecar then records the
runtime link) and composes with `--split-build`.
`TUR_RUNTIME=auto|lib|source|split` in the environment seeds the default for every
build (a CLI `--runtime=` flag still wins), so CI can flip the whole suite
over with one env var.

### Which archive gets linked

Two archives can back `--runtime=lib`, probed per directory in this order:

1. **`libturt_runtime.a`** (preferred) -- a lean, **non-sanitized** archive of
   exactly the autolinkable runtime TUs (`hamt.c`, `symbols.c`, `tur_string.c`).
   Built by the `turt_runtime` CMake target. Because it is non-ASan, linking it
   is behaviorally identical to the bare-source recompile -- no sanitizer is
   imposed on your program.
2. **`libturi.a`** (fallback) -- the full runtime library. In a Debug build this
   is AddressSanitizer-instrumented, so the ASan autodetect pulls
   `-fsanitize=address,undefined` into the link and your program then runs under
   ASan/LeakSanitizer. That is by design when only the full lib is available;
   use a Release/non-sanitized `libturi.a` (or `ASAN_OPTIONS=detect_leaks=0`) if
   you want a plain run.

Archive directories are searched in order: `$TUR_RUNTIME_LIB` (an archive file
or its directory) -> `<tur_exe_dir>/src` -> `<turmeric_root>/build/src`. A
prefix-installed SDK's `libturi.a` is found automatically once `-lturi` is on
the line. Set `TUR_RUNTIME_LIB` if your archive lives elsewhere.

### The runtime preamble -- compiled once, not per program

Every emitted translation unit opens with the same runtime preamble, about half
the C of a one-line program. On Linux and Windows a default `tur build` does not
hand that preamble to `cc`. It swaps in just the declarations and links the
definitions from **`libturt_preamble.a`**, which was compiled once when `tur` was
built. On a one-line program that halves the `cc` call. Across the fixture
suite it is about 10% of wall-clock.

The swap is all-or-nothing and it fails closed. A build keeps the whole preamble
inline, which is slower and always correct, whenever any of these holds:

- `libturt_preamble.a` is not next to `libturt_runtime.a`, for example after a
  `cmake --build build --target tur`, which builds neither archive.
- `TUR_CC_FLAGS` contains `-fsanitize`. The archive is not instrumented, so
  ASan or TSan would stop seeing the preamble's code.
- `--debug` is on, `--target wasm` is set, or `--runtime=lib`/`source` is set.
- The program is `#lang r7rs`, which has its own prelude split.
- The compiler's preamble no longer matches the committed split artifact (see
  `tools/gen-runtime-split.py`).
- The host is macOS, where the split is not the default yet.

Only builds that compile the whole program in one `cc` call take the split:
`tur build <file>`, `tur run`, `tur test`, and a project with a single `main`.
`tur emit-c` always writes the whole, self-contained TU. `tur compile`,
`--split-build`, `--shared` and separately compiled projects do not swap.

```
TUR_PREAMBLE_SPLIT=0 tur build foo.tur -o foo  # keep the whole preamble inline
TUR_PREAMBLE_SPLIT=1 tur build foo.tur -o foo  # opt in on macOS
tur build --runtime=split foo.tur -o foo       # insist; says so if it declines
```

To see which way a build went, run it with `TUR_SHOW_CC=1`. The link line names
`-lturt_preamble` when the build took the split.

## Why this exists

See [docs/archive/tur-link-and-build-split-plan.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/tur-link-and-build-split-plan.md).
The short version: splitting the compile from the link makes the object
compiles cacheable (ccache hits on unchanged runtime sources across every
fixture and every run) and stops re-compiling the runtime per build -- the
dominant cost in the test-suite wall-clock.
