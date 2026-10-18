# A `:path` cmake-dep declared by a dependency spice resolves through that spice's `cmake/` directory, which a fresh checkout does not have

**Severity:** medium (configure failure for a consumer; masked wherever the
dependency spice has been fetched on its own first). A `:path` in `:cmake-deps`
is written relative to the declaring spice's generated `cmake/` directory.
When another project reaches that spice through `:spices`, `tur` joins the
path onto `<spice>/cmake/` and hands CMake the result without normalizing it:
`<spice>/cmake/../cmake-deps/foo`. `<spice>/cmake/` is gitignored and only
created by a `tur fetch` run inside that spice, so in a fresh checkout or a
`:url`-fetched copy the `..` walks out of a directory that does not exist, and
`add_subdirectory` fails.

Found 2026-10-17 building the configure-order repro for
[cmake-deps-no-cross-spice-override-or-order](cmake-deps-no-cross-spice-override-or-order.md).

## Repro

Start from the repro in
[cmake-dep-link-walk-stops-at-archive](cmake-dep-link-walk-stops-at-archive.md),
then, back in `$R`, move the `midcore` entry into a spice of its own:

```sh
mkdir -p midspice app2/src
cat > midspice/build.tur <<'EOF'
(defpackage midspice
  :name "midspice"
  :version "0.1.0"
  :cmake-deps #map{
    "midcore" #map{:path "../../deps/midcore" :targets ["mid" "core"]}})
EOF
cat > app2/build.tur <<'EOF'
(defpackage app2
  :name "app2"
  :version "0.1.0"
  :spices #map{"midspice" #map{:path "../midspice"}}
  :cmake-deps #map{
    "top" #map{:path "../../deps/top" :targets ["top"]}})
EOF
cp app/src/main.tur app2/src/
cd app2 && tur fetch
```

```
CMake Error at CMakeLists.txt:84 (add_subdirectory):
  add_subdirectory given source
  "<R>/app2/./../midspice/cmake/../../deps/midcore"
  which is not an existing directory.
```

`mkdir ../midspice/cmake` and the same `tur fetch` succeeds; the program
prints `42`. Measured on `main` 3d9e02a2 (tur v0.65.5), Linux.

In turmeric-spices the shape is live: `opengl` declares
`:path "../cmake-deps/opengl"`. A project that depends on `opengl` resolves
it through `spices/opengl/cmake/`, which exists only if someone ran
`tur fetch` inside `spices/opengl` first.

## Root cause

`append_cmake_dep_with_conflict_check` (`src/compiler/pkg.c:3634`) absolutizes
a relative `:path` against `<origin_dir>/cmake`. That is the base the archived
[cmake-dep-path-base-dir-inconsistent](../archive/cmake-dep-path-base-dir-inconsistent.md)
settled on, and the base is right. But the join is textual, and
`pkg_gen_cmake_deps` writes it straight into `add_subdirectory`, where the
operating system resolves `cmake/..` component by component and fails at
`cmake/`. That report's closing note already said no fixture covers a
dependency spice declaring a `:path` cmake-dep, which is why this was not
caught.

## Fix directions

- Normalize the joined path lexically (collapse `.` and `dir/..`) before
  storing it, so the result never depends on `<spice>/cmake/` existing.
  `<spice>/cmake/../cmake-deps/foo` becomes `<spice>/cmake-deps/foo`.
- Add the missing fixture: a dependency spice whose `cmake/` directory does
  not exist, declaring a `:path` cmake-dep. The repro above is that fixture.

## Resolution (2026-10-18)

The first fix direction. When `realpath()` cannot settle the joined
`<origin_dir>/cmake/<path>` in `append_cmake_dep_with_conflict_check`
(`src/compiler/pkg.c`), the join is now collapsed lexically
(`path_collapse_dots`): `.` components are dropped and each `dir/..` pair is
removed. A leading `..` of a relative path is kept, and one above an absolute
root is dropped. The result no longer depends on the gitignored `cmake/`
existing, so `<spice>/cmake/../../deps/midcore` becomes `<R>/deps/midcore`.
`realpath()` is still tried first, so a path through an existing symlink
resolves as before. On Windows, `realpath` is `_fullpath`, which was already
lexical.

Pinned by `tests/spice-resolver-tests.sh`
`cmake-dep-path-through-missing-cmake-dir`, the missing fixture this report
asked for. A dependency spice whose `cmake/` does not exist declares a `:path`
cmake-dep. The consumer fetches and runs (`42`), and its generated
CMakeLists has no `midspice/cmake/..` in it.
