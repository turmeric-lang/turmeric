# turmeric-godot does not compile against turmeric `main`

**Summary:** `turmeric-godot`'s `src/aot/aot_dispatch.cpp` includes
`runtime/ffi_dispatch_thunk.h` and calls `tur_ffi_thunk_call`. That header is
in turmeric `v0.44.2`, the tag the shim's CI pins, and is gone from `main`, so
a build against a sibling `main` checkout -- the layout `SConstruct` defaults
to (`../turmeric`) and the README describes -- stops at the first AOT file.

**Severity:** Low today (CI is pinned and green), but it is what will break
the day the pin is bumped, and it already breaks the documented local build.

**Repro** (Linux x86-64, 2026-11-24, turmeric-godot `0427a29`):

```sh
cd turmeric
cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON
cmake --build build-rel --target libturi -j
cd ../turmeric-godot
scons platform=linux arch=x86_64 target=template_debug
```

```
src/aot/aot_dispatch.cpp:15:10: fatal error: runtime/ffi_dispatch_thunk.h: No such file or directory
```

With `TURMERIC_ROOT` pointed at a `v0.44.2` checkout the same command builds.

## Root cause

`src/runtime/ffi_dispatch_thunk.{c,h}` -- the generated fixed-shape thunk
table -- was deleted when the interpreter's FFI went arbitrary-arity
([interpreter-arbitrary-arity-ffi-plan](../archive/interpreter-arbitrary-arity-ffi-plan.md),
[ffi-spices-integration-plan](../archive/ffi-spices-integration-plan.md)).
The shim reaches into turmeric's `src/` for a private header
(`SConstruct` adds `<turmeric>/src` to `CPPPATH`), so nothing on the turmeric
side knew it had a consumer.

## Fix directions

- Port `aot_dispatch.cpp` (the one call at `tur_ffi_thunk_call`) to whatever
  `main` dispatches a `void *` plus an `i`/`f` class string through now, then
  bump the CI pin in `.github/workflows/build.yml` in the same change.
- Longer term, the entry point the shim needs belongs in an installed
  `include/turi/` header rather than a path under `src/`, so a removal shows
  up as an API change.

Only the first missing header was observed; the build stops there, so there
may be further drift behind it.
