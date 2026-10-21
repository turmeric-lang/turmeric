# A `:cmake-deps` archive's own static dependencies are not repeated after a later archive that needs them (GNU ld: undefined reference)

**Severity:** medium (link failure on GNU ld, so Linux and MinGW; latent today).
When one `:cmake-deps` entry's static library depends on a static library from
an earlier entry, and that library in turn depends on a third, the link line
lists the second library again after the first but not the third. GNU ld does
not revisit earlier archives, so the link fails with undefined references.
macOS's ld64 and lld are order-insensitive and link fine, so a spice written on
a Mac can ship broken for Linux.

No spice in turmeric-spices has this shape on `main` today. The first live
case is `tur-nng` with TLS: nng (static) uses mbedTLS, mbedx509 and
mbedcrypto, all declared in the same manifest's earlier `mbedtls` entry
([nng-tls-plan](../upcoming/spices/nng-tls-plan.md)). Found there on
2026-10-17.

## Repro (offline; three tiny local CMake libraries)

`mid` links `core` the way `mbedtls` links `mbedx509` / `mbedcrypto`. `top`
calls both directly and picks up `mid` with `if (TARGET mid)`, the way nng's
TLS engine does.

```sh
R=$(mktemp -d) && cd "$R" && mkdir -p deps/midcore deps/top app/src
cat > deps/midcore/CMakeLists.txt <<'EOF'
cmake_minimum_required(VERSION 3.20)
project(midcore C)
add_library(core STATIC core.c)
add_library(mid STATIC mid.c)
target_link_libraries(mid PUBLIC core)
EOF
printf 'int core_value(void) { return 2; }\n' > deps/midcore/core.c
printf 'int core_value(void);\nint mid_value(void) { return 38 + core_value(); }\n' > deps/midcore/mid.c
cat > deps/top/CMakeLists.txt <<'EOF'
cmake_minimum_required(VERSION 3.20)
project(top C)
add_library(top STATIC top.c)
if (TARGET mid)
  target_link_libraries(top PRIVATE mid)
endif()
EOF
printf 'int mid_value(void);\nint core_value(void);\nint top_value(void) { return mid_value() + core_value(); }\n' > deps/top/top.c
cat > app/build.tur <<'EOF'
(defpackage app
  :name "app"
  :version "0.1.0"
  :cmake-deps #map{
    "midcore" #map{:path "../../deps/midcore" :targets ["mid" "core"]}
    "top"     #map{:path "../../deps/top"     :targets ["top"]}})
EOF
cat > app/src/main.tur <<'EOF'
(defn answer [] : int
  ```c
  extern int top_value(void);
  return top_value();
  ```)

(defn main [] : int
  (println (answer))
  0)
EOF
cd app && tur fetch && tur run src/main.tur
```

`cmake/spice-deps-manifest.json` gets these `link_flags` (basenames):

```
midcore  libmid.a libcore.a libcore.a
top      libtop.a libmid.a
```

and the link fails:

```
top.c:(.text+0x15): undefined reference to `core_value'
mid.c:(.text+0x9): undefined reference to `core_value'
```

Measured 2026-10-17 on `main` 3d9e02a2 (tur v0.65.5), GNU ld 2.42.

**Workaround:** list the transitive targets on the consuming entry:
`"top" #map{... :targets ["top" "mid" "core"]}`. That puts `libmid.a` and
`libcore.a` after `libtop.a`, and the program prints `42`. nng-tls-plan uses
this form for the `nng` entry.

## Root cause

`emit_link_flags_prelude` (`src/compiler/pkg.c:3440`) gives each entry its own
`:targets` artifacts, then each target's `_tur_resolve_link_iface`.
`_tur_walk_iface`, which `emit_link_iface_helper` emits (`pkg.c:3365`),
appends a **linkable** interface entry's `$<TARGET_FILE>` but recurses only
into **non-linkable** ones (the OBJECT and INTERFACE libraries its comment is
about). So the walk stops at an archive: `top`'s interface yields `libmid.a`
and never reaches `mid`'s own `core`. `pkg_cmake_manifest_append_cc_flags`
(`pkg.c:4954`) then concatenates the entries in declaration order, and
`libcore.a` appears only before `libtop.a`.

## Fix directions

1. **Keep walking through archives.** In `_tur_walk_iface`, after appending a
   linkable target's `$<TARGET_FILE>`, walk that target's
   `INTERFACE_LINK_LIBRARIES` too. A static archive's static dependencies have
   to follow it, and `_seen` already stops cycles. For the repro that gives
   `libtop.a libmid.a libcore.a`, and for nng it gives `libnng.a libmbedtls.a
   libmbedx509.a libmbedcrypto.a ...`, the order the workaround spells by hand.
2. Wrap the dependency link flags in `-Wl,--start-group ... -Wl,--end-group`
   when the linker is GNU ld. That is blunter, and it hides the order rather
   than producing the right one.

A pin can be the repro verbatim, asserting the program prints `42`.

## Resolved 2026-10-21: fix direction 1

`_tur_walk_iface` (emitted by `emit_link_iface_helper`, `src/compiler/pkg.c`)
now walks a linkable target's own `INTERFACE_LINK_LIBRARIES` after appending
its `$<TARGET_FILE>`, instead of only recursing into non-linkable ones, so a
static archive's static dependencies follow it on the line; `_seen` still
stops cycles. The repro's `top` entry resolves to `libtop.a libmid.a
libcore.a` and the program prints 42 with no `:targets` workaround. Pinned
as `tests/fixtures/cmake-deps-archive-chain`, run by
`tests/run-transitive-cmake-deps.sh` (`cmake-deps-archive-chain-links`).
