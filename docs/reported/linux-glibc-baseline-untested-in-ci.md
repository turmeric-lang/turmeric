# Nothing in CI builds turmeric against an older glibc

**Summary:** Every Linux job in `.github/workflows/ci.yml` runs on
`ubuntu-latest`. A source that leans on something only newer glibc declares
without a feature macro therefore builds green here and fails for anyone on an
older distribution. One such case shipped in `v0.73.0`; it is fixed, but the
gap that let it through is not.

**Severity:** Low. It bites downstream builders, not users of the release
tarballs, and each instance is a one-line fix once seen.

## The instance that shipped

`src/jit_engine.c` and `src/turi/jit_ffi.c` call `dlsym(RTLD_DEFAULT, ...)`.
glibc before 2.36 declares `RTLD_DEFAULT` only under `_GNU_SOURCE`;
`src/CMakeLists.txt` asks for `_DEFAULT_SOURCE` on top of `-std=c11`. Newer
glibc follows POSIX.1-2024 and exposes it unconditionally, so `ubuntu-latest`
(2.39) compiles it and Ubuntu 22.04 (2.35) does not. With the JIT on, which is
the default:

```
src/turi/jit_ffi.c:419:18: error: 'RTLD_DEFAULT' undeclared (first use in this function)
src/jit_engine.c:991:17: error: 'RTLD_DEFAULT' undeclared (first use in this function)
```

Found by `turmeric-godot`'s CI, whose Linux legs build `libturi.a` on
`ubuntu-22.04` so the shipped extension loads on an older glibc, the first
time its pin moved past the JIT (turmeric-godot#7).

Fixed 2026-11-24: both files define `_GNU_SOURCE` before their first include.
The guard is easy to see without a 22.04 machine -- compiled the way turmeric
compiles it, `<dlfcn.h>` leaves `__USE_GNU` undefined, and that macro is what
2.35's header puts `RTLD_DEFAULT` behind.

## Fix direction

Give one Linux job an older base (`ubuntu-22.04`, or a container with an older
glibc) that builds `tur` and `libturi` with the default options. It need not
run the suite; the failure mode is a compile error. Worth deciding at the same
time what the oldest supported glibc is, since the release tarballs are built
on `ubuntu-latest` too and carry that glibc's symbol versions.
