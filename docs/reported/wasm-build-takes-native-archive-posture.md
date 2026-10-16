# `tur build --target wasm` emits the native-archive form of the runtime

**Severity:** medium. When a native runtime archive can be found (a normal full
build or an install), a wasm build emits C that expects the runtime's
definitions to come from that archive. emcc cannot link a native archive and
is not given one.

## Repro

```
$ : > /tmp/x/libturt_runtime.a        # any locatable archive triggers it
$ TUR_RUNTIME_LIB=/tmp/x/libturt_runtime.a tur build --target wasm hello.tur
$ grep -c "definitions live in the runtime archive" /tmp/tur-build/hello_tur.c
2
$ tur build --target wasm hello.tur    # no archive locatable
$ grep -c "definitions live in the runtime archive" /tmp/tur-build/hello_tur.c
0
```

In the first case the emitted C carries only the region and rc/GC
prototypes, and the emcc command line names no archive. emcc was not
available where this was found, so the resulting link failure is inferred,
not observed.

## Root cause

`src/main.c:resolve_rcgc_from_archive` decides between the full runtime
replica and the archive form purely from whether an archive is locatable. It
never checks the build target.

## Fix directions

- Return the replica (full definitions in the emitted C) when the target is
  wasm, or more generally whenever the located archive does not match the
  target.
- Add a wasm build to CI on a tree that has a native archive.

See [runtime-preamble-and-linkage-guide](../guides/runtime-preamble-and-linkage-guide.md).
