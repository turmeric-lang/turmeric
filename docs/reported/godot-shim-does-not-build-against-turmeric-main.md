# turmeric-godot does not compile against turmeric `main`

**Summary:** `turmeric-godot`'s `src/aot/aot_dispatch.cpp` includes
`runtime/ffi_dispatch_thunk.h` and calls `tur_ffi_thunk_call`. That header is
in turmeric `v0.44.2`, the tag the shim's CI pins, and is gone from `main`, so
a build against a sibling `main` checkout -- the layout `SConstruct` defaults
to (`../turmeric`) and the README describes -- stops at the first AOT file.

> Status: **fixed on the `claude/godot-packed-array-push-fy8kv9` branch of
> `turmeric-godot` (commit `10956a8`, 2026-11-24), not yet merged.** Held
> open until it ships. The drift went further than the header: see "What it
> took" at the end.

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

## What it took (2026-11-24, on the branch above)

Getting past the header exposed one more drift, and one fault that was never
drift at all.

1. **The header.** `aot_dispatch.cpp` no longer includes anything from
   turmeric's `runtime/`. On `main` the generic call path is the JIT thunk
   provider (`src/turi/jit_ffi.h`), which the shim does not link; but every
   export built by `tur build --shared` also carries a `<mangled>__ffi` shim
   taking the same `iv`/`fv` buffers, in `v0.44.2` and `main` alike.
   `AotImage` resolves it beside the export and dispatch calls it. That is the
   "installed header" direction above, reached by needing no header.
2. **A duplicate `defn` in the generated facade** -- the second drift. The
   generator emitted `resource/emit-changed` twice, for the method
   `emit_changed` and for the signal `changed`. `v0.44.2` let the second
   replace the first; `main` rejects it (`defn: 'resource/emit-changed' is
   already defined`), the facade fails to eval, and so does every script. The
   generator now drops a signal wrapper whose name a method wrapper took.
3. **AOT never loaded on Linux**, on either turmeric. Godot opens the
   extension without `RTLD_GLOBAL`, so a staged library's undefined
   `godot_*` references had nothing to bind to: `dlopen` failed with
   `undefined symbol: godot_packed_vec3_push` and the script fell back to the
   interpreter, which the pong driver's assertions cannot tell apart. The
   extension now re-opens itself `RTLD_NOLOAD | RTLD_GLOBAL` before the first
   AOT load.

Checked on Linux x86-64 with Godot 4.3, the shim built once against `v0.44.2`
and once against `main`: `examples/paddle-pong-tur` passes interpreted and
under `TURMERIC_GODOT_AOT=1` (ball and paddle load 110 exports and report
`first dispatch routed via AOT`), and the 27 spike drivers give the same
results on both.

The CI pin was left at `v0.44.2`; bumping it is now possible but is its own
decision. macOS and Windows were not run.

Left over, the same on both turmerics and so not drift:
[godot-spike-scripts-stale-against-typed-handles](godot-spike-scripts-stale-against-typed-handles.md).
