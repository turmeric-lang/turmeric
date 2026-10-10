# The emitted unit's `_XOPEN_SOURCE 700` guard is inert on glibc, and it stranded a release

**RESOLVED 2026-10-03** by fix direction 1, in the fork
([turmeric-lang/mir#6](https://github.com/turmeric-lang/mir/pull/6)) and
re-synced into `external/mir/` (merged as `3c0d8c84`) -- see *Resolution* at the end.
The release workflow's aarch64 exception for this fallback is deleted, so the
next release's linux-aarch64 leg is the real-hardware confirmation.

**Severity: high -- it cost the v0.60.0 release.** The `linux-aarch64` leg of
[release run 37110162147](https://github.com/turmeric-lang/turmeric/actions/runs/37110162147)
failed at *Run a program through the JIT from the extracted archive*, so
`Create Release` was skipped by design and `v0.60.0` is a tag with no release,
no assets and no attestation. Product impact on its own is narrower: `tur jit`
on `linux-aarch64` always falls back to the cc path with `TUR-W0070`, which
still prints the right answer. The **cc path is unaffected** -- gcc parses the
offending header fine -- and the archive's *Compile a program from the extracted
archive* step passed.

Found 2026-10-03 cutting v0.60.0. Not a regression in this release: the step
that catches it was added by `923b803ae`, the same commit that made `TUR_JIT`
default ON, so v0.60.0's aarch64 archive is the first to ship the engine on that
platform. `release.yml`'s own comment had named the blind spot --
"linux-aarch64 is a leg no CI job runs the engine on."

## Repro

Needs an aarch64 Linux host (an `ubuntu-24.04-arm` runner; glibc 2.39 here).
On one, from an extracted release archive or a build tree:

```sh
printf '(defn main [] : int (println "hi") 0)\n' > hello.tur
TUR_JIT_DUMP_C=jit.c ./build/tur jit hello.tur
# /usr/include/aarch64-linux-gnu/sys/user.h:30:1: syntax error on struct (expected '<declarator>')
# tur: warning: TUR-W0071: split-runtime path failed to compile; retrying with the full preamble
# /usr/include/aarch64-linux-gnu/sys/user.h:30:1: syntax error on struct (expected '<declarator>')
# tur: warning: TUR-W0070: jit engine could not compile this program; falling back to the cc path
# hi
```

Both arms of the retry fail at the same header line, so the preamble split is
not implicated. The build-tree and extracted-archive dumps are byte-identical
(256,767 bytes), so the archive adds nothing either.

The chain needs no turmeric at all:

```sh
grep -rln 'sys/user\.h' /usr/include
# /usr/include/aarch64-linux-gnu/sys/procfs.h        <- the only one

printf '#include <ucontext.h>\n' | gcc -H -fsyntax-only -xc - 2>&1 | grep user.h
# ... sys/user.h                                      <- reached

printf '#define _XOPEN_SOURCE 700\n#include <ucontext.h>\n' |
  gcc -H -fsyntax-only -xc - 2>&1 | grep user.h
# (nothing)                                           <- NOT reached
```

Of the emitted unit's 19 includes, `<ucontext.h>` is the only one that reaches
`sys/user.h`:

```
<ucontext.h>
 +- aarch64-linux-gnu/sys/ucontext.h
     +- aarch64-linux-gnu/sys/procfs.h
         +- ... -> sys/user.h
```

## Root cause

`src/compiler/emit_module.c:13251` emits `#define _XOPEN_SOURCE 700`
immediately before `<ucontext.h>` -- but `:13237`-`:13240` have already emitted
`<sys/select.h>`, `<sys/socket.h>`, `<netinet/in.h>` and `<arpa/inet.h>`. In the
emitted text those land at lines 53-56 and the `#define` at line 63. By then
glibc's `features.h` has been processed, and it is include-guarded, so the macro
is never re-evaluated: it has no effect on anything.

`sys/ucontext.h` is therefore read with `__USE_MISC` live, which is what makes
it include `sys/procfs.h`. That reaches `sys/user.h`, whose
`struct user_fpsimd_struct` declares `__uint128_t vregs[32]` at the line c2mir
names.

The ordering is deliberate and load-bearing on macOS, which is why the obvious
fix is wrong:

- `:13231` (**T24**) -- the BSD networking headers MUST be processed *without*
  `_XOPEN_SOURCE`, or macOS include guards lock out `INADDR_*`, `sockaddr_in`
  and friends.
- `:13247` (**T21**) -- `<ucontext.h>` MUST precede `setjmp.h`/`pthread.h`, or
  macOS locks in a 56-byte `ucontext_t` where `FiberBlock` needs the full
  880-byte layout.

So **do not simply hoist the `#define` to the top of the unit.** It would
satisfy glibc and break both macOS constraints.

`linux-x86_64` passes the same step. The likely reason is that x86-64's
`sys/user.h` declares its register structs with plain integer types rather than
`__uint128_t` -- *not verified*, and worth checking before relying on it.

## Established by reading (2026-10-03): c2mir has no `__uint128_t` on Linux aarch64

glibc 2.39's `aarch64-linux-gnu/sys/user.h` (read from Ubuntu's
`libc6-dev-arm64-cross` 2.39-0ubuntu8cross1, the runner's version):

```c
30  struct user_fpsimd_struct
31  {
32    __uint128_t  vregs[32];
33    unsigned int fpsr;
34    unsigned int fpcr;
35  };
```

`__uint128_t` is a compiler builtin type name, so it is only a type in c2mir if
the target's predefined header declares it.  `external/mir/c2mir/aarch64/mirc_aarch64_linux.h`
does -- `typedef struct {_Alignas(16) unsigned long hi; unsigned long lo;} __uint128_t;`,
the layout-only stand-in fork commit `90633091` aligned to 16 -- but **only in
its `#elif defined(__APPLE__)` branch**.  On Linux aarch64 the name is
undeclared, line 32 is `<identifier> <identifier>[32];`, and c2mir reports the
failure at the start of the enclosing declaration: line 30, column 1, "syntax
error on struct (expected '<declarator>')" -- exactly the message every `tur
jit` run on that platform prints.  x86-64 never reaches it: no x86-64 glibc
header on the `<ucontext.h>` path names the type.

So the fix is one line in the fork: declare the same stand-in on Linux
aarch64 (outside the Apple branch).  `struct user_fpsimd_struct` then parses
with glibc's layout (512 + 8 bytes, 16-aligned), `<ucontext.h>` compiles, and
nothing in the unit does arithmetic on the type.  It is *not* measured here --
this container is x86-64 and no CI job runs the engine on aarch64 -- so the
release workflow's archive JIT step (or an `ubuntu-24.04-arm` probe run) is the
check.  User inline C that does arithmetic on `__uint128_t` stays a separate,
larger gap ([c2mir-rejects-uint128](c2mir-rejects-uint128.md)).

## Fix directions

**Option B -- keep `sys/procfs.h` out of the unit -- is measured DEAD.** Tried in
[probe run 37111947470](https://github.com/turmeric-lang/turmeric/actions/runs/37111947470)
by predefining `_SYS_PROCFS_H` ahead of `<ucontext.h>`. `sys/ucontext.h` does not
merely *include* procfs.h, it **uses** it:

```c
/usr/include/aarch64-linux-gnu/sys/ucontext.h
39: typedef elf_greg_t greg_t;
42: typedef elf_gregset_t gregset_t;
45: typedef elf_fpregset_t fpregset_t;
```

so the stub turns into `error: unknown type name 'elf_greg_t'` and
`<ucontext.h>` stops compiling at all. The same patch in the emitter broke **the
cc path as well** (`tur: cc invocation failed (status 256)`) -- strictly worse
than the bug, since the cc path is what works today. Do not retry this shape.

Worth noting why the narrower variant ("stub procfs, but supply the three
`elf_*` typedefs") is also unpromising: on aarch64 glibc `elf_fpregset_t` is
`struct user_fpsimd_struct` -- the very struct carrying `__uint128_t`. If that
holds, `<ucontext.h>` on this platform **cannot** be included without a
frontend that understands the type. *Unverified* -- read
`/usr/include/aarch64-linux-gnu/sys/procfs.h` on an arm box before relying on
it.

That leaves:

1. **Declare c2mir's `__uint128_t` stand-in on Linux aarch64** in the vendored
   fork (see "Established by reading" above) -- now the primary route, and a
   one-line change; full 128-bit arithmetic is not needed for the headers.
   Originally: **teach c2mir `__uint128_t`**.
   It fixes this *and* user inline C on arm64, and it disturbs none of the
   carefully-ordered include dance. See
   [c2mir-rejects-uint128](c2mir-rejects-uint128.md),
   `external/mir/VENDORED.md`, and the layout prior art in
   `docs/archive/history/jit-arm64-uint128-align-struct-layout-skew.md`.
2. **Emit `<ucontext.h>` only when the program needs `FiberBlock`.** The unit
   includes it unconditionally; a program that uses no fibers would then never
   reach the header, and `hello.tur` is such a program. *Untested*, and it
   narrows rather than closes the gap -- any fiber-using program on aarch64
   still falls back -- but it is the only route that does not wait on a fork
   change. Check what else in the preamble references `ucontext_t`
   unconditionally before costing this.
3. **Fix the inert `_XOPEN_SOURCE` properly** -- make it effective on glibc
   without violating T24/T21. Nothing cheap suggests itself: the macro has to
   be live before `features.h` is first processed, and T24 requires the
   networking headers to be processed *before* it is. A separate translation
   unit for the fiber code is the shape that could satisfy both, which is a much
   larger change than this bug justifies on its own.
4. Relaxing the aarch64 JIT check is **not** a fix: it would publish an archive
   whose shipped engine cannot run, which is exactly what the step was added to
   prevent.

Whichever lands, `v0.60.0`'s tag has to move onto it (or the fix ships as
`v0.60.1`) -- the tag as pushed has no release behind it.

## Resolution (2026-10-03)

Direction 1, but not as the one line this report expected.  Declaring the
`__uint128_t` stand-in for Linux aarch64 makes `<ucontext.h>` parse, and
measured on its own it would have made things worse: glibc's `mcontext_t`
carries `unsigned char __reserved[4096] __attribute__ ((__aligned__ (16)))`,
glibc's `<sys/cdefs.h>` defines `__attribute__(xyz)` to nothing for a
compiler that is neither gcc nor clang, and c2mir ignored `aligned` anyway.
So `ucontext_t` came out 4544 bytes / align 8 against gcc's 4560 / 16, and
glibc's `getcontext`/`swapcontext` -- compiled by gcc -- would write 16 bytes
past the end of a JIT-allocated one, inside `FiberBlock`.  The loud fallback
would have become silent memory corruption in fiber programs.

The fork commit (`e502b185`, merged as `3c0d8c84` in turmeric-lang/mir#6) therefore carries four
changes: the stand-in declared outside the Apple branch; an empty
function-like `#define __attribute__` (the libc erase idiom) ignored outside
pedantic mode, so attributes reach c2mir's parser; a run of attribute
specifiers merged into one list (glibc's `<pthread.h>` needed it once they
did); and a member's trailing `aligned (N)` raising its alignment as
`_Alignas` does.  `external/mir/VENDORED.md` logs it.

Measured, with Ubuntu's arm64 glibc 2.39 (this runner's) under qemu-user:

- `ucontext_t` / `mcontext_t` equal aarch64 gcc's layout (4560/16/176,
  4384/16/288), and so does a real emitted unit's `FiberBlock` (9648/16 and
  every checked offset); master fails the same `_Static_assert`s.
- A cross-built aarch64 `tur jit` runs hello-world natively -- `hi`, no
  TUR-W0070 -- where the same build against master MIR prints exactly this
  report's two `sys/user.h:30:1` errors, TUR-W0071 and TUR-W0070.
- `tests/run-jit.sh` with that `tur`, filtered to the fiber / async /
  channel / scheduler / generator fixtures: 72 passed, 0 failed; the 3 cc
  fallbacks (`cancel-chan`, `httpd-async-*`) fall back identically on
  x86-64 (a link-level decline).
- x86-64, where glibc's attributes now reach c2mir too: the full JIT corpus
  3430 passed, 0 failed, no new fallbacks; `tests/run.sh` 3557/0; the fork's
  c-tests 1090/1090 with both the generator and the interpreter.

Not measured: real arm64 hardware (qemu-user only), and macOS / MinGW headers,
which the `__attribute__` change reaches where they use the same idiom.

The release workflow's linux-aarch64 exception ("narrowed rather than off")
said to delete it when the fork fix landed; it is deleted, so a TUR-W0070 on
any leg fails the release again.  `VERSION` already reads 0.60.1, so the next
cut carries the fix; the `v0.60.0` tag still has no release behind it.
User inline C that does arithmetic on `__uint128_t` was still a gap --
[c2mir-rejects-uint128](c2mir-rejects-uint128.md), narrowed to
that, and resolved 2026-10-10.
