# `(c-fn ...)` typedef emitted to per-module `.c` but referenced by the `.h`

> **FIXED (verified 2026-06-15):** Option A landed. Under separate compilation
> a `(c-fn [...] R)` typedef referenced by an exported defn's signature is now
> registered during the header walk and flushed into the per-module `.h` ahead
> of the declaration that names it (see `src/compiler/emit_module.c` -- the
> `cfnptr-typedef-emitted-to-c-not-h` comment blocks register the result/param
> typedefs at ~6528-6552, and `type_codegen_emit_fn_ptr_typedefs(out)` flushes
> them into the header at ~6653, before the forward-decl loop). A two-module
> `--shared` project that exports a c-fn-param defn and imports it across the
> `.h` boundary now links clean; the typedef is defined in `app__sink.h` before
> its use. Locked in by the `build-shared-exported-cfn-typedef-in-header`
> regression case in `tests/run-build-project.sh`. The original repro depended
> on the sibling spices checkout; the regression is reproduced entirely
> in-repo with a synthetic two-module project, so no spice is required.

**Status:** Fixed (see banner above). Surfaced 2026-06-14 while applying the
spice-side follow-up of `c-fn-ptr-element-and-size-precision-gap.md` -- updating
`spices/rtmidi/src/rtmidi/in.tur:midi-in-set-callback` to the precise
`(c-fn [float ptr<const-u8> usize ptr<void>] void)` signature and
dropping the `(RtMidiCCallback)callback` cast.

**Severity:** Medium.  Project-mode `tur build <dir>` of any spice that
**exports** a defn whose signature contains a `(c-fn ...)` parameter
fails to compile: the per-module `.h` declares the exported function
using the precise fnptr typedef name (e.g.
`tur_fnptr_void_double_const_uint8_t___size_t_void___t`), but the
typedef itself is emitted only into the per-module `.c`, *after* the
`.c` has already included its `.h`.  The C front-end then treats the
unknown identifier as implicit `int`, and the in-`.c` decl/def conflict
with the `.h` decl.  Pre-existing -- happens both with the report's
old workaround-cast signature and with the precision-fixed signature
shipped in turmeric `28396c06`, so this is *not* a regression of the
precision fix; it has been latent ever since the export path started
splitting modules into `.c`/`.h` pairs.

**Scope:** the `(c-fn ...)` fn-ptr typedef registry in
`src/compiler/emit_*` -- specifically the side that decides which
translation unit each registered typedef lands in for project-mode
builds.

## Minimal repro

```sh
git clone https://github.com/turmeric-lang/turmeric-spices /tmp/spices
cd /tmp/spices && git checkout migrate-cfn-s1-2026-06-14
cd spices/rtmidi
CPATH=$(brew --prefix rtmidi)/include LIBRARY_PATH=$(brew --prefix rtmidi)/lib \
  tur build .
```

Excerpt from the failure (precision-fixed signature, but the cast-only
variant fails identically with the older typedef name):

```
build/obj/rtmidi__in.h:161:54: error: type specifier missing,
        defaults to 'int'; ISO C99 and later do not support implicit int
  void rtmidi__in__midi_hyin_hyset_hycallback(int64_t,
       tur_fnptr_void_double_const_uint8_t___size_t_void___t);
                                                   ^
build/obj/rtmidi__in.c:216:6: error: conflicting types for
        'rtmidi__in__midi_hyin_hyset_hycallback'
build/obj/rtmidi__in.h:161:6: note: previous declaration is here
```

Layout in `rtmidi__in.c`:

```
line   2 : #include "rtmidi__in.h"          <- .h decl uses typedef name
line 128 : typedef void (*tur_fnptr_..._t)(...)   <- typedef defined here
line 216 : void rtmidi__in__midi_hyin_hyset_hycallback(int64_t,
                 tur_fnptr_..._t);              <- in-.c forward decl
line 1049: void rtmidi__in__midi_hyin_hyset_hycallback(int64_t mi,
                 tur_fnptr_..._t callback) { ... }
```

`grep -l tur_fnptr_void_double_const_uint8_t build/obj/*.{c,h}` returns
only `rtmidi__in.c` and `rtmidi__in.h`; the typedef is *referenced* in
`rtmidi__in.h` but only *defined* in `rtmidi__in.c`.

## Observed vs expected

- **Observed:** project-mode codegen places the fn-ptr typedef into the
  `.c` file even when the same typedef is referenced from the
  corresponding `.h`, which means including the `.h` (from itself, or
  from any consumer TU that imports this module) hits an unknown type
  name.
- **Expected:** any fn-ptr typedef referenced from a `.h` declaration
  must be emitted *into that `.h`* (or into a header that the `.h`
  transitively includes, e.g. `tur_runtime.h`), *before* the
  declaration that uses it.  Single-file `tur emit-c <file>` already
  does the right thing because there is no `.h` boundary; project mode
  is where the regression manifests.

## Root-cause pointers

- `src/compiler/emit_*` -- the fn-ptr typedef registry
  (`type_codegen_emit_fn_ptr_typedefs`, per the c-fn-precision-gap
  report's "Implementation" section) emits to the current per-module
  `.c`.  For exported defns whose signatures touch the typedef, the
  emit needs to be promoted to the matching `.h`.
- A practical fix is to emit every cfn typedef that appears in any
  exported defn's signature into the consolidated `tur_runtime.h`
  (alongside the other registry typedefs the runtime header already
  carries), keeping the `<stddef.h>` lazy-include logic adjacent.

## Why a separate report

The c-fn-precision-gap fix shipped 2026-06-15 and was verified by a
new `tests/fixtures/c-fn-ptr-size-const-precise/` regression fixture
that round-trips the precise lowering at runtime *within a single
translation unit*.  That fixture does not exercise the
multi-TU project-mode export boundary, so this defect slipped through.

## Proposed fix directions

### Option A -- promote to `tur_runtime.h` (recommended)

When emitting a fn-ptr typedef whose name appears in any exported
defn's declared signature, emit it to `tur_runtime.h` instead of the
per-module `.c`.  Cheap because the registry already has the typedef
name + spelling cached; the `<stddef.h>` gating already lives next to
the emitter.

Pros: zero per-consumer churn -- every TU that includes
`tur_runtime.h` already sees the typedef.  Same approach the runtime
already uses for `tur_thunk_fn`, `defer_fn_t`, etc.

Cons: requires deciding the export set during emit (the export list
already drives header emission, so this is already known).

### Option B -- promote to the per-module `.h`

Emit the typedef into `rtmidi__in.h` directly, just before the
declaration that uses it.

Pros: keeps `tur_runtime.h` smaller.

Cons: redundant if the same typedef appears in multiple module
headers; harder to reason about across modules.

### Option C -- never emit the typedef into the `.h`

Have the `.h` use the canonical raw fn-ptr spelling
(`void (*)(double, const uint8_t *, size_t, void *)`) inline and keep
the typedef as a `.c`-local convenience.  Awkward at call-sites
(consumers can't name the type to declare a callback variable) but
sidesteps cross-TU concerns.

**Recommendation:** Option A.  It composes with the existing fn-ptr
typedef registry, fixes consumers transparently, and matches the
pattern already used for the rest of the runtime's typedefs.

## Validation

- A new project-mode fixture under
  `tests/fixtures/c-fn-ptr-exported-cfn-multi-tu/` that exports a defn
  whose signature carries `(c-fn [...] T)` and is consumed from a
  sibling module.  Should compile clean under `tur build <dir>`.
- Rebuild `spices/rtmidi` against the fix; the spice-side change
  prepared on `migrate-cfn-s1-2026-06-14` (precise
  `ptr<const-u8>` / `usize` signature, cast dropped) should
  link without `-Wimplicit-int` / `-Wincompatible-function-pointer-types`.

## Cross-references

- [docs/reported/c-fn-ptr-element-and-size-precision-gap.md](./c-fn-ptr-element-and-size-precision-gap.md)
  -- the parent fix; this defect blocks the "drop the cast" follow-up.
- `../turmeric-spices` branch `migrate-cfn-s1-2026-06-14`,
  `spices/rtmidi/src/rtmidi/in.tur:midi-in-set-callback` -- carries
  the precision-fixed signature; rebuild blocked on this defect.
