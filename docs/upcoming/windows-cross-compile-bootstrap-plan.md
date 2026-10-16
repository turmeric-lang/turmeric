# Windows Cross-Compile Bootstrap Plan (no Windows machine)

**Status:** Draft. Companion to `windows-support-plan.md`.

**Last updated:** 2026-06-25

---

## Purpose

`windows-support-plan.md` answers *what* has to change for Windows. This doc
answers a narrower, practical question: **how do we iterate on that work from a
Linux box (this container / CI), without owning a Windows machine, and without
treating a `windows-latest` GitHub Action as the only way to build?**

It is grounded in probes run in-session on 2026-06-25, not speculation. The
reproductions are in the appendix.

---

## What we proved in-session

### 1. The MSVC compile toolchain is already here

| Tool | State | Note |
| --- | --- | --- |
| `clang` 18.1.3 | present | `clang --target=x86_64-pc-windows-msvc` and `clang --driver-mode=cl` both emit amd64 COFF objects today. |
| `lld-link` | present | the MSVC-style linker. |
| `llvm-rc` | present | Windows resource compiler. |
| `cargo` | present | can build/install `xwin`. |
| `mingw-w64` | apt-installable | reachable mirror; provides `unistd.h` + winpthreads. |
| `wine64` | apt-installable | runs PE32+ exes; see (3). |

So the *compilation* half of "cross-compile to Windows" needs no exotic setup.
The plan's `cmake/toolchain-windows-clang.cmake` can run locally, not just on a
runner.

### 2. The one hard blocker is egress, not tooling: the MSVC SDK is unreachable

`xwin` (the legitimate way to fetch the MSVC CRT + Windows SDK headers/import
libs from Microsoft, no Windows box) downloads from Microsoft's CDN. Both
required hosts are **denied by this environment's egress policy**:

```
https://aka.ms/vs/17/release/channel               -> CONNECT tunnel failed, response 403
https://download.visualstudio.microsoft.com/       -> CONNECT tunnel failed, response 403
```

These are genuine policy denials (proxy refuses the CONNECT), not host-side
quirks. The proxy README says do not retry policy denials -- so **xwin cannot
splat an SDK in-session as things stand.** Without the SDK, clang-cl can target
the MSVC triple but cannot find `stdio.h`/`kernel32.lib`/etc., so it cannot
*link* a real `tur.exe` or `.dll`.

By contrast these are reachable (200): `index.crates.io`, real
`static.crates.io/crates/...` paths, `archive.ubuntu.com` (apt), and
`github.com/.../releases`.

**Implication:** the MSVC-ABI artifacts the GDExtension actually ships (WIN0
`tur.exe`/`libturi.lib`, WIN2 `.dll`) must be linked where the SDK is reachable:
CI's `windows-latest`, or a host with an egress allowlist for the two MS hosts,
or against a **vendored, pre-splatted SDK** we host somewhere reachable.

### 3. The execution half works locally: wine runs real Windows exes

A PE32+ exe built with MinGW runs under `wine64` and propagates its exit code:

```
$ x86_64-w64-mingw32-gcc exehello.c -o exehello.exe   # PE32+ console x86-64
$ /usr/lib/wine/wine64 ./exehello.exe
hello-from-wine
exit code: 7
```

This confirms the earlier recommendation: **wine, not QEMU.** Same arch, no VM,
exit codes and stdout flow through. Once we can *produce* an exe, we can *run*
it here.

### 4. The WIN1 codegen blocker is real -- and fundamental, not MSVC-specific

`tur emit-c` on a trivial `(println "hello") 0` program emits a **monolithic
POSIX preamble** with zero platform gating
(`src/compiler/emit_module.c:5116-5132`):

```
#include <sys/select.h>   <- POSIX only
#include <sys/socket.h>   <- POSIX only
#include <netinet/in.h>   <- POSIX only
#include <arpa/inet.h>    <- POSIX only
#include <ucontext.h>     <- POSIX only (no MSVC and no MinGW equivalent)
#include <pthread.h>      <- POSIX only
...plus <unistd.h> later, and ~164 pthread/STM references
```

The STM runtime (`pthread_mutex_t`, `pthread_cond_t`, `pthread_once`,
`__atomic_*`) is emitted **even though hello-world never touches STM**. Both
toolchains die immediately:

```
clang --target=x86_64-pc-windows-msvc -c hello.c   -> fatal: 'sys/select.h' file not found
x86_64-w64-mingw32-gcc              -c hello.c     -> fatal: sys/select.h: No such file or directory
```

The MinGW failure is the important one: MinGW *has* `unistd.h` and winpthreads,
yet still cannot compile the output. So **WIN1 (gate the preamble) is required
no matter which Windows toolchain we pick** -- it is not an MSVC quirk we can
dodge by switching ABIs. `grep -c _WIN32` on the emitted file is `0`: there is
no platform conditioning to build on yet.

---

## Strategy: split the loop by what egress allows

The egress reality forces a clean separation that turns out to be a *good*
division of labor:

### Local, offline, today -- codegen portability via MinGW + wine

For the WIN1 question -- *"does the generated C reference any POSIX-only header
or symbol?"* -- the answer is ABI-independent. A POSIX header is missing on both
MSVC and MinGW. So **MinGW + wine is a legitimate local oracle for codegen
portability**, and it is the only fully-offline one given the blocked MSVC SDK:

1. Do the WIN1 preamble split (core vs async-runtime).
2. Locally: `x86_64-w64-mingw32-gcc` the gated output; run under `wine64`.
3. If a no-async program compiles clean and runs, the core preamble is
   POSIX-free. Necessary for MSVC, even if not sufficient.

This is a real edit -> compile -> run loop in this container, with no Windows
machine and no MS-CDN dependency.

**Caveat, stated loudly:** MinGW is the *wrong ABI to ship*. It validates
codegen portability (no POSIX headers/symbols), **not** the MSVC ABI the Godot
GDExtension `.dll` must match. Do not let a green MinGW+wine run stand in for
MSVC linkage. This is the same "don't validate the wrong ABI" caution as before
-- here MinGW is explicitly scoped to the ABI-independent portability check only.

### MSVC-ABI fidelity -- needs the SDK, so CI or an unblock

WIN0 `tur.exe`, the static `libturi`, and the WIN2 `.dll` must be built in the
MSVC ABI Godot loads. Pick one (not mutually exclusive):

- **A. Keep it in CI.** Author `cmake/toolchain-windows-clang.cmake` to work on
  `windows-latest`; iterate the MSVC-specific bits there. Simplest, zero new
  infra, but it is the "GH Action is clunky" path for the MSVC half.
- **B. Vendor a pre-splatted SDK (recommended for local fidelity).** Run `xwin
  splat` once somewhere with MS-CDN access, tar the result, host it as a GitHub
  release asset (github.com *is* reachable here), and point the toolchain file's
  `/winsysroot` at the unpacked tarball. This removes the MS-CDN dependency from
  every build -- local *and* CI -- and makes CI deterministic. Requires
  confirming the MSVC redistributable license permits this redistribution.
- **C. Request an egress allowlist** for `aka.ms` and
  `download.visualstudio.microsoft.com` so `xwin` runs in-session directly. The
  cleanest if org policy can accommodate it; nothing to vendor.

Recommended sequencing: **A immediately** (unblocks MSVC iteration in CI with no
new infra) + **B as the durable fix** for local MSVC fidelity. C is strictly
better than A if policy allows it -- ask first.

### One toolchain file, two drivers

Whichever of A/B/C, write a single `cmake/toolchain-windows-clang.cmake` that:

- uses `clang --target=x86_64-pc-windows-msvc` + `lld-link` + `llvm-rc`;
- takes the winsysroot from a variable (`-DWINSYSROOT=...`) so it points at
  xwin output (B/C) or the runner's installed SDK (A) without edits.

Then "local" and "CI" are the same code path with a different `WINSYSROOT`,
which is the concrete answer to "GH Action alone is clunky": CI becomes
*confirmation*, not the only way to build.

### Wire wine into the existing harness

`tests/run.sh` already honors `CC` (`BUILD_CC="${CC:-cc}"`). Add one
indirection at the execute step (~line 467) through `${TUR_RUN_WRAPPER:-}` so a
cross-built exe runs under wine:

```sh
CC="x86_64-w64-mingw32-gcc" TUR_RUN_WRAPPER="/usr/lib/wine/wine64" \
  bash tests/run.sh           # WIN1 codegen-portability subset, fully local
```

The same `TUR_RUN_WRAPPER` hook serves the MSVC path under wine later. Gate this
to the `windows-core` fixture group the plan proposes (WIN1 testing section)
until the preamble split lands -- the full suite will not pass until WIN3.

---

## Concrete next steps (in order)

1. **WIN1 preamble split** in `src/compiler/emit_module.c` (~5100-5145): emit a
   "core" preamble (Result/Option, RC, cons, format -- C-stdlib + `<stdatomic.h>`
   for the RC counters) always, and move `sys/select.h` / `sys/socket.h` /
   `netinet` / `arpa` / `ucontext.h` / `pthread.h` + the STM block behind the
   async-runtime path (reachability already known from the elaborated AST).
2. **Local oracle**: `mingw-w64` + `wine64` (both installed in-session now) to
   drive that split -- compile + run the gated output offline.
3. **`TUR_RUN_WRAPPER` hook** in `tests/run.sh`; add a `windows-core` fixture
   group mirroring the `turmeric-godot` script shapes.
4. **Toolchain file** `cmake/toolchain-windows-clang.cmake` parameterized on
   `WINSYSROOT`.
5. **Decide the SDK story** (A/B/C above) -- needs a human call on policy and on
   the MSVC redistributable license for option B.

---

## Open questions for the maintainer

1. **SDK acquisition (A/B/C).** Is an egress allowlist for the two MS hosts
   possible (C)? If not, are we comfortable vendoring a pre-splatted SDK as a
   release asset (B), license permitting? Otherwise the MSVC half stays in CI
   (A).
2. **MinGW as a sanctioned local oracle.** `windows-support-plan.md` rules out
   MinGW *as a shipping toolchain*. This doc proposes MinGW only as an offline
   *codegen-portability* check, never for ABI. Is that scoped use acceptable, or
   do you want the local loop to also be MSVC (which then hard-depends on B/C)?
3. **Preamble split granularity.** Mirrors the parent plan's open question 1: is
   gating just `<pthread.h>` + the socket/ucontext/STM block enough, given RC
   atomics already work under both toolchains via `__atomic_*` / `<stdatomic.h>`?

---

## Appendix: reproductions (run 2026-06-25, this container)

```sh
# (0) toolchain present
clang --target=x86_64-pc-windows-msvc -c t.c -o t.obj   # -> amd64 COFF
lld-link --version                                      # -> LLD 18.1.3

# (1) egress: MSVC SDK hosts blocked, everything else reachable
curl -sS -o/dev/null -w '%{http_code}' https://aka.ms/vs/17/release/channel   # CONNECT 403
curl -sS -o/dev/null -w '%{http_code}' https://download.visualstudio.microsoft.com/  # CONNECT 403
curl -sS -o/dev/null -w '%{http_code}' https://static.crates.io/crates/xwin/xwin-0.6.5.crate  # 200
curl -sS -o/dev/null -w '%{http_code}' http://archive.ubuntu.com/ubuntu/dists/noble/Release    # 200

# (2) execution half: real exe under wine
x86_64-w64-mingw32-gcc exehello.c -o exehello.exe       # PE32+ console x86-64
/usr/lib/wine/wine64 ./exehello.exe ; echo $?           # "hello-from-wine"; 7

# (3) WIN1 blocker: trivial turmeric program, both toolchains
./build/tur emit-c hello.tur > hello.c
grep -c _WIN32 hello.c                                   # 0 (no platform gating)
clang --target=x86_64-pc-windows-msvc -c hello.c         # fatal: sys/select.h not found
x86_64-w64-mingw32-gcc              -c hello.c            # fatal: sys/select.h not found
```
