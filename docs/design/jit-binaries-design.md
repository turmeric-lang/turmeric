# JIT Binaries: a Second Output Format Next to Compiled Binaries

**Status:** Proposal -- not scheduled  
**Last updated:** 2026-10-04

---

## Question

Should `tur build` be able to produce a *JIT binary* as an alternative to the
native, `cc`-compiled binary -- an executable made of the program's JIT-ready
code plus a runtime shim that executes it?

## Short answer

Feasible, and most of the pieces already exist. One correction to the framing:
the payload cannot be JIT'd *machine code*; it has to be **MIR IR**, generated
to machine code by the shim at startup. Whether it is *sensible* depends on one
thing -- whether a toolchain-free `tur build` (no `cc` on the build machine)
matters. If it does, this is a strong feature. If it does not, it mostly loses
to the native binary.

---

## 1. Why the payload is MIR IR, not machine code

What `MIR_gen` produces is bound to the process that generated it:

- Imports are resolved by `MIR_link` through `jit_import_resolver`
  (`src/jit_engine.c`), i.e. `dlsym(RTLD_DEFAULT, ...)` against the live
  process, and baked in as absolute addresses.
- Calls go through lazy-gen thunks that are patched in place
  (`_MIR_redirect_thunk`).
- MIR has no relocatable object emission.

Persisting generated code would mean inventing a relocation format -- i.e.
writing a linker. Not worth it.

MIR *does* have a binary IR format: `MIR_write` / `MIR_read`
(`external/mir/mir.h`), the format upstream's `m2b` / `b2m` tools use. It is
already compiled into `tur_mir` (`cmake/mir.cmake` sets no `MIR_NO_IO`), but
the engine never calls it today (`docs/guides/jit-guide.md`, "What we use").

## 2. Proposed shape

```
tur build --engine jit foo.tur
  front half (unchanged) -> emitted C -> c2mir -> MIR module -> MIR_write -> foo.bmir
  output = prebuilt shim executable + appended foo.bmir payload (+ header)

./foo
  shim: locate payload -> MIR_read -> MIR_link (resolver: dlsym on itself)
        -> lazy MIR_gen -> __tur_static_init -> main(argc, argv)
```

This is the stub-plus-appended-payload pattern of `deno compile`,
`bun build --compile`, and PyInstaller. The build step **links nothing**: it
concatenates. The shim is a release artifact, shipped per target alongside
`tur`.

Payload header (sketch): magic, format version, the S2 runtime preamble hash
(section 4), target triple, the resolved autolink list, payload length.

## 3. What already lines up

- **The shim is approximately the `tur jit` host minus the compiler.** The S2
  preamble split already keeps ~60% of the runtime preamble resident in the
  host (`src/runtime/generated/tur_rt_split*.c`), plus host-resident atomics
  and TLS (`src/runtime/tur_atomics.c`, `src/runtime/tur_tls.c`). The shim is
  that runtime + `mir.c` + `mir-gen.c` + a loader, built with
  `ENABLE_EXPORTS` -- **without** `c2mir.c`.
- **c2mir moves to build time.** The C front end is the expensive part of a
  JIT run, and the bulk of resident memory (~25 MB of parsed headers per image,
  per `src/jit_engine.h`). The shim pays only for `MIR_read` + link + lazy gen.
- **`TurJitImage` is the precedent.** The REPL's persistent image already does
  compile -> link -> eager gen -> explicit `__tur_static_init` -> symbol lookup.
  The shim's loader is the same sequence with `MIR_read` replacing c2mir.
- **The fallback contract moves to build time.** `tur jit` falls back to `cc`
  at run time when c2mir rejects the C. For a JIT binary that decision happens
  at `tur build`: either emit a native binary instead, or fail with the c2mir
  diagnostic.

## 4. Costs and constraints

| Concern | Detail |
|---|---|
| **Not portable** | c2mir has already expanded system headers, struct layouts, and ABI decisions into the IR. A `.bmir` is per-OS/arch, exactly like a native binary. One shim per target; no "compile once, run anywhere". |
| **Version lock** | The payload assumes the exact runtime resident in the shim (the S2 preamble hash). Embed the hash in the payload header; the shim refuses a mismatch with a clear message rather than misbehaving. |
| **Platform policy** | A JIT binary is a JIT application: `MAP_JIT`, the `com.apple.security.cs.allow-jit` entitlement for a notarized hardened-runtime macOS binary, and **impossible on iOS**. A native binary has none of this. The MIR interpreter is not an escape hatch (`docs/archive/mir-interp-tier-plan.md`). |
| **Performance** | Upstream claims ~91% of `gcc -O2` steady-state speed, plus per-launch code generation (lazy, per function). Fine for long-running tools and servers; poor for short-lived CLIs invoked thousands of times. |
| **FFI is dlopen-only** | Autolinked libraries must exist as shared objects on the *target* machine and are `dlopen`'d `RTLD_GLOBAL` by the shim. No static linking of a user's C library into the artifact. |
| **Binary size** | Shim = runtime + MIR core + generator. Unmeasured; likely on the order of a native binary's runtime plus ~1 MB. |
| **Maintenance** | A third execution artifact (next to `cc` binaries and `tur jit`) to keep green on every target. |

## 5. Where it actually pays off

1. **Toolchain-free builds (strongest case).** `tur build` needs no `cc`, no
   Xcode Command Line Tools, no MinGW. This fits the tvm install path: the
   shim ships in the release tarball, and a fresh machine can produce runnable
   artifacts immediately.
2. **Plugins / hot-loadable modules.** A `.bmir` loaded by a host that links a
   JIT-enabled `libturi` is a natural plugin format. This is close to what the
   Godot embedding spike is after
   (`docs/reported/jit-godot-embedding-spike.md`), and may be the more
   valuable variant than standalone executables.
3. **Fast builds of large programs (weak case).** For iteration, `tur jit`
   already covers this; for distribution you usually want the optimized `cc`
   binary anyway.

## 6. Recommendation

- **If toolchain-free builds matter for v1:** run a timeboxed spike (a few
  days) answering three questions with measurements:
  1. **Round-trip fidelity.** Do `MIR_write` / `MIR_read` round-trip our c2mir
     output correctly? Run the fixture corpus through a write-then-read path
     in `tests/run-jit.sh`.
  2. **Size.** Shim size per target, and `.bmir` size for representative
     programs.
  3. **Cold start.** Wall time from process start to `main`, compared with the
     native binary.
- **Otherwise:** leave this as a recorded idea. It does not advance the v1
  track, and it adds a third backend to maintain.

## Decisions

- **c2mir rejects the C at build time -> fail.** `tur build --engine jit`
  exits with the c2mir diagnostic; it never silently substitutes a native
  binary. The two artifacts differ in platform policy (section 4), so asking
  for one and getting the other would be a surprise. Building native remains
  an explicit choice of engine.
- **Payload is appended to the shim.** One self-contained file per program,
  like `deno compile` / `bun build --compile`. No sidecar `.bmir`.

## Open questions

- **Eager-generating hot functions.** The payload could carry a list of
  functions to generate up front (e.g. gathered from a profiling run), with
  the rest still generated lazily, to reduce cold-start latency. Undecided.
  It only matters if the spike's cold-start measurement (section 6, question
  3) shows lazy generation is a real cost, so defer it until that number
  exists. Note it would add a profiling step to the build, and
  `TUR_JIT_GEN=eager` already gives an all-or-nothing baseline to compare
  against.
